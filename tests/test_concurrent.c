/*
 * test_concurrent.c - proof that the fork() server serves 12 clients at once.
 *
 *   1. start ./server
 *   2. launch 12 ./client processes at the same moment; each keeps its
 *      connection open for HOLD seconds after its reply
 *   3. while all are connected: ps aux snapshot, process tree (PPID) and the
 *      established TCP connections
 *   4. cross-check: the server child PID each client reports must be one of
 *      the children seen in ps, and all 12 must be distinct
 *   5. after the clients leave: no zombies; bonus --stress run with latency
 *
 * Usage : make test-concurrent        (env: PORT=9090 CLIENTS=12 HOLD=4)
 * Output: logs/test_concurrent.log, logs/concurrent_server.log,
 *         logs/concurrent_clients.log
 */
#include "testlib.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CLIENTS 256
#define PS_ROW "^([^[:space:]]+[[:space:]]+){10}"

static int run(int clients, int hold, const char *tmp)
{
    const char *server_log = "logs/concurrent_server.log";
    const char *client_log = "logs/concurrent_clients.log";
    int width = snprintf(NULL, 0, "%d", clients);
    char *hold_str = tl_fmt("%d", hold);
    char *server_pid;
    char *snapshot;
    char *served_text;
    pid_t pids[MAX_CLIENTS];
    pid_t kids[MAX_CLIENTS];
    pid_t served[MAX_CLIENTS];
    int ok = 0, nserver, nchild, established, ndistinct, unknown = 0;
    int zombies, left, accepted, reaped, stress_rc, status, i, j;

    tl_heading("Environment");
    printf("date     : %s\n", tl_date());
    printf("kernel   : %s\n", tl_kernel());
    printf("compiler : %s\n", tl_compiler());
    printf("test     : %d concurrent clients, each holds its connection %d s\n", clients, hold);

    tl_heading("Build (make)");
    if (tl_run(NULL, CMD("make", "-s", "all")) != 0)
        return 1;
    fputs(tl_grep(tl_capture_out(CMD("ldd", "./server", "./client")), "^\\./|libutils", 0), stdout);
    if (tl_port_in_use(tl_port)) {
        printf("port %d is already in use; stop the other server or set PORT=...\n", tl_port);
        return 1;
    }

    tl_heading("Start server");
    if (tl_start_server(NULL, server_log, NO_FLAGS) != 0)
        return 1;
    server_pid = tl_fmt("%d", (int)tl_server_pid);
    printf("$ ./server -p %d   -> parent PID %s (log: %s)\n", tl_port, server_pid, server_log);

    tl_heading("Launch %d clients at the same moment", clients);
    for (i = 0; i < clients; i++) {
        pids[i] = tl_spawn(tl_fmt("%s/client_%0*d.txt", tmp, width, i + 1),
                           CMD("./client", "-p", tl_port_str, "-q", "--hold", hold_str));
    }
    printf("$ ./client -p %d -q --hold %d   x %d (background)\n", tl_port, hold, clients);
    fputs("client PIDs:", stdout);
    for (i = 0; i < clients; i++)
        printf(" %d", (int)pids[i]);
    putchar('\n');
    tl_sleep(1.5);

    tl_heading("ps aux while all %d clients are connected", clients);
    snapshot = tl_grep(tl_capture_out(CMD("ps", "auxww")),
                       "^USER |" PS_ROW "\\./(server|client)([[:space:]]|$)", 0);
    fputs(snapshot, stdout);
    nserver = tl_count(snapshot, PS_ROW "\\./server([[:space:]]|$)");

    tl_heading("Server process tree (every child's PPID = %s)", server_pid);
    tl_run(NULL, CMD("ps", "--forest", "-o", "pid,ppid,stat,etime,cmd", "-p", server_pid,
                     "--ppid", server_pid));
    nchild = tl_children(tl_server_pid, kids, MAX_CLIENTS, NULL);
    established = tl_established(tl_port);
    printf("server children alive           : %d\n", nchild);
    printf("established TCP conns on :%d  : %d\n", tl_port, established);

    tl_heading("Client results (pid= is the server child that answered)");
    for (i = 0; i < clients; i++) {
        if (tl_wait(pids[i]) == 0)
            ok++;
    }
    tl_write(client_log, "", 0);
    for (i = 0; i < clients; i++) {
        tl_append_prefixed(tl_fmt("%s/client_%0*d.txt", tmp, width, i + 1),
                           tl_fmt("[client %0*d] ", width, i + 1), client_log);
    }
    served_text = tl_read(client_log);
    fputs(served_text, stdout);
    ndistinct = tl_distinct_pids(served_text, served, MAX_CLIENTS);
    for (i = 0; i < ndistinct; i++) {
        for (j = 0; j < nchild && kids[j] != served[i]; j++)
            ;
        if (j == nchild)
            unknown++;
    }
    printf("clients exited 0: %d / %d, distinct server PIDs: %d\n", ok, clients, ndistinct);

    tl_sleep(1);
    left = tl_children(tl_server_pid, NULL, 0, &zombies);
    accepted = tl_file_count(server_log, "accepted");
    reaped = tl_file_count(server_log, "reaped child");
    printf("after the clients left: %d child process(es), %d zombie(s)\n", left, zombies);
    printf("server log: %d accepted, %d reaped\n", accepted, reaped);

    tl_heading("Bonus: built-in stress mode (100 worker processes x 10 requests)");
    printf("$ ./client -p %d --stress 100 -n 10\n", tl_port);
    stress_rc = tl_run(NULL, CMD("./client", "-p", tl_port_str, "--stress", "100", "-n", "10"));

    tl_heading("Stop server (SIGTERM)");
    kill(tl_server_pid, SIGTERM);
    status = tl_wait(tl_server_pid);
    printf("server exit status: %d\n", status);
    tl_server_pid = 0;
    fputs(tl_tail(tl_read(server_log), 1), stdout);

    tl_heading("Checks");
    tl_expect(nchild == clients, "%d server children alive at the same time (ps --ppid)", clients);
    tl_expect(nserver == clients + 1, "ps aux shows 1 parent + %d children = %d ./server processes",
              clients, clients + 1);
    tl_expect(established == clients, "%d established TCP connections at the same time", clients);
    tl_expect(ok == clients, "all %d clients exited successfully", clients);
    tl_expect(ndistinct == clients, "clients were answered by %d distinct server PIDs", clients);
    tl_expect(unknown == 0, "every answering PID is a child seen in the ps snapshot");
    tl_expect(zombies + left == 0, "no zombie or leftover children after the clients left");
    tl_expect(accepted == clients && reaped == clients,
              "server log: all %d accepted children reported as reaped", clients);
    tl_expect(stress_rc == 0, "stress run: all 1000 requests succeeded");

    tl_heading("Result");
    return tl_print_result();
}

int main(void)
{
    int clients, hold, rc;
    char *tmp;

    tl_init(9090);
    clients = tl_env_int("CLIENTS", 12);
    hold = tl_env_int("HOLD", 4);       /* must stay below the server's 5 s timeout */
    if (clients > MAX_CLIENTS)
        clients = MAX_CLIENTS;
    tl_tee("logs/test_concurrent.log");
    tmp = tl_mktemp("np_concurrent");
    rc = run(clients, hold, tmp);
    tl_stop_server();
    tl_rmrf(tmp);
    return tl_finish(rc);
}
