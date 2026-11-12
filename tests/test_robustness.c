/*
 * test_robustness.c - With / Without proof for the five robustness mechanisms.
 *
 *   [1] SIGCHLD handler + waitpid() loop   ./server --no-sigchld   vs ./server
 *   [2] SIGPIPE ignored, EPIPE handled     ./server --no-sigpipe   vs ./server
 *   [3] request line length limit          ./server --no-validate  vs ./server
 *   [4] limit on connected clients         ./server --no-limit     vs ./server -m 5
 *   [5] children die with the parent       ./server --no-pdeathsig vs ./server
 *
 * Each scenario starts a fresh server, provokes the failure with ./client,
 * collects evidence (ps, /proc, server log, client output) and records
 * PASS/FAIL. A With/Without table closes the report.
 *
 * Usage : make test-robust        (env: PORT=9090)
 * Output: logs/test_robustness.log, logs/robust_<n>_<without|with>.log
 */
#include "testlib.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WITHOUT, WITH };

#define IDLE_CLIENTS 10
#define MAX_KIDS     64

static const char *const LABEL[] = { "without", "with" };
static const char *const UPPER[] = { "WITHOUT", "WITH" };

static char *g_tmp;
static const char *g_row[6][2];         /* summary table, [mechanism][WITHOUT/WITH] */

static char *server_log(int n, int w)
{
    return tl_fmt("logs/robust_%d_%s.log", n, LABEL[w]);
}

static int start(int n, int w, const char *const flags[])
{
    tl_section("[%d] %s: ./server -p %d %s", n, UPPER[w], tl_port, tl_join(flags));
    return tl_start_server(NULL, server_log(n, w), flags);
}

static void show_log(int n, int w, const char *ere)
{
    puts("  server log:");
    tl_print_lines(tl_grep(tl_read(server_log(n, w)), ere, 0), "    | ", 150);
}

static void show_children(void)
{
    char *pid = tl_fmt("%d", (int)tl_server_pid);

    printf("  $ ps -o pid,ppid,stat,rss,cmd --ppid %s\n", pid);
    tl_print_lines(tl_capture_out(CMD("ps", "-o", "pid,ppid,stat,rss,cmd", "--ppid", pid)),
                   "    ", 150);
}

static long vm_rss_kb(pid_t pid)
{
    char *m = tl_match(tl_read(tl_fmt("/proc/%d/status", (int)pid)), "VmRSS:[[:space:]]+[0-9]+");

    return m ? strtol(m + 6, NULL, 10) : 0;
}

/* State and PPID from /proc/<pid>/stat. Returns 0 if the process is gone. */
static int proc_state(pid_t pid, char *state, int *ppid)
{
    char *p = strrchr(tl_read(tl_fmt("/proc/%d/stat", (int)pid)), ')');

    return p != NULL && sscanf(p + 1, " %c %d", state, ppid) == 2;
}

/* [1] Three finished children: zombies or reaped? */
static void scenario1(void)
{
    int zombies[2] = { -1, -1 }, reaped[2] = { -1, -1 };

    tl_heading("[1] SIGCHLD handler + waitpid() loop");
    for (int w = WITHOUT; w <= WITH; w++) {
        int kids, z;

        if (start(1, w, w == WITHOUT ? CMD("--no-sigchld") : NO_FLAGS) != 0)
            return;
        printf("  $ ./client -p %d   x 3 (one after another)\n", tl_port);
        for (int i = 0; i < 3; i++)
            tl_run("/dev/null", CMD("./client", "-p", tl_port_str));
        tl_sleep(0.3);
        show_children();
        kids = tl_children(tl_server_pid, NULL, 0, &z);
        printf("  children left: %d, zombies (STAT Z): %d\n", kids, z);
        tl_stop_server();
        show_log(1, w, "reaped|robustness");
        zombies[w] = z;
        reaped[w] = tl_file_count(server_log(1, w), "reaped child");
    }
    tl_expect(zombies[WITHOUT] == 3, "[1] without: the 3 finished children stay zombies");
    tl_expect(zombies[WITH] == 0 && reaped[WITH] == 3,
              "[1] with: 0 zombies, all 3 children reaped and logged");
    g_row[1][WITHOUT] = tl_fmt("%d zombies <defunct>", zombies[WITHOUT]);
    g_row[1][WITH] = tl_fmt("%d zombies, %d reaped", zombies[WITH], reaped[WITH]);
}

/* [2] The client leaves in the middle of WATCH 10 200. */
static void scenario2(void)
{
    int killed[2] = { -1, -1 }, handled[2] = { -1, -1 }, next[2] = { -1, -1 };

    tl_heading("[2] SIGPIPE ignored, EPIPE handled");
    for (int w = WITHOUT; w <= WITH; w++) {
        if (start(2, w, w == WITHOUT ? CMD("--no-sigpipe") : NO_FLAGS) != 0)
            return;
        printf("  $ ./client -p %d --mode abort\n", tl_port);
        tl_print_lines(tl_capture_out(CMD("./client", "-p", tl_port_str, "--mode", "abort")),
                       "    ", 150);
        tl_sleep(1.0);                  /* the child writes again at 200 and 400 ms */
        next[w] = tl_run("/dev/null", CMD("./client", "-p", tl_port_str));
        printf("  next ./client after that: exit %d (the parent keeps serving)\n", next[w]);
        tl_sleep(0.2);
        tl_stop_server();
        show_log(2, w, "WATCH|left|reaped");
        killed[w] = tl_file_count(server_log(2, w), "killed by signal 13");
        handled[w] = tl_file_count(server_log(2, w), "exit 3 \\(client left\\)");
    }
    tl_expect(killed[WITHOUT] == 1, "[2] without: the child is killed by SIGPIPE (signal 13)");
    tl_expect(killed[WITH] == 0 && handled[WITH] == 1,
              "[2] with: write() returns EPIPE, the child logs it and exits 3");
    tl_expect(next[WITHOUT] == 0 && next[WITH] == 0, "[2] both: the next client is served");
    g_row[2][WITHOUT] = "child killed by signal 13";
    g_row[2][WITH] = "EPIPE handled, child exit 3";
}

/* [3] 16 MiB request line without a newline. */
static void scenario3(void)
{
    int rejected[2] = { -1, -1 }, grown[2] = { -1, -1 }, alive[2] = { -1, -1 };
    long rss[2] = { 0, 0 };
    char *buffer = "?";

    tl_heading("[3] request line length limit");
    for (int w = WITHOUT; w <= WITH; w++) {
        char *out = tl_fmt("%s/longline_%s.txt", g_tmp, LABEL[w]);
        pid_t kids[MAX_KIDS], cpid;
        char *log;

        if (start(3, w, w == WITHOUT ? CMD("--no-validate") : NO_FLAGS) != 0)
            return;
        printf("  $ ./client -p %d --mode longline   (16 MiB, no newline)\n", tl_port);
        cpid = tl_spawn(out, CMD("./client", "-p", tl_port_str, "--mode", "longline"));
        tl_sleep(1.5);
        alive[w] = tl_children(tl_server_pid, kids, MAX_KIDS, NULL);
        rss[w] = alive[w] > 0 ? vm_rss_kb(kids[0]) : 0;
        show_children();
        kill(cpid, SIGTERM);            /* the client never gives up by itself */
        tl_wait(cpid);
        tl_print_lines(tl_grep(tl_read(out), "RESULT", 0), "    ", 150);
        if (tl_file_count(out, "RESULT: (server reply|no reply)") == 0)
            puts("    (no reply after 1.5 s: the test ends the client)");
        tl_sleep(0.3);
        tl_stop_server();
        show_log(3, w, "longer than|grown to|reaped|left before");
        log = tl_read(server_log(3, w));
        rejected[w] = tl_count(log, "longer than 127 bytes -> rejected");
        grown[w] = tl_count(log, "grown to");
        if (w == WITHOUT && grown[w] > 0)
            buffer = tl_match(tl_tail(tl_grep(log, "grown to", 0), 1), "[0-9]+ KiB");
    }
    tl_expect(grown[WITHOUT] >= 5 && rss[WITHOUT] > 16000,
              "[3] without: the buffer grows to %s, child RSS %ld KiB", buffer, rss[WITHOUT]);
    tl_expect(rejected[WITH] == 1 && alive[WITH] == 0,
              "[3] with: rejected after 127 bytes, the child is gone 1.5 s later");
    g_row[3][WITHOUT] = tl_fmt("buffer %s, RSS %ld KiB", buffer, rss[WITHOUT]);
    g_row[3][WITH] = "rejected after 127 bytes";
}

/* [4] Ten clients connect and send nothing. */
static void scenario4(void)
{
    int kids[2] = { -1, -1 }, busy[2] = { -1, -1 };

    tl_heading("[4] limit on connected clients");
    for (int w = WITHOUT; w <= WITH; w++) {
        pid_t pids[IDLE_CLIENTS];

        if (start(4, w, w == WITHOUT ? CMD("--no-limit") : CMD("-m", "5")) != 0)
            return;
        printf("  $ ./client -p %d --mode idle   x %d (connect, send nothing)\n",
               tl_port, IDLE_CLIENTS);
        for (int i = 0; i < IDLE_CLIENTS; i++)
            pids[i] = tl_spawn(tl_fmt("%s/idle_%s_%d.txt", g_tmp, LABEL[w], i),
                               CMD("./client", "-p", tl_port_str, "--mode", "idle"));
        tl_sleep(0.6);
        kids[w] = tl_child_count();
        printf("  server children while the %d clients are connected: %d\n", IDLE_CLIENTS, kids[w]);
        puts("  the test ends the clients that are still waiting");
        busy[w] = 0;
        for (int i = 0; i < IDLE_CLIENTS; i++) {
            kill(pids[i], SIGTERM);     /* a no-op for clients already told busy */
            tl_wait(pids[i]);
            busy[w] += tl_file_count(tl_fmt("%s/idle_%s_%d.txt", g_tmp, LABEL[w], i),
                                     "server busy");
        }
        printf("  clients answered \"ERROR server busy\": %d\n", busy[w]);
        tl_stop_server();
        show_log(4, w, "robustness|rejected");
    }
    tl_expect(kids[WITHOUT] == IDLE_CLIENTS && busy[WITHOUT] == 0,
              "[4] without: %d idle clients hold %d children", IDLE_CLIENTS, IDLE_CLIENTS);
    tl_expect(kids[WITH] == 5 && busy[WITH] == IDLE_CLIENTS - 5,
              "[4] with -m 5: 5 children, the other %d clients told busy", IDLE_CLIENTS - 5);
    g_row[4][WITHOUT] = tl_fmt("%d children for %d idle clients", kids[WITHOUT], IDLE_CLIENTS);
    g_row[4][WITH] = tl_fmt("%d children, %d told busy", kids[WITH], busy[WITH]);
}

/* [5] The parent is killed while a child is serving WATCH 10 300. */
static void scenario5(void)
{
    int alive[2] = { -1, -1 }, lines[2] = { -1, -1 }, new_ppid = -1;
    pid_t old_ppid = 0;

    tl_heading("[5] children die with the parent (PR_SET_PDEATHSIG)");
    for (int w = WITHOUT; w <= WITH; w++) {
        char *out = tl_fmt("%s/watch_%s.txt", g_tmp, LABEL[w]);
        pid_t kids[MAX_KIDS], parent, child = 0, cpid;
        char state = '?';
        int ppid = 0;

        if (start(5, w, w == WITHOUT ? CMD("--no-pdeathsig") : NO_FLAGS) != 0)
            return;
        printf("  $ ./client -p %d -n 10 -w 300   (10 snapshots in 3 s)\n", tl_port);
        cpid = tl_spawn(out, CMD("./client", "-p", tl_port_str, "-n", "10", "-w", "300"));
        tl_sleep(0.8);
        parent = tl_server_pid;
        if (tl_children(parent, kids, MAX_KIDS, NULL) > 0)
            child = kids[0];
        show_children();
        printf("  $ kill -9 %d   (the parent server crashes)\n", (int)parent);
        kill(parent, SIGKILL);
        tl_wait(parent);
        tl_server_pid = 0;
        tl_sleep(0.3);
        alive[w] = child > 0 && proc_state(child, &state, &ppid) && state != 'Z';
        if (alive[w]) {
            printf("  child %d still running: STAT %c, PPID %d (was %d) -> orphan\n",
                   (int)child, state, ppid, (int)parent);
            if (w == WITHOUT) {
                old_ppid = parent;
                new_ppid = ppid;
            }
        } else {
            printf("  child %d is gone: the kernel sent it SIGTERM when the parent died\n",
                   (int)child);
        }
        tl_wait(cpid);
        lines[w] = tl_file_count(out, "^OK seq=");
        printf("  the client received %d of 10 snapshots\n", lines[w]);
        tl_print_lines(tl_grep(tl_read(out), "WARN|ERROR", 0), "    ", 150);
    }
    tl_expect(alive[WITHOUT] == 1 && new_ppid != (int)old_ppid && lines[WITHOUT] == 10,
              "[5] without: the child outlives the parent as an orphan and sends all 10");
    tl_expect(alive[WITH] == 0 && lines[WITH] < 10,
              "[5] with: the child ends with the parent (%d of 10 sent)", lines[WITH]);
    g_row[5][WITHOUT] = tl_fmt("orphan runs on (PPID %d), 10/10", new_ppid);
    g_row[5][WITH] = tl_fmt("child ends with parent, %d/10", lines[WITH]);
}

static void summary(void)
{
    static const char *const names[] = {
        NULL, "[1] reap children", "[2] ignore SIGPIPE", "[3] length limit",
        "[4] max clients", "[5] pdeathsig"
    };

    tl_banner("With / Without summary");
    printf("  %-19s | %-34s | %s\n", "mechanism", "WITHOUT (switched off)", "WITH (default)");
    printf("  %.19s-+-%.34s-+-%.30s\n", "--------------------", "-----------------------------------",
           "------------------------------");
    for (int n = 1; n <= 5; n++)
        printf("  %-19s | %-34s | %s\n", names[n], g_row[n][WITHOUT] ? g_row[n][WITHOUT] : "?",
               g_row[n][WITH] ? g_row[n][WITH] : "?");
}

static int run(void)
{
    tl_banner("Robustness: five mechanisms, each With and Without");
    printf("date     : %s\n", tl_date());
    printf("kernel   : %s\n", tl_kernel());
    if (tl_run(NULL, CMD("make", "-s", "all")) != 0)
        return 1;
    if (tl_port_in_use(tl_port)) {
        printf("port %d is already in use; stop the other server or set PORT=...\n", tl_port);
        return 1;
    }
    scenario1();
    scenario2();
    scenario3();
    scenario4();
    scenario5();
    summary();
    tl_banner("Result");
    return tl_print_result();
}

int main(void)
{
    int rc;

    tl_init(9090);
    tl_tee("logs/test_robustness.log");
    g_tmp = tl_mktemp("np_robust");
    rc = run();
    tl_stop_server();
    tl_rmrf(g_tmp);
    return tl_finish(rc);
}
