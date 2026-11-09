/*
 * test_debug.c - proof of the two-level debug control.
 *
 *   Compile time  make DEBUG=0 (no -DENABLE_DEBUG)  vs  make DEBUG=1
 *                 LOG_DEBUG / LOG_HEXDUMP expand to ((void)0): the release
 *                 binaries contain no debug format strings and no logger
 *                 calls, and ./server -d 2 prints no DEBUG line at all.
 *   Run time      debug build with ./server -d 0 / -d 1 / -d 2
 *                 0 = INFO/WARN/ERROR only, 1 = + DEBUG lines with file:line,
 *                 2 = + packet trace (decoded headers and hex dumps)
 *
 * Usage : make test-debug        (env: PORT=9090)
 * Output: logs/test_debug.log, logs/debug_*.log
 * Leaves the default debug build (make DEBUG=1) in the working tree.
 */
#include "testlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Format strings that exist only inside LOG_DEBUG / LOG_HEXDUMP calls */
#define DEBUG_ONLY_RE "child started|push #%u sent|RX header magic=|TX %s\\(0x|read_n: interrupted"
/* Lines that only -d 2 produces: decoded headers and hex dump rows */
#define PACKET_RE "RX header magic=|TX [A-Z_]+\\(0x|(TX|RX) (packet|header|body): |DEBUG +[0-9a-f]{4}  "

static char *g_tmp;

static int start_server(const char *dir, const char *log, const char *const flags[])
{
    printf("$ ./server -p %d %s      (%s build)\n", tl_port, tl_join(flags), strrchr(dir, '/') + 1);
    return tl_start_server(dir, log, flags);
}

static void snapshot_build(const char *name)
{
    char *dir = tl_fmt("%s/%s", g_tmp, name);

    tl_run(NULL, CMD("mkdir", "-p", dir));
    tl_run(NULL, CMD("cp", "server", "client", "libutils.so", dir));
}

static char *debug_strings(const char *dir)
{
    return tl_grep(tl_capture_out(CMD("strings", "-a", tl_fmt("%s/server", dir),
                                      tl_fmt("%s/libutils.so", dir))), DEBUG_ONLY_RE, 0);
}

/* The call instructions inside proto_send() of a build's libutils.so */
static char *proto_send_calls(const char *dir)
{
    char *dump = tl_capture_out(CMD("objdump", "-d", "--no-show-raw-insn",
                                    tl_fmt("%s/libutils.so", dir)));
    char *text = NULL;
    char *body = strstr(dump, " <proto_send>:\n");
    char *end;
    char *calls;
    char *line;
    char *save = NULL;

    if (!body)
        return strdup("");
    body = strchr(body, '\n') + 1;
    end = strstr(body, "\n\n");
    if (end)
        end[1] = '\0';
    calls = tl_grep(body, "[[:space:]]call", 0);
    text = strdup("");
    for (line = strtok_r(calls, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *colon = strchr(line, ':');

        line = colon ? colon + 1 : line;
        line += strspn(line, " \t");
        text = tl_fmt("%s%s\n", text, line);
    }
    return text;
}

static int run(void)
{
    char *release_dir = tl_fmt("%s/release", g_tmp);
    char *debug_dir = tl_fmt("%s/debug", g_tmp);
    char *make0 = tl_fmt("%s/make0.txt", g_tmp);
    char *make1 = tl_fmt("%s/make1.txt", g_tmp);
    char *rel_client_out = tl_fmt("%s/rel_client.txt", g_tmp);
    const char *rel_server_log = "logs/debug_release_server_d2.log";
    const char *client_d2 = "logs/debug_client_d2.log";
    int total[3], info[3], dbg[3], pkt[3];
    int rel_gcc, dbg_gcc, rel_flags, dbg_flags, rel_strings, dbg_strings, rel_calls, dbg_calls;
    int rel_client, rel_debug, rel_warn, lvl;

    tl_banner("Two-level debug control: compile time + run time");
    printf("date     : %s\n", tl_date());
    printf("compiler : %s\n", tl_compiler());
    if (tl_port_in_use(tl_port)) {
        printf("port %d is already in use; stop the other server or set PORT=...\n", tl_port);
        return 1;
    }

    tl_banner("Part 1 - compile time: make DEBUG=0 vs make DEBUG=1");
    tl_section("make clean && make DEBUG=0 all   (release)");
    tl_run(NULL, CMD("make", "-s", "clean-build"));
    if (tl_run(make0, CMD("make", "DEBUG=0", "all")) != 0) {
        tl_print_file(make0);
        return 1;
    }
    tl_print_file(make0);
    snapshot_build("release");
    tl_section("make DEBUG=1 all   (debug, the default)");
    if (tl_run(make1, CMD("make", "DEBUG=1", "all")) != 0) {
        tl_print_file(make1);
        return 1;
    }
    tl_print_file(make1);
    snapshot_build("debug");
    rel_gcc = tl_file_count(make0, "^gcc ");
    dbg_gcc = tl_file_count(make1, "^gcc ");
    rel_flags = tl_file_count(make0, "-DENABLE_DEBUG");
    dbg_flags = tl_file_count(make1, "-DENABLE_DEBUG");

    tl_section("Debug-only format strings in server + libutils.so (strings | grep)");
    rel_strings = tl_nlines(debug_strings(release_dir));
    dbg_strings = tl_nlines(debug_strings(debug_dir));
    printf("release build: %d\n", rel_strings);
    printf("debug build  : %d\n", dbg_strings);
    tl_print_lines(debug_strings(debug_dir), "    ", 0);

    tl_section("call instructions inside proto_send() (objdump -d libutils.so)");
    puts("release build:");
    tl_print_lines(proto_send_calls(release_dir), "    ", 0);
    puts("debug build:");
    tl_print_lines(proto_send_calls(debug_dir), "    ", 0);
    rel_calls = tl_count(proto_send_calls(release_dir), "<log_(write|hexdump)");
    dbg_calls = tl_count(proto_send_calls(debug_dir), "<log_(write|hexdump)");

    tl_section("Code size (size, bytes)");
    tl_wait(tl_spawn_opts(g_tmp, NULL, 0, CMD("size", "release/libutils.so", "debug/libutils.so",
                                              "release/server", "debug/server")));

    tl_section("Release build run with -d 2 (server and client)");
    if (start_server(release_dir, rel_server_log, CMD("-d", "2")) != 0)
        return 1;
    rel_client = tl_wait(tl_spawn_opts(release_dir, rel_client_out, 0,
                                       CMD("./client", "-p", tl_port_str, "-q", "-d", "2")));
    tl_sleep(0.3);
    tl_stop_server();
    puts("server log:");
    tl_print_lines(tl_read(rel_server_log), "    ", 0);
    puts("client output:");
    tl_print_lines(tl_read(rel_client_out), "    ", 0);
    rel_debug = tl_file_count(rel_server_log, " DEBUG ") + tl_file_count(rel_client_out, " DEBUG ");
    rel_warn = tl_file_count(rel_server_log, "has no effect");

    tl_banner("Part 2 - run time: debug build with -d 0 / -d 1 / -d 2");
    for (lvl = 0; lvl <= 2; lvl++) {
        char *log = tl_fmt("logs/debug_server_d%d.log", lvl);
        char *lvl_str = tl_fmt("%d", lvl);

        tl_section("./server -d %d, one client request", lvl);
        if (start_server(debug_dir, log, CMD("-d", lvl_str)) != 0)
            return 1;
        if (lvl == 2) {
            tl_wait(tl_spawn_opts(debug_dir, client_d2, 0,
                                  CMD("./client", "-p", tl_port_str, "-q", "-d", "2")));
        } else {
            tl_wait(tl_spawn_opts(debug_dir, "/dev/null", 0,
                                  CMD("./client", "-p", tl_port_str, "-q")));
        }
        tl_sleep(0.3);
        tl_stop_server();
        tl_print_file(log);
        total[lvl] = tl_nlines(tl_read(log));
        info[lvl] = tl_file_count(log, " (INFO|WARN|ERROR) ");
        dbg[lvl] = tl_file_count(log, " DEBUG ");
        pkt[lvl] = tl_file_count(log, PACKET_RE);
    }

    tl_section("Client side with -d 2 (logs/debug_client_d2.log)");
    tl_print_file(client_d2);

    tl_section("Server log lines per runtime level");
    printf("  %-6s %7s %11s %7s %14s\n", "level", "total", "INFO/WARN", "DEBUG", "packet-trace");
    for (lvl = 0; lvl <= 2; lvl++) {
        printf("  -d %-3d %7d %11d %7d %14d\n", lvl, total[lvl], info[lvl], dbg[lvl], pkt[lvl]);
    }

    tl_banner("Checks");
    tl_expect(rel_gcc == 3 && rel_flags == 0, "make DEBUG=0 builds all 3 targets without -DENABLE_DEBUG");
    tl_expect(dbg_gcc == 3 && dbg_flags == 3, "make DEBUG=1 builds all 3 targets with -DENABLE_DEBUG");
    tl_expect(rel_strings == 0, "release binaries contain 0 debug-only format strings");
    tl_expect(dbg_strings > 0, "debug binaries contain them (%d found)", dbg_strings);
    tl_expect(rel_calls == 0, "release proto_send() makes no logger call");
    tl_expect(dbg_calls >= 2, "debug proto_send() calls the logger (%d calls)", dbg_calls);
    tl_expect(rel_debug == 0, "release build with -d 2: 0 DEBUG lines from server and client");
    tl_expect(rel_warn == 1, "release server warns that -d has no effect");
    tl_expect(rel_client == 0, "release build still serves the request");
    tl_expect(info[0] > 0 && dbg[0] == 0, "-d 0: INFO lines only, 0 DEBUG lines");
    tl_expect(dbg[1] > 0 && pkt[1] == 0, "-d 1: verbose DEBUG lines, no packet trace");
    tl_expect(tl_file_count("logs/debug_server_d1.log", "DEBUG [a-z]+\\.c:[0-9]+:") > 0,
              "-d 1: DEBUG lines carry file:line");
    tl_expect(pkt[2] > 0, "-d 2: packet trace present (%d lines)", pkt[2]);
    tl_expect(tl_file_count("logs/debug_server_d2.log", "DEBUG     0000  4e 50") > 0,
              "-d 2: hex dump starts with the NP magic (4e 50)");
    tl_expect(total[0] < total[1] && total[1] < total[2], "output grows with the level (%d < %d < %d lines)",
              total[0], total[1], total[2]);

    tl_banner("Result");
    return tl_print_result();
}

int main(void)
{
    int rc;

    tl_init(9090);
    tl_tee("logs/test_debug.log");
    g_tmp = tl_mktemp("np_debug");
    rc = run();
    tl_stop_server();
    tl_rmrf(g_tmp);
    return tl_finish(rc);
}
