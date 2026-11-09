# 關鍵原始碼說明（Description of Key Source Code）

[← 回到 README](../README.md) ・ [系統設計說明](system-design.md) ・ [執行截圖](screenshots.md)

以下片段都來自本 repo 的原始碼，`/* ... */` 表示省略與重點無關的部分。點擊「位置」的連結可以看到完整程式。

## 1. Parent 主迴圈：只負責接連線與 fork

**位置：** [`server.c` 的 `main()`](../server.c#L958-L1006)

```c
while (!g_stop) {
    /* ... */
    report_reaped_children();
    FD_ZERO(&rfds);
    FD_SET(lfd, &rfds);
    if (pselect(lfd + 1, &rfds, NULL, NULL, NULL, &wait_mask) < 0) {
        if (errno != EINTR)
            LOG_ERROR("pselect: %s", strerror(errno));
        continue;               /* EINTR: SIGCHLD or shutdown request */
    }
    cfd = accept(lfd, (struct sockaddr *)&addr, &addr_len);
    /* ... */
    if (limit_reason(addr.sin_addr, why, sizeof(why)) != NULL) {
        reject_busy(cfd, peer, why);
        continue;
    }

    pid = fork();
    /* ... */
    if (pid == 0)
        run_child(lfd, cfd, peer, self, &wait_mask);

    track_child(pid, addr.sin_addr);
    /* ... */
    close(cfd);         /* the child owns the connection now */
}
```

- 每條連線都 `fork()` 一個 child，符合「不使用 thread」的要求。
- `fork()` 之後兩邊都持有連線的 fd，所以 parent 要 `close(cfd)`，child 也會關閉 listening socket。
- 超過連線上限時直接拒絕，不會 fork，避免一台主機把 server 撐爆。

## 2. SIGCHLD handler：安全地回收 child

**位置：** [`server.c` 的 `on_sigchld()`](../server.c#L117-L134)

```c
static void on_sigchld(int sig)
{
    int saved_errno = errno;
    int status;
    pid_t pid;

    (void)sig;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int h = g_reap_head;

        g_reap_pid[h % REAP_RING]    = pid;
        g_reap_status[h % REAP_RING] = status;
        g_reap_head = h + 1;
        if (!g_cfg.reap_loop)
            break;
    }
    errno = saved_errno;
}
```

- signal 不會排隊，一個 SIGCHLD 可能代表好幾個 child 結束，所以 `waitpid` 要寫成迴圈。關掉迴圈時，30 個 child 同時結束會留下 28 個殭屍。
- `WNOHANG` 讓 handler 不會卡住。
- handler 裡不呼叫 `printf`，只把結果存入 ring buffer，由主迴圈輸出 log。
- handler 可能打斷任何程式，所以要保存並還原 `errno`。

## 3. Signal 封鎖與 `pselect`：消除競爭條件

**位置：** [`server.c` 的 `main()`](../server.c#L947-L951)，[`install_signals()`](../server.c#L142-L166)

```c
sigemptyset(&blocked);
sigaddset(&blocked, SIGCHLD);
sigaddset(&blocked, SIGINT);
sigaddset(&blocked, SIGTERM);
sigprocmask(SIG_BLOCK, &blocked, &wait_mask);
```

- 平時封鎖這三個 signal，只在 `pselect()` 等待連線時，由 kernel 原子性地換回 `wait_mask`。
- 如果 SIGCHLD 落在「寫出回收紀錄」和「開始等待」之間，紀錄會延到下一條連線才輸出。修正前，12 個連線只記錄到 5 筆回收。
- signal 一律用 `sigaction` 設定，不用舊式的 `signal()`。

## 4. Child 初始化：還原 signal，並跟著 parent 結束

**位置：** [`server.c` 的 `run_child()`](../server.c#L649-L678)

```c
close(lfd);
/* ... */
set_signal(SIGINT, SIG_DFL);
set_signal(SIGTERM, SIG_DFL);
set_signal(SIGCHLD, SIG_DFL);
/* [2] write() to a vanished client returns EPIPE instead of killing us. */
set_signal(SIGPIPE, g_cfg.ignore_sigpipe ? SIG_IGN : SIG_DFL);
sigprocmask(SIG_SETMASK, mask, NULL);
/* ... */
if (g_cfg.pdeathsig) {
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() != parent)
        _exit(CHILD_IO_ERROR);
}

LOG_DEBUG(DBG_VERBOSE, "%s: child started", peer);
rc = serve_client(cfd, peer);
/* ... */
_exit(rc);
```

- child 會繼承 parent 的 signal 設定和封鎖狀態，所以要先還原。
- 忽略 SIGPIPE 後，寫入已離線的 client 時 `write()` 會回傳 `EPIPE`，程式能正常處理，不會被 signal 殺掉。
- `SIGKILL` 無法攔截，所以由 kernel 透過 `PR_SET_PDEATHSIG` 通知 child。之後再檢查 `getppid()`，處理 parent 在 `fork` 和 `prctl` 之間就結束的情況。
- `_exit(rc)` 的 exit code 代表結束原因，parent 回收時會寫進 log。

## 5. `read_n`：完整讀取，解決 TCP 拆包

**位置：** [`utils.c` 的 `read_n()`](../utils.c#L154-L175)，對應的 [`write_n()`](../utils.c#L177-L196)

```c
ssize_t read_n(int fd, void *buf, size_t n)
{
    unsigned char *p = buf;
    size_t done = 0;

    while (done < n) {
        ssize_t r = read(fd, p + done, n - done);

        if (r > 0) {
            done += (size_t)r;
        } else if (r == 0) {
            break;                      /* EOF: peer closed */
        } else if (errno == EINTR) {
            LOG_DEBUG(DBG_VERBOSE, "read_n: interrupted by a signal (EINTR), retrying");
        } else {
            /* ... */
            return -1;
        }
    }
    return (ssize_t)done;
}
```

- TCP 是位元組串流，一次 `read()` 可能只拿到部分資料，所以要讀到滿 n bytes 為止。
- 被 signal 中斷（`EINTR`）時自動重試。發生其他錯誤時回傳 -1 並保留 `errno`。
- 實測中，1 則請求拆成 8 段能組回完整請求，5 則黏在一起的請求也都得到回應。

## 6. Header 驗證與協定結構

**位置：** [`protocol.h` 的 header](../protocol.h#L66-L71)，[`utils.c` 的 `proto_recv_header_budget()`](../utils.c#L331-L372)

```c
typedef struct __attribute__((packed)) {
    uint16_t magic;             /* PROTO_MAGIC */
    uint8_t  version;           /* PROTO_VERSION */
    uint8_t  opcode;            /* enum proto_opcode */
    uint32_t body_len;          /* body size in bytes, header excluded */
} proto_header_t;

_Static_assert(sizeof(proto_header_t) == PROTO_HEADER_SIZE,
               "protocol header must be exactly 8 bytes on the wire");
```

```c
memcpy(hdr, raw, sizeof(raw));
hdr->magic    = ntohs(hdr->magic);
hdr->body_len = ntohl(hdr->body_len);
/* ... */
if (!validate)
    return PROTO_OK;
if (hdr->magic != PROTO_MAGIC)
    return PROTO_BAD_MAGIC;
if (hdr->version != PROTO_VERSION)
    return PROTO_BAD_VERSION;
if (hdr->body_len > PROTO_MAX_BODY)
    return PROTO_BAD_LENGTH;
return PROTO_OK;
```

- 收到資料後先轉回主機位元組序，再檢查 magic、版本和長度。
- 沒有驗證時，偽造的 `body_len = 0xFFFFFFFF` 會讓 child 配置約 4 GiB 的虛擬記憶體，並一直等待不會來的資料。
- `_Static_assert` 確保結構大小和協定規格一致。

## 7. 整筆請求期限：擋下慢速傳送

**位置：** [`utils.c` 的 `read_n_until()`](../utils.c#L211-L252)

```c
while (done < n) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    uint64_t now = mono_ms();
    /* ... */
    if (now >= deadline_ms) {
        /* ... */
        errno = ETIMEDOUT;
        return -1;
    }
    left = deadline_ms - now;
    pr = poll(&pfd, 1, left > INT_MAX ? INT_MAX : (int)left);
    /* ... */
    r = read(fd, p + done, n - done);
    /* ... */
}
```

- `SO_RCVTIMEO` 每次 `read()` 都重新計時，所以每 3 秒送 1 byte 永遠不會逾時。
- 這裡的 `poll()` 只等「離固定期限還剩多少時間」，資料再慢也會在期限到時放棄。
- 期限用 `CLOCK_MONOTONIC` 計算，系統時間被調整也不受影響。

## 8. 連線上限：依 client 位址公平分配

**位置：** [`server.c` 的 `limit_reason()`](../server.c#L683-L698)

```c
if (g_cfg.max_clients > 0 && g_nchildren >= (size_t)g_cfg.max_clients) {
    snprintf(buf, len, "server busy: %zu/%d connections in use",
             g_nchildren, g_cfg.max_clients);
    return buf;
}
if (g_cfg.max_per_ip > 0 && (n = children_from(ip)) >= (size_t)g_cfg.max_per_ip) {
    snprintf(buf, len, "too many connections from your address (%zu/%d)",
             n, g_cfg.max_per_ip);
    return buf;
}
return NULL;
```

- 同時檢查總數和同一位址的數量。只有總上限時，一台主機占滿名額會連帶擋住其他主機。
- 這個檢查在 `fork()` 之前，被拒絕的連線不會產生 child。
- 每個位址的上限預設關閉，要用 `--max-per-ip` 開啟。

## 9. 心跳：發現被凍結的 client

**位置：** [`server.c` 的 `wait_for_acks()`](../server.c#L331-L397)，[逾時設定](../server.c#L423-L426)

```c
if (hb->enforce && now - hb->last_ack_ms > hb->timeout_ms) {
    LOG_WARN("%s: no ACK for %.1f s (last one for push #%u) -> client frozen or "
             "unreachable, ending the subscription",
             peer, (double)(now - hb->last_ack_ms) / 1000.0, hb->last_seq);
    proto_send_error(cfd, ERR_HEARTBEAT, NULL);
    return CHILD_HEARTBEAT;
}
```

- client 被 Ctrl+Z 凍結時，對方的 kernel 仍會接收資料，`write()` 一直成功，只看寫入結果發現不了問題。
- 不限次數的訂閱中，client 每收到一筆推送就要回 ACK。超過 3 秒（或 2 倍推送間隔加 1 秒，取較大者）沒有收到，就結束訂閱。

## 10. Lingering close：讓錯誤回應確實送達

**位置：** [`server.c` 的 `lingering_close()`](../server.c#L637-L647)

```c
static void lingering_close(int cfd)
{
    unsigned char sink[512];
    int reads = 0;

    shutdown(cfd, SHUT_WR);
    set_socket_timeout(cfd, 1);
    while (reads < 16 && read(cfd, sink, sizeof(sink)) > 0)
        reads++;
    close(cfd);
}
```

- 關閉一個還有未讀資料的 socket 時，kernel 會送出 RST，可能把剛送出的錯誤回應一起毀掉。
- 所以先 `shutdown(SHUT_WR)` 半關閉，讀掉殘留資料後再 `close()`，client 就能讀到錯誤訊息。

## 11. 雙層 Debug 巨集

**位置：** [`utils.h`](../utils.h#L50-L67)

```c
#ifdef ENABLE_DEBUG
#define LOG_DEBUG_ENABLED 1
#define LOG_DEBUG(lvl, ...)                                                 \
    do {                                                                    \
        if (g_runtime_debug_level >= (lvl))                                 \
            log_write(LOG_LVL_DEBUG, __FILE__, __LINE__, __VA_ARGS__);      \
    } while (0)
/* ... */
#else
/* Compiled out: no code, no format strings, no runtime level check. */
#define LOG_DEBUG_ENABLED 0
#define LOG_DEBUG(lvl, ...)           ((void)0)
#define LOG_HEXDUMP(label, data, len) ((void)0)
#endif
```

- 編譯期：沒有 `-DENABLE_DEBUG` 時巨集展開成 `((void)0)`，程式碼和字串都不會進入執行檔。
- 執行期：`-d 0/1/2` 設定 `g_runtime_debug_level`，決定要輸出多少訊息。
- `__FILE__` 和 `__LINE__` 讓每一行 debug 訊息都能對應到原始碼位置。
