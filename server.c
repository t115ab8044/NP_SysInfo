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
 *   [1] SIGCHLD handler reaps children with a waitpid() loop
 *         --no-sigchld    finished children stay behind as zombies
 *   [2] SIGPIPE ignored, EPIPE from write() handled
 *         --no-sigpipe    a child writing to a client that has left is killed
 *   [3] request line length limit (MAX_LINE)
 *         --no-validate   a request with no '\n' grows the child's memory
 *                         without limit
 *   [4] limit on connected clients (-m, default 32)
 *         --no-limit      every connection gets a child, however many
 *   [5] a child dies with its parent (PR_SET_PDEATHSIG)
 *         --no-pdeathsig  children of a killed parent keep running as orphans
 */
#define _DEFAULT_SOURCE     /* strsignal */

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "utils.h"

#define LISTEN_BACKLOG      64
#define REAP_RING           256     /* reaped children waiting to be logged */
#define DEFAULT_MAX_CLIENTS 32

/* Child exit codes; the parent logs them when it reaps the child. */
enum child_exit {
    CHILD_OK          = 0,      /* request served */
    CHILD_IO_ERROR    = 1,
    CHILD_BAD_REQUEST = 2,      /* [3] too long, unknown or bad arguments */
    CHILD_PEER_GONE   = 3       /* [2] write() failed: the client left */
};

static struct {
    int port;
    int max_clients;            /* [4] 0 = no limit */
    int reap;                   /* [1] */
    int ignore_sigpipe;         /* [2] */
    int validate;               /* [3] */
    int pdeathsig;              /* [5] */
} g_cfg = { DEFAULT_PORT, DEFAULT_MAX_CLIENTS, 1, 1, 1, 1 };

/* Filled by the SIGCHLD handler, logged later by the main loop. */
static volatile sig_atomic_t g_reap_pid[REAP_RING];
static volatile sig_atomic_t g_reap_status[REAP_RING];
static volatile sig_atomic_t g_reap_head;   /* children reaped so far */
static int g_reap_tail;                     /* records already logged */
static int g_served;                        /* connections given a child */
static int g_active;                        /* children not reaped yet */

/* ===== Parent: reaping children ========================================= */

/*
 * [1] Reap every finished child. Signals do not queue: one SIGCHLD can stand
 * for several children that ended at about the same time, so waitpid() is
 * called in a loop until no finished child is left. WNOHANG keeps the
 * handler from blocking. printf() is not safe in a signal handler, so the
 * handler only records the result and the main loop prints it.
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
    default:                return "unknown";
    }
}

static void report_reaped_children(void)
{
    while (g_reap_tail < g_reap_head) {
        int i = g_reap_tail % REAP_RING;
        int pid = g_reap_pid[i];
        int status = g_reap_status[i];

        g_reap_tail++;
        g_active--;
        if (WIFEXITED(status))
            LOG_INFO("reaped child %d: exit %d (%s) | active=%d",
                     pid, WEXITSTATUS(status), exit_reason(WEXITSTATUS(status)), g_active);
        else if (WIFSIGNALED(status))
            LOG_WARN("reaped child %d: killed by signal %d (%s) | active=%d",
                     pid, WTERMSIG(status), strsignal(WTERMSIG(status)), g_active);
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
 * buffer, and the child's memory, grow without bound.
 */
static ssize_t read_line_unlimited(int fd, char **out)
{
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
    ssize_t n;

    if (g_cfg.validate) {
        n = read_line(cfd, line, cap);
    } else {
        char *big = NULL;

        n = read_line_unlimited(cfd, &big);
        if (n >= 0) {
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
        /* [2] Without SIGPIPE ignored, this write() kills the child when
         * the client has already gone. With it, write() returns EPIPE. */
        if (write_all(cfd, line, (size_t)n) < 0) {
            if (errno == EPIPE || errno == ECONNRESET) {
                LOG_INFO("%s: client left before snapshot %d/%d (%s) -> stop",
                         peer, seq, count, strerror(errno));
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

static int serve_client(int cfd, const char *peer)
{
    char line[MAX_LINE];
    char cmd[16], extra[2];
    int count = 0, interval_ms = 0, fields, rc;

    LOG_DEBUG(DBG_FLOW, "%s: child started, waiting for a request", peer);
    rc = read_request(cfd, peer, line, sizeof(line));
    if (rc >= 0)
        return rc;
    LOG_HEXDUMP("request", line, strlen(line));
    LOG_DEBUG(DBG_FLOW, "%s: request \"%s\"", peer, line);
    /* Widths 3 and 5 fit the limits and keep %d from overflowing an int. */
    fields = sscanf(line, "%15s %3d %5d %1s", cmd, &count, &interval_ms, extra);
    if (strcmp(line, "SYSINFO") == 0) {
        count = 1;
        interval_ms = 0;
    } else if (fields >= 1 && strcmp(cmd, "WATCH") == 0) {
        if (fields != 3 || count < 1 || count > WATCH_MAX_COUNT ||
            interval_ms < WATCH_MIN_MS || interval_ms > WATCH_MAX_MS) {
            LOG_WARN("%s: bad WATCH arguments \"%s\" -> rejected", peer, line);
            send_error(cfd, "bad arguments: WATCH <count 1-100> <ms 100-10000>");
            return CHILD_BAD_REQUEST;
        }
    } else {
        LOG_WARN("%s: unknown request \"%.40s\" -> rejected", peer, line);
        send_error(cfd, "unknown request: use SYSINFO or WATCH <count> <ms>");
        return CHILD_BAD_REQUEST;
    }

    LOG_INFO("%s: %s -> %d snapshot(s)", peer, line, count);
    return send_snapshots(cfd, peer, count, interval_ms);
}

static void run_child(int lfd, int cfd, const char *peer, pid_t parent)
{
    int rc;

    /* [5] If the parent dies, the kernel sends this child SIGTERM. Without
     * it the child would go on as an orphan, adopted by init. */
    if (g_cfg.pdeathsig) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent)    /* the parent died before prctl() */
            _exit(CHILD_IO_ERROR);
    }
    close(lfd);                     /* only the parent accepts */
    set_signal(SIGCHLD, SIG_DFL);   /* the child has no children to reap */
    rc = serve_client(cfd, peer);
    close(cfd);
    LOG_DEBUG(DBG_FLOW, "%s: child exiting with code %d (%s)", peer, rc, exit_reason(rc));
    _exit(rc);
}

/* ===== Parent: setup and main loop ====================================== */

/*
 * [4] Too many clients: the parent answers itself, without fork(). The send
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

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [-p PORT] [-d LEVEL] [-m MAX] [--no-sigchld] [--no-sigpipe]\n"
            "          [--no-validate] [--no-limit] [--no-pdeathsig]\n"
            "  -p, --port PORT         port to listen on (default %d)\n"
            "  -d, --debug LEVEL       0 normal, 1 program flow, 2 flow + bytes (debug build only)\n"
            "  -m, --max-clients MAX   [4] clients served at once (default %d)\n"
            "  --no-sigchld            [1] do not reap children (zombies)\n"
            "  --no-sigpipe            [2] keep the default SIGPIPE action\n"
            "  --no-validate           [3] no request length limit\n"
            "  --no-limit              [4] no limit on clients served at once\n"
            "  --no-pdeathsig          [5] children outlive a killed parent\n",
            prog, DEFAULT_PORT, DEFAULT_MAX_CLIENTS);
}

static int parse_args(int argc, char *argv[])
{
    static const struct option opts[] = {
        { "port",         required_argument, NULL, 'p' },
        { "debug",        required_argument, NULL, 'd' },
        { "max-clients",  required_argument, NULL, 'm' },
        { "no-sigchld",   no_argument,       NULL, 1 },
        { "no-sigpipe",   no_argument,       NULL, 2 },
        { "no-validate",  no_argument,       NULL, 3 },
        { "no-limit",     no_argument,       NULL, 4 },
        { "no-pdeathsig", no_argument,       NULL, 5 },
        { "help",         no_argument,       NULL, 'h' },
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
        case 1: g_cfg.reap = 0;           break;
        case 2: g_cfg.ignore_sigpipe = 0; break;
        case 3: g_cfg.validate = 0;       break;
        case 4: g_cfg.max_clients = 0;    break;
        case 5: g_cfg.pdeathsig = 0;      break;
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

    /* [1] Without a handler, SIGCHLD is ignored but the children are never
     * waited for, so each one stays a zombie. */
    if (g_cfg.reap)
        set_signal(SIGCHLD, on_sigchld);
    /* [2] Children inherit this through fork(). */
    if (g_cfg.ignore_sigpipe)
        set_signal(SIGPIPE, SIG_IGN);

    lfd = open_listener(g_cfg.port);
    if (lfd < 0)
        return EXIT_FAILURE;
    self = getpid();
    if (g_cfg.max_clients > 0)
        snprintf(limit, sizeof(limit), "%d", g_cfg.max_clients);
    else
        snprintf(limit, sizeof(limit), "OFF");
    LOG_INFO("listening on port %d (pid %d)", g_cfg.port, (int)self);
    LOG_INFO("robustness: [1] reap %s | [2] ignore SIGPIPE %s | [3] length limit %s | "
             "[4] max clients %s | [5] pdeathsig %s",
             on_off(g_cfg.reap), on_off(g_cfg.ignore_sigpipe), on_off(g_cfg.validate),
             limit, on_off(g_cfg.pdeathsig));
    LOG_INFO("debug logs: compile-time %s, runtime level %d",
             LOG_DEBUG_ENABLED ? "ENABLED" : "compiled out", g_runtime_debug_level);

    for (;;) {
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        char peer[INET_ADDRSTRLEN + 8];
        int cfd;
        pid_t pid;

        report_reaped_children();
        cfd = accept(lfd, (struct sockaddr *)&addr, &addr_len);
        if (cfd < 0) {
            if (errno != EINTR)     /* EINTR: a SIGCHLD arrived, log it */
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
}
