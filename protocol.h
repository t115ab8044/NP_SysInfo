/*
 * protocol.h - NP SysInfo wire protocol, shared by server and client.
 *
 * Every message is a fixed 8-byte header followed by an optional body.
 * All multi-byte fields are big-endian (network byte order).
 *
 *   Byte:   0      1        2         3        4      5      6      7
 *        +------+------+---------+--------+------+------+------+------+-----------
 *        | magic 0x4E50 | version | opcode |          body_len         | body ...
 *        |  'N'    'P'  |  0x01   |        |    (bytes, <= 4096)       |
 *        +------+------+---------+--------+------+------+------+------+-----------
 *
 * The magic bytes 4E 50 print as ASCII "NP" in `tcpdump -X` and Wireshark,
 * so every message boundary is easy to spot in a loopback capture.
 *
 * Message flow:
 *   client --OP_SYSINFO_REQ (no body)------------------------> server
 *   client <--------------------OP_SYSINFO_RESP (snapshot)---- server
 *
 *   client --OP_SUBSCRIBE_REQ (proto_subscribe_t)------------> server
 *   client <--OP_SYSINFO_RESP x count, one every interval_ms-- server
 *   client --OP_ACK (seq) after every push ------------------> server
 *            (only for count = 0, an endless subscription: the heartbeat
 *             that lets the server notice a frozen client, mechanism [7])
 *
 *   client --OP_DETAIL_REQ (no body)-------------------------> server
 *   client <--------------------OP_DETAIL_RESP (details)------ server
 *
 *   invalid request -> server replies OP_ERROR (proto_error_t) and closes.
 *   server full     -> the parent replies OP_ERROR(ERR_BUSY) without forking.
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

#define PROTO_MAGIC         0x4E50  /* "NP" */
#define PROTO_VERSION       0x01
#define PROTO_HEADER_SIZE   8
#define PROTO_MAX_BODY      4096    /* larger body_len is rejected (OOM guard) */
#define PROTO_DEFAULT_PORT  9090

/* Requests use 0x01-0x7F, replies 0x80-0xFF. */
enum proto_opcode {
    OP_SYSINFO_REQ   = 0x01,    /* body: none */
    OP_SUBSCRIBE_REQ = 0x02,    /* body: proto_subscribe_t */
    OP_DETAIL_REQ    = 0x03,    /* body: none */
    OP_ACK           = 0x04,    /* body: proto_ack_t, heartbeat during a subscription */
    OP_SYSINFO_RESP  = 0x81,    /* body: proto_sysinfo_t */
    OP_DETAIL_RESP   = 0x83,    /* body: proto_detail_t */
    OP_ERROR         = 0xFF     /* body: proto_error_t */
};

/* Carried in proto_error_t.code */
enum proto_errcode {
    ERR_BAD_MAGIC   = 1,
    ERR_BAD_VERSION = 2,
    ERR_BAD_LENGTH  = 3,
    ERR_UNKNOWN_OP  = 4,
    ERR_INTERNAL    = 5,
    ERR_BUSY        = 6,        /* connection limit reached, try again later */
    ERR_HEARTBEAT   = 7,        /* no OP_ACK in time: client frozen or unreachable */
    ERR_DEADLINE    = 8         /* request not complete in time: slow client */
};

typedef struct __attribute__((packed)) {
    uint16_t magic;             /* PROTO_MAGIC */
    uint8_t  version;           /* PROTO_VERSION */
    uint8_t  opcode;            /* enum proto_opcode */
    uint32_t body_len;          /* body size in bytes, header excluded */
} proto_header_t;

/* OP_SUBSCRIBE_REQ body: the server pushes one snapshot every interval_ms. */
typedef struct __attribute__((packed)) {
    uint32_t interval_ms;
    uint32_t count;             /* number of pushes; 0 = until the client leaves */
} proto_subscribe_t;

#define PROTO_HOSTNAME_LEN  64

/*
 * OP_SYSINFO_RESP body: one snapshot of the server host. Every field sits at
 * its natural alignment, so the layout is the same with or without `packed`.
 */
typedef struct __attribute__((packed)) {
    uint32_t server_pid;                    /* forked child serving this client */
    uint32_t seq;                           /* 0 = one-shot reply, 1..N = push */
    char     hostname[PROTO_HOSTNAME_LEN];  /* NUL-terminated */
    uint64_t uptime_sec;
    uint64_t mem_total_kb;
    uint64_t mem_free_kb;
    uint64_t mem_avail_kb;
    uint32_t load_x100[3];                  /* 1/5/15-min load average x 100 */
    uint32_t nprocs;                        /* tasks reported by sysinfo(2) */
} proto_sysinfo_t;

/* OP_ACK body: the seq of the push being acknowledged. */
typedef struct __attribute__((packed)) {
    uint32_t seq;
} proto_ack_t;

/*
 * OP_DETAIL_RESP body: slower-changing details of the server host. CPU usage
 * is measured over a short sampling window, so this request takes ~200 ms.
 */
typedef struct __attribute__((packed)) {
    uint32_t server_pid;
    uint32_t ncpu;                          /* online CPUs */
    uint32_t cpu_busy_x100;                 /* CPU busy % x 100 over sample_ms */
    uint32_t sample_ms;
    char     sysname[32];                   /* uname(2), e.g. "Linux" */
    char     release[64];                   /* kernel release */
    char     machine[32];                   /* e.g. "x86_64" */
    char     cpu_model[64];                 /* /proc/cpuinfo "model name" */
    uint64_t disk_total_kb;                 /* statvfs("/") */
    uint64_t disk_avail_kb;
} proto_detail_t;

#define PROTO_ERRMSG_LEN    60

typedef struct __attribute__((packed)) {
    uint32_t code;                          /* enum proto_errcode */
    char     message[PROTO_ERRMSG_LEN];     /* NUL-terminated */
} proto_error_t;

_Static_assert(sizeof(proto_header_t) == PROTO_HEADER_SIZE,
               "protocol header must be exactly 8 bytes on the wire");
_Static_assert(sizeof(proto_subscribe_t) == 8, "unexpected proto_subscribe_t size");
_Static_assert(sizeof(proto_sysinfo_t) == 120, "unexpected proto_sysinfo_t size");
_Static_assert(sizeof(proto_ack_t) == 4, "unexpected proto_ack_t size");
_Static_assert(sizeof(proto_detail_t) == 224, "unexpected proto_detail_t size");
_Static_assert(sizeof(proto_error_t) == 64, "unexpected proto_error_t size");

#endif /* PROTOCOL_H */
