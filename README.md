# NP SysInfo：Fork 多進程系統資訊伺服器

C 語言網路與系統程式設計期中專案。

**學號：** 115ab8044 ・ **姓名：** 顏祺宸

Client 連到 server，用一行文字請求查詢主機的即時系統資訊：hostname、uptime、CPU load average、可用記憶體與進程數。可以查一次，也可以要求 server 每隔一段時間連續回報。Server 為每個連線 `fork()` 一個子進程處理，雙方共用的工具函式封裝在動態函式庫 `libutils.so`。

**應用情境：** 管理多台 Linux 主機時，常需要快速知道每台主機的負載和記憶體狀況。主機上跑 `server`，其他人不用登入主機，就能用 `client` 查詢一次，或連續觀察幾秒鐘的變化。

## 繳交內容導覽

| 項目 | 位置 |
|---|---|
| System design explanation（系統設計說明） | [docs/system-design.md](docs/system-design.md) |
| Description of key source code（關鍵原始碼說明） | [docs/key-source.md](docs/key-source.md) |
| Execution screenshots（執行截圖） | [docs/screenshots.md](docs/screenshots.md) |
| Complete source code（完整原始碼） | 本 repo，檔案清單見[第 1 節](#1-專案架構) |
| Real Test（真實測試） | [第 7 節](#7-real-testwindows--wsl2)：Windows client 連到 WSL2 上的 server |
| Auditing discussion（稽核討論） | [docs/audit.md](docs/audit.md) |

## 需求對照

| 期中規定 | 本專案的做法 |
|---|---|
| Server 用 `fork()` 處理連線，不可用 thread | parent 只負責 `accept()`，每個連線 fork 一個 child；整個專案沒有使用 thread |
| 至少同時處理 10 個 client，並提供執行期證明 | `tests/test_concurrent.c` 讓 12 個 client 同時連線、各送出不同的 `WATCH` 請求，用 `ps aux` 和 `ps --forest` 證明 12 個子進程同時存在 |
| 封包擷取與分析 | `tests/test_capture.c` 用 tcpdump 抓 loopback，讀回 pcap 檔，逐包列出三向交握、請求、回應、FIN，用 `tcp[13] & 2 == 2` 只留下 SYN 封包，把請求封包拆成 Ethernet、IPv4、TCP 標頭逐欄說明，並抓下 client 用 `--half-close` 半關閉的連線 |
| 雙層 Debug | 編譯期 `-DENABLE_DEBUG`（關閉時巨集展開成 `((void)0)`），加上執行期 `-d 0/1/2` |
| 共用函式封裝成 `.so`，兩邊都連結 | logger、`write_all`／`read_line`、系統資訊擷取都在 `libutils.so`，server 和 client 都以 `-lutils` 動態連結 |
| Client 向 server 請求系統資訊 | `SYSINFO` 查一次，`WATCH <次數> <毫秒>` 連續回報 |
| 穩健機制，至少 3 個 With/Without 執行範例 | 5 個機制、5 組 With/Without 對照（[第 5 節](#5-五個穩健機制)），每個機制都能用參數關閉，由 `tests/test_robustness.c` 自動跑出對照 |
| 使用 Makefile 建置 | `Makefile` 提供建置、展示與測試的 target |

---

## 1. 專案架構

### 檔案結構

```text
.
├── Makefile             建置 libutils.so、server、client，以及測試 target
├── utils.h / utils.c    libutils.so：logger、socket I/O、系統資訊、雜項工具
├── server.c             fork 多進程 server，5 個穩健機制可個別關閉
├── client.c             client：查詢、連續回報、3 種故障模擬
├── tests/               C 測試程式
│   ├── testlib.h / testlib.c   共用函式：fork + exec、regex 過濾、讀取 /proc
│   ├── demo.c                  一個指令的現場展示（約 3 秒）
│   ├── test_concurrent.c       12 個 client 併發證明
│   ├── test_robustness.c       5 組穩健機制的 With/Without 對照
│   ├── test_debug.c            雙層 Debug 證明（編譯期與執行期）
│   ├── test_capture.c          tcpdump 抓包與標頭分析
│   └── win_client.c            Real Test 用的 Windows client（Winsock）
├── docs/                系統設計、關鍵原始碼說明、執行截圖與稽核討論
└── logs/                測試結果與抓包檔（重跑測試會產生細項 log）
```

### 進程模型

```text
 client 1..N ── connect() ──────► server parent   accept → 檢查上限 → fork()
                                    │      ▲
                             fork() │      │ SIGCHLD：child 結束時 kernel 通知，
                                    ▼      │ parent 用 waitpid 迴圈回收
 client 1 ◄═══ request/reply ═══► child (pid A)   服務 client 1，完成後 _exit
 client 2 ◄═════════════════════► child (pid B)   服務 client 2，完成後 _exit
   ...                            ...
 client N ◄═════════════════════► child (pid N)
```

- **parent：** 只負責 `accept()`、`fork()` 和回收子進程。唯一會寫入 client 的情況，是連線數達上限時直接回 `ERROR server busy`（機制 [4]）。
- **child：** 讀一行請求、回覆、`_exit()`。連線建立後，請求和回覆直接在 client 和 child 之間傳送，不經過 parent。exit code 代表結束原因，parent 回收時寫進 log。

### libutils.so 模組

| 模組 | 函式 | 說明 |
|---|---|---|
| Logger | `log_init`、`log_write`、`log_hexdump`、`LOG_INFO/WARN/ERROR`、`LOG_DEBUG`、`LOG_HEXDUMP`、`g_runtime_debug_level` | 每行格式為 `HH:MM:SS.mmm [tag:pid] LEVEL msg`，整行只用一次 `write()` 輸出，多個進程同時寫也不會交錯 |
| Socket I/O | `write_all`、`read_line` | `write_all` 補完 short write、遇到 `EINTR` 重試；`read_line` 一次讀 1 byte，最多讀 127 bytes（機制 [3]） |
| 系統資訊 | `sysinfo_collect`、`sysinfo_format` | 資料來源是 `gethostname`、`sysinfo(2)`、`getloadavg`、`/proc/meminfo`，格式化成一行 `OK ...` |
| 其他 | `parse_int_range`、`sleep_ms`、`set_signal` | 命令列整數解析、被 signal 中斷後會繼續睡的 sleep、用 `sigaction` 設定 signal |

`libutils.so` 共匯出 11 個符號，可用 `make inspect` 查看。

---

## 2. 建置

**需求：** Linux（本專案在 WSL2 Ubuntu 上測試）、gcc、GNU make。測試程式另外會用到 `ps`、`strings`、`objdump`、`size`、`tcpdump`。

```bash
make
```

`make` 預設編出 debug 版（帶 `-DENABLE_DEBUG`），實際的編譯指令如下。debug 和 release 兩種模式都是 **0 warning**（`-Wall -Wextra -pedantic`）。

```text
gcc -Wall -Wextra -pedantic -std=gnu11 -O2 -g -DENABLE_DEBUG -fPIC -shared -o libutils.so utils.c
gcc -Wall -Wextra -pedantic -std=gnu11 -O2 -g -DENABLE_DEBUG -o server server.c -L. -Wl,-rpath,'$ORIGIN' -lutils
gcc -Wall -Wextra -pedantic -std=gnu11 -O2 -g -DENABLE_DEBUG -o client client.c -L. -Wl,-rpath,'$ORIGIN' -lutils
```

| Make target | 作用 |
|---|---|
| `make`、`make all` | debug 版（`DEBUG=1`，預設） |
| `make release` | release 版（`DEBUG=0`，`LOG_DEBUG` 完全編譯掉） |
| `make demo` | 一個指令的現場展示（約 3 秒）：`.so` 連結、查詢、12 個 client 各送出不同的請求（12 個併發子進程）、拒絕錯誤請求、沒有殭屍進程 |
| `make test` | 依序執行 `test_concurrent`、`test_robustness`、`test_debug` |
| `make test-concurrent`、`make test-robust`、`make test-debug` | 個別執行一支測試 |
| `make test-capture` | tcpdump 抓包測試，需要 root 或 sudo |
| `make inspect` | 列出 `libutils.so` 匯出的符號，以及 `ldd` 連結結果 |
| `make capture` | 用 `sudo tcpdump -i lo -l -vn -e -x 'tcp port 9090'` 即時顯示 port 9090 的流量：`-l` 逐行輸出、`-v` 顯示 IP 標頭的 tos／ttl／id／checksum、`-n` 不反查名稱、`-e` 顯示 MAC 和 EtherType、`-x` 顯示 hex |
| `make win-client` | 用 mingw-w64 編出 Real Test 用的 `win_client.exe`（[第 7 節](#7-real-testwindows--wsl2)） |
| `make clean` | 刪除 `.so`、執行檔與測試程式 |

- **切換 DEBUG 會自動全部重編。** Makefile 把 CFLAGS 記錄在 `.build_flags`，`DEBUG=1` 和 `DEBUG=0` 互換時，`.so` 和兩個執行檔會一起重編。
- **函式庫跟著執行檔走。** `-Wl,-rpath,'$ORIGIN'` 讓執行檔載入同一個資料夾裡的 `libutils.so`，從任何目錄啟動都可以。

---

## 3. 使用方式

### 3.1 請求與回應

Client 送出一行文字，server 回覆一行或多行文字，每行以 `\n` 結尾，一條連線只處理一個請求。

| Client 送出 | Server 回覆 |
|---|---|
| `SYSINFO` | 一行 `OK seq=1 ...`，然後關閉連線 |
| `WATCH <count> <ms>` | 每隔 `<ms>` 毫秒一行，共 `<count>` 行（count 1–100，ms 100–10000） |
| 其他內容、參數超出範圍、超過 127 bytes | 一行 `ERROR <原因>`，然後關閉連線 |

```text
OK seq=1 pid=556 host=LAPTOP-0F0NVT4H up=0d 00:00:30 load=0.11/0.03/0.01 mem_avail=7.09/7.61 GiB procs=231
```

`pid=` 是負責這條連線的 server 子進程 PID，所以 client 的輸出本身就能證明每條連線由不同的進程處理。

### 3.2 Server

```bash
./server                    # 監聽 0.0.0.0:9090，5 個機制全開
./server -p 9000 -d 1       # 改用 port 9000，開啟流程 debug
./server --no-sigchld       # 關閉機制 [1]，示範殭屍進程
```

| 參數 | 說明 |
|---|---|
| `-p, --port PORT` | 監聽的 port（預設 9090） |
| `-d, --debug LEVEL` | 執行期 debug 等級：0 一般訊息，1 加上流程，2 再加上 hex dump |
| `-m, --max-clients N` | 機制 [4]：同時服務的 client 上限（預設 32） |
| `--no-sigchld`、`--no-sigpipe`、`--no-validate`、`--no-limit`、`--no-pdeathsig` | 分別關閉機制 [1]～[5]，用於 With/Without 對照 |

子進程的 exit code 由 parent 回收時記錄，例如 `reaped child 795: exit 3 (client left)`：

| exit code | 意義 |
|---|---|
| 0 | 請求處理完成 |
| 1 | I/O 錯誤 |
| 2 | 請求被拒絕（太長、未知的指令、參數錯誤） |
| 3 | client 已經離開（寫入時收到 `EPIPE`，或還沒送完請求就斷線） |

子進程如果是被 signal 殺掉的，log 會顯示 `killed by signal 13 (Broken pipe)`。

### 3.3 Client

```bash
./client                      # SYSINFO：查詢一次
./client -n 5 -w 1000         # WATCH 5 1000：每秒一行，共 5 行
./client -r HELLO             # 送出任意一行，觀察 ERROR 回覆
./client -n 3 -w 200 --half-close   # 送完請求就半關閉，仍然收到 3 行
./client --mode abort         # 故障模擬，見下表
```

| 參數 | 說明 |
|---|---|
| `-H, --host HOST` | server 位址（預設 127.0.0.1） |
| `-p, --port PORT` | server port（預設 9090） |
| `-d, --debug LEVEL` | 執行期 debug 等級 0/1/2 |
| `-n, --count N`、`-w, --interval MS` | 改送 `WATCH N MS`（預設 5 次、1000 ms） |
| `-r, --request LINE` | 把 LINE 當成請求送出 |
| `--half-close` | 送完請求後呼叫 `shutdown(fd, SHUT_WR)`：只關閉送出的方向（送出 FIN），仍然繼續讀取回覆 |
| `--mode MODE` | 故障模擬，見下表 |
| `--mib N` | `longline` 模式送出的資料量（預設 16 MiB） |

| 故障模式 | 行為 | 用來測試 |
|---|---|---|
| `abort` | 送出 `WATCH 10 200`，收到第 1 行就斷線 | 機制 [2]：SIGPIPE |
| `longline` | 送出 16 MiB 都不換行的請求，然後等 server 回應 | 機制 [3]：請求長度上限 |
| `idle` | 連上後什麼都不送，一直佔住連線 | 機制 [4]：連線上限 |

Client 沒有逾時，會一直等到 server 回覆或關閉連線；要提早結束就按 `Ctrl+C`。測試程式會自己結束 `longline` 和 `idle` 的 client。

Client 的 exit code：0 成功，1 失敗或收到 `ERROR`，2 連線在收到任何回覆前就結束。

---

## 4. 雙層 Debug 控制

| 層級 | 控制方式 | 效果 |
|---|---|---|
| 編譯期 | `make`（帶 `-DENABLE_DEBUG`）或 `make release` | 關閉時 `LOG_DEBUG`、`LOG_HEXDUMP` 展開成 `((void)0)`：沒有程式碼、沒有格式字串，也沒有等級判斷 |
| 執行期 | `-d 0/1/2`，設定 `g_runtime_debug_level` | 0：只有 INFO、WARN、ERROR<br>1：加上 DEBUG 流程訊息（附 `file:line`）<br>2：再加上每個請求與回覆的 hex dump |

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

release 版仍可接受 `-d`，但會警告 `built without -DENABLE_DEBUG: -d 2 has no effect`。`-d 2` 時 client 端看到的請求：

```text
DEBUG request: 8 byte(s)
DEBUG     0000  53 59 53 49 4e 46 4f 0a                           |SYSINFO.|
```

---

## 5. 五個穩健機制

每個機制都可以用參數單獨關閉，由 `tests/test_robustness.c` 自動跑 Without 和 With 兩種情況並比較。

| # | 異常情況 | 機制 | 關閉參數 |
|---|---|---|---|
| [1] | 子進程結束後沒人回收，變成殭屍 | SIGCHLD handler 內以迴圈呼叫 `waitpid(WNOHANG)` | `--no-sigchld` |
| [2] | 寫入已經離開的 client，子進程被 SIGPIPE 殺掉 | 忽略 SIGPIPE，處理 `write()` 回傳的 `EPIPE` | `--no-sigpipe` |
| [3] | 不換行的超長請求讓子進程記憶體一直長大 | 請求最多 127 bytes，超過就回 `ERROR request too long` | `--no-validate` |
| [4] | 大量連線讓 server 產生無限多個子進程 | 同時服務的 client 上限，超過時 parent 直接回 `ERROR server busy`、不 fork | `--no-limit` |
| [5] | parent 被 `kill -9` 後，子進程變成孤兒繼續執行 | `prctl(PR_SET_PDEATHSIG, SIGTERM)` | `--no-pdeathsig` |

[1] 和 [5] 剛好是一組：殭屍是「child 死了、parent 沒收」，孤兒是「parent 死了、child 還在」。

### [1] SIGCHLD + `waitpid` 迴圈

- **問題：** 子進程結束後，如果 parent 不呼叫 `wait`，它會一直以 `<defunct>` 狀態佔用 process table。
- **作法：** 用 `sigaction` 註冊 SIGCHLD handler，在 handler 裡以 `while (waitpid(-1, &status, WNOHANG) > 0)` 回收。signal 不會排隊，多個子進程同時結束可能只收到一次 SIGCHLD，所以一定要用迴圈。handler 只把 PID 和 status 存進 ring buffer，log 由 main loop 輸出，因為 `printf()` 不能在 signal handler 裡使用。
- **關閉後：** SIGCHLD 維持預設動作，結束的子進程全部變成殭屍。

### [2] 忽略 SIGPIPE

- **問題：** 對已經關閉的連線呼叫 `write()` 會觸發 SIGPIPE，預設動作是直接終止進程，沒有任何錯誤處理。
- **作法：** parent 啟動時設定 `SIGPIPE` 為 `SIG_IGN`，子進程透過 `fork()` 繼承。之後 `write()` 改成回傳 -1、errno 為 `EPIPE`，子進程記錄 `client left` 後以 exit 3 結束。
- **關閉後：** 子進程被 signal 13 殺掉。不論開或關，parent 都不受影響，能繼續服務新的 client。

### [3] 請求長度上限

- **問題：** 如果 server 一直讀到 `\n` 為止，client 只要不送換行，就能讓子進程的緩衝區無限長大。
- **作法：** `read_line()` 一次讀 1 byte，讀滿 127 bytes 還沒看到 `\n` 就停止，回覆 `ERROR request too long` 並結束。
- **關閉後：** 改用不限長度的讀法，緩衝區每次加倍。16 MiB 的請求讓緩衝區長到 32 MiB，子進程的 RSS 約 17 MiB。

### [4] 連線上限

- **問題：** 每個連線都 fork，代表只要大量連線，就能讓 server 產生無限多個進程。連上後不送資料的 client 也會一直佔著子進程。
- **作法：** parent 記錄還沒回收的子進程數量。達到上限（`-m`，預設 32）時，parent 用 `send(..., MSG_DONTWAIT | MSG_NOSIGNAL)` 直接回 `ERROR server busy` 後關閉，不 fork。這兩個 flag 讓 parent 不會因為 client 而卡住或被 SIGPIPE 殺掉。
- **關閉後：** 10 個閒置的 client 佔住 10 個子進程。

### [5] 子進程跟著 parent 結束

- **問題：** `SIGKILL` 無法攔截，parent 被 `kill -9` 時來不及通知子進程。子進程會被過繼給其他進程（WSL2 上是該 session 的 `/init`，不一定是 PID 1），繼續執行，沒有人管理。
- **作法：** 子進程在 fork 後呼叫 `prctl(PR_SET_PDEATHSIG, SIGTERM)`，parent 一死，kernel 就送 `SIGTERM` 給它。設定後再檢查一次 `getppid()`，處理 parent 在 fork 和 `prctl` 之間就死掉的情況。
- **關閉後：** 子進程的 PPID 從 parent 變成別的進程（log 中從 1903 變成 1632），繼續把 10 筆資料送完。

---

## 6. 測試程式

測試程式都是 C 寫的，透過 `fork` + `exec` 執行，不經過 shell。它們會自行啟動 server，每項檢查印出 `[PASS]` 或 `[FAIL]`，全部通過時以 exit code 0 結束，完整輸出存在 `logs/`。可用環境變數 `PORT` 改 port。

| 測試程式 | 證明內容 | 執行方式 | 結果 |
|---|---|---|---|
| `tests/test_concurrent.c` | 12 個 client 同時連線、各送出不同的請求，每條連線由不同的子進程處理 | `make test-concurrent` | 9/9 PASS |
| `tests/test_robustness.c` | 5 個穩健機制的 With/Without 對照 | `make test-robust` | 11/11 PASS |
| `tests/test_debug.c` | 編譯期與執行期的雙層 Debug | `make test-debug` | 15/15 PASS |
| `tests/test_capture.c` | tcpdump 抓包、三向交握、SYN 過濾、標頭欄位、半關閉 | `make test-capture`（需要 root 或 sudo） | 12/12 PASS |

### 6.1 test_concurrent：12 個 client 併發

12 個 client 同時連線，每個送出不同的請求，從 `WATCH 2 3000` 到 `WATCH 31 100`，筆數和間隔都不一樣。每個請求的「(筆數 − 1) × 間隔」都是 3000 ms，所以每條連線都維持 3 秒。測試程式在這段時間內拍下 `ps aux` 和進程樹：

```text
    PID    PPID STAT     ELAPSED CMD
  12061   12041 S+         00:01 ./server -p 9090
  12074   12061 S+         00:01  \_ ./server -p 9090
  12075   12061 S+         00:01  \_ ./server -p 9090
    ...                              (共 12 個 child，PPID 都是 12061)
server children alive           : 12
established TCP conns on :9090  : 12
```

client 結束後，列出每個 client 的請求、收到幾筆、由哪個 child 回應：

```text
[client 01] WATCH 2 3000  ->  2 snapshot(s), served by pid=12077
[client 02] WATCH 3 1500  ->  3 snapshot(s), served by pid=12080
[client 03] WATCH 4 1000  ->  4 snapshot(s), served by pid=12081
[client 04] WATCH 5 750   ->  5 snapshot(s), served by pid=12082
[client 05] WATCH 6 600   ->  6 snapshot(s), served by pid=12078
[client 06] WATCH 7 500   ->  7 snapshot(s), served by pid=12079
[client 07] WATCH 9 375   ->  9 snapshot(s), served by pid=12076
[client 08] WATCH 11 300  -> 11 snapshot(s), served by pid=12074
[client 09] WATCH 13 250  -> 13 snapshot(s), served by pid=12075
[client 10] WATCH 16 200  -> 16 snapshot(s), served by pid=12083
[client 11] WATCH 21 150  -> 21 snapshot(s), served by pid=12084
[client 12] WATCH 31 100  -> 31 snapshot(s), served by pid=12085
```

- **ps 證明：** 共 13 個 `./server` 進程（1 個 parent、12 個 child），12 條 ESTABLISHED 連線，server log 出現 `active=12`。
- **12 種請求同時處理：** 每個 client 收到的筆數都和自己的請求相符（2～31 筆，共 128 筆）；server log 記錄每個 child 收到的請求。
- **PID 交叉比對：** 12 個 client 收到的 `pid=` 互不相同，而且都出現在 ps 快照中。
- **結束後：** 0 個殭屍、0 個殘留子進程；server log 記錄 12 筆 accepted、12 筆 reaped。

### 6.2 test_robustness：五組 With/Without 對照

以下數字來自 `logs/test_robustness.log`。

| 機制 | 測試情境 | Without（關閉） | With（預設） |
|---|---|---|---|
| [1] 回收子進程 | 3 個 client 依序查詢 | **3 個** `[server] <defunct>` 殭屍 | 0 個殭屍，log 記錄 3 筆回收 |
| [2] 忽略 SIGPIPE | `--mode abort`：收到 1 行就斷線 | 子進程 `killed by signal 13 (Broken pipe)` | `client left before snapshot 3/10`，子進程 exit 3 |
| [3] 長度上限 | `--mode longline`：16 MiB 不換行 | 緩衝區長到 **32768 KiB**，子進程 RSS **17808 KiB** | 讀到 127 bytes 就回 `ERROR request too long` |
| [4] 連線上限 | 10 個 `--mode idle` client 同時連線 | **10 個**子進程 | 5 個子進程，另外 5 個 client 收到 `ERROR server busy` |
| [5] pdeathsig | `WATCH 10 300` 進行中 `kill -9` parent | 子進程變成**孤兒**（PPID 改變），10 筆全部送完 | 子進程跟著結束，client 只收到 3/10 筆 |

**手動展示：** 開兩個終端機視窗，A 跑 server，B 跑 client。

| 機制 | A 視窗 | B 視窗 | 觀察重點 |
|---|---|---|---|
| [1] | `./server --no-sigchld` | `./client` 執行 3 次，再 `ps -o pid,stat,cmd --ppid <parent PID>` | 3 個 `Z` 狀態的 `<defunct>` |
| [2] | `./server --no-sigpipe` 與 `./server` | `./client --mode abort` | A 的 log：`killed by signal 13` 與 `exit 3 (client left)` |
| [3] | `./server --no-validate` 與 `./server` | `./client --mode longline` | A 的 log：`buffer grown to ... KiB` 與 `request too long` |
| [4] | `./server -m 2` | 開 3 個 `./client --mode idle &`，看完再 `pkill -x client` | 第 3 個立刻收到 `ERROR server busy` |
| [5] | `./server --no-pdeathsig` 與 `./server` | `./client -n 20 -w 500`，再 `kill -9 <parent PID>` | client 是否繼續收到資料 |

### 6.3 test_debug：雙層 Debug

| 檢查項目 | release（`make DEBUG=0`） | debug（`make DEBUG=1`） |
|---|---|---|
| gcc 指令帶 `-DENABLE_DEBUG` | 0/3 | 3/3 |
| 執行檔中的 debug 專用格式字串（`strings`） | **0** | 4 |
| `write_all()` 內對 logger 的呼叫（`objdump -d`） | **0** | 2（`log_write@plt`） |
| `libutils.so` text 區段大小 | 8,333 bytes | 8,741 bytes |
| 用 `-d 2` 執行時的 DEBUG 行數 | 0，並警告 `-d 2 has no effect` | — |

debug 版處理一次 `SYSINFO` 時，server log 的行數：

| 等級 | 總行數 | INFO/WARN | DEBUG | hex dump |
|---|---|---|---|---|
| `-d 0` | 6 | 6 | 0 | 0 |
| `-d 1` | 11 | 6 | 5（附 `file:line`） | 0 |
| `-d 2` | 21 | 6 | 15 | 10（請求開頭為 `53 59 53 49 4e 46 4f`，即 `SYSINFO`） |

### 6.4 test_capture：tcpdump 抓包與標頭分析

測試程式在 `lo` 上執行 `tcpdump -i lo -n -w logs/capture.pcap 'tcp port 9090'`，期間建立 4 條連線，共 48 個封包：

| 連線 | client 指令 | 請求 | 用來觀察 |
|---|---|---|---|
| 1 | `./client` | `SYSINFO` | 三向交握、一問一答、雙方的 FIN |
| 2 | `./client -n 3 -w 200` | `WATCH 3 200` | 一個請求、3 個回覆 |
| 3 | `./client -r HELLO` | `HELLO` | `ERROR unknown request` |
| 4 | `./client -n 3 -w 200 --half-close` | `WATCH 3 200` | 半關閉：client 先送 FIN，仍然收到 3 個回覆 |

結束後依序：

1. `tcpdump -r logs/capture.pcap -n -c 10` 逐包列出第一條連線。
2. `tcpdump -r ... -n -x -c 4` 顯示前 4 個封包的 hex。
3. `tcpdump -r ... -n 'tcp[13] & 2 == 2'` 只留下 SYN 和 SYN+ACK：`tcp[13]` 是 TCP 標頭第 13 個 byte（flags），`2` 是 SYN 位元。4 條連線各 2 個，共 8 個封包，和測試程式自己數的一致。
4. 測試程式內建的 pcap 讀取器（24-byte 檔頭、每包 16-byte 紀錄頭）逐包解析，並把請求封包拆成各層標頭，逐欄說明。
5. `tcpdump -r ... -n 'tcp port <連線 4 的 client port>'` 列出半關閉的那條連線。

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

- **三向交握：** SYN → SYN+ACK → ACK。SYN 帶著 MSS 65495、SACK permitted、timestamp 和 window scale 10 這些選項。
- **資料：** 請求 8 bytes（`SYSINFO\n`），回應 109 bytes（一行 `OK ...`）。
- **關閉：** server 先送 FIN（子進程處理完就關閉），client 回 FIN，各自 ACK。
- **IPv4 標頭（20 bytes）：** version 4、IHL 5、type of service `0x00`、total length 60、identification、flags `0x2`（DF）、fragment offset 0、TTL 64、protocol 6（TCP）、header checksum、來源和目的位址。
- **TCP 標頭（32 bytes）：** 來源和目的 port、sequence 和 ack number、data offset 8（20 bytes 加上 12 bytes 的選項）、reserved、flags `0x18`（逐位元列出 `URG 0 ACK 1 PSH 1 RST 0 SYN 0 FIN 0`）、window 64（乘上 SYN 協商的 2^10 = 65536 bytes）、checksum、urgent pointer，以及選項（NOP、NOP、timestamps）。

**半關閉（連線 4）：** client 送出 `WATCH 3 200` 後呼叫 `shutdown(fd, SHUT_WR)`，只關閉自己送出的方向。tcpdump 看到的順序：

```text
#4  client -> server  [P.]  length 12   WATCH 3 200\n
#6  client -> server  [F.]              client 的 FIN，此時 server 還沒回覆
#7  server -> client  [P.]  length 109  OK seq=1 ...
#9  server -> client  [P.]  length 109  OK seq=2 ...（200 ms 後）
#11 server -> client  [P.]  length 109  OK seq=3 ...（再 200 ms 後）
#13 server -> client  [F.]              server 送完才關閉
```

client 的 FIN 出現在 server 第一個回覆之前，之後 3 個回覆仍然送達，表示 TCP 的兩個方向可以分開關閉。server 讀完一行請求後就不再讀取，所以 client 提早送 FIN 不影響回覆。

**用 Wireshark 開啟：** `logs/capture.pcap`，display filter 輸入 `tcp.port == 9090`，對封包按右鍵選 Follow → TCP Stream，就能看到請求和回應的文字。

**權限：** tcpdump 需要 root。用一般帳號執行時，測試程式會先以 `sudo -v` 詢問一次密碼，之後只有 tcpdump 以 root 執行。在 Windows 上也可以直接用 WSL 的 root 執行（先在終端機切到專案資料夾）：

```bash
wsl -d Ubuntu -u root -- make test-capture
```

### 6.5 logs/ 檔案一覽

| 檔案 | 內容 |
|---|---|
| `test_concurrent.log`、`test_robustness.log`、`test_debug.log`、`test_capture.log` | 四項測試的完整輸出 |
| `capture.pcap` | tcpdump 抓包檔，可用 Wireshark 開啟 |
| `capture_decoded.txt` | `tcpdump -r capture.pcap -n -x` 的完整輸出 |
| `real_test.log`、`real_test.pcapng` | Real Test 的紀錄（兩邊的輸出和連線統計）與 Windows 端的抓包檔 |

每個情境的 server、client 細項紀錄（`robust_*`、`concurrent_*`、`debug_*`、`capture_server.log`）沒有放進 repo，重跑測試後會自動產生在 `logs/`。

---

## 7. Real Test：Windows ↔ WSL2

前面的測試都在 loopback（`127.0.0.1`）上進行，封包不會離開 kernel。Real Test 改從 **Windows** 連到 **WSL2** 裡的 server：兩邊是不同的作業系統，各有自己的 IP，封包要經過 Hyper-V 的虛擬網卡 `vEthernet (WSL)`，Wireshark 可以在 Windows 端抓到。

`client.c` 要連結 `libutils.so`，只能在 Linux 執行，所以另外寫了一支 Windows 版的 [tests/win_client.c](tests/win_client.c)。它用 Winsock 送出同樣的一行請求，印出每一行回覆，一樣沒有逾時。

**限制：** 兩邊在同一台實體電腦上，網路是虛擬的，延遲和掉包都比真實網路少。

**步驟：**

1. WSL 終端機：建置 Windows client（需要 mingw-w64，Ubuntu 上用 `sudo apt install mingw-w64` 安裝），查 WSL 的 IP，再啟動 server。

   ```bash
   make win-client && hostname -I
   ```

   ```bash
   ./server -d 1
   ```

2. Wireshark：選 `vEthernet (WSL (Hyper-V firewall))` 介面開始抓包，display filter 輸入 `tcp.port == 9090`。
3. Windows PowerShell：進到專案資料夾，把下面的 `172.17.107.87` 換成第 1 步查到的 IP。

   ```powershell
   .\win_client.exe 172.17.107.87
   .\win_client.exe 172.17.107.87 9090 "WATCH 3 500"
   .\win_client.exe 172.17.107.87 9090 HELLO
   ```

**結果**（[logs/real_test.log](logs/real_test.log)）：

```text
PS> .\win_client.exe 172.17.107.87 9090 "WATCH 3 500"
connected 172.17.96.1:49376 -> 172.17.107.87:9090 in 1.33 ms
request: WATCH 3 500
     6.12 ms  OK seq=1 pid=3334 host=LAPTOP-0F0NVT4H up=0d 00:46:59 load=0.00/0.02/0.00 mem_avail=7.04/7.61 GiB procs=212
   508.19 ms  OK seq=2 pid=3334 host=LAPTOP-0F0NVT4H up=0d 00:46:59 load=0.00/0.02/0.00 mem_avail=7.04/7.61 GiB procs=206
  1010.51 ms  OK seq=3 pid=3334 host=LAPTOP-0F0NVT4H up=0d 00:47:00 load=0.00/0.02/0.00 mem_avail=7.04/7.61 GiB procs=206
  1011.47 ms  server closed the connection: 3 OK line(s), 0 ERROR line(s)
```

| 項目 | 結果 |
|---|---|
| 位址 | client 是 Windows 的 `172.17.96.1`，server 是 WSL2 的 `172.17.107.87`，不是 `127.0.0.1` |
| 三個請求 | `SYSINFO` 收到 1 行 `OK`；`WATCH 3 500` 每隔約 500 ms 收到 1 行，共 3 行；`HELLO` 收到 `ERROR unknown request` |
| Server log | 3 條連線各由一個 child 處理（PID 3333、3334、3335），結束後全部回收，exit code 分別是 0、0、2 |
| 抓包 | `logs/real_test.pcapng` 共 34 個封包、3 條 TCP 連線；Ethernet 位址是 Hyper-V 虛擬網卡（`00:15:5d:...`），每條連線都有三向交握、請求、回應和雙方的 FIN |

---

## 8. 測試環境

- WSL2 Ubuntu，kernel `6.18.33.2-microsoft-standard-WSL2`
- gcc 15.2.0、GNU Make 4.4.1、tcpdump 4.99.6

## 9. 限制

- **只支援 Linux：** 用到 `prctl(PR_SET_PDEATHSIG)`、`sysinfo(2)` 和 `/proc/meminfo`。
- **只支援 IPv4：** server 監聽 `0.0.0.0`，client 用 `AF_INET` 連線。
- **沒有逾時：** server 和 client 都沒有逾時。連上後不送資料的 client 會一直佔著一個子進程，直到它斷線，機制 [4] 限制這種 client 最多佔用的數量；server 不回應時，client 會一直等到使用者按 `Ctrl+C`。
- **沒有加密與身分驗證：** 連得上 server 的人都能讀到系統資訊。
- **`make test-capture` 需要 root 權限。**
