/*
 * test_robustness.c - Without/With proof for the robustness mechanisms.
 *
 *   [1]  SIGCHLD handler + waitpid(WNOHANG)    ./server --no-sigchld   vs ./server
 *   [1b] waitpid in a loop (signals coalesce)  ./server --single-wait  vs ./server
 *   [2]  SIGPIPE ignored + EPIPE handling      ./server --no-sigpipe   vs ./server
 *   [3]  header validation + socket timeout    ./server --no-validate --no-timeout
 *                                              vs ./server -t 5
 *   [4]  connection limit, total and per IP    ./server --no-limit / -m 10
 *                                              vs ./server -m 10 --max-per-ip 5
 *   [5]  whole-request deadline                ./server --no-deadline  vs ./server
 *   [6]  TCP stream framing                    ./server --naive-recv   vs ./server
 *   [7]  heartbeat for endless subscriptions   ./server --no-heartbeat vs ./server
 *   [8]  client reconnect with backoff         ./client --no-reconnect vs ./client
 *   [9]  children die with the parent          ./server --no-pdeathsig vs ./server
 *
 * Each scenario starts a fresh server, provokes the failure with the client's
 * fault-injection modes, collects evidence (ps, server log, client output) and
 * records PASS/FAIL. A With/Without table closes the report.
 *
 * Usage : make test-robust        (env: PORT=9090)
 * Output: logs/test_robustness.log, logs/robust_m<N>_<without|with>_*.log
 */
#include "testlib.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNSET (-1)
#define MAX_PIDS 64

enum { WITHOUT, WITH, CAP };

static const char *const LABEL[] = { "without", "with", "cap" };
static const char *const UPPER[] = { "WITHOUT", "WITH", "CAP" };

static char *g_tmp;

static int m1[2] = { UNSET, UNSET };
static int m1_reaped[2] = { UNSET, UNSET };
static int m1b[2] = { UNSET, UNSET };
static int m2_killed[2] = { UNSET, UNSET };
static int m2_handled[2] = { UNSET, UNSET };
static int m2_next[2] = { UNSET, UNSET };
static int m3_stuck[2] = { UNSET, UNSET };
static int m3_noreact[2] = { UNSET, UNSET };
static int m3_ok[2] = { UNSET, UNSET };
static int m4_kids[3] = { UNSET, UNSET, UNSET };
static int m4_rej[3] = { UNSET, UNSET, UNSET };
static const char *m4_other[3] = { "?", "?", "?" };
static int m5_alive[2] = { UNSET, UNSET };
static int m5_cut[2] = { UNSET, UNSET };
static int m6_frag[2] = { UNSET, UNSET };
static int m6_pipe[2] = { UNSET, UNSET };
static int m7_alive[2] = { UNSET, UNSET };
static int m7_hb[2] = { UNSET, UNSET };
static const char *m8_state[2] = { "?", "?" };
static int m8_pids[2] = { UNSET, UNSET };
static int m9[2] = { UNSET, UNSET };

static const char *v(int value)
{
    return value == UNSET ? "?" : tl_fmt("%d", value);
}

static char *robust_log(const char *m, int mode, const char *suffix)
{
    return tl_fmt("logs/robust_%s_%s_%s.log", m, LABEL[mode], suffix);
}

static int start(const char *log, const char *const flags[])
{
    printf("$ ./server -p %d %s\n", tl_port, tl_join(flags));
    return tl_start_server(NULL, log, flags);
}

static char *server_pid_str(void)
{
    return tl_fmt("%d", (int)tl_server_pid);
}

static void show_children(void)
{
    if (tl_run(NULL, CMD("ps", "-o", "pid,ppid,stat,vsz,rss,etime,cmd", "--ppid", server_pid_str())) != 0)
        puts("  (no child processes)");
}

static void show_log(const char *ere)
{
    tl_print_lines(tl_grep(tl_read(tl_server_log), ere, 0), "  ", 0);
}

static int wait_all(const pid_t *pids, int n)
{
    int ok = 0;
    int i;

    for (i = 0; i < n; i++) {
        if (tl_wait(pids[i]) == 0)
            ok++;
    }
    return ok;
}

/* [1] Twelve short clients, then count the <defunct> children left behind. */
static void m1_run(int mode)
{
    const char *const *flags = mode == WITHOUT ? CMD("--no-sigchld") : NO_FLAGS;
    pid_t pids[12];
    int ok, z, reaped, i;

    tl_section("%s", UPPER[mode]);
    if (start(robust_log("m1", mode, "server"), flags) != 0)
        return;
    for (i = 0; i < 12; i++)
        pids[i] = tl_spawn("/dev/null", CMD("./client", "-p", tl_port_str, "-q"));
    ok = wait_all(pids, 12);
    tl_sleep(1);
    printf("12 clients done (%d exited 0). Server children 1 s later:\n", ok);
    show_children();
    puts("ps aux | grep defunct:");
    fputs(tl_grep(tl_capture_out(CMD("ps", "auxww")), "^USER |[[:space:]]<defunct>$", 0), stdout);
    z = tl_zombie_count();
    reaped = tl_file_count(tl_server_log, "reaped child");
    printf("zombie children: %d | 'reaped child' lines in server log: %d\n", z, reaped);
    m1[mode] = z;
    m1_reaped[mode] = reaped;
    tl_stop_server();
}

/* [2] Three clients subscribe and vanish while the server keeps pushing. */
static void m2_run(int mode)
{
    const char *const *flags = mode == WITHOUT ? CMD("--no-sigpipe") : NO_FLAGS;
    char *clog = robust_log("m2", mode, "clients");
    pid_t pids[3];
    int killed, handled, rc, i;
    const char *alive;

    tl_section("%s", UPPER[mode]);
    if (start(robust_log("m2", mode, "server"), flags) != 0)
        return;
    tl_write(clog, "", 0);
    for (i = 0; i < 3; i++) {
        pids[i] = tl_spawn_opts(NULL, clog, 1, CMD("./client", "-p", tl_port_str, "--mode", "abort",
                                                   "--abort-after", "1"));
    }
    wait_all(pids, 3);
    puts("$ ./client --mode abort --abort-after 1   x 3 (subscribe, then disconnect)");
    tl_sleep(1.5);      /* pushes go on every 200 ms; the 2nd or 3rd hits the closed socket */
    puts("server log, child exits:");
    show_log("exiting cleanly|killed by signal|reaped child");
    killed = tl_file_count(tl_server_log, "killed by signal 13");
    handled = tl_file_count(tl_server_log, "exit 4 \\(");
    rc = tl_run("/dev/null", CMD("./client", "-p", tl_port_str, "-q"));
    alive = kill(tl_server_pid, 0) == 0 && tl_running(tl_server_pid) ? "yes" : "no";
    printf("children killed by SIGPIPE: %d | children that handled EPIPE (exit 4): %d\n", killed, handled);
    printf("server parent alive: %s | next normal client exit status: %d\n", alive, rc);
    m2_killed[mode] = killed;
    m2_handled[mode] = handled;
    m2_next[mode] = rc;
    tl_stop_server();
}

/* [3] garbage / biglen / idle clients that keep their socket open for 8 s. */
static void m3_run(int mode)
{
    static const char *const modes[] = { "garbage", "biglen", "idle" };
    const char *const *flags = mode == WITHOUT ? CMD("--no-validate", "--no-timeout") : CMD("-t", "5");
    char *clog = robust_log("m3", mode, "clients");
    pid_t pids[3];
    int stuck, noreact, ok, i;

    tl_section("%s", UPPER[mode]);
    if (start(robust_log("m3", mode, "server"), flags) != 0)
        return;
    for (i = 0; i < 3; i++) {
        pids[i] = tl_spawn(tl_fmt("%s/m3_%s.txt", g_tmp, modes[i]),
                           CMD("./client", "-p", tl_port_str, "--mode", modes[i], "--hold", "8"));
    }
    puts("$ ./client --mode garbage|biglen|idle --hold 8   (3 clients, background)");
    tl_sleep(6.5);
    puts("6.5 s after connecting (a 5 s timeout would have fired by now), server children:");
    show_children();
    stuck = tl_child_count();
    ok = wait_all(pids, 3);
    tl_write(clog, "", 0);
    for (i = 0; i < 3; i++)
        tl_append_prefixed(tl_fmt("%s/m3_%s.txt", g_tmp, modes[i]), tl_fmt("[%s] ", modes[i]), clog);
    puts("client output:");
    tl_print_lines(tl_read(clog), "  ", 0);
    puts("server log:");
    show_log("rejected header|trusting body_len|no request within|malloc|reaped child");
    noreact = tl_file_count(clog, "no reaction");
    printf("children still alive at 6.5 s: %d | clients with no reaction: %d | clients answered: %d\n",
           stuck, noreact, ok);
    m3_stuck[mode] = stuck;
    m3_noreact[mode] = noreact;
    m3_ok[mode] = ok;
    tl_stop_server();
}

/* [1b] 30 clients leave at once: their SIGCHLDs coalesce into fewer signals. */
static void m1b_run(int mode)
{
    const char *const *flags = mode == WITHOUT ? CMD("--single-wait") : NO_FLAGS;
    pid_t pids[30];
    int z, i;

    tl_section("%s", UPPER[mode]);
    if (start(robust_log("m1b", mode, "server"), flags) != 0)
        return;
    for (i = 0; i < 30; i++)
        pids[i] = tl_spawn("/dev/null", CMD("./client", "-p", tl_port_str, "-q"));
    wait_all(pids, 30);
    tl_sleep(1);
    puts("30 clients done. Zombie children 1 s later (first lines):");
    fputs(tl_head(tl_grep(tl_capture_out(CMD("ps", "-o", "pid,ppid,stat,cmd", "--ppid", server_pid_str())),
                          "^ *PID |^ *[0-9]+ +[0-9]+ +Z", 0), 6), stdout);
    z = tl_zombie_count();
    printf("zombie children: %d | children reaped by the handler: %d/30\n", z,
           tl_file_count(tl_server_log, "reaped child"));
    m1b[mode] = z;
    tl_stop_server();
}

/* [4] 20 idle clients flood from 127.0.0.1, then one client from 127.0.0.2. */
static void m4_run(int mode)
{
    const char *const *flags = mode == WITHOUT ? CMD("--no-limit")
                             : mode == CAP ? CMD("-m", "10") : CMD("-m", "10", "--max-per-ip", "5");
    char *clog = robust_log("m4", mode, "clients");
    char *other_out = tl_fmt("%s/m4_other.txt", g_tmp);
    pid_t pids[20];
    int kids, rej = 0, other, i;
    const char *other_state;

    tl_section("%s: ./server %s", UPPER[mode], tl_join(flags));
    if (start(robust_log("m4", mode, "server"), flags) != 0)
        return;
    for (i = 0; i < 20; i++) {
        pids[i] = tl_spawn(tl_fmt("%s/m4_%d.txt", g_tmp, i + 1),
                           CMD("./client", "-p", tl_port_str, "--mode", "idle", "--hold", "3"));
    }
    puts("$ ./client --mode idle --hold 3   x 20 from 127.0.0.1 (one host floods the server)");
    tl_sleep(1);
    kids = tl_child_count();
    printf("server children 1 s later: %d\n", kids);
    puts("$ ./client --bind 127.0.0.2 -q   (an ordinary client from another address)");
    other = tl_run(other_out, CMD("./client", "-p", tl_port_str, "--bind", "127.0.0.2", "-q"));
    tl_print_lines(tl_read(other_out), "  ", 108);
    wait_all(pids, 20);
    tl_write(clog, "", 0);
    for (i = 0; i < 20; i++) {
        char *out = tl_fmt("%s/m4_%d.txt", g_tmp, i + 1);

        tl_append_prefixed(out, tl_fmt("[flood %d] ", i + 1), clog);
        rej += tl_count(tl_grep(tl_read(out), "RESULT", 0), "error 6");
    }
    tl_append_prefixed(other_out, "[127.0.0.2] ", clog);
    other_state = other == 0 ? "served" : "refused";
    puts("server log:");
    tl_print_lines(tl_head(tl_grep(tl_read(tl_server_log), "rejected", 0), 2), "  ", 0);
    printf("flood children: %d | flood clients refused (ERR_BUSY): %d/20 | 127.0.0.2: %s\n",
           kids, rej, other_state);
    m4_kids[mode] = kids;
    m4_rej[mode] = rej;
    m4_other[mode] = other_state;
    tl_stop_server();
}

/* [5] A request dripped one byte every 3 s: each byte beats the 5 s timeout. */
static void m5_run(int mode)
{
    const char *const *flags = mode == WITHOUT ? CMD("--no-deadline") : NO_FLAGS;
    char *clog = robust_log("m5", mode, "clients");
    pid_t client;
    int alive;

    tl_section("%s", UPPER[mode]);
    if (start(robust_log("m5", mode, "server"), flags) != 0)
        return;
    puts("$ ./client --mode slow --hold 9   (16-byte request, 1 byte every 3 s)");
    client = tl_spawn(clog, CMD("./client", "-p", tl_port_str, "--mode", "slow", "--hold", "9"));
    tl_sleep(6.5);
    alive = tl_child_count();
    puts("6.5 s after the first byte, server children:");
    show_children();
    tl_wait(client);
    puts("client output:");
    tl_print_lines(tl_read(clog), "  ", 0);
    puts("server log:");
    show_log("deadline|reaped child");
    m5_alive[mode] = alive;
    m5_cut[mode] = tl_file_count(clog, "error 8");
    tl_stop_server();
}

/* [6] TCP is a byte stream: one request split in 8, five glued into one. */
static void m6_run(int mode)
{
    const char *const *flags = mode == WITHOUT ? CMD("--naive-recv") : NO_FLAGS;
    char *clog = robust_log("m6", mode, "clients");
    char *text;
    char *result;

    tl_section("%s", UPPER[mode]);
    if (start(robust_log("m6", mode, "server"), flags) != 0)
        return;
    tl_write(clog, "$ ./client --mode fragment       (1 request as 8 one-byte segments)\n", 0);
    tl_wait(tl_spawn_opts(NULL, clog, 1, CMD("./client", "-p", tl_port_str, "--mode", "fragment")));
    tl_write(clog, "$ ./client --mode pipeline -q    (5 requests glued into one write)\n", 1);
    tl_wait(tl_spawn_opts(NULL, clog, 1, CMD("./client", "-p", tl_port_str, "--mode", "pipeline", "-q")));
    text = tl_read(clog);
    tl_print_lines(text, "  ", 0);
    puts("server log:");
    show_log("naive recv|closed the connection");
    m6_frag[mode] = tl_count(text, "were reassembled");
    result = tl_match(text, "RESULT: [0-9]*/5");
    m6_pipe[mode] = result ? (int)strtol(result + strlen("RESULT: "), NULL, 10) : UNSET;
    tl_stop_server();
}

/* [7] An endless subscriber freezes (SIGSTOP, like Ctrl+Z) for 5 s. */
static void m7_run(int mode)
{
    const char *const *flags = mode == WITHOUT ? CMD("--no-heartbeat") : NO_FLAGS;
    char *clog = robust_log("m7", mode, "clients");
    pid_t client;
    int alive;

    tl_section("%s", UPPER[mode]);
    if (start(robust_log("m7", mode, "server"), flags) != 0)
        return;
    puts("$ ./client -w 100 --no-reconnect   (endless subscription, one ACK per push)");
    client = tl_spawn(clog, CMD("./client", "-p", tl_port_str, "-w", "100", "--no-reconnect"));
    tl_sleep(1);
    kill(client, SIGSTOP);
    printf("$ kill -STOP %d   (the client freezes like Ctrl+Z; its kernel still takes data)\n", (int)client);
    tl_sleep(5);
    alive = tl_child_count();
    puts("after 5 s frozen, server children:");
    show_children();
    kill(client, SIGCONT);
    printf("$ kill -CONT %d   (the client resumes and reads what piled up)\n", (int)client);
    tl_sleep(1);
    kill(client, SIGTERM);
    tl_wait(client);
    printf("client: %d snapshot lines; last lines:\n", tl_file_count(clog, "^seq="));
    tl_print_lines(tl_tail(tl_read(clog), 3), "  ", 110);
    puts("server log:");
    show_log("subscribe|no ACK|reaped child");
    m7_alive[mode] = alive;
    m7_hb[mode] = tl_file_count(clog, "server error 7");
    tl_stop_server();
}

/* [8] The server goes away for 2 s while a client is subscribed. */
static void m8_run(int mode)
{
    const char *const *argv = mode == WITHOUT
        ? CMD("./client", "-p", tl_port_str, "-w", "300", "--no-reconnect")
        : CMD("./client", "-p", tl_port_str, "-w", "300");
    char *clog = robust_log("m8", mode, "clients");
    pid_t distinct[MAX_PIDS];
    const char *state;
    pid_t client;
    int npids;

    tl_section("%s", UPPER[mode]);
    if (start(robust_log("m8", mode, "server1"), NO_FLAGS) != 0)
        return;
    printf("$ ./client -w 300 %s   (background subscriber)\n", mode == WITHOUT ? "--no-reconnect" : "");
    client = tl_spawn(clog, argv);
    tl_sleep(1);
    puts("stopping the server (crash or upgrade), starting a new one 2 s later");
    tl_stop_server();
    tl_sleep(2);
    if (start(robust_log("m8", mode, "server2"), NO_FLAGS) != 0)
        return;
    tl_sleep(4);
    if (tl_running(client)) {
        state = "running";
        kill(client, SIGTERM);
    } else {
        state = "exited";
    }
    tl_wait(client);
    puts("client status lines:");
    tl_print_lines(tl_grep(tl_read(clog), "^seq=", 1), "  ", 0);
    npids = tl_distinct_pids(tl_read(clog), distinct, MAX_PIDS);
    printf("client 4 s after the restart: %s | server children that served it: %d\n", state, npids);
    m8_state[mode] = state;
    m8_pids[mode] = npids;
    tl_stop_server();
}

/* [9] kill -9 of the parent while 3 children are busy. */
static void m9_run(int mode)
{
    const char *const *flags = mode == WITHOUT ? CMD("-t", "30", "--no-pdeathsig") : CMD("-t", "30");
    pid_t pids[3];
    pid_t kids[MAX_PIDS];
    char *list = strdup("");
    char *comma_list = strdup("");
    int nkids, left = 0, i;

    tl_section("%s", UPPER[mode]);
    if (start(robust_log("m9", mode, "server"), flags) != 0)
        return;
    for (i = 0; i < 3; i++) {
        pids[i] = tl_spawn("/dev/null", CMD("./client", "-p", tl_port_str, "--mode", "idle", "--hold", "4"));
    }
    puts("$ ./client --mode idle --hold 4   x 3 (three busy children)");
    tl_sleep(0.5);
    nkids = tl_children(tl_server_pid, kids, MAX_PIDS, NULL);
    for (i = 0; i < nkids; i++) {
        list = tl_fmt("%s%s%d", list, i ? " " : "", (int)kids[i]);
        comma_list = tl_fmt("%s%s%d", comma_list, i ? "," : "", (int)kids[i]);
    }
    printf("server parent %d, children: %s\n", (int)tl_server_pid, list);
    printf("$ kill -9 %d   (SIGKILL cannot be caught: no cleanup code runs)\n", (int)tl_server_pid);
    kill(tl_server_pid, SIGKILL);
    tl_wait(tl_server_pid);
    tl_server_pid = 0;
    tl_sleep(0.5);
    puts("the children afterwards:");
    if (nkids > 0) {
        tl_print_lines(tl_capture_out(CMD("ps", "-o", "pid,ppid,stat,cmd", "-p", comma_list)), "  ", 0);
        left = tl_count(tl_capture_out(CMD("ps", "-o", "pid=", "-p", comma_list)), ".");
        for (i = 0; i < nkids; i++)
            kill(kids[i], SIGTERM);
    }
    printf("children still alive: %d (an orphan's PPID is no longer the server)\n", left);
    m9[mode] = left;
    wait_all(pids, 3);
}

static void row(const char *mechanism, const char *without, const char *with)
{
    printf("%-38s| %-38s| %s\n", mechanism, without, with);
}

static void repeat(char c, int n)
{
    while (n-- > 0)
        putchar(c);
}

static void summary(void)
{
    tl_banner("Summary: WITHOUT vs WITH");
    row(" Mechanism", "WITHOUT (switched off)", "WITH (default)");
    repeat('-', 38);
    putchar('+');
    repeat('-', 39);
    putchar('+');
    repeat('-', 39);
    putchar('\n');
    row(" [1]  SIGCHLD + waitpid(WNOHANG)",
        tl_fmt("%s zombie children", v(m1[WITHOUT])), tl_fmt("%s zombie children", v(m1[WITH])));
    row(" [1b] waitpid loop (signals coalesce)",
        tl_fmt("%s zombies after 30 exits", v(m1b[WITHOUT])),
        tl_fmt("%s zombies after 30 exits", v(m1b[WITH])));
    row(" [2]  SIG_IGN(SIGPIPE) + EPIPE check",
        tl_fmt("%s children killed by SIGPIPE", v(m2_killed[WITHOUT])),
        tl_fmt("%s children exit 4 after EPIPE", v(m2_handled[WITH])));
    row(" [3]  header check + SO_RCVTIMEO",
        tl_fmt("%s children stuck, %s unanswered", v(m3_stuck[WITHOUT]), v(m3_noreact[WITHOUT])),
        tl_fmt("%s children left, %s/3 answered", v(m3_stuck[WITH]), v(m3_ok[WITH])));
    row(" [4]  connection limit + per IP",
        tl_fmt("%s children for one host", v(m4_kids[WITHOUT])),
        tl_fmt("%s children, 127.0.0.2 %s", v(m4_kids[WITH]), m4_other[WITH]));
    row(" [5]  whole-request deadline",
        tl_fmt("%s child still waiting at 6.5 s", v(m5_alive[WITHOUT])),
        tl_fmt("%s children, cut off at 5 s (error 8)", v(m5_alive[WITH])));
    row(" [6]  stream framing (read_n)",
        tl_fmt("split: %s/1 ok, glued: %s/5", v(m6_frag[WITHOUT]), v(m6_pipe[WITHOUT])),
        tl_fmt("split: %s/1 ok, glued: %s/5", v(m6_frag[WITH]), v(m6_pipe[WITH])));
    row(" [7]  heartbeat (ACK per push)",
        tl_fmt("%s child pushing to a frozen client", v(m7_alive[WITHOUT])),
        tl_fmt("%s children, ended at 3 s (error 7)", v(m7_alive[WITH])));
    row(" [8]  client reconnect + backoff",
        tl_fmt("client %s", m8_state[WITHOUT]),
        tl_fmt("client %s, %s server pids", m8_state[WITH], v(m8_pids[WITH])));
    row(" [9]  PR_SET_PDEATHSIG",
        tl_fmt("%s orphans after kill -9", v(m9[WITHOUT])),
        tl_fmt("%s orphans after kill -9", v(m9[WITH])));
}

static int run(void)
{
    tl_banner("Robustness test: WITHOUT vs WITH");
    printf("date      : %s\n", tl_date());
    printf("kernel    : %s\n", tl_kernel());
    printf("compiler  : %s\n", tl_compiler());
    printf("overcommit: vm.overcommit_memory=%s", tl_read("/proc/sys/vm/overcommit_memory"));
    if (tl_run(NULL, CMD("make", "-s", "all")) != 0)
        return 1;
    if (tl_port_in_use(tl_port)) {
        printf("port %d is already in use; stop the other server or set PORT=...\n", tl_port);
        return 1;
    }

    tl_banner("[1] Zombie prevention: SIGCHLD handler + waitpid(-1, ..., WNOHANG)");
    puts("Scenario: 12 clients each make one request and leave.");
    m1_run(WITHOUT);
    m1_run(WITH);
    tl_section("Checks [1]");
    tl_expect(m1[WITHOUT] == 12, "without: all 12 finished children stay as zombies");
    tl_expect(m1[WITH] == 0, "with: 0 zombies, every child reaped");
    tl_expect(m1_reaped[WITH] == 12, "with: server log reports all 12 reaped children");

    tl_banner("[2] SIGPIPE: set_signal(SIGPIPE, SIG_IGN) + EPIPE/ECONNRESET in write_n");
    puts("Scenario: 3 clients subscribe (push every 200 ms) and disconnect after 1 push.");
    m2_run(WITHOUT);
    m2_run(WITH);
    tl_section("Checks [2]");
    tl_expect(m2_killed[WITHOUT] == 3, "without: 3 children killed by signal 13 (SIGPIPE)");
    tl_expect(m2_killed[WITH] == 0, "with: 0 children killed by SIGPIPE");
    tl_expect(m2_handled[WITH] == 3, "with: 3 children saw EPIPE and exited cleanly (exit 4)");
    tl_expect(m2_next[WITH] == 0, "with: the server keeps serving new clients");

    tl_banner("[3] Bad input: header validation (magic 0x4E50) + SO_RCVTIMEO/SO_SNDTIMEO");
    puts("Scenario: garbage = an HTTP request instead of an NP header");
    puts("          biglen  = a valid header claiming body_len = 0xFFFFFFFF");
    puts("          idle    = connect and send nothing");
    puts("          each client keeps its socket open for 8 s, waiting for a reaction");
    m3_run(WITHOUT);
    m3_run(WITH);
    tl_section("Checks [3]");
    tl_expect(m3_stuck[WITHOUT] == 3, "without: 3 children still blocked after 6.5 s");
    tl_expect(m3_noreact[WITHOUT] == 3, "without: no client got any reaction within 8 s");
    tl_expect(m3_stuck[WITH] == 0, "with: 0 children left after 6.5 s");
    tl_expect(m3_ok[WITH] == 3, "with: every client got a reaction (error reply or close)");

    tl_banner("[1b] Signals do not queue: waitpid() in a loop vs once per SIGCHLD");
    puts("Scenario: 30 clients make one request each and leave at nearly the same time.");
    m1b_run(WITHOUT);
    m1b_run(WITH);
    tl_section("Checks [1b]");
    tl_expect(m1b[WITHOUT] > 0, "without: coalesced SIGCHLDs leave zombies behind");
    tl_expect(m1b[WITH] == 0, "with: 0 zombies");

    tl_banner("[4] Connection limit: total cap + per-client-address cap");
    puts("Scenario: 20 idle clients from 127.0.0.1 flood the server, then an ordinary");
    puts("          client connects from 127.0.0.2 (all of 127.0.0.0/8 is this host).");
    m4_run(WITHOUT);
    m4_run(CAP);
    m4_run(WITH);
    tl_section("Checks [4]");
    tl_expect(m4_kids[WITHOUT] == 20, "without: one host gets 20 children, no bound");
    tl_expect(m4_kids[CAP] == 10 && strcmp(m4_other[CAP], "refused") == 0,
              "total cap only: 10 children, but 127.0.0.2 is starved");
    tl_expect(m4_kids[WITH] == 5 && m4_rej[WITH] == 15,
              "with per-IP cap: 5 children, 15 flood clients refused");
    tl_expect(strcmp(m4_other[WITH], "served") == 0, "with per-IP cap: 127.0.0.2 is still served");

    tl_banner("[5] Slow sender: whole-request deadline vs per-read timeout");
    puts("Scenario: a 16-byte request sent one byte every 3 s. Each read() gets its");
    puts("          byte before the 5 s SO_RCVTIMEO, so only a deadline can stop it.");
    m5_run(WITHOUT);
    m5_run(WITH);
    tl_section("Checks [5]");
    tl_expect(m5_alive[WITHOUT] == 1, "without: the child is still waiting after 6.5 s");
    tl_expect(m5_alive[WITH] == 0 && m5_cut[WITH] == 1, "with: the child cut the sender off (error 8) and exited");

    tl_banner("[6] TCP is a byte stream: split and glued messages");
    puts("Scenario: fragment = one request in 8 one-byte segments, 100 ms apart");
    puts("          pipeline = 5 requests glued into one write()");
    m6_run(WITHOUT);
    m6_run(WITH);
    tl_section("Checks [6]");
    tl_expect(m6_frag[WITHOUT] == 0 && m6_pipe[WITHOUT] == 1,
              "without: the split request is rejected, 4 of 5 glued requests lost");
    tl_expect(m6_frag[WITH] == 1 && m6_pipe[WITH] == 5,
              "with: the split request is reassembled, 5/5 glued requests answered");

    tl_banner("[7] Frozen client: heartbeat (one ACK per push)");
    puts("Scenario: an endless subscriber is frozen with SIGSTOP (as Ctrl+Z) for 5 s.");
    m7_run(WITHOUT);
    m7_run(WITH);
    tl_section("Checks [7]");
    tl_expect(m7_alive[WITHOUT] == 1, "without: the child keeps pushing to the frozen client");
    tl_expect(m7_alive[WITH] == 0 && m7_hb[WITH] == 1, "with: the child ended the subscription (error 7)");

    tl_banner("[8] Server restart: client reconnect with exponential backoff");
    puts("Scenario: the server stops while a client is subscribed and is back 2 s later.");
    m8_run(WITHOUT);
    m8_run(WITH);
    tl_section("Checks [8]");
    tl_expect(strcmp(m8_state[WITHOUT], "exited") == 0, "without: the client gave up when the server left");
    tl_expect(strcmp(m8_state[WITH], "running") == 0 && m8_pids[WITH] == 2,
              "with: the client reconnected and was served by the new server");

    tl_banner("[9] kill -9 of the parent: PR_SET_PDEATHSIG");
    puts("Scenario: 3 busy children, then the parent is killed with SIGKILL.");
    m9_run(WITHOUT);
    m9_run(WITH);
    tl_section("Checks [9]");
    tl_expect(m9[WITHOUT] == 3, "without: 3 orphaned children keep running");
    tl_expect(m9[WITH] == 0, "with: no child outlives the parent");

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
