/*
 * client.c - NP SysInfo client.
 *
 *   ./client                   one snapshot, printed as a report
 *   ./client --detail          OS, CPU model/usage and disk details
 *   ./client -n 5              five requests over one connection
 *   ./client -w 1000           subscribe: the server pushes every 1000 ms until
 *                              Ctrl+C (-n N stops after N pushes); reconnects
 *                              automatically if the server goes away ([8])
 *   ./client --hold 5          keep the connection open 5 s after the reply
 *   ./client --stress 12       fork 12 clients at once, print latency stats
 *   ./client --bind 127.0.0.2  connect from another loopback address ([4] demo)
 *   ./client --mode MODE       fault injection for the robustness demo:
 *       garbage   send an HTTP request instead of an NP header      [3]
 *       biglen    send a valid header that claims a 4 GiB body      [3]
 *       idle      connect and send nothing                          [3]
 *       abort     subscribe, then disconnect after --abort-after    [2]
 *       fragment  send one request in 8 one-byte pieces             [6]
 *       pipeline  send -n requests (default 5) glued in one write   [6]
 *       slow      send a request one byte every 3 s                 [5]
 */
#define _DEFAULT_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "utils.h"

#define FAULT_WAIT_SEC      8       /* default wait for the server's reaction */
#define ABORT_INTERVAL_MS   200
#define FRAGMENT_GAP_MS     100     /* [6] pause between the pieces of a request */
#define SLOW_GAP_MS         3000    /* [5] below the server's 5 s read timeout */
#define PIPELINE_DEFAULT    5
#define PIPELINE_MAX        64
#define PIPELINE_WAIT_SEC   3
#define RETRY_MAX_DELAY_SEC 8       /* [8] backoff 1, 2, 4, 8, 8, ... s */

enum client_mode {
    MODE_NORMAL, MODE_GARBAGE, MODE_BIGLEN, MODE_IDLE, MODE_ABORT,
    MODE_FRAGMENT, MODE_PIPELINE, MODE_SLOW
};

static const char *const mode_names[] = {
    "normal", "garbage", "biglen", "idle", "abort", "fragment", "pipeline", "slow"
};

/* Exit codes */
enum { CLIENT_OK = 0, CLIENT_FAIL = 1, CLIENT_NO_RESPONSE = 2 };

static struct {
    const char *host;
    const char *bind_addr;  /* source address, NULL = chosen by the kernel */
    int port;
    int count;              /* requests, or pushes with -w (0 = forever); -1 = default */
    int watch_ms;           /* > 0: subscribe instead of polling */
    int timeout_sec;        /* 0 = no socket timeout */
    int hold_sec;
    int stress;             /* parallel worker processes, 0 = off */
    int abort_after;
    int drip_ms;            /* fragment/slow: gap between bytes, 0 = mode default */
    int quiet;
    int detail;
    int reconnect;          /* [8] */
    int retries;
    enum client_mode mode;
} g_cfg = {
    .host = "127.0.0.1", .port = PROTO_DEFAULT_PORT, .count = -1, .timeout_sec = 10,
    .abort_after = 1, .reconnect = 1, .retries = 5, .mode = MODE_NORMAL,
};

/* Code of the last OP_ERROR received, 0 if the last reply was not one. */
static uint32_t g_last_error;

/* One request result, sent by a stress worker to the parent over a pipe. */
struct sample {
    uint32_t server_pid;
    uint32_t latency_us;
    int32_t  ok;
};

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ===== Output =========================================================== */

static const char *fmt_kb(uint64_t kb, char *buf, size_t len)
{
    if (kb >= 1024ULL * 1024ULL)
        snprintf(buf, len, "%.2f GiB", (double)kb / (1024.0 * 1024.0));
    else
        snprintf(buf, len, "%.1f MiB", (double)kb / 1024.0);
    return buf;
}

static const char *fmt_uptime(uint64_t sec, char *buf, size_t len)
{
    snprintf(buf, len, "%llud %02llu:%02llu:%02llu",
             (unsigned long long)(sec / 86400), (unsigned long long)(sec % 86400 / 3600),
             (unsigned long long)(sec % 3600 / 60), (unsigned long long)(sec % 60));
    return buf;
}

static void print_report(const proto_sysinfo_t *s)
{
    char up[48], total[24], freem[24], avail[24];
    double pct = s->mem_total_kb ? 100.0 * (double)s->mem_avail_kb / (double)s->mem_total_kb : 0.0;

    printf("================ NP SysInfo ================\n");
    printf("  Hostname      : %s\n", s->hostname);
    printf("  Uptime        : %s\n", fmt_uptime(s->uptime_sec, up, sizeof(up)));
    printf("  Load average  : %u.%02u  %u.%02u  %u.%02u   (1 / 5 / 15 min)\n",
           s->load_x100[0] / 100, s->load_x100[0] % 100,
           s->load_x100[1] / 100, s->load_x100[1] % 100,
           s->load_x100[2] / 100, s->load_x100[2] % 100);
    printf("  Memory total  : %s\n", fmt_kb(s->mem_total_kb, total, sizeof(total)));
    printf("  Memory free   : %s\n", fmt_kb(s->mem_free_kb, freem, sizeof(freem)));
    printf("  Memory avail  : %s  (%.1f %%)\n", fmt_kb(s->mem_avail_kb, avail, sizeof(avail)), pct);
    printf("  Tasks         : %u\n", s->nprocs);
    printf("  Served by PID : %u  (fork()ed server child)\n", s->server_pid);
    printf("============================================\n");
}

static void print_line(const proto_sysinfo_t *s)
{
    char up[48], total[24], avail[24];

    printf("seq=%-3u pid=%-6u host=%s up=%s load=%u.%02u/%u.%02u/%u.%02u mem_avail=%s/%s\n",
           s->seq, s->server_pid, s->hostname, fmt_uptime(s->uptime_sec, up, sizeof(up)),
           s->load_x100[0] / 100, s->load_x100[0] % 100,
           s->load_x100[1] / 100, s->load_x100[1] % 100,
           s->load_x100[2] / 100, s->load_x100[2] % 100,
           fmt_kb(s->mem_avail_kb, avail, sizeof(avail)),
           fmt_kb(s->mem_total_kb, total, sizeof(total)));
    fflush(stdout);
}

static void print_snapshot(const proto_sysinfo_t *s)
{
    if (g_cfg.quiet || g_cfg.count != 1 || g_cfg.watch_ms > 0)
        print_line(s);
    else
        print_report(s);
}

static void print_detail(const proto_detail_t *d)
{
    char total[24], avail[24];
    double used = d->disk_total_kb
        ? 100.0 * (double)(d->disk_total_kb - d->disk_avail_kb) / (double)d->disk_total_kb : 0.0;

    printf("============= NP SysInfo detail =============\n");
    printf("  OS            : %s %s (%s)\n", d->sysname, d->release, d->machine);
    printf("  CPU model     : %s\n", d->cpu_model);
    printf("  CPU cores     : %u online\n", d->ncpu);
    printf("  CPU usage     : %u.%02u %%   (sampled over %u ms)\n",
           d->cpu_busy_x100 / 100, d->cpu_busy_x100 % 100, d->sample_ms);
    printf("  Disk /        : %s free of %s  (%.1f %% used)\n",
           fmt_kb(d->disk_avail_kb, avail, sizeof(avail)),
           fmt_kb(d->disk_total_kb, total, sizeof(total)), used);
    printf("  Served by PID : %u  (fork()ed server child)\n", d->server_pid);
    printf("=============================================\n");
}

/* ===== Networking ======================================================= */

static int bind_local(int fd)
{
    struct sockaddr_in local;

    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    inet_pton(AF_INET, g_cfg.bind_addr, &local.sin_addr);  /* checked in parse_args */
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        LOG_ERROR("cannot bind to %s: %s", g_cfg.bind_addr, strerror(errno));
        return -1;
    }
    return 0;
}

static int connect_server(void)
{
    struct addrinfo hints, *res, *ai;
    char port[16];
    int fd = -1;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port, sizeof(port), "%d", g_cfg.port);
    rc = getaddrinfo(g_cfg.host, port, &hints, &res);
    if (rc != 0) {
        LOG_ERROR("cannot resolve %s: %s", g_cfg.host, gai_strerror(rc));
        return -1;
    }
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        int err;

        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        /* Set before connect() so SO_SNDTIMEO also bounds the handshake. */
        if (g_cfg.timeout_sec > 0)
            set_socket_timeout(fd, g_cfg.timeout_sec);
        if ((g_cfg.bind_addr == NULL || bind_local(fd) == 0) &&
            connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        err = errno;
        close(fd);
        fd = -1;
        errno = err;
    }
    freeaddrinfo(res);
    if (fd < 0)
        LOG_ERROR("cannot connect to %s:%d: %s", g_cfg.host, g_cfg.port, strerror(errno));
    else
        LOG_DEBUG(DBG_VERBOSE, "connected to %s:%d (fd %d)", g_cfg.host, g_cfg.port, fd);
    return fd;
}

/* Send every write() as its own TCP segment instead of letting Nagle merge them. */
static void set_nodelay(int fd)
{
    int on = 1;

    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) < 0)
        LOG_WARN("TCP_NODELAY: %s", strerror(errno));
}

/* Receive one reply of type want into out[size]. Returns 0, or -1 after
 * logging why; an OP_ERROR from the server leaves its code in g_last_error. */
static int recv_reply(int fd, uint8_t want, void *out, size_t size)
{
    proto_header_t hdr;
    unsigned char body[PROTO_MAX_BODY];
    int st = proto_recv_header(fd, &hdr, 1);

    g_last_error = 0;
    if (st == PROTO_OK)
        st = proto_recv_body(fd, body, hdr.body_len);
    if (st != PROTO_OK) {
        LOG_ERROR("receive failed: %s", proto_status_str(st));
        return -1;
    }
    if (hdr.opcode == OP_ERROR && hdr.body_len == sizeof(proto_error_t)) {
        proto_error_t err;

        memcpy(&err, body, sizeof(err));
        err.message[sizeof(err.message) - 1] = '\0';
        g_last_error = ntohl(err.code);
        LOG_ERROR("server error %u: %s", g_last_error, err.message);
        return -1;
    }
    if (hdr.opcode != want || hdr.body_len != size) {
        LOG_ERROR("unexpected reply %s(0x%02X), body_len=%u",
                  proto_opcode_name(hdr.opcode), hdr.opcode, hdr.body_len);
        return -1;
    }
    memcpy(out, body, size);
    return 0;
}

static int recv_snapshot(int fd, proto_sysinfo_t *out)
{
    if (recv_reply(fd, OP_SYSINFO_RESP, out, sizeof(*out)) < 0)
        return -1;
    sysinfo_ntoh(out);
    out->hostname[sizeof(out->hostname) - 1] = '\0';
    return 0;
}

static void report_sample(int report_fd, uint32_t server_pid, double seconds, int ok)
{
    struct sample smp = { server_pid, (uint32_t)(seconds * 1e6), ok };

    write_n(report_fd, &smp, sizeof(smp));
}

/* ===== Modes ============================================================ */

/* Send g_cfg.count SYSINFO_REQs. With report_fd >= 0 (stress worker) the
 * results go to the parent instead of stdout. */
static int run_poll(int fd, int report_fd)
{
    for (int i = 0; i < g_cfg.count; i++) {
        proto_sysinfo_t s = { 0 };
        double t0 = now_sec();
        int st = proto_send(fd, OP_SYSINFO_REQ, NULL, 0);
        int ok;

        if (st != PROTO_OK)
            LOG_ERROR("send failed: %s (%s)", proto_status_str(st), strerror(errno));
        ok = st == PROTO_OK && recv_snapshot(fd, &s) == 0;
        if (report_fd >= 0)
            report_sample(report_fd, s.server_pid, now_sec() - t0, ok);
        else if (ok)
            print_snapshot(&s);
        if (!ok)
            return CLIENT_FAIL;
    }
    return CLIENT_OK;
}

static int run_detail(int fd)
{
    proto_detail_t d;
    int st = proto_send(fd, OP_DETAIL_REQ, NULL, 0);

    if (st != PROTO_OK) {
        LOG_ERROR("send failed: %s (%s)", proto_status_str(st), strerror(errno));
        return CLIENT_FAIL;
    }
    if (recv_reply(fd, OP_DETAIL_RESP, &d, sizeof(d)) < 0)
        return CLIENT_FAIL;
    sysdetail_ntoh(&d);
    d.sysname[sizeof(d.sysname) - 1]     = '\0';
    d.release[sizeof(d.release) - 1]     = '\0';
    d.machine[sizeof(d.machine) - 1]     = '\0';
    d.cpu_model[sizeof(d.cpu_model) - 1] = '\0';
    print_detail(&d);
    return CLIENT_OK;
}

static int subscribe(int fd, int interval_ms, int count)
{
    proto_subscribe_t sub;
    int st;

    sub.interval_ms = htonl((uint32_t)interval_ms);
    sub.count       = htonl((uint32_t)count);
    st = proto_send(fd, OP_SUBSCRIBE_REQ, &sub, sizeof(sub));
    if (st != PROTO_OK) {
        LOG_ERROR("subscribe failed: %s (%s)", proto_status_str(st), strerror(errno));
        return -1;
    }
    return 0;
}

/*
 * One subscription on fd; *received counts pushes across reconnects.
 * Returns 1 once every requested push arrived, 0 if the connection was lost.
 * [7] An endless subscription answers every push with an ACK: the heartbeat
 * that tells the server someone is still reading.
 */
static int watch_once(int fd, int *received)
{
    int endless = g_cfg.count == 0;

    if (subscribe(fd, g_cfg.watch_ms, endless ? 0 : g_cfg.count - *received) < 0)
        return 0;
    for (;;) {
        proto_sysinfo_t s;

        if (recv_snapshot(fd, &s) < 0)
            return 0;
        print_line(&s);
        (*received)++;
        if (endless) {
            proto_ack_t ack = { htonl(s.seq) };

            /* A failed ACK is not fatal here: keep reading what the server
             * already sent, which ends with its reason for closing. */
            if (proto_send(fd, OP_ACK, &ack, sizeof(ack)) != PROTO_OK)
                LOG_DEBUG(DBG_VERBOSE, "ACK for push #%u not sent: %s", s.seq, strerror(errno));
        } else if (*received >= g_cfg.count) {
            return 1;
        }
    }
}

/* Errors that a new connection can cure. Protocol errors cannot. */
static int retryable(uint32_t err)
{
    return err == 0 || err == ERR_BUSY || err == ERR_HEARTBEAT || err == ERR_INTERNAL;
}

/* [8] Connect again with exponential backoff: 1, 2, 4, 8, 8, ... s. */
static int reconnect(void)
{
    for (int attempt = 1; attempt <= g_cfg.retries; attempt++) {
        unsigned delay = 1u << (attempt - 1 < 3 ? attempt - 1 : 3);
        int fd;

        if (delay > RETRY_MAX_DELAY_SEC)
            delay = RETRY_MAX_DELAY_SEC;
        LOG_WARN("reconnecting in %u s (attempt %d/%d)", delay, attempt, g_cfg.retries);
        sleep_ms(delay * 1000u);
        fd = connect_server();
        if (fd >= 0) {
            LOG_INFO("reconnected to %s:%d on attempt %d, subscribing again",
                     g_cfg.host, g_cfg.port, attempt);
            return fd;
        }
    }
    LOG_ERROR("server still unreachable after %d attempts, giving up", g_cfg.retries);
    return -1;
}

/* Subscribe; *fdp may be replaced by a new connection ([8]). */
static int run_watch(int *fdp)
{
    int received = 0;

    for (;;) {
        if (watch_once(*fdp, &received))
            return CLIENT_OK;
        if (!g_cfg.reconnect) {
            LOG_ERROR("connection lost after %d push(es); reconnect disabled, exiting", received);
            return CLIENT_FAIL;
        }
        if (!retryable(g_last_error)) {
            LOG_ERROR("the server refused the request; reconnecting would not help");
            return CLIENT_FAIL;
        }
        LOG_WARN("connection lost after %d push(es)", received);
        close(*fdp);
        *fdp = reconnect();
        if (*fdp < 0)
            return CLIENT_FAIL;
    }
}

/* Normal mode: request(s), then optionally keep the connection open. */
static int run_session(int *fdp, int report_fd)
{
    int rc = g_cfg.watch_ms > 0 ? run_watch(fdp) : run_poll(*fdp, report_fd);

    if (rc == CLIENT_OK && g_cfg.hold_sec > 0) {
        LOG_DEBUG(DBG_VERBOSE, "holding the connection open for %d s", g_cfg.hold_sec);
        sleep_ms((unsigned)g_cfg.hold_sec * 1000u);
    }
    return rc;
}

/* Subscribe, then leave while the server is still pushing (mechanism [2]).
 * The socket is closed with nothing unread, so the server first gets a FIN;
 * its next push draws a RST and the one after that hits EPIPE/SIGPIPE. */
static int run_abort(int fd)
{
    int interval = g_cfg.watch_ms > 0 ? g_cfg.watch_ms : ABORT_INTERVAL_MS;

    if (subscribe(fd, interval, 0) < 0)
        return CLIENT_FAIL;
    for (int i = 0; i < g_cfg.abort_after; i++) {
        proto_sysinfo_t s;

        if (recv_snapshot(fd, &s) < 0)
            return CLIENT_FAIL;
        print_line(&s);
    }
    LOG_INFO("got %d push(es); disconnecting while the server keeps pushing every %d ms",
             g_cfg.abort_after, interval);
    return CLIENT_OK;
}

/* Wait up to wait_sec for the server to react to a bad, idle or slow
 * connection; times are reported from t0. */
static int await_reaction(int fd, int wait_sec, double t0)
{
    proto_header_t hdr;
    unsigned char body[PROTO_MAX_BODY];
    double dt;
    int st;

    set_socket_timeout(fd, wait_sec);
    st = proto_recv_header(fd, &hdr, 1);
    if (st == PROTO_OK)
        st = proto_recv_body(fd, body, hdr.body_len);
    dt = now_sec() - t0;

    if (st == PROTO_OK && hdr.opcode == OP_ERROR && hdr.body_len == sizeof(proto_error_t)) {
        proto_error_t err;

        memcpy(&err, body, sizeof(err));
        err.message[sizeof(err.message) - 1] = '\0';
        printf("RESULT: server rejected the request after %.2f s: error %u (%s)\n",
               dt, ntohl(err.code), err.message);
        return CLIENT_OK;
    }
    if (st == PROTO_EOF) {
        printf("RESULT: server closed the connection after %.2f s\n", dt);
        return CLIENT_OK;
    }
    if (st == PROTO_TIMEOUT) {
        printf("RESULT: no reaction within %d s - the server child is stuck\n", wait_sec);
        return CLIENT_NO_RESPONSE;
    }
    if (st == PROTO_OK)
        printf("RESULT: unexpected reply %s(0x%02X) after %.2f s\n",
               proto_opcode_name(hdr.opcode), hdr.opcode, dt);
    else
        printf("RESULT: connection error after %.2f s: %s (%s)\n",
               dt, proto_status_str(st), strerror(errno));
    return CLIENT_FAIL;
}

/* garbage / biglen / idle (mechanism [3]) */
static int run_fault(int fd)
{
    static const char http[] = "GET / HTTP/1.1\r\nHost: np-sysinfo\r\n\r\n";
    int wait_sec = g_cfg.hold_sec > 0 ? g_cfg.hold_sec : FAULT_WAIT_SEC;
    double t0 = now_sec();
    ssize_t w = 0;

    if (g_cfg.mode == MODE_GARBAGE) {
        LOG_INFO("sending %zu bytes of non-NP data (an HTTP request)", sizeof(http) - 1);
        w = write_n(fd, http, sizeof(http) - 1);
    } else if (g_cfg.mode == MODE_BIGLEN) {
        proto_header_t hdr = { htons(PROTO_MAGIC), PROTO_VERSION, OP_SYSINFO_REQ,
                               htonl(0xFFFFFFFFu) };

        LOG_INFO("sending a valid header that claims body_len=4294967295");
        w = write_n(fd, &hdr, sizeof(hdr));
    } else {
        LOG_INFO("connected; sending nothing");
    }
    if (w < 0) {
        LOG_ERROR("send failed: %s", strerror(errno));
        return CLIENT_FAIL;
    }
    return await_reaction(fd, wait_sec, t0);
}

/*
 * Write buf one byte at a time, gap_ms apart, each byte its own TCP segment.
 * Stops early when the server answers or closes (*reacted = 1) or once
 * limit_ms have passed (0 = no limit). Returns the bytes sent, -1 on error.
 */
static ssize_t drip(int fd, const void *buf, size_t len, unsigned gap_ms,
                    unsigned limit_ms, int *reacted)
{
    const unsigned char *p = buf;
    uint64_t start = mono_ms();

    *reacted = 0;
    set_nodelay(fd);
    for (size_t i = 0; i < len; i++) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };

        if (write_n(fd, p + i, 1) < 0)
            return -1;
        LOG_DEBUG(DBG_VERBOSE, "sent byte %zu/%zu (0x%02x)", i + 1, len, p[i]);
        if (i + 1 == len)
            break;
        if (poll(&pfd, 1, (int)gap_ms) > 0) {
            *reacted = 1;
            return (ssize_t)(i + 1);
        }
        if (limit_ms > 0 && mono_ms() - start >= limit_ms)
            return (ssize_t)(i + 1);
    }
    return (ssize_t)len;
}

/* [6] One SYSINFO_REQ split into 8 one-byte TCP segments. */
static int run_fragment(int fd)
{
    proto_header_t req = { htons(PROTO_MAGIC), PROTO_VERSION, OP_SYSINFO_REQ, 0 };
    unsigned gap = g_cfg.drip_ms > 0 ? (unsigned)g_cfg.drip_ms : FRAGMENT_GAP_MS;
    double t0 = now_sec();
    proto_sysinfo_t s;
    int reacted;

    LOG_INFO("sending one 8-byte SYSINFO_REQ as 8 separate 1-byte segments, %u ms apart", gap);
    if (drip(fd, &req, sizeof(req), gap, 0, &reacted) < 0) {
        printf("RESULT: send failed after %.2f s: %s\n", now_sec() - t0, strerror(errno));
        return CLIENT_FAIL;
    }
    if (recv_snapshot(fd, &s) < 0) {
        printf("RESULT: the split request was NOT understood (%.2f s)\n", now_sec() - t0);
        return CLIENT_FAIL;
    }
    printf("RESULT: the 8 pieces were reassembled into one request; "
           "reply from server pid %u after %.2f s\n", s.server_pid, now_sec() - t0);
    return CLIENT_OK;
}

/* [6] n SYSINFO_REQs glued together into a single write(). */
static int run_pipeline(int fd)
{
    proto_header_t reqs[PIPELINE_MAX];
    int n = g_cfg.count;
    int got = 0;

    for (int i = 0; i < n; i++) {
        reqs[i].magic    = htons(PROTO_MAGIC);
        reqs[i].version  = PROTO_VERSION;
        reqs[i].opcode   = OP_SYSINFO_REQ;
        reqs[i].body_len = 0;
    }
    LOG_INFO("sending %d SYSINFO_REQs glued together in one write() (%zu bytes)",
             n, (size_t)n * sizeof(reqs[0]));
    if (write_n(fd, reqs, (size_t)n * sizeof(reqs[0])) < 0) {
        LOG_ERROR("send failed: %s", strerror(errno));
        return CLIENT_FAIL;
    }
    set_socket_timeout(fd, PIPELINE_WAIT_SEC);
    while (got < n) {
        proto_sysinfo_t s;

        if (recv_snapshot(fd, &s) < 0)
            break;
        got++;
        if (!g_cfg.quiet)
            print_line(&s);
    }
    if (got == n)
        printf("RESULT: %d/%d replies - every glued request was answered\n", got, n);
    else
        printf("RESULT: %d/%d replies - %d glued request(s) were lost\n", got, n, n - got);
    return got == n ? CLIENT_OK : CLIENT_FAIL;
}

/*
 * [5] A 16-byte SUBSCRIBE_REQ sent one byte every 3 s. Every byte arrives
 * well inside the server's 5 s per-read timeout, but the request as a whole
 * would take 45 s. Gives up after --hold seconds (default 8).
 */
static int run_slow(int fd)
{
    struct __attribute__((packed)) {
        proto_header_t    hdr;
        proto_subscribe_t sub;
    } req;
    unsigned gap = g_cfg.drip_ms > 0 ? (unsigned)g_cfg.drip_ms : SLOW_GAP_MS;
    int wait_sec = g_cfg.hold_sec > 0 ? g_cfg.hold_sec : FAULT_WAIT_SEC;
    double t0 = now_sec();
    proto_sysinfo_t s;
    ssize_t sent;
    int reacted;

    req.hdr.magic       = htons(PROTO_MAGIC);
    req.hdr.version     = PROTO_VERSION;
    req.hdr.opcode      = OP_SUBSCRIBE_REQ;
    req.hdr.body_len    = htonl(sizeof(req.sub));
    req.sub.interval_ms = htonl(1000);
    req.sub.count       = htonl(1);

    LOG_INFO("slow sender: a %zu-byte request, one byte every %u ms (%.0f s for all of it)",
             sizeof(req), gap, (double)(sizeof(req) - 1) * gap / 1000.0);
    sent = drip(fd, &req, sizeof(req), gap, (unsigned)wait_sec * 1000u, &reacted);
    if (sent < 0 || reacted) {
        if (sent < 0)
            LOG_WARN("send failed: %s", strerror(errno));
        else
            printf("(the server reacted after %zd of %zu bytes)\n", sent, sizeof(req));
        return await_reaction(fd, 2, t0);
    }
    if ((size_t)sent == sizeof(req)) {
        if (recv_snapshot(fd, &s) < 0)
            return CLIENT_FAIL;
        printf("RESULT: request completed after %.2f s and was served by pid %u\n",
               now_sec() - t0, s.server_pid);
        return CLIENT_OK;
    }
    printf("RESULT: no reaction within %d s - %zd/%zu bytes sent, the server child is still "
           "waiting for the rest\n", wait_sec, sent, sizeof(req));
    return CLIENT_NO_RESPONSE;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;

    return (x > y) - (x < y);
}

/* p-th percentile (nearest rank) of a sorted array, in milliseconds. */
static double percentile_ms(const uint32_t *sorted, size_t n, unsigned p)
{
    size_t rank = (n * p + 99) / 100;

    return sorted[rank > 0 ? rank - 1 : 0] / 1000.0;
}

/* Fork g_cfg.stress worker processes that run a normal session at once. */
static int run_stress(void)
{
    int pfd[2];
    struct sample smp;
    uint32_t *lat, *pids;
    size_t cap, nok = 0, nfail = 0, distinct = 0;
    int workers, workers_ok = 0, status;
    double t0, wall;

    if (pipe(pfd) < 0) {
        LOG_ERROR("pipe: %s", strerror(errno));
        return CLIENT_FAIL;
    }
    LOG_INFO("stress: forking %d worker process(es) x %d request(s) against %s:%d",
             g_cfg.stress, g_cfg.count, g_cfg.host, g_cfg.port);
    fflush(stdout);

    t0 = now_sec();
    for (workers = 0; workers < g_cfg.stress; workers++) {
        pid_t pid = fork();

        if (pid < 0) {
            LOG_ERROR("fork: %s", strerror(errno));
            break;
        }
        if (pid == 0) {
            int fd, rc = CLIENT_FAIL;

            close(pfd[0]);
            fd = connect_server();
            if (fd < 0) {
                report_sample(pfd[1], 0, 0.0, 0);
            } else {
                rc = run_session(&fd, pfd[1]);
                close(fd);
            }
            _exit(rc);
        }
    }
    close(pfd[1]);

    cap  = (size_t)workers * (size_t)g_cfg.count + 1;
    lat  = malloc(cap * sizeof(*lat));
    pids = malloc(cap * sizeof(*pids));
    if (lat == NULL || pids == NULL) {
        LOG_ERROR("out of memory");
        return CLIENT_FAIL;
    }
    while (read_n(pfd[0], &smp, sizeof(smp)) == (ssize_t)sizeof(smp)) {
        if (!smp.ok) {
            nfail++;
        } else if (nok < cap) {
            lat[nok]  = smp.latency_us;
            pids[nok] = smp.server_pid;
            nok++;
        }
    }
    close(pfd[0]);
    while (wait(&status) > 0)
        if (WIFEXITED(status) && WEXITSTATUS(status) == CLIENT_OK)
            workers_ok++;
    wall = now_sec() - t0;

    printf("\n===== Stress test: %d worker process(es) x %d request(s) =====\n",
           workers, g_cfg.count);
    printf("  Requests     : %zu ok, %zu failed (%.1f %% success)\n", nok, nfail,
           nok + nfail ? 100.0 * (double)nok / (double)(nok + nfail) : 0.0);
    if (nok > 0) {
        unsigned long long sum = 0;

        qsort(lat, nok, sizeof(*lat), cmp_u32);
        qsort(pids, nok, sizeof(*pids), cmp_u32);
        for (size_t i = 0; i < nok; i++) {
            sum += lat[i];
            if (i == 0 || pids[i] != pids[i - 1])
                distinct++;
        }
        printf("  Latency (ms) : avg %.3f | p50 %.3f | p95 %.3f | p99 %.3f | max %.3f\n",
               (double)sum / (double)nok / 1000.0, percentile_ms(lat, nok, 50),
               percentile_ms(lat, nok, 95), percentile_ms(lat, nok, 99),
               lat[nok - 1] / 1000.0);
    }
    printf("  Wall time    : %.3f s (%.1f ok req/s)\n", wall, wall > 0 ? (double)nok / wall : 0.0);
    printf("  Server PIDs  : %zu distinct fork()ed server children\n", distinct);
    printf("  Workers      : %d exited ok, %d failed\n", workers_ok, workers - workers_ok);

    free(lat);
    free(pids);
    return nfail == 0 && workers_ok == workers ? CLIENT_OK : CLIENT_FAIL;
}

/* ===== Command line ===================================================== */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  -H, --host HOST       server address (default 127.0.0.1)\n"
            "  -p, --port PORT       server port (default %d)\n"
            "  -d, --debug LEVEL     runtime debug level: 0 quiet, 1 verbose, 2 packet trace\n"
            "  -n, --count N         requests per connection (default 1); with -w, pushes\n"
            "                        (default 0 = until Ctrl+C); with --mode pipeline, requests\n"
            "  -w, --watch MS        subscribe: the server pushes a snapshot every MS ms\n"
            "  -t, --timeout SEC     socket timeout (default 10, 0 = none)\n"
            "  -q, --quiet           one line per snapshot\n"
            "      --detail          request OS, CPU and disk details instead\n"
            "      --bind ADDR       connect from this local address (e.g. 127.0.0.2)\n"
            "      --hold SEC        keep the connection open SEC s after the last reply\n"
            "                        (fault modes: how long to wait for a reaction, default %d)\n"
            "      --stress N        fork N clients in parallel and print latency statistics\n"
            "      --no-reconnect    [8] with -w: exit when the connection is lost\n"
            "      --retries N       [8] reconnect attempts before giving up (default 5)\n"
            "      --mode MODE       robustness demo: garbage | biglen | idle   [3]\n"
            "                        abort [2] | slow [5] | fragment | pipeline [6]\n"
            "      --abort-after K   abort mode: disconnect after K pushes (default 1)\n"
            "      --drip-ms MS      fragment/slow: pause between bytes (default %d / %d)\n"
            "  -h, --help            show this help\n",
            prog, PROTO_DEFAULT_PORT, FAULT_WAIT_SEC, FRAGMENT_GAP_MS, SLOW_GAP_MS);
}

enum {
    OPT_HOLD = 256, OPT_STRESS, OPT_MODE, OPT_ABORT_AFTER, OPT_DETAIL, OPT_BIND,
    OPT_NO_RECONNECT, OPT_RETRIES, OPT_DRIP_MS
};

static int parse_mode(const char *s)
{
    for (size_t i = 0; i < sizeof(mode_names) / sizeof(mode_names[0]); i++) {
        if (strcmp(s, mode_names[i]) == 0) {
            g_cfg.mode = (enum client_mode)i;
            return 0;
        }
    }
    return -1;
}

static int parse_bind(const char *s)
{
    struct in_addr a;

    if (inet_pton(AF_INET, s, &a) != 1)
        return -1;
    g_cfg.bind_addr = s;
    return 0;
}

static int parse_args(int argc, char *argv[])
{
    static const struct option longopts[] = {
        { "host",         required_argument, NULL, 'H' },
        { "port",         required_argument, NULL, 'p' },
        { "debug",        required_argument, NULL, 'd' },
        { "count",        required_argument, NULL, 'n' },
        { "watch",        required_argument, NULL, 'w' },
        { "timeout",      required_argument, NULL, 't' },
        { "quiet",        no_argument,       NULL, 'q' },
        { "help",         no_argument,       NULL, 'h' },
        { "hold",         required_argument, NULL, OPT_HOLD },
        { "stress",       required_argument, NULL, OPT_STRESS },
        { "mode",         required_argument, NULL, OPT_MODE },
        { "abort-after",  required_argument, NULL, OPT_ABORT_AFTER },
        { "detail",       no_argument,       NULL, OPT_DETAIL },
        { "bind",         required_argument, NULL, OPT_BIND },
        { "no-reconnect", no_argument,       NULL, OPT_NO_RECONNECT },
        { "retries",      required_argument, NULL, OPT_RETRIES },
        { "drip-ms",      required_argument, NULL, OPT_DRIP_MS },
        { NULL, 0, NULL, 0 }
    };
    int c, bad = 0;

    while ((c = getopt_long(argc, argv, "H:p:d:n:w:t:qh", longopts, NULL)) != -1) {
        switch (c) {
        case 'H': g_cfg.host = optarg; break;
        case 'p': bad = parse_int_range(optarg, 1, 65535, &g_cfg.port); break;
        case 'd': bad = parse_int_range(optarg, 0, 2, &g_runtime_debug_level); break;
        case 'n': bad = parse_int_range(optarg, 0, 1000000, &g_cfg.count); break;
        case 'w': bad = parse_int_range(optarg, 1, 60000, &g_cfg.watch_ms); break;
        case 't': bad = parse_int_range(optarg, 0, 3600, &g_cfg.timeout_sec); break;
        case 'q': g_cfg.quiet = 1; break;
        case OPT_HOLD:         bad = parse_int_range(optarg, 0, 3600, &g_cfg.hold_sec); break;
        case OPT_STRESS:       bad = parse_int_range(optarg, 1, 2000, &g_cfg.stress); break;
        case OPT_MODE:         bad = parse_mode(optarg); break;
        case OPT_ABORT_AFTER:  bad = parse_int_range(optarg, 1, 1000, &g_cfg.abort_after); break;
        case OPT_DETAIL:       g_cfg.detail = 1; break;
        case OPT_BIND:         bad = parse_bind(optarg); break;
        case OPT_NO_RECONNECT: g_cfg.reconnect = 0; break;
        case OPT_RETRIES:      bad = parse_int_range(optarg, 1, 100, &g_cfg.retries); break;
        case OPT_DRIP_MS:      bad = parse_int_range(optarg, 1, 60000, &g_cfg.drip_ms); break;
        case 'h':
            usage(argv[0]);
            exit(EXIT_SUCCESS);
        default:
            usage(argv[0]);
            return -1;
        }
        if (bad) {
            LOG_ERROR("invalid value for option -%c: %s", c < 256 ? c : '-', optarg);
            return -1;
        }
    }
    if (optind < argc) {
        LOG_ERROR("unexpected argument: %s", argv[optind]);
        return -1;
    }
    if (g_cfg.count < 0)
        g_cfg.count = g_cfg.watch_ms > 0 ? 0 : g_cfg.mode == MODE_PIPELINE ? PIPELINE_DEFAULT : 1;
    if (g_cfg.count == 0 && g_cfg.watch_ms == 0) {
        LOG_ERROR("-n 0 (forever) needs -w");
        return -1;
    }
    if (g_cfg.mode == MODE_PIPELINE && g_cfg.count > PIPELINE_MAX) {
        LOG_ERROR("--mode pipeline sends at most %d requests", PIPELINE_MAX);
        return -1;
    }
    if (g_cfg.stress > 0 && (g_cfg.mode != MODE_NORMAL || g_cfg.watch_ms > 0)) {
        LOG_ERROR("--stress works with plain requests only (no --mode, no -w)");
        return -1;
    }
    if (g_cfg.detail && (g_cfg.mode != MODE_NORMAL || g_cfg.watch_ms > 0 || g_cfg.stress > 0)) {
        LOG_ERROR("--detail cannot be combined with --mode, -w or --stress");
        return -1;
    }
    /* A push interval longer than the socket timeout would look like a dead server. */
    if (g_cfg.watch_ms > 0 && g_cfg.timeout_sec > 0 && g_cfg.watch_ms / 1000 + 2 > g_cfg.timeout_sec)
        g_cfg.timeout_sec = g_cfg.watch_ms / 1000 + 2;
    if (!LOG_DEBUG_ENABLED && g_runtime_debug_level > 0)
        LOG_WARN("built without -DENABLE_DEBUG: -d %d has no effect", g_runtime_debug_level);
    return 0;
}

int main(int argc, char *argv[])
{
    int fd, rc;

    log_init("client");
    if (parse_args(argc, argv) < 0)
        return CLIENT_FAIL;
    set_signal(SIGPIPE, SIG_IGN);   /* a vanished server must not kill the client */

    if (g_cfg.stress > 0)
        return run_stress();

    fd = connect_server();
    if (fd < 0) {
        /* [8] A subscriber started before its server simply waits for it. */
        if (g_cfg.watch_ms == 0 || g_cfg.mode != MODE_NORMAL || !g_cfg.reconnect)
            return CLIENT_FAIL;
        fd = reconnect();
        if (fd < 0)
            return CLIENT_FAIL;
    }
    switch (g_cfg.mode) {
    case MODE_NORMAL:   rc = g_cfg.detail ? run_detail(fd) : run_session(&fd, -1); break;
    case MODE_ABORT:    rc = run_abort(fd); break;
    case MODE_FRAGMENT: rc = run_fragment(fd); break;
    case MODE_PIPELINE: rc = run_pipeline(fd); break;
    case MODE_SLOW:     rc = run_slow(fd); break;
    default:            rc = run_fault(fd); break;
    }
    if (fd >= 0)
        close(fd);
    return rc;
}
