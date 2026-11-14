/*
 * server.c - NP SysInfo server: one fork()ed child process per client.
 *
 * The parent only accepts connections and reaps finished children. Each
 * client is served by its own child process, which exits when it is done.
 * No threads are used.
 *
 * Robustness mechanisms, all on by default. Each one can be switched off to
 * show the difference (With / Without):
 *
 *   [1]  the SIGCHLD handler reaps children with a waitpid() loop
 *          --reap-once        one waitpid() per signal: of several children
 *                             that end together, the rest stay zombies
 *   [2a] SIGPIPE ignored, EPIPE from write() handled
 *          --no-sigpipe       writing to a client that has left kills the child
 *   [2b] TCP_USER_TIMEOUT: data unacknowledged for 3 s ends the connection
 *          --no-user-timeout  a client whose network vanished is written to
 *                             as if it were still there
 *   [3]  request validation: length limit, bounded numbers, value ranges
 *          --no-validate      unlimited line, plain %d, no range checks
 *   [4]  request deadline: the whole request line within 3 s
 *          --no-deadline      a client sending one byte at a time holds a
 *                             child for as long as it likes
 *   [5a] a child dies with its parent (PR_SET_PDEATHSIG)
 *          --no-pdeathsig     children of a killed parent run on as orphans
 *   [5b] Ctrl+C: stop accepting, let the running children finish
 *          --no-graceful      Ctrl+C cuts every client off at once
 *
 * -m caps the clients served at once (default 32); --no-limit removes the cap.
 */
#define _DEFAULT_SOURCE     /* strsignal, TCP_USER_TIMEOUT */

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "utils.h"

#define LISTEN_BACKLOG      64
#define REAP_RING           256     /* reaped children waiting to be logged */
#define DEFAULT_MAX_CLIENTS 32
#define USER_TIMEOUT_MS     3000    /* [2b] */
#define REQUEST_DEADLINE_MS 3000    /* [4] */
#define SHUTDOWN_GRACE_SEC  30      /* [5b] */

/* Child exit codes; the parent logs them when it reaps the child. */
enum child_exit {
    CHILD_OK          = 0,      /* request served */
    CHILD_IO_ERROR    = 1,
    CHILD_BAD_REQUEST = 2,      /* [3] too long, unknown or bad arguments */
    CHILD_PEER_GONE   = 3,      /* [2] the client left or became unreachable */
    CHILD_TIMEOUT     = 4       /* [4] no complete request line in time */
};

static struct {
    int port;
    int max_clients;            /* 0 = no limit */
    int reap_loop;              /* [1] */
    int ignore_sigpipe;         /* [2a] */
    int user_timeout;           /* [2b] */
    int validate;               /* [3] */
    int deadline;               /* [4] */
    int pdeathsig;              /* [5a] */
    int graceful;               /* [5b] */
} g_cfg = { DEFAULT_PORT, DEFAULT_MAX_CLIENTS, 1, 1, 1, 1, 1, 1, 1 };

/* Filled by the SIGCHLD handler, logged later by the main loop. */
static volatile sig_atomic_t g_reap_pid[REAP_RING];
static volatile sig_atomic_t g_reap_status[REAP_RING];
static volatile sig_atomic_t g_reap_head;   /* children reaped so far */
static int g_reap_tail;                     /* records already logged */
static int g_served;                        /* connections given a child */
static int g_active;                        /* children not reaped yet */

/* [5b] Set by the SIGINT and SIGALRM handlers. */
static volatile sig_atomic_t g_listen_fd = -1;
static volatile sig_atomic_t g_interrupts;  /* Ctrl+C presses so far */
static volatile sig_atomic_t g_grace_over;

static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ===== Parent: reaping children ========================================= */

/*
 * [1] Reap finished children. Signals do not queue: while a SIGCHLD is
 * pending, more children ending only set the same pending bit again, so one
 * SIGCHLD can stand for several children. The handler therefore calls
 * waitpid() in a loop until no finished child is left. --reap-once shows the
 * common mistake of calling it once per signal: when children end together,
 * the ones it misses stay zombies. WNOHANG keeps the handler from blocking.
 * printf() is not safe in a signal handler, so the handler only records the
 * result and the main loop prints it.
 */
static void on_sigchld(int sig)
{
    int saved_errno = errno;
    int status;
    pid_t pid;

    (void)sig;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int h = g_reap_head;

        g_reap_pid[h % REAP_RING] = pid;
        g_reap_status[h % REAP_RING] = status;
        g_reap_head = h + 1;
        if (!g_cfg.reap_loop)
            break;                  /* --reap-once: the mistake being shown */
    }
    errno = saved_errno;
}

static const char *exit_reason(int code)
{
    switch (code) {
    case CHILD_OK:          return "request served";
    case CHILD_IO_ERROR:    return "I/O error";
    case CHILD_BAD_REQUEST: return "bad request";
    case CHILD_PEER_GONE:   return "client left";
    case CHILD_TIMEOUT:     return "request timeout";
    default:                return "unknown";
    }
}

static void log_reaped(pid_t pid, int status)
{
    g_active--;
    if (WIFEXITED(status))
        LOG_INFO("reaped child %d: exit %d (%s) | active=%d",
                 (int)pid, WEXITSTATUS(status), exit_reason(WEXITSTATUS(status)), g_active);
    else if (WIFSIGNALED(status))
        LOG_WARN("reaped child %d: killed by signal %d (%s) | active=%d",
                 (int)pid, WTERMSIG(status), strsignal(WTERMSIG(status)), g_active);
}

static void report_reaped_children(void)
{
    while (g_reap_tail < g_reap_head) {
        int i = g_reap_tail % REAP_RING;

        g_reap_tail++;
        log_reaped(g_reap_pid[i], g_reap_status[i]);
    }
}

/* ===== Child: serving one client ======================================== */

static void send_error(int cfd, const char *reason)
{
    char line[MAX_LINE + 64];
    int n = snprintf(line, sizeof(line), "ERROR %s\n", reason);

    LOG_HEXDUMP("reply", line, (size_t)n);
    write_all(cfd, line, (size_t)n);
}

/*
 * [3] Without the length limit (--no-validate): keep reading and growing the
 * buffer until a '\n' arrives. A client that never sends one makes the
 * buffer, and the child's memory, grow without bound. The request deadline
 * [4] still applies (timeout_ms >= 0); on READ_TIMEOUT, *out holds the bytes
 * received so far.
 */
static ssize_t read_line_unlimited(int fd, char **out, int timeout_ms)
{
    long long deadline = now_ms() + timeout_ms;
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);

    if (buf == NULL)
        return READ_ERR;
    for (;;) {
        ssize_t r;
        char *nl;

        if (cap - len < 4096) {
            char *bigger = realloc(buf, cap * 2);

            if (bigger == NULL) {
                LOG_ERROR("out of memory after %zu bytes without a newline", len);
                free(buf);
                return READ_ERR;
            }
            buf = bigger;
            cap *= 2;
            if (cap >= 1024 * 1024)
                LOG_WARN("no newline after %zu bytes: request buffer grown to %zu KiB",
                         len, cap / 1024);
        }
        if (timeout_ms >= 0) {
            struct pollfd pfd = { .fd = fd, .events = POLLIN };
            long long left = deadline - now_ms();
            int ready = left > 0 ? poll(&pfd, 1, (int)left) : 0;

            if (ready == 0) {
                buf[len] = '\0';
                *out = buf;
                return READ_TIMEOUT;
            }
            if (ready < 0 && errno != EINTR) {
                free(buf);
                return READ_ERR;
            }
            if (ready < 0)
                continue;
        }
        r = read(fd, buf + len, cap - len - 1);
        if (r == 0) {
            free(buf);
            return READ_EOF;
        }
        if (r < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            return READ_ERR;
        }
        nl = memchr(buf + len, '\n', (size_t)r);
        len += (size_t)r;
        if (nl != NULL) {
            len = (size_t)(nl - buf);
            if (len > 0 && buf[len - 1] == '\r')
                len--;
            buf[len] = '\0';
            *out = buf;
            return (ssize_t)len;
        }
    }
}

/* Read the request line. On failure, tell the client why and return the exit code. */
static int read_request(int cfd, const char *peer, char *line, size_t cap)
{
    /* [4] The whole line must arrive within REQUEST_DEADLINE_MS. */
    int timeout_ms = g_cfg.deadline ? REQUEST_DEADLINE_MS : -1;
    ssize_t n;

    if (g_cfg.validate) {
        n = read_line_timeout(cfd, line, cap, timeout_ms);
    } else {
        char *big = NULL;

        n = read_line_unlimited(cfd, &big, timeout_ms);
        if (big != NULL) {
            snprintf(line, cap, "%s", big);
            free(big);
        }
    }
    if (n >= 0)
        return -1;                          /* got a line */

    if (n == READ_EOF) {
        LOG_INFO("%s: left before sending a complete request", peer);
        return CHILD_PEER_GONE;
    }
    if (n == READ_TOO_LONG) {
        LOG_WARN("%s: request longer than %d bytes -> rejected", peer, MAX_LINE - 1);
        send_error(cfd, "request too long");
        return CHILD_BAD_REQUEST;
    }
    if (n == READ_TIMEOUT) {
        LOG_WARN("%s: no complete request line within %d ms (got \"%.20s\") -> rejected",
                 peer, REQUEST_DEADLINE_MS, line);
        send_error(cfd, "request timeout");
        return CHILD_TIMEOUT;
    }
    LOG_WARN("%s: read failed: %s", peer, strerror(errno));
    return CHILD_IO_ERROR;
}

/* Send count snapshots, interval_ms apart. */
static int send_snapshots(int cfd, const char *peer, int count, int interval_ms)
{
    for (int seq = 1; seq <= count; seq++) {
        struct host_info info;
        char line[512];
        int n;

        if (sysinfo_collect(&info) < 0) {
            send_error(cfd, "cannot read system information");
            return CHILD_IO_ERROR;
        }
        n = sysinfo_format(&info, (unsigned)seq, line, sizeof(line));
        LOG_HEXDUMP("reply", line, (size_t)n);
        /* [2a] Without SIGPIPE ignored, this write() kills the child when
         * the client has already gone. With it, write() returns EPIPE.
         * [2b] With TCP_USER_TIMEOUT, it returns ETIMEDOUT once data sent
         * earlier has gone unacknowledged too long. Without it, write()
         * keeps succeeding for a client that can no longer be reached. */
        if (write_all(cfd, line, (size_t)n) < 0) {
            if (errno == EPIPE || errno == ECONNRESET) {
                LOG_INFO("%s: client left before snapshot %d/%d (%s) -> stop",
                         peer, seq, count, strerror(errno));
                return CHILD_PEER_GONE;
            }
            if (errno == ETIMEDOUT) {
                LOG_WARN("%s: client unreachable, no ACK for %d ms (%s) -> stop before "
                         "snapshot %d/%d", peer, USER_TIMEOUT_MS, strerror(errno), seq, count);
                return CHILD_PEER_GONE;
            }
            LOG_WARN("%s: write failed: %s", peer, strerror(errno));
            return CHILD_IO_ERROR;
        }
        LOG_DEBUG(DBG_FLOW, "%s: sent snapshot %d/%d (%d bytes)", peer, seq, count, n);
        if (seq < count)
            sleep_ms((unsigned)interval_ms);
    }
    return CHILD_OK;
}

static int reject_unknown(int cfd, const char *peer, const char *line)
{
    LOG_WARN("%s: unknown request \"%.40s\" -> rejected", peer, line);
    send_error(cfd, "unknown request: use SYSINFO or WATCH <count> <ms>");
    return CHILD_BAD_REQUEST;
}

static int serve_client(int cfd, const char *peer)
{
    char line[MAX_LINE];
    char cmd[16] = "", extra[2];
    int count = 0, interval_ms = 0, rc;

    LOG_DEBUG(DBG_FLOW, "%s: child started, waiting for a request", peer);
    rc = read_request(cfd, peer, line, sizeof(line));
    if (rc >= 0)
        return rc;
    LOG_HEXDUMP("request", line, strlen(line));
    LOG_DEBUG(DBG_FLOW, "%s: request \"%s\"", peer, line);
    if (strcmp(line, "SYSINFO") == 0) {
        count = 1;
        interval_ms = 0;
    } else if (g_cfg.validate) {
        /* [3] Widths 3 and 5 keep %d from overflowing an int, %1s catches
         * anything left over, and the ranges stop one request from flooding
         * the client or holding a child for hours. */
        int fields = sscanf(line, "%15s %3d %5d %1s", cmd, &count, &interval_ms, extra);

        if (fields < 1 || strcmp(cmd, "WATCH") != 0)
            return reject_unknown(cfd, peer, line);
        if (fields != 3 || count < 1 || count > WATCH_MAX_COUNT ||
            interval_ms < WATCH_MIN_MS || interval_ms > WATCH_MAX_MS) {
            LOG_WARN("%s: bad WATCH arguments \"%s\" -> rejected", peer, line);
            send_error(cfd, "bad arguments: WATCH <count 1-100> <ms 100-10000>");
            return CHILD_BAD_REQUEST;
        }
    } else {
        /* [3] --no-validate: plain %d, no width and no range checks. glibc
         * keeps only the low 32 bits of a number too big for an int, so
         * WATCH 4294967297 100 is served as WATCH 1 100. */
        if (sscanf(line, "%15s %d %d", cmd, &count, &interval_ms) != 3 ||
            strcmp(cmd, "WATCH") != 0)
            return reject_unknown(cfd, peer, line);
    }

    LOG_INFO("%s: %s -> %d snapshot(s)", peer, line, count);
    return send_snapshots(cfd, peer, count, interval_ms);
}

static void run_child(int lfd, int cfd, const char *peer, pid_t parent)
{
    int rc;

    /* [5a] If the parent dies, the kernel sends this child SIGTERM. Without
     * it the child would go on as an orphan, adopted by init. */
    if (g_cfg.pdeathsig) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent)    /* the parent died before prctl() */
            _exit(CHILD_IO_ERROR);
    }
    /* [5b] Ctrl+C goes to every process in the terminal's foreground group,
     * this child too. It ignores it: the parent decides when serving ends.
     * (fork() copied the parent's handler, which is meant for the parent.) */
    if (g_cfg.graceful)
        set_signal(SIGINT, SIG_IGN);
    close(lfd);                     /* only the parent accepts */
    set_signal(SIGCHLD, SIG_DFL);   /* the child has no children to reap */
    /* [2b] If data sent to the client stays unacknowledged for
     * USER_TIMEOUT_MS, the kernel drops the connection and the next write()
     * fails with ETIMEDOUT. Without it, write() only copies the data into
     * the send buffer and succeeds even when the client cannot be reached. */
    if (g_cfg.user_timeout) {
        unsigned int ms = USER_TIMEOUT_MS;

        if (setsockopt(cfd, IPPROTO_TCP, TCP_USER_TIMEOUT, &ms, sizeof(ms)) < 0)
            LOG_WARN("%s: TCP_USER_TIMEOUT not set: %s", peer, strerror(errno));
    }
    rc = serve_client(cfd, peer);
    close(cfd);
    LOG_DEBUG(DBG_FLOW, "%s: child exiting with code %d (%s)", peer, rc, exit_reason(rc));
    _exit(rc);
}

/* ===== Parent: setup, main loop and shutdown ============================ */

/*
 * Too many clients (-m): the parent answers itself, without fork(). The send
 * never blocks (MSG_DONTWAIT) and never raises SIGPIPE (MSG_NOSIGNAL), so
 * a client cannot stall or kill the parent here.
 */
static void reject_busy(int cfd, const char *peer)
{
    static const char busy[] = "ERROR server busy, try again later\n";

    LOG_WARN("%s rejected: %d clients connected, limit %d", peer, g_active, g_cfg.max_clients);
    if (send(cfd, busy, sizeof(busy) - 1, MSG_DONTWAIT | MSG_NOSIGNAL) < 0)
        LOG_DEBUG(DBG_FLOW, "%s: busy reply not sent: %s", peer, strerror(errno));
    close(cfd);
}

/*
 * [5b] Ctrl+C. The first one closes the listening socket right here, so new
 * clients are refused at once and the next accept() fails even if the
 * signal lands just before the call: it cannot be missed. close() is
 * async-signal-safe. The main loop then waits for the running children.
 * A second Ctrl+C makes the parent quit without waiting.
 */
static void on_sigint(int sig)
{
    (void)sig;
    if (g_interrupts++ == 0 && g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
}

static void on_sigalrm(int sig)
{
    (void)sig;
    g_grace_over = 1;
}

/*
 * [5b] After Ctrl+C: wait for every running child to finish, for at most
 * SHUTDOWN_GRACE_SEC seconds. The parent stays alive meanwhile, so
 * PR_SET_PDEATHSIG [5a] leaves the children alone. If the time runs out or
 * Ctrl+C comes again, the parent exits and [5a] ends the children.
 */
static int shut_down(void)
{
    int status, all_done;
    pid_t pid;

    set_signal(SIGCHLD, SIG_DFL);   /* the waitpid() below reaps from now on */
    report_reaped_children();
    LOG_INFO("Ctrl+C: no new clients; waiting for %d child(ren) to finish "
             "(at most %d s, Ctrl+C again to quit now)", g_active, SHUTDOWN_GRACE_SEC);
    set_signal(SIGALRM, on_sigalrm);
    alarm(SHUTDOWN_GRACE_SEC);
    for (;;) {
        pid = waitpid(-1, &status, 0);
        if (pid > 0)
            log_reaped(pid, status);
        else if (errno != EINTR || g_interrupts > 1 || g_grace_over)
            break;
    }
    all_done = errno == ECHILD;
    alarm(0);
    if (all_done) {
        LOG_INFO("all children finished -> server exits");
        return EXIT_SUCCESS;
    }
    LOG_WARN("%s: %d child(ren) still running -> server exits, %s",
             g_grace_over ? "grace period over" : "Ctrl+C again", g_active,
             g_cfg.pdeathsig ? "PR_SET_PDEATHSIG [5a] ends them" : "they run on as orphans");
    return EXIT_FAILURE;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [-p PORT] [-d LEVEL] [-m MAX] [--no-limit] [--reap-once] [--no-sigpipe]\n"
            "          [--no-user-timeout] [--no-validate] [--no-deadline] [--no-pdeathsig]\n"
            "          [--no-graceful]\n"
            "  -p, --port PORT         port to listen on (default %d)\n"
            "  -d, --debug LEVEL       0 normal, 1 program flow, 2 flow + bytes (debug build only)\n"
            "  -m, --max-clients MAX   clients served at once (default %d)\n"
            "  --no-limit              no limit on clients served at once\n"
            "robustness mechanisms are all on; each option below switches one off:\n"
            "  --reap-once             [1]  one waitpid() per SIGCHLD (zombies)\n"
            "  --no-sigpipe            [2a] keep the default SIGPIPE action\n"
            "  --no-user-timeout       [2b] no TCP_USER_TIMEOUT on client connections\n"
            "  --no-validate           [3]  no length limit, plain %%d, no range checks\n"
            "  --no-deadline           [4]  wait for the request line for ever\n"
            "  --no-pdeathsig          [5a] children outlive a killed parent\n"
            "  --no-graceful           [5b] Ctrl+C ends every child at once\n",
            prog, DEFAULT_PORT, DEFAULT_MAX_CLIENTS);
}

static int parse_args(int argc, char *argv[])
{
    static const struct option opts[] = {
        { "port",            required_argument, NULL, 'p' },
        { "debug",           required_argument, NULL, 'd' },
        { "max-clients",     required_argument, NULL, 'm' },
        { "no-limit",        no_argument,       NULL, 1 },
        { "reap-once",       no_argument,       NULL, 2 },
        { "no-sigpipe",      no_argument,       NULL, 3 },
        { "no-user-timeout", no_argument,       NULL, 4 },
        { "no-validate",     no_argument,       NULL, 5 },
        { "no-deadline",     no_argument,       NULL, 6 },
        { "no-pdeathsig",    no_argument,       NULL, 7 },
        { "no-graceful",     no_argument,       NULL, 8 },
        { "help",            no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    int c;

    while ((c = getopt_long(argc, argv, "p:d:m:h", opts, NULL)) != -1) {
        switch (c) {
        case 'p':
            if (parse_int_range(optarg, 1, 65535, &g_cfg.port) < 0)
                goto bad;
            break;
        case 'd':
            if (parse_int_range(optarg, 0, 2, &g_runtime_debug_level) < 0)
                goto bad;
            break;
        case 'm':
            if (parse_int_range(optarg, 1, 1024, &g_cfg.max_clients) < 0)
                goto bad;
            break;
        case 1: g_cfg.max_clients = 0;    break;
        case 2: g_cfg.reap_loop = 0;      break;
        case 3: g_cfg.ignore_sigpipe = 0; break;
        case 4: g_cfg.user_timeout = 0;   break;
        case 5: g_cfg.validate = 0;       break;
        case 6: g_cfg.deadline = 0;       break;
        case 7: g_cfg.pdeathsig = 0;      break;
        case 8: g_cfg.graceful = 0;       break;
        case 'h':
            usage(argv[0]);
            exit(EXIT_SUCCESS);
        default:
            goto bad;
        }
    }
    if (optind == argc)
        return 0;
bad:
    usage(argv[0]);
    return -1;
}

static int open_listener(int port)
{
    struct sockaddr_in addr;
    int one = 1;
    int lfd = socket(AF_INET, SOCK_STREAM, 0);

    if (lfd < 0) {
        LOG_ERROR("socket: %s", strerror(errno));
        return -1;
    }
    /* Restart at once even while old connections are in TIME_WAIT. */
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("bind port %d: %s", port, strerror(errno));
        close(lfd);
        return -1;
    }
    if (listen(lfd, LISTEN_BACKLOG) < 0) {
        LOG_ERROR("listen: %s", strerror(errno));
        close(lfd);
        return -1;
    }
    return lfd;
}

static const char *on_off(int on)
{
    return on ? "ON" : "OFF";
}

int main(int argc, char *argv[])
{
    char limit[16];
    pid_t self;
    int lfd;

    log_init("server");
    if (parse_args(argc, argv) < 0)
        return EXIT_FAILURE;
    if (!LOG_DEBUG_ENABLED && g_runtime_debug_level > 0)
        LOG_WARN("built without -DENABLE_DEBUG: -d %d has no effect", g_runtime_debug_level);

    set_signal(SIGCHLD, on_sigchld);        /* [1] */
    if (g_cfg.ignore_sigpipe)               /* [2a] children inherit it */
        set_signal(SIGPIPE, SIG_IGN);

    lfd = open_listener(g_cfg.port);
    if (lfd < 0)
        return EXIT_FAILURE;
    g_listen_fd = lfd;
    if (g_cfg.graceful)                     /* [5b] */
        set_signal(SIGINT, on_sigint);
    self = getpid();
    if (g_cfg.max_clients > 0)
        snprintf(limit, sizeof(limit), "%d", g_cfg.max_clients);
    else
        snprintf(limit, sizeof(limit), "OFF");
    LOG_INFO("listening on port %d (pid %d), max clients %s", g_cfg.port, (int)self, limit);
    LOG_INFO("robustness: [1] waitpid loop %s | [2a] ignore SIGPIPE %s | "
             "[2b] TCP_USER_TIMEOUT %s | [3] validation %s",
             on_off(g_cfg.reap_loop), on_off(g_cfg.ignore_sigpipe),
             on_off(g_cfg.user_timeout), on_off(g_cfg.validate));
    LOG_INFO("robustness: [4] request deadline %s | [5a] pdeathsig %s | [5b] graceful Ctrl+C %s",
             on_off(g_cfg.deadline), on_off(g_cfg.pdeathsig), on_off(g_cfg.graceful));
    LOG_INFO("debug logs: compile-time %s, runtime level %d",
             LOG_DEBUG_ENABLED ? "ENABLED" : "compiled out", g_runtime_debug_level);

    for (;;) {
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        char peer[INET_ADDRSTRLEN + 8];
        int cfd;
        pid_t pid;

        report_reaped_children();
        if (g_interrupts)
            break;                  /* [5b] Ctrl+C: stop accepting */
        cfd = accept(lfd, (struct sockaddr *)&addr, &addr_len);
        if (cfd < 0) {
            /* EINTR: a signal arrived (SIGCHLD to log, or Ctrl+C).
             * EBADF: Ctrl+C has closed the listening socket. */
            if (errno != EINTR && !g_interrupts)
                LOG_ERROR("accept: %s", strerror(errno));
            continue;
        }
        inet_ntop(AF_INET, &addr.sin_addr, peer, sizeof(peer));
        snprintf(peer + strlen(peer), sizeof(peer) - strlen(peer), ":%d",
                 ntohs(addr.sin_port));

        report_reaped_children();   /* children that ended during accept() */
        if (g_cfg.max_clients > 0 && g_active >= g_cfg.max_clients) {
            reject_busy(cfd, peer);
            continue;
        }

        pid = fork();
        if (pid < 0) {
            LOG_ERROR("fork for %s: %s", peer, strerror(errno));
            close(cfd);
            continue;
        }
        if (pid == 0)
            run_child(lfd, cfd, peer, self);

        g_served++;
        g_active++;
        LOG_INFO("accepted %s -> child pid %d (connection #%d) | active=%d",
                 peer, (int)pid, g_served, g_active);
        close(cfd);                 /* the child owns the connection now */
    }
    return shut_down();
}
