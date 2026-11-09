/*
 * test_audit.c - repeatable code and behaviour audit.
 *
 *   1. static analysis     gcc -fanalyzer, stricter warnings
 *   2. source rules        no unbounded string calls, no threads, no signal()
 *   3. binary hardening    PIE, BIND_NOW, stack canary, RUNPATH=$ORIGIN
 *   4. sustained load      descriptors and memory of the parent stay flat
 *   5. connection flood    what one host can take, and --max-per-ip
 *   6. sanitizers          ASan + UBSan under the concurrency/robustness suites
 *   7. hostile peer        a fake server sends malformed replies to the client
 *
 * Everything is built in a temporary copy, so the working tree is not touched.
 *
 * Usage : make audit        (env: PORT=9290)
 * Output: logs/test_audit.log
 */
#include "testlib.h"

#include <dirent.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BASE_FLAGS "-std=gnu11", "-O2", "-g", "-DENABLE_DEBUG"

static const char *const SRCS[] = { "server.c", "client.c", "utils.c" };
static char *g_work;

static int count_in_sources(const char *ere, int with_headers)
{
    int total = 0;
    size_t i;

    for (i = 0; i < sizeof(SRCS) / sizeof(SRCS[0]); i++)
        total += tl_file_count(SRCS[i], ere);
    if (with_headers)
        total += tl_file_count("utils.h", ere) + tl_file_count("protocol.h", ere);
    return total;
}

static int fd_count(void)
{
    DIR *dir = opendir(tl_fmt("/proc/%d/fd", (int)tl_server_pid));
    struct dirent *entry;
    int n = 0;

    if (!dir)
        return -1;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] != '.')
            n++;
    }
    closedir(dir);
    return n;
}

static long rss_kb(void)
{
    char *line = tl_match(tl_read(tl_fmt("/proc/%d/status", (int)tl_server_pid)), "^VmRSS:[[:space:]]*[0-9]+");

    return line ? strtol(line + strcspn(line, "0123456789"), NULL, 10) : -1;
}

static void audit_static(void)
{
    int total = 0;
    size_t i;
    char *strict;

    tl_heading("1. Static analysis");
    for (i = 0; i < sizeof(SRCS) / sizeof(SRCS[0]); i++) {
        char *out = tl_fmt("analyzer_%s.txt", SRCS[i]);
        int n;

        tl_run(out, CMD("gcc", BASE_FLAGS, "-fanalyzer", "-c", SRCS[i], "-o", "/dev/null"));
        n = tl_file_count(out, "warning:");
        printf("gcc -fanalyzer %s: %d finding(s)\n", SRCS[i], n);
        total += n;
    }
    tl_expect(total == 0, "gcc -fanalyzer reports nothing");

    tl_write("strict.txt", "", 0);
    for (i = 0; i < sizeof(SRCS) / sizeof(SRCS[0]); i++) {
        tl_wait(tl_spawn_opts(NULL, "strict.txt", 1,
                              CMD("gcc", BASE_FLAGS, "-Wall", "-Wextra", "-pedantic", "-Wformat=2", "-Wshadow",
                                  "-Wnull-dereference", "-Wwrite-strings", "-D_FORTIFY_SOURCE=2",
                                  "-c", SRCS[i], "-o", "/dev/null")));
    }
    strict = tl_grep(tl_read("strict.txt"), "warning:", 0);
    tl_expect(tl_nlines(strict) == 0, "stricter warning set reports nothing");
    fputs(tl_head(strict, 10), stdout);
}

static void audit_source(void)
{
    tl_heading("2. Source rules");
    tl_expect(count_in_sources("\\b(strcpy|strcat|sprintf|vsprintf|gets|system|popen|atoi)\\(", 0) == 0,
              "no unbounded string or shell calls");
    tl_expect(count_in_sources("pthread_|thrd_|\\bclone\\(", 1) == 0, "no thread API (fork only)");
    tl_expect(count_in_sources("(^|[^_[:alnum:]])signal\\(", 0) == 0, "no legacy signal(), sigaction only");
}

static int audit_binary(void)
{
    char *header, *dynamic, *undefined, *type, *runpath;
    char cwd[PATH_MAX];
    int rc;

    tl_heading("3. Binary hardening");
    if (tl_run(NULL, CMD("make", "-s", "all")) != 0)
        return 1;
    header = tl_capture_out(CMD("readelf", "-h", "server"));
    dynamic = tl_capture_out(CMD("readelf", "-d", "server"));
    undefined = tl_capture_out(CMD("nm", "-u", "server"));
    type = tl_match(header, "Type:[[:space:]]+[A-Z]+");
    type = type ? type + strcspn(type, " \t") + strspn(type + strcspn(type, " \t"), " \t") : strdup("");
    runpath = tl_match(dynamic, "runpath: .*");
    runpath = runpath ? runpath + strlen("runpath: ") : strdup("");
    printf("ELF type : %s\n", type);
    printf("RUNPATH  : %s\n", runpath);
    tl_expect(strcmp(type, "DYN") == 0, "position independent executable");
    tl_expect(tl_count(dynamic, "BIND_NOW|FLAGS.*NOW") > 0, "full RELRO (BIND_NOW)");
    tl_expect(strstr(undefined, "__stack_chk_fail") != NULL, "stack protector present");
    tl_expect(strstr(dynamic, "runpath: [$ORIGIN]") != NULL, "library resolved through $ORIGIN, not the current directory");
    if (!getcwd(cwd, sizeof(cwd)))
        return 1;
    rc = tl_wait(tl_spawn_opts("/", "/dev/null", 0, CMD(tl_fmt("%s/server", cwd), "--help")));
    tl_expect(rc != 127, "binary starts from another directory");
    return 0;
}

static int audit_load(void)
{
    long rss0, rss1;
    int fd0, fd1, failed = 0, r;

    tl_heading("4. Sustained load: 1000 connections, 10000 requests");
    if (tl_start_server(NULL, "load.log", NO_FLAGS) != 0)
        return 1;
    fd0 = fd_count();
    rss0 = rss_kb();
    for (r = 1; r <= 10; r++) {
        if (tl_run(tl_fmt("stress_%d.txt", r), CMD("./client", "-p", tl_port_str, "--stress", "100", "-n", "10")) != 0)
            failed++;
    }
    tl_sleep(0.5);
    fd1 = fd_count();
    rss1 = rss_kb();
    fputs(tl_head(tl_tail(tl_read("stress_10.txt"), 4), 3), stdout);
    printf("parent descriptors: %d -> %d\n", fd0, fd1);
    printf("parent RSS (kB)   : %ld -> %ld\n", rss0, rss1);
    tl_expect(failed == 0, "every round succeeded");
    tl_expect(fd1 == fd0, "no descriptor leak in the parent");
    tl_expect(rss1 - rss0 < 256, "parent memory stays flat (under 256 kB growth)");
    tl_expect(tl_child_count() == 0, "no child left behind");
    tl_stop_server();
    fputs(tl_tail(tl_read("load.log"), 1), stdout);
    return 0;
}

/* Opens n connections that stay up for hold seconds; reports what the next client sees. */
static void flood(int n, const char *hold, int *children, char **next)
{
    pid_t *pids = malloc(sizeof(pid_t) * (size_t)n);
    char *line;
    int i;

    for (i = 0; i < n; i++)
        pids[i] = tl_spawn("/dev/null", CMD("./client", "-p", tl_port_str, "-q", "--hold", hold));
    tl_sleep(1.5);
    *children = tl_child_count();
    line = tl_tail(tl_capture(CMD("./client", "-p", tl_port_str, "-q")), 1);
    line[strcspn(line, "\n")] = '\0';
    *next = line;
    for (i = 0; i < n; i++)
        tl_wait(pids[i]);
    free(pids);
}

static const char *after_error(const char *line)
{
    const char *found = line;
    const char *p = line;

    while ((p = strstr(p, "ERROR  ")) != NULL) {
        p += strlen("ERROR  ");
        found = p;
    }
    return found;
}

static int audit_flood(void)
{
    char *next;
    int children;

    tl_heading("5. Connection flood from one host");
    if (tl_start_server(NULL, "flood.log", NO_FLAGS) != 0)
        return 1;
    flood(130, "3", &children, &next);
    printf("default limits : %d children; next client: %s\n", children, after_error(next));
    tl_expect(children == 128, "total limit holds at 128");
    tl_expect(strstr(next, "server busy") != NULL, "FINDING: one host can use every slot by default");
    tl_stop_server();

    if (tl_start_server(NULL, "flood_ip.log", CMD("--max-per-ip", "20")) != 0)
        return 1;
    flood(30, "3", &children, &next);
    printf("--max-per-ip 20: %d children; next client: %s\n", children, after_error(next));
    tl_expect(children == 20, "--max-per-ip caps that host at 20");
    tl_stop_server();
    return 0;
}

static int report_count(void)
{
    DIR *dir = opendir(".");
    struct dirent *entry;
    int n = 0;

    if (!dir)
        return -1;
    while ((entry = readdir(dir))) {
        if (strncmp(entry->d_name, "asan.", 5) == 0 || strncmp(entry->d_name, "ubsan.", 6) == 0)
            n++;
    }
    closedir(dir);
    return n;
}

static int audit_sanitizers(void)
{
    static const char *const suites[] = { "test_concurrent", "test_robustness" };
    size_t i;

    tl_heading("6. ASan + UBSan under the test suites");
    tl_run("/dev/null", CMD("make", "clean-build"));
    if (tl_run("sanitize_build.txt", CMD("make", "CC=gcc -fsanitize=address,undefined -fno-omit-frame-pointer")) != 0) {
        tl_print_file("sanitize_build.txt");
        return 1;
    }
    if (tl_run("/dev/null", CMD("make", "-s", "test-programs")) != 0)
        return 1;
    setenv("ASAN_OPTIONS", tl_fmt("log_path=%s/asan:allocator_may_return_null=1:detect_leaks=1", g_work), 1);
    setenv("UBSAN_OPTIONS", tl_fmt("log_path=%s/ubsan:print_stacktrace=1", g_work), 1);
    for (i = 0; i < sizeof(suites) / sizeof(suites[0]); i++) {
        char *out = tl_fmt("%s.out", suites[i]);
        int rc = tl_run(out, CMD(tl_fmt("./tests/%s", suites[i])));

        printf("%s: PASS=%d FAIL=%d\n", suites[i], tl_file_count(out, "\\[PASS\\]"), tl_file_count(out, "\\[FAIL\\]"));
        tl_expect(rc == 0, "%s passes on the sanitized build", suites[i]);
    }
    tl_expect(report_count() == 0, "no memory or undefined-behaviour report");
    return 0;
}

/* A fake server answers with filler bytes; extra is one more client flag or NULL. */
static void hostile(const char *opcode, const char *len, const char *extra)
{
    pid_t fake = tl_spawn(NULL, CMD("./tests/hostile_server", tl_port_str, opcode, len));
    char *last;
    int rc;

    tl_sleep(0.5);
    rc = tl_run("hostile.txt", extra ? CMD("timeout", "10", "./client", "-p", tl_port_str, extra)
                                     : CMD("timeout", "10", "./client", "-p", tl_port_str));
    tl_wait(fake);
    last = tl_tail(tl_read("hostile.txt"), 1);
    last[strcspn(last, "\n")] = '\0';
    if (strlen(last) > 80)
        last[80] = '\0';
    printf("reply op=%s len=%s: client exit %d | %s\n", opcode, len, rc, last);
    tl_expect(rc < 124, "client survives op=%s len=%s", opcode, len);
}

static int audit_hostile(void)
{
    tl_heading("7. Hostile server against the sanitized client");
    if (tl_run(NULL, CMD("gcc", "-Wall", "-Wextra", "-pedantic", "-std=gnu11", "-O2",
                         "-o", "tests/hostile_server", "tests/hostile_server.c")) != 0)
        return 1;
    hostile("0x81", "120", NULL);                   /* strings with no terminating NUL */
    hostile("0x83", "224", "--detail");
    hostile("0x81", "5000", NULL);                  /* body over the protocol limit */
    hostile("0xFF", "64", NULL);                    /* error message with no terminating NUL */
    tl_expect(report_count() == 0, "no memory or undefined-behaviour report");
    return 0;
}

static int copy_tree(void)
{
    static const char *const root_files[] = { "server.c", "client.c", "utils.c", "utils.h", "protocol.h", "Makefile" };
    static const char *const test_files[] = {
        "tests/testlib.c", "tests/testlib.h", "tests/demo.c", "tests/test_concurrent.c", "tests/test_robustness.c",
        "tests/test_debug.c", "tests/test_capture.c", "tests/test_real.c", "tests/test_audit.c", "tests/hostile_server.c"
    };
    size_t i;

    if (tl_run(NULL, CMD("mkdir", "-p", tl_fmt("%s/logs", g_work), tl_fmt("%s/tests", g_work))) != 0)
        return -1;
    for (i = 0; i < sizeof(root_files) / sizeof(root_files[0]); i++) {
        if (tl_run(NULL, CMD("cp", root_files[i], g_work)) != 0)
            return -1;
    }
    for (i = 0; i < sizeof(test_files) / sizeof(test_files[0]); i++) {
        if (tl_run(NULL, CMD("cp", test_files[i], tl_fmt("%s/tests/", g_work))) != 0)
            return -1;
    }
    return 0;
}

static int run(void)
{
    tl_heading("Environment");
    printf("date     : %s\n", tl_date());
    printf("compiler : %s\n", tl_compiler());
    if (tl_port_in_use(tl_port)) {
        printf("port %d is already in use; set PORT=...\n", tl_port);
        return 1;
    }

    g_work = tl_mktemp("np_audit");
    if (copy_tree() != 0 || chdir(g_work) != 0)
        return 1;

    audit_static();
    audit_source();
    if (audit_binary() != 0)
        tl_fails++;
    if (audit_load() != 0)
        tl_fails++;
    if (audit_flood() != 0)
        tl_fails++;
    if (audit_sanitizers() != 0)
        tl_fails++;
    if (audit_hostile() != 0)
        tl_fails++;

    tl_heading("Result");
    return tl_print_result();
}

int main(void)
{
    int rc;

    tl_init(9290);
    tl_tee("logs/test_audit.log");
    rc = run();
    tl_stop_server();
    if (g_work) {
        if (chdir("/") == 0)
            tl_rmrf(g_work);
    }
    return tl_finish(rc);
}
