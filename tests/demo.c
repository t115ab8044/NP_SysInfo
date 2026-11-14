/*
 * demo.c - one-command live demo of the whole project (about 3 seconds).
 *
 *   make demo
 *
 *   1. both binaries are linked against libutils.so
 *   2. start the fork() server in the background
 *   3. one client requests system information (SYSINFO)
 *   4. CLIENTS clients connect at the same time -> CLIENTS fork()ed children
 *   5. robustness: an unknown request is rejected with an ERROR line
 *   6. stop the server; every child was reaped (no zombies)
 *
 * env: PORT=9090 (the next free port is used if it is busy) CLIENTS=12
 */
#include "testlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_CLIENTS 256

static const char *B = "";
static const char *G = "";
static const char *R = "";
static const char *N = "";
static char *g_tmp;

static void step(const char *title)
{
    printf("\n%s== %s ==%s\n", B, title, N);
}

static int run(void)
{
    int clients = tl_env_int("CLIENTS", 12);
    pid_t pids[MAX_CLIENTS];
    pid_t distinct[MAX_CLIENTS];
    char *server_log, *server_pid;
    char *all_output = strdup("");
    long long t0, elapsed;
    int nchild, ok = 0, ndistinct, zombies, accepted, reaped, i;

    if (clients > MAX_CLIENTS)
        clients = MAX_CLIENTS;
    if (tl_run(NULL, CMD("make", "-s", "all")) != 0)
        return 1;
    g_tmp = tl_mktemp("np_demo");
    server_log = tl_fmt("%s/server.log", g_tmp);
    while (tl_port_in_use(tl_port))
        tl_set_port(tl_port + 1);
    t0 = tl_now_ms();

    step("1. libutils.so is shared by server and client");
    printf("  ./%-7s %s\n", "server",
           tl_match(tl_capture_out(CMD("ldd", "./server")), "libutils\\.so => [^ ]*"));
    printf("  ./%-7s %s\n", "client",
           tl_match(tl_capture_out(CMD("ldd", "./client")), "libutils\\.so => [^ ]*"));

    step("2. start the fork() server");
    if (tl_start_server(NULL, server_log, NO_FLAGS) != 0)
        return 1;
    server_pid = tl_fmt("%d", (int)tl_server_pid);
    printf("  ./server -p %d   -> parent PID %s\n", tl_port, server_pid);

    step("3. client requests system information");
    printf("  $ ./client -p %d\n", tl_port);
    fputs(tl_capture_out(CMD("./client", "-p", tl_port_str)), stdout);

    printf("\n%s== 4. %d clients at the same time (WATCH 6 500: 6 snapshots in 3 s) ==%s\n",
           B, clients, N);
    for (i = 0; i < clients; i++) {
        pids[i] = tl_spawn(tl_fmt("%s/client_%d", g_tmp, i + 1),
                           CMD("./client", "-p", tl_port_str, "-n", "6", "-w", "500"));
    }
    for (i = 0; i < 100 && tl_child_count() < clients; i++)
        tl_sleep(0.02);
    tl_run(NULL, CMD("ps", "--forest", "-o", "pid,ppid,stat,cmd", "-p", server_pid,
                     "--ppid", server_pid));
    nchild = tl_child_count();
    for (i = 0; i < clients; i++) {
        if (tl_wait(pids[i]) == 0)
            ok++;
    }
    for (i = 0; i < clients; i++)
        all_output = tl_fmt("%s%s", all_output, tl_read(tl_fmt("%s/client_%d", g_tmp, i + 1)));
    ndistinct = tl_distinct_pids(all_output, distinct, MAX_CLIENTS);

    step("5. robustness: an unknown request is rejected");
    printf("  $ ./client -p %d -r HELLO\n", tl_port);
    fputs(tl_capture_out(CMD("./client", "-p", tl_port_str, "-r", "HELLO")), stdout);

    tl_sleep(0.2);
    zombies = tl_zombie_count();
    tl_stop_server();
    accepted = tl_file_count(server_log, "accepted");
    reaped = tl_file_count(server_log, "reaped child");
    elapsed = tl_now_ms() - t0;

    step("Result");
    printf("  fork() children alive at once : %d / %d\n", nchild, clients);
    printf("  distinct server PIDs answered : %d / %d\n", ndistinct, clients);
    printf("  clients exited OK             : %d / %d\n", ok, clients);
    printf("  server log                    : %d accepted, %d reaped, %d zombies\n",
           accepted, reaped, zombies);
    if (nchild == clients && ndistinct == clients && ok == clients && zombies == 0 &&
        accepted == reaped) {
        printf("%s  ALL OK%s  (%lld.%02lld s)\n", G, N, elapsed / 1000, elapsed % 1000 / 10);
        return 0;
    }
    printf("%s  CHECK FAILED%s  (%lld.%02lld s)\n", R, N, elapsed / 1000, elapsed % 1000 / 10);
    return 1;
}

int main(void)
{
    int rc;

    tl_init(9090);
    if (isatty(STDOUT_FILENO)) {
        B = "\033[1;36m";
        G = "\033[1;32m";
        R = "\033[1;31m";
        N = "\033[0m";
    }
    rc = run();
    tl_stop_server();
    if (g_tmp)
        tl_rmrf(g_tmp);
    return tl_finish(rc);
}
