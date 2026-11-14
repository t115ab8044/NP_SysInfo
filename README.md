# NP SysInfo: A Fork-Based Multi-Process System Information Server

Midterm project for C Network and System Programming.

**Student ID:** 115ab8044 · **Name:** 顏祺宸

A client connects to the server and sends a one-line text request for the host's live system information: hostname, uptime, CPU load average, available memory, and the total number of tasks (each thread counts as one). It can query once, or ask the server to report repeatedly at a fixed interval. The server `fork()`s a child process for each connection, and the utility functions both sides share are packaged in the shared library `libutils.so`.

**Use case:** When you manage several Linux hosts, you often need a quick look at each host's load and memory. With `server` running on a host, other people can use `client` to query it once, or watch it change over a few seconds, without logging in to the host.

## Submission Guide

| Item | Location |
|---|---|
| System design explanation | [docs/system-design.md](docs/system-design.md) |
| Description of key source code | [docs/key-source.md](docs/key-source.md) |
| Execution screenshots | [docs/screenshots.md](docs/screenshots.md) |
| Complete source code | This repo; the file list is in [Section 1](#1-project-structure) |

## Requirements Mapping

| Midterm requirement | How this project meets it |
|---|---|
| The server handles connections with `fork()`; threads are not allowed | The parent only `accept()`s and forks one child per connection; the project uses no threads anywhere |
| Handle at least 10 clients at once, with runtime proof | `tests/test_concurrent.c` connects 12 clients at once, each sending a different `WATCH` request, and uses `ps aux` and `ps --forest` to show 12 child processes alive at the same time |
| Packet capture and analysis | `tests/test_capture.c` captures loopback traffic with tcpdump, reads the pcap file back, lists the three-way handshake, request, reply and FIN packet by packet, keeps only the SYN packets with `tcp[13] & 2 == 2`, breaks the request packet into its Ethernet, IPv4 and TCP headers field by field, and captures a connection the client half-closes with `--half-close` |
| Two-level debug | Compile-time `-DENABLE_DEBUG` (when it is off, the macros expand to `((void)0)`), plus runtime `-d 0/1/2` |
| Shared functions packaged in a `.so` that both sides link | The logger, `write_all`/`read_line` and system information collection are in `libutils.so`; the server and the client both link it dynamically with `-lutils` |
| The client requests system information from the server | `SYSINFO` queries once; `WATCH <count> <ms>` reports repeatedly |
| Robustness mechanisms, with at least 3 With/Without runs | 5 kinds of failure, 7 mechanisms, 7 With/Without pairs ([Section 5](#5-seven-robustness-mechanisms)); each mechanism can be turned off with a flag, and `tests/test_robustness.c` runs the comparisons automatically |
| Build with a Makefile | The `Makefile` provides targets for building, the demo and the tests |

---

## 1. Project Structure

### File layout

```text
.
├── Makefile             builds libutils.so, server, client, and the test targets
├── utils.h / utils.c    libutils.so: logger, socket I/O, system information, misc helpers
├── server.c             fork-based multi-process server; each of the 7 mechanisms can be turned off
├── client.c             client: one-shot query, repeated reports, 4 failure simulations
├── tests/               C test programs
│   ├── testlib.h / testlib.c   shared helpers: fork + exec, regex filtering, reading /proc
│   ├── demo.c                  one-command live demo (about 3 seconds)
│   ├── test_concurrent.c       proof of 12 concurrent clients
│   ├── test_robustness.c       With/Without comparisons for the 7 robustness mechanisms
│   ├── test_debug.c            proof of two-level debug (compile time and runtime)
│   └── test_capture.c          tcpdump capture and header analysis
├── docs/                system design, key source code and execution screenshots
└── logs/                test results and the capture file (rerunning the tests writes detailed logs)
```

### Process model

```text
 client 1..N ── connect() ──────► server parent   accept → check the limit → fork()
                                    │      ▲
                             fork() │      │ SIGCHLD: the kernel notifies the parent when a child exits,
                                    ▼      │ and the parent reaps with a waitpid loop
 client 1 ◄═══ request/reply ═══► child (pid A)   serves client 1, then _exit
 client 2 ◄═════════════════════► child (pid B)   serves client 2, then _exit
   ...                            ...
 client N ◄═════════════════════► child (pid N)
```

- **parent:** Only calls `accept()` and `fork()` and reaps children. The only time it writes to a client is when the connection limit (`-m`, default 32) is reached; then it replies `ERROR server busy` itself.
- **child:** Reads one request line, replies, and calls `_exit()`. Once the connection is set up, requests and replies go directly between the client and the child, not through the parent. The exit code gives the reason the child ended, and the parent writes it to the log when it reaps the child.

### libutils.so modules

| Module | Functions | Description |
|---|---|---|
| Logger | `log_init`, `log_write`, `log_hexdump`, `LOG_INFO/WARN/ERROR`, `LOG_DEBUG`, `LOG_HEXDUMP`, `g_runtime_debug_level` | Each line has the form `HH:MM:SS.mmm [tag:pid] LEVEL msg` and is written with a single `write()`, so lines from several processes writing at once never interleave |
| Socket I/O | `write_all`, `read_line`, `read_line_timeout` | `write_all` completes short writes and retries on `EINTR`; `read_line` reads 1 byte at a time, up to 127 bytes (mechanism [3]); `read_line_timeout` adds a deadline for the whole line (mechanism [4]) |
| System information | `sysinfo_collect`, `sysinfo_format` | The data comes from `gethostname`, `sysinfo(2)`, `getloadavg` and `/proc/meminfo`, formatted into one `OK ...` line |
| Misc | `parse_int_range`, `sleep_ms`, `set_signal` | Command-line integer parsing, a sleep that keeps sleeping after a signal interrupts it, and setting signals with `sigaction` |

`libutils.so` exports 12 symbols in total; `make inspect` lists them.

---

## 2. Build

**Requirements:** Linux (this project was tested on WSL2 Ubuntu), gcc and GNU make. The tests also use `ps`, `strings`, `objdump`, `size`, `tcpdump`, and `tc` (iproute2; the test for mechanism [2b] uses it to cut the network).

```bash
make
```

`make` builds the debug version by default (with `-DENABLE_DEBUG`). The actual compile commands are below. Both the debug and the release build have **0 warnings** (`-Wall -Wextra -pedantic`).

```text
gcc -Wall -Wextra -pedantic -std=gnu11 -O2 -g -DENABLE_DEBUG -fPIC -shared -o libutils.so utils.c
gcc -Wall -Wextra -pedantic -std=gnu11 -O2 -g -DENABLE_DEBUG -o server server.c -L. -Wl,-rpath,'$ORIGIN' -lutils
gcc -Wall -Wextra -pedantic -std=gnu11 -O2 -g -DENABLE_DEBUG -o client client.c -L. -Wl,-rpath,'$ORIGIN' -lutils
```

| Make target | What it does |
|---|---|
| `make`, `make all` | Debug build (`DEBUG=1`, the default) |
| `make release` | Release build (`DEBUG=0`; `LOG_DEBUG` is compiled out entirely) |
| `make demo` | One-command live demo (about 3 seconds): `.so` linking, a query, 12 clients each sending a different request (12 concurrent child processes), rejecting bad requests, no zombie processes |
| `make test` | Runs `test_concurrent`, `test_robustness` and `test_debug` in order |
| `make test-concurrent`, `make test-robust`, `make test-debug` | Runs one test |
| `make test-capture` | tcpdump capture test; needs root or sudo |
| `make inspect` | Lists the symbols `libutils.so` exports and the `ldd` linking result |
| `make capture` | Shows port 9090 traffic live with `sudo tcpdump -i lo -l -vn -e -x 'tcp port 9090'`: `-l` prints line by line, `-v` shows the IP header's tos/ttl/id/checksum, `-n` skips name lookups, `-e` shows the MAC addresses and EtherType, `-x` shows hex |
| `make clean` | Removes the `.so`, the executables and the test programs |

- **Switching DEBUG rebuilds everything automatically.** The Makefile records CFLAGS in `.build_flags`, so when you switch between `DEBUG=1` and `DEBUG=0`, the `.so` and both executables are rebuilt together.
- **The library travels with the executables.** `-Wl,-rpath,'$ORIGIN'` makes each executable load the `libutils.so` in its own folder, so it can be started from any directory.

---

## 3. Usage

### 3.1 Requests and replies

The client sends one line of text and the server replies with one or more lines of text. Every line ends with `\n`, and each connection handles one request.

| Client sends | Server replies |
|---|---|
| `SYSINFO` | One `OK seq=1 ...` line, then closes the connection |
| `WATCH <count> <ms>` | One line every `<ms>` milliseconds, `<count>` lines in total (count 1–100, ms 100–10000) |
| Anything else, arguments out of range, more than 127 bytes, or a line not finished within 3 seconds | One `ERROR <reason>` line, then closes the connection |

```text
OK seq=1 pid=556 host=LAPTOP-0F0NVT4H up=0d 00:00:30 load=0.11/0.03/0.01 mem_avail=7.09/7.61 GiB procs=231
```

`pid=` is the PID of the server child process that handles this connection, so the client's output on its own shows that each connection is handled by a different process.

### 3.2 Server

```bash
./server                    # listen on 0.0.0.0:9090 with all 7 mechanisms on
./server -p 9000 -d 1       # use port 9000 and turn on flow debug
./server --reap-once        # turn off mechanism [1] to show the zombies a simultaneous disconnect leaves
```

| Option | Description |
|---|---|
| `-p, --port PORT` | Port to listen on (default 9090) |
| `-d, --debug LEVEL` | Runtime debug level: 0 normal messages, 1 adds the flow, 2 also adds hex dumps |
| `-m, --max-clients N` | Maximum number of clients served at once (default 32); beyond it the parent replies `ERROR server busy` itself and does not fork |
| `--no-limit` | No limit on the number of clients served at once |
| `--reap-once`, `--no-sigpipe`, `--no-user-timeout`, `--no-validate`, `--no-deadline`, `--no-pdeathsig`, `--no-graceful` | Turn off mechanisms [1], [2a], [2b], [3], [4], [5a] and [5b] respectively, for the With/Without comparisons ([Section 5](#5-seven-robustness-mechanisms)) |

When you press `Ctrl+C` in the terminal, the server stops accepting new connections and waits for the child processes still serving to finish before it exits (up to 30 seconds); pressing it again exits at once (mechanism [5b]).

The parent records each child's exit code when it reaps the child, for example `reaped child 795: exit 3 (client left)`:

| Exit code | Meaning |
|---|---|
| 0 | Request served |
| 1 | I/O error |
| 2 | Request rejected (too long, unknown command, bad arguments) |
| 3 | The client is already gone (`EPIPE` or `ETIMEDOUT` on a write, or it disconnected before finishing the request) |
| 4 | Request timed out (a line was not finished within 3 seconds) |

If a signal killed the child, the log shows `killed by signal 13 (Broken pipe)`.

### 3.3 Client

```bash
./client                      # SYSINFO: query once
./client -n 5 -w 1000         # WATCH 5 1000: one line per second, 5 lines
./client -r HELLO             # send any line and see the ERROR reply
./client -n 3 -w 200 --half-close   # half-close right after the request; still receives 3 lines
./client --mode abort         # failure simulation, see the table below
```

| Option | Description |
|---|---|
| `-H, --host HOST` | Server address (default 127.0.0.1) |
| `-p, --port PORT` | Server port (default 9090) |
| `-d, --debug LEVEL` | Runtime debug level 0/1/2 |
| `-n, --count N`, `-w, --interval MS` | Send `WATCH N MS` instead (default 5 times, 1000 ms); in `slow` mode `-w` is the gap between two bytes (default 500 ms) |
| `-r, --request LINE` | Send LINE as the request |
| `--half-close` | Call `shutdown(fd, SHUT_WR)` after sending the request: closes only the sending direction (sends a FIN) and keeps reading the reply |
| `--mode MODE` | Failure simulation, see the table below |
| `--mib N` | Amount of data the `longline` mode sends (default 16 MiB) |

| Failure mode | Behavior | Used to test |
|---|---|---|
| `abort` | Sends `WATCH 10 200` and disconnects after the first line | Mechanism [2a]: SIGPIPE |
| `longline` | Sends a 16 MiB request with no newline, then waits for the server's reply | Mechanism [3]: request validation |
| `slow` | Slowloris: sends `SYSINFO` 1 byte every 0.5 seconds and never sends a newline (after `SYSINFO` it sends spaces) | Mechanism [4]: request deadline |
| `idle` | Connects and sends nothing, holding the connection | Mechanism [1]: keeps 12 child processes waiting at once, then disconnects them all together |

The client has no timeout: it waits until the server replies or closes the connection. Press `Ctrl+C` to stop it early. The tests stop the `longline`, `slow` and `idle` clients themselves.

Client exit codes: 0 success, 1 failure or an `ERROR` reply, 2 the connection ended before any reply arrived.

---

## 4. Two-Level Debug Control

| Level | How it is controlled | Effect |
|---|---|---|
| Compile time | `make` (with `-DENABLE_DEBUG`) or `make release` | When it is off, `LOG_DEBUG` and `LOG_HEXDUMP` expand to `((void)0)`: no code, no format strings, and no level check |
| Runtime | `-d 0/1/2`, which sets `g_runtime_debug_level` | 0: INFO, WARN and ERROR only<br>1: adds DEBUG flow messages (with `file:line`)<br>2: also adds a hex dump of every request and reply |

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

## 5. Seven Robustness Mechanisms

5 kinds of failure, 7 mechanisms in total. Each mechanism can be turned off on its own with a flag, and `tests/test_robustness.c` automatically runs both the Without and the With case and compares them.

| # | Failure | Mechanism | Flag to turn it off |
|---|---|---|---|
| [1] | 12 clients disconnect at once, so 12 child processes exit at once | The SIGCHLD handler calls `waitpid(WNOHANG)` in a loop and reaps them all in one go | `--reap-once` |
| [2a] | After the client closes normally, the child is still writing and is killed by SIGPIPE | Ignore SIGPIPE and handle the `EPIPE` that `write()` returns | `--no-sigpipe` |
| [2b] | The client's network suddenly vanishes (like a phone going into an elevator) without sending a FIN or RST | `TCP_USER_TIMEOUT`: give up the connection when sent data has no ACK for 3 seconds | `--no-user-timeout` |
| [3] | Malicious requests: 16 MiB with no newline, a number that overflows, a 0 ms interval, a request that would run for 27.8 hours | A 127-byte length limit, width-limited `%3d %5d`, and range checks | `--no-validate` |
| [4] | Slowloris: 1 byte every 0.5 seconds, never finished, filling all 32 slots | The whole request line must arrive within 3 seconds | `--no-deadline` |
| [5a] | The parent gets `kill -9`, and its child processes become orphans that keep running | `prctl(PR_SET_PDEATHSIG, SIGTERM)` | `--no-pdeathsig` |
| [5b] | Pressing `Ctrl+C` in the terminal cuts off a client that is watching `WATCH` | Graceful shutdown: stop accepting new connections and wait for the child processes to finish before exiting | `--no-graceful` |

These mechanisms come in pairs. Of [1] and [5a], one is "the child died and the parent did not reap it" (a zombie) and the other is "the parent died and the child lives on" (an orphan). [2a] and [2b] are both a client leaving midway; they differ in whether the client gets the chance to tell the server. [5a] and [5b] are both the server ending: in one it is killed, in the other someone asks it to stop.

### [1] SIGCHLD + a `waitpid` loop: signals do not queue

- **Problem:** When a child process exits, the kernel sends SIGCHLD to the parent. But signals do not queue: if another child exits before SIGCHLD is handled, it only sets the same pending bit again. When 12 clients disconnect at once, the parent may receive SIGCHLD only 2 or 3 times.
- **Solution:** The handler runs `while (waitpid(-1, &status, WNOHANG) > 0)` to reap every child that has exited, however many this SIGCHLD stands for. The handler only stores the PID and status in a ring buffer, and the main loop writes the log, because `printf()` cannot be used inside a signal handler.
- **Turned off (`--reap-once`):** The code switches to the common mistake of calling `waitpid` only once per SIGCHLD. In the test only 2 of the 12 child processes are reaped and the other 10 become `<defunct>`; the parent's count stays at `active=10`, as if those clients were still connected, and those 10 slots never come back.

### [2a] Ignoring SIGPIPE: the client leaves normally

- **Problem:** After the client closes the connection, the child's next `write()` triggers SIGPIPE, whose default action is to terminate the process at once, with no error handling at all.
- **Solution:** At startup the parent sets `SIGPIPE` to `SIG_IGN`, and the child processes inherit it through `fork()`. `write()` then returns -1 with errno set to `EPIPE` instead, and the child logs `client left` and exits with code 3.
- **Turned off:** Signal 13 kills the child. Either way the parent is unaffected and keeps serving new clients.

### [2b] `TCP_USER_TIMEOUT`: the client's network suddenly vanishes

- **Problem:** When the client closes normally, the kernel sends a FIN and the server finds out quickly ([2a]). But when a phone goes into an elevator, the Wi-Fi drops, or a network cable is pulled, no packet is sent at all. `write()` then only copies the data into the kernel's send buffer and still succeeds; the child thinks everything is fine and keeps sending to a client that is no longer there.
- **Solution:** The child sets `setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, 3000)` on the connection: when sent data has gone more than 3 seconds without an ACK, the kernel gives up the connection, the next `write()` returns `ETIMEDOUT`, and the child logs `client unreachable` and exits with code 3.
- **Turned off:** After the client receives 2 snapshots, the test drops every packet with `tc qdisc add dev lo root netem loss 100%`. The child "sends" for another 9 seconds and gets all 20 snapshots out, and the log says `exit 0 (request served)`, but the client received only 2. With the mechanism on, the child gets `ETIMEDOUT` 4.0 seconds after the cut and stops. It takes longer than 3 seconds because the 3 seconds start from the first data sent after the cut, and after the timeout the child only finds out at its next `write()`, which comes every 0.5 seconds.
- **Test environment:** `tc` needs root. The test first creates its own network namespace with `unshare(CLONE_NEWUSER | CLONE_NEWNET)` (the same as `unshare -rn`, no root needed) and works only on the loopback inside it, so the system's network is not affected.

### [3] Request validation: four malicious requests

- **Problem:** Requests come from the network, so anything can arrive. The test sends 4 requests to the same server:
  - (a) 16 MiB with no newline: reading until `\n` would make the buffer grow without limit.
  - (b) `WATCH 4294967297 100`: `sscanf`'s `%d` does not check for overflow; glibc keeps only the low 32 bits, so 4294967297 (2^32 + 1) becomes 1.
  - (c) `WATCH 100 0`: without a range check, a 0 ms interval makes 100 lines pour out at once.
  - (d) `WATCH 100000 1000`: one request holds a child process for 100000 seconds, about 27.8 hours.
- **Solution:** `read_line()` reads at most 127 bytes; `sscanf(line, "%15s %3d %5d %1s", ...)` limits the number of digits with field widths, and `%1s` catches anything extra; the count must be in 1–100 and the interval in 100–10000 ms. A request that fails gets an `ERROR` reply and the child exits.
- **Turned off:** The code switches to an unbounded read and a plain `%d` with no field width and no range check, and all 4 requests are accepted: (a) the buffer doubles up to 32 MiB and the child's RSS is about 17 MiB; (b) it is treated as `WATCH 1 100`; (c) 100 lines go out within 22 ms; (d) the child is still sending after 1.5 seconds.

### [4] Request deadline: Slowloris

- **Problem:** A Slowloris attack sends 1 byte every so often, so the connection always shows activity but the request never finishes. Each such connection holds a child process, 32 of them fill the `-m 32` limit, and normal clients only get `ERROR server busy`. Limiting only the wait "between two reads" cannot stop it, because each byte arrives quickly.
- **Solution:** The whole request line has an overall deadline: once the child starts reading the request, it must read a `\n` within 3 seconds. Before reading each byte, `read_line_timeout()` uses `poll()` to wait for "the time that is left", so however slowly the client sends, it cannot extend the deadline. On a timeout the child replies `ERROR request timeout` and exits with code 4.
- **Turned off:** 32 `--mode slow` clients still hold 32 child processes after 4 seconds, and the 33rd client, a normal one, gets `ERROR server busy`. With the mechanism on, all 32 are cut off at the 3-second mark and the normal client is served right after.

### [5a] Child processes exit along with the parent

- **Problem:** `SIGKILL` cannot be caught, so when the parent gets `kill -9` it has no chance to tell its child processes. They are re-parented to another process (on WSL2 that is the session's `/init`, not necessarily PID 1) and keep running with nobody managing them.
- **Solution:** After the fork, the child calls `prctl(PR_SET_PDEATHSIG, SIGTERM)`, so the moment the parent dies the kernel sends it `SIGTERM`. After setting it, the child checks `getppid()` once more, to handle a parent that died between the fork and the `prctl`.
- **Turned off:** The child's PPID changes from the parent to another process, and it goes on to send all 10 snapshots.

### [5b] Graceful shutdown on `Ctrl+C`

- **Problem:** Pressing `Ctrl+C` in the terminal sends SIGINT to the whole foreground process group, so the parent and every child process are killed together, and a client watching `WATCH` is cut off.
- **Solution:** The parent installs a SIGINT handler and the child processes set SIGINT to `SIG_IGN`, so the parent decides when to exit. On the first `Ctrl+C`, the handler calls `close()` on the listening socket directly (`close` is async-signal-safe): new clients get connection refused at once, and the signal is not missed even if it lands just before `accept()`. The parent then waits with `waitpid()` for the child processes still serving to finish, and exits with code 0.
- **Working with [5a]:** The parent is still alive while it waits, so pdeathsig does not kill the child processes early. After at most 30 seconds (`alarm`), or on a second `Ctrl+C`, the parent exits at once and [5a]'s pdeathsig takes care of the remaining child processes. `kill` (SIGTERM) does not go through graceful shutdown; the parent exits at once.
- **Turned off:** The client is cut off after receiving only 3 of 10 snapshots, and signal 2 kills the server. With the mechanism on, new clients are refused, the client being served receives all 10 snapshots, and the server exits with code 0 1.9 seconds after `Ctrl+C`.

---

## 6. Tests

The tests are all written in C and run programs with `fork` + `exec`, without a shell. They start the server themselves, print `[PASS]` or `[FAIL]` for each check, exit with code 0 when everything passes, and save their full output in `logs/`. The environment variable `PORT` changes the port.

| Test | What it proves | How to run | Result |
|---|---|---|---|
| `tests/test_concurrent.c` | 12 clients connect at once, each sending a different request, and each connection is handled by a different child process | `make test-concurrent` | 9/9 PASS |
| `tests/test_robustness.c` | With/Without comparisons for the 7 robustness mechanisms | `make test-robust` | 18/18 PASS |
| `tests/test_debug.c` | Two-level debug at compile time and at runtime | `make test-debug` | 15/15 PASS |
| `tests/test_capture.c` | tcpdump capture, three-way handshake, SYN filter, header fields, half close | `make test-capture` (needs root or sudo) | 12/12 PASS |

### 6.1 test_concurrent: 12 concurrent clients

12 clients connect at once, each sending a different request, from `WATCH 2 3000` to `WATCH 31 100`, with different counts and intervals. For every request, "(count − 1) × interval" is 3000 ms, so every connection lasts 3 seconds. During that time the test takes a snapshot of `ps aux` and the process tree (taken from `logs/test_concurrent.log`):

```text
    PID    PPID STAT     ELAPSED CMD
  18160   18140 S+         00:01 ./server -p 9090
  18173   18160 S+         00:01  \_ ./server -p 9090
  18174   18160 S+         00:01  \_ ./server -p 9090
    ...                              (12 children in all, every PPID is 18160)
server children alive           : 12
established TCP conns on :9090  : 12
```

After the clients finish, the test lists each client's request, how many snapshots it received, and which child replied:

```text
[client 01] WATCH 2 3000  ->  2 snapshot(s), served by pid=18173
[client 02] WATCH 3 1500  ->  3 snapshot(s), served by pid=18176
[client 03] WATCH 4 1000  ->  4 snapshot(s), served by pid=18182
[client 04] WATCH 5 750   ->  5 snapshot(s), served by pid=18179
[client 05] WATCH 6 600   ->  6 snapshot(s), served by pid=18184
[client 06] WATCH 7 500   ->  7 snapshot(s), served by pid=18180
[client 07] WATCH 9 375   ->  9 snapshot(s), served by pid=18181
[client 08] WATCH 11 300  -> 11 snapshot(s), served by pid=18175
[client 09] WATCH 13 250  -> 13 snapshot(s), served by pid=18177
[client 10] WATCH 16 200  -> 16 snapshot(s), served by pid=18174
[client 11] WATCH 21 150  -> 21 snapshot(s), served by pid=18178
[client 12] WATCH 31 100  -> 31 snapshot(s), served by pid=18183
```

- **ps proof:** 13 `./server` processes in total (1 parent, 12 children), 12 ESTABLISHED connections, and `active=12` in the server log.
- **12 different requests handled at once:** Every client receives exactly the number of snapshots it asked for (2 to 31, 128 in total); the server log records the request each child received.
- **PID cross-check:** The 12 `pid=` values the clients receive are all different, and every one of them is in the ps snapshot.
- **Afterwards:** 0 zombies and 0 leftover child processes; the server log records 12 accepted and 12 reaped.

### 6.2 test_robustness: seven With/Without comparisons

The numbers below come from `logs/test_robustness.log`. The test enters its own network namespace right at the start (see [2b]), so it does not compete for the port with a server already running on the system.

| Mechanism | Scenario | Without (off) | With (default) |
|---|---|---|---|
| [1] waitpid loop | 12 `--mode idle` clients get `kill -9` at once | Only 2 reaped, **10** `<defunct>` zombies, and the parent still counts `active=10` | All 12 reaped, 0 zombies |
| [2a] Ignore SIGPIPE | `--mode abort`: disconnects after 1 line | The child is `killed by signal 13 (Broken pipe)` | `client left before snapshot 3/10`, the child exits with code 3 |
| [2b] TCP_USER_TIMEOUT | After `WATCH 20 500` delivers 2 snapshots, `tc netem loss 100%` drops every packet | The child sends for another **9.0 seconds**, the log says `exit 0 (request served)`, and the client receives only 2/20 | `write()` returns `ETIMEDOUT` **4.0 seconds** after the cut, and the child exits with code 3 |
| [3] Request validation | 4 malicious requests (see Section 5) | **All 4 are accepted**: RSS 17844 KiB, 4294967297 becomes 1, 100 lines take only 22 ms, and a request that would run for 27.8 hours | All 4 get `ERROR` |
| [4] Request deadline | 32 `--mode slow` clients, then 1 normal client 4 seconds later | 32 child processes are still held, and the normal client gets `ERROR server busy` | The 32 get `ERROR request timeout` at the 3-second mark, and the normal client is served |
| [5a] pdeathsig | `kill -9` the parent during `WATCH 10 300` | The child becomes an **orphan** (its PPID changes) and sends all 10 snapshots | The child exits too, and the client receives only 3/10 |
| [5b] Graceful shutdown | During `WATCH 10 300`, send SIGINT to the server's process group (the same as `Ctrl+C`) | The server and its child processes are killed at once, and the client receives only **3/10** | New connections are refused, the client receives **10/10**, and the server exits with code 0 after 1.9 seconds |

**Manual demo:** Open two terminal windows: A runs the server, B runs the client.

| Mechanism | Window A | Window B | What to look for |
|---|---|---|---|
| [1] | `./server --reap-once` and `./server` | `for i in $(seq 12); do ./client --mode idle & done`, `pkill -KILL -x client` 1 second later, then `ps -o pid,stat,cmd --ppid <parent PID>` | Off: several `<defunct>` processes in state `Z` are left; on: none |
| [2a] | `./server --no-sigpipe` and `./server` | `./client --mode abort` | The log in A: `killed by signal 13` versus `exit 3 (client left)` |
| [2b] | Enter your own network namespace with `unshare -rn bash`, run `ip link set lo up`, then start `./server --no-user-timeout &` or `./server &` in the background | In the same window: `./client -n 20 -w 500 &`, and after 2 snapshots arrive, `tc qdisc add dev lo root netem loss 100%` | Off: `exit 0 (request served)` appears in the log after about 9 seconds; on: `client unreachable` appears after about 4 seconds |
| [3] | `./server --no-validate` and `./server` | `./client --mode longline`, `./client -r "WATCH 4294967297 100"`, `./client -r "WATCH 100 0"`, `./client -r "WATCH 100000 1000"` | Off: all 4 are accepted; on: all 4 get `ERROR` |
| [4] | `./server --no-deadline` and `./server` | `for i in $(seq 32); do ./client --mode slow & done`, `./client` 4 seconds later, then `pkill -x client` when you are done | Off: the last `./client` gets `ERROR server busy`; on: the 32 get `ERROR request timeout` at the 3-second mark and the last `./client` receives data normally |
| [5a] | `./server --no-pdeathsig` and `./server` | `./client -n 20 -w 500`, then `kill -9 <parent PID>` | Whether the client keeps receiving data |
| [5b] | `./server --no-graceful` and `./server` | `./client -n 10 -w 500`, then press `Ctrl+C` in window A | Off: the client disconnects at once; on: the client receives all 10 snapshots, and then the server in A exits |

### 6.3 test_debug: two-level debug

| Check | release (`make DEBUG=0`) | debug (`make DEBUG=1`) |
|---|---|---|
| gcc commands with `-DENABLE_DEBUG` | 0/3 | 3/3 |
| Debug-only format strings in the executables (`strings`) | **0** | 4 |
| Logger calls inside `write_all()` (`objdump -d`) | **0** | 2 (`log_write@plt`) |
| `libutils.so` text section size | 9,015 bytes | 9,423 bytes |
| DEBUG lines when run with `-d 2` | 0, with the warning `-d 2 has no effect` | — |

Lines in the server log when the debug build handles one `SYSINFO`:

| Level | Total lines | INFO/WARN | DEBUG | hex dump |
|---|---|---|---|---|
| `-d 0` | 7 | 7 | 0 | 0 |
| `-d 1` | 12 | 7 | 5 (with `file:line`) | 0 |
| `-d 2` | 22 | 7 | 15 | 10 (the request starts with `53 59 53 49 4e 46 4f`, that is, `SYSINFO`) |

### 6.4 test_capture: tcpdump capture and header analysis

The test runs `tcpdump -i lo -n -w logs/capture.pcap 'tcp port 9090'` on `lo` and opens 4 connections meanwhile, 48 packets in total:

| Connection | Client command | Request | What it shows |
|---|---|---|---|
| 1 | `./client` | `SYSINFO` | Three-way handshake, one request and one reply, FINs from both sides |
| 2 | `./client -n 3 -w 200` | `WATCH 3 200` | One request, 3 replies |
| 3 | `./client -r HELLO` | `HELLO` | `ERROR unknown request` |
| 4 | `./client -n 3 -w 200 --half-close` | `WATCH 3 200` | Half close: the client sends its FIN first and still receives 3 replies |

Afterwards, in order:

1. `tcpdump -r logs/capture.pcap -n -c 10` lists the first connection packet by packet.
2. `tcpdump -r ... -n -x -c 4` shows the first 4 packets in hex.
3. `tcpdump -r ... -n 'tcp[13] & 2 == 2'` keeps only SYN and SYN+ACK: `tcp[13]` is byte 13 of the TCP header (the flags), and `2` is the SYN bit. 2 for each of the 4 connections makes 8 packets, matching the test's own count.
4. The test's built-in pcap reader (a 24-byte file header and a 16-byte record header per packet) parses each packet and breaks the request packet into its header layers, field by field.
5. `tcpdump -r ... -n 'tcp port <client port of connection 4>'` lists the half-closed connection.

```text
  #   time      dir   flags         rel seq  rel ack  len  payload
  1    0.000000 C->S  SYN                 0        0    0
  2    0.000011 S->C  SYN+ACK             0        1    0
  3    0.000019 C->S  ACK                 1        1    0
  4    0.000132 C->S  PSH+ACK             1        1    8  SYSINFO\n
  5    0.000136 S->C  ACK                 1        9    0
  6    0.001243 S->C  PSH+ACK             1        9  109  OK seq=1 pid=12849 host=LAPTOP-0...
  7    0.001267 C->S  ACK                 9      110    0
  8    0.001275 S->C  FIN+ACK           110        9    0
  9    0.001332 C->S  FIN+ACK             9      111    0
  10   0.001347 S->C  ACK               111       10    0

  Ethernet 0x0000:  00 00 00 00 00 00 00 00 00 00 00 00 08 00
  IPv4     0x000e:  45 00 00 3c 07 38 40 00 40 06 35 82 7f 00 00 01
           0x001e:  7f 00 00 01
  TCP      0x0022:  de ca 23 82 71 ab e1 5b 15 e5 eb b5 80 18 00 40
           0x0032:  fe 30 00 00 01 01 08 0a 33 b0 bb 14 30 dd 57 b7
  payload  0x0042:  53 59 53 49 4e 46 4f 0a
```

- **Three-way handshake:** SYN → SYN+ACK → ACK. The SYN carries the options MSS 65495, SACK permitted, timestamp and window scale 10.
- **Data:** The request is 8 bytes (`SYSINFO\n`) and the reply 109 bytes (one `OK ...` line).
- **Close:** The server sends its FIN first (the child closes as soon as it is done), the client answers with its own FIN, and each side ACKs the other's.
- **IPv4 header (20 bytes):** version 4, IHL 5, type of service `0x00`, total length 60, identification, flags `0x2` (DF), fragment offset 0, TTL 64, protocol 6 (TCP), header checksum, and the source and destination addresses.
- **TCP header (32 bytes):** source and destination ports, sequence and ack numbers, data offset 8 (20 bytes plus 12 bytes of options), reserved, flags `0x18` (listed bit by bit as `URG 0 ACK 1 PSH 1 RST 0 SYN 0 FIN 0`), window 64 (times the 2^10 negotiated in the SYN, 65536 bytes), checksum, urgent pointer, and the options (NOP, NOP, timestamps).

**Half close (connection 4):** After sending `WATCH 3 200`, the client calls `shutdown(fd, SHUT_WR)`, which closes only the direction it sends in. The order tcpdump sees:

```text
#4  client -> server  [P.]  length 12   WATCH 3 200\n
#6  client -> server  [F.]              the client's FIN; the server has not replied yet
#7  server -> client  [P.]  length 109  OK seq=1 ...
#9  server -> client  [P.]  length 109  OK seq=2 ... (200 ms later)
#11 server -> client  [P.]  length 109  OK seq=3 ... (another 200 ms later)
#13 server -> client  [F.]              the server closes only after sending everything
```

The client's FIN comes before the server's first reply, and all 3 replies still arrive after it, which shows that the two directions of a TCP connection can be closed separately. The server stops reading once it has read the request line, so the client's early FIN does not affect the replies.

**Opening it in Wireshark:** Open `logs/capture.pcap`, enter the display filter `tcp.port == 9090`, right-click a packet and choose Follow → TCP Stream to see the text of the request and the reply.

**Permissions:** tcpdump needs root. When you run the test from a normal account, it first asks for the password once with `sudo -v`, and after that only tcpdump runs as root. On Windows you can also run it directly as WSL's root (first change to the project folder in the terminal):

```bash
wsl -d Ubuntu -u root -- make test-capture
```

### 6.5 Files in logs/

| File | Contents |
|---|---|
| `test_concurrent.log`, `test_robustness.log`, `test_debug.log`, `test_capture.log` | Full output of the four tests |
| `capture.pcap` | tcpdump capture file; it can be opened in Wireshark |
| `capture_decoded.txt` | Full output of `tcpdump -r capture.pcap -n -x` |

The detailed server and client logs for each scenario (`robust_*`, `concurrent_*`, `debug_*`, `capture_server.log`) are not in the repo; rerunning the tests writes them to `logs/` automatically.

---

## 7. Test Environment

- WSL2 Ubuntu, kernel `6.18.33.2-microsoft-standard-WSL2`
- gcc 15.2.0, GNU Make 4.4.1, tcpdump 4.99.6

## 8. Limitations

- **Linux only:** The code uses `prctl(PR_SET_PDEATHSIG)`, `sysinfo(2)` and `/proc/meminfo`.
- **IPv4 only:** The server listens on `0.0.0.0` and the client connects with `AF_INET`.
- **The client has no timeout:** If the server does not respond, the client waits until the user presses `Ctrl+C`. The server side has the request deadline ([4]) and `TCP_USER_TIMEOUT` ([2b]).
- **[2b] only detects a lost client while sending:** `TCP_USER_TIMEOUT` measures "how long sent data has gone without an ACK". If the client's network vanishes between two `WATCH` sends, the server only finds out about 3 seconds after the next send; the server does not turn on TCP keepalive.
- **A valid request can still take a long time:** The request deadline covers only the request line. `WATCH 100 10000` is a valid request that holds a child process for about 16.5 minutes, and 32 of them sent at once fill every slot. Further protection could limit the number of simultaneous connections from one IP, or the total time of each request.
- **No reply when `fork()` fails:** The parent only logs the error and closes the connection, and the client exits with code 2.
- **Times and ranges cannot be changed with flags:** The 3 seconds, the 30 seconds and the `WATCH` ranges are constants in `server.c` and `utils.h`; changing them means recompiling.
- **No encryption or authentication:** Anyone who can reach the server can read the system information.
- **`make test-capture` needs root.**
