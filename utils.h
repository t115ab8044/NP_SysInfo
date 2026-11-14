/*
 * utils.h - public API of libutils.so, linked dynamically by server and client.
 *
 *   1. Logger      LOG_INFO/WARN/ERROR are always on. LOG_DEBUG is switched
 *                  twice: at compile time (-DENABLE_DEBUG) and at run time
 *                  (g_runtime_debug_level, set with -d).
 *   2. Socket I/O  write_all() and read_line() cope with short reads and
 *                  writes and with EINTR; read_line() also caps the line
 *                  length (robustness mechanism [3]), and
 *                  read_line_timeout() adds a deadline for the whole line
 *                  (mechanism [4]).
 *   3. Sysinfo     collect a snapshot of this host and format it as a line.
 *   4. Misc        integer parsing, EINTR-safe sleep, sigaction() wrapper.
 *
 * Requests and replies are plain text lines ending in '\n':
 *
 *   client -> server   SYSINFO               one snapshot
 *                      WATCH <count> <ms>    <count> snapshots, <ms> apart
 *   server -> client   OK seq=1 pid=... ...  one line per snapshot
 *                      ERROR <reason>        and the server closes
 */
#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>
#include <sys/types.h>

#define DEFAULT_PORT    9090
#define MAX_LINE        128     /* longest request line, '\n' included */
#define WATCH_MAX_COUNT 100
#define WATCH_MIN_MS    100
#define WATCH_MAX_MS    10000

/* ===== 1. Logger ======================================================== */

/* 0 = normal messages only, 1 = program flow, 2 = flow + bytes sent/received */
extern int g_runtime_debug_level;

#define DBG_FLOW  1
#define DBG_BYTES 2

enum log_level { LOG_LVL_ERROR, LOG_LVL_WARN, LOG_LVL_INFO, LOG_LVL_DEBUG };

/* Set the tag printed on every line, e.g. "server" or "client". */
void log_init(const char *tag);

/*
 * Print "HH:MM:SS.mmm [tag:pid] LEVEL msg" to stderr with a single write(),
 * so lines from different processes never mix. file:line is printed when
 * file is not NULL. errno is preserved.
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
        if (g_runtime_debug_level >= DBG_BYTES)                             \
            log_hexdump((label), (data), (len));                            \
    } while (0)
#else
/* Compiled out: no code, no format strings, no run-time level check. */
#define LOG_DEBUG_ENABLED 0
#define LOG_DEBUG(lvl, ...)           ((void)0)
#define LOG_HEXDUMP(label, data, len) ((void)0)
#endif

/* ===== 2. Socket I/O ==================================================== */

/*
 * Write all n bytes, retrying after EINTR and short writes.
 * Returns n, or -1 with errno set (EPIPE or ECONNRESET: the peer is gone;
 * ETIMEDOUT: the peer stopped acknowledging, mechanism [2b]).
 * Unless the process ignores SIGPIPE,
 * writing to a peer that has gone kills the process instead of returning
 * EPIPE (robustness mechanism [2a]).
 */
ssize_t write_all(int fd, const void *buf, size_t n);

enum {
    READ_EOF      = -1,     /* peer closed before a whole line arrived */
    READ_ERR      = -2,     /* read() failed; errno tells why */
    READ_TOO_LONG = -3,     /* cap - 1 bytes and still no '\n' */
    READ_TIMEOUT  = -4      /* the deadline passed before the '\n' */
};

/*
 * Read one line, one byte at a time, so nothing after the '\n' is consumed.
 * At most cap - 1 bytes are stored (mechanism [3]); the '\n' and a '\r'
 * before it are removed and the line is NUL-terminated.
 * Returns the line length, or READ_EOF / READ_ERR / READ_TOO_LONG.
 */
ssize_t read_line(int fd, char *buf, size_t cap);

/*
 * read_line() with one deadline for the whole line (mechanism [4]): the
 * '\n' must arrive within timeout_ms of the call, however the bytes trickle
 * in. timeout_ms < 0 waits for ever. On READ_TIMEOUT, buf holds the bytes
 * received so far.
 */
ssize_t read_line_timeout(int fd, char *buf, size_t cap, int timeout_ms);

/* ===== 3. System information ============================================ */

struct host_info {
    char hostname[64];
    unsigned long uptime_sec;
    double load[3];                 /* 1, 5 and 15 minute load average */
    unsigned long long mem_total_kb;
    unsigned long long mem_avail_kb;
    unsigned procs;                 /* number of processes */
};

/* Fill *out from gethostname(), sysinfo(2), getloadavg() and /proc/meminfo. */
int sysinfo_collect(struct host_info *out);

/*
 * Format one reply line, '\n' included:
 *   OK seq=1 pid=1234 host=... up=0d 01:02:03 load=0.00/0.00/0.00 mem_avail=... procs=...
 * Returns the line length.
 */
int sysinfo_format(const struct host_info *info, unsigned seq, char *buf, size_t len);

/* ===== 4. Misc ========================================================== */

/* Parse a decimal integer in [min, max]. Returns 0, or -1 if s is invalid. */
int parse_int_range(const char *s, int min, int max, int *out);

/* Sleep for ms milliseconds, resuming after signal interruptions. */
void sleep_ms(unsigned int ms);

/* Set a signal's handler (or SIG_IGN / SIG_DFL) with sigaction(), no SA_RESTART. */
int set_signal(int sig, void (*handler)(int));

#endif /* UTILS_H */
