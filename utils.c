/*
 * utils.c - implementation of libutils.so (API documented in utils.h).
 */
#define _DEFAULT_SOURCE     /* getloadavg, htobe64, localtime_r */

#include "utils.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <endian.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/utsname.h>
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

/* vsnprintf at buf + *off; on truncation *off is clamped to the last byte. */
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

/* The logger's own output path. It must not use write_n(), which logs. */
static void log_emit(const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t w = write(STDERR_FILENO, buf, len);

        if (w < 0) {
            if (errno == EINTR)
                continue;
            return;                     /* stderr unusable: drop the line */
        }
        buf += w;
        len -= (size_t)w;
    }
}

void log_write(int level, const char *file, int line, const char *fmt, ...)
{
    char buf[LOG_LINE_MAX];
    size_t cap = sizeof(buf) - 1;       /* keep one byte for the '\n' */
    size_t off;
    struct timespec ts;
    struct tm tm;
    va_list ap;
    int saved_errno = errno;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    off = strftime(buf, cap, "%H:%M:%S", &tm);
    buf_append(buf, cap, &off, ".%03ld [%s:%d] %-5s ",
               ts.tv_nsec / 1000000L, g_log_tag, (int)getpid(), level_name(level));
    if (file != NULL)
        buf_append(buf, cap, &off, "%s:%d: ", file, line);

    va_start(ap, fmt);
    buf_vappend(buf, cap, &off, fmt, ap);
    va_end(ap);

    buf[off++] = '\n';
    log_emit(buf, off);
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

/* ===== 2. Robust I/O ==================================================== */

ssize_t read_n(int fd, void *buf, size_t n)
{
    unsigned char *p = buf;
    size_t done = 0;

    while (done < n) {
        ssize_t r = read(fd, p + done, n - done);

        if (r > 0) {
            done += (size_t)r;
        } else if (r == 0) {
            break;                      /* EOF: peer closed */
        } else if (errno == EINTR) {
            LOG_DEBUG(DBG_VERBOSE, "read_n: interrupted by a signal (EINTR), retrying");
        } else {
            LOG_DEBUG(DBG_VERBOSE, "read_n: fd=%d failed after %zu/%zu bytes: %s",
                      fd, done, n, strerror(errno));
            return -1;
        }
    }
    return (ssize_t)done;
}

ssize_t write_n(int fd, const void *buf, size_t n)
{
    const unsigned char *p = buf;
    size_t done = 0;

    while (done < n) {
        ssize_t w = write(fd, p + done, n - done);

        if (w >= 0) {
            done += (size_t)w;
        } else if (errno == EINTR) {
            LOG_DEBUG(DBG_VERBOSE, "write_n: interrupted by a signal (EINTR), retrying");
        } else {
            LOG_DEBUG(DBG_VERBOSE, "write_n: fd=%d failed after %zu/%zu bytes: %s",
                      fd, done, n, strerror(errno));
            return -1;
        }
    }
    return (ssize_t)done;
}

uint64_t mono_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/*
 * SO_RCVTIMEO restarts with every read(), so a peer that sends one byte just
 * before each timeout keeps a reader waiting forever. Here poll() only ever
 * gets the time left until one fixed deadline.
 */
ssize_t read_n_until(int fd, void *buf, size_t n, uint64_t deadline_ms)
{
    unsigned char *p = buf;
    size_t done = 0;

    if (deadline_ms == 0)
        return read_n(fd, buf, n);
    while (done < n) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        uint64_t now = mono_ms();
        uint64_t left;
        ssize_t r;
        int pr;

        if (now >= deadline_ms) {
            LOG_DEBUG(DBG_VERBOSE, "read_n_until: fd=%d deadline passed after %zu/%zu bytes",
                      fd, done, n);
            errno = ETIMEDOUT;
            return -1;
        }
        left = deadline_ms - now;
        pr = poll(&pfd, 1, left > INT_MAX ? INT_MAX : (int)left);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (pr == 0)
            continue;                   /* the loop re-checks the deadline */
        r = read(fd, p + done, n - done);
        if (r > 0) {
            done += (size_t)r;
        } else if (r == 0) {
            break;                      /* EOF: peer closed */
        } else if (errno != EINTR) {
            LOG_DEBUG(DBG_VERBOSE, "read_n_until: fd=%d failed after %zu/%zu bytes: %s",
                      fd, done, n, strerror(errno));
            return -1;
        }
    }
    return (ssize_t)done;
}

int set_socket_timeout(int fd, int seconds)
{
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
        return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0)
        return -1;
    return 0;
}

/* ===== 3. Protocol helpers ============================================== */

/* Map the errno of a failed read_n/write_n to a proto_status. */
static int io_status(void)
{
    if (errno == ETIMEDOUT)
        return PROTO_DEADLINE;
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? PROTO_TIMEOUT : PROTO_IO_ERR;
}

static int recv_exact(int fd, void *buf, size_t n, uint64_t deadline_ms)
{
    ssize_t r = read_n_until(fd, buf, n, deadline_ms);

    if (r < 0)
        return io_status();
    if ((size_t)r < n) {
        LOG_DEBUG(DBG_VERBOSE, "peer closed after %zd/%zu bytes", r, n);
        return PROTO_EOF;
    }
    return PROTO_OK;
}

int proto_send(int fd, uint8_t opcode, const void *body, uint32_t body_len)
{
    unsigned char pkt[PROTO_HEADER_SIZE + PROTO_MAX_BODY];
    size_t total = PROTO_HEADER_SIZE + (size_t)body_len;
    proto_header_t hdr;

    if (body_len > PROTO_MAX_BODY || (body_len > 0 && body == NULL))
        return PROTO_BAD_LENGTH;

    hdr.magic    = htons(PROTO_MAGIC);
    hdr.version  = PROTO_VERSION;
    hdr.opcode   = opcode;
    hdr.body_len = htonl(body_len);
    memcpy(pkt, &hdr, PROTO_HEADER_SIZE);
    if (body_len > 0)
        memcpy(pkt + PROTO_HEADER_SIZE, body, body_len);

    LOG_DEBUG(DBG_PACKET, "TX %s(0x%02X) body_len=%u",
              proto_opcode_name(opcode), opcode, body_len);
    LOG_HEXDUMP("TX packet", pkt, total);

    /* Header and body leave in one write(), normally one TCP segment. */
    if (write_n(fd, pkt, total) < 0)
        return io_status();
    return PROTO_OK;
}

int proto_send_error(int fd, uint32_t code, const char *msg)
{
    proto_error_t err;

    memset(&err, 0, sizeof(err));
    err.code = htonl(code);
    snprintf(err.message, sizeof(err.message), "%s",
             msg != NULL ? msg : proto_errcode_str(code));
    return proto_send(fd, OP_ERROR, &err, sizeof(err));
}

int proto_recv_header(int fd, proto_header_t *hdr, int validate)
{
    return proto_recv_header_budget(fd, hdr, validate, 0, NULL);
}

int proto_recv_header_budget(int fd, proto_header_t *hdr, int validate,
                             unsigned budget_ms, uint64_t *deadline_ms)
{
    unsigned char raw[PROTO_HEADER_SIZE];
    uint64_t deadline = 0;
    int st;

    if (budget_ms == 0) {
        st = recv_exact(fd, raw, sizeof(raw), 0);
    } else {
        /* An idle wait for the next request is SO_RCVTIMEO's job; the
         * deadline starts with the request's first byte. */
        st = recv_exact(fd, raw, 1, 0);
        if (st == PROTO_OK) {
            deadline = mono_ms() + budget_ms;
            st = recv_exact(fd, raw + 1, sizeof(raw) - 1, deadline);
        }
    }
    if (deadline_ms != NULL)
        *deadline_ms = deadline;
    if (st != PROTO_OK)
        return st;

    memcpy(hdr, raw, sizeof(raw));
    hdr->magic    = ntohs(hdr->magic);
    hdr->body_len = ntohl(hdr->body_len);

    LOG_DEBUG(DBG_PACKET, "RX header magic=0x%04X ver=%u op=%s(0x%02X) body_len=%u",
              hdr->magic, hdr->version, proto_opcode_name(hdr->opcode),
              hdr->opcode, hdr->body_len);
    LOG_HEXDUMP("RX header", raw, sizeof(raw));

    if (!validate)
        return PROTO_OK;
    if (hdr->magic != PROTO_MAGIC)
        return PROTO_BAD_MAGIC;
    if (hdr->version != PROTO_VERSION)
        return PROTO_BAD_VERSION;
    if (hdr->body_len > PROTO_MAX_BODY)
        return PROTO_BAD_LENGTH;
    return PROTO_OK;
}

int proto_recv_body(int fd, void *buf, uint32_t len)
{
    return proto_recv_body_until(fd, buf, len, 0);
}

int proto_recv_body_until(int fd, void *buf, uint32_t len, uint64_t deadline_ms)
{
    int st;

    if (len == 0)
        return PROTO_OK;
    st = recv_exact(fd, buf, len, deadline_ms);
    if (st == PROTO_OK)
        LOG_HEXDUMP("RX body", buf, len);
    return st;
}

const char *proto_status_str(int status)
{
    switch (status) {
    case PROTO_OK:          return "ok";
    case PROTO_EOF:         return "peer closed the connection";
    case PROTO_TIMEOUT:     return "socket timeout";
    case PROTO_IO_ERR:      return "I/O error";
    case PROTO_BAD_MAGIC:   return "bad magic number";
    case PROTO_BAD_VERSION: return "unsupported protocol version";
    case PROTO_BAD_LENGTH:  return "body length exceeds limit";
    case PROTO_DEADLINE:    return "request deadline exceeded";
    default:                return "unknown status";
    }
}

const char *proto_opcode_name(uint8_t opcode)
{
    switch (opcode) {
    case OP_SYSINFO_REQ:    return "SYSINFO_REQ";
    case OP_SUBSCRIBE_REQ:  return "SUBSCRIBE_REQ";
    case OP_DETAIL_REQ:     return "DETAIL_REQ";
    case OP_ACK:            return "ACK";
    case OP_SYSINFO_RESP:   return "SYSINFO_RESP";
    case OP_DETAIL_RESP:    return "DETAIL_RESP";
    case OP_ERROR:          return "ERROR";
    default:                return "UNKNOWN";
    }
}

const char *proto_errcode_str(uint32_t code)
{
    switch (code) {
    case ERR_BAD_MAGIC:     return "bad magic number";
    case ERR_BAD_VERSION:   return "unsupported protocol version";
    case ERR_BAD_LENGTH:    return "body length exceeds limit";
    case ERR_UNKNOWN_OP:    return "unknown opcode";
    case ERR_INTERNAL:      return "internal server error";
    case ERR_BUSY:          return "server busy, try again later";
    case ERR_HEARTBEAT:     return "heartbeat lost: no ACK from the client";
    case ERR_DEADLINE:      return "request deadline exceeded";
    default:                return "unknown error";
    }
}

/* ===== 4. System information ============================================ */

/* Parse MemTotal, MemFree and MemAvailable (kB) from /proc/meminfo. */
static int read_meminfo(uint64_t *total, uint64_t *free_kb, uint64_t *avail)
{
    FILE *fp = fopen("/proc/meminfo", "r");
    char line[128];
    unsigned found = 0;

    if (fp == NULL)
        return -1;
    while (found != 7 && fgets(line, sizeof(line), fp) != NULL) {
        unsigned long long kb;

        if (sscanf(line, "MemTotal: %llu kB", &kb) == 1) {
            *total = kb;
            found |= 1;
        } else if (sscanf(line, "MemFree: %llu kB", &kb) == 1) {
            *free_kb = kb;
            found |= 2;
        } else if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) {
            *avail = kb;
            found |= 4;
        }
    }
    fclose(fp);
    return found == 7 ? 0 : -1;
}

int sysinfo_collect(proto_sysinfo_t *out)
{
    struct sysinfo si;
    double load[3];
    uint64_t total = 0, free_kb = 0, avail = 0;

    memset(out, 0, sizeof(*out));
    out->server_pid = (uint32_t)getpid();

    if (gethostname(out->hostname, sizeof(out->hostname)) < 0)
        snprintf(out->hostname, sizeof(out->hostname), "unknown");
    out->hostname[sizeof(out->hostname) - 1] = '\0';

    if (sysinfo(&si) < 0) {
        LOG_ERROR("sysinfo() failed: %s", strerror(errno));
        return -1;
    }
    out->uptime_sec = (uint64_t)si.uptime;
    out->nprocs     = si.procs;

    if (getloadavg(load, 3) == 3) {
        for (int i = 0; i < 3; i++)
            out->load_x100[i] = (uint32_t)(load[i] * 100.0 + 0.5);
    }

    if (read_meminfo(&total, &free_kb, &avail) == 0) {
        out->mem_total_kb = total;
        out->mem_free_kb  = free_kb;
        out->mem_avail_kb = avail;
    } else {
        /* Kernels before 3.14 lack MemAvailable: approximate as free + buffers. */
        uint64_t unit = si.mem_unit ? si.mem_unit : 1;

        out->mem_total_kb = (uint64_t)si.totalram * unit / 1024;
        out->mem_free_kb  = (uint64_t)si.freeram * unit / 1024;
        out->mem_avail_kb = ((uint64_t)si.freeram + si.bufferram) * unit / 1024;
    }

    LOG_DEBUG(DBG_VERBOSE, "sysinfo: host=%s uptime=%llus load1=%u.%02u mem_avail=%llu/%llu kB",
              out->hostname, (unsigned long long)out->uptime_sec,
              out->load_x100[0] / 100, out->load_x100[0] % 100,
              (unsigned long long)out->mem_avail_kb,
              (unsigned long long)out->mem_total_kb);
    return 0;
}

void sysinfo_hton(proto_sysinfo_t *s)
{
    s->server_pid   = htonl(s->server_pid);
    s->seq          = htonl(s->seq);
    s->uptime_sec   = htobe64(s->uptime_sec);
    s->mem_total_kb = htobe64(s->mem_total_kb);
    s->mem_free_kb  = htobe64(s->mem_free_kb);
    s->mem_avail_kb = htobe64(s->mem_avail_kb);
    for (int i = 0; i < 3; i++)
        s->load_x100[i] = htonl(s->load_x100[i]);
    s->nprocs       = htonl(s->nprocs);
}

void sysinfo_ntoh(proto_sysinfo_t *s)
{
    s->server_pid   = ntohl(s->server_pid);
    s->seq          = ntohl(s->seq);
    s->uptime_sec   = be64toh(s->uptime_sec);
    s->mem_total_kb = be64toh(s->mem_total_kb);
    s->mem_free_kb  = be64toh(s->mem_free_kb);
    s->mem_avail_kb = be64toh(s->mem_avail_kb);
    for (int i = 0; i < 3; i++)
        s->load_x100[i] = ntohl(s->load_x100[i]);
    s->nprocs       = ntohl(s->nprocs);
}

/* Copy src into dst[len], truncating if needed; always NUL-terminated. */
static void copy_str(char *dst, size_t len, const char *src)
{
    size_t n = strnlen(src, len - 1);

    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Total and idle (idle + iowait) jiffies from the "cpu" line of /proc/stat. */
static int read_cpu_times(uint64_t *total, uint64_t *idle)
{
    unsigned long long v[8] = { 0 };
    FILE *fp = fopen("/proc/stat", "r");
    int n;

    if (fp == NULL)
        return -1;
    n = fscanf(fp, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    fclose(fp);
    if (n < 4)
        return -1;
    *total = 0;
    for (int i = 0; i < 8; i++)
        *total += v[i];
    *idle = v[3] + v[4];
    return 0;
}

static void read_cpu_model(char *buf, size_t len)
{
    FILE *fp = fopen("/proc/cpuinfo", "r");
    char line[256];

    copy_str(buf, len, "unknown");
    if (fp == NULL)
        return;
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *v = strchr(line, ':');

        if (strncmp(line, "model name", 10) != 0 || v == NULL)
            continue;
        v++;
        v += strspn(v, " \t");
        v[strcspn(v, "\n")] = '\0';
        copy_str(buf, len, v);
        break;
    }
    fclose(fp);
}

int sysdetail_collect(proto_detail_t *out, unsigned sample_ms)
{
    struct utsname un;
    struct statvfs vfs;
    uint64_t t0, i0, t1, i1;
    long ncpu;

    memset(out, 0, sizeof(*out));
    out->server_pid = (uint32_t)getpid();
    if (uname(&un) < 0) {
        LOG_ERROR("uname() failed: %s", strerror(errno));
        return -1;
    }
    copy_str(out->sysname, sizeof(out->sysname), un.sysname);
    copy_str(out->release, sizeof(out->release), un.release);
    copy_str(out->machine, sizeof(out->machine), un.machine);
    read_cpu_model(out->cpu_model, sizeof(out->cpu_model));

    ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    out->ncpu = ncpu > 0 ? (uint32_t)ncpu : 0;

    if (statvfs("/", &vfs) == 0) {
        out->disk_total_kb = (uint64_t)vfs.f_blocks * vfs.f_frsize / 1024;
        out->disk_avail_kb = (uint64_t)vfs.f_bavail * vfs.f_frsize / 1024;
    }

    out->sample_ms = sample_ms;
    if (read_cpu_times(&t0, &i0) == 0) {
        sleep_ms(sample_ms);
        if (read_cpu_times(&t1, &i1) == 0 && t1 > t0) {
            uint64_t dt = t1 - t0;
            uint64_t di = i1 - i0 < dt ? i1 - i0 : dt;

            out->cpu_busy_x100 = (uint32_t)((dt - di) * 10000u / dt);
        }
    }
    LOG_DEBUG(DBG_VERBOSE, "sysdetail: %s %s %s, %u cpu(s) busy %u.%02u %%",
              out->sysname, out->release, out->machine, out->ncpu,
              out->cpu_busy_x100 / 100, out->cpu_busy_x100 % 100);
    return 0;
}

void sysdetail_hton(proto_detail_t *d)
{
    d->server_pid    = htonl(d->server_pid);
    d->ncpu          = htonl(d->ncpu);
    d->cpu_busy_x100 = htonl(d->cpu_busy_x100);
    d->sample_ms     = htonl(d->sample_ms);
    d->disk_total_kb = htobe64(d->disk_total_kb);
    d->disk_avail_kb = htobe64(d->disk_avail_kb);
}

void sysdetail_ntoh(proto_detail_t *d)
{
    d->server_pid    = ntohl(d->server_pid);
    d->ncpu          = ntohl(d->ncpu);
    d->cpu_busy_x100 = ntohl(d->cpu_busy_x100);
    d->sample_ms     = ntohl(d->sample_ms);
    d->disk_total_kb = be64toh(d->disk_total_kb);
    d->disk_avail_kb = be64toh(d->disk_avail_kb);
}

/* ===== 5. Misc ========================================================== */

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

int set_signal(int sig, void (*disposition)(int))
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = disposition;
    return sigaction(sig, &sa, NULL);
}
