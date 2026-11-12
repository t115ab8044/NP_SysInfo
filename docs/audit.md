# 稽核討論（Auditing Discussion）

[← 回到 README](../README.md) ・ [系統設計說明](system-design.md) ・ [關鍵原始碼說明](key-source.md) ・ [執行截圖](screenshots.md)

## 1. 檢查項目與結果

| 檢查 | 方法 | 結果 |
|---|---|---|
| 編譯警告 | Makefile 的 `-Wall -Wextra -pedantic`，另外加上 `-Wshadow -Wformat=2 -Wstrict-prototypes -Wnull-dereference -Wcast-qual` | 0 個警告 |
| 靜態分析 | `gcc -fanalyzer` 分析 `server.c`、`client.c`、`utils.c` | 0 個警告 |
| 危險函式 | 搜尋 `strcpy`、`strcat`、`sprintf`、`gets`、`system`、`popen`、`signal()` | 0 處：字串都用 `snprintf`，signal 都用 `sigaction` |
| 記憶體錯誤 | 用 `-fsanitize=address,undefined` 重新編譯，送出 35 個請求：11 種錯誤請求、`SYSINFO`、`-d 2`、`WATCH`、中途斷線、16 MiB 不換行、超過上限的閒置連線、12 個併發 client | 0 個 AddressSanitizer、UndefinedBehaviorSanitizer、LeakSanitizer 報告 |
| fd 洩漏 | 連續服務 50 個 client，前後各數一次 parent 的 `/proc/<pid>/fd` | 前後都是 5 個：0、1、2、listening socket，以及從終端機繼承的 1 個 |
| child 有沒有關掉 listening socket | `WATCH` 進行中列出 child 的 fd | child 只有自己那條連線的 socket，inode 和 listening socket 不同 |
| 殭屍與孤兒 | `make test-robust` 的 [1]、[5] | 0 個殭屍；parent 被 `kill -9` 時 child 跟著結束 |
| release 版有沒有 debug 程式碼 | `make test-debug` | debug 專用字串 0 個，`write_all()` 裡 0 個 logger 呼叫 |
| 輸入邊界 | 15 種錯誤請求：空行、未知指令、缺參數、多餘參數、負數、超出範圍、超過 `int` 的數字 | 修正前有 2 種被接受，修正後 15 種全部回 `ERROR`（見第 2 節第 1 項） |

## 2. 發現並修正的問題

| # | 問題 | 怎麼發現 | 修正 |
|---|---|---|---|
| 1 | `WATCH 4294967297 100` 被當成 `WATCH 1 100` 接受，`WATCH 2 4294967496` 被當成間隔 200 ms。`sscanf` 的 `%d` 讀到超過 `int` 範圍的數字是未定義行為，glibc 實際上會截斷，截斷後的值剛好落在合法範圍內 | 稽核時用超大數字測輸入邊界 | 改成 `%3d`、`%5d`，數字最多讀 3 位和 5 位，不可能溢位；多出來的位數會被 `%1s` 讀到，請求被拒絕（[server.c:266](../server.c#L266)） |
| 2 | client 有 `-t` 逾時（`SO_RCVTIMEO`），但文件寫「不做逾時」，而逾時屬於期末的可靠度項目 | 拿期中、期末規定逐項對照 | 拿掉 client 的逾時和 `.so` 裡的 `set_socket_timeout()`；測試用的 `idle`、`longline` client 改由測試程式結束 |
| 3 | client 沒送完請求就斷線時，log 寫 `closed the connection without a request`，exit code 卻是 0（代表成功） | 讀 `make test-robust` 的 server log | 改成 `left before sending a complete request`，exit 3（client 已離開） |
| 4 | `make test-capture` 只抓到 68 個封包中的 11 個 | 測試程式比對封包數時失敗 | libpcap 是一批一批把封包交給 tcpdump，太早停止就會漏掉；停止前多等 2 秒 |
| 5 | 用 `make -s test` 執行時，`test_debug` 的「`make DEBUG=0/1` 能建置」兩項失敗 | 測試結果 | 內層 make 繼承了外層的 `MAKEFLAGS`；執行內層 make 之前先清掉 `MAKEFLAGS`、`MFLAGS` |

第 1 項修正前後的實測：

| 請求 | 修正前 | 修正後 |
|---|---|---|
| `WATCH 4294967297 100` | `OK seq=1 ...` | `ERROR bad arguments` |
| `WATCH 2 4294967496` | `OK seq=1 ...` | `ERROR bad arguments` |
| `WATCH 99999999999 100` | `ERROR bad arguments` | `ERROR bad arguments` |
| `WATCH 1000 100`、`WATCH 1 100000` | `ERROR bad arguments` | `ERROR bad arguments` |
| `WATCH 100 10000`（上限） | `OK seq=1 ...` | `OK seq=1 ...` |

## 3. 已知風險

| 風險 | 影響 | 為什麼沒修、怎麼改善 |
|---|---|---|
| 閒置連線佔滿名額 | 32 個連上後不送資料的 client，就能讓其他人都收到 `ERROR server busy` | 根本解法是逾時，屬於期末的可靠度項目。期中用機制 [4] 把影響限制在最多 32 個 child，不會拖垮整台主機 |
| 明文、沒有身分驗證 | 連得到 port 9090 的人都能讀到 hostname、負載和記憶體 | 加密與登入屬於期末的安全性項目。實際使用時，可以用防火牆只開放給管理用的網段 |
| 回收紀錄可能延後 | SIGCHLD 剛好在 `accept()` 開始之前到達時，`reaped child` 那行 log 要等到下一條連線進來才印出 | 只影響 log 的時間。每次檢查連線上限前都會先處理回收紀錄，所以 `active` 的數字是對的 |
| 回收紀錄的 ring buffer 只有 256 格 | 兩次處理之間回收超過 256 個 child 時，較早的 PID 紀錄會被覆蓋 | 預設最多 32 個 child，不會發生；只有 `--no-limit` 時才有可能，而且只影響 log 內容，`active` 的計數不受影響 |
| 只支援 Linux 與 IPv4 | 用到 `prctl`、`sysinfo(2)` 和 `/proc/meminfo` | 期中範圍內不需要跨平台 |

## 4. 重現方式

在專案資料夾執行。

**額外的編譯警告：**

```bash
gcc -std=gnu11 -O2 -DENABLE_DEBUG -Wall -Wextra -pedantic -Wshadow -Wformat=2 -Wstrict-prototypes -Wnull-dereference -Wcast-qual -fsyntax-only server.c client.c utils.c
```

**靜態分析：**

```bash
for f in server.c client.c utils.c; do gcc -std=gnu11 -O1 -DENABLE_DEBUG -fanalyzer -c "$f" -o /dev/null; done
```

**Sanitizer 版本：** 編譯到另一個資料夾，不影響專案裡的執行檔。

```bash
mkdir -p ~/np_san && cp *.c *.h ~/np_san && cd ~/np_san
gcc -fsanitize=address,undefined -g -O1 -DENABLE_DEBUG -fPIC -shared utils.c -o libutils.so
gcc -fsanitize=address,undefined -g -O1 -DENABLE_DEBUG server.c -L. -lutils -Wl,-rpath,'$ORIGIN' -o server
gcc -fsanitize=address,undefined -g -O1 -DENABLE_DEBUG client.c -L. -lutils -Wl,-rpath,'$ORIGIN' -o client
```

之後照平常的方式執行 `./server` 和 `./client`，有記憶體錯誤時，終端機會印出 `AddressSanitizer` 或 `runtime error`。

**輸入邊界：**

```bash
./server 2>/dev/null & sleep 0.3; for r in "WATCH 4294967297 100" "WATCH 2 4294967496" "WATCH 1000 100" "WATCH 100 10000"; do ./client -r "$r" 2>/dev/null | head -1; done; kill $!
```

**fd 數量：**

```bash
./server 2>/dev/null & sleep 0.3; ls /proc/$!/fd | wc -l; for i in $(seq 50); do ./client >/dev/null 2>&1; done; ls /proc/$!/fd | wc -l; kill $!
```
