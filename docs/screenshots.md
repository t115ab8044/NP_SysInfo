# Execution Screenshots

[← Back to README](../README.md) · [System Design](system-design.md) · [Key Source Code](key-source.md) · [Auditing Discussion](audit.md)

Apart from the Wireshark and Windows PowerShell windows in sections 13 to 15, every screenshot is a real run on WSL2 Ubuntu. Above each image is the command that produced it; run it in the project folder to reproduce the screen. Screenshots 3, 5 and 14 start a server on port 9090, so make sure no other server is running first.

## 1. Build and the shared library

```bash
make clean && make && ldd ./server ./client | grep -E '^\./|libutils'
```

`libutils.so` is compiled with `-fPIC -shared`, and `ldd` shows that the server and the client both link it dynamically.

![Build and ldd output](screenshots/01_build_so.png)

## 2. 12 concurrent clients (fork-based multi-process)

```bash
make demo
```

12 children are alive at the same time (all with the parent as PPID), the 12 clients are served by 12 different PIDs, the bad request gets `ERROR`, and no zombie processes are left at the end.

![make demo output](screenshots/02_concurrent_demo.png)

## 3. Two-level debug: runtime

```bash
./server -d 2 & sleep 0.3; ./client; sleep 0.2; kill $!
```

`-d 2` shows `file:line` and hex dumps; the request starts with `53 59 53 49 4e 46 4f` (`SYSINFO`).

![Runtime debug level 2](screenshots/03_debug_runtime.png)

## 4. Two-level debug: compile time

```bash
tail -n 24 logs/test_debug.log
```

The release build has 0 debug strings and no logger calls inside `write_all()`; the number of output lines grows with the `-d` level.

![Compile-time debug check](screenshots/04_debug_compile.png)

## 5. With/Without [1]: zombie processes

```bash
./server --no-sigchld 2>/dev/null & sleep 0.3; for i in 1 2 3; do ./client >/dev/null 2>&1; done; sleep 0.3; ps -o pid,ppid,stat,cmd --ppid $!; kill $!
```

```bash
./server 2>/dev/null & sleep 0.3; for i in 1 2 3; do ./client >/dev/null 2>&1; done; sleep 0.3; ps -o pid,ppid,stat,cmd --ppid $!; kill $!
```

Without leaves 3 `<defunct>` children in state `Z`; With leaves no child behind.

![Zombie processes, With/Without](screenshots/05_zombie.png)

## 6. With/Without [2]: the client leaves midway (SIGPIPE)

```bash
sed -n '/^==== \[2\]/,/both: the next client/p' logs/test_robustness.log
```

Without, the child is killed by signal 13; With, the child detects `EPIPE` and exits normally with code 3. In both cases the parent keeps serving.

![SIGPIPE, With/Without](screenshots/06_sigpipe.png)

## 7. With/Without [3]: an oversized request with no newline

```bash
sed -n '/^==== \[3\]/,/with: rejected after/p' logs/test_robustness.log
```

Without, the buffer keeps doubling up to 32 MiB and the child's RSS reaches about 17 MiB; With, the server replies `ERROR request too long` after 127 bytes.

The buffer is allocated at 32 MiB, but only 16 MiB is actually written; Linux gives no physical memory to the untouched part, so the RSS is about 17 MiB. In the With case `ps` lists no child because the child ends within 2 ms of rejecting the request.

![Request length limit, With/Without](screenshots/07_longline.png)

## 8. With/Without [4]: connection limit

```bash
sed -n '/^==== \[4\]/,/with -m 5:/p' logs/test_robustness.log
```

Without, 10 idle clients hold 10 children; With (`-m 5`), there are only 5 children, and the other 5 clients get `ERROR server busy` right away.

![Connection limit, With/Without](screenshots/08_limit.png)

## 9. With/Without [5]: the parent gets `kill -9`

```bash
sed -n '/^==== \[5\]/,/with: the child ends/p' logs/test_robustness.log
```

Without, the child becomes an orphan (its PPID changes) and sends all 10 snapshots; With, the child ends with the parent and the client gets only 3.

![PR_SET_PDEATHSIG, With/Without](screenshots/09_pdeathsig.png)

## 10. Summary of the five With/Without pairs

```bash
sed -n '/With \/ Without summary/,$p' logs/test_robustness.log
```

![Robustness summary](screenshots/10_robust_summary.png)

## 11. tcpdump capture: one connection from start to finish

```bash
tcpdump -r logs/capture.pcap -n -c 10
```

The three-way handshake (`[S]`, `[S.]`, `[.]`), an 8-byte request, a 107-byte reply, and finally one FIN from each side (`[F.]`).

![tcpdump reading the capture file](screenshots/11_tcpdump.png)

## 12. Header analysis of the request packet

```bash
sed -n '/Bytes of the request packet/,/payload (8 bytes)/p' logs/test_capture.log
```

The test reads the pcap file itself, breaks the request packet into Ethernet (14 bytes), IPv4 (20 bytes), TCP (32 bytes) and the payload (`SYSINFO\n`), and explains each field.

![Header fields of the request packet](screenshots/12_header_fields.png)

## 13. Wireshark packet analysis

Open `logs/capture.pcap` in Wireshark, enter the display filter `tcp.port == 9090`, right-click the 4th packet (`[PSH, ACK]`, Len=8) and choose Follow → TCP Stream. Wireshark changes the filter to `tcp.stream eq 0` and keeps only the 10 packets of the first connection, which match the tcpdump output above; in the lower pane, the red text is the `SYSINFO` sent by the client and the blue text is the server's reply `OK seq=1 ...`.

![Wireshark packet analysis](screenshots/13_wireshark.png)

## 14. Real Test: a Windows client connects to the server in WSL2

First build the Windows client in WSL (this needs mingw-w64) and look up the WSL IP address:

```bash
make win-client && hostname -I
```

WSL terminal:

```bash
./server -d 1
```

Windows PowerShell, in the project folder, with the IP address replaced by the one `hostname -I` printed above:

```powershell
.\win_client.exe 172.17.107.87 9090 "WATCH 3 500"
```

The two images show the same connection. The client shows its own address as Windows' `172.17.96.1:57920`, and `accepted 172.17.96.1:57920 -> child pid 3992` in the server log is the same connection, not `127.0.0.1`. The 3 reply lines are about 500 ms apart and all carry `pid=3992`, the same child, which finally exits 0 and is reaped.

![Real Test: the server in WSL2](screenshots/14_real_test_wsl.png)

![Real Test: the client on Windows](screenshots/14_real_test_windows.png)

## 15. Wireshark capture of the Real Test

Open `logs/real_test.pcapng` in Wireshark (34 packets over 3 connections) and enter the display filter `tcp.stream eq 0` to see only the 10 packets of the first connection (`SYSINFO`): the three-way handshake, the 8-byte request, the 108-byte reply and the FIN from each side. The source and destination are `172.17.96.1` and `172.17.107.87`. With the 4th packet (`[PSH, ACK]`, Len=8) selected, the Ethernet II addresses below belong to the Hyper-V virtual network adapter (`00:15:5d:...`), which shows the packets go over the virtual network, not loopback; the hex on the right ends with `53 59 53 49 4e 46 4f 0a` (`SYSINFO\n`).

![Wireshark capture of the Real Test](screenshots/15_real_wireshark.png)
