/*
 * utils.h - public API of libutils.so, linked dynamically by server and client.
 *
 *   1. Logger      LOG_INFO/WARN/ERROR are always on. LOG_DEBUG is gated
 *                  twice: at compile time (-DENABLE_DEBUG) and at run time
 *                  (g_runtime_debug_level).
 *   2. Robust I/O  read_n / write_n finish short transfers and retry EINTR;
 *                  read_n_until adds a whole-transfer deadline.
 *   3. Protocol    send, receive and validate NP packets (see protocol.h).
 *   4. Sysinfo     collect a proto_sysinfo_t snapshot of the local host.
 *   5. Misc        command-line integer parsing, EINTR-safe sleep.
 */
#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "protocol.h"

/* ===== 1. Logger ======================================================== */

/* 0 = quiet (info/warn/error only), 1 = verbose, 2 = packet trace (hex dumps) */
extern int g_runtime_debug_level;

#define DBG_VERBOSE 1
#define DBG_PACKET  2

enum log_level { LOG_LVL_ERROR, LOG_LVL_WARN, LOG_LVL_INFO, LOG_LVL_DEBUG };

/* Set the tag printed on every line, e.g. "server" or "client". */
void log_init(const char *tag);

/*
 * Emit "HH:MM:SS.mmm [tag:pid] LEVEL msg" to stderr with a single write(),
 * so lines from concurrent processes never interleave. file:line is printed
 * when file is non-NULL. errno is preserved.
 */
void log_write(int level, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/* Hex + ASCII dump of the first 256 bytes; call it through LOG_HEXDUMP. */
void log_hexdump(const char *label, const void *data, size_t len);

#define LOG_ERROR(...) log_write(LOG_LVL_ERROR, NULL, 0, __VA_ARGS__)
#define LOG_WARN(...)  log_write(LOG_LVL_WARN,  NULL, 0, __VA_ARGS__)
#define LOG_INFO(...)  log_write(LOG_LVL_INFO,  NULL, 0, __VA_ARGS__)

#ifdef ENABLE_DEBUG
#define LOG_DEBUG_ENABLED 1
#define LOG_DEBUG(lvl, ...)                                                 \
    do {                                                                    \
        if (g_runtime_debug_level >= (lvl))                                 \
            log_write(LOG_LVL_DEBUG, __FILE__, __LINE__, __VA_ARGS__);      \
    } while (0)
#define LOG_HEXDUMP(label, data, len)                                       \
    do {                                                                    \
        if (g_runtime_debug_level >= DBG_PACKET)                            \
            log_hexdump((label), (data), (len));                            \
    } while (0)
#else
/* Compiled out: no code, no format strings, no runtime level check. */
#define LOG_DEBUG_ENABLED 0
#define LOG_DEBUG(lvl, ...)           ((void)0)
#define LOG_HEXDUMP(label, data, len) ((void)0)
#endif

/* ===== 2. Robust I/O ==================================================== */

/*
 * Read exactly n bytes, retrying on EINTR and short reads.
 * Returns n on success, a smaller count (possibly 0) if the peer closed the
 * connection first, or -1 with errno set: EAGAIN/EWOULDBLOCK when
 * SO_RCVTIMEO expires, ECONNRESET when the peer reset the connection, ...
 */
ssize_t read_n(int fd, void *buf, size_t n);

/*
 * Write exactly n bytes, retrying on EINTR and short writes.
 * Returns n, or -1 with errno set: EPIPE/ECONNRESET when the peer is gone,
 * EAGAIN when SO_SNDTIMEO expires. Uses plain write() on purpose: unless the
 * process ignores SIGPIPE, writing to a closed peer kills it instead of
 * returning EPIPE (robustness mechanism 2).
 */
ssize_t write_n(int fd, const void *buf, size_t n);

/*
 * Like read_n(), but give up when the CLOCK_MONOTONIC time deadline_ms
 * (see mono_ms) passes, however the data trickles in: -1 with errno
 * ETIMEDOUT. deadline_ms = 0 means no deadline (plain read_n).
 */
ssize_t read_n_until(int fd, void *buf, size_t n, uint64_t deadline_ms);

/* Milliseconds on CLOCK_MONOTONIC, which never jumps when the clock is set. */
uint64_t mono_ms(void);

/* Apply SO_RCVTIMEO and SO_SNDTIMEO. seconds = 0 blocks forever. */
int set_socket_timeout(int fd, int seconds);

/* ===== 3. Protocol helpers ============================================== */

enum proto_status {
    PROTO_OK          =  0,
    PROTO_EOF         = -1,     /* peer closed the connection */
    PROTO_TIMEOUT     = -2,     /* socket timeout expired */
    PROTO_IO_ERR      = -3,     /* other I/O error, errno set */
    PROTO_BAD_MAGIC   = -4,
    PROTO_BAD_VERSION = -5,
    PROTO_BAD_LENGTH  = -6,     /* body_len > PROTO_MAX_BODY */
    PROTO_DEADLINE    = -7      /* message not complete before its deadline */
};

/* Send header + body in one write_n(). body may be NULL when body_len is 0. */
int proto_send(int fd, uint8_t opcode, const void *body, uint32_t body_len);

/* Send an OP_ERROR packet. A NULL msg uses the default text for code. */
int proto_send_error(int fd, uint32_t code, const char *msg);

/*
 * Read the 8-byte header and convert it to host byte order. When validate is
 * non-zero, also check magic, version and the body_len limit (robustness
 * mechanism 3); otherwise the header is trusted as received. *hdr is filled
 * even when a PROTO_BAD_* status is returned, so the caller can log it.
 */
int proto_recv_header(int fd, proto_header_t *hdr, int validate);

/*
 * proto_recv_header() with a whole-message deadline (mechanism [5]). Waiting
 * for the first byte is bounded only by SO_RCVTIMEO (an idle client); once
 * it arrives, the rest of the message must arrive within budget_ms. The
 * absolute deadline is stored in *deadline_ms for proto_recv_body_until().
 * budget_ms = 0 disables the deadline (*deadline_ms = 0).
 */
int proto_recv_header_budget(int fd, proto_header_t *hdr, int validate,
                             unsigned budget_ms, uint64_t *deadline_ms);

/* Read exactly len body bytes. */
int proto_recv_body(int fd, void *buf, uint32_t len);

/* Read exactly len body bytes before deadline_ms (0 = no deadline). */
int proto_recv_body_until(int fd, void *buf, uint32_t len, uint64_t deadline_ms);

const char *proto_status_str(int status);
const char *proto_opcode_name(uint8_t opcode);
const char *proto_errcode_str(uint32_t code);

/* ===== 4. System information ============================================ */

/*
 * Fill *out, in host byte order, with hostname, uptime, load average, memory
 * and the caller's PID; seq is left 0. Returns 0, or -1 if sysinfo(2) fails.
 */
int sysinfo_collect(proto_sysinfo_t *out);

/* Convert a snapshot between host and network byte order, in place. */
void sysinfo_hton(proto_sysinfo_t *s);
void sysinfo_ntoh(proto_sysinfo_t *s);

/*
 * Fill *out, in host byte order, with OS, CPU and disk details. CPU usage is
 * sampled from /proc/stat over sample_ms. Returns 0, or -1 if uname fails.
 */
int sysdetail_collect(proto_detail_t *out, unsigned sample_ms);
void sysdetail_hton(proto_detail_t *d);
void sysdetail_ntoh(proto_detail_t *d);

/* ===== 5. Misc ========================================================== */

/* Parse a decimal integer in [min, max]. Returns 0, or -1 if s is invalid. */
int parse_int_range(const char *s, int min, int max, int *out);

/* Sleep for ms milliseconds, resuming after signal interruptions. */
void sleep_ms(unsigned int ms);

/* Set a signal to SIG_IGN or SIG_DFL through sigaction(). Returns 0 or -1. */
int set_signal(int sig, void (*disposition)(int));

#endif /* UTILS_H */
