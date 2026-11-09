# NP SysInfo：Fork 多進程系統資訊伺服器

C 語言網路與系統程式設計期中專案。

**學號：** 115ab8044 ・ **姓名：** 顏祺宸

Client 透過自訂的 8-byte 二進位協定，向 server 查詢即時系統資訊（hostname、uptime、CPU load average、記憶體），也能訂閱定時推送，或查詢 OS 版本、CPU 型號與使用率、磁碟用量等延伸資訊。Server 為每個連線 `fork()` 一個子進程處理，雙方共用的工具函式封裝在動態函式庫 `libutils.so`。

**應用情境：** 管理多台 Linux 主機時，維運人員或監控程式常需要快速知道每台主機的負載、記憶體和磁碟狀況。NP SysInfo 把這件事做成輕量的網路服務：主機上跑 `server`，其他人不用登入主機，就能用 `client` 查詢一次、持續訂閱，或取得 OS、CPU、磁碟的詳細資訊。

## 繳交內容導覽

| 項目 | 位置 |
|---|---|
| System design explanation（系統設計說明） | [docs/system-design.md](docs/system-design.md) |
| Description of key source code（關鍵原始碼說明） | [docs/key-source.md](docs/key-source.md) |
| Execution screenshots（執行截圖） | [docs/screenshots.md](docs/screenshots.md) |
| Complete source code（完整原始碼） | 本 repo，檔案清單見[第 1 節](#1-專案架構) |
| Application introduction（應用介紹） | [系統設計第 1 節](docs/system-design.md#1-應用情境與目標) |
| Flow explanation（流程說明） | [系統設計第 3 節](docs/system-design.md#3-執行流程) |
| Real Test（真實測試） | [7.5 節](#75-test_real單機-real-test) |
| Auditing discussion（稽核討論） | [7.6 節](#76-test_audit稽核討論) |

## 需求對照

| 專案要求 | 本專案的做法 |
|---|---|
| Server 用 `fork()` 處理連線，不可用 pthread | parent 只負責 `accept()`，每個連線 fork 一個 child；整個專案沒有使用任何 thread |
| 共用函式封裝成 `.so` | `read_n`/`write_n`、logger、協定收送、系統資訊擷取都在 `libutils.so`，以 `-L. -lutils -Wl,-rpath,'$ORIGIN'` 動態連結 |
| Client 向 server 請求系統資訊 | `SYSINFO_REQ` 單次查詢、`SUBSCRIBE_REQ` 定時推送、`DETAIL_REQ` 延伸資訊（OS、CPU、磁碟） |
| 雙層 Debug | 編譯期 `-DENABLE_DEBUG`（關閉時巨集展開成 `((void)0)`），加上執行期 `g_runtime_debug_level`（`-d 0/1/2`） |
| 穩健機制，至少 3 個 With/Without 執行範例 | 9 個機制、10 組 With/Without 情境（見[第 6 節](#6-九個穩健機制)）；每個機制都能用參數關閉，`tests/test_robustness.c` 自動跑出對照並逐項驗證 |
| 至少同時處理 10 個 client，並提供執行期證明 | `tests/test_concurrent.c` 以 12 個 client 測試，用 `ps aux` 證明 12 個子進程同時存在 |
| 封包擷取與分析 | 固定 8-byte header，magic `0x4E50` 在封包中顯示為 ASCII `NP`；`tests/test_capture.c` 用 tcpdump 抓 loopback 並解碼，`tests/test_real.c` 用 Wireshark 抓 Windows ↔ WSL2 的流量 |
| 使用 Makefile 或 CMake 建置 | `Makefile` 提供建置、展示、測試與稽核的 target |

---

## 1. 專案架構

### 檔案結構

```text
.
├── Makefile             建置 libutils.so、server、client，以及各測試 target
├── protocol.h           8-byte header、opcode、錯誤碼、payload 結構
├── utils.h / utils.c    libutils.so：logger、read_n/write_n、協定收送、系統資訊
├── server.c             fork 多進程 server，9 個穩健機制可個別關閉
├── client.c             CLI client：查詢、訂閱（含自動重連）、延伸資訊、壓力測試、故障模擬
├── tests/               C 測試程式
│   ├── testlib.h / testlib.c   共用函式：fork + exec、regex 過濾、讀取 /proc
│   ├── demo.c                  一個指令的現場展示（約 2～3 秒）
│   ├── test_concurrent.c       12 個 client 併發證明（ps aux PID）
│   ├── test_robustness.c       10 組穩健性情境的 Without/With 自動對比
│   ├── test_debug.c            雙層 Debug 證明（編譯期與執行期）
│   ├── test_capture.c          tcpdump 抓包與協定解碼
│   ├── test_real.c             單機 Real Test：Windows ↔ WSL2、模擬壞網路與斷線
│   ├── test_audit.c            稽核：靜態分析、sanitizer、負載、連線洪水、惡意 server
│   ├── np_client_win.c         獨立實作的 NP client，編譯成 Windows 程式給 test_real 使用
│   └── hostile_server.c        假的惡意 server，只給 test_audit 使用
├── docs/                系統設計、關鍵原始碼說明與執行截圖
└── logs/                測試結果總結與抓包檔（重跑測試會產生細項 log）
```

### 進程模型

```text
 client 1 ──┐                                ┌─► child (pid A)：服務 client 1，結束後 exit
 client 2 ──┼──► server parent ── fork() ────┼─► child (pid B)：服務 client 2，結束後 exit
   ...      │    只做 pselect + accept        │       ...
 client N ──┘                                └─► child (pid N)
                   ▲
                   └── SIGCHLD → waitpid(-1, &status, WNOHANG) 回收結束的 child
```

- **parent：** 只負責等待連線、`accept()`、`fork()` 和回收子進程，並記錄每個 child 服務哪個 client 位址（連線上限用）。除了連線數達上限或 fork 失敗時回傳一個錯誤封包，不讀寫任何 client 資料。
- **child：** 處理一條連線上的所有請求，連線結束就 `_exit()`。exit code 代表結束原因（見 [3.1 Server](#31-server)）。
- **不留孤兒：** child 設定了 `PR_SET_PDEATHSIG`。parent 結束時，即使是被 `kill -9`，child 也會收到 `SIGTERM`。

### libutils.so 模組

| 模組 | 函式 | 說明 |
|---|---|---|
| Logger | `log_init`、`log_write`、`log_hexdump`、`LOG_INFO/WARN/ERROR`、`LOG_DEBUG`、`LOG_HEXDUMP`、`g_runtime_debug_level` | 每行格式為 `HH:MM:SS.mmm [tag:pid] LEVEL msg`，整行只用一次 `write()` 輸出，多個進程同時寫也不會交錯 |
| Robust I/O | `read_n`、`write_n`、`read_n_until`、`mono_ms`、`set_socket_timeout` | 讀寫滿 n bytes，遇到 `EINTR` 自動重試；失敗時保留 errno（`EAGAIN` 代表逾時，另有 `EPIPE`、`ECONNRESET`）。`read_n_until` 用 `poll()` 加上 `CLOCK_MONOTONIC` 的固定期限，資料再怎麼慢慢來都會在期限到時放棄（`ETIMEDOUT`） |
| 協定 | `proto_send`、`proto_send_error`、`proto_recv_header`、`proto_recv_header_budget`、`proto_recv_body`、`proto_recv_body_until`、`proto_status_str`、`proto_opcode_name`、`proto_errcode_str` | header 和 body 用一次 `write()` 送出；接收 header 時可選擇要不要驗證；`_budget`／`_until` 版本加上整筆請求的期限 |
| 系統資訊 | `sysinfo_collect`、`sysinfo_hton`、`sysinfo_ntoh`、`sysdetail_collect`、`sysdetail_hton`、`sysdetail_ntoh` | 資料來源是 `gethostname`、`sysinfo(2)`、`getloadavg`、`/proc/meminfo`；延伸資訊來自 `uname(2)`、`/proc/cpuinfo`、`/proc/stat`（取樣 200 ms 算 CPU 使用率）、`statvfs("/")` |
| 其他 | `parse_int_range`、`sleep_ms`、`set_signal` | 命令列整數解析、被 signal 中斷後會繼續睡的 sleep，以及用 `sigaction` 設定 `SIG_IGN`／`SIG_DFL` |

`libutils.so` 共匯出 27 個符號，可用 `make inspect` 查看。

---

## 2. 建置

**需求：** Linux（本專案在 WSL2 Ubuntu 上測試）、gcc、GNU make。測試程式另外會用到 `ps`、`strings`、`objdump`、`size`、`tcpdump`。

```bash
make
```

`make` 預設編出 debug 版（帶 `-DENABLE_DEBUG`）。實際的編譯指令如下：

```text
gcc -Wall -Wextra -pedantic -std=gnu11 -O2 -g -DENABLE_DEBUG -fPIC -shared -o libutils.so utils.c
gcc -Wall -Wextra -pedantic -std=gnu11 -O2 -g -DENABLE_DEBUG -o server server.c -L. -Wl,-rpath,'$ORIGIN' -lutils
gcc -Wall -Wextra -pedantic -std=gnu11 -O2 -g -DENABLE_DEBUG -o client client.c -L. -Wl,-rpath,'$ORIGIN' -lutils
```

debug 和 release 兩種模式都是 **0 warning**（`-Wall -Wextra -pedantic`）。

| Make target | 作用 |
|---|---|
| `make`、`make all` | debug 版（`DEBUG=1`，預設） |
| `make release` | release 版（`DEBUG=0`，`LOG_DEBUG` 完全編譯掉） |
| `make demo` | 一個指令的現場展示（約 2～3 秒）：`.so` 連結、系統資訊、12 個併發子進程、拒絕垃圾封包、沒有殭屍進程 |
| `make test` | 依序執行 `tests/test_concurrent.c`、`tests/test_robustness.c`、`tests/test_debug.c` |
| `make test-concurrent`、`make test-robust`、`make test-debug` | 個別執行一支測試 |
| `make test-capture` | tcpdump 抓包測試，需要 root 或 sudo |
| `make test-real` | 單機 Real Test，不需要 root |
| `make audit` | 靜態分析、ASan/UBSan、負載、連線洪水、惡意 server 測試 |
| `make inspect` | 列出 `libutils.so` 匯出的符號，以及 `ldd` 連結結果 |
| `make capture`、`make capture-pcap` | 用 `sudo tcpdump` 即時以 hex 顯示 loopback 流量，或存成 `capture.pcap` |
| `make clean` | 刪除 `.so`、執行檔與 `.build_flags` |

注意事項：

- **切換 DEBUG 會自動全部重編。** Makefile 把 CFLAGS 記錄在 `.build_flags`，`DEBUG=1` 和 `DEBUG=0` 互換時，`.so` 和兩個執行檔會一起重編，避免兩邊的 debug 設定不一致。
- **函式庫跟著執行檔走。** `-Wl,-rpath,'$ORIGIN'` 讓執行檔載入同一個資料夾裡的 `libutils.so`，從任何目錄啟動都可以，也不會載入目前目錄裡別人放的同名函式庫。`ldd ./server` 會顯示 `libutils.so` 的完整路徑。
- **在 Windows 上透過 WSL 建置：** 先開啟 Ubuntu，再下載並編譯。

  ```bash
  git clone https://github.com/t115ab8044/NP_SysInfo.git
  cd NP_SysInfo
  make
  ```

---

## 3. 使用方式

### 3.1 Server

```bash
./server                    # 監聽 0.0.0.0:9090，所有機制全開，socket 逾時與請求期限 5 秒
./server -p 9000 -d 1       # 改用 port 9000，開啟 verbose debug
./server --no-sigchld       # 關閉機制 [1]，示範殭屍進程
./server -m 10 --max-per-ip 5   # 最多 10 個子進程，同一個位址最多 5 個
```

| 參數 | 說明 |
|---|---|
| `-p, --port PORT` | 監聽的 port（預設 9090） |
| `-d, --debug LEVEL` | 執行期 debug 等級：0 只有一般訊息，1 詳細流程，2 封包追蹤 |
| `-t, --timeout SEC` | 每條連線的 socket 逾時秒數，也是單筆請求的期限（預設 5，0 表示都不設） |
| `-m, --max-clients N` | 機制 [4]：同時最多 N 個子進程（預設 128） |
| `--max-per-ip N` | 機制 [4]：同一個 client 位址最多 N 個子進程（預設不限） |

With/Without 對照用的開關（預設全部開啟，加上參數就關掉該機制）：

| 參數 | 關掉的機制 | 關掉後的行為 |
|---|---|---|
| `--no-sigchld` | [1] SIGCHLD 回收 | 不回收子進程，產生殭屍進程 |
| `--single-wait` | [1b] handler 內的 `waitpid` 迴圈 | 每個 SIGCHLD 只回收一個子進程；多個 signal 合併時留下殭屍 |
| `--no-sigpipe` | [2] 忽略 SIGPIPE | 寫入已關閉的連線時子進程被殺掉 |
| `--no-validate` | [3] header 驗證 | 不檢查 magic、version、長度 |
| `--no-timeout` | [3] socket 逾時 | 不設 `SO_RCVTIMEO`/`SO_SNDTIMEO`（等同 `-t 0`） |
| `--no-limit` | [4] 連線上限 | 每個連線都 fork，沒有任何上限 |
| `--no-deadline` | [5] 請求期限 | 只剩每次 `read()` 的逾時，慢慢送的 client 能把子進程佔住 |
| `--naive-recv` | [6] TCP 串流切訊息 | 假設「一次 `read()` 就是一則訊息」 |
| `--no-heartbeat` | [7] 心跳 | 對不再回 ACK 的 client 繼續推送 |
| `--no-pdeathsig` | [9] 跟著 parent 結束 | parent 被殺後，子進程變成孤兒繼續執行 |

子進程的 exit code 由 parent 在回收時記錄到 log，例如 `reaped child 548: exit 4 (client gone, EPIPE/ECONNRESET handled)`：

| exit code | 意義 |
|---|---|
| 0 | client 正常關閉連線 |
| 1 | I/O 錯誤 |
| 2 | socket 逾時（閒置或卡住的 client） |
| 3 | 請求被拒絕（header 驗證失敗或未知的 opcode） |
| 4 | client 已離線，寫入時收到 `EPIPE`/`ECONNRESET` 並正常處理 |
| 5 | 心跳中斷：client 停止回 ACK（被凍結或網路斷了） |
| 6 | 請求超過期限：慢速傳送的 client 被切斷 |

子進程如果是被 signal 殺掉的，log 會顯示 `killed by signal 13 (Broken pipe)`。

按 Ctrl+C 或送 `SIGTERM` 時，server 會先關閉 listening socket（新連線立即被拒），對還在執行的子進程送 `SIGTERM`、最多等 2 秒把它們回收，再記錄統計後結束。

### 3.2 Client

```bash
./client                      # 查詢一次，印出報表
./client --detail             # 延伸資訊：OS、CPU 型號與使用率、磁碟用量
./client -n 5                 # 同一條連線上送 5 次請求
./client -w 1000              # 訂閱：server 每 1000 ms 推送一次，直到 Ctrl+C；server 斷線會自動重連
./client -w 1000 -n 10        # 訂閱 10 次推送後結束
./client --hold 5             # 收到回應後保持連線 5 秒（併發測試用）
./client --stress 100 -n 10   # fork 100 個 client 同時連線、各送 10 次，輸出延遲統計
./client --bind 127.0.0.2     # 從另一個 loopback 位址連線（模擬另一台電腦）
./client --mode garbage       # 故障模擬（穩健性測試用）
```

| 參數 | 說明 |
|---|---|
| `-H, --host HOST` | server 位址（預設 127.0.0.1） |
| `-p, --port PORT` | server port（預設 9090） |
| `-d, --debug LEVEL` | 執行期 debug 等級 0/1/2 |
| `-n, --count N` | 每條連線的請求數（預設 1）；搭配 `-w` 時是推送次數（預設 0，表示不停止）；搭配 `--mode pipeline` 時是黏在一起的請求數（預設 5） |
| `-w, --watch MS` | 訂閱模式：server 每 MS 毫秒推送一次（server 會把間隔限制在 50–60000 ms）。不限次數的訂閱每收到一筆就回一個 ACK（機制 [7]） |
| `-t, --timeout SEC` | socket 逾時秒數（預設 10，0 表示不設） |
| `-q, --quiet` | 每筆資料只印一行 |
| `--detail` | 改送 `DETAIL_REQ`，印出延伸資訊 |
| `--bind ADDR` | 用這個本機位址連線，例如 `127.0.0.2` |
| `--hold SEC` | 最後一次回應後保持連線 SEC 秒；在故障模式下則是等待 server 反應的秒數（預設 8） |
| `--stress N` | fork N 個 client 同時連線（1–2000），輸出成功率、延遲與不同 server PID 的數量；不可和 `--mode`、`-w` 一起用 |
| `--no-reconnect` | 關閉機制 [8]：訂閱中斷線就直接結束 |
| `--retries N` | 機制 [8] 的重連次數（預設 5，間隔 1、2、4、8、8… 秒） |
| `--mode MODE` | 故障模擬，說明見下表 |
| `--abort-after K` | abort 模式下，收到 K 次推送後斷線（預設 1） |
| `--drip-ms MS` | fragment／slow 模式下每個 byte 之間的間隔（預設 100／3000） |

| 故障模式 | 行為 | 用來測試 |
|---|---|---|
| `garbage` | 送出一段 HTTP 請求，而不是 NP 封包 | 機制 [3]：magic 驗證 |
| `biglen` | 送出合法的 header，但 `body_len = 0xFFFFFFFF` | 機制 [3]：body 長度上限 |
| `idle` | 連線後什麼都不送 | 機制 [3]：socket 逾時 |
| `abort` | 訂閱推送，收到 K 次後立刻斷線 | 機制 [2]：SIGPIPE |
| `slow` | 16 bytes 的請求每 3 秒送 1 byte | 機制 [5]：整筆請求的期限 |
| `fragment` | 8 bytes 的請求拆成 8 個 1-byte 的 TCP 封包，間隔 100 ms | 機制 [6]：拆包 |
| `pipeline` | 5 個請求黏在一次 `write()` 裡送出 | 機制 [6]：黏包 |

**延伸資訊的輸出（`./client --detail`）：**

```text
============= NP SysInfo detail =============
  OS            : Linux 6.18.33.2-microsoft-standard-WSL2 (x86_64)
  CPU model     : 11th Gen Intel(R) Core(TM) i5-1135G7 @ 2.40GHz
  CPU cores     : 8 online
  CPU usage     : 0.00 %   (sampled over 200 ms)
  Disk /        : 940.93 GiB free of 1006.85 GiB  (6.5 % used)
  Served by PID : 2083  (fork()ed server child)
=============================================
```

Client 的 exit code：0 成功，1 失敗，2 表示故障模式下 server 完全沒有反應。

**單次查詢的輸出：**

```text
================ NP SysInfo ================
  Hostname      : LAPTOP-0F0NVT4H
  Uptime        : 0d 00:01:53
  Load average  : 0.87  0.20  0.06   (1 / 5 / 15 min)
  Memory total  : 7.61 GiB
  Memory free   : 6.38 GiB
  Memory avail  : 7.00 GiB  (92.0 %)
  Tasks         : 262
  Served by PID : 434  (fork()ed server child)
============================================
```

`Served by PID` 是負責這條連線的 server 子進程 PID，所以 client 的輸出本身就能證明每條連線由不同的進程處理。

---

## 4. 通訊協定：固定 8-byte header

每則訊息都是固定 8 bytes 的 header，後面接選用的 body。所有多位元組欄位都是 **big-endian（network byte order）**。

```text
 Byte:   0      1        2         3        4      5      6      7
      +------+------+---------+--------+------+------+------+------+-----------
      |  magic 0x4E50 | version | opcode |          body_len         | body ...
      |  'N'    'P'   |  0x01   |        |    (bytes, <= 4096)       |
      +------+------+---------+--------+------+------+------+------+-----------
```

| Offset | 欄位 | 型別 | 說明 |
|---|---|---|---|
| 0 | `magic` | `uint16` | 固定 `0x4E50`，在 tcpdump、Wireshark 中顯示為 ASCII `NP` |
| 2 | `version` | `uint8` | 目前為 `0x01` |
| 3 | `opcode` | `uint8` | 訊息種類，見下表 |
| 4 | `body_len` | `uint32` | body 長度（不含 header），上限 4096 |

結構大小都用 `_Static_assert` 在編譯期鎖死：header 8、subscribe 8、sysinfo 120、ack 4、detail 224、error 64 bytes。

**Opcode：** client 送出的使用 `0x01`–`0x7F`，server 送出的使用 `0x80`–`0xFF`。

| Opcode | 名稱 | 方向 | Body |
|---|---|---|---|
| `0x01` | `SYSINFO_REQ` | client → server | 無 |
| `0x02` | `SUBSCRIBE_REQ` | client → server | `proto_subscribe_t`（8 bytes） |
| `0x03` | `DETAIL_REQ` | client → server | 無 |
| `0x04` | `ACK` | client → server | `proto_ack_t`（4 bytes：收到的 `seq`），只用在不限次數的訂閱 |
| `0x81` | `SYSINFO_RESP` | server → client | `proto_sysinfo_t`（120 bytes） |
| `0x83` | `DETAIL_RESP` | server → client | `proto_detail_t`（224 bytes） |
| `0xFF` | `ERROR` | server → client | `proto_error_t`（64 bytes） |

**`proto_sysinfo_t`（120 bytes）：** 每個欄位都在自然對齊的位置上。

| Offset | 欄位 | 型別 | 說明 |
|---|---|---|---|
| 0 | `server_pid` | `uint32` | 負責這條連線的子進程 PID |
| 4 | `seq` | `uint32` | 單次回應為 0，訂閱推送為 1..N |
| 8 | `hostname` | `char[64]` | 以 NUL 結尾 |
| 72 | `uptime_sec` | `uint64` | 開機秒數 |
| 80 | `mem_total_kb` | `uint64` | 總記憶體（kB） |
| 88 | `mem_free_kb` | `uint64` | 空閒記憶體（kB） |
| 96 | `mem_avail_kb` | `uint64` | 可用記憶體 MemAvailable（kB） |
| 104 | `load_x100[3]` | `uint32 × 3` | 1、5、15 分鐘 load average × 100（定點數，避免在網路上傳浮點數） |
| 116 | `nprocs` | `uint32` | `sysinfo(2)` 回報的 task 數 |

**`proto_subscribe_t`（8 bytes）：** `interval_ms`（`uint32`）、`count`（`uint32`，0 表示推送到 client 離線為止）。

**`proto_detail_t`（224 bytes）：** `server_pid`、`ncpu`、`cpu_busy_x100`、`sample_ms`（各 `uint32`），`sysname[32]`、`release[64]`、`machine[32]`、`cpu_model[64]`（字串），`disk_total_kb`、`disk_avail_kb`（`uint64`）。

**`proto_error_t`（64 bytes）：** `code`（`uint32`）、`message`（`char[60]`）。

| 錯誤碼 | 名稱 | 意義 |
|---|---|---|
| 1 | `ERR_BAD_MAGIC` | magic 不是 `0x4E50` |
| 2 | `ERR_BAD_VERSION` | 不支援的協定版本 |
| 3 | `ERR_BAD_LENGTH` | `body_len` 超過上限、不符合 opcode，或訊息不完整 |
| 4 | `ERR_UNKNOWN_OP` | 未知的 opcode，或在不該出現的時機送來的 opcode |
| 5 | `ERR_INTERNAL` | server 內部錯誤 |
| 6 | `ERR_BUSY` | 連線數達上限（機制 [4]），由 parent 直接回覆，不 fork |
| 7 | `ERR_HEARTBEAT` | 太久沒收到 ACK，訂閱被結束（機制 [7]） |
| 8 | `ERR_DEADLINE` | 請求沒有在期限內收完（機制 [5]） |

**訊息流程：**

```text
client --SYSINFO_REQ (no body)-------------------------> server
client <--------------------SYSINFO_RESP (120 bytes)---- server

client --SUBSCRIBE_REQ (interval_ms, count)------------> server
client <--SYSINFO_RESP x count, one every interval_ms--- server
client --ACK (seq) after every push--------------------> server   (count = 0 only)

client --DETAIL_REQ (no body)--------------------------> server
client <--------------------DETAIL_RESP (224 bytes)----- server

invalid request -> server replies ERROR and closes the connection
server full     -> the parent replies ERROR(ERR_BUSY) without forking
```

原本的 `SYSINFO_REQ`／`SYSINFO_RESP` 封包格式完全沒變（8 + 120 = 128 bytes）。

**抓包中的實際位元組**（`logs/capture_decoded.txt`）：

```text
請求  4e 50 01 01 00 00 00 00                   "NP......"   SYSINFO_REQ，body_len 0
回應  4e 50 01 81 00 00 00 78  00 00 d3 21 ...   "NP.....x"   SYSINFO_RESP，body_len 120，server_pid 54049
```

---

## 5. 雙層 Debug 控制

| 層級 | 控制方式 | 效果 |
|---|---|---|
| 編譯期 | `make`（帶 `-DENABLE_DEBUG`）或 `make release` | 關閉時，`LOG_DEBUG` 和 `LOG_HEXDUMP` 展開成 `((void)0)`：沒有程式碼、沒有格式字串，也沒有等級判斷 |
| 執行期 | `-d 0/1/2`，設定 `g_runtime_debug_level` | 0：只有 INFO、WARN、ERROR<br>1：加上 DEBUG 訊息（附 `file:line`）<br>2：再加上封包追蹤（解碼後的 header 與 hex dump） |

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

release 版仍可接受 `-d`，但會警告 `built without -DENABLE_DEBUG: -d 2 has no effect`。

`-d 2` 的輸出範例（server 收到請求並回應）：

```text
DEBUG utils.c:275: RX header magic=0x4E50 ver=1 op=SYSINFO_REQ(0x01) body_len=0
DEBUG     0000  4e 50 01 01 00 00 00 00                           |NP......|
DEBUG utils.c:242: TX SYSINFO_RESP(0x81) body_len=120
DEBUG     0000  4e 50 01 81 00 00 00 78  00 00 02 42 00 00 00 00  |NP.....x...B....|
```

---

## 6. 九個穩健機制

每個機制都可以用參數單獨關閉，由 `tests/test_robustness.c` 自動跑 Without 和 With 兩種情況並比較（結果見 [7.2](#72-test_robustness十組-withoutwith-對比)）。

| # | 異常情況 | 機制 | 關閉參數 |
|---|---|---|---|
| [1] | 子進程結束後沒人回收 | SIGCHLD handler + `waitpid(WNOHANG)` | `--no-sigchld` |
| [1b] | 很多子進程同時結束，SIGCHLD 合併成一個 | handler 內用迴圈呼叫 `waitpid` | `--single-wait` |
| [2] | 寫入已離線的 client | 忽略 SIGPIPE，處理 `EPIPE` | `--no-sigpipe` |
| [3] | 亂碼、偽造長度、連上後不說話 | header 驗證 + `SO_RCVTIMEO`/`SO_SNDTIMEO` | `--no-validate --no-timeout` |
| [4] | 一台主機開大量連線把 server 撐爆 | 總連線上限 + 每個位址的上限，超過就回 `ERR_BUSY`、不 fork | `--no-limit` |
| [5] | 每 3 秒送 1 byte 的慢速攻擊 | 從第一個 byte 起算的整筆請求期限 | `--no-deadline` |
| [6] | TCP 拆包、黏包 | 依長度精確讀取（`read_n`） | `--naive-recv` |
| [7] | 訂閱中的 client 被凍結（Ctrl+Z）或網路斷了 | 心跳：每筆推送都要回 ACK | `--no-heartbeat` |
| [8] | server 重啟或當機 | client 自動重連，指數退避 | client 端 `--no-reconnect` |
| [9] | parent 被 `kill -9` | `PR_SET_PDEATHSIG`，子進程跟著結束 | `--no-pdeathsig` |

### [1] 防止殭屍進程：SIGCHLD + `waitpid`

- **問題：** 子進程結束後，如果 parent 不呼叫 `wait`，它會一直以 `<defunct>` 狀態佔用 process table。
- **作法：**
  - 用 `sigaction`（`SA_NOCLDSTOP`）註冊 SIGCHLD handler，在 handler 裡以 `while (waitpid(-1, &status, WNOHANG) > 0)` 迴圈回收。用迴圈是因為多個 child 同時結束時，可能只會收到一次 SIGCHLD。
  - 規格中的寫法是 `waitpid(-1, NULL, WNOHANG)`。這裡改成保留 `status`，目的只是讓 log 能記錄每個 child 怎麼結束的，例如被 SIGPIPE 殺掉，這也是機制 [2] 的證據。
  - handler 只做 async-signal-safe 的事：把 PID 和 status 存進 ring buffer，由 main loop 負責輸出 log。
  - parent 平時把 SIGCHLD、SIGINT、SIGTERM 擋住，只在 `pselect()` 等待連線時才原子性地解除封鎖。這樣 signal 不會落在「輸出回收記錄」和「開始等待」之間的空檔，回收記錄能立即寫進 log。這是實測時發現的 race：修正前，12 個連線只記錄到 5 筆回收。
- **關閉方式：** `--no-sigchld`。SIGCHLD 會維持 `SIG_DFL`。這裡不能設成 `SIG_IGN`，因為在 Linux 上 `SIG_IGN` 會讓 kernel 自動回收子進程，殭屍問題就看不到了。

### [2] SIGPIPE 與安全 I/O

- **問題：** 對已經關閉的連線呼叫 `write()` 會觸發 SIGPIPE，預設動作是直接終止進程，沒有任何錯誤處理或 log。
- **作法：**
  - 用 `set_signal(SIGPIPE, SIG_IGN)`（內部是 `sigaction`）忽略 `SIGPIPE` 後，`write()` 會改成回傳 -1，errno 為 `EPIPE`。
  - `read_n` 和 `write_n` 遇到 `EINTR` 會自動重試，也會補完 short read/write。發生錯誤時回傳 -1 並保留 errno。
  - server 子進程遇到 `EPIPE` 或 `ECONNRESET` 時，記錄 `client gone` 後以 exit 4 正常結束。client 端同樣忽略 SIGPIPE。
  - `write_n` 刻意用 `write()`，而不是 `send(..., MSG_NOSIGNAL)`，這樣機制開關的差異才觀察得到。
- **關閉方式：** `--no-sigpipe`。

### [3] Header 驗證 + Socket 逾時

- **問題：**
  - 如果直接相信 header，一個偽造的 `body_len = 0xFFFFFFFF` 會讓子進程配置 4 GiB 記憶體，然後永遠等待不會來的資料。
  - 連上之後什麼都不送的 client（slowloris 型態），也能讓子進程永遠卡住。
- **作法：**
  - `proto_recv_header()` 檢查 magic `0x4E50`、版本，以及 `body_len ≤ 4096`。不符合就回傳 `ERROR` 封包並關閉連線。
  - 關閉前先 `shutdown(SHUT_WR)`，再讀掉殘留的輸入（lingering close）。這是因為關閉一個還有未讀資料的 socket 時，kernel 會送出 RST，可能把剛送出的錯誤回應一起毀掉。
  - 用 `SO_RCVTIMEO` 和 `SO_SNDTIMEO` 設定每條連線的逾時（預設 5 秒，可用 `-t` 調整）。
- **關閉方式：** `--no-validate --no-timeout`。

### [1b] Signal 不會排隊：`waitpid` 一定要寫成迴圈

- **問題：** 一般 signal 不會排隊。SIGCHLD 還沒處理時又有子進程結束，kernel 不會再記一次，所以「一個 SIGCHLD」可能代表好幾個已結束的子進程。parent 平時把 SIGCHLD 擋住（見 [1]），合併的情形就更常見。
- **作法：** handler 用 `while (waitpid(-1, &status, WNOHANG) > 0)`，一直回收到沒有為止。
- **關閉方式：** `--single-wait`，handler 每次只呼叫一次 `waitpid`。30 個 client 同時離開，結果留下 28 個殭屍。

### [4] 連線上限，並依 client 位址公平分配

- **問題：** 每個連線都 fork，代表一台主機只要大量連線，就能讓 server 產生無限多個進程（類似 fork bomb）。只設總上限也不夠：一台主機佔滿名額，其他主機就全部被擋在外面。
- **作法：**
  - parent 用一張表記錄每個子進程的 PID 和 client 位址；回收子進程時從表中移除。
  - fork 之前先檢查總數（`-m`，預設 128）和同一位址的數量（`--max-per-ip`）。超過時 parent 直接回 `ERR_BUSY`，**不 fork**；回覆後先 `shutdown(SHUT_WR)` 再讀掉 client 已送出的請求，避免 `close()` 送出 RST 把回覆毀掉。
  - 示範技巧：Linux 上整段 `127.0.0.0/8` 都是本機，client 加 `--bind 127.0.0.2` 就能在一台電腦上模擬「另一台主機」。
- **關閉方式：** `--no-limit`。

### [5] 慢速攻擊：整筆請求的期限

- **問題：** `SO_RCVTIMEO` 是「每次 `read()`」的逾時，每收到一個 byte 就重新計時。client 每 3 秒送 1 byte，每次 `read()` 都在 5 秒內拿到資料，永遠不會逾時；宣告 4096 bytes 的 body 再慢慢送，就能佔住一個子進程約 4.5 小時。
- **作法：**
  - 等待請求的第一個 byte 仍由 `SO_RCVTIMEO` 負責（閒置的 client，機制 [3]）。
  - 第一個 byte 到達後，算出一個固定的期限（預設 5 秒後），剩下的 header 和 body 都用 `read_n_until()` 讀：每次 `poll()` 只等「離期限還剩多少時間」，時間到就回 `ERR_DEADLINE` 並結束（exit 6）。
  - 期限用 `CLOCK_MONOTONIC` 計算，系統時間被調整也不受影響。
- **關閉方式：** `--no-deadline`。

### [6] TCP 是位元組串流：拆包與黏包

- **問題：** TCP 不保留訊息邊界。一則訊息可能分好幾次才收到（拆包），好幾則訊息也可能一次收到（黏包）。常見的錯誤寫法是「`read()` 一次就當作一則訊息」。
- **作法：** 先用 `read_n` 讀滿 8 bytes 的 header，再依 `body_len` 讀滿 body；下一則訊息的 bytes 留在 socket 裡，等下一輪再讀。
- **關閉方式：** `--naive-recv`，改用「一次 `read()` 一則訊息」的寫法：拆包時只讀到 1 byte 就判定格式錯誤；黏包時只處理第一則，其餘 4 則被丟掉。

### [7] 心跳：發現被凍結的 client

- **問題：** 訂閱中的 client 被暫停（Ctrl+Z）、當掉或網路斷線時，對方的 kernel 仍會接收資料，server 的 `write()` 一直成功，完全不知道沒有人在讀。要等兩邊的緩衝區都塞滿才會卡住，在 100 ms 推送一次的情況下需要好幾分鐘。
- **作法：**
  - 不限次數的訂閱（`count = 0`）中，client 每收到一筆推送就回一個 `ACK(seq)`。
  - server 在兩次推送之間用 `poll()` 讀 ACK；超過 3 秒（或 2 倍推送間隔加 1 秒，取較大者）沒有收到，就回 `ERR_HEARTBEAT` 並結束訂閱（exit 5）。
  - client 送出 FIN 不算離線：TCP 允許半關閉、只收不送的連線，client 是否真的離開仍由寫入結果判斷（機制 [2]）。
- **關閉方式：** `--no-heartbeat`。

### [8] Client 自動重連（指數退避）

- **問題：** 前面的機制都在保護 server。server 重啟或當機時，訂閱中的 client 會直接結束。
- **作法：** 訂閱模式的 client 斷線後，依 1、2、4、8、8… 秒的間隔重試（預設 5 次），連上後重新訂閱；有限次數的訂閱會接著收剩下的筆數。協定錯誤（例如 `ERR_BAD_MAGIC`）重連也無法解決，所以不重試；`ERR_BUSY`、`ERR_HEARTBEAT` 和斷線則會重試。client 比 server 先啟動時，也會用同樣的方式等 server 起來。
- **關閉方式：** client 端 `--no-reconnect`。

### [9] Parent 被 `kill -9`：子進程不能變成孤兒

- **問題：** `SIGKILL` 無法攔截，parent 來不及做任何清理。子進程會被過繼給 init，繼續佔著連線，沒有人管。
- **作法：** 子進程在 fork 後呼叫 `prctl(PR_SET_PDEATHSIG, SIGTERM)`，parent 一死，kernel 就送 `SIGTERM` 給它。設定後再檢查一次 `getppid()`，處理 parent 在 fork 和 `prctl` 之間就死掉的情況。
- **關閉方式：** `--no-pdeathsig`。

### 其他防護

- listening socket 設成 non-blocking：`pselect()` 回報有連線、但連線在 `accept()` 之前就被重置時，不會卡住。
- 優雅關機：收到 SIGINT 或 SIGTERM 時，先關閉 listening socket，對還在執行的子進程送 `SIGTERM`，最多等 2 秒把它們回收，再記錄統計後結束。
- parent 本身一律忽略 SIGPIPE（它只在回覆 `ERR_BUSY` 時寫入 client）；機制 [2] 的開關只影響子進程。
- 訂閱的推送間隔限制在 50–60000 ms 之間。

---

## 7. 測試程式

六支測試程式都是 C 寫的，透過 `fork` + `exec` 執行，不經過 shell。它們都會自行啟動 server，每項檢查印出 `[PASS]` 或 `[FAIL]`，全部通過時以 exit code 0 結束。完整輸出存在 `logs/`。可用環境變數 `PORT` 改 port，`tests/test_concurrent.c` 另外支援 `CLIENTS` 和 `HOLD`。

| 測試程式 | 證明內容 | 執行方式 | 結果 |
|---|---|---|---|
| `tests/test_concurrent.c` | 12 個 client 同時連線，每條連線由不同的子進程處理 | `make test-concurrent` | 9/9 PASS |
| `tests/test_robustness.c` | 9 個穩健機制、10 組情境的 Without/With 對比 | `make test-robust` | 27/27 PASS |
| `tests/test_debug.c` | 編譯期與執行期的雙層 Debug | `make test-debug` | 15/15 PASS |
| `tests/test_capture.c` | tcpdump 抓包與 8-byte header 解碼 | `make test-capture`（需要 root 或 sudo） | 11/11 PASS |
| `tests/test_real.c` | Windows ↔ WSL2 跨系統連線、模擬壞網路與斷線 | `make test-real` | 19/19 PASS |
| `tests/test_audit.c` | 靜態分析、原始碼規則、執行檔防護、負載、連線洪水、sanitizer、惡意 server | `make audit` | 25/25 PASS |

### 7.1 test_concurrent：12 個 client 併發

12 個 client 同時連線，每個在收到回應後保持連線 4 秒，測試程式在這段時間內拍下 `ps aux` 快照。

```text
    PID    PPID STAT     ELAPSED CMD
    505     477 S+         00:01 ./server -p 9090
    526     505 S+         00:01  \_ ./server -p 9090
    527     505 S+         00:01  \_ ./server -p 9090
    ...                              (共 12 個 child，PID 526–537)
server children alive           : 12
established TCP conns on :9090  : 12
```

- **ps aux 證明：** 共 13 個 `./server` 進程（1 個 parent 和 12 個 child），每個 child 的 PPID 都是 parent，`ss` 顯示有 12 條 ESTABLISHED 連線。
- **PID 交叉比對：** 12 個 client 回報的 server PID 互不相同，而且都出現在 ps 快照中。
- **結束後：** 所有 client 離開後，沒有殭屍進程，也沒有殘留的子進程；server log 記錄 12 筆接受、12 筆回收。
- **額外的壓力測試：** 100 個 worker 進程各送 10 次請求。

```text
===== Stress test: 100 worker process(es) x 10 request(s) =====
  Requests     : 1000 ok, 0 failed (100.0 % success)
  Latency (ms) : avg 8.115 | p50 0.153 | p95 84.520 | p99 125.116 | max 134.597
  Wall time    : 0.169 s (5931.6 ok req/s)
  Server PIDs  : 100 distinct fork()ed server children
  Workers      : 100 exited ok, 0 failed
```

### 7.2 test_robustness：十組 Without/With 對比

每組情境都先用關掉機制的 server（或 client）跑一次，再用預設設定跑一次，並自動檢查差異。以下數字來自 `logs/test_robustness.log`。

| 機制 | 測試情境 | Without（關閉） | With（預設） |
|---|---|---|---|
| [1] SIGCHLD + `waitpid` | 12 個 client 各查詢一次後離開 | **12 個** `[server] <defunct>` 殭屍進程，0 筆回收記錄 | **0 個**殭屍，log 記錄 12 筆回收 |
| [1b] `waitpid` 迴圈 | 30 個 client 幾乎同時離開 | `--single-wait`：handler 只回收到 2 個，留下 **28 個**殭屍 | **0 個**殭屍，30/30 回收 |
| [2] `SIG_IGN` + EPIPE | 3 個 client 訂閱（每 200 ms 推送），收到 1 次後斷線 | 3 個子進程 `killed by signal 13 (Broken pipe)` | 3 個子進程記錄 `Broken pipe -> client gone` 後以 exit 4 結束 |
| [3] Header 驗證 + 逾時 | garbage、biglen、idle 三個 client，各等待 8 秒 | 6.5 秒時 3 個子進程仍卡住；biglen 的子進程 VSZ 達 **4,197,128 KB**，garbage 的達 774,936 KB；3 個 client 都沒收到任何回應 | garbage 立即收到 `bad magic number`，biglen 立即收到 `body length exceeds limit`，idle 在 **5.23 秒**後被逾時關閉；6.5 秒時沒有殘留子進程 |
| [4] 連線上限 + 每位址上限 | 127.0.0.1 開 20 個閒置連線，接著 127.0.0.2 送一個正常請求 | `--no-limit`：一台主機就產生 **20 個**子進程<br>只設總上限 `-m 10`：10 個子進程，**127.0.0.2 也被擋**（`server busy: 10/10`） | `-m 10 --max-per-ip 5`：127.0.0.1 只拿到 **5 個**，其餘 15 個收到 `too many connections from your address (5/5)`；**127.0.0.2 正常取得服務** |
| [5] 請求期限 | 16 bytes 的請求每 3 秒送 1 byte | 6.5 秒時子進程仍在等；client 9 秒後放棄（只送出 3/16 bytes），子進程一直被佔住 | 第一個 byte 後 **5.00 秒**被切斷：`error 8 (request deadline exceeded)`，子進程 exit 6 |
| [6] TCP 串流切訊息 | fragment：請求拆成 8 個 1-byte 封包<br>pipeline：5 個請求黏在一次 `write()` | `--naive-recv`：拆包被判定為 `incomplete message`；黏包只回 **1/5**（log：`the other 32 byte(s) are discarded`） | 拆包在 0.70 秒後組回完整請求並回應；黏包回 **5/5** |
| [7] 心跳 | 每 100 ms 推送的訂閱，client 被 `SIGSTOP`（等同 Ctrl+Z）凍結 5 秒 | `--no-heartbeat`：5 秒後子進程仍在推送；恢復後 client 一口氣印出 **70 筆**累積的舊資料 | 3.0 秒沒收到 ACK 就結束訂閱（`no ACK for 3.0 s (last one for push #10)`，exit 5）；恢復後 client 讀完緩衝區，看到 `error 7: heartbeat lost` |
| [8] 自動重連 | 訂閱中 server 停止，2 秒後重新啟動 | `--no-reconnect`：`connection lost after 4 push(es)`，client 結束 | 依 1 秒、2 秒退避，第 2 次重連成功並重新訂閱；client 先後由 **2 個不同的** server 子進程服務 |
| [9] `PR_SET_PDEATHSIG` | 3 個忙碌的子進程，parent 被 `kill -9` | `--no-pdeathsig`：**3 個孤兒**繼續執行，PPID 變成 init | 子進程跟著結束，**0 個**殘留 |

機制 [2] 不論開或關，受影響的都只有子進程：server parent 都存活，也能繼續服務新的 client（測試程式會實際再連一次確認）。

**手動展示（兩個終端機視窗，A 跑 server、B 跑 client）：**

| 機制 | A 視窗 | B 視窗 | 觀察重點 |
|---|---|---|---|
| [5] | `./server` 與 `./server --no-deadline` | `./client --mode slow` | With 約 5 秒被切斷；Without 8 秒都沒反應 |
| [6] | `./server` 與 `./server --naive-recv` | `./client --mode fragment`、`./client --mode pipeline` | Without 拆包失敗、黏包只回 1/5 |
| [7] | `./server` 與 `./server --no-heartbeat` | `./client -w 200`，按 Ctrl+Z，等 5 秒後輸入 `fg` | With 時 A 出現 `no ACK for 3.0 s`；Without 時 `fg` 後一次湧出大量舊資料 |
| [8] | `./server`，Ctrl+C 停掉後再啟動 | `./client -w 500` 與 `./client -w 500 --no-reconnect` | With 自動重連，`pid=` 換成新的子進程 |
| [9] | `./server -t 30` 與 `./server -t 30 --no-pdeathsig` | `./client --mode idle --hold 20 &`，再 `kill -9 <parent PID>` | 用 `ps -ef \| grep '[.]/server'` 看有沒有 PPID 變了的孤兒 |

### 7.3 test_debug：雙層 Debug

**編譯期：** release 版（`make DEBUG=0`）與 debug 版（`make DEBUG=1`）比較如下。

| 檢查項目 | release | debug |
|---|---|---|
| gcc 指令帶 `-DENABLE_DEBUG` | 0/3 | 3/3 |
| 執行檔中的 debug 專用格式字串（`strings`） | **0** | 5 |
| `proto_send()` 內對 logger 的呼叫（`objdump -d`） | **0**（只有 `write_n`） | 2（`log_write@plt`、`log_hexdump@plt`） |
| `libutils.so` text 區段大小 | 15,533 bytes | 17,328 bytes |
| 用 `-d 2` 執行時的 DEBUG 行數 | 0，並警告 `-d 2 has no effect` | — |

**執行期：** debug 版處理一次請求時，server log 的行數如下。

| 等級 | 總行數 | INFO/WARN | DEBUG | 封包追蹤 |
|---|---|---|---|---|
| `-d 0` | 8 | 8 | 0 | 0 |
| `-d 1` | 12 | 8 | 4（附 `file:line`） | 0 |
| `-d 2` | 25 | 8 | 17 | 13（header 解碼與 hex dump，開頭為 `4e 50`） |

### 7.4 test_capture：tcpdump 抓包

測試程式在 `lo` 上以 `tcpdump -i lo -nn -U -w logs/capture.pcap 'tcp port 9090'` 抓包，期間送出四種流量：一次查詢、3 次推送的訂閱、garbage 和 biglen。抓完後讀回 pcap：

- 逐包列出第一條連線：三向交握、8 bytes 的請求、128 bytes 的回應，以及 FIN。
- 用 `tcpdump -X` 顯示請求與回應的 hex，ASCII 欄可以看到 `NP`。
- 用測試程式內建的解碼器，把每段 TCP payload 拆成 NP 訊息（環境中沒有 tshark）。

```text
seg  dir  ports       bytes   magic   ver  opcode           body_len
  1  C->S 51498>9090      8   0x4E50    1  SYSINFO_REQ             0
  2  S->C 9090>51498    128   0x4E50    1  SYSINFO_RESP          120
  3  C->S 51500>9090     16   0x4E50    1  SUBSCRIBE_REQ           8
  4  S->C 9090>51500    128   0x4E50    1  SYSINFO_RESP          120
  5  S->C 9090>51500    128   0x4E50    1  SYSINFO_RESP          120
  6  S->C 9090>51500    128   0x4E50    1  SYSINFO_RESP          120
  7  C->S 51512>9090     36   non-NP data: "GET / HTTP/1.1.."
  8  S->C 9090>51512     72   0x4E50    1  ERROR                  64
  9  C->S 51524>9090      8   0x4E50    1  SYSINFO_REQ    4294967295
 10  S->C 9090>51524     72   0x4E50    1  ERROR                  64
```

共抓到 44 個封包、10 個資料段、9 則 NP 訊息。server 送出的每一段都剛好是「8-byte header 加上 body_len」，garbage 被辨識為非 NP 資料，偽造的 `body_len 4294967295` 在封包中清楚可見。

**用 Wireshark 開啟：** `logs/capture.pcap`，display filter 為 `tcp.port == 9090 && tcp.len > 0`。header 欄位依序是 `tcp.payload[0:2] == 4e:50`（magic）、`[2]` version、`[3]` opcode、`[4:4]` body_len。

**權限：** tcpdump 需要 root。用 root 身分執行時直接抓包；用一般帳號執行時，測試程式會先以 `sudo -v` 詢問一次密碼，之後只有 tcpdump 以 root 執行。在 WSL 上也可以直接用 root 執行：

```bash
wsl -d Ubuntu -u root -- make test-capture
```

請在 Windows 終端機中先切到專案資料夾再執行這行指令，WSL 會沿用目前的目錄。

### 7.5 test_real：單機 Real Test

本專案只用一台實體電腦測試，所以用兩種方式離開 loopback。第一部分是真實的 host 與 VM 連線，第二部分的網路故障是模擬的；兩者都不等於兩台實體電腦。

**第一部分：Windows ↔ WSL2。** `tests/np_client_win.c` 用 MinGW-w64 編譯成 Windows 程式，依照協定規格獨立撰寫，和 `client.c` 沒有共用程式碼或標頭檔。它連到 WSL2 VM 裡的 C server，兩邊是不同的作業系統、核心和 IP，流量走虛擬乙太網路 `vEthernet (WSL)`，並由 Wireshark 的 `dumpcap` 錄下。

| 檢查 | 實測結果 |
|---|---|
| 連線路徑 | `172.17.96.1` 連到 `172.17.107.87`，server log 沒有 `127.0.0.1` |
| 跨系統併發 | 12 條連線由 12 個不同的子進程回應，`ps` 同時看到 12 個子進程 |
| 跨系統異常 | 垃圾資料收到 `error 1`（bad magic number）；訂閱中送 RST，子進程 `exit 4` |
| 收尾 | 15 條連線全部服務完畢，0 個子進程未回收 |
| Wireshark 抓包 | 29 個 frame 帶有 NP 協定資料，15 次連線請求 |

用 Wireshark 開啟 `logs/real_windows_wsl.pcapng`，display filter 輸入 `tcp.payload[0:2] == 4e:50`，就能看到 NP 封包。

**第二部分：兩個網路堆疊與故障注入。** 用 Linux network namespace 建立兩個獨立的網路環境，以 veth 虛擬網路線相連（`10.99.0.1`、`10.99.0.2`），再用 `tc netem` 製造故障。不需要 root。

| 情境 | 實測結果 |
|---|---|
| 每個方向延遲 50 ms、掉包 5% | RTT 100.9 ms；60 個請求全部成功，p50 100.6 ms、p99 432 ms（掉包靠 TCP 重傳救回） |
| 訂閱中斷線 6 秒（第 0 秒斷線） | 第 3 秒內 server 靠心跳發現斷線，子進程 `exit 5`；client 也發現斷線並開始重連 |
| 網路恢復 | 恢復約 2 秒後，client 第 2 次重連成功，繼續收到推送 |
| 抓包 | `logs/real_net.pcap` 共 300 個封包，含真實的 Ethernet 標頭與 MAC，4 個 ARP，`tcp[13] & 2 == 2` 找到 30 個 SYN、SYN+ACK |

### 7.6 test_audit：稽核討論

`tests/test_audit.c` 在暫存副本裡建置和測試，不會動到專案目錄。下面的數字是單次執行的結果。

**通過的項目：**

| 面向 | 檢查 | 結果 |
|---|---|---|
| 靜態分析 | `gcc -fanalyzer`；`-pedantic -Wformat=2 -Wshadow -Wnull-dereference -Wwrite-strings` | 0 個問題、0 個 warning |
| 原始碼規則 | `strcpy`、`sprintf`、`gets`、`system` 等；執行緒 API；舊式 `signal()` | 都沒有使用 |
| 執行檔防護 | PIE、完整 RELRO、stack canary、`RUNPATH=$ORIGIN` | 都有 |
| 長時間負載 | 1,000 條連線、10,000 個請求 | 全部成功；parent 的檔案描述元維持 5 個，記憶體只多 68 KB |
| 記憶體與未定義行為 | ASan + UBSan 下跑併發與穩健性測試 | 9/9、27/27，0 份錯誤報告 |
| 惡意 server | 回傳沒有結尾的字串、超長封包 | client 全部安全處理，0 份錯誤報告 |

**稽核中發現並已修正：**

| 問題 | 風險 | 修正 |
|---|---|---|
| 仍有 6 處使用舊式 `signal()` | 行為依平台而異，課堂也建議避免 | 全部改用 `sigaction`（`set_signal()`） |
| `rpath` 設成 `.` | 換目錄就無法啟動；在別人的目錄執行可能載入惡意 `libutils.so` | 改用 `$ORIGIN` |

**仍然存在的風險與取捨：**

| 弱點 | 實測證據 | 影響 | 改善方式 |
|---|---|---|---|
| 明文傳輸 | 封包裡直接看得到主機名稱 | 網路上的人可以監聽 | TLS，或走 SSH tunnel |
| 沒有身分驗證，監聽所有網卡 | `INADDR_ANY`（`server.c`） | 連得上的人都能讀系統資訊 | 加 token、只綁定內部網卡、防火牆限制來源 |
| 預設不限制單一 IP | 同一台主機開 130 條連線，占滿全部 128 個名額，下一個 client 被拒絕 | 一台主機就能阻斷服務 | 啟動時加 `--max-per-ip`；實測 `--max-per-ip 20` 能把該主機限制在 20 條 |

**稽核軌跡：** 每條連線都會留下時間、來源 IP 和 port、子進程 PID，以及結束原因（`accepted`、`reaped child ... exit N (原因)` 兩行可用 PID 對應）。正式使用時可以用 `./server 2>&1 | tee audit.log` 保存。

**這次稽核沒有涵蓋：** 子進程用 `_exit()` 結束，ASan 的記憶體洩漏檢查涵蓋不到它們；沒有使用 Valgrind，也沒有做滲透測試。

### 7.7 logs/ 檔案一覽

| 檔案 | 內容 |
|---|---|
| `test_concurrent.log`、`test_robustness.log`、`test_debug.log`、`test_capture.log`、`test_real.log`、`test_audit.log` | 六項測試的完整輸出 |
| `real_windows_wsl.pcapng` | Windows ↔ WSL2 測試的 Wireshark 抓包檔 |
| `real_net.pcap` | 模擬網路測試的抓包檔 |
| `capture.pcap`、`capture_decoded.txt` | tcpdump 抓包檔與完整的 `tcpdump -X` 輸出 |

每個情境的 server、client 細項紀錄（`robust_m*`、`concurrent_*`、`debug_*` 等）沒有放進 repo，重跑測試後會自動產生在 `logs/`。

---

## 8. 測試環境

- WSL2 Ubuntu，kernel `6.18.33.2-microsoft-standard-WSL2`
- gcc 15.2.0、GNU Make 4.4.1、tcpdump 4.99.6
- Real Test 另外用到 Windows 11、MinGW-w64（`sudo apt install gcc-mingw-w64-x86-64`）、Wireshark（`dumpcap`、`tshark`）

延遲與吞吐量會隨機器負載變動，上面的數字是單次執行的結果。

## 9. 限制

- **只支援 Linux：** 用到 `prctl(PR_SET_PDEATHSIG)`、`sysinfo(2)` 和 `/proc/meminfo`。
- **只支援 IPv4：** server 監聽 `0.0.0.0`，client 用 `AF_INET` 連線。
- **Real Test 只用一台實體電腦：** 詳見 [7.5 節](#75-test_real單機-real-test)。
- **沒有加密與身分驗證：** 詳見 [7.6 節](#76-test_audit稽核討論)。
- **`make test-capture` 需要 root 權限。**
