/*
 * np_client_win.c - independent NP client for the Windows side of tests/test_real.c.
 *
 * The graded client is client.c. This one is written from the wire format and
 * shares no code or header with it, so a successful exchange shows that the
 * protocol description is complete. Built with MinGW-w64 it runs on Windows
 * and talks to the C server inside the WSL2 VM, over a virtual Ethernet link.
 * The same file also builds on Linux with gcc.
 *
 * Usage: np_client_win HOST PORT [HOLD_SECONDS]
 */
#ifdef _WIN32
#define _WIN32_WINNT 0x0601             /* getaddrinfo, inet_ntop */
#define __USE_MINGW_ANSI_STDIO 1        /* %llu */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
#define sock_close closesocket
#define sock_error() ((int)WSAGetLastError())
#define sleep_ms(ms) Sleep(ms)
#define OS_NAME "Windows"
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
typedef int sock_t;
#define INVALID_SOCKET (-1)
#define sock_close close
#define sock_error() errno
#define OS_NAME "Linux"

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

    nanosleep(&ts, NULL);
}
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEADER_SIZE      8
#define MAX_BODY         4096
#define MAGIC            0x4E50
#define VERSION          1
#define OP_SYSINFO_REQ   0x01
#define OP_SUBSCRIBE_REQ 0x02
#define SYSINFO_SIZE     120
#define CONCURRENT       12
#define TIMEOUT_SEC      5

/* Offsets inside the 120-byte sysinfo body. */
#define OFF_PID          0
#define OFF_HOSTNAME     8
#define OFF_UPTIME       72
#define OFF_MEM_TOTAL    80
#define OFF_NPROCS       116

static const char *g_host;
static const char *g_port;

static void die(const char *what, int code)
{
    fprintf(stderr, "np_client_win: %s (error %d)\n", what, code);
    exit(1);
}

static void put_be16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
}

static void put_be32(unsigned char *p, uint32_t v)
{
    put_be16(p, (unsigned)(v >> 16));
    put_be16(p + 2, (unsigned)(v & 0xFFFF));
}

static unsigned get_be16(const unsigned char *p)
{
    return ((unsigned)p[0] << 8) | p[1];
}

static uint32_t get_be32(const unsigned char *p)
{
    return ((uint32_t)get_be16(p) << 16) | get_be16(p + 2);
}

static uint64_t get_be64(const unsigned char *p)
{
    return ((uint64_t)get_be32(p) << 32) | get_be32(p + 4);
}

static void set_timeouts(sock_t s)
{
#ifdef _WIN32
    DWORD timeout = TIMEOUT_SEC * 1000;
#else
    struct timeval timeout = { TIMEOUT_SEC, 0 };
#endif

    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
}

static sock_t connect_server(void)
{
    struct addrinfo hints;
    struct addrinfo *res;
    sock_t s;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    rc = getaddrinfo(g_host, g_port, &hints, &res);
    if (rc != 0)
        die("cannot resolve the host", rc);
    s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET || connect(s, res->ai_addr, (int)res->ai_addrlen) != 0)
        die("cannot connect", sock_error());
    freeaddrinfo(res);
    set_timeouts(s);
    return s;
}

static void send_all(sock_t s, const void *data, size_t len)
{
    const char *p = data;

    while (len > 0) {
        int n = send(s, p, (int)len, 0);

        if (n <= 0)
            die("send failed", sock_error());
        p += n;
        len -= (size_t)n;
    }
}

static void recv_all(sock_t s, void *data, size_t len)
{
    char *p = data;

    while (len > 0) {
        int n = recv(s, p, (int)len, 0);

        if (n == 0)
            die("peer closed the connection", 0);
        if (n < 0)
            die("recv failed", sock_error());
        p += n;
        len -= (size_t)n;
    }
}

static void send_request(sock_t s, unsigned opcode, const unsigned char *body, uint32_t body_len)
{
    unsigned char msg[HEADER_SIZE + 8];

    put_be16(msg, MAGIC);
    msg[2] = VERSION;
    msg[3] = (unsigned char)opcode;
    put_be32(msg + 4, body_len);
    if (body_len > 0)
        memcpy(msg + HEADER_SIZE, body, body_len);
    send_all(s, msg, HEADER_SIZE + (size_t)body_len);
}

/* Reads one message, NUL-terminates the body for printing, returns the opcode. */
static unsigned read_msg(sock_t s, unsigned char *body, uint32_t *body_len)
{
    unsigned char hdr[HEADER_SIZE];

    recv_all(s, hdr, sizeof(hdr));
    if (get_be16(hdr) != MAGIC)
        die("bad magic in the reply", 0);
    *body_len = get_be32(hdr + 4);
    if (*body_len > MAX_BODY)
        die("reply body is too long", 0);
    recv_all(s, body, *body_len);
    body[*body_len] = '\0';
    return hdr[3];
}

static void request_sysinfo(sock_t s, unsigned char *body)
{
    uint32_t body_len;

    send_request(s, OP_SYSINFO_REQ, NULL, 0);
    read_msg(s, body, &body_len);
    if (body_len != SYSINFO_SIZE)
        die("unexpected sysinfo size", (int)body_len);
}

static void print_addresses(sock_t s)
{
    struct sockaddr_in local;
    struct sockaddr_in peer;
    char local_ip[INET_ADDRSTRLEN];
    char peer_ip[INET_ADDRSTRLEN];
    socklen_t len = sizeof(local);

    if (getsockname(s, (struct sockaddr *)&local, &len) != 0)
        die("getsockname failed", sock_error());
    len = sizeof(peer);
    if (getpeername(s, (struct sockaddr *)&peer, &len) != 0)
        die("getpeername failed", sock_error());
    inet_ntop(AF_INET, &local.sin_addr, local_ip, sizeof(local_ip));
    inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip));
    printf("local=%s:%u server=%s:%u\n", local_ip, (unsigned)ntohs(local.sin_port),
           peer_ip, (unsigned)ntohs(peer.sin_port));
}

/* Close so that the peer gets RST instead of FIN. */
static void abort_with_rst(sock_t s)
{
    struct linger lg;

    lg.l_onoff = 1;
    lg.l_linger = 0;
    setsockopt(s, SOL_SOCKET, SO_LINGER, (const char *)&lg, sizeof(lg));
    sock_close(s);
}

int main(int argc, char **argv)
{
    unsigned char body[MAX_BODY + 1];
    unsigned char subscribe[8];
    uint32_t pids[CONCURRENT];
    sock_t socks[CONCURRENT];
    uint32_t body_len;
    unsigned opcode;
    unsigned hold_ms = 3000;
    sock_t s;
    int distinct = 0;
    int i;
    int j;
#ifdef _WIN32
    WSADATA wsa;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        die("WSAStartup failed", 0);
#endif

    if (argc < 3) {
        fprintf(stderr, "usage: %s HOST PORT [HOLD_SECONDS]\n", argv[0]);
        return 2;
    }
    g_host = argv[1];
    g_port = argv[2];
    if (argc > 3)
        hold_ms = (unsigned)(strtod(argv[3], NULL) * 1000.0);

    printf("client_os=%s compiler=gcc-%s\n", OS_NAME, __VERSION__);

    s = connect_server();
    print_addresses(s);
    request_sysinfo(s, body);
    printf("sysinfo pid=%lu host=%.64s uptime=%llus mem_total_kb=%llu tasks=%lu\n",
           (unsigned long)get_be32(body + OFF_PID), (const char *)body + OFF_HOSTNAME,
           (unsigned long long)get_be64(body + OFF_UPTIME),
           (unsigned long long)get_be64(body + OFF_MEM_TOTAL),
           (unsigned long)get_be32(body + OFF_NPROCS));
    sock_close(s);

    for (i = 0; i < CONCURRENT; i++)
        socks[i] = connect_server();
    for (i = 0; i < CONCURRENT; i++) {
        request_sysinfo(socks[i], body);
        pids[i] = get_be32(body + OFF_PID);
    }
    for (i = 0; i < CONCURRENT; i++) {
        for (j = 0; j < i && pids[j] != pids[i]; j++)
            ;
        if (j == i)
            distinct++;
    }
    printf("concurrent=%d distinct_pids=%d\n", CONCURRENT, distinct);
    sleep_ms(hold_ms);
    for (i = 0; i < CONCURRENT; i++)
        sock_close(socks[i]);

    s = connect_server();
    send_all(s, "GET / HTTP/1.0\r\n\r\n", 18);
    opcode = read_msg(s, body, &body_len);
    if (body_len < 4)
        die("error reply is too short", (int)body_len);
    printf("garbage reply_op=0x%02X error=%lu message=%s\n", opcode,
           (unsigned long)get_be32(body), (const char *)body + 4);
    sock_close(s);

    s = connect_server();
    put_be32(subscribe, 200);
    put_be32(subscribe + 4, 0);
    send_request(s, OP_SUBSCRIBE_REQ, subscribe, sizeof(subscribe));
    opcode = read_msg(s, body, &body_len);
    printf("subscribe first_push_op=0x%02X then abort with RST\n", opcode);
    abort_with_rst(s);
    sleep_ms(500);
    return 0;
}
