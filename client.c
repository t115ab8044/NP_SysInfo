/*
 * client.c - NP SysInfo client.
 *
 *   ./client                  ask for one snapshot (SYSINFO) and print it
 *   ./client -n 5 -w 1000     WATCH 5 1000: five snapshots, one per second
 *   ./client -r "HELLO"       send any request line, e.g. to see an ERROR reply
 *   ./client -n 3 -w 200 --half-close
 *                             half close: shutdown(SHUT_WR) right after the
 *                             request, then still read every reply
 *   ./client --mode MODE      misbehave on purpose, for the robustness demo:
 *       abort     ask for WATCH 10 200, read one snapshot, then leave  [2a]
 *       longline  send a 16 MiB request with no '\n' (size: --mib N)   [3]
 *       slow      Slowloris: one byte of the request every 500 ms
 *                 (-w MS), never the '\n'                              [4]
 *       idle      connect, send nothing, wait for the server to act
 *
 * The client has no timeout: it waits until the server replies or closes
 * the connection. Ctrl+C ends it; the tests end idle clients themselves.
 *
 * Exit status: 0 = got the expected reply, 1 = error or ERROR reply,
 *              2 = the connection ended before any reply line.
 */
#define _DEFAULT_SOURCE

#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "utils.h"

#define REPLY_MAX       512
#define DEFAULT_COUNT   5
#define DEFAULT_MS      1000
#define SLOW_GAP_MS     500             /* --mode slow: ms between two bytes */

enum client_mode { MODE_NORMAL, MODE_ABORT, MODE_LONGLINE, MODE_SLOW, MODE_IDLE };

enum { CLIENT_OK = 0, CLIENT_FAIL = 1, CLIENT_NO_REPLY = 2 };

static struct {
    const char *host;
    int port;
    int count;                  /* -n, -1 = not given */
    int interval_ms;            /* -w, -1 = not given */
    const char *request;        /* -r, NULL = build SYSINFO or WATCH */
    int mib;                    /* size of the longline request */
    enum client_mode mode;
    int half_close;             /* --half-close */
} g_cfg = { "127.0.0.1", DEFAULT_PORT, -1, -1, NULL, 16, MODE_NORMAL, 0 };

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static int connect_to_server(const char *host, int port)
{
    struct addrinfo hints, *res, *ai;
    char port_str[8];
    int fd = -1, rc, err = 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port_str, sizeof(port_str), "%d", port);
    rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0) {
        LOG_ERROR("cannot resolve %s: %s", host, gai_strerror(rc));
        return -1;
    }
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            err = errno;
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        err = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        LOG_ERROR("cannot connect to %s:%d: %s", host, port, strerror(err));
    return fd;
}

static int send_request(int fd, const char *line)
{
    size_t n = strlen(line);

    LOG_HEXDUMP("request", line, n);
    if (write_all(fd, line, n) < 0) {
        LOG_ERROR("sending the request failed: %s", strerror(errno));
        return -1;
    }
    LOG_DEBUG(DBG_FLOW, "request sent (%zu bytes)", n);
    return 0;
}

/* Why read_line() failed, for the log. */
static const char *read_error(ssize_t n)
{
    if (n == READ_EOF)
        return "the server closed the connection";
    if (n == READ_TOO_LONG)
        return "reply line too long";
    return strerror(errno);
}

/* SYSINFO, WATCH or the -r line: print every reply line. */
static int run_normal(int fd)
{
    char request[MAX_LINE + 2], line[REPLY_MAX];
    int expected = 1, until_eof = 0;

    if (g_cfg.request != NULL) {
        snprintf(request, sizeof(request), "%s\n", g_cfg.request);
        until_eof = 1;                  /* any request: read until the server closes */
    } else if (g_cfg.count >= 0 || g_cfg.interval_ms >= 0) {
        expected = g_cfg.count >= 0 ? g_cfg.count : DEFAULT_COUNT;
        snprintf(request, sizeof(request), "WATCH %d %d\n", expected,
                 g_cfg.interval_ms >= 0 ? g_cfg.interval_ms : DEFAULT_MS);
    } else {
        snprintf(request, sizeof(request), "SYSINFO\n");
    }
    LOG_INFO("request: %.*s", (int)strcspn(request, "\n"), request);
    if (send_request(fd, request) < 0)
        return CLIENT_FAIL;
    if (g_cfg.half_close) {
        /* Half close: send our FIN now. Only the sending direction is shut,
         * so the server still reads the request and its replies still arrive. */
        if (shutdown(fd, SHUT_WR) < 0) {
            LOG_ERROR("shutdown(SHUT_WR) failed: %s", strerror(errno));
            return CLIENT_FAIL;
        }
        LOG_INFO("half close: shutdown(SHUT_WR) sent FIN, still reading the replies");
    }

    if (expected < 1)
        until_eof = 1;                  /* the server will answer with an ERROR */

    for (int i = 0; until_eof || i < expected; i++) {
        ssize_t n = read_line(fd, line, sizeof(line));

        if (n < 0) {
            if (until_eof && n == READ_EOF && i > 0)
                return CLIENT_OK;
            LOG_WARN("after %d reply line(s): %s", i, read_error(n));
            return i == 0 ? CLIENT_NO_REPLY : CLIENT_FAIL;
        }
        LOG_HEXDUMP("reply", line, (size_t)n);
        printf("%s\n", line);
        fflush(stdout);
        if (strncmp(line, "ERROR", 5) == 0)
            return CLIENT_FAIL;
    }
    return CLIENT_OK;
}

/* [2a] Ask for 10 snapshots, leave after the first one. */
static int run_abort(int fd)
{
    char line[REPLY_MAX];
    ssize_t n;

    LOG_INFO("mode abort: request WATCH 10 200, leave after the first snapshot");
    if (send_request(fd, "WATCH 10 200\n") < 0)
        return CLIENT_FAIL;
    n = read_line(fd, line, sizeof(line));
    if (n < 0) {
        LOG_WARN("%s", read_error(n));
        return CLIENT_NO_REPLY;
    }
    printf("%s\n", line);
    printf("RESULT: got snapshot 1 of 10, closing the connection now\n");
    return CLIENT_OK;
}

/* [3] Send mib MiB of 'A' and never a '\n'. */
static int run_longline(int fd)
{
    static char chunk[64 * 1024];
    size_t total = (size_t)g_cfg.mib * 1024 * 1024, sent = 0;
    char line[REPLY_MAX];
    double t0;
    ssize_t n;

    memset(chunk, 'A', sizeof(chunk));
    set_signal(SIGPIPE, SIG_IGN);       /* survive the server closing on us */
    LOG_INFO("mode longline: sending %d MiB with no newline", g_cfg.mib);
    while (sent < total) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        size_t len = total - sent < sizeof(chunk) ? total - sent : sizeof(chunk);

        if (poll(&pfd, 1, 0) > 0)
            break;                      /* the server answered early */
        if (write_all(fd, chunk, len) < 0)
            break;
        sent += len;
    }
    if (sent < total)
        printf("RESULT: stopped after sending %zu KiB of %d MiB: the server cut us off\n",
               sent / 1024, g_cfg.mib);
    else
        printf("RESULT: sent all %d MiB without a newline, waiting for the server\n",
               g_cfg.mib);
    fflush(stdout);

    t0 = now_sec();
    n = read_line(fd, line, sizeof(line));
    if (n >= 0)
        printf("RESULT: server reply after %.1f s: %s\n", now_sec() - t0, line);
    else
        printf("RESULT: no reply line after %.1f s (%s)\n", now_sec() - t0,
               n == READ_ERR && errno == ECONNRESET ? "connection reset" : read_error(n));
    return CLIENT_OK;
}

/*
 * [4] Slowloris: send the request one byte at a time, gap_ms apart, and
 * never the '\n'; after the request text, keep trickling spaces. Every byte
 * arrives well within any per-read timeout, so only a deadline for the
 * whole line stops it. Ends when the server answers or closes.
 */
static int run_slow(int fd)
{
    const char *text = g_cfg.request != NULL ? g_cfg.request : "SYSINFO";
    int gap_ms = g_cfg.interval_ms >= 0 ? g_cfg.interval_ms : SLOW_GAP_MS;
    size_t len = strlen(text), sent = 0;
    char line[REPLY_MAX];
    double t0 = now_sec();
    ssize_t n;

    set_signal(SIGPIPE, SIG_IGN);       /* survive the server closing on us */
    LOG_INFO("mode slow: \"%s\" one byte every %d ms, never a newline", text, gap_ms);
    for (;;) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        char c = sent < len ? text[sent] : ' ';

        if (write_all(fd, &c, 1) < 0)
            break;                      /* the server has closed */
        sent++;
        if (poll(&pfd, 1, gap_ms) > 0)
            break;                      /* the server answered or closed */
    }
    n = read_line(fd, line, sizeof(line));
    if (n >= 0)
        printf("RESULT: after %.1f s and %zu bytes the server replied: %s\n",
               now_sec() - t0, sent, line);
    else
        printf("RESULT: after %.1f s and %zu bytes: %s\n", now_sec() - t0, sent,
               n == READ_ERR && errno == ECONNRESET ? "connection reset" : read_error(n));
    return CLIENT_OK;
}

/* Connect, send nothing, and wait for whatever the server does. */
static int run_idle(int fd)
{
    char line[REPLY_MAX];
    double t0 = now_sec();
    ssize_t n;

    LOG_INFO("mode idle: connected, sending nothing");
    n = read_line(fd, line, sizeof(line));
    if (n >= 0) {
        printf("RESULT: after %.1f s the server replied: %s\n", now_sec() - t0, line);
        return CLIENT_OK;
    }
    if (n == READ_EOF) {
        printf("RESULT: after %.1f s the server closed the connection\n", now_sec() - t0);
        return CLIENT_OK;
    }
    printf("RESULT: read failed after %.1f s: %s\n", now_sec() - t0, strerror(errno));
    return CLIENT_FAIL;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [-H HOST] [-p PORT] [-d LEVEL] [-n COUNT] [-w MS]\n"
            "          [-r LINE] [--half-close] [--mode abort|longline|slow|idle] [--mib N]\n"
            "  -H, --host HOST      server address (default 127.0.0.1)\n"
            "  -p, --port PORT      server port (default %d)\n"
            "  -d, --debug LEVEL    0 normal, 1 program flow, 2 flow + bytes (debug build only)\n"
            "  -n, --count COUNT    WATCH: number of snapshots (default %d)\n"
            "  -w, --interval MS    WATCH: milliseconds between snapshots (default %d);\n"
            "                       slow: milliseconds between bytes (default %d)\n"
            "  -r, --request LINE   send LINE as the request\n"
            "  --half-close         shutdown(SHUT_WR) after the request, keep reading replies\n"
            "  --mode MODE          abort [2a], longline [3], slow [4] or idle\n"
            "  --mib N              longline: request size in MiB (default 16)\n",
            prog, DEFAULT_PORT, DEFAULT_COUNT, DEFAULT_MS, SLOW_GAP_MS);
}

static int parse_args(int argc, char *argv[])
{
    static const struct option opts[] = {
        { "host",     required_argument, NULL, 'H' },
        { "port",     required_argument, NULL, 'p' },
        { "debug",    required_argument, NULL, 'd' },
        { "count",    required_argument, NULL, 'n' },
        { "interval", required_argument, NULL, 'w' },
        { "request",  required_argument, NULL, 'r' },
        { "mode",     required_argument, NULL, 1 },
        { "mib",      required_argument, NULL, 2 },
        { "half-close", no_argument,     NULL, 3 },
        { "help",     no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    int c;

    while ((c = getopt_long(argc, argv, "H:p:d:n:w:r:h", opts, NULL)) != -1) {
        switch (c) {
        case 'H':
            g_cfg.host = optarg;
            break;
        case 'p':
            if (parse_int_range(optarg, 1, 65535, &g_cfg.port) < 0)
                goto bad;
            break;
        case 'd':
            if (parse_int_range(optarg, 0, 2, &g_runtime_debug_level) < 0)
                goto bad;
            break;
        /* -n and -w are not range-checked here: the server checks them. */
        case 'n':
            if (parse_int_range(optarg, 0, 1000000, &g_cfg.count) < 0)
                goto bad;
            break;
        case 'w':
            if (parse_int_range(optarg, 0, 1000000, &g_cfg.interval_ms) < 0)
                goto bad;
            break;
        case 'r':
            if (strlen(optarg) > MAX_LINE) {
                fprintf(stderr, "-r: at most %d characters (use --mode longline)\n", MAX_LINE);
                return -1;
            }
            g_cfg.request = optarg;
            break;
        case 1:
            if (strcmp(optarg, "abort") == 0)
                g_cfg.mode = MODE_ABORT;
            else if (strcmp(optarg, "longline") == 0)
                g_cfg.mode = MODE_LONGLINE;
            else if (strcmp(optarg, "slow") == 0)
                g_cfg.mode = MODE_SLOW;
            else if (strcmp(optarg, "idle") == 0)
                g_cfg.mode = MODE_IDLE;
            else
                goto bad;
            break;
        case 2:
            if (parse_int_range(optarg, 1, 1024, &g_cfg.mib) < 0)
                goto bad;
            break;
        case 3:
            g_cfg.half_close = 1;
            break;
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

int main(int argc, char *argv[])
{
    int fd, rc;

    log_init("client");
    if (parse_args(argc, argv) < 0)
        return CLIENT_FAIL;
    if (!LOG_DEBUG_ENABLED && g_runtime_debug_level > 0)
        LOG_WARN("built without -DENABLE_DEBUG: -d %d has no effect", g_runtime_debug_level);

    fd = connect_to_server(g_cfg.host, g_cfg.port);
    if (fd < 0)
        return CLIENT_FAIL;
    LOG_INFO("connected to %s:%d", g_cfg.host, g_cfg.port);

    switch (g_cfg.mode) {
    case MODE_ABORT:    rc = run_abort(fd);    break;
    case MODE_LONGLINE: rc = run_longline(fd); break;
    case MODE_SLOW:     rc = run_slow(fd);     break;
    case MODE_IDLE:     rc = run_idle(fd);     break;
    default:            rc = run_normal(fd);   break;
    }
    close(fd);
    LOG_DEBUG(DBG_FLOW, "connection closed, exit status %d", rc);
    return rc;
}
