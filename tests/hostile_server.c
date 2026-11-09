/*
 * hostile_server.c - fake NP server for the hostile-peer audit in tests/test_audit.c.
 *
 * It accepts one connection on 127.0.0.1, reads the request, and answers with
 * a well-formed header carrying the given opcode and body length, followed by
 * filler bytes. The filler has no NUL terminator and BODY_LEN may exceed the
 * protocol limit: client.c has to survive all of it.
 *
 * Usage: hostile_server PORT OPCODE BODY_LEN
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define HEADER_SIZE 8
#define MAX_FILLER  8192

int main(int argc, char **argv)
{
    static unsigned char reply[HEADER_SIZE + MAX_FILLER];
    struct timespec pause_time = { 0, 300000000L };
    struct sockaddr_in addr;
    char request[64];
    uint16_t magic = htons(0x4E50);
    uint32_t net_len;
    unsigned long length;
    size_t filler;
    int on = 1;
    int srv;
    int conn;

    if (argc != 4) {
        fprintf(stderr, "usage: %s PORT OPCODE BODY_LEN\n", argv[0]);
        return 2;
    }
    length = strtoul(argv[3], NULL, 0);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)strtoul(argv[1], NULL, 10));

    srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0 || setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0 ||
        bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(srv, 1) != 0) {
        perror("hostile_server");
        return 1;
    }
    conn = accept(srv, NULL, NULL);
    if (conn < 0) {
        perror("hostile_server: accept");
        return 1;
    }
    (void)recv(conn, request, sizeof(request), 0);

    net_len = htonl((uint32_t)length);
    memcpy(reply, &magic, sizeof(magic));
    reply[2] = 1;
    reply[3] = (unsigned char)strtoul(argv[2], NULL, 0);
    memcpy(reply + 4, &net_len, sizeof(net_len));
    filler = length < MAX_FILLER ? length : MAX_FILLER;
    memset(reply + HEADER_SIZE, 'A', filler);

    /* The client may reject the reply and close first: not an error here. */
    (void)send(conn, reply, HEADER_SIZE + filler, MSG_NOSIGNAL);
    nanosleep(&pause_time, NULL);
    close(conn);
    close(srv);
    return 0;
}
