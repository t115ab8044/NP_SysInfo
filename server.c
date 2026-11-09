/*
 * server.c - NP SysInfo server: one fork()ed child process per client.
 *
 * The parent only accepts connections. Every client is served by its own
 * child process, which exits when the client leaves. No threads are used.
 *
 * Robustness mechanisms, all ON by default. Each one can be switched off
 * for the With/Without comparison:
 *
 *   [1] SIGCHLD handler reaps finished children        --no-sigchld -> zombies
 *       in a loop: signals do not queue               --single-wait -> one
 *                                                      waitpid per signal
 *   [2] SIGPIPE ignored, EPIPE handled via write_n     --no-sigpipe -> a child
 *       writing to a client that already left is killed by SIGPIPE
 *   [3] header validation + SO_RCVTIMEO/SO_SNDTIMEO   --no-validate,
 *       --no-timeout -> garbage or idle clients pin a child forever
 *   [4] connection limit, total and per client IP      --no-limit -> one
 *       host can fork the server without bound and starve everybody else
 *   [5] whole-request deadline                         --no-deadline -> a
 *       client dripping one byte every few seconds pins a child for hours
 *   [6] TCP stream framing: exact-length reads         --naive-recv -> split
 *       or coalesced messages are misparsed or lost
 *   [7] heartbeat: endless subscriptions need ACKs     --no-heartbeat -> a
 *       frozen client (Ctrl+Z) is pushed to for minutes
 *   [8] automatic reconnect with backoff               (client side)
 *   [9] children die with the parent (PDEATHSIG)       --no-pdeathsig ->
 *       kill -9 of the parent leaves orphans behind
 */
#define _DEFAULT_SOURCE     /* strsignal */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "utils.h"

#define LISTEN_BACKLOG      128
#define REAP_RING           256     /* child-exit records awaiting a log line */
#define SUB_MIN_INTERVAL_MS 50
#define SUB_MAX_INTERVAL_MS 60000
#define HB_MIN_TIMEOUT_MS   3000    /* [7] no ACK for this long ends a subscription */
#define ACK_BUDGET_MS       1000    /* [7] an ACK, once started, must arrive within */
#define DETAIL_SAMPLE_MS    200     /* CPU usage sampling window */
#define SHUTDOWN_WAIT_MS    2000    /* time children get to exit on shutdown */
#define KEEP_SERVING        (-1)    /* request handled, wait for the next one */

/* Child exit codes; the parent logs them when it reaps the child. */
enum child_exit {
    CHILD_OK          = 0,  /* client closed the connection */
    CHILD_IO_ERROR    = 1,
    CHILD_TIMEOUT     = 2,  /* socket timeout: idle or stalled client */
    CHILD_BAD_REQUEST = 3,  /* failed validation or unknown opcode */
    CHILD_PEER_GONE   = 4,  /* write hit EPIPE/ECONNRESET: client left */
    CHILD_HEARTBEAT   = 5,  /* [7] no ACK in time: client frozen */
    CHILD_DEADLINE    = 6   /* [5] request not complete in time: slow sender */
};

/* recv_request() results besides enum proto_status */
enum { REQ_NOMEM = -100, REQ_INCOMPLETE, REQ_BODY_TIMEOUT, REQ_BODY_LOST };

static struct {
    int port;
    int timeout_sec;        /* 0 = no socket timeout */
    int reap_children;      /* [1] */
    int reap_loop;          /* [1] keep calling waitpid() in the handler */
    int ignore_sigpipe;     /* [2] */
    int validate_header;    /* [3] */
    int max_clients;        /* [4] 0 = unlimited */
    int max_per_ip;         /* [4] 0 = unlimited */
    int deadline;           /* [5] */
    int framing;            /* [6] */
    int heartbeat;          /* [7] */
    int pdeathsig;          /* [9] */
} g_cfg = { PROTO_DEFAULT_PORT, 5, 1, 1, 1, 1, 128, 0, 1, 1, 1, 1 };

static volatile sig_atomic_t g_stop;

/* Filled by the SIGCHLD handler, logged later by the main loop. */
static volatile sig_atomic_t g_reap_pid[REAP_RING];
static volatile sig_atomic_t g_reap_status[REAP_RING];
static volatile sig_atomic_t g_reap_head;   /* children reaped so far */
static int g_reap_tail;                     /* records already logged */
static int g_spawned;                       /* children forked so far */
static int g_rejected;                      /* [4] connections refused */

/* [4] Live children and the client address each one serves (parent only). */
struct child {
    pid_t pid;
    struct in_addr ip;
};
static struct child *g_children;
static size_t g_nchildren, g_children_cap;

/* ===== Signals (parent) ================================================= */

/*
 * [1] Reap every finished child. Signals do not queue: while one SIGCHLD is
 * pending, more exits add nothing, so one signal may stand for many dead
 * children and the handler must loop until waitpid() finds no more.
 * --single-wait reaps one child per signal to show what goes wrong.
 * The status is kept (instead of waitpid(-1, NULL, WNOHANG)) only so main()
 * can log how each child ended. Logging is not async-signal-safe, so the
 * handler just records the exit and main() does the logging.
 */
static void on_sigchld(int sig)
{
    int saved_errno = errno;
    int status;
    pid_t pid;

    (void)sig;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int h = g_reap_head;

        g_reap_pid[h % REAP_RING]    = pid;
        g_reap_status[h % REAP_RING] = status;
        g_reap_head = h + 1;
        if (!g_cfg.reap_loop)
            break;
    }
    errno = saved_errno;
}

static void on_shutdown(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void install_signals(void)
{
    struct sigaction sa;

    /* The handlers only run while main() waits in pselect(), which then
     * fails with EINTR so the loop logs reaped children or shuts down. */
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_shutdown;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (g_cfg.reap_children) {
        sa.sa_handler = on_sigchld;
        sa.sa_flags   = SA_NOCLDSTOP;
        sigaction(SIGCHLD, &sa, NULL);
    }
    /* Otherwise SIGCHLD stays SIG_DFL. Setting SIG_IGN would make Linux
     * auto-reap children and hide the zombies the demo is meant to show. */

    /* The parent itself always ignores SIGPIPE: it writes to clients only
     * to refuse them ([4]). Mechanism [2] is about the children, which set
     * their own disposition in run_child(). */
    set_signal(SIGPIPE, SIG_IGN);
}

static const char *child_exit_str(int code)
{
    switch (code) {
    case CHILD_OK:          return "client closed normally";
    case CHILD_IO_ERROR:    return "I/O error";
    case CHILD_TIMEOUT:     return "socket timeout";
    case CHILD_BAD_REQUEST: return "bad request rejected";
    case CHILD_PEER_GONE:   return "client gone, EPIPE/ECONNRESET handled";
    case CHILD_HEARTBEAT:   return "heartbeat lost, client frozen or unreachable";
    case CHILD_DEADLINE:    return "request deadline exceeded, slow sender cut off";
    default:                return "unknown";
    }
}

/* ===== Child table (parent) ============================================= */

static void track_child(pid_t pid, struct in_addr ip)
{
    if (g_nchildren == g_children_cap) {
        size_t cap = g_children_cap ? g_children_cap * 2 : 64;
        struct child *p = realloc(g_children, cap * sizeof(*p));

        if (p == NULL) {
            LOG_ERROR("out of memory tracking child %d", (int)pid);
            return;
        }
        g_children = p;
        g_children_cap = cap;
    }
    g_children[g_nchildren].pid = pid;
    g_children[g_nchildren].ip  = ip;
    g_nchildren++;
}

static void untrack_child(pid_t pid)
{
    for (size_t i = 0; i < g_nchildren; i++) {
        if (g_children[i].pid == pid) {
            g_children[i] = g_children[--g_nchildren];
            return;
        }
    }
}

/* After lost exit records: drop entries whose process no longer exists. */
static void prune_children(void)
{
    for (size_t i = 0; i < g_nchildren; ) {
        if (kill(g_children[i].pid, 0) < 0 && errno == ESRCH)
            g_children[i] = g_children[--g_nchildren];
        else
            i++;
    }
}

static size_t children_from(struct in_addr ip)
{
    size_t n = 0;

    for (size_t i = 0; i < g_nchildren; i++)
        if (g_children[i].ip.s_addr == ip.s_addr)
            n++;
    return n;
}

static void report_reaped_children(void)
{
    int head = g_reap_head;

    if (head - g_reap_tail > REAP_RING) {
        LOG_WARN("%d child exit record(s) lost (ring full)", head - g_reap_tail - REAP_RING);
        g_reap_tail = head - REAP_RING;
        prune_children();
    }
    while (g_reap_tail < head) {
        int i = g_reap_tail++ % REAP_RING;
        int pid = g_reap_pid[i];
        int status = g_reap_status[i];

        untrack_child(pid);
        if (WIFSIGNALED(status))
            LOG_WARN("reaped child %d: killed by signal %d (%s) | active=%zu",
                     pid, WTERMSIG(status), strsignal(WTERMSIG(status)), g_nchildren);
        else
            LOG_INFO("reaped child %d: exit %d (%s) | active=%zu",
                     pid, WEXITSTATUS(status), child_exit_str(WEXITSTATUS(status)), g_nchildren);
    }
}

/* ===== Request handling (child) ========================================= */

static int status_to_errcode(int st)
{
    switch (st) {
    case PROTO_BAD_MAGIC:   return ERR_BAD_MAGIC;
    case PROTO_BAD_VERSION: return ERR_BAD_VERSION;
    case PROTO_BAD_LENGTH:  return ERR_BAD_LENGTH;
    default:                return ERR_INTERNAL;
    }
}

/* A reply could not be written: decide how the child ends. */
static int send_failed(const char *peer, const char *what, int st)
{
    if (st == PROTO_TIMEOUT) {
        LOG_WARN("%s: %s timed out (SO_SNDTIMEO)", peer, what);
        return CHILD_TIMEOUT;
    }
    if (errno == EPIPE || errno == ECONNRESET) {
        LOG_WARN("%s: %s failed: %s -> client gone, exiting cleanly",
                 peer, what, strerror(errno));
        return CHILD_PEER_GONE;
    }
    LOG_WARN("%s: %s failed: %s", peer, what, strerror(errno));
    return CHILD_IO_ERROR;
}

static int send_snapshot(int cfd, uint32_t seq)
{
    proto_sysinfo_t snap;

    if (sysinfo_collect(&snap) < 0)
        return proto_send_error(cfd, ERR_INTERNAL, "cannot read system information");
    snap.seq = seq;
    sysinfo_hton(&snap);
    return proto_send(cfd, OP_SYSINFO_RESP, &snap, sizeof(snap));
}

static int serve_detail(int cfd, const char *peer)
{
    proto_detail_t d;
    int st;

    LOG_DEBUG(DBG_VERBOSE, "%s: DETAIL_REQ", peer);
    if (sysdetail_collect(&d, DETAIL_SAMPLE_MS) < 0) {
        st = proto_send_error(cfd, ERR_INTERNAL, "cannot read system details");
    } else {
        sysdetail_hton(&d);
        st = proto_send(cfd, OP_DETAIL_RESP, &d, sizeof(d));
    }
    return st == PROTO_OK ? KEEP_SERVING : send_failed(peer, "detail reply", st);
}

/* [7] Liveness of a subscriber, judged by its OP_ACKs. */
struct heartbeat {
    int enforce;            /* end the subscription when ACKs stop */
    int peer_closed;        /* client sent FIN: no more ACKs can come */
    uint32_t timeout_ms;
    uint32_t last_seq;      /* push acknowledged most recently */
    uint64_t last_ack_ms;
};

/*
 * [7] Wait one push interval while reading the client's ACKs. A frozen
 * client (Ctrl+Z), a hung one or one cut off by the network still has a
 * kernel that accepts our pushes, so every write succeeds; only the missing
 * ACKs show that nobody is reading. The ACKs are read even when the
 * heartbeat is not enforced, so they never pile up unread.
 *
 * A FIN from the client is not treated as "client gone": TCP allows a
 * half-closed peer that still reads. Writes find out whether it left
 * (mechanism [2]).
 */
static int wait_for_acks(int cfd, const char *peer, struct heartbeat *hb, uint32_t interval_ms)
{
    uint64_t end = mono_ms() + interval_ms;

    for (;;) {
        struct pollfd pfd = { .fd = cfd, .events = POLLIN };
        uint64_t now = mono_ms();
        uint64_t wake = end;
        proto_header_t hdr;
        proto_ack_t ack;
        uint64_t deadline;
        int st, pr;

        if (hb->enforce && now - hb->last_ack_ms > hb->timeout_ms) {
            LOG_WARN("%s: no ACK for %.1f s (last one for push #%u) -> client frozen or "
                     "unreachable, ending the subscription",
                     peer, (double)(now - hb->last_ack_ms) / 1000.0, hb->last_seq);
            proto_send_error(cfd, ERR_HEARTBEAT, NULL);
            return CHILD_HEARTBEAT;
        }
        if (now >= end)
            return KEEP_SERVING;
        if (hb->enforce && hb->last_ack_ms + hb->timeout_ms + 1 < wake)
            wake = hb->last_ack_ms + hb->timeout_ms + 1;
        if (hb->peer_closed) {
            sleep_ms((unsigned)(wake - now));
            continue;
        }
        pr = poll(&pfd, 1, (int)(wake - now));
        if (pr < 0 && errno != EINTR) {
            LOG_WARN("%s: poll: %s", peer, strerror(errno));
            return CHILD_IO_ERROR;
        }
        if (pr <= 0)
            continue;

        st = proto_recv_header_budget(cfd, &hdr, 1, ACK_BUDGET_MS, &deadline);
        if (st == PROTO_EOF) {
            LOG_DEBUG(DBG_VERBOSE, "%s: client half-closed its side, no more ACKs", peer);
            hb->peer_closed = 1;
            continue;
        }
        if (st == PROTO_IO_ERR && (errno == ECONNRESET || errno == EPIPE)) {
            LOG_WARN("%s: connection reset during the subscription -> client gone", peer);
            return CHILD_PEER_GONE;
        }
        if (st != PROTO_OK) {
            LOG_WARN("%s: bad data during the subscription: %s", peer, proto_status_str(st));
            proto_send_error(cfd, status_to_errcode(st), NULL);
            return CHILD_BAD_REQUEST;
        }
        if (hdr.opcode != OP_ACK || hdr.body_len != sizeof(ack)) {
            LOG_WARN("%s: only ACK is allowed during a subscription, got %s(0x%02X) body_len=%u",
                     peer, proto_opcode_name(hdr.opcode), hdr.opcode, hdr.body_len);
            proto_send_error(cfd, ERR_UNKNOWN_OP, "only ACK is allowed during a subscription");
            return CHILD_BAD_REQUEST;
        }
        st = proto_recv_body_until(cfd, &ack, sizeof(ack), deadline);
        if (st != PROTO_OK) {
            LOG_WARN("%s: reading an ACK failed: %s", peer, proto_status_str(st));
            return CHILD_IO_ERROR;
        }
        hb->last_ack_ms = mono_ms();
        hb->last_seq = ntohl(ack.seq);
        LOG_DEBUG(DBG_VERBOSE, "%s: ACK for push #%u", peer, hb->last_seq);
    }
}

static int serve_subscribe(int cfd, const char *peer, const unsigned char *body,
                           uint32_t body_len)
{
    proto_subscribe_t sub;
    struct heartbeat hb;
    uint32_t interval, count;

    if (body_len != sizeof(sub)) {
        LOG_WARN("%s: SUBSCRIBE_REQ body is %u bytes, expected %zu",
                 peer, body_len, sizeof(sub));
        proto_send_error(cfd, ERR_BAD_LENGTH, "subscribe body must be 8 bytes");
        return CHILD_BAD_REQUEST;
    }
    memcpy(&sub, body, sizeof(sub));
    interval = ntohl(sub.interval_ms);
    count    = ntohl(sub.count);
    if (interval < SUB_MIN_INTERVAL_MS)
        interval = SUB_MIN_INTERVAL_MS;
    if (interval > SUB_MAX_INTERVAL_MS)
        interval = SUB_MAX_INTERVAL_MS;

    /* An endless subscription ends only when the client is noticed gone,
     * so it is the one that needs the heartbeat. */
    memset(&hb, 0, sizeof(hb));
    hb.enforce     = g_cfg.heartbeat && count == 0;
    hb.timeout_ms  = 2 * interval + 1000 > HB_MIN_TIMEOUT_MS ? 2 * interval + 1000
                                                             : HB_MIN_TIMEOUT_MS;
    hb.last_ack_ms = mono_ms();

    if (count == 0)
        LOG_INFO("%s: subscribe every %u ms until the client leaves | heartbeat %s",
                 peer, interval, hb.enforce ? "ON" : "OFF");
    else
        LOG_INFO("%s: subscribe every %u ms, %u push(es)", peer, interval, count);
    if (hb.enforce)
        LOG_DEBUG(DBG_VERBOSE, "%s: no ACK for %u ms ends the subscription", peer, hb.timeout_ms);

    for (uint32_t seq = 1; count == 0 || seq <= count; seq++) {
        int st = send_snapshot(cfd, seq);

        if (st != PROTO_OK) {
            char what[32];

            snprintf(what, sizeof(what), "push #%u", seq);
            return send_failed(peer, what, st);
        }
        LOG_DEBUG(DBG_VERBOSE, "%s: push #%u sent", peer, seq);
        if (count == 0 || seq < count) {
            int rc = wait_for_acks(cfd, peer, &hb, interval);

            if (rc != KEEP_SERVING)
                return rc;
        }
    }
    return KEEP_SERVING;
}

static int dispatch(int cfd, const char *peer, const proto_header_t *hdr,
                    const unsigned char *body)
{
    int st;

    switch (hdr->opcode) {
    case OP_SYSINFO_REQ:
        LOG_DEBUG(DBG_VERBOSE, "%s: SYSINFO_REQ", peer);
        st = send_snapshot(cfd, 0);
        return st == PROTO_OK ? KEEP_SERVING : send_failed(peer, "reply", st);
    case OP_SUBSCRIBE_REQ:
        return serve_subscribe(cfd, peer, body, hdr->body_len);
    case OP_DETAIL_REQ:
        return serve_detail(cfd, peer);
    case OP_ACK:
        LOG_WARN("%s: ACK outside a subscription", peer);
        proto_send_error(cfd, ERR_UNKNOWN_OP, "ACK outside a subscription");
        return CHILD_BAD_REQUEST;
    default:
        LOG_WARN("%s: unknown opcode 0x%02X", peer, hdr->opcode);
        proto_send_error(cfd, ERR_UNKNOWN_OP, NULL);
        return CHILD_BAD_REQUEST;
    }
}

/*
 * [5][6] Receive one request the robust way. TCP is a byte stream, so the
 * header and then exactly body_len bytes are read with read_n, whatever
 * pieces they arrive in, and bytes of the next request stay in the socket
 * for the next call. Once the first byte is in, the whole request must
 * arrive within the timeout ([5]); before that SO_RCVTIMEO bounds the wait.
 */
static int recv_request(int cfd, const char *peer, proto_header_t *hdr, unsigned char **body)
{
    unsigned budget = g_cfg.deadline && g_cfg.timeout_sec > 0 ? (unsigned)g_cfg.timeout_sec * 1000u : 0;
    uint64_t deadline;
    int st = proto_recv_header_budget(cfd, hdr, g_cfg.validate_header, budget, &deadline);

    *body = NULL;
    if (st != PROTO_OK || hdr->body_len == 0)
        return st;

    /* With validation off, body_len is trusted as received: a forged
     * 0xFFFFFFFF makes the child allocate 4 GiB and wait for data that
     * never comes. That is the hole mechanism [3] closes. */
    if (hdr->body_len > PROTO_MAX_BODY)
        LOG_WARN("%s: trusting body_len=%u (validation disabled)", peer, hdr->body_len);
    *body = malloc(hdr->body_len);
    if (*body == NULL) {
        LOG_ERROR("%s: malloc(%u) failed", peer, hdr->body_len);
        return REQ_NOMEM;
    }
    st = proto_recv_body_until(cfd, *body, hdr->body_len, deadline);
    if (st == PROTO_OK)
        return PROTO_OK;
    free(*body);
    *body = NULL;
    if (st == PROTO_DEADLINE)
        return st;
    LOG_WARN("%s: reading %u-byte body failed: %s", peer, hdr->body_len, proto_status_str(st));
    return st == PROTO_TIMEOUT ? REQ_BODY_TIMEOUT : REQ_BODY_LOST;
}

/*
 * [6] switched off (--naive-recv): the classic beginner's receive, which
 * assumes one read() returns exactly one message. On a byte stream a message
 * can arrive in pieces (a short read is taken for a broken message) and
 * several messages can arrive glued together (all but the first are lost).
 */
static int recv_request_naive(int cfd, const char *peer, proto_header_t *hdr, unsigned char **body)
{
    static unsigned char buf[PROTO_HEADER_SIZE + PROTO_MAX_BODY];
    ssize_t n = read(cfd, buf, sizeof(buf));
    size_t used;

    *body = NULL;
    memset(hdr, 0, sizeof(*hdr));
    if (n == 0)
        return PROTO_EOF;
    if (n < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? PROTO_TIMEOUT : PROTO_IO_ERR;
    LOG_HEXDUMP("RX naive read()", buf, (size_t)n);
    if ((size_t)n < PROTO_HEADER_SIZE) {
        LOG_WARN("%s: naive recv: one read() returned %zd byte(s), less than the 8-byte "
                 "header -> taken as a broken message", peer, n);
        return REQ_INCOMPLETE;
    }
    memcpy(hdr, buf, PROTO_HEADER_SIZE);
    hdr->magic    = ntohs(hdr->magic);
    hdr->body_len = ntohl(hdr->body_len);
    if (g_cfg.validate_header) {
        if (hdr->magic != PROTO_MAGIC)
            return PROTO_BAD_MAGIC;
        if (hdr->version != PROTO_VERSION)
            return PROTO_BAD_VERSION;
        if (hdr->body_len > PROTO_MAX_BODY)
            return PROTO_BAD_LENGTH;
    }
    if (hdr->body_len > (size_t)n - PROTO_HEADER_SIZE) {
        LOG_WARN("%s: naive recv: header promises %u body byte(s), this read() had %zd "
                 "-> taken as a broken message", peer, hdr->body_len, n - PROTO_HEADER_SIZE);
        return REQ_INCOMPLETE;
    }
    used = PROTO_HEADER_SIZE + hdr->body_len;
    if ((size_t)n > used)
        LOG_WARN("%s: naive recv: one read() returned %zd bytes; parsed 1 message, the other "
                 "%zu byte(s) are discarded (requests lost)", peer, n, (size_t)n - used);
    if (hdr->body_len > 0) {
        *body = malloc(hdr->body_len);
        if (*body == NULL)
            return REQ_NOMEM;
        memcpy(*body, buf + PROTO_HEADER_SIZE, hdr->body_len);
    }
    return PROTO_OK;
}

/* Serve requests until the client leaves; returns an enum child_exit. */
static int serve_client(int cfd, const char *peer)
{
    unsigned served = 0;

    if (g_cfg.timeout_sec > 0 && set_socket_timeout(cfd, g_cfg.timeout_sec) < 0)
        LOG_WARN("%s: cannot set socket timeout: %s", peer, strerror(errno));

    for (;;) {
        proto_header_t hdr;
        unsigned char *body;
        int rc;
        int st = g_cfg.framing ? recv_request(cfd, peer, &hdr, &body)
                               : recv_request_naive(cfd, peer, &hdr, &body);

        switch (st) {
        case PROTO_OK:
            break;
        case PROTO_EOF:
            LOG_INFO("%s: client closed the connection after %u request(s)", peer, served);
            return CHILD_OK;
        case PROTO_TIMEOUT:
            LOG_WARN("%s: no request within %d s, closing (SO_RCVTIMEO)",
                     peer, g_cfg.timeout_sec);
            return CHILD_TIMEOUT;
        case PROTO_DEADLINE:
            LOG_WARN("%s: request still incomplete %d s after its first byte -> slow sender "
                     "cut off (deadline)", peer, g_cfg.timeout_sec);
            proto_send_error(cfd, ERR_DEADLINE, NULL);
            return CHILD_DEADLINE;
        case PROTO_IO_ERR:
            LOG_WARN("%s: receive failed: %s", peer, strerror(errno));
            return CHILD_IO_ERROR;
        case REQ_BODY_TIMEOUT:
            return CHILD_TIMEOUT;
        case REQ_BODY_LOST:
            return CHILD_IO_ERROR;
        case REQ_NOMEM:
            proto_send_error(cfd, ERR_INTERNAL, "out of memory");
            return CHILD_BAD_REQUEST;
        case REQ_INCOMPLETE:
            proto_send_error(cfd, ERR_BAD_LENGTH, "incomplete message");
            return CHILD_BAD_REQUEST;
        default:
            LOG_WARN("%s: rejected header: %s (magic=0x%04X ver=%u op=0x%02X body_len=%u)",
                     peer, proto_status_str(st), hdr.magic, hdr.version,
                     hdr.opcode, hdr.body_len);
            proto_send_error(cfd, status_to_errcode(st), NULL);
            return CHILD_BAD_REQUEST;
        }

        served++;
        rc = dispatch(cfd, peer, &hdr, body);
        free(body);
        if (rc != KEEP_SERVING)
            return rc;
    }
}

/*
 * Close after an error reply without losing it. Closing a socket that still
 * has unread input makes the kernel send RST, which can destroy the reply
 * in flight. Half-close first and drain briefly, so the client reads the
 * error and then a clean FIN.
 */
static void lingering_close(int cfd)
{
    unsigned char sink[512];
    int reads = 0;

    shutdown(cfd, SHUT_WR);
    set_socket_timeout(cfd, 1);
    while (reads < 16 && read(cfd, sink, sizeof(sink)) > 0)
        reads++;
    close(cfd);
}

static void __attribute__((noreturn))
run_child(int lfd, int cfd, const char *peer, pid_t parent, const sigset_t *mask)
{
    int rc;

    close(lfd);
    /* Children react to Ctrl+C and kill like ordinary processes. The
     * parent's blocked-signal mask is inherited across fork(), so restore it. */
    set_signal(SIGINT, SIG_DFL);
    set_signal(SIGTERM, SIG_DFL);
    set_signal(SIGCHLD, SIG_DFL);
    /* [2] write() to a vanished client returns EPIPE instead of killing us. */
    set_signal(SIGPIPE, g_cfg.ignore_sigpipe ? SIG_IGN : SIG_DFL);
    sigprocmask(SIG_SETMASK, mask, NULL);
    /* [9] If the parent dies, even by kill -9, take this child down too
     * instead of leaving an orphan behind (e.g. one stuck on a bad client). */
    if (g_cfg.pdeathsig) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent)
            _exit(CHILD_IO_ERROR);
    }

    LOG_DEBUG(DBG_VERBOSE, "%s: child started", peer);
    rc = serve_client(cfd, peer);
    if (rc == CHILD_BAD_REQUEST || rc == CHILD_DEADLINE)
        lingering_close(cfd);
    else
        close(cfd);
    _exit(rc);
}

/* ===== Main (parent) ==================================================== */

/* [4] NULL if one more child may be forked for ip, else the reason why not. */
static const char *limit_reason(struct in_addr ip, char *buf, size_t len)
{
    size_t n;

    if (g_cfg.max_clients > 0 && g_nchildren >= (size_t)g_cfg.max_clients) {
        snprintf(buf, len, "server busy: %zu/%d connections in use",
                 g_nchildren, g_cfg.max_clients);
        return buf;
    }
    if (g_cfg.max_per_ip > 0 && (n = children_from(ip)) >= (size_t)g_cfg.max_per_ip) {
        snprintf(buf, len, "too many connections from your address (%zu/%d)",
                 n, g_cfg.max_per_ip);
        return buf;
    }
    return NULL;
}

/* [4] Refuse a connection without forking: ERR_BUSY, then a clean close. */
static void reject_busy(int cfd, const char *peer, const char *why)
{
    unsigned char sink[256];

    g_rejected++;
    LOG_WARN("rejected %s: %s (no fork)", peer, why);
    proto_send_error(cfd, ERR_BUSY, why);
    shutdown(cfd, SHUT_WR);
    /* Drain the request already sent, so close() does not answer with RST. */
    while (recv(cfd, sink, sizeof(sink), MSG_DONTWAIT) > 0)
        ;
    close(cfd);
}

static int open_listener(int port)
{
    struct sockaddr_in addr;
    int on = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        LOG_ERROR("socket: %s", strerror(errno));
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0)
        LOG_WARN("SO_REUSEADDR: %s", strerror(errno));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, LISTEN_BACKLOG) < 0) {
        LOG_ERROR("cannot listen on port %d: %s", port, strerror(errno));
        close(fd);
        return -1;
    }
    /* pselect() may report a connection that is reset before accept();
     * non-blocking, accept() then fails with EAGAIN instead of hanging. */
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0)
        LOG_WARN("O_NONBLOCK: %s", strerror(errno));
    return fd;
}

static void format_peer(const struct sockaddr_in *addr, char *buf, size_t len)
{
    char ip[INET_ADDRSTRLEN];

    if (inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip)) == NULL)
        snprintf(ip, sizeof(ip), "?");
    snprintf(buf, len, "%s:%u", ip, (unsigned)ntohs(addr->sin_port));
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  -p, --port PORT        listen port (default %d)\n"
            "  -d, --debug LEVEL      runtime debug level: 0 quiet, 1 verbose, 2 packet trace\n"
            "  -t, --timeout SEC      per-connection socket timeout and request deadline\n"
            "                         (default 5, 0 = none)\n"
            "  -m, --max-clients N    [4] at most N children at once (default 128)\n"
            "      --max-per-ip N     [4] at most N children per client address (default off)\n"
            "  -h, --help             show this help\n"
            "Robustness switches for the With/Without demo (all ON by default):\n"
            "      --no-sigchld       [1] never reap children -> zombie processes\n"
            "      --single-wait      [1] one waitpid() per SIGCHLD -> coalesced signals leave zombies\n"
            "      --no-sigpipe       [2] keep default SIGPIPE -> child killed on write to a closed socket\n"
            "      --no-validate      [3] trust packet headers (no magic/version/length check)\n"
            "      --no-timeout       [3] no SO_RCVTIMEO/SO_SNDTIMEO (same as -t 0)\n"
            "      --no-limit         [4] fork for every connection, no limits\n"
            "      --no-deadline      [5] only per-read timeouts -> a slow sender pins a child\n"
            "      --naive-recv       [6] assume one read() = one message\n"
            "      --no-heartbeat     [7] keep pushing to a client that stopped ACKing\n"
            "      --no-pdeathsig     [9] children outlive a killed parent as orphans\n",
            prog, PROTO_DEFAULT_PORT);
}

enum {
    OPT_NO_SIGCHLD = 256, OPT_SINGLE_WAIT, OPT_NO_SIGPIPE, OPT_NO_VALIDATE, OPT_NO_TIMEOUT,
    OPT_MAX_PER_IP, OPT_NO_LIMIT, OPT_NO_DEADLINE, OPT_NAIVE_RECV, OPT_NO_HEARTBEAT,
    OPT_NO_PDEATHSIG
};

static int parse_args(int argc, char *argv[])
{
    static const struct option longopts[] = {
        { "port",         required_argument, NULL, 'p' },
        { "debug",        required_argument, NULL, 'd' },
        { "timeout",      required_argument, NULL, 't' },
        { "max-clients",  required_argument, NULL, 'm' },
        { "max-per-ip",   required_argument, NULL, OPT_MAX_PER_IP },
        { "help",         no_argument,       NULL, 'h' },
        { "no-sigchld",   no_argument,       NULL, OPT_NO_SIGCHLD },
        { "single-wait",  no_argument,       NULL, OPT_SINGLE_WAIT },
        { "no-sigpipe",   no_argument,       NULL, OPT_NO_SIGPIPE },
        { "no-validate",  no_argument,       NULL, OPT_NO_VALIDATE },
        { "no-timeout",   no_argument,       NULL, OPT_NO_TIMEOUT },
        { "no-limit",     no_argument,       NULL, OPT_NO_LIMIT },
        { "no-deadline",  no_argument,       NULL, OPT_NO_DEADLINE },
        { "naive-recv",   no_argument,       NULL, OPT_NAIVE_RECV },
        { "no-heartbeat", no_argument,       NULL, OPT_NO_HEARTBEAT },
        { "no-pdeathsig", no_argument,       NULL, OPT_NO_PDEATHSIG },
        { NULL, 0, NULL, 0 }
    };
    int c, no_limit = 0;

    while ((c = getopt_long(argc, argv, "p:d:t:m:h", longopts, NULL)) != -1) {
        switch (c) {
        case 'p':
            if (parse_int_range(optarg, 1, 65535, &g_cfg.port) < 0) {
                LOG_ERROR("invalid port: %s", optarg);
                return -1;
            }
            break;
        case 'd':
            if (parse_int_range(optarg, 0, 2, &g_runtime_debug_level) < 0) {
                LOG_ERROR("debug level must be 0, 1 or 2: %s", optarg);
                return -1;
            }
            break;
        case 't':
            if (parse_int_range(optarg, 0, 3600, &g_cfg.timeout_sec) < 0) {
                LOG_ERROR("invalid timeout: %s", optarg);
                return -1;
            }
            break;
        case 'm':
            if (parse_int_range(optarg, 1, 100000, &g_cfg.max_clients) < 0) {
                LOG_ERROR("invalid max clients: %s", optarg);
                return -1;
            }
            break;
        case OPT_MAX_PER_IP:
            if (parse_int_range(optarg, 1, 100000, &g_cfg.max_per_ip) < 0) {
                LOG_ERROR("invalid max per IP: %s", optarg);
                return -1;
            }
            break;
        case OPT_NO_SIGCHLD:   g_cfg.reap_children   = 0; break;
        case OPT_SINGLE_WAIT:  g_cfg.reap_loop       = 0; break;
        case OPT_NO_SIGPIPE:   g_cfg.ignore_sigpipe  = 0; break;
        case OPT_NO_VALIDATE:  g_cfg.validate_header = 0; break;
        case OPT_NO_TIMEOUT:   g_cfg.timeout_sec     = 0; break;
        case OPT_NO_LIMIT:     no_limit              = 1; break;
        case OPT_NO_DEADLINE:  g_cfg.deadline        = 0; break;
        case OPT_NAIVE_RECV:   g_cfg.framing         = 0; break;
        case OPT_NO_HEARTBEAT: g_cfg.heartbeat       = 0; break;
        case OPT_NO_PDEATHSIG: g_cfg.pdeathsig       = 0; break;
        case 'h':
            usage(argv[0]);
            exit(EXIT_SUCCESS);
        default:
            usage(argv[0]);
            return -1;
        }
    }
    if (optind < argc) {
        LOG_ERROR("unexpected argument: %s", argv[optind]);
        return -1;
    }
    if (no_limit) {
        g_cfg.max_clients = 0;
        g_cfg.max_per_ip  = 0;
    }
    if (!LOG_DEBUG_ENABLED && g_runtime_debug_level > 0)
        LOG_WARN("built without -DENABLE_DEBUG: -d %d has no effect", g_runtime_debug_level);
    return 0;
}

static const char *on_off(int on)
{
    return on ? "ON" : "OFF";
}

static void log_config(pid_t self)
{
    char timeout_desc[16], limit_desc[64], per_ip[24];
    const char *reap_desc = !g_cfg.reap_children ? "OFF"
                          : g_cfg.reap_loop      ? "ON"
                                                 : "ON, single waitpid per signal";

    if (g_cfg.timeout_sec > 0)
        snprintf(timeout_desc, sizeof(timeout_desc), "%ds", g_cfg.timeout_sec);
    else
        snprintf(timeout_desc, sizeof(timeout_desc), "OFF");
    if (g_cfg.max_per_ip > 0)
        snprintf(per_ip, sizeof(per_ip), "%d", g_cfg.max_per_ip);
    else
        snprintf(per_ip, sizeof(per_ip), "off");
    if (g_cfg.max_clients > 0 || g_cfg.max_per_ip > 0)
        snprintf(limit_desc, sizeof(limit_desc), "%d total, %s per IP",
                 g_cfg.max_clients, per_ip);
    else
        snprintf(limit_desc, sizeof(limit_desc), "OFF");

    LOG_INFO("NP SysInfo server pid %d listening on 0.0.0.0:%d", (int)self, g_cfg.port);
    LOG_INFO("robustness: [1] SIGCHLD reaping %s | [2] SIGPIPE ignored %s | "
             "[3] header validation %s, socket timeout %s",
             reap_desc, on_off(g_cfg.ignore_sigpipe),
             on_off(g_cfg.validate_header), timeout_desc);
    LOG_INFO("robustness: [4] connection limit %s | [5] request deadline %s | "
             "[6] stream framing %s | [7] heartbeat %s | [9] die with parent %s",
             limit_desc, g_cfg.deadline && g_cfg.timeout_sec > 0 ? timeout_desc : "OFF",
             on_off(g_cfg.framing), on_off(g_cfg.heartbeat), on_off(g_cfg.pdeathsig));
    LOG_INFO("debug logs: compile-time %s, runtime level %d",
             LOG_DEBUG_ENABLED ? "ENABLED" : "compiled out", g_runtime_debug_level);
}

/* Stop accepting, ask the remaining children to finish, reap them. */
static void shut_down(int lfd, const sigset_t *wait_mask)
{
    close(lfd);             /* new connections are refused from now on */
    report_reaped_children();
    if (g_nchildren > 0) {
        uint64_t end = mono_ms() + SHUTDOWN_WAIT_MS;

        LOG_INFO("shutting down: listener closed, sending SIGTERM to %zu child(ren)",
                 g_nchildren);
        for (size_t i = 0; i < g_nchildren; i++)
            kill(g_children[i].pid, SIGTERM);
        while (g_cfg.reap_children && g_nchildren > 0 && mono_ms() < end) {
            struct timespec ts = { 0, 50 * 1000000L };

            pselect(0, NULL, NULL, NULL, &ts, wait_mask);   /* lets SIGCHLD in */
            report_reaped_children();
        }
    }
    LOG_INFO("shut down: %d connection(s) served, %d rejected, %zu child(ren) not reaped",
             g_spawned, g_rejected, g_nchildren);
}

int main(int argc, char *argv[])
{
    sigset_t blocked, wait_mask;
    pid_t self = getpid();
    int lfd;

    log_init("server");
    if (parse_args(argc, argv) < 0)
        return EXIT_FAILURE;
    install_signals();

    /* Keep the handlers blocked except inside pselect(). Otherwise a SIGCHLD
     * landing between report_reaped_children() and the wait would leave its
     * exit record unlogged until the next connection arrives. */
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGCHLD);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);
    sigprocmask(SIG_BLOCK, &blocked, &wait_mask);

    lfd = open_listener(g_cfg.port);
    if (lfd < 0)
        return EXIT_FAILURE;
    log_config(self);

    while (!g_stop) {
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        char peer[INET_ADDRSTRLEN + 8];
        char why[PROTO_ERRMSG_LEN];
        fd_set rfds;
        int cfd;
        pid_t pid;

        report_reaped_children();
        FD_ZERO(&rfds);
        FD_SET(lfd, &rfds);
        if (pselect(lfd + 1, &rfds, NULL, NULL, NULL, &wait_mask) < 0) {
            if (errno != EINTR)
                LOG_ERROR("pselect: %s", strerror(errno));
            continue;               /* EINTR: SIGCHLD or shutdown request */
        }
        cfd = accept(lfd, (struct sockaddr *)&addr, &addr_len);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED)
                continue;           /* the client left before accept() */
            LOG_ERROR("accept: %s", strerror(errno));
            if (errno == EMFILE || errno == ENFILE)
                sleep_ms(100);      /* out of descriptors: back off */
            continue;
        }
        format_peer(&addr, peer, sizeof(peer));

        if (limit_reason(addr.sin_addr, why, sizeof(why)) != NULL) {
            reject_busy(cfd, peer, why);
            continue;
        }

        pid = fork();
        if (pid < 0) {
            LOG_ERROR("fork for %s: %s", peer, strerror(errno));
            proto_send_error(cfd, ERR_INTERNAL, "server cannot fork");
            close(cfd);
            continue;
        }
        if (pid == 0)
            run_child(lfd, cfd, peer, self, &wait_mask);

        track_child(pid, addr.sin_addr);
        g_spawned++;
        LOG_INFO("accepted %s -> child pid %d (connection #%d) | active=%zu",
                 peer, (int)pid, g_spawned, g_nchildren);
        close(cfd);         /* the child owns the connection now */
    }

    shut_down(lfd, &wait_mask);
    return EXIT_SUCCESS;
}
