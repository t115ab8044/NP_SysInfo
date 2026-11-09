/*
 * testlib.c - helpers shared by the C test programs in tests/.
 */
#define _GNU_SOURCE
#include "testlib.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RULE "=============================================================================="
#define MAX_ARGS 32

int tl_fails;
int tl_port;
char tl_port_str[16];
pid_t tl_server_pid;
const char *tl_server_log;
int tl_last_status;

static pid_t g_tee_pid;

void tl_die(const char *fmt, ...)
{
    va_list ap;

    fflush(stdout);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

char *tl_fmt(const char *fmt, ...)
{
    va_list ap;
    char *s;
    int n;

    va_start(ap, fmt);
    n = vasprintf(&s, fmt, ap);
    va_end(ap);
    if (n < 0)
        tl_die("out of memory");
    return s;
}

int tl_env_int(const char *name, int fallback)
{
    const char *value = getenv(name);

    return value && *value ? (int)strtol(value, NULL, 10) : fallback;
}

void tl_set_port(int port)
{
    tl_port = port;
    snprintf(tl_port_str, sizeof(tl_port_str), "%d", port);
}

const char *tl_self_path(void)
{
    static char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);

    if (n <= 0)
        tl_die("cannot read /proc/self/exe");
    exe[n] = '\0';
    return exe;
}

void tl_init(int default_port)
{
    char root[PATH_MAX];
    int i;

    snprintf(root, sizeof(root), "%s", tl_self_path());
    for (i = 0; i < 2; i++) {               /* strip "/tests/<program>" */
        char *slash = strrchr(root, '/');

        if (slash)
            *slash = '\0';
    }
    if (chdir(root) != 0)
        tl_die("cannot enter %s: %s", root, strerror(errno));
    setvbuf(stdout, NULL, _IOLBF, 0);
    tl_set_port(tl_env_int("PORT", default_port));
}

static void write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);

        if (n <= 0)
            return;
        buf += n;
        len -= (size_t)n;
    }
}

void tl_tee(const char *path)
{
    char buf[4096];
    int fds[2];
    int file;
    ssize_t n;

    mkdir("logs", 0755);
    file = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (file < 0 || pipe(fds) != 0)
        tl_die("cannot open %s: %s", path, strerror(errno));
    fflush(stdout);
    fflush(stderr);
    g_tee_pid = fork();
    if (g_tee_pid < 0)
        tl_die("fork failed: %s", strerror(errno));
    if (g_tee_pid == 0) {
        close(fds[1]);
        while ((n = read(fds[0], buf, sizeof(buf))) > 0) {
            write_all(STDOUT_FILENO, buf, (size_t)n);
            write_all(file, buf, (size_t)n);
        }
        _exit(0);
    }
    close(file);
    close(fds[0]);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
}

int tl_finish(int rc)
{
    fflush(stdout);
    fflush(stderr);
    if (g_tee_pid > 0) {
        int null_fd = open("/dev/null", O_WRONLY);

        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
        close(null_fd);
        waitpid(g_tee_pid, NULL, 0);
    }
    return rc;
}

void tl_heading(const char *fmt, ...)
{
    va_list ap;

    fputs("\n==== ", stdout);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    puts(" ====");
}

void tl_section(const char *fmt, ...)
{
    va_list ap;

    fputs("\n--- ", stdout);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    puts(" ---");
}

void tl_banner(const char *fmt, ...)
{
    va_list ap;

    printf("\n%s\n ", RULE);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n%s\n", RULE);
}

void tl_expect(int ok, const char *fmt, ...)
{
    va_list ap;

    fputs(ok ? "  [PASS] " : "  [FAIL] ", stdout);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    if (!ok)
        tl_fails++;
}

int tl_print_result(void)
{
    if (tl_fails == 0) {
        puts("ALL CHECKS PASSED");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", tl_fails);
    return 1;
}

pid_t tl_spawn_opts(const char *dir, const char *out, int append, const char *const argv[])
{
    pid_t pid;

    fflush(stdout);
    fflush(stderr);
    pid = fork();
    if (pid < 0)
        tl_die("fork failed: %s", strerror(errno));
    if (pid == 0) {
        /* Nothing outlives a test program that is killed. */
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (out) {
            int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
            int fd = open(out, flags, 0644);

            if (fd < 0)
                _exit(126);
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        if (dir && chdir(dir) != 0)
            _exit(126);
        execvp(argv[0], (char *const *)argv);
        fprintf(stderr, "%s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    return pid;
}

pid_t tl_spawn(const char *out, const char *const argv[])
{
    return tl_spawn_opts(NULL, out, 0, argv);
}

int tl_wait(pid_t pid)
{
    int status;

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 128 + WTERMSIG(status);
}

int tl_run(const char *out, const char *const argv[])
{
    return tl_wait(tl_spawn(out, argv));
}

/* True until the child has exited; the child stays waitable afterwards. */
int tl_running(pid_t pid)
{
    siginfo_t info;

    info.si_pid = 0;
    if (waitid(P_PID, (id_t)pid, &info, WEXITED | WNOHANG | WNOWAIT) != 0)
        return 0;
    return info.si_pid == 0;
}

int tl_which(const char *name)
{
    const char *path = getenv("PATH");
    char *copy;
    char *dir;
    char *save = NULL;

    if (!path)
        return 0;
    copy = strdup(path);
    for (dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        if (access(tl_fmt("%s/%s", dir, name), X_OK) == 0)
            return 1;
    }
    return 0;
}

void tl_sleep(double seconds)
{
    struct timespec ts;

    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

long long tl_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static char *capture(const char *const argv[], int with_stderr)
{
    char *text = NULL;
    size_t size = 0;
    FILE *mem;
    char buf[4096];
    int fds[2];
    ssize_t n;
    pid_t pid;

    if (pipe(fds) != 0)
        tl_die("pipe failed: %s", strerror(errno));
    fflush(stdout);
    fflush(stderr);
    pid = fork();
    if (pid < 0)
        tl_die("fork failed: %s", strerror(errno));
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        if (with_stderr) {
            dup2(fds[1], STDERR_FILENO);
        } else {
            int null_fd = open("/dev/null", O_WRONLY);

            dup2(null_fd, STDERR_FILENO);
            close(null_fd);
        }
        close(fds[1]);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(fds[1]);
    mem = open_memstream(&text, &size);
    while ((n = read(fds[0], buf, sizeof(buf))) > 0)
        fwrite(buf, 1, (size_t)n, mem);
    fclose(mem);
    close(fds[0]);
    tl_last_status = tl_wait(pid);
    return text;
}

char *tl_capture(const char *const argv[])
{
    return capture(argv, 1);
}

char *tl_capture_out(const char *const argv[])
{
    return capture(argv, 0);
}

int tl_start_server(const char *dir, const char *log, const char *const flags[])
{
    const char *argv[MAX_ARGS];
    int n = 0;
    int i;

    argv[n++] = "./server";
    argv[n++] = "-p";
    argv[n++] = tl_port_str;
    for (i = 0; flags[i] && n < MAX_ARGS - 1; i++)
        argv[n++] = flags[i];
    argv[n] = NULL;
    tl_server_log = log;
    tl_server_pid = tl_spawn_opts(dir, log, 0, argv);
    for (i = 0; i < 100; i++) {
        if (tl_port_in_use(tl_port))
            return 0;
        tl_sleep(0.05);
    }
    puts("server did not start:");
    tl_print_file(log);
    return -1;
}

void tl_stop_server(void)
{
    if (tl_server_pid > 0) {
        kill(tl_server_pid, SIGTERM);
        tl_wait(tl_server_pid);
    }
    tl_server_pid = 0;
}

static int tcp_count(int port, unsigned state)
{
    static const char *const files[] = { "/proc/self/net/tcp", "/proc/self/net/tcp6" };
    int total = 0;
    size_t i;

    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        char line[512];
        FILE *f = fopen(files[i], "r");

        if (!f)
            continue;
        while (fgets(line, sizeof(line), f)) {
            unsigned local;
            unsigned st;

            if (sscanf(line, " %*d: %*[0-9A-Fa-f]:%x %*[0-9A-Fa-f]:%*x %x", &local, &st) == 2 &&
                local == (unsigned)port && st == state)
                total++;
        }
        fclose(f);
    }
    return total;
}

int tl_port_in_use(int port)
{
    return tcp_count(port, 0x0A) > 0;
}

int tl_established(int port)
{
    return tcp_count(port, 0x01);
}

int tl_children(pid_t ppid, pid_t *out, int max, int *zombies)
{
    DIR *proc = opendir("/proc");
    struct dirent *entry;
    int count = 0;

    if (zombies)
        *zombies = 0;
    if (!proc)
        return 0;
    while ((entry = readdir(proc))) {
        char buf[1024];
        char *close_paren;
        char state;
        size_t n;
        FILE *f;
        int parent;

        if (!isdigit((unsigned char)entry->d_name[0]))
            continue;
        f = fopen(tl_fmt("/proc/%s/stat", entry->d_name), "r");
        if (!f)
            continue;
        n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        close_paren = strrchr(buf, ')');
        if (!close_paren || sscanf(close_paren + 1, " %c %d", &state, &parent) != 2 ||
            parent != ppid)
            continue;
        if (out && count < max)
            out[count] = (pid_t)strtol(entry->d_name, NULL, 10);
        count++;
        if (zombies && state == 'Z')
            (*zombies)++;
    }
    closedir(proc);
    return count;
}

int tl_child_count(void)
{
    return tl_children(tl_server_pid, NULL, 0, NULL);
}

int tl_zombie_count(void)
{
    int zombies;

    tl_children(tl_server_pid, NULL, 0, &zombies);
    return zombies;
}

char *tl_read(const char *path)
{
    char *text = NULL;
    size_t size = 0;
    FILE *mem = open_memstream(&text, &size);
    char buf[4096];
    size_t n;
    FILE *f = fopen(path, "r");

    if (f) {
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
            fwrite(buf, 1, n, mem);
        fclose(f);
    }
    fclose(mem);
    return text;
}

void tl_write(const char *path, const char *text, int append)
{
    FILE *f = fopen(path, append ? "a" : "w");

    if (!f)
        tl_die("cannot write %s: %s", path, strerror(errno));
    fputs(text, f);
    fclose(f);
}

/* Returns the next line as its own string, or NULL at the end of the text. */
static char *next_line(const char **cursor)
{
    const char *start = *cursor;
    const char *end;
    size_t len;

    if (*start == '\0')
        return NULL;
    end = strchr(start, '\n');
    len = end ? (size_t)(end - start) : strlen(start);
    *cursor = end ? end + 1 : start + len;
    return strndup(start, len);
}

static void compile(regex_t *re, const char *ere, int flags)
{
    if (regcomp(re, ere, REG_EXTENDED | flags) != 0)
        tl_die("bad regular expression: %s", ere);
}

char *tl_grep(const char *text, const char *ere, int invert)
{
    char *result = NULL;
    size_t size = 0;
    FILE *mem = open_memstream(&result, &size);
    const char *cursor = text;
    regex_t re;
    char *line;

    compile(&re, ere, REG_NOSUB);
    while ((line = next_line(&cursor))) {
        if ((regexec(&re, line, 0, NULL, 0) == 0) != (invert != 0))
            fprintf(mem, "%s\n", line);
        free(line);
    }
    regfree(&re);
    fclose(mem);
    return result;
}

int tl_count(const char *text, const char *ere)
{
    return tl_nlines(tl_grep(text, ere, 0));
}

int tl_file_count(const char *path, const char *ere)
{
    return tl_count(tl_read(path), ere);
}

/* The first match in the text, or NULL. */
char *tl_match(const char *text, const char *ere)
{
    regmatch_t m;
    regex_t re;
    char *found = NULL;

    compile(&re, ere, REG_NEWLINE);
    if (regexec(&re, text, 1, &m, 0) == 0)
        found = strndup(text + m.rm_so, (size_t)(m.rm_eo - m.rm_so));
    regfree(&re);
    return found;
}

int tl_nlines(const char *text)
{
    int n = 0;

    for (; *text; text++) {
        if (*text == '\n')
            n++;
    }
    return n;
}

char *tl_head(const char *text, int n)
{
    const char *p = text;

    while (n-- > 0 && *p) {
        p = strchr(p, '\n');
        if (!p)
            return strdup(text);
        p++;
    }
    return strndup(text, (size_t)(p - text));
}

char *tl_tail(const char *text, int n)
{
    int skip = tl_nlines(text) - n;
    const char *p = text;

    while (skip-- > 0 && *p) {
        p = strchr(p, '\n');
        if (!p)
            return strdup("");
        p++;
    }
    return strdup(p);
}

char *tl_strip_cr(const char *text)
{
    char *copy = strdup(text);
    char *out = copy;

    for (; *text; text++) {
        if (*text != '\r')
            *out++ = *text;
    }
    *out = '\0';
    return copy;
}

void tl_print_lines(const char *text, const char *prefix, int cut)
{
    const char *cursor = text;
    char *line;

    while ((line = next_line(&cursor))) {
        if (cut > 0 && (int)strlen(line) > cut)
            line[cut] = '\0';
        printf("%s%s\n", prefix, line);
        free(line);
    }
}

void tl_print_file(const char *path)
{
    fputs(tl_read(path), stdout);
}

void tl_append_prefixed(const char *src, const char *prefix, const char *dst)
{
    const char *cursor = tl_read(src);
    FILE *f = fopen(dst, "a");
    char *line;

    if (!f)
        tl_die("cannot write %s: %s", dst, strerror(errno));
    while ((line = next_line(&cursor))) {
        fprintf(f, "%s%s\n", prefix, line);
        free(line);
    }
    fclose(f);
}

int tl_distinct_pids(const char *text, pid_t *out, int max)
{
    int count = 0;
    const char *p = text;

    while ((p = strstr(p, "pid=")) != NULL) {
        pid_t pid = (pid_t)strtol(p + 4, NULL, 10);
        int i;

        p += 4;
        for (i = 0; i < count && out[i] != pid; i++)
            ;
        if (i == count && count < max)
            out[count++] = pid;
    }
    return count;
}

char *tl_join(const char *const items[])
{
    char *text = NULL;
    size_t size = 0;
    FILE *mem = open_memstream(&text, &size);
    int i;

    for (i = 0; items[i]; i++)
        fprintf(mem, "%s%s", i ? " " : "", items[i]);
    fclose(mem);
    return text;
}

static char *format_now(const char *fmt)
{
    char buf[64];
    time_t now = time(NULL);
    struct tm tm_now;

    localtime_r(&now, &tm_now);
    strftime(buf, sizeof(buf), fmt, &tm_now);
    return strdup(buf);
}

char *tl_date(void)
{
    return format_now("%F %T");
}

char *tl_time_hms(void)
{
    return format_now("%T");
}

char *tl_kernel(void)
{
    struct utsname u;

    if (uname(&u) != 0)
        return strdup("unknown");
    return tl_fmt("%s %s", u.sysname, u.release);
}

char *tl_compiler(void)
{
    char *first = tl_head(tl_capture_out(CMD("gcc", "--version")), 1);

    first[strcspn(first, "\n")] = '\0';
    return first;
}

char *tl_mktemp(const char *prefix)
{
    char *path = tl_fmt("/tmp/%s.XXXXXX", prefix);

    if (!mkdtemp(path))
        tl_die("mkdtemp failed: %s", strerror(errno));
    return path;
}

void tl_rmrf(const char *path)
{
    tl_run("/dev/null", CMD("rm", "-rf", path));
}
