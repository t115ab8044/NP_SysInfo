/*
 * win_client.c - Windows client for the Real Test (Windows -> WSL2 server).
 *
 * client.c only runs on Linux, because it links libutils.so. This small
 * Winsock version sends the same one-line request, so the server can be
 * tested from another operating system over a real network interface
 * instead of loopback.
 *
 *   make win-client                                  (needs mingw-w64)
 *   .\win_client.exe 172.17.107.87                   SYSINFO on port 9090
 *   .\win_client.exe 172.17.107.87 9090 "WATCH 3 500"
 *
 * Like client.c it has no timeout: it prints every reply line until the
 * server closes the connection.
 * Exit status: 0 = got OK lines, 1 = error or ERROR reply, 2 = no reply.
 */
#define _WIN32_WINNT 0x0600             /* inet_ntop */
#define WIN32_LEAN_AND_MEAN

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>

#define MAX_REQUEST 127                 /* the server's request limit */

static double now_ms(void)
{
    LARGE_INTEGER freq, count;

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart * 1000.0 / (double)freq.QuadPart;
}

/* send() may write only part of the buffer, like write() on Linux. */
static int send_all(SOCKET s, const char *buf, int n)
{
    int done = 0;

    while (done < n) {
        int w = send(s, buf + done, n - done, 0);

        if (w == SOCKET_ERROR)
            return -1;
        done += w;
    }
    return 0;
}

static int run(SOCKET s, const char *request, double t0)
{
    char buf[512], line[512];
    int len = 0, ok = 0, errors = 0, n;

    n = snprintf(line, sizeof(line), "%s\n", request);
    printf("request: %s\n", request);
    if (send_all(s, line, n) < 0) {
        fprintf(stderr, "send failed: error %d\n", WSAGetLastError());
        return 1;
    }
    /* Split the byte stream into lines until the server closes. */
    while ((n = recv(s, buf, (int)sizeof(buf), 0)) > 0) {
        for (int i = 0; i < n; i++) {
            if (buf[i] != '\n') {
                if (len < (int)sizeof(line) - 1)
                    line[len++] = buf[i];
                continue;
            }
            line[len] = '\0';
            len = 0;
            printf("%9.2f ms  %s\n", now_ms() - t0, line);
            if (strncmp(line, "OK ", 3) == 0)
                ok++;
            else if (strncmp(line, "ERROR", 5) == 0)
                errors++;
        }
    }
    if (n == SOCKET_ERROR)
        fprintf(stderr, "recv failed: error %d\n", WSAGetLastError());
    printf("%9.2f ms  server closed the connection: %d OK line(s), %d ERROR line(s)\n",
           now_ms() - t0, ok, errors);
    if (errors > 0 || n == SOCKET_ERROR)
        return 1;
    return ok > 0 ? 0 : 2;
}

int main(int argc, char *argv[])
{
    const char *host, *port = "9090", *request = "SYSINFO";
    struct addrinfo hints, *res;
    struct sockaddr_in local;
    int local_len = (int)sizeof(local), rc;
    char addr[INET_ADDRSTRLEN] = "?";
    WSADATA wsa;
    SOCKET s;
    double t0;

    if (argc < 2 || argc > 4) {
        fprintf(stderr, "usage: %s HOST [PORT] [REQUEST]   (default: 9090 SYSINFO)\n", argv[0]);
        return 1;
    }
    host = argv[1];
    if (argc >= 3)
        port = argv[2];
    if (argc == 4)
        request = argv[3];
    if (strlen(request) > MAX_REQUEST) {
        fprintf(stderr, "request: at most %d characters\n", MAX_REQUEST);
        return 1;
    }
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) {
        fprintf(stderr, "cannot resolve %s\n", host);
        WSACleanup();
        return 1;
    }
    t0 = now_ms();
    s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET || connect(s, res->ai_addr, (int)res->ai_addrlen) == SOCKET_ERROR) {
        fprintf(stderr, "cannot connect to %s:%s: error %d\n", host, port, WSAGetLastError());
        freeaddrinfo(res);
        WSACleanup();
        return 1;
    }
    freeaddrinfo(res);
    if (getsockname(s, (struct sockaddr *)&local, &local_len) == 0)
        inet_ntop(AF_INET, &local.sin_addr, addr, sizeof(addr));
    printf("connected %s:%u -> %s:%s in %.2f ms\n", addr, ntohs(local.sin_port), host, port,
           now_ms() - t0);

    rc = run(s, request, t0);
    closesocket(s);
    WSACleanup();
    return rc;
}
