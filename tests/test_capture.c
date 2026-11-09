/*
 * test_capture.c - loopback capture of the NP protocol with tcpdump.
 *
 *   1. start ./server, then tcpdump -i lo 'tcp port 9090' -w logs/capture.pcap
 *   2. traffic: one request, a 3-push subscription, an HTTP request (garbage)
 *      and a header claiming a 4 GiB body (biglen)
 *   3. stop tcpdump and read the pcap back:
 *        - the first connection packet by packet (handshake, request, reply, FIN)
 *        - tcpdump -X of the first request/reply: "NP" in the ASCII column
 *        - a small decoder that splits every TCP payload into NP messages
 *          (magic, version, opcode, body_len)
 *   4. check that the 8-byte header with magic 0x4E50 frames every message
 *
 * tcpdump needs root: run the program as root, or it uses sudo for tcpdump.
 * Usage : make test-capture        (env: PORT=9090)
 * Output: logs/test_capture.log, logs/capture.pcap (opens in Wireshark),
 *         logs/capture_decoded.txt (full tcpdump -X dump), logs/capture_server.log
 */
#include "testlib.h"

#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PCAP "logs/capture.pcap"
#define DECODED "logs/capture_decoded.txt"
#define SERVER_LOG "logs/capture_server.log"
#define MAX_PACKET 65536

static int g_sudo;
static char *g_tmp;
static char *g_err;
static char *g_pidfile;
static pid_t g_cap;

/* Decoder state and counters */
static unsigned char b[MAX_PACKET];
static int nb;
static char g_ports[128];
static int g_from_server;
static int seg, msgs, n_req, n_sub, n_resp, n_err;
static int bad_server, nonnp_client, bad_resp, unframed, biglen;

/* tcpdump runs as root (directly or via sudo). The helper records its own PID
 * and then execs tcpdump, so the PID file names tcpdump itself. */
static int tcpdump_helper(const char *pidfile, const char *pcap, const char *filter)
{
    FILE *f = fopen(pidfile, "w");

    if (!f)
        return 1;
    fprintf(f, "%ld\n", (long)getpid());
    fclose(f);
    execlp("tcpdump", "tcpdump", "-i", "lo", "-nn", "-U", "-Z", "root", "-w", pcap, filter, (char *)NULL);
    perror("tcpdump");
    return 127;
}

static int start_capture(void)
{
    const char *self = tl_self_path();
    char *filter = tl_fmt("tcp port %d", tl_port);
    int i;

    g_err = tl_fmt("%s/tcpdump.err", g_tmp);
    g_pidfile = tl_fmt("%s/tcpdump.pid", g_tmp);
    unlink(PCAP);
    if (g_sudo)
        g_cap = tl_spawn(g_err, CMD("sudo", self, "--tcpdump-helper", g_pidfile, PCAP, filter));
    else
        g_cap = tl_spawn(g_err, CMD(self, "--tcpdump-helper", g_pidfile, PCAP, filter));
    for (i = 0; i < 50; i++) {
        if (strstr(tl_read(g_err), "listening on"))
            return 0;
        tl_sleep(0.1);
    }
    puts("tcpdump did not start:");
    tl_print_file(g_err);
    return -1;
}

static void stop_capture(void)
{
    char *pid;

    if (!g_cap)
        return;
    pid = tl_read(g_pidfile);
    pid[strcspn(pid, "\n")] = '\0';
    if (g_sudo)
        tl_run("/dev/null", CMD("sudo", "kill", "-INT", pid));
    else
        kill((pid_t)strtol(pid, NULL, 10), SIGINT);
    tl_wait(g_cap);
    g_cap = 0;
    if (g_sudo)
        tl_run(NULL, CMD("sudo", "chown", tl_fmt("%d:%d", (int)getuid(), (int)getgid()), PCAP));
}

static const char *opname(unsigned op)
{
    switch (op) {
    case 0x01: return "SYSINFO_REQ";
    case 0x02: return "SUBSCRIBE_REQ";
    case 0x81: return "SYSINFO_RESP";
    case 0xFF: return "ERROR";
    default:   return "UNKNOWN";
    }
}

static void text_preview(int from, int n, char *out)
{
    int i, k = 0;

    for (i = from; i < from + n && i < nb; i++)
        out[k++] = (b[i] >= 32 && b[i] < 127) ? (char)b[i] : '.';
    out[k] = '\0';
}

/* Split the current packet's TCP payload into NP messages, one line each. */
static void flush_packet(void)
{
    unsigned long long off;
    int ihl, payload;
    const char *dir;

    if (nb == 0)
        return;
    ihl = (b[0] & 0x0F) * 4;
    off = (unsigned long long)ihl + (unsigned long long)((b[ihl + 12] >> 4) * 4);
    payload = nb - (int)off;
    dir = g_from_server ? "S->C" : "C->S";
    seg++;
    while (off + 8 <= (unsigned long long)nb) {
        unsigned op = b[off + 3];
        unsigned long blen = ((unsigned long)b[off + 4] << 24) | ((unsigned long)b[off + 5] << 16) |
                             ((unsigned long)b[off + 6] << 8) | b[off + 7];
        const char *name = opname(op);

        if (b[off] != 0x4E || b[off + 1] != 0x50) {
            char preview[32];

            text_preview((int)off, 16, preview);
            printf("%3d  %-4s %-11s %5d   non-NP data: \"%s\"\n", seg, dir, g_ports, payload, preview);
            if (g_from_server)
                bad_server++;
            else
                nonnp_client++;
            nb = 0;
            return;
        }
        printf("%3d  %-4s %-11s %5d   0x4E50  %3d  %-14s %10lu\n", seg, dir, g_ports, payload,
               (unsigned)b[off + 2], name, blen);
        msgs++;
        n_req += op == 0x01;
        n_sub += op == 0x02;
        n_resp += op == 0x81;
        n_err += op == 0xFF;
        if (op == 0x81 && blen != 120)
            bad_resp++;
        if (blen == 4294967295UL)
            biglen++;
        off += 8 + (unsigned long long)blen;
    }
    if (g_from_server && off != (unsigned long long)nb)
        unframed++;
    nb = 0;
}

static const char *after_last_dot(const char *s)
{
    const char *dot = strrchr(s, '.');

    return dot ? dot + 1 : s;
}

static void add_hex_tokens(char *line)
{
    char *save = NULL;
    char *tok = strtok_r(line, " \t\n", &save);

    for (tok = strtok_r(NULL, " \t\n", &save); tok; tok = strtok_r(NULL, " \t\n", &save)) {
        size_t len = strlen(tok);
        unsigned long value = strtoul(tok, NULL, 16);

        if (len == 4 && nb + 2 <= MAX_PACKET) {
            b[nb++] = (unsigned char)(value >> 8);
            b[nb++] = (unsigned char)value;
        } else if (len == 2 && nb < MAX_PACKET) {
            b[nb++] = (unsigned char)value;
        }
    }
}

/* Parses `tcpdump -x`: a packet line "time IP src.port > dst.port: ..." and
 * hex lines "0x0010:  7f00 0001 ...". */
static void decode_np(const char *data_filter)
{
    char *dump = tl_capture_out(CMD("tcpdump", "-Z", "root", "-r", PCAP, "-nn", "-x", data_filter));
    char *save = NULL;
    char *line;

    for (line = strtok_r(dump, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *start = line + strspn(line, " \t");

        if (isdigit((unsigned char)line[0])) {
            char src[64], dst[64];

            flush_packet();
            if (sscanf(line, "%*s %*s %63s %*s %63s", src, dst) == 2) {
                char *colon = strchr(dst, ':');
                char *sport = strdup(after_last_dot(src));

                if (colon)
                    *colon = '\0';
                snprintf(g_ports, sizeof(g_ports), "%s>%s", sport, after_last_dot(dst));
                g_from_server = (int)strtol(sport, NULL, 10) == tl_port;
                free(sport);
            }
        } else if (strncmp(start, "0x", 2) == 0) {
            add_hex_tokens(start);
        }
    }
    flush_packet();
}

static int run(void)
{
    char *data_filter = tl_fmt("tcp port %d and (((ip[2:2] - ((ip[0]&0xf)<<2)) - ((tcp[12]&0xf0)>>2)) != 0)",
                               tl_port);
    char *captured_line;
    int captured = 0, np_hex;

    tl_banner("Packet capture of the NP protocol (tcpdump on lo)");
    printf("date     : %s\n", tl_date());
    printf("kernel   : %s\n", tl_kernel());
    {
        char *version = tl_head(tl_capture(CMD("tcpdump", "--version")), 1);

        version[strcspn(version, "\n")] = '\0';
        printf("tcpdump  : %s\n", version);
    }
    printf("privilege: %s\n", g_sudo ? "sudo" : "running as root");
    if (tl_run(NULL, CMD("make", "-s", "all")) != 0)
        return 1;
    if (tl_port_in_use(tl_port)) {
        printf("port %d is already in use; stop the other server or set PORT=...\n", tl_port);
        return 1;
    }

    tl_section("Start server and tcpdump");
    if (tl_start_server(NULL, SERVER_LOG, NO_FLAGS) != 0)
        return 1;
    printf("$ ./server -p %d                      -> pid %d\n", tl_port, (int)tl_server_pid);
    if (start_capture() != 0)
        return 1;
    printf("$ tcpdump -i lo -nn -U -w %s 'tcp port %d'\n", PCAP, tl_port);
    tl_print_file(g_err);

    tl_section("Traffic");
    puts("$ ./client -q                 (1 request)");
    tl_run(NULL, CMD("./client", "-p", tl_port_str, "-q"));
    puts("$ ./client -w 200 -n 3        (subscription, 3 pushes)");
    tl_run(NULL, CMD("./client", "-p", tl_port_str, "-w", "200", "-n", "3"));
    puts("$ ./client --mode garbage     (HTTP request instead of NP)");
    tl_run(NULL, CMD("./client", "-p", tl_port_str, "--mode", "garbage"));
    puts("$ ./client --mode biglen      (header claims 4 GiB body)");
    tl_run(NULL, CMD("./client", "-p", tl_port_str, "--mode", "biglen"));
    tl_sleep(0.5);

    tl_section("Stop tcpdump");
    stop_capture();
    fputs(tl_grep(tl_read(g_err), "captured|received|dropped", 0), stdout);
    captured_line = tl_grep(tl_read(g_err), "packets captured", 0);
    captured = (int)strtol(captured_line, NULL, 10);
    tl_stop_server();
    tl_write(DECODED, tl_capture_out(CMD("tcpdump", "-Z", "root", "-r", PCAP, "-nn", "-X")), 0);
    np_hex = tl_file_count(DECODED, "4e50 01");

    tl_section("First connection, packet by packet (tcpdump -r -c 10)");
    fputs(tl_capture_out(CMD("tcpdump", "-Z", "root", "-r", PCAP, "-nn", "-c", "10")), stdout);

    tl_section("First request and reply as hex + ASCII (tcpdump -X): look for \"NP\"");
    fputs(tl_capture_out(CMD("tcpdump", "-Z", "root", "-r", PCAP, "-nn", "-X", "-c", "2", data_filter)), stdout);

    tl_section("NP decoder: every TCP payload split into NP messages");
    printf("%3s  %-4s %-11s %5s   %-6s  %3s  %-14s %10s\n",
           "seg", "dir", "ports", "bytes", "magic", "ver", "opcode", "body_len");
    decode_np(data_filter);
    printf("data segments: %d, NP messages: %d\n", seg, msgs);

    tl_banner("Checks");
    tl_expect(captured > 0, "tcpdump captured packets on lo (%d)", captured);
    tl_expect(n_req == 2, "SYSINFO_REQ x2 (normal request + biglen header)");
    tl_expect(n_sub == 1, "SUBSCRIBE_REQ x1");
    tl_expect(n_resp == 4, "SYSINFO_RESP x4 (1 reply + 3 pushes)");
    tl_expect(n_err == 2, "ERROR x2 (garbage + biglen rejected)");
    tl_expect(bad_server == 0, "every server segment starts with magic 0x4E50");
    tl_expect(unframed == 0, "server segments split exactly into 8-byte header + body_len");
    tl_expect(bad_resp == 0, "every SYSINFO_RESP has body_len 120 (128 bytes on the wire)");
    tl_expect(nonnp_client == 1, "the garbage client's bytes are identified as non-NP");
    tl_expect(biglen == 1, "the forged body_len 0xFFFFFFFF is visible in the capture");
    tl_expect(np_hex > 0, "tcpdump -X dump shows the header bytes 4e50 01 (%d lines)", np_hex);

    tl_section("Open the capture in Wireshark");
    printf("  file          : %s\n", PCAP);
    printf("  display filter: tcp.port == %d && tcp.len > 0\n", tl_port);
    puts("  NP header     : tcp.payload[0:2] == 4e:50 (magic), [2] version,");
    puts("                  [3] opcode, [4:4] body_len (big-endian)");

    tl_banner("Result");
    return tl_print_result();
}

int main(int argc, char **argv)
{
    int rc;

    if (argc == 5 && strcmp(argv[1], "--tcpdump-helper") == 0)
        return tcpdump_helper(argv[2], argv[3], argv[4]);

    tl_init(9090);
    if (getuid() != 0) {
        if (!tl_which("sudo")) {
            puts("tcpdump needs root: run as root or install sudo");
            return 1;
        }
        g_sudo = 1;
        if (tl_run(NULL, CMD("sudo", "-v")) != 0) {
            puts("tcpdump needs root: sudo authentication failed");
            return 1;
        }
    }
    tl_tee("logs/test_capture.log");
    g_tmp = tl_mktemp("np_capture");
    rc = run();
    stop_capture();
    tl_stop_server();
    tl_rmrf(g_tmp);
    return tl_finish(rc);
}
