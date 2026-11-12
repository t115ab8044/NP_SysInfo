/*
 * test_capture.c - capture the client/server traffic on loopback with tcpdump
 *                  and read the headers back.
 *
 *   1. start ./server, then tcpdump -i lo -n -w logs/capture.pcap 'tcp port 9090'
 *   2. traffic: SYSINFO, WATCH 3 200 and an unknown request (HELLO)
 *   3. stop tcpdump and read the capture back:
 *        - tcpdump -r -n -c 10: the first connection (handshake, data, FIN)
 *        - tcpdump -r -n -x -c 4: the same packets in hex
 *        - a small pcap reader in C: every packet of the first connection,
 *          then the request packet split field by field into its Ethernet,
 *          IPv4 and TCP headers and the payload
 *   4. checks: three-way handshake, "SYSINFO\n" request, "OK seq=1" reply,
 *      FIN from both sides, the WATCH and ERROR replies
 *
 * tcpdump needs root: run the program as root, or it uses sudo for tcpdump.
 * Usage : make test-capture        (env: PORT=9090)
 * Output: logs/test_capture.log, logs/capture.pcap (opens in Wireshark),
 *         logs/capture_decoded.txt (tcpdump -n -x of every packet),
 *         logs/capture_server.log
 */
#include "testlib.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PCAP        "logs/capture.pcap"
#define DECODED     "logs/capture_decoded.txt"
#define SERVER_LOG  "logs/capture_server.log"
#define MAX_PKTS    512

#define TH_FIN 0x01
#define TH_SYN 0x02
#define TH_RST 0x04
#define TH_PSH 0x08
#define TH_ACK 0x10

static int g_sudo;
static char *g_tmp;
static char *g_err;
static char *g_pidfile;
static pid_t g_cap;

/* ===== tcpdump ========================================================== */

/* tcpdump runs as root (directly or via sudo). The helper records its own PID
 * and then execs tcpdump, so the PID file names tcpdump itself. -Z root keeps
 * tcpdump allowed to write the file into the project directory. */
static int tcpdump_helper(const char *pidfile, const char *pcap, const char *filter)
{
    FILE *f = fopen(pidfile, "w");

    if (!f)
        return 1;
    fprintf(f, "%ld\n", (long)getpid());
    fclose(f);
    execlp("tcpdump", "tcpdump", "-i", "lo", "-n", "-Z", "root", "-w", pcap, filter, (char *)NULL);
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

/* ===== pcap reader ====================================================== */

struct packet {
    double t;                       /* seconds since the first packet */
    const unsigned char *frame;     /* link-layer header first */
    int frame_len;
    const unsigned char *ip, *tcp, *payload;
    int ihl, tcp_len, payload_len;
    unsigned sport, dport, flags, window;
    unsigned long seq, ack;
};

static unsigned char *g_file;
static struct packet g_pk[MAX_PKTS];
static int g_npk;
static int g_link_len;              /* bytes before the IPv4 header */
static unsigned long g_linktype;
static int g_big_endian, g_nano;

static unsigned rd16(const unsigned char *p)
{
    return (unsigned)p[0] << 8 | p[1];
}

static unsigned long rd32(const unsigned char *p)
{
    return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | (unsigned long)p[2] << 8 | p[3];
}

/* pcap headers use the byte order of the machine that wrote the file. */
static unsigned long hdr32(const unsigned char *p)
{
    if (g_big_endian)
        return rd32(p);
    return (unsigned long)p[3] << 24 | (unsigned long)p[2] << 16 | (unsigned long)p[1] << 8 | p[0];
}

/*
 * File layout: a 24-byte global header (magic, version, snaplen, link type),
 * then for every packet a 16-byte record header (ts_sec, ts_usec, incl_len,
 * orig_len) followed by incl_len bytes of the frame.
 */
static int load_pcap(const char *path)
{
    FILE *f = fopen(path, "rb");
    size_t size = 0, off = 24;
    double t0 = -1;
    long n;

    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    if (n < 24 || (g_file = malloc((size_t)n)) == NULL ||
        fread(g_file, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        return -1;
    }
    fclose(f);
    size = (size_t)n;

    if (rd32(g_file) == 0xd4c3b2a1UL || rd32(g_file) == 0x4d3cb2a1UL)
        g_big_endian = 0;
    else if (rd32(g_file) == 0xa1b2c3d4UL || rd32(g_file) == 0xa1b23c4dUL)
        g_big_endian = 1;
    else
        return -1;
    g_nano = rd32(g_file) == 0x4d3cb2a1UL || rd32(g_file) == 0xa1b23c4dUL;
    g_linktype = hdr32(g_file + 20);
    switch (g_linktype) {
    case 1:   g_link_len = 14; break;   /* Ethernet: what Linux uses on lo */
    case 113: g_link_len = 16; break;   /* Linux cooked capture (-i any) */
    case 276: g_link_len = 20; break;   /* Linux cooked capture v2 */
    default:  return -1;
    }

    while (off + 16 <= size && g_npk < MAX_PKTS) {
        const unsigned char *rec = g_file + off;
        unsigned long incl = hdr32(rec + 8);
        double t = (double)hdr32(rec) + (double)hdr32(rec + 4) / (g_nano ? 1e9 : 1e6);
        struct packet *pk = &g_pk[g_npk];
        int ip_total;

        if (off + 16 + incl > size)
            break;
        off += 16 + incl;
        if ((int)incl < g_link_len + 40)
            continue;
        pk->frame = rec + 16;
        pk->frame_len = (int)incl;
        pk->ip = pk->frame + g_link_len;
        if (g_linktype == 1 && rd16(pk->frame + 12) != 0x0800)
            continue;                   /* not IPv4 */
        if (pk->ip[0] >> 4 != 4 || pk->ip[9] != 6)
            continue;                   /* not IPv4 / TCP */
        pk->ihl = (pk->ip[0] & 0x0f) * 4;
        pk->tcp = pk->ip + pk->ihl;
        pk->tcp_len = (pk->tcp[12] >> 4) * 4;
        ip_total = (int)rd16(pk->ip + 2);
        pk->payload = pk->tcp + pk->tcp_len;
        pk->payload_len = ip_total - pk->ihl - pk->tcp_len;
        if (pk->payload_len < 0 || pk->payload + pk->payload_len > pk->frame + pk->frame_len)
            continue;
        pk->sport = rd16(pk->tcp);
        pk->dport = rd16(pk->tcp + 2);
        pk->seq = rd32(pk->tcp + 4);
        pk->ack = rd32(pk->tcp + 8);
        pk->flags = pk->tcp[13];
        pk->window = rd16(pk->tcp + 14);
        if (t0 < 0)
            t0 = t;
        pk->t = t - t0;
        g_npk++;
    }
    return g_npk;
}

static char *flag_names(unsigned flags)
{
    static const struct { unsigned bit; const char *name; } names[] = {
        { TH_SYN, "SYN" }, { TH_FIN, "FIN" }, { TH_RST, "RST" }, { TH_PSH, "PSH" }, { TH_ACK, "ACK" }
    };
    char *s = strdup("");

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (flags & names[i].bit)
            s = tl_fmt("%s%s%s", s, *s ? "+" : "", names[i].name);
    }
    return s;
}

/* Payload as text, "\n" shown as \n, at most max characters. */
static char *payload_text(const struct packet *pk, int max)
{
    char *s = strdup("");

    for (int i = 0; i < pk->payload_len && (int)strlen(s) < max; i++) {
        unsigned char c = pk->payload[i];

        if (c == '\n')
            s = tl_fmt("%s\\n", s);
        else
            s = tl_fmt("%s%c", s, c >= 32 && c < 127 ? c : '.');
    }
    if (pk->payload_len > 0 && (int)strlen(s) >= max)
        s = tl_fmt("%s...", s);
    return s;
}

static int from_server(const struct packet *pk)
{
    return (int)pk->sport == tl_port;
}

static int starts_with(const struct packet *pk, const char *text)
{
    size_t n = strlen(text);

    return pk->payload_len >= (int)n && memcmp(pk->payload, text, n) == 0;
}

static void hex_bytes(const char *label, const unsigned char *base, const unsigned char *p, int n)
{
    for (int row = 0; row < n; row += 16) {
        printf("  %-8s 0x%04x: ", row == 0 ? label : "", (unsigned)(p - base) + (unsigned)row);
        for (int i = row; i < n && i < row + 16; i++)
            printf(" %02x", p[i]);
        putchar('\n');
    }
}

static const char *ip_str(const unsigned char *p)
{
    return tl_fmt("%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
}

/* The request packet, one header at a time. */
static void dissect(const struct packet *pk, unsigned long client_isn, unsigned long server_isn)
{
    const unsigned char *f = pk->frame, *ip = pk->ip, *tcp = pk->tcp;

    tl_section("Bytes of the request packet, split at the header boundaries");
    if (g_linktype == 1)
        hex_bytes("Ethernet", f, f, g_link_len);
    else
        hex_bytes("link", f, f, g_link_len);
    hex_bytes("IPv4", f, ip, pk->ihl);
    hex_bytes("TCP", f, tcp, pk->tcp_len);
    hex_bytes("payload", f, pk->payload, pk->payload_len);

    tl_section("Header fields of the request packet");
    if (g_linktype == 1) {
        printf("  Ethernet (%d bytes)\n", g_link_len);
        printf("    destination MAC  %02x:%02x:%02x:%02x:%02x:%02x   (loopback: all zero)\n",
               f[0], f[1], f[2], f[3], f[4], f[5]);
        printf("    source MAC       %02x:%02x:%02x:%02x:%02x:%02x\n",
               f[6], f[7], f[8], f[9], f[10], f[11]);
        printf("    EtherType        0x%04x   (IPv4)\n", rd16(f + 12));
    }
    printf("  IPv4 (%d bytes)\n", pk->ihl);
    printf("    version          %u\n", ip[0] >> 4);
    printf("    header length    %u x 4 = %d bytes\n", ip[0] & 0x0f, pk->ihl);
    printf("    total length     %u   (IP header + TCP header + payload)\n", rd16(ip + 2));
    printf("    identification   0x%04x\n", rd16(ip + 4));
    printf("    flags            0x%x%s\n", ip[6] >> 5, (ip[6] & 0x40) ? "   (DF: don't fragment)" : "");
    printf("    TTL              %u\n", ip[8]);
    printf("    protocol         %u   (TCP)\n", ip[9]);
    printf("    header checksum  0x%04x\n", rd16(ip + 10));
    printf("    source           %s\n", ip_str(ip + 12));
    printf("    destination      %s\n", ip_str(ip + 16));
    printf("  TCP (%d bytes)\n", pk->tcp_len);
    printf("    source port      %u   (client, chosen by the kernel)\n", pk->sport);
    printf("    destination port %u   (server)\n", pk->dport);
    printf("    sequence number  %lu   (relative %lu)\n", pk->seq, pk->seq - client_isn);
    printf("    ack number       %lu   (relative %lu)\n", pk->ack, pk->ack - server_isn);
    printf("    data offset      %u x 4 = %d bytes%s\n", tcp[12] >> 4, pk->tcp_len,
           pk->tcp_len > 20 ? "   (20 + options, e.g. timestamps)" : "");
    printf("    flags            0x%02x   (%s)\n", pk->flags, flag_names(pk->flags));
    printf("    window           %u\n", pk->window);
    printf("    checksum         0x%04x\n", rd16(tcp + 16));
    printf("  payload (%d bytes)  \"%s\"\n", pk->payload_len, payload_text(pk, 60));
}

/* ===== test ============================================================= */

static int run(void)
{
    char *captured_line, *filter = tl_fmt("tcp port %d", tl_port);
    unsigned cport = 0;
    unsigned long client_isn = 0, server_isn = 0;
    const struct packet *request = NULL, *reply = NULL;
    int captured, conn[3] = { -1, -1, -1 }, nconn = 0;
    int fin_client = 0, fin_server = 0, ok_lines = 0, error_reply = 0, hello = 0;
    int handshake = 0, all_tcp_v4;

    tl_banner("Packet capture on loopback (tcpdump) and header analysis");
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
    printf("$ ./server -p %d                     -> pid %d\n", tl_port, (int)tl_server_pid);
    if (start_capture() != 0)
        return 1;
    printf("$ tcpdump -i lo -n -w %s '%s'\n", PCAP, filter);
    tl_print_file(g_err);

    tl_section("Traffic");
    printf("$ ./client -p %d                (SYSINFO)\n", tl_port);
    tl_run("/dev/null", CMD("./client", "-p", tl_port_str));
    printf("$ ./client -p %d -n 3 -w 200    (WATCH 3 200)\n", tl_port);
    tl_run("/dev/null", CMD("./client", "-p", tl_port_str, "-n", "3", "-w", "200"));
    printf("$ ./client -p %d -r HELLO       (unknown request)\n", tl_port);
    tl_run("/dev/null", CMD("./client", "-p", tl_port_str, "-r", "HELLO"));
    /* libpcap hands packets to tcpdump in batches, about once a second;
     * packets of a batch not handed over yet are lost on Ctrl+C. */
    tl_sleep(2.0);

    tl_section("Stop tcpdump (Ctrl+C = SIGINT)");
    stop_capture();
    fputs(tl_grep(tl_read(g_err), "captured|received|dropped", 0), stdout);
    captured_line = tl_grep(tl_read(g_err), "packets captured", 0);
    captured = (int)strtol(captured_line, NULL, 10);
    tl_stop_server();
    tl_write(DECODED, tl_capture_out(CMD("tcpdump", "-Z", "root", "-r", PCAP, "-n", "-x")), 0);

    tl_section("First connection, packet by packet: tcpdump -r %s -n -c 10", PCAP);
    fputs(tl_capture_out(CMD("tcpdump", "-Z", "root", "-r", PCAP, "-n", "-c", "10")), stdout);

    tl_section("The same packets in hex: tcpdump -r %s -n -x -c 4", PCAP);
    fputs(tl_capture_out(CMD("tcpdump", "-Z", "root", "-r", PCAP, "-n", "-x", "-c", "4")), stdout);

    tl_section("pcap reader: every packet of the first connection");
    if (load_pcap(PCAP) <= 0) {
        puts("cannot read the pcap file");
        return 1;
    }
    printf("link type %lu (%s), %d TCP/IPv4 packets in the file\n", g_linktype,
           g_linktype == 1 ? "Ethernet" : "Linux cooked", g_npk);
    for (int i = 0; i < g_npk; i++) {
        const struct packet *pk = &g_pk[i];

        if ((pk->flags & (TH_SYN | TH_ACK)) == TH_SYN && nconn < 3)
            conn[nconn++] = (int)pk->sport;
        if (!from_server(pk) && starts_with(pk, "HELLO\n"))
            hello++;
        if (from_server(pk) && starts_with(pk, "OK seq="))
            ok_lines++;
        if (from_server(pk) && starts_with(pk, "ERROR unknown request"))
            error_reply++;
    }
    cport = conn[0] > 0 ? (unsigned)conn[0] : 0;
    printf("  %-3s %-9s %-5s %-12s %8s %8s %4s  %s\n",
           "#", "time", "dir", "flags", "rel seq", "rel ack", "len", "payload");
    for (int i = 0, k = 0; i < g_npk; i++) {
        const struct packet *pk = &g_pk[i];
        unsigned long seq, ack;

        if (pk->sport != cport && pk->dport != cport)
            continue;
        if (k == 0 && pk->flags == TH_SYN)
            client_isn = pk->seq;
        if (from_server(pk) && (pk->flags & TH_SYN))
            server_isn = pk->seq;
        seq = pk->seq - (from_server(pk) ? server_isn : client_isn);
        ack = (pk->flags & TH_ACK) ? pk->ack - (from_server(pk) ? client_isn : server_isn) : 0;
        printf("  %-3d %9.6f %-5s %-12s %8lu %8lu %4d  %s\n", ++k, pk->t,
               from_server(pk) ? "S->C" : "C->S", flag_names(pk->flags), seq, ack,
               pk->payload_len, payload_text(pk, 32));
        if (k == 1 && pk->flags == TH_SYN)
            handshake = 1;
        else if (k == 2 && handshake == 1 && pk->flags == (TH_SYN | TH_ACK))
            handshake = 2;
        else if (k == 3 && handshake == 2 && pk->flags == TH_ACK)
            handshake = 3;
        if (!from_server(pk) && pk->payload_len > 0 && request == NULL)
            request = pk;
        if (from_server(pk) && pk->payload_len > 0 && reply == NULL)
            reply = pk;
        if ((pk->flags & TH_FIN) && from_server(pk))
            fin_server++;
        else if (pk->flags & TH_FIN)
            fin_client++;
    }
    all_tcp_v4 = g_npk == captured;

    if (request != NULL)
        dissect(request, client_isn, server_isn);

    tl_banner("Checks");
    tl_expect(captured > 0, "tcpdump captured packets on lo (%d)", captured);
    tl_expect(all_tcp_v4, "the C pcap reader decoded every captured packet as IPv4/TCP (%d)", g_npk);
    tl_expect(nconn == 3, "3 connections (3 SYN packets): SYSINFO, WATCH, HELLO");
    tl_expect(handshake == 3, "first connection opens with the three-way handshake SYN, SYN+ACK, ACK");
    tl_expect(request != NULL && request->payload_len == 8 && starts_with(request, "SYSINFO\n"),
              "the request payload is the 8 bytes \"SYSINFO\\n\"");
    tl_expect(request != NULL && request->ihl == 20 && request->ip[9] == 6 &&
              (int)request->dport == tl_port,
              "request: IPv4 header 20 bytes, protocol 6 (TCP), destination port %d", tl_port);
    tl_expect(reply != NULL && starts_with(reply, "OK seq=1 pid=") &&
              reply->payload[reply->payload_len - 1] == '\n',
              "the reply is one text line \"OK seq=1 pid=... \\n\"");
    tl_expect(fin_client >= 1 && fin_server >= 1, "both sides send FIN to close the connection");
    tl_expect(ok_lines == 4, "4 \"OK seq=\" segments from the server (1 SYSINFO + 3 WATCH)");
    tl_expect(hello == 1 && error_reply == 1, "\"HELLO\\n\" is answered with \"ERROR unknown request ...\"");

    tl_section("Open the capture in Wireshark");
    printf("  file          : %s\n", PCAP);
    printf("  display filter: tcp.port == %d\n", tl_port);
    puts("  right-click a packet -> Follow -> TCP Stream shows the request and reply text");

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
