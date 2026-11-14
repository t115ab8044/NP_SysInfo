/*
 * test_robustness.c - With / Without proof for the robustness mechanisms.
 *
 *   [1]  waitpid() loop in the SIGCHLD handler   ./server --reap-once       vs ./server
 *   [2a] SIGPIPE ignored, EPIPE handled          ./server --no-sigpipe      vs ./server
 *   [2b] TCP_USER_TIMEOUT                        ./server --no-user-timeout vs ./server
 *   [3]  request validation                      ./server --no-validate     vs ./server
 *   [4]  request deadline                        ./server --no-deadline     vs ./server
 *   [5a] children die with the parent            ./server --no-pdeathsig    vs ./server
 *   [5b] graceful Ctrl+C                         ./server --no-graceful     vs ./server
 *
 * Each scenario starts a fresh server, provokes the failure with ./client,
 * collects evidence (ps, /proc, server log, client output) and records
 * PASS/FAIL. A With/Without table closes the report.
 *
 * The test first moves into a network namespace of its own, made without
 * root through a user namespace, like `unshare -rn`. Its loopback interface
 * belongs to the test alone, so [2b] can cut it with tc netem, and the port
 * cannot clash with a server already running. Where user namespaces are not
 * allowed, the test runs on the normal network and skips [2b].
 *
 * Usage : make test-robust        (env: PORT=9090)
 * Output: logs/test_robustness.log, logs/robust_<id>_<without|with>.log
 */
#define _GNU_SOURCE                     /* unshare(), CLONE_NEWUSER */
#include "testlib.h"

#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <unistd.h>

enum { WITHOUT, WITH };

#define SERVER_LIMIT 32                 /* DEFAULT_MAX_CLIENTS in server.c */
#define BURST        12                 /* [1] clients that disconnect together */
#define MAX_KIDS     64

static const char *const LABEL[] = { "without", "with" };
static const char *const UPPER[] = { "WITHOUT", "WITH" };

enum { R1, R2A, R2B, R3, R4, R5A, R5B, NROWS };
static const char *const ROW_NAME[NROWS] = {
    "[1] waitpid loop", "[2a] ignore SIGPIPE", "[2b] TCP_USER_TIMEOUT", "[3] validation",
    "[4] request deadline", "[5a] pdeathsig", "[5b] graceful Ctrl+C"
};

static char *g_tmp;
static const char *g_row[NROWS][2];     /* summary table, [mechanism][WITHOUT/WITH] */
static int g_netem;                     /* [2b] tc netem can cut the loopback */
static const char *g_tc = "tc";

/* ===== Helpers ========================================================== */

static char *server_log(const char *id, int w)
{
    return tl_fmt("logs/robust_%s_%s.log", id, LABEL[w]);
}

static int start(const char *id, int w, const char *const flags[])
{
    tl_section("[%s] %s: ./server -p %d %s", id, UPPER[w], tl_port, tl_join(flags));
    return tl_start_server(NULL, server_log(id, w), flags);
}

/*
 * [5b] Start the server the way a shell starts a job: in a process group of
 * its own, which its children join. Ctrl+C in a terminal sends SIGINT to the
 * whole group; kill(-group, SIGINT) does the same here.
 */
static int start_job(const char *id, int w, const char *const flags[])
{
    const char *argv[16];
    int n = 0;

    tl_section("[%s] %s: ./server -p %d %s", id, UPPER[w], tl_port, tl_join(flags));
    argv[n++] = "./server";
    argv[n++] = "-p";
    argv[n++] = tl_port_str;
    for (int i = 0; flags[i] != NULL && n < 15; i++)
        argv[n++] = flags[i];
    argv[n] = NULL;
    fflush(stdout);
    tl_server_pid = fork();
    if (tl_server_pid < 0)
        tl_die("fork failed: %s", strerror(errno));
    if (tl_server_pid == 0) {
        int fd = open(server_log(id, w), O_WRONLY | O_CREAT | O_TRUNC, 0644);

        setpgid(0, 0);
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        signal(SIGINT, SIG_DFL);        /* as in a terminal, even if we ignore it */
        if (fd < 0)
            _exit(126);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    setpgid(tl_server_pid, tl_server_pid);  /* on both sides, as a shell does */
    for (int i = 0; i < 100; i++) {
        if (tl_port_in_use(tl_port))
            return 0;
        tl_sleep(0.05);
    }
    puts("server did not start:");
    tl_print_file(server_log(id, w));
    return -1;
}

/* Print the server log lines matching ere, at most max of them. */
static void show_log(const char *id, int w, const char *ere, int max)
{
    char *lines = tl_grep(tl_read(server_log(id, w)), ere, 0);
    int n = tl_nlines(lines);

    puts("  server log:");
    if (n == 0)
        puts("    | (none)");
    tl_print_lines(n > max ? tl_head(lines, max) : lines, "    | ", 150);
    if (n > max)
        printf("    | ... and %d more line(s) like these\n", n - max);
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

/* Wait until the server has no children, at most max_s seconds. */
static void wait_children_gone(double max_s)
{
    long long end = tl_now_ms() + (long long)(max_s * 1000);

    while (tl_child_count() > 0 && tl_now_ms() < end)
        tl_sleep(0.05);
}

/* ===== Private network namespace (for [2b]) ============================= */

static int write_text(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY);
    ssize_t n;

    if (fd < 0)
        return -1;
    n = write(fd, text, strlen(text));
    close(fd);
    return n == (ssize_t)strlen(text) ? 0 : -1;
}

/*
 * Like `unshare -rn`: a new user namespace, in which this process is root,
 * and a new network namespace owned by it. No real root is needed, and
 * nothing changes outside. Returns 0 when the test now runs in it.
 */
static int private_network(void)
{
    char *uid_map = tl_fmt("0 %d 1", (int)getuid());
    char *gid_map = tl_fmt("0 %d 1", (int)getgid());
    struct ifreq ifr;
    int s;

    if (unshare(CLONE_NEWUSER | CLONE_NEWNET) != 0)
        return -1;
    if (write_text("/proc/self/setgroups", "deny") != 0 ||
        write_text("/proc/self/uid_map", uid_map) != 0 ||
        write_text("/proc/self/gid_map", gid_map) != 0)
        tl_die("cannot map the user id in the new namespace: %s", strerror(errno));
    /* The new namespace has one interface, lo, and it is down. */
    s = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "lo");
    if (s < 0 || ioctl(s, SIOCGIFFLAGS, &ifr) != 0)
        tl_die("cannot read the flags of lo: %s", strerror(errno));
    ifr.ifr_flags |= IFF_UP;
    if (ioctl(s, SIOCSIFFLAGS, &ifr) != 0)
        tl_die("cannot bring lo up: %s", strerror(errno));
    close(s);
    return 0;
}

static int find_tc(void)
{
    static const char *const paths[] = { "/usr/sbin/tc", "/sbin/tc" };

    if (tl_which("tc"))
        return 1;
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        if (access(paths[i], X_OK) == 0) {
            g_tc = paths[i];
            return 1;
        }
    }
    return 0;
}

/* Drop every packet on lo (cut = 1), or let them through again (cut = 0). */
static int netem(int cut)
{
    if (cut)
        return tl_run("/dev/null", CMD(g_tc, "qdisc", "add", "dev", "lo", "root",
                                       "netem", "loss", "100%"));
    return tl_run("/dev/null", CMD(g_tc, "qdisc", "del", "dev", "lo", "root"));
}

/* ===== Scenarios ======================================================== */

/* [1] 12 clients disconnect at the same moment. */
static void scenario1(void)
{
    int zombies[2] = { -1, -1 }, reaped[2] = { -1, -1 };
    char *active = "active=?";

    tl_heading("[1] SIGCHLD handler: waitpid() loop");
    for (int w = WITHOUT; w <= WITH; w++) {
        pid_t pids[BURST];
        char *log;

        if (start("1", w, w == WITHOUT ? CMD("--reap-once") : NO_FLAGS) != 0)
            return;
        printf("  $ ./client -p %d --mode idle   x %d (connect, send nothing)\n", tl_port, BURST);
        for (int i = 0; i < BURST; i++)
            pids[i] = tl_spawn("/dev/null", CMD("./client", "-p", tl_port_str, "--mode", "idle"));
        for (int i = 0; i < 150 && tl_child_count() < BURST; i++)
            tl_sleep(0.02);
        printf("  %d children are serving them\n", tl_child_count());
        printf("  $ kill -9 <the %d clients>   (all of them disconnect at the same moment)\n",
               BURST);
        for (int i = 0; i < BURST; i++)
            kill(pids[i], SIGKILL);
        for (int i = 0; i < BURST; i++)
            tl_wait(pids[i]);
        tl_sleep(0.5);
        show_children();
        tl_children(tl_server_pid, NULL, 0, &zombies[w]);
        log = tl_read(server_log("1", w));
        reaped[w] = tl_count(log, "reaped child");
        printf("  children reaped: %d of %d, zombies (STAT Z): %d\n", reaped[w], BURST, zombies[w]);
        if (w == WITHOUT && reaped[w] > 0) {
            active = tl_match(tl_tail(tl_grep(log, "reaped child", 0), 1), "active=[0-9]+");
            printf("  the parent still counts %s: it thinks those clients are connected\n",
                   active);
        }
        tl_stop_server();
        show_log("1", w, "reaped", w == WITHOUT ? BURST : 2);
    }
    tl_expect(zombies[WITHOUT] > 0 && reaped[WITHOUT] + zombies[WITHOUT] == BURST,
              "[1] without (one waitpid per signal): %d of %d reaped, %d stay zombies",
              reaped[WITHOUT], BURST, zombies[WITHOUT]);
    tl_expect(zombies[WITH] == 0 && reaped[WITH] == BURST,
              "[1] with the loop: all %d reaped, 0 zombies", BURST);
    g_row[R1][WITHOUT] = tl_fmt("%d of %d zombies, parent: %s", zombies[WITHOUT], BURST, active);
    g_row[R1][WITH] = tl_fmt("%d of %d reaped, 0 zombies", reaped[WITH], BURST);
}

/* [2a] The client leaves in the middle of WATCH 10 200. */
static void scenario2a(void)
{
    int killed[2] = { -1, -1 }, handled[2] = { -1, -1 }, next[2] = { -1, -1 };

    tl_heading("[2a] client leaves: SIGPIPE ignored, EPIPE handled");
    for (int w = WITHOUT; w <= WITH; w++) {
        if (start("2a", w, w == WITHOUT ? CMD("--no-sigpipe") : NO_FLAGS) != 0)
            return;
        printf("  $ ./client -p %d --mode abort\n", tl_port);
        tl_print_lines(tl_capture_out(CMD("./client", "-p", tl_port_str, "--mode", "abort")),
                       "    ", 150);
        tl_sleep(1.0);                  /* the child writes again at 200 and 400 ms */
        next[w] = tl_run("/dev/null", CMD("./client", "-p", tl_port_str));
        printf("  next ./client after that: exit %d (the parent keeps serving)\n", next[w]);
        tl_sleep(0.2);
        tl_stop_server();
        show_log("2a", w, "WATCH|left|reaped", 6);
        killed[w] = tl_file_count(server_log("2a", w), "killed by signal 13");
        handled[w] = tl_file_count(server_log("2a", w), "exit 3 \\(client left\\)");
    }
    tl_expect(killed[WITHOUT] == 1, "[2a] without: the child is killed by SIGPIPE (signal 13)");
    tl_expect(killed[WITH] == 0 && handled[WITH] == 1,
              "[2a] with: write() returns EPIPE, the child logs it and exits 3");
    tl_expect(next[WITHOUT] == 0 && next[WITH] == 0, "[2a] both: the next client is served");
    g_row[R2A][WITHOUT] = "child killed by signal 13";
    g_row[R2A][WITH] = "EPIPE handled, child exit 3";
}

/* [2b] The client's network vanishes in the middle of WATCH 20 500. */
static void scenario2b(void)
{
    int lines[2] = { -1, -1 }, served[2] = { -1, -1 }, unreachable[2] = { -1, -1 };
    double ended[2] = { -1, -1 };

    tl_heading("[2b] network vanishes: TCP_USER_TIMEOUT");
    if (!g_netem) {
        puts("  SKIPPED: needs a private network namespace and tc netem (see \"network\" above)");
        g_row[R2B][WITHOUT] = g_row[R2B][WITH] = "skipped (no netns or tc)";
        return;
    }
    for (int w = WITHOUT; w <= WITH; w++) {
        char *out = tl_fmt("%s/vanish_%s.txt", g_tmp, LABEL[w]);
        long long t0;
        pid_t cpid;

        if (start("2b", w, w == WITHOUT ? CMD("--no-user-timeout") : NO_FLAGS) != 0)
            return;
        printf("  $ ./client -p %d -n 20 -w 500   (20 snapshots in 10 s)\n", tl_port);
        cpid = tl_spawn(out, CMD("./client", "-p", tl_port_str, "-n", "20", "-w", "500"));
        for (int i = 0; i < 150 && tl_file_count(out, "^OK seq=") < 2; i++)
            tl_sleep(0.02);
        puts("  after 2 snapshots the client's network vanishes, and so does the client:");
        puts("  $ tc qdisc add dev lo root netem loss 100%   (drop every packet)");
        printf("  $ kill -9 %d\n", (int)cpid);
        t0 = tl_now_ms();
        netem(1);
        kill(cpid, SIGKILL);
        tl_wait(cpid);
        wait_children_gone(15);
        ended[w] = (double)(tl_now_ms() - t0) / 1000.0;
        netem(0);
        lines[w] = tl_file_count(out, "^OK seq=");
        printf("  the child ended %.1f s after the cut; the client had received %d of 20\n",
               ended[w], lines[w]);
        tl_stop_server();
        show_log("2b", w, "WATCH|unreachable|reaped", 6);
        served[w] = tl_file_count(server_log("2b", w), "exit 0 \\(request served\\)");
        unreachable[w] = tl_file_count(server_log("2b", w), "client unreachable");
    }
    tl_expect(served[WITHOUT] == 1 && lines[WITHOUT] < 20 && ended[WITHOUT] > 8,
              "[2b] without: every write() succeeds for %.1f s, the child logs \"request "
              "served\" (client got %d of 20)", ended[WITHOUT], lines[WITHOUT]);
    tl_expect(unreachable[WITH] == 1 && ended[WITH] < 6,
              "[2b] with: write() fails with ETIMEDOUT %.1f s after the cut, the child stops",
              ended[WITH]);
    g_row[R2B][WITHOUT] = tl_fmt("writes on %.1f s, \"served\", client got %d/20",
                                 ended[WITHOUT], lines[WITHOUT]);
    g_row[R2B][WITH] = tl_fmt("ETIMEDOUT %.1f s after the cut, exit 3", ended[WITH]);
}

/* [3] Four malicious requests. */
static void scenario3(void)
{
    static const char *const WRAP = "WATCH 4294967297 100";
    static const char *const FLOOD = "WATCH 100 0";
    static const char *const ENDLESS = "WATCH 100000 1000";
    int rejected[2] = { 0, 0 }, wrap = -1, flood = -1, endless = 0;
    long long flood_ms = 0;
    long rss = 0;

    tl_heading("[3] request validation: four malicious requests");
    for (int w = WITHOUT; w <= WITH; w++) {
        char *out = tl_fmt("%s/longline_%s.txt", g_tmp, LABEL[w]);
        char *slow = tl_fmt("%s/endless_%s.txt", g_tmp, LABEL[w]);
        pid_t kids[MAX_KIDS], cpid;
        long long t0;
        char *text;
        int n;

        if (start("3", w, w == WITHOUT ? CMD("--no-validate") : NO_FLAGS) != 0)
            return;

        printf("  (a) $ ./client -p %d --mode longline   (16 MiB, no newline)\n", tl_port);
        cpid = tl_spawn(out, CMD("./client", "-p", tl_port_str, "--mode", "longline"));
        tl_sleep(1.5);
        if (tl_children(tl_server_pid, kids, MAX_KIDS, NULL) > 0) {
            long kb = vm_rss_kb(kids[0]);

            printf("      after 1.5 s the child is still reading: RSS %ld KiB\n", kb);
            if (w == WITHOUT)
                rss = kb;
        }
        kill(cpid, SIGTERM);            /* the client never gives up by itself */
        tl_wait(cpid);
        tl_print_lines(tl_grep(tl_read(out), "RESULT: (sent all|server reply|no reply)", 0),
                       "      ", 150);
        rejected[w] += tl_file_count(out, "ERROR request too long");

        printf("  (b) $ ./client -p %d -r \"%s\"   (2^32 + 1)\n", tl_port, WRAP);
        text = tl_capture_out(CMD("./client", "-p", tl_port_str, "-r", WRAP));
        tl_print_lines(text, "      ", 110);
        rejected[w] += tl_count(text, "^ERROR");
        if (w == WITHOUT)
            wrap = tl_count(text, "^OK seq=");

        printf("  (c) $ ./client -p %d -r \"%s\"   (no pause between snapshots)\n", tl_port, FLOOD);
        t0 = tl_now_ms();
        text = tl_capture_out(CMD("./client", "-p", tl_port_str, "-r", FLOOD));
        n = tl_count(text, "^OK seq=");
        if (n > 0)
            printf("      %d snapshots in %lld ms\n", n, tl_now_ms() - t0);
        else
            tl_print_lines(text, "      ", 110);
        rejected[w] += tl_count(text, "^ERROR");
        if (w == WITHOUT) {
            flood = n;
            flood_ms = tl_now_ms() - t0;
        }

        printf("  (d) $ ./client -p %d -r \"%s\"   (one snapshot a second)\n", tl_port, ENDLESS);
        cpid = tl_spawn(slow, CMD("./client", "-p", tl_port_str, "-r", ENDLESS));
        tl_sleep(1.5);
        n = tl_child_count();
        kill(cpid, SIGTERM);
        tl_wait(cpid);
        if (n > 0)
            printf("      after 1.5 s: %d snapshot(s), the child keeps going: "
                   "100000 x 1 s = 27.8 hours\n", tl_file_count(slow, "^OK seq="));
        else
            tl_print_lines(tl_grep(tl_read(slow), "^ERROR", 0), "      ", 110);
        rejected[w] += tl_file_count(slow, "^ERROR");
        if (w == WITHOUT)
            endless = n > 0;

        tl_sleep(0.3);
        tl_stop_server();
        /* the last two doublings of the buffer are enough to show the growth */
        show_log("3", w, "grown to [0-9]{5} KiB|snapshot\\(s\\)|rejected", 10);
    }
    tl_expect(rss > 16000, "[3] without (a): 16 MiB with no newline is buffered, child RSS %ld KiB",
              rss);
    tl_expect(wrap == 1, "[3] without (b): %s is served as WATCH 1 100", WRAP);
    tl_expect(flood == 100, "[3] without (c): %s sends 100 snapshots in %lld ms", FLOOD, flood_ms);
    tl_expect(endless, "[3] without (d): %s keeps a child busy for 27.8 hours", ENDLESS);
    tl_expect(rejected[WITH] == 4, "[3] with: all 4 requests rejected with an ERROR line");
    g_row[R3][WITHOUT] = tl_fmt("%d of 4 accepted, child RSS %ld KiB",
                                4 - rejected[WITHOUT], rss);
    g_row[R3][WITH] = tl_fmt("%d of 4 rejected with ERROR", rejected[WITH]);
}

/* [4] 32 Slowloris clients take every slot, then an ordinary client comes. */
static void scenario4(void)
{
    int held[2] = { -1, -1 }, timeouts[2] = { -1, -1 }, next[2] = { -1, -1 };

    tl_heading("[4] request deadline: Slowloris");
    for (int w = WITHOUT; w <= WITH; w++) {
        pid_t pids[SERVER_LIMIT];
        long long t0;
        double left;
        char *text;

        if (start("4", w, w == WITHOUT ? CMD("--no-deadline") : NO_FLAGS) != 0)
            return;
        printf("  $ ./client -p %d --mode slow   x %d (1 byte every 0.5 s, never a newline)\n",
               tl_port, SERVER_LIMIT);
        t0 = tl_now_ms();
        for (int i = 0; i < SERVER_LIMIT; i++)
            pids[i] = tl_spawn(tl_fmt("%s/slow_%s_%d.txt", g_tmp, LABEL[w], i),
                               CMD("./client", "-p", tl_port_str, "--mode", "slow"));
        for (int i = 0; i < 100 && tl_child_count() < SERVER_LIMIT; i++)
            tl_sleep(0.02);
        printf("  %d children, each waiting for a request line (server limit -m %d)\n",
               tl_child_count(), SERVER_LIMIT);
        left = 4.0 - (double)(tl_now_ms() - t0) / 1000.0;
        if (left > 0)
            tl_sleep(left);
        held[w] = tl_child_count();
        printf("  4 s later: %d children still waiting\n", held[w]);
        printf("  $ ./client -p %d   (an ordinary client arrives)\n", tl_port);
        text = tl_capture_out(CMD("./client", "-p", tl_port_str));
        tl_print_lines(text, "    ", 110);
        next[w] = tl_count(text, "^OK seq=1 ");
        timeouts[w] = 0;
        for (int i = 0; i < SERVER_LIMIT; i++) {
            kill(pids[i], SIGTERM);     /* a no-op for clients already cut off */
            tl_wait(pids[i]);
            timeouts[w] += tl_file_count(tl_fmt("%s/slow_%s_%d.txt", g_tmp, LABEL[w], i),
                                         "request timeout");
        }
        printf("  slow clients that got \"ERROR request timeout\": %d\n", timeouts[w]);
        if (timeouts[w] > 0)
            tl_print_lines(tl_grep(tl_read(tl_fmt("%s/slow_%s_0.txt", g_tmp, LABEL[w])),
                                   "RESULT", 0), "    ", 150);
        tl_stop_server();
        show_log("4", w, "no complete|clients connected", 3);
    }
    tl_expect(held[WITHOUT] == SERVER_LIMIT && next[WITHOUT] == 0,
              "[4] without: %d slow clients hold all %d children, the next client is told busy",
              SERVER_LIMIT, SERVER_LIMIT);
    tl_expect(timeouts[WITH] == SERVER_LIMIT && held[WITH] == 0 && next[WITH] == 1,
              "[4] with: all %d cut off after 3 s, the next client is served", SERVER_LIMIT);
    g_row[R4][WITHOUT] = tl_fmt("%d slow clients hold %d slots, next: busy",
                                held[WITHOUT], SERVER_LIMIT);
    g_row[R4][WITH] = tl_fmt("%d cut off at 3 s, next client served", timeouts[WITH]);
}

/* [5a] The parent is killed while a child is serving WATCH 10 300. */
static void scenario5a(void)
{
    int alive[2] = { -1, -1 }, lines[2] = { -1, -1 }, new_ppid = -1;
    pid_t old_ppid = 0;

    tl_heading("[5a] parent killed: children die with it (PR_SET_PDEATHSIG)");
    for (int w = WITHOUT; w <= WITH; w++) {
        char *out = tl_fmt("%s/watch_%s.txt", g_tmp, LABEL[w]);
        pid_t kids[MAX_KIDS], parent, child = 0, cpid;
        char state = '?';
        int ppid = 0;

        if (start("5a", w, w == WITHOUT ? CMD("--no-pdeathsig") : NO_FLAGS) != 0)
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
              "[5a] without: the child outlives the parent as an orphan and sends all 10");
    tl_expect(alive[WITH] == 0 && lines[WITH] < 10,
              "[5a] with: the child ends with the parent (%d of 10 sent)", lines[WITH]);
    g_row[R5A][WITHOUT] = tl_fmt("orphan runs on (PPID %d), 10/10", new_ppid);
    g_row[R5A][WITH] = tl_fmt("child ends with parent, %d/10", lines[WITH]);
}

/* [5b] Ctrl+C while a child is serving WATCH 10 300. */
static void scenario5b(void)
{
    int lines[2] = { -1, -1 }, status[2] = { -1, -1 }, refused[2] = { -1, -1 };
    double took[2] = { -1, -1 };

    tl_heading("[5b] Ctrl+C: graceful shutdown");
    for (int w = WITHOUT; w <= WITH; w++) {
        char *out = tl_fmt("%s/ctrlc_%s.txt", g_tmp, LABEL[w]);
        pid_t cpid, server;
        long long t0;
        char *text;

        if (start_job("5b", w, w == WITHOUT ? CMD("--no-graceful") : NO_FLAGS) != 0)
            return;
        server = tl_server_pid;
        printf("  $ ./client -p %d -n 10 -w 300   (10 snapshots in 3 s)\n", tl_port);
        cpid = tl_spawn(out, CMD("./client", "-p", tl_port_str, "-n", "10", "-w", "300"));
        tl_sleep(0.8);
        printf("  $ kill -INT -%d   (Ctrl+C: SIGINT to the server's whole process group)\n",
               (int)server);
        t0 = tl_now_ms();
        kill(-server, SIGINT);
        tl_sleep(0.2);
        printf("  $ ./client -p %d   (a new client, 0.2 s after Ctrl+C)\n", tl_port);
        text = tl_capture(CMD("./client", "-p", tl_port_str));
        tl_print_lines(tl_grep(text, "ERROR|^OK", 0), "    ", 120);
        refused[w] = tl_count(text, "Connection refused");
        tl_wait(cpid);
        lines[w] = tl_file_count(out, "^OK seq=");
        printf("  the running client received %d of 10 snapshots\n", lines[w]);
        tl_print_lines(tl_grep(tl_read(out), "WARN", 0), "    ", 150);
        status[w] = tl_wait(server);
        took[w] = (double)(tl_now_ms() - t0) / 1000.0;
        tl_server_pid = 0;
        if (status[w] > 128)
            printf("  server: killed by signal %d (%s), %.1f s after Ctrl+C\n", status[w] - 128,
                   strsignal(status[w] - 128), took[w]);
        else
            printf("  server: exit %d, %.1f s after Ctrl+C\n", status[w], took[w]);
        show_log("5b", w, "Ctrl\\+C|reaped|exits", 6);
    }
    tl_expect(status[WITHOUT] == 128 + SIGINT && lines[WITHOUT] < 10,
              "[5b] without: Ctrl+C kills the server and the child at once (%d of 10 sent)",
              lines[WITHOUT]);
    tl_expect(status[WITH] == 0 && lines[WITH] == 10 && refused[WITH] == 1,
              "[5b] with: new clients refused, the running one gets all 10, then exit 0");
    g_row[R5B][WITHOUT] = tl_fmt("client cut off at %d/10", lines[WITHOUT]);
    g_row[R5B][WITH] = tl_fmt("10/10 delivered, exit 0 after %.1f s", took[WITH]);
}

static void summary(void)
{
    static const char dashes[] = "----------------------------------------------";

    tl_banner("With / Without summary");
    printf("  %-21s | %-42s | %s\n", "mechanism", "WITHOUT (switched off)", "WITH (default)");
    printf("  %.21s-+-%.42s-+-%.36s\n", dashes, dashes, dashes);
    for (int r = 0; r < NROWS; r++)
        printf("  %-21s | %-42s | %s\n", ROW_NAME[r], g_row[r][WITHOUT] ? g_row[r][WITHOUT] : "?",
               g_row[r][WITH] ? g_row[r][WITH] : "?");
}

static int run(void)
{
    int own_net;

    tl_banner("Robustness: seven mechanisms, each With and Without");
    printf("date     : %s\n", tl_date());
    printf("kernel   : %s\n", tl_kernel());
    if (tl_run(NULL, CMD("make", "-s", "all")) != 0)
        return 1;
    own_net = private_network() == 0;
    g_netem = own_net && find_tc() && netem(1) == 0 && netem(0) == 0;
    if (!own_net)
        printf("network  : host network (user namespaces not allowed here): [2b] skipped\n");
    else
        printf("network  : private network namespace, made without root; tc netem %s\n",
               g_netem ? "available" : "NOT available: [2b] skipped");
    if (tl_port_in_use(tl_port)) {
        printf("port %d is already in use; stop the other server or set PORT=...\n", tl_port);
        return 1;
    }
    scenario1();
    scenario2a();
    scenario2b();
    scenario3();
    scenario4();
    scenario5a();
    scenario5b();
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
