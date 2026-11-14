/*
 * utils.c - implementation of libutils.so (API documented in utils.h).
 */
#define _DEFAULT_SOURCE     /* getloadavg, localtime_r */

#include "utils.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* ===== 1. Logger ======================================================== */

int g_runtime_debug_level = 0;

#define LOG_LINE_MAX    1024
#define HEXDUMP_MAX     256

static const char *g_log_tag = "app";

void log_init(const char *tag)
{
    if (tag != NULL)
        g_log_tag = tag;
}

static const char *level_name(int level)
{
    switch (level) {
    case LOG_LVL_ERROR: return "ERROR";
    case LOG_LVL_WARN:  return "WARN";
    case LOG_LVL_INFO:  return "INFO";
    default:            return "DEBUG";
    }
}

/* vsnprintf at buf + *off; on truncation *off stops at the last byte. */
static void __attribute__((format(printf, 4, 0)))
buf_vappend(char *buf, size_t cap, size_t *off, const char *fmt, va_list ap)
{
    int n;

    if (*off + 1 >= cap)
        return;
    n = vsnprintf(buf + *off, cap - *off, fmt, ap);
    if (n < 0)
        return;
    *off += (size_t)n;
    if (*off >= cap)
        *off = cap - 1;
}

static void __attribute__((format(printf, 4, 5)))
buf_append(char *buf, size_t cap, size_t *off, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    buf_vappend(buf, cap, off, fmt, ap);
    va_end(ap);
}

void log_write(int level, const char *file, int line, const char *fmt, ...)
{
    char buf[LOG_LINE_MAX];
    size_t cap = sizeof(buf) - 1;       /* keep one byte for the '\n' */
    size_t off;
    struct timeval tv;
    struct tm tm;
    va_list ap;
    int saved_errno = errno;

    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);
    off = strftime(buf, cap, "%H:%M:%S", &tm);
    buf_append(buf, cap, &off, ".%03ld [%s:%d] %-5s ",
               (long)tv.tv_usec / 1000, g_log_tag, (int)getpid(), level_name(level));
    if (file != NULL)
        buf_append(buf, cap, &off, "%s:%d: ", file, line);

    va_start(ap, fmt);
    buf_vappend(buf, cap, &off, fmt, ap);
    va_end(ap);

    buf[off++] = '\n';
    /* One write() per line; plain write() because write_all() logs itself. */
    if (write(STDERR_FILENO, buf, off) < 0) {
        /* stderr is gone: nothing useful left to do with the message */
    }
    errno = saved_errno;
}

void log_hexdump(const char *label, const void *data, size_t len)
{
    const unsigned char *p = data;
    size_t shown = len < HEXDUMP_MAX ? len : HEXDUMP_MAX;

    log_write(LOG_LVL_DEBUG, NULL, 0, "%s: %zu byte(s)", label, len);
    for (size_t row = 0; row < shown; row += 16) {
        char line[96];
        size_t off = 0;

        buf_append(line, sizeof(line), &off, "    %04zx  ", row);
        for (size_t i = 0; i < 16; i++) {
            if (row + i < shown)
                buf_append(line, sizeof(line), &off, "%02x ", p[row + i]);
            else
                buf_append(line, sizeof(line), &off, "   ");
            if (i == 7)
                buf_append(line, sizeof(line), &off, " ");
        }
        buf_append(line, sizeof(line), &off, " |");
        for (size_t i = 0; i < 16 && row + i < shown; i++)
            buf_append(line, sizeof(line), &off, "%c",
                       isprint(p[row + i]) ? p[row + i] : '.');
        buf_append(line, sizeof(line), &off, "|");
        log_write(LOG_LVL_DEBUG, NULL, 0, "%s", line);
    }
    if (shown < len)
        log_write(LOG_LVL_DEBUG, NULL, 0, "    ... %zu more byte(s) not shown",
                  len - shown);
}

/* ===== 2. Socket I/O ==================================================== */

ssize_t write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    size_t done = 0;

    while (done < n) {
        ssize_t w = write(fd, p + done, n - done);

        if (w >= 0) {
            done += (size_t)w;
        } else if (errno == EINTR) {
            LOG_DEBUG(DBG_FLOW, "write_all: interrupted by a signal, retrying");
        } else {
            LOG_DEBUG(DBG_FLOW, "write_all: fd %d failed after %zu of %zu bytes: %s",
                      fd, done, n, strerror(errno));
            return -1;
        }
    }
    return (ssize_t)done;
}

static long long monotonic_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

ssize_t read_line_timeout(int fd, char *buf, size_t cap, int timeout_ms)
{
    long long deadline = timeout_ms >= 0 ? monotonic_ms() + timeout_ms : 0;
    size_t len = 0;

    while (len < cap - 1) {
        char c;
        ssize_t r;

        if (timeout_ms >= 0) {
            /* [4] Wait only for the time left until the deadline, so a
             * client sending one byte at a time cannot stretch it. */
            struct pollfd pfd = { .fd = fd, .events = POLLIN };
            long long left = deadline - monotonic_ms();
            int ready = left > 0 ? poll(&pfd, 1, (int)left) : 0;

            if (ready == 0) {
                buf[len] = '\0';
                return READ_TIMEOUT;
            }
            if (ready < 0) {
                if (errno == EINTR)
                    continue;
                buf[len] = '\0';
                return READ_ERR;
            }
        }
        r = read(fd, &c, 1);
        if (r == 1) {
            if (c == '\n') {
                if (len > 0 && buf[len - 1] == '\r')
                    len--;
                buf[len] = '\0';
                return (ssize_t)len;
            }
            buf[len++] = c;
        } else if (r == 0) {
            buf[len] = '\0';
            return READ_EOF;
        } else if (errno != EINTR) {
            buf[len] = '\0';
            return READ_ERR;
        }
    }
    buf[len] = '\0';
    return READ_TOO_LONG;
}

ssize_t read_line(int fd, char *buf, size_t cap)
{
    return read_line_timeout(fd, buf, cap, -1);
}

/* ===== 3. System information ============================================ */

/* MemTotal and MemAvailable from /proc/meminfo, in kB. */
static int read_meminfo(unsigned long long *total, unsigned long long *avail)
{
    FILE *fp = fopen("/proc/meminfo", "r");
    char line[128];
    int found = 0;

    if (fp == NULL)
        return -1;
    while (found != 3 && fgets(line, sizeof(line), fp) != NULL) {
        if (sscanf(line, "MemTotal: %llu kB", total) == 1)
            found |= 1;
        else if (sscanf(line, "MemAvailable: %llu kB", avail) == 1)
            found |= 2;
    }
    fclose(fp);
    return found == 3 ? 0 : -1;
}

int sysinfo_collect(struct host_info *out)
{
    struct sysinfo si;

    memset(out, 0, sizeof(*out));
    if (gethostname(out->hostname, sizeof(out->hostname)) < 0)
        snprintf(out->hostname, sizeof(out->hostname), "unknown");
    out->hostname[sizeof(out->hostname) - 1] = '\0';

    if (sysinfo(&si) < 0) {
        LOG_ERROR("sysinfo() failed: %s", strerror(errno));
        return -1;
    }
    out->uptime_sec = (unsigned long)si.uptime;
    out->procs = si.procs;
    if (getloadavg(out->load, 3) != 3)
        out->load[0] = out->load[1] = out->load[2] = 0.0;

    if (read_meminfo(&out->mem_total_kb, &out->mem_avail_kb) < 0) {
        unsigned long long unit = si.mem_unit ? si.mem_unit : 1;

        out->mem_total_kb = si.totalram * unit / 1024;
        out->mem_avail_kb = (si.freeram + si.bufferram) * unit / 1024;
    }
    LOG_DEBUG(DBG_FLOW, "sysinfo: host=%s uptime=%lus load1=%.2f mem_avail=%llu/%llu kB",
              out->hostname, out->uptime_sec, out->load[0],
              out->mem_avail_kb, out->mem_total_kb);
    return 0;
}

int sysinfo_format(const struct host_info *info, unsigned seq, char *buf, size_t len)
{
    unsigned long up = info->uptime_sec;
    int n;

    n = snprintf(buf, len,
                 "OK seq=%u pid=%d host=%s up=%lud %02lu:%02lu:%02lu "
                 "load=%.2f/%.2f/%.2f mem_avail=%.2f/%.2f GiB procs=%u\n",
                 seq, (int)getpid(), info->hostname,
                 up / 86400, up / 3600 % 24, up / 60 % 60, up % 60,
                 info->load[0], info->load[1], info->load[2],
                 info->mem_avail_kb / 1048576.0, info->mem_total_kb / 1048576.0,
                 info->procs);
    if (n < 0)
        return 0;
    return (size_t)n < len ? n : (int)len - 1;
}

/* ===== 4. Misc ========================================================== */

int parse_int_range(const char *s, int min, int max, int *out)
{
    char *end;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < min || v > max)
        return -1;
    *out = (int)v;
    return 0;
}

void sleep_ms(unsigned int ms)
{
    struct timespec req = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    struct timespec rem;

    while (nanosleep(&req, &rem) < 0 && errno == EINTR)
        req = rem;
}

int set_signal(int sig, void (*handler)(int))
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = handler;
    sa.sa_flags = 0;    /* no SA_RESTART: a blocking accept() returns EINTR */
    return sigaction(sig, &sa, NULL);
}
