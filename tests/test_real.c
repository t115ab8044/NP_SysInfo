/*
 * test_real.c - Real Test that needs only one physical computer.
 *
 *   Part 1  Windows host <-> WSL2 VM. np_client_win.c, built with MinGW-w64, runs
 *           on Windows and talks to ./server inside the VM: two operating systems,
 *           two kernels, two IP addresses, a virtual Ethernet link. Wireshark's
 *           dumpcap records it.
 *   Part 2  Two network stacks joined by a veth "cable" (unprivileged Linux
 *           namespaces). tc netem adds delay and loss, then the link goes down.
 *
 * Limit: everything still runs on one physical machine. Part 1 is a real
 * host-to-VM connection; the faults of part 2 are simulated.
 *
 * Usage : make test-real        (env: PORT=9190)
 * Output: logs/test_real.log, logs/real_windows_*.log, logs/real_windows_wsl.pcapng,
 *         logs/real_net_server.log, logs/real_net_client.log, logs/real_net.pcap
 */
#include "testlib.h"

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MINGW_CC "x86_64-w64-mingw32-gcc"
#define DUMPCAP "/mnt/c/Program Files/Wireshark/dumpcap.exe"
#define TSHARK "/mnt/c/Program Files/Wireshark/tshark.exe"
#define SERVER_IP "10.99.0.1"
#define PEER_IP "10.99.0.2"
#define NP_FILTER "tcp.payload[0:2] == 4e:50"

#define PEER(...) CMD("nsenter", "-t", g_peer_ns, "-n", __VA_ARGS__)

static char g_peer_ns[16];
static pid_t g_peer_pid;
static pid_t g_tcpdump_pid;

static int contains(const char *path, const char *needle)
{
    return strstr(tl_read(path), needle) != NULL;
}

static char *first_line(char *text)
{
    text[strcspn(text, "\r\n")] = '\0';
    return text;
}

/* Part 1: Windows host -> WSL2 VM */
static int part_windows(void)
{
    const char *server_log = "logs/real_windows_server.log";
    const char *client_log = "logs/real_windows_client.log";
    const char *cap = "logs/real_windows_wsl.pcapng";
    char cwd[PATH_MAX];
    char *exe, *eth0, *wsl_ip, *iface = NULL, *win_cap = NULL, *win_ip;
    pid_t cli, cap_pid = 0;
    int max = 0, rc, cap_rc = 0;

    tl_heading("Part 1: Windows host -> WSL2 VM (not loopback)");
    if (!tl_which("cmd.exe")) {
        puts("SKIP: needs WSL with access to Windows programs");
        return 0;
    }
    if (!tl_which(MINGW_CC)) {
        printf("SKIP: %s not found (sudo apt install gcc-mingw-w64-x86-64)\n", MINGW_CC);
        return 0;
    }
    if (!getcwd(cwd, sizeof(cwd)))
        return 1;
    exe = tl_fmt("%s/tests/np_client_win.exe", cwd);
    if (tl_run(NULL, CMD(MINGW_CC, "-Wall", "-Wextra", "-pedantic", "-std=gnu11", "-O2", "-static",
                         "-o", exe, "tests/np_client_win.c", "-lws2_32")) != 0)
        return 1;
    eth0 = tl_capture_out(CMD("ip", "-4", "-o", "addr", "show", "eth0"));
    wsl_ip = tl_match(eth0, "[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+");
    if (!wsl_ip)
        return 1;
    printf("server: %s at %s:%d\n", tl_kernel(), wsl_ip, tl_port);

    if (tl_start_server(NULL, server_log, NO_FLAGS) != 0)
        return 1;

    if (access(DUMPCAP, X_OK) == 0) {
        char *lines = tl_grep(tl_strip_cr(tl_capture_out(CMD(DUMPCAP, "-D"))), "vEthernet \\(WSL", 0);

        if (*lines) {
            iface = strdup(lines);
            iface[strcspn(iface, ".")] = '\0';
        }
    }
    if (iface) {
        win_cap = first_line(tl_capture_out(CMD("wslpath", "-w", tl_fmt("%s/%s", cwd, cap))));
        cap_pid = tl_spawn("/dev/null", CMD(DUMPCAP, "-q", "-i", iface, "-f", tl_fmt("tcp port %d", tl_port),
                                            "-a", "duration:9", "-w", win_cap));
        tl_sleep(2);
    } else {
        puts("Wireshark capture interface not found: skipping the capture");
    }

    cli = tl_spawn_opts("/mnt/c", client_log, 0, CMD(exe, wsl_ip, tl_port_str, "3"));
    while (tl_running(cli)) {
        int n = tl_child_count();

        if (n > max)
            max = n;
        tl_sleep(0.1);
    }
    rc = tl_wait(cli);
    tl_write(client_log, tl_strip_cr(tl_read(client_log)), 0);
    tl_print_lines(tl_read(client_log), "  [windows] ", 0);
    tl_stop_server();
    if (cap_pid)
        cap_rc = tl_wait(cap_pid);

    win_ip = tl_match(tl_read(client_log), "^local=[0-9.]*");
    win_ip = win_ip ? win_ip + strlen("local=") : strdup("");
    printf("children seen by ps at the same time: %d\n", max);
    fputs(tl_head(tl_grep(tl_read(server_log), "accepted", 0), 2), stdout);
    fputs(tl_tail(tl_read(server_log), 1), stdout);

    tl_expect(rc == 0, "Windows client finished without error");
    tl_expect(contains(server_log, tl_fmt("accepted %s:", win_ip)),
              "server saw the Windows address %s, not loopback", win_ip);
    tl_expect(tl_file_count(server_log, "accepted 127\\.0\\.0\\.1") == 0, "no connection came from 127.0.0.1");
    tl_expect(contains(client_log, "concurrent=12 distinct_pids=12"), "12 connections answered by 12 distinct children");
    tl_expect(max >= 12, "at least 12 children alive at once in ps");
    tl_expect(contains(client_log, "error=1"), "garbage from Windows rejected with error 1");
    tl_expect(contains(server_log, "exit 4"), "RST from Windows handled: child exit 4");
    tl_expect(contains(server_log, " 0 child(ren) not reaped"), "every child reaped");

    if (iface && access(TSHARK, X_OK) == 0) {
        int np_frames = tl_nlines(tl_capture_out(CMD(TSHARK, "-r", win_cap, "-Y", NP_FILTER)));
        int syn_frames = tl_nlines(tl_capture_out(CMD(TSHARK, "-r", win_cap, "-Y",
                                                      "tcp.flags.syn == 1 && tcp.flags.ack == 0")));

        printf("Wireshark capture: %d frames carrying NP, %d connection attempts\n", np_frames, syn_frames);
        tl_print_lines(tl_head(tl_strip_cr(tl_capture_out(CMD(TSHARK, "-r", win_cap, "-Y", NP_FILTER, "-T", "fields",
                                                              "-e", "ip.src", "-e", "ip.dst", "-e", "tcp.len",
                                                              "-e", "tcp.payload"))), 2), "  ", 70);
        tl_expect(cap_rc == 0 && np_frames > 0, "Wireshark recorded NP frames between host and VM");
    }
    return 0;
}

static void inner_cleanup(void)
{
    if (g_peer_pid > 0)
        kill(g_peer_pid, SIGTERM);
    if (g_tcpdump_pid > 0)
        kill(g_tcpdump_pid, SIGTERM);
    tl_stop_server();
}

static int setup_network(void)
{
    tl_run(NULL, CMD("ip", "link", "set", "lo", "up"));
    g_peer_pid = tl_spawn("/dev/null", CMD("unshare", "-n", "sleep", "90"));
    snprintf(g_peer_ns, sizeof(g_peer_ns), "%d", (int)g_peer_pid);
    tl_sleep(0.3);
    if (tl_run(NULL, CMD("ip", "link", "add", "v0", "type", "veth", "peer", "name", "v1")) != 0)
        return -1;
    if (tl_run(NULL, CMD("ip", "link", "set", "v1", "netns", g_peer_ns)) != 0)
        return -1;
    tl_run(NULL, CMD("ip", "addr", "add", SERVER_IP "/24", "dev", "v0"));
    tl_run(NULL, CMD("ip", "link", "set", "v0", "up"));
    tl_run(NULL, PEER("ip", "link", "set", "lo", "up"));
    tl_run(NULL, PEER("ip", "addr", "add", PEER_IP "/24", "dev", "v1"));
    tl_run(NULL, PEER("ip", "link", "set", "v1", "up"));
    tl_sleep(0.5);
    return 0;
}

/* The average of "rtt min/avg/max/mdev = a/b/c/d ms" (fifth '/' field). */
static char *rtt_average(const char *rtt)
{
    const char *p = rtt;
    int i;

    for (i = 0; i < 4 && p; i++) {
        p = strchr(p, '/');
        if (p)
            p++;
    }
    return p ? strndup(p, strcspn(p, "/")) : strdup("0");
}

static int count_after_reconnect(const char *text)
{
    const char *from = strstr(text, "reconnected to");

    return from ? tl_count(from, "^seq=") : 0;
}

/* Part 2: runs inside `unshare -Urn`, this process's namespace is the server side */
static int part_net_inner(void)
{
    const char *server_log = "logs/real_net_server.log";
    const char *client_log = "logs/real_net_client.log";
    const char *pcap = "logs/real_net.pcap";
    char *filter = tl_fmt("tcp port %d or arp", tl_port);
    const char *syn = "tcp[13] & 2 == 2";
    char *rtt, *avg, *stress;
    pid_t watch;
    int seq_after, narp, nsyn;

    if (setup_network() != 0)
        return 1;
    if (tl_start_server(NULL, server_log, NO_FLAGS) != 0)
        return 1;
    unlink(pcap);
    g_tcpdump_pid = tl_spawn("/dev/null", CMD("tcpdump", "-i", "v0", "-nn", "-U", "-Z", "root", "-w", pcap, filter));
    tl_sleep(1);

    puts("--- clean link");
    tl_print_lines(tl_capture(PEER("./client", "-H", SERVER_IP, "-p", tl_port_str, "-q")), "", 90);
    tl_expect(contains(server_log, "accepted " PEER_IP ":"), "client in another network stack is served");

    puts("--- 50 ms each way, 5 % packet loss");
    if (tl_run(NULL, CMD("tc", "qdisc", "add", "dev", "v0", "root", "netem", "delay", "50ms", "loss", "5%")) != 0)
        return 1;
    if (tl_run(NULL, PEER("tc", "qdisc", "add", "dev", "v1", "root", "netem", "delay", "50ms", "loss", "5%")) != 0)
        return 1;
    rtt = tl_tail(tl_capture_out(PEER("ping", "-c", "5", "-q", SERVER_IP)), 1);
    fputs(rtt, stdout);
    avg = rtt_average(rtt);
    stress = tl_capture(PEER("./client", "-H", SERVER_IP, "-p", tl_port_str, "--stress", "12", "-n", "5"));
    fputs(tl_tail(stress, 5), stdout);
    tl_run(NULL, CMD("tc", "qdisc", "del", "dev", "v0", "root"));
    tl_run(NULL, PEER("tc", "qdisc", "del", "dev", "v1", "root"));
    tl_expect(strtod(avg, NULL) >= 100, "netem really delays the link (RTT %s ms)", avg);
    tl_expect(strstr(stress, "Requests     : 60 ok, 0 failed") != NULL, "all 60 requests survive delay and loss");

    puts("--- link cut for 6 s during a subscription");
    watch = tl_spawn(client_log, PEER("./client", "-H", SERVER_IP, "-p", tl_port_str, "-w", "500", "-q", "-t", "3"));
    tl_sleep(2);
    tl_run(NULL, CMD("ip", "link", "set", "v0", "down"));
    printf("link DOWN at %s\n", tl_time_hms());
    tl_sleep(6);
    tl_run(NULL, CMD("ip", "link", "set", "v0", "up"));
    printf("link UP   at %s\n", tl_time_hms());
    tl_sleep(6);
    kill(watch, SIGTERM);
    tl_wait(watch);
    tl_sleep(0.5);
    tl_print_lines(tl_grep(tl_read(client_log), "connection lost|reconnect|cannot connect", 0), "", 100);
    tl_print_lines(tl_grep(tl_read(server_log), "no ACK|heartbeat lost", 0), "", 110);
    seq_after = count_after_reconnect(tl_read(client_log));

    kill(g_tcpdump_pid, SIGINT);
    tl_wait(g_tcpdump_pid);
    g_tcpdump_pid = 0;
    tl_stop_server();
    fputs(tl_tail(tl_read(server_log), 1), stdout);

    tl_expect(contains(server_log, "no ACK for"), "server noticed the dead link by heartbeat");
    tl_expect(contains(server_log, "exit 5"), "that child exited with status 5");
    tl_expect(contains(client_log, "connection lost"), "client noticed the dead link");
    tl_expect(contains(client_log, "reconnected to"), "client reconnected after the link came back");
    tl_expect(seq_after > 0, "pushes resumed after reconnecting (%d received)", seq_after);

    puts("--- capture on the virtual cable (real Ethernet framing)");
    narp = tl_nlines(tl_capture_out(CMD("tcpdump", "-Z", "root", "-nn", "-r", pcap, "arp")));
    nsyn = tl_nlines(tl_capture_out(CMD("tcpdump", "-Z", "root", "-nn", "-r", pcap, syn)));
    printf("packets: %d, ARP: %d, SYN or SYN+ACK: %d\n",
           tl_nlines(tl_capture_out(CMD("tcpdump", "-Z", "root", "-nn", "-r", pcap))), narp, nsyn);
    tl_print_lines(tl_head(tl_capture_out(CMD("tcpdump", "-Z", "root", "-nn", "-e", "-r", pcap, syn)), 2), "", 120);
    tl_expect(narp > 0, "ARP resolved the peer's MAC address");
    tl_expect(nsyn > 0, "tcp[13] & 2 == 2 finds the handshakes");
    return tl_fails == 0 ? 0 : 1;
}

static int run(void)
{
    tl_heading("Environment");
    printf("date     : %s\n", tl_date());
    printf("kernel   : %s\n", tl_kernel());
    puts("note     : one physical computer; see the header of this program");
    if (tl_run(NULL, CMD("make", "-s", "all")) != 0)
        return 1;
    if (tl_port_in_use(tl_port)) {
        printf("port %d is already in use; set PORT=...\n", tl_port);
        return 1;
    }

    if (part_windows() != 0)
        tl_fails++;

    tl_heading("Part 2: two network stacks, injected faults");
    if (tl_which("tc") && tl_run("/dev/null", CMD("unshare", "-Urn", "true")) == 0) {
        if (tl_run(NULL, CMD("unshare", "-Urn", tl_self_path(), "--net-inner")) != 0)
            tl_fails++;
    } else {
        puts("SKIP: needs tc and unprivileged user namespaces");
    }

    tl_heading("Result");
    return tl_print_result();
}

int main(int argc, char **argv)
{
    int rc;

    tl_init(9190);
    if (argc > 1 && strcmp(argv[1], "--net-inner") == 0) {
        rc = part_net_inner();
        inner_cleanup();
        return rc;
    }
    tl_tee("logs/test_real.log");
    rc = run();
    tl_stop_server();
    return tl_finish(rc);
}
