# Auditing Discussion

[← Back to README](../README.md) · [System Design](system-design.md) · [Key Source Code](key-source.md) · [Execution Screenshots](screenshots.md)

## 1. Checks and Results

| Check | Method | Result |
|---|---|---|
| Compiler warnings | The Makefile's `-Wall -Wextra -pedantic`, plus `-Wshadow -Wformat=2 -Wstrict-prototypes -Wnull-dereference -Wcast-qual` | 0 warnings |
| Static analysis | `gcc -fanalyzer` on `server.c`, `client.c` and `utils.c` | 0 warnings |
| Dangerous functions | Search for `strcpy`, `strcat`, `sprintf`, `gets`, `system`, `popen` and `signal()` | 0 uses: strings always use `snprintf`, and signals always use `sigaction` |
| Memory errors | Rebuilt with `-fsanitize=address,undefined` and sent 35 requests: 11 kinds of bad request, `SYSINFO`, `-d 2`, `WATCH`, a disconnect midway, 16 MiB with no newline, idle connections beyond the limit, and 12 concurrent clients | 0 AddressSanitizer, UndefinedBehaviorSanitizer or LeakSanitizer reports |
| fd leaks | Served 50 clients in a row and counted the parent's `/proc/<pid>/fd` before and after | 5 both before and after: 0, 1, 2, the listening socket, and 1 inherited from the terminal |
| Whether the child closes the listening socket | Listed the child's fds during a `WATCH` | The child has only the socket of its own connection, with an inode different from the listening socket |
| Zombies and orphans | [1] and [5] of `make test-robust` | 0 zombies; the child ends with the parent when the parent gets `kill -9` |
| Whether the release build has debug code | `make test-debug` | 0 debug-only strings, and 0 logger calls inside `write_all()` |
| Input boundaries | 15 kinds of bad request: an empty line, an unknown command, missing arguments, extra arguments, negative numbers, out-of-range values, numbers larger than `int` | Before the fix 2 kinds were accepted; after it all 15 get `ERROR` (see item 1 in Section 2) |

## 2. Problems Found and Fixed

| # | Problem | How it was found | Fix |
|---|---|---|---|
| 1 | `WATCH 4294967297 100` was accepted as `WATCH 1 100`, and `WATCH 2 4294967496` was treated as a 200 ms interval. `sscanf`'s `%d` reading a number outside the `int` range is undefined behavior; glibc actually truncates it, and the truncated value happened to fall in the valid range | Testing the input boundaries with huge numbers during the audit | Changed to `%3d` and `%5d`, which read at most 3 and 5 digits and cannot overflow; any extra digits are read by `%1s` and the request is rejected ([server.c:266](../server.c#L266)) |
| 2 | The client had a `-t` timeout (`SO_RCVTIMEO`), but the docs said "no timeout", so the code and the docs disagreed | Checking the code against the midterm requirements and the docs, item by item | Removed the client's timeout and `set_socket_timeout()` from the `.so`; the test programs now end the `idle` and `longline` clients themselves |
| 3 | When the client disconnected before sending a complete request, the log said `closed the connection without a request`, but the exit code was 0 (which means success) | Reading the server log of `make test-robust` | Changed it to `left before sending a complete request` with exit 3 (the client has left) |
| 4 | `make test-capture` captured only 11 of the 68 packets | The test failed when it compared the packet count | libpcap hands packets to tcpdump in batches, so stopping too early loses some; it now waits 2 more seconds before stopping |
| 5 | When run with `make -s test`, the two `test_debug` checks "`make DEBUG=0/1` builds" failed | Test results | The inner make inherited the outer make's `MAKEFLAGS`; `MAKEFLAGS` and `MFLAGS` are now cleared before the inner make runs |

Measured results before and after fix 1:

| Request | Before the fix | After the fix |
|---|---|---|
| `WATCH 4294967297 100` | `OK seq=1 ...` | `ERROR bad arguments` |
| `WATCH 2 4294967496` | `OK seq=1 ...` | `ERROR bad arguments` |
| `WATCH 99999999999 100` | `ERROR bad arguments` | `ERROR bad arguments` |
| `WATCH 1000 100`, `WATCH 1 100000` | `ERROR bad arguments` | `ERROR bad arguments` |
| `WATCH 100 10000` (the limits) | `OK seq=1 ...` | `OK seq=1 ...` |

## 3. Known Risks

| Risk | Impact | Why it is not fixed, and how to improve it |
|---|---|---|
| Idle connections take all the slots | 32 clients that connect and send nothing are enough to make everyone else get `ERROR server busy` | The real fix is a timeout, which this project does not have. For now mechanism [4] limits the impact to at most 32 children, so the host as a whole is not dragged down |
| Plaintext, no authentication | Anyone who can reach port 9090 can read the hostname, load and memory | This project has no encryption or login. In real use, a firewall can open the port only to the management network |
| Reap records may be delayed | If SIGCHLD arrives just before `accept()` starts, the `reaped child` log line is not printed until the next connection comes in | Only the log timing is affected. The reap records are always processed before the connection limit is checked, so the `active` count is correct |
| The reap record ring buffer has only 256 slots | If more than 256 children are reaped between two processing passes, the PID records of the older ones are overwritten | With the default of at most 32 children this cannot happen; it is possible only with `--no-limit`, and it affects only the log content, not the `active` count |
| Linux and IPv4 only | Uses `prctl`, `sysinfo(2)` and `/proc/meminfo` | Cross-platform support is not needed within the scope of the midterm |

## 4. How to Reproduce

Run these in the project folder.

**Extra compiler warnings:**

```bash
gcc -std=gnu11 -O2 -DENABLE_DEBUG -Wall -Wextra -pedantic -Wshadow -Wformat=2 -Wstrict-prototypes -Wnull-dereference -Wcast-qual -fsyntax-only server.c client.c utils.c
```

**Static analysis:**

```bash
for f in server.c client.c utils.c; do gcc -std=gnu11 -O1 -DENABLE_DEBUG -fanalyzer -c "$f" -o /dev/null; done
```

**Sanitizer build:** Build it in a separate folder, so the executables in the project are not affected.

```bash
mkdir -p ~/np_san && cp *.c *.h ~/np_san && cd ~/np_san
gcc -fsanitize=address,undefined -g -O1 -DENABLE_DEBUG -fPIC -shared utils.c -o libutils.so
gcc -fsanitize=address,undefined -g -O1 -DENABLE_DEBUG server.c -L. -lutils -Wl,-rpath,'$ORIGIN' -o server
gcc -fsanitize=address,undefined -g -O1 -DENABLE_DEBUG client.c -L. -lutils -Wl,-rpath,'$ORIGIN' -o client
```

Then run `./server` and `./client` as usual; if there is a memory error, the terminal prints `AddressSanitizer` or `runtime error`.

**Input boundaries:**

```bash
./server 2>/dev/null & sleep 0.3; for r in "WATCH 4294967297 100" "WATCH 2 4294967496" "WATCH 1000 100" "WATCH 100 10000"; do ./client -r "$r" 2>/dev/null | head -1; done; kill $!
```

**fd count:**

```bash
./server 2>/dev/null & sleep 0.3; ls /proc/$!/fd | wc -l; for i in $(seq 50); do ./client >/dev/null 2>&1; done; ls /proc/$!/fd | wc -l; kill $!
```
