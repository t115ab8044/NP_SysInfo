# NP SysInfo: A Fork-Based Multi-Process System Information Server

Midterm project for C network and system programming.

**Student ID:** 115ab8044 · **Name:** 顏祺宸

A client connects to the server and sends a one-line text request for live system information about the host: hostname, uptime, CPU load average, available memory and number of processes. It can ask once, or ask the server to keep reporting at a fixed interval. The server `fork()`s a child process for each connection, and the helper functions shared by both sides are packaged in the shared library `libutils.so`.

**Use case:** When you manage several Linux hosts, you often need a quick look at each host's load and memory. With `server` running on a host, other people can use `client` to query it once, or watch it change over a few seconds, without logging in to the host.

## Submission Guide

| Item | Location |
|---|---|
| System design explanation | [docs/system-design.md](docs/system-design.md) |
| Description of key source code | [docs/key-source.md](docs/key-source.md) |
| Execution screenshots | [docs/screenshots.md](docs/screenshots.md) |
| Complete source code | This repo; the file list is in [Section 1](#1-project-structure) |
| Real Test | [Section 7](#7-real-test-windows--wsl2): a Windows client connects to the server in WSL2 |
| Auditing discussion | [docs/audit.md](docs/audit.md) |

## Requirements Mapping

| Midterm requirement | How this project meets it |
|---|---|
| The server handles connections with `fork()`; no threads | The parent only `accept()`s and forks one child per connection; the project uses no threads anywhere |
| At least 10 concurrent clients, with runtime proof | `tests/test_concurrent.c` connects 12 clients at once, and `ps aux` and `ps --forest` show 12 child processes alive at the same time |
| Packet capture and analysis | `tests/test_capture.c` captures loopback traffic with tcpdump, reads the pcap file back, lists the three-way handshake, request, reply and FIN packet by packet, and breaks the request packet into its Ethernet, IPv4 and TCP headers, field by field |
| Two-level debug | Compile time `-DENABLE_DEBUG` (when it is off, the macros expand to `((void)0)`), plus runtime `-d 0/1/2` |
| Shared functions in a `.so`, linked by both sides | The logger, `write_all`/`read_line` and the system information helpers are in `libutils.so`; the server and the client both link it dynamically with `-lutils` |
| The client requests system information from the server | `SYSINFO` asks once, `WATCH <count> <ms>` keeps reporting |
| Robustness mechanisms, at least 3 With/Without examples | 5 mechanisms and 5 With/Without comparisons ([Section 5](#5-five-robustness-mechanisms)); each mechanism can be turned off with a flag, and `tests/test_robustness.c` runs the comparisons automatically |
| Built with a Makefile | The `Makefile` has targets for building, the demo and the tests |

---

## 1. Project Structure

### File layout

```text
.
├── Makefile             builds libutils.so, server and client, plus the test targets
├── utils.h / utils.c    libutils.so: logger, socket I/O, system information, misc helpers
├── server.c             fork-based multi-process server; each of the 5 robustness mechanisms can be turned off
├── client.c             client: one query, repeated reports, 3 failure simulations
├── tests/               test programs in C
│   ├── testlib.h / testlib.c   shared helpers: fork + exec, regex filtering, reading /proc
│   ├── demo.c                  one-command live demo (about 3 s)
│   ├── test_concurrent.c       proof of 12 concurrent clients
│   ├── test_robustness.c       With/Without comparisons for the 5 robustness mechanisms
│   ├── test_debug.c            proof of two-level debug (compile time and runtime)
│   ├── test_capture.c          tcpdump capture and header analysis
│   └── win_client.c            Windows client for the Real Test (Winsock)
├── docs/                system design, key source code, execution screenshots and auditing discussion
└── logs/                test results and capture files (rerunning the tests writes the detailed logs)
```

### Process model

```text
 client 1..N ── connect() ──────► server parent   accept → check the limit → fork()
                                    │      ▲
                             fork() │      │ SIGCHLD: the kernel tells the parent a child ended,
                                    ▼      │ and the parent reaps it in a waitpid loop
 client 1 ◄═══ request/reply ═══► child (pid A)   serves client 1, then _exit
 client 2 ◄═════════════════════► child (pid B)   serves client 2, then _exit
   ...                            ...
 client N ◄═════════════════════► child (pid N)
```

- **parent:** Only `accept()`s, `fork()`s and reaps children. The only time it writes to a client is when the connection limit is reached and it replies `ERROR server busy` directly (mechanism [4]).
- **child:** Reads one request line, replies, and calls `_exit()`. Once the connection is set up, requests and replies go directly between the client and the child, not through the parent. The exit code gives the reason the child ended, and the parent writes it to the log when it reaps the child.

### libutils.so modules

| Module | Functions | Description |
|---|---|---|
| Logger | `log_init`, `log_write`, `log_hexdump`, `LOG_INFO/WARN/ERROR`, `LOG_DEBUG`, `LOG_HEXDUMP`, `g_runtime_debug_level` | Each line has the format `HH:MM:SS.mmm [tag:pid] LEVEL msg` and is written with a single `write()`, so lines from several processes writing at once never interleave |
| Socket I/O | `write_all`, `read_line` | `write_all` finishes short writes and retries on `EINTR`; `read_line` reads 1 byte at a time, at most 127 bytes (mechanism [3]) |
| System information | `sysinfo_collect`, `sysinfo_format` | The data comes from `gethostname`, `sysinfo(2)`, `getloadavg` and `/proc/meminfo`, and is formatted as one `OK ...` line |
| Misc | `parse_int_range`, `sleep_ms`, `set_signal` | Command-line integer parsing, a sleep that keeps sleeping after a signal interrupts it, and setting signal handlers with `sigaction` |

`libutils.so` exports 11 symbols in total; `make inspect` lists them.

---

## 2. Build

**Requirements:** Linux (this project is tested on WSL2 Ubuntu), gcc and GNU make. The test programs also use `ps`, `strings`, `objdump`, `size` and `tcpdump`.

```bash
make
```

By default `make` builds the debug version (with `-DENABLE_DEBUG`); the actual compile commands are below. Both the debug and the release build have **0 warnings** (`-Wall -Wextra -pedantic`).

```text
gcc -Wall -Wextra -pedantic -std=gnu11 -O2 -g -DENABLE_DEBUG -fPIC -shared -o libutils.so utils.c
gcc -Wall -Wextra -pedantic -std=gnu11 -O2 -g -DENABLE_DEBUG -o server server.c -L. -Wl,-rpath,'$ORIGIN' -lutils
gcc -Wall -Wextra -pedantic -std=gnu11 -O2 -g -DENABLE_DEBUG -o client client.c -L. -Wl,-rpath,'$ORIGIN' -lutils
```

| Make target | What it does |
|---|---|
| `make`, `make all` | Debug build (`DEBUG=1`, the default) |
| `make release` | Release build (`DEBUG=0`; `LOG_DEBUG` is compiled out completely) |
| `make demo` | One-command live demo (about 3 s): `.so` linking, a query, 12 concurrent child processes, a rejected bad request, no zombie processes |
| `make test` | Runs `test_concurrent`, `test_robustness` and `test_debug` in order |
| `make test-concurrent`, `make test-robust`, `make test-debug` | Run one test each |
| `make test-capture` | tcpdump capture test; needs root or sudo |
| `make inspect` | Lists the symbols `libutils.so` exports and the `ldd` linking result |
| `make capture` | Shows port 9090 traffic live with `sudo tcpdump -i lo -n -x` |
| `make win-client` | Builds `win_client.exe` for the Real Test with mingw-w64 ([Section 7](#7-real-test-windows--wsl2)) |
| `make clean` | Deletes the `.so`, the executables and the test programs |

- **Switching DEBUG rebuilds everything.** The Makefile records the CFLAGS in `.build_flags`, so when you switch between `DEBUG=1` and `DEBUG=0`, the `.so` and both executables are rebuilt together.
- **The library travels with the executables.** `-Wl,-rpath,'$ORIGIN'` makes the executables load the `libutils.so` in their own folder, so they can be started from any directory.

---

## 3. Usage

### 3.1 Requests and replies

The client sends one line of text and the server replies with one or more lines of text; every line ends with `\n`. Each connection handles only one request.

| Client sends | Server replies |
|---|---|
| `SYSINFO` | One line `OK seq=1 ...`, then closes the connection |
| `WATCH <count> <ms>` | One line every `<ms>` milliseconds, `<count>` lines in total (count 1–100, ms 100–10000) |
| Anything else, arguments out of range, more than 127 bytes | One line `ERROR <reason>`, then closes the connection |

```text
OK seq=1 pid=556 host=LAPTOP-0F0NVT4H up=0d 00:00:30 load=0.11/0.03/0.01 mem_avail=7.09/7.61 GiB procs=231
```

`pid=` is the PID of the server child that handles this connection, so the client's output by itself shows that each connection is handled by a different process.

### 3.2 Server

```bash
./server                    # listen on 0.0.0.0:9090 with all 5 mechanisms on
./server -p 9000 -d 1       # use port 9000 and turn on flow debug
./server --no-sigchld       # turn off mechanism [1] to show zombie processes
```

| Option | Description |
|---|---|
| `-p, --port PORT` | Port to listen on (default 9090) |
| `-d, --debug LEVEL` | Runtime debug level: 0 normal messages, 1 adds the flow, 2 also adds hex dumps |
| `-m, --max-clients N` | Mechanism [4]: maximum number of clients served at once (default 32) |
| `--no-sigchld`, `--no-sigpipe`, `--no-validate`, `--no-limit`, `--no-pdeathsig` | Turn off mechanisms [1] to [5] respectively, for the With/Without comparisons |

The parent records each child's exit code when it reaps the child, for example `reaped child 795: exit 3 (client left)`:

| exit code | Meaning |
|---|---|
| 0 | Request served |
| 1 | I/O error |
| 2 | Request rejected (too long, unknown command, bad arguments) |
| 3 | The client has left (`EPIPE` on a write, or it disconnected before sending a complete request) |

If a child is killed by a signal, the log shows `killed by signal 13 (Broken pipe)`.

### 3.3 Client

```bash
./client                      # SYSINFO: query once
./client -n 5 -w 1000         # WATCH 5 1000: one line per second, 5 lines
./client -r HELLO             # send any line and see the ERROR reply
./client --mode abort         # failure simulation, see the table below
```

| Option | Description |
|---|---|
| `-H, --host HOST` | Server address (default 127.0.0.1) |
| `-p, --port PORT` | Server port (default 9090) |
| `-d, --debug LEVEL` | Runtime debug level 0/1/2 |
| `-n, --count N`, `-w, --interval MS` | Send `WATCH N MS` instead (default 5 times, 1000 ms) |
| `-r, --request LINE` | Send LINE as the request |
| `--mode MODE` | Failure simulation, see the table below |
| `--mib N` | Amount of data the `longline` mode sends (default 16 MiB) |

| Failure mode | Behavior | Used to test |
|---|---|---|
| `abort` | Sends `WATCH 10 200` and disconnects as soon as it receives the first line | Mechanism [2]: SIGPIPE |
| `longline` | Sends a 16 MiB request with no newline, then waits for the server's reply | Mechanism [3]: request length limit |
| `idle` | Connects and sends nothing, holding the connection | Mechanism [4]: connection limit |

The client has no timeout: it waits until the server replies or closes the connection. Press `Ctrl+C` to stop it early. The test programs end the `longline` and `idle` clients themselves.

Client exit codes: 0 success, 1 failure or an `ERROR` reply, 2 the connection ended before any reply arrived.

---

## 4. Two-Level Debug Control

| Level | How it is controlled | Effect |
|---|---|---|
| Compile time | `make` (with `-DENABLE_DEBUG`) or `make release` | When it is off, `LOG_DEBUG` and `LOG_HEXDUMP` expand to `((void)0)`: no code, no format strings and no level check |
| Runtime | `-d 0/1/2`, which sets `g_runtime_debug_level` | 0: only INFO, WARN and ERROR<br>1: adds DEBUG flow messages (with `file:line`)<br>2: also adds a hex dump of every request and reply |

```c
#ifdef ENABLE_DEBUG
#define LOG_DEBUG(lvl, ...)                                                 \
    do {                                                                    \
        if (g_runtime_debug_level >= (lvl))                                 \
            log_write(LOG_LVL_DEBUG, __FILE__, __LINE__, __VA_ARGS__);      \
    } while (0)
#else
#define LOG_DEBUG(lvl, ...)           ((void)0)
#endif
```

The release build still accepts `-d`, but warns `built without -DENABLE_DEBUG: -d 2 has no effect`. The request as the client sees it with `-d 2`:

```text
DEBUG request: 8 byte(s)
DEBUG     0000  53 59 53 49 4e 46 4f 0a                           |SYSINFO.|
```

---

## 5. Five Robustness Mechanisms

Each mechanism can be turned off on its own with a flag, and `tests/test_robustness.c` automatically runs both the Without and the With case and compares them.

| # | Failure | Mechanism | Flag to turn it off |
|---|---|---|---|
| [1] | A finished child is never reaped and becomes a zombie | SIGCHLD handler that calls `waitpid(WNOHANG)` in a loop | `--no-sigchld` |
| [2] | Writing to a client that has left kills the child with SIGPIPE | Ignore SIGPIPE and handle the `EPIPE` that `write()` returns | `--no-sigpipe` |
| [3] | An oversized request with no newline makes the child's memory keep growing | Requests are capped at 127 bytes; longer ones get `ERROR request too long` | `--no-validate` |
| [4] | A flood of connections makes the server create unlimited child processes | A cap on the clients served at once; above it the parent replies `ERROR server busy` itself and does not fork | `--no-limit` |
| [5] | After the parent gets `kill -9`, its children become orphans and keep running | `prctl(PR_SET_PDEATHSIG, SIGTERM)` | `--no-pdeathsig` |

[1] and [5] form a pair: a zombie is "the child died and the parent never collected it", an orphan is "the parent died and the child is still running".

### [1] SIGCHLD + a `waitpid` loop

- **Problem:** If the parent never calls `wait` after a child ends, the child keeps its slot in the process table in the `<defunct>` state.
- **Solution:** Register a SIGCHLD handler with `sigaction` and reap in it with `while (waitpid(-1, &status, WNOHANG) > 0)`. Signals do not queue: when several children end at the same time there may be only one SIGCHLD, so the loop is required. The handler only stores the PID and status in a ring buffer and the main loop writes the log, because `printf()` must not be used in a signal handler.
- **When turned off:** SIGCHLD keeps its default action, and every finished child becomes a zombie.

### [2] Ignoring SIGPIPE

- **Problem:** Calling `write()` on a connection that has been closed raises SIGPIPE, whose default action terminates the process at once, without any error handling.
- **Solution:** At startup the parent sets `SIGPIPE` to `SIG_IGN`, and the children inherit it through `fork()`. `write()` then returns -1 with errno `EPIPE` instead; the child logs `client left` and exits with code 3.
- **When turned off:** The child is killed by signal 13. Either way the parent is unaffected and keeps serving new clients.

### [3] Request length limit

- **Problem:** If the server keeps reading until it sees `\n`, a client that never sends a newline can make the child's buffer grow without limit.
- **Solution:** `read_line()` reads 1 byte at a time and stops once it has read 127 bytes without seeing `\n`; the child replies `ERROR request too long` and ends.
- **When turned off:** An unlimited read is used instead, which doubles the buffer each time. A 16 MiB request grows the buffer to 32 MiB, and the child's RSS reaches about 17 MiB.

### [4] Connection limit

- **Problem:** Forking for every connection means that a flood of connections can make the server create unlimited processes. A client that connects and sends nothing also holds a child the whole time.
- **Solution:** The parent counts the children it has not reaped yet. At the limit (`-m`, default 32), the parent replies `ERROR server busy` itself with `send(..., MSG_DONTWAIT | MSG_NOSIGNAL)`, closes the connection and does not fork. The two flags keep the parent from blocking on a client or being killed by SIGPIPE.
- **When turned off:** 10 idle clients hold 10 child processes.

### [5] Child processes exit along with the parent

- **Problem:** `SIGKILL` cannot be caught, so a parent that gets `kill -9` has no chance to tell its children. The children are adopted by another process (on WSL2, that session's `/init`, not necessarily PID 1) and keep running with nobody managing them.
- **Solution:** After the fork, the child calls `prctl(PR_SET_PDEATHSIG, SIGTERM)`, so the kernel sends it `SIGTERM` as soon as the parent dies. It then checks `getppid()` once more, to handle a parent that died between the fork and `prctl`.
- **When turned off:** The child's PPID changes from the parent to another process (from 1903 to 1632 in the log), and the child goes on to send all 10 snapshots.

---

## 6. Tests

The test programs are all written in C and run other programs with `fork` + `exec`, without going through a shell. They start the server themselves, print `[PASS]` or `[FAIL]` for every check, exit with code 0 when all checks pass, and save their full output in `logs/`. The environment variable `PORT` changes the port.

| Test | What it proves | How to run | Result |
|---|---|---|---|
| `tests/test_concurrent.c` | 12 clients connect at the same time, and each connection is handled by a different child | `make test-concurrent` | 9/9 PASS |
| `tests/test_robustness.c` | With/Without comparisons for the 5 robustness mechanisms | `make test-robust` | 11/11 PASS |
| `tests/test_debug.c` | Two-level debug at compile time and runtime | `make test-debug` | 15/15 PASS |
| `tests/test_capture.c` | tcpdump capture, three-way handshake, header fields | `make test-capture` (needs root or sudo) | 10/10 PASS |

### 6.1 test_concurrent: 12 concurrent clients

12 clients send `WATCH 6 500` at the same time, and each connection lasts about 3 seconds. During that time the test takes snapshots of `ps aux` and of the process tree:

```text
    PID    PPID STAT     ELAPSED CMD
    504     484 S+         00:01 ./server -p 9090
    517     504 S+         00:01  \_ ./server -p 9090
    518     504 S+         00:01  \_ ./server -p 9090
    ...                              (12 children in total, all with PPID 504)
server children alive           : 12
established TCP conns on :9090  : 12
```

- **ps proof:** 13 `./server` processes in total (1 parent and 12 children), 12 ESTABLISHED connections, and `active=12` in the server log.
- **PID cross-check:** The `pid=` values received by the 12 clients are all different, and all of them appear in the ps snapshot.
- **Afterwards:** 0 zombies and 0 leftover children; the server log records 12 accepted and 12 reaped.

### 6.2 test_robustness: five With/Without comparisons

The numbers below come from `logs/test_robustness.log`.

| Mechanism | Scenario | Without (off) | With (default) |
|---|---|---|---|
| [1] Reaping children | 3 clients query one after another | **3** `[server] <defunct>` zombies | 0 zombies; the log records 3 reaps |
| [2] Ignoring SIGPIPE | `--mode abort`: disconnects after 1 line | The child is `killed by signal 13 (Broken pipe)` | `client left before snapshot 3/10`, the child exits 3 |
| [3] Length limit | `--mode longline`: 16 MiB with no newline | The buffer grows to **32768 KiB**, child RSS **17808 KiB** | `ERROR request too long` after 127 bytes |
| [4] Connection limit | 10 `--mode idle` clients connect at the same time | **10** child processes | 5 child processes; the other 5 clients get `ERROR server busy` |
| [5] pdeathsig | `kill -9` the parent during `WATCH 10 300` | The child becomes an **orphan** (its PPID changes) and sends all 10 | The child ends with the parent; the client gets only 3/10 |

**Manual demo:** Open two terminal windows: A runs the server and B runs the client.

| Mechanism | Window A | Window B | What to look for |
|---|---|---|---|
| [1] | `./server --no-sigchld` | Run `./client` 3 times, then `ps -o pid,stat,cmd --ppid <parent PID>` | 3 `<defunct>` entries in state `Z` |
| [2] | `./server --no-sigpipe` and `./server` | `./client --mode abort` | Log in A: `killed by signal 13` and `exit 3 (client left)` |
| [3] | `./server --no-validate` and `./server` | `./client --mode longline` | Log in A: `buffer grown to ... KiB` and `request too long` |
| [4] | `./server -m 2` | Start 3 `./client --mode idle &`, then `pkill -x client` when done | The 3rd one gets `ERROR server busy` right away |
| [5] | `./server --no-pdeathsig` and `./server` | `./client -n 20 -w 500`, then `kill -9 <parent PID>` | Whether the client keeps receiving data |

### 6.3 test_debug: two-level debug

| Check | release (`make DEBUG=0`) | debug (`make DEBUG=1`) |
|---|---|---|
| gcc commands with `-DENABLE_DEBUG` | 0/3 | 3/3 |
| Debug-only format strings in the executables (`strings`) | **0** | 4 |
| Logger calls inside `write_all()` (`objdump -d`) | **0** | 2 (`log_write@plt`) |
| Size of the `libutils.so` text section | 8,333 bytes | 8,741 bytes |
| DEBUG lines when run with `-d 2` | 0, with the warning `-d 2 has no effect` | — |

Lines in the server log when the debug build handles one `SYSINFO`:

| Level | Total lines | INFO/WARN | DEBUG | hex dump |
|---|---|---|---|---|
| `-d 0` | 6 | 6 | 0 | 0 |
| `-d 1` | 11 | 6 | 5 (with `file:line`) | 0 |
| `-d 2` | 21 | 6 | 15 | 10 (the request starts with `53 59 53 49 4e 46 4f`, that is `SYSINFO`) |

### 6.4 test_capture: tcpdump capture and header analysis

The test runs `tcpdump -i lo -n -w logs/capture.pcap 'tcp port 9090'` on `lo` and meanwhile sends three requests, `SYSINFO`, `WATCH 3 200` and `HELLO`. Afterwards:

1. `tcpdump -r logs/capture.pcap -n -c 10` lists the first connection packet by packet.
2. `tcpdump -r ... -n -x -c 4` shows the hex of the first 4 packets.
3. The test's built-in pcap reader (24-byte file header, 16-byte record header per packet) parses every packet and breaks the request packet into its header layers:

```text
  #   time      dir   flags         rel seq  rel ack  len  payload
  1    0.000000 C->S  SYN                 0        0    0
  2    0.000011 S->C  SYN+ACK             0        1    0
  3    0.000019 C->S  ACK                 1        1    0
  4    0.000094 C->S  PSH+ACK             1        1    8  SYSINFO\n
  5    0.000097 S->C  ACK                 1        9    0
  6    0.001503 S->C  PSH+ACK             1        9  107  OK seq=1 pid=717 host=LAPTOP-0F0...
  7    0.001526 C->S  ACK                 9      108    0
  8    0.001533 S->C  FIN+ACK           108        9    0
  9    0.001620 C->S  FIN+ACK             9      109    0
  10   0.001647 S->C  ACK               109       10    0

  Ethernet 0x0000:  00 00 00 00 00 00 00 00 00 00 00 00 08 00
  IPv4     0x000e:  45 00 00 3c b8 41 40 00 40 06 84 78 7f 00 00 01
           0x001e:  7f 00 00 01
  TCP      0x0022:  e5 ea 23 82 20 51 df 63 c1 57 7d 2d 80 18 00 40
           0x0032:  fe 30 00 00 01 01 08 0a 00 31 a7 a0 c5 13 a4 5c
  payload  0x0042:  53 59 53 49 4e 46 4f 0a
```

- **Three-way handshake:** SYN → SYN+ACK → ACK.
- **Data:** An 8-byte request (`SYSINFO\n`) and a 107-byte reply (one `OK ...` line).
- **Close:** The server sends FIN first (the child closes as soon as it is done), the client answers with its own FIN, and each side ACKs the other.
- **Header fields:** The IPv4 header is 20 bytes (`45` = version 4, IHL 5), and protocol 6 is TCP; the TCP header is 32 bytes (20 bytes plus the timestamp option), and flags `0x18` = PSH+ACK.

**Opening it in Wireshark:** Open `logs/capture.pcap`, enter the display filter `tcp.port == 9090`, right-click a packet and choose Follow → TCP Stream to see the text of the request and the reply.

**Permissions:** tcpdump needs root. When the test runs as a normal user, it first asks for the password once with `sudo -v`, and after that only tcpdump runs as root. On Windows you can also run it directly as root in WSL (first change to the project folder in the terminal):

```bash
wsl -d Ubuntu -u root -- make test-capture
```

### 6.5 Files in logs/

| File | Contents |
|---|---|
| `test_concurrent.log`, `test_robustness.log`, `test_debug.log`, `test_capture.log` | Full output of the four tests |
| `capture.pcap` | tcpdump capture file; opens in Wireshark |
| `capture_decoded.txt` | Full output of `tcpdump -r capture.pcap -n -x` |
| `real_test.log`, `real_test.pcapng` | Record of the Real Test (output from both sides and the connection statistics) and the capture file from the Windows side |

The detailed server and client logs of each scenario (`robust_*`, `concurrent_*`, `debug_*`, `capture_server.log`) are not in the repo; rerunning the tests creates them in `logs/`.

---

## 7. Real Test: Windows ↔ WSL2

All the tests above run over loopback (`127.0.0.1`), so the packets never leave the kernel. The Real Test connects from **Windows** to the server inside **WSL2** instead: the two sides are different operating systems with their own IP addresses, the packets pass through the Hyper-V virtual network adapter `vEthernet (WSL)`, and Wireshark can capture them on the Windows side.

`client.c` links `libutils.so` and can only run on Linux, so a separate Windows client, [tests/win_client.c](tests/win_client.c), was written. It sends the same one-line request with Winsock and prints every reply line; it has no timeout either.

**Limitation:** Both sides are on the same physical computer and the network is virtual, so there is less delay and packet loss than on a real network.

**Steps:**

1. WSL terminal: build the Windows client (this needs mingw-w64; on Ubuntu install it with `sudo apt install mingw-w64`), look up the WSL IP address, then start the server.

   ```bash
   make win-client && hostname -I
   ```

   ```bash
   ./server -d 1
   ```

2. Wireshark: start capturing on the `vEthernet (WSL (Hyper-V firewall))` interface and enter the display filter `tcp.port == 9090`.
3. Windows PowerShell: go to the project folder and replace `172.17.107.87` below with the IP address found in step 1.

   ```powershell
   .\win_client.exe 172.17.107.87
   .\win_client.exe 172.17.107.87 9090 "WATCH 3 500"
   .\win_client.exe 172.17.107.87 9090 HELLO
   ```

**Results** ([logs/real_test.log](logs/real_test.log)):

```text
PS> .\win_client.exe 172.17.107.87 9090 "WATCH 3 500"
connected 172.17.96.1:49376 -> 172.17.107.87:9090 in 1.33 ms
request: WATCH 3 500
     6.12 ms  OK seq=1 pid=3334 host=LAPTOP-0F0NVT4H up=0d 00:46:59 load=0.00/0.02/0.00 mem_avail=7.04/7.61 GiB procs=212
   508.19 ms  OK seq=2 pid=3334 host=LAPTOP-0F0NVT4H up=0d 00:46:59 load=0.00/0.02/0.00 mem_avail=7.04/7.61 GiB procs=206
  1010.51 ms  OK seq=3 pid=3334 host=LAPTOP-0F0NVT4H up=0d 00:47:00 load=0.00/0.02/0.00 mem_avail=7.04/7.61 GiB procs=206
  1011.47 ms  server closed the connection: 3 OK line(s), 0 ERROR line(s)
```

| Item | Result |
|---|---|
| Addresses | The client is Windows at `172.17.96.1` and the server is WSL2 at `172.17.107.87`, not `127.0.0.1` |
| Three requests | `SYSINFO` gets 1 `OK` line; `WATCH 3 500` gets 3 lines, about 500 ms apart; `HELLO` gets `ERROR unknown request` |
| Server log | Each of the 3 connections is handled by its own child (PIDs 3333, 3334 and 3335); all of them are reaped afterwards, with exit codes 0, 0 and 2 |
| Capture | `logs/real_test.pcapng` has 34 packets over 3 TCP connections; the Ethernet addresses belong to the Hyper-V virtual network adapter (`00:15:5d:...`), and every connection shows the three-way handshake, the request, the reply and a FIN from each side |

---

## 8. Test Environment

- WSL2 Ubuntu, kernel `6.18.33.2-microsoft-standard-WSL2`
- gcc 15.2.0, GNU Make 4.4.1, tcpdump 4.99.6

## 9. Limitations

- **Linux only:** Uses `prctl(PR_SET_PDEATHSIG)`, `sysinfo(2)` and `/proc/meminfo`.
- **IPv4 only:** The server listens on `0.0.0.0` and the client connects with `AF_INET`.
- **No timeouts:** Neither the server nor the client has a timeout. A client that connects and sends nothing holds a child process until it disconnects; mechanism [4] limits how many children such clients can hold. If the server does not reply, the client waits until the user presses `Ctrl+C`.
- **No encryption or authentication:** Anyone who can reach the server can read its system information.
- **`make test-capture` needs root.**
