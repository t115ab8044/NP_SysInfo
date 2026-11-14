# 執行截圖（Execution Screenshots）

[← 回到 README](../README.md) ・ [系統設計說明](system-design.md) ・ [關鍵原始碼說明](key-source.md)

除了第 13 節的 Wireshark 畫面（在 Windows 上開啟抓包檔），其他都是在 WSL2 Ubuntu 上實際執行的畫面。每張圖上方是產生該畫面的指令，在專案資料夾執行即可重現。截圖 3 會在 9090 埠啟動 server，執行前要先確定沒有其他 server 在跑。

## 1. 建置與動態函式庫

```bash
make clean && make && ldd ./server ./client | grep -E '^\./|libutils'
```

`libutils.so` 以 `-fPIC -shared` 編譯，`ldd` 顯示 server 和 client 都動態連結到它。

![建置與 ldd 結果](screenshots/01_build_so.png)

## 2. 12 個 client 併發（fork 多程序）

```bash
make demo
```

12 個 client 各送出不同的請求，從 `WATCH 2 3000` 到 `WATCH 31 100`，每條連線都維持 3 秒。12 個 child 同時存在（PPID 都是 parent），12 個 client 由 12 個不同的 PID 服務，每個都收到自己要求的筆數；錯誤的請求收到 `ERROR`，結束後沒有殭屍程序。

![make demo 結果](screenshots/02_concurrent_demo.png)

## 3. 雙層 Debug：執行期

```bash
./server -d 2 & sleep 0.3; ./client; sleep 0.2; kill $!
```

`-d 2` 會顯示 `file:line` 和 hex dump，請求開頭是 `53 59 53 49 4e 46 4f`（`SYSINFO`）。

![執行期 Debug 等級 2](screenshots/03_debug_runtime.png)

## 4. 雙層 Debug：編譯期

```bash
tail -n 24 logs/test_debug.log
```

release 版的 debug 字串為 0 個，`write_all()` 內也沒有 logger 呼叫；輸出行數隨 `-d` 等級增加。

![編譯期 Debug 驗證結果](screenshots/04_debug_compile.png)

## 5. With/Without [1]：12 個 client 同時斷線

```bash
sed -n '/^==== \[1\]/,/\[1\] with the loop/p' logs/test_robustness.log
```

12 個 `--mode idle` client 連上後一起被 `kill -9`，12 個 child 幾乎同時結束。signal 不會排隊，parent 只收到幾次 SIGCHLD。Without（`--reap-once`，每個 SIGCHLD 只呼叫一次 `waitpid`）只回收 2 個，另外 10 個停在 `Z` 狀態，parent 的計數也停在 `active=10`；With 用迴圈一次收完，12 個全部回收。

![waitpid 迴圈的 With/Without 對比](screenshots/05_reap_once.png)

## 6. With/Without [2]：client 中途離開

### 6.1 [2a] client 正常關閉（SIGPIPE）

```bash
sed -n '/^==== \[2a\]/,/\[2a\] both/p' logs/test_robustness.log
```

Without 時 child 被 signal 13 殺掉，With 時 child 偵測 `EPIPE` 後以 exit 3 正常結束；兩種情況下 parent 都能繼續服務。

![SIGPIPE 的 With/Without 對比](screenshots/06_sigpipe.png)

### 6.2 [2b] client 的網路突然消失（TCP_USER_TIMEOUT）

```bash
sed -n '/^==== \[2b\]/,/\[2b\] with:/p' logs/test_robustness.log
```

client 收到 2 筆後，`tc qdisc add dev lo root netem loss 100%` 丟掉 loopback 上的所有封包，client 也跟著消失。Without 時每次 `write()` 都成功，child 又「送」了 9 秒，最後記錄 `exit 0 (request served)`；With 時斷線 4.0 秒後 `write()` 回傳 `ETIMEDOUT`，child 記錄 `client unreachable` 並停止。測試程式在自己的 network namespace 裡執行 `tc`，不需要 root，也不影響系統的網路。

![TCP_USER_TIMEOUT 的 With/Without 對比](screenshots/06_net_vanish.png)

## 7. With/Without [3]：四種惡意請求

```bash
sed -n '/^==== \[3\]/,/\[3\] with:/p' logs/test_robustness.log
```

同一個 server 依序收到 4 個請求：(a) 16 MiB 不換行、(b) `WATCH 4294967297 100`、(c) `WATCH 100 0`、(d) `WATCH 100000 1000`。Without 時 4 個都被接受：(a) 緩衝區加倍到 32 MiB，child 的 RSS 約 17 MiB；(b) 4294967297 溢位成 1，log 記錄 `-> 1 snapshot(s)`；(c) 100 行在 22 ms 內送完；(d) 這個請求要跑 27.8 小時。With 時 4 個都收到 `ERROR`。

緩衝區配置到 32 MiB，但實際只寫入 16 MiB；沒寫到的部分 Linux 不會配實體記憶體，所以 RSS 約 17 MiB。

![請求驗證的 With/Without 對比](screenshots/07_malicious.png)

## 8. With/Without [4]：Slowloris

```bash
sed -n '/^==== \[4\]/,/\[4\] with:/p' logs/test_robustness.log
```

32 個 `--mode slow` client 每 0.5 秒送 1 byte、永遠不送換行，佔滿 server 預設的 32 個名額。4 秒後再來一個正常的 client：Without 時 32 個 child 還在等，正常的 client 收到 `ERROR server busy`；With 時 32 個在第 3 秒收到 `ERROR request timeout`（3 秒內每個只送出 7 bytes，連 `SYSINFO` 都還沒送完），正常的 client 得到服務。

![請求期限的 With/Without 對比](screenshots/08_slowloris.png)

## 9. With/Without [5]：server 結束

### 9.1 [5a] parent 被 `kill -9`

```bash
sed -n '/^==== \[5a\]/,/\[5a\] with:/p' logs/test_robustness.log
```

Without 時 child 變成孤兒（PPID 改變）並把 10 筆送完；With 時 child 跟著 parent 結束，client 只收到 3 筆。

![PR_SET_PDEATHSIG 的 With/Without 對比](screenshots/09_pdeathsig.png)

### 9.2 [5b] `Ctrl+C`

```bash
sed -n '/^==== \[5b\]/,/\[5b\] with:/p' logs/test_robustness.log
```

測試程式把 server 放在自己的 process group，再用 `kill -INT -<pgid>` 模擬 `Ctrl+C`。Without 時 server 和 child 一起被 signal 2 殺掉，client 只收到 3 筆；With 時新的 client 立刻被拒絕（connection refused），正在服務的 client 收到全部 10 筆，server 等 child 做完後以 exit 0 結束。

![優雅關閉的 With/Without 對比](screenshots/09_ctrl_c.png)

## 10. 七組 With/Without 總表

```bash
sed -n '/With \/ Without summary/,$p' logs/test_robustness.log
```

![穩健性總表](screenshots/10_robust_summary.png)

## 11. tcpdump 抓包：一條連線的完整過程

```bash
tcpdump -r logs/capture.pcap -n -c 10
```

測試程式抓到的第一條連線（`SYSINFO`）：三向交握（`[S]`、`[S.]`、`[.]`）、8 bytes 的請求、109 bytes 的回應，最後雙方各送一次 FIN（`[F.]`）。

![tcpdump 讀回抓包檔](screenshots/11_tcpdump.png)

## 12. 請求封包的標頭分析

```bash
sed -n '/Bytes of the request packet/,/payload (8 bytes)/p' logs/test_capture.log
```

測試程式直接讀 pcap 檔，把請求封包拆成 Ethernet（14 bytes）、IPv4（20 bytes）、TCP（32 bytes）和 payload（`SYSINFO\n`），逐欄說明。IPv4 包含 type of service 和 fragment offset；TCP 包含 data offset、reserved、6 個 flag 位元（`URG 0 ACK 1 PSH 1 RST 0 SYN 0 FIN 0`）、乘上 window scale 後的視窗大小（64 × 2^10 = 65536 bytes）、urgent pointer，以及選項（NOP、NOP、timestamps）。

![請求封包的標頭欄位](screenshots/12_header_fields.png)

## 13. Wireshark 封包分析

用 Wireshark 開啟 `logs/capture.pcap`，display filter 輸入 `tcp.port == 9090`，對第 4 個封包（`[PSH, ACK]`，Len=8）按右鍵選 Follow → TCP Stream。Wireshark 會把 filter 換成 `tcp.stream eq 0`，只留下第一條連線的 10 個封包，和上一張 tcpdump 的結果一致；下方視窗紅字是 client 送的 `SYSINFO`，藍字是 server 回的 `OK seq=1 ...`。

![Wireshark 封包分析](screenshots/13_wireshark.png)

## 14. tcpdump 過濾 SYN 封包

```bash
tcpdump -r logs/capture.pcap -n 'tcp[13] & 2 == 2'
```

`tcp[13]` 是 TCP 標頭第 13 個 byte，也就是 flags；`& 2` 取出 SYN 位元。抓包檔裡有 4 條連線，所以只剩 8 個封包：每條連線的 `[S]` 和 `[S.]` 各一個。SYN 封包帶著 MSS、SACK permitted、timestamp 和 window scale 等選項。

![只留下 SYN 和 SYN+ACK](screenshots/14_syn_filter.png)

## 15. 半關閉（half close）

```bash
sed -n '/Half close (connection 4)/,/replies after/p' logs/test_capture.log
```

第 4 條連線是 `./client -n 3 -w 200 --half-close`。client 送出 `WATCH 3 200`（length 12）之後呼叫 `shutdown(fd, SHUT_WR)`，第 6 個封包就是 client 的 `[F.]`，這時 server 還沒有回覆。之後 server 仍然每隔 200 ms 送出一行（第 7、9、11 個封包，各 109 bytes），送完才送出自己的 `[F.]`。TCP 的兩個方向可以分開關閉：client 不再送資料，但還能繼續接收。

![半關閉的連線](screenshots/15_half_close.png)
