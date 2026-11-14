# 關鍵原始碼說明（Description of Key Source Code）

[← 回到 README](../README.md) ・ [系統設計說明](system-design.md) ・ [執行截圖](screenshots.md) ・ [稽核討論](audit.md)

以下片段都來自本 repo 的原始碼，`/* ... */` 表示省略與重點無關的部分。點擊「位置」的連結可以看到完整程式。

## 1. Parent 主迴圈：只負責接連線與 fork

**位置：** [`server.c` 的 `main()`](../server.c#L458-L496)

```c
for (;;) {
    /* ... */
    report_reaped_children();
    cfd = accept(lfd, (struct sockaddr *)&addr, &addr_len);
    if (cfd < 0) {
        if (errno != EINTR)     /* EINTR: a SIGCHLD arrived, log it */
            LOG_ERROR("accept: %s", strerror(errno));
        continue;
    }
    /* ... */
    report_reaped_children();   /* children that ended during accept() */
    if (g_cfg.max_clients > 0 && g_active >= g_cfg.max_clients) {
        reject_busy(cfd, peer);
        continue;
    }

    pid = fork();
    /* ... */
    if (pid == 0)
        run_child(lfd, cfd, peer, self);

    g_served++;
    g_active++;
    LOG_INFO("accepted %s -> child pid %d (connection #%d) | active=%d", /* ... */);
    close(cfd);                 /* the child owns the connection now */
}
```

- 每條連線都 `fork()` 一個 child，整個專案沒有使用 thread。
- `fork()` 之後兩邊都持有連線的 fd：parent 要 `close(cfd)`，child 則關閉 listening socket。
- `set_signal()` 沒有設 `SA_RESTART`，所以 SIGCHLD 會讓 `accept()` 回傳 `EINTR`。parent 回到迴圈開頭，先把剛回收的 child 寫進 log。
- 達到連線上限時直接拒絕，不會 fork（機制 [4]）。

## 2. SIGCHLD handler：回收 child（機制 [1]）

**位置：** [`server.c` 的 `on_sigchld()`](../server.c#L78-L93)，輸出 log 的 [`report_reaped_children()`](../server.c#L106-L122)

```c
static void on_sigchld(int sig)
{
    int saved_errno = errno;
    int status;
    pid_t pid;

    (void)sig;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int h = g_reap_head;

        g_reap_pid[h % REAP_RING] = pid;
        g_reap_status[h % REAP_RING] = status;
        g_reap_head = h + 1;
    }
    errno = saved_errno;
}
```

- signal 不會排隊，一個 SIGCHLD 可能代表好幾個 child 結束，所以 `waitpid` 要寫成迴圈，一直收到沒有為止。
- `WNOHANG` 讓 handler 不會卡住。
- `printf()` 不能在 signal handler 裡呼叫，所以 handler 只把 PID 和 status 存進 ring buffer，由主迴圈用 `WIFEXITED`／`WIFSIGNALED` 解讀後寫 log。
- handler 可能打斷任何程式，所以要保存並還原 `errno`。
- `--no-sigchld` 時不註冊 handler，結束的 child 全部變成 `<defunct>`。

## 3. Child 初始化：跟著 parent 結束（機制 [5]）

**位置：** [`server.c` 的 `run_child()`](../server.c#L287-L304)

```c
static void run_child(int lfd, int cfd, const char *peer, pid_t parent)
{
    int rc;

    if (g_cfg.pdeathsig) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent)    /* the parent died before prctl() */
            _exit(CHILD_IO_ERROR);
    }
    close(lfd);                     /* only the parent accepts */
    set_signal(SIGCHLD, SIG_DFL);   /* the child has no children to reap */
    rc = serve_client(cfd, peer);
    close(cfd);
    /* ... */
    _exit(rc);
}
```

- parent 被 `kill -9` 時無法通知任何人，所以改由 kernel 負責：`PR_SET_PDEATHSIG` 讓 parent 一死，child 就收到 `SIGTERM`。
- 設定之後再檢查 `getppid()`：parent 如果在 `fork()` 和 `prctl()` 之間就死了，child 已經被過繼，設定不會生效，要自己結束。
- `_exit(rc)` 的 exit code 代表結束原因，parent 回收時會寫進 log。

## 4. 忽略 SIGPIPE，處理 EPIPE（機制 [2]）

**位置：** [`server.c` 的 `main()`](../server.c#L436-L440)，[`send_snapshots()`](../server.c#L222-L251)

```c
/* main(): children inherit this through fork() */
if (g_cfg.ignore_sigpipe)
    set_signal(SIGPIPE, SIG_IGN);
```

```c
for (int seq = 1; seq <= count; seq++) {
    /* ... */
    n = sysinfo_format(&info, (unsigned)seq, line, sizeof(line));
    if (write_all(cfd, line, (size_t)n) < 0) {
        if (errno == EPIPE || errno == ECONNRESET) {
            LOG_INFO("%s: client left before snapshot %d/%d (%s) -> stop",
                     peer, seq, count, strerror(errno));
            return CHILD_PEER_GONE;
        }
        /* ... */
    }
    if (seq < count)
        sleep_ms((unsigned)interval_ms);
}
```

- 對已經關閉的連線 `write()`，預設會收到 SIGPIPE 直接被殺掉。忽略之後，`write()` 改成回傳 -1、errno 為 `EPIPE`。
- signal 的處理方式會透過 `fork()` 繼承，所以只要在 parent 設定一次。
- client 斷線後，第一次寫入只會讓對方回 RST，第二次寫入才收到 `EPIPE`。`WATCH` 每隔一段時間寫一次，正好能穩定重現。

## 5. 請求長度上限（機制 [3]）

**位置：** [`utils.c` 的 `read_line()`](../utils.c#L155-L181)，對照組 [`read_line_unlimited()`](../server.c#L140-L187)

```c
ssize_t read_line(int fd, char *buf, size_t cap)
{
    size_t len = 0;

    while (len < cap - 1) {
        char c;
        ssize_t r = read(fd, &c, 1);

        if (r == 1) {
            if (c == '\n') {
                if (len > 0 && buf[len - 1] == '\r')
                    len--;
                buf[len] = '\0';
                return (ssize_t)len;
            }
            buf[len++] = c;
        } else if (r == 0) {
            buf[len] = '\0';
            return READ_EOF;
        } else if (errno != EINTR) {
            buf[len] = '\0';
            return READ_ERR;
        }
    }
    buf[len] = '\0';
    return READ_TOO_LONG;
}
```

- 緩衝區大小固定（`MAX_LINE` 128），讀滿 127 bytes 還沒看到 `\n` 就回傳 `READ_TOO_LONG`，server 回 `ERROR request too long` 後結束。
- 一次讀 1 byte，`\n` 之後的資料留在 socket 裡，不會被多讀走。
- 對照組 `read_line_unlimited()` 每次把緩衝區加倍，直到看到 `\n`。16 MiB 不換行的請求讓它長到 32 MiB。

## 6. 連線上限：parent 直接拒絕（機制 [4]）

**位置：** [`server.c` 的 `reject_busy()`](../server.c#L313-L321)

```c
static void reject_busy(int cfd, const char *peer)
{
    static const char busy[] = "ERROR server busy, try again later\n";

    LOG_WARN("%s rejected: %d clients connected, limit %d", peer, g_active, g_cfg.max_clients);
    if (send(cfd, busy, sizeof(busy) - 1, MSG_DONTWAIT | MSG_NOSIGNAL) < 0)
        LOG_DEBUG(DBG_FLOW, "%s: busy reply not sent: %s", peer, strerror(errno));
    close(cfd);
}
```

- `g_active` 是還沒回收的 child 數量：fork 後加 1，回收時減 1。檢查前會先處理一次回收紀錄，避免數字偏高。
- 這是 parent 唯一會寫 client 的地方，所以特別小心：`MSG_DONTWAIT` 讓它不會卡住，`MSG_NOSIGNAL` 讓它不會被 SIGPIPE 殺掉。
- 被拒絕的連線不會產生 child。

## 7. 解析請求

**位置：** [`server.c` 的 `serve_client()`](../server.c#L253-L285)

```c
fields = sscanf(line, "%15s %3d %5d %1s", cmd, &count, &interval_ms, extra);
if (strcmp(line, "SYSINFO") == 0) {
    count = 1;
    interval_ms = 0;
} else if (fields >= 1 && strcmp(cmd, "WATCH") == 0) {
    if (fields != 3 || count < 1 || count > WATCH_MAX_COUNT ||
        interval_ms < WATCH_MIN_MS || interval_ms > WATCH_MAX_MS) {
        LOG_WARN("%s: bad WATCH arguments \"%s\" -> rejected", peer, line);
        send_error(cfd, "bad arguments: WATCH <count 1-100> <ms 100-10000>");
        return CHILD_BAD_REQUEST;
    }
} else {
    LOG_WARN("%s: unknown request \"%.40s\" -> rejected", peer, line);
    send_error(cfd, "unknown request: use SYSINFO or WATCH <count> <ms>");
    return CHILD_BAD_REQUEST;
}
```

- `%15s` 限制指令長度，避免寫出 `cmd` 的範圍。
- `%3d`、`%5d` 限制數字最多讀 3 位和 5 位，剛好容納上限 100 和 10000。沒有寬度時，`WATCH 4294967297 100` 會讓 `int` 溢位，變成 `WATCH 1 100` 被接受（稽核時發現，見 [稽核討論](audit.md)）。
- 多讀一個 `%1s`：如果後面還有多餘的字，`fields` 會是 4，請求就被拒絕。數字超過寬度時，剩下的位數也會被 `%1s` 讀到，一樣被拒絕。
- `count` 和 `interval_ms` 都有上下限，client 無法要求無限次或過於密集的回報。

## 8. `write_all`：寫完全部資料

**位置：** [`utils.c` 的 `write_all()`](../utils.c#L134-L153)

```c
ssize_t write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    size_t done = 0;

    while (done < n) {
        ssize_t w = write(fd, p + done, n - done);

        if (w >= 0) {
            done += (size_t)w;
        } else if (errno == EINTR) {
            LOG_DEBUG(DBG_FLOW, "write_all: interrupted by a signal, retrying");
        } else {
            /* ... */
            return -1;
        }
    }
    return (ssize_t)done;
}
```

- 一次 `write()` 不一定寫得完（short write），要寫到滿 n bytes 為止。
- 被 signal 中斷（`EINTR`）時重試；其他錯誤回傳 -1 並保留 `errno`，讓呼叫者判斷是不是 `EPIPE`。
- server 和 client 都透過 `libutils.so` 使用這個函式。

## 9. 雙層 Debug 巨集

**位置：** [`utils.h`](../utils.h#L60-L77)

```c
#ifdef ENABLE_DEBUG
#define LOG_DEBUG_ENABLED 1
#define LOG_DEBUG(lvl, ...)                                                 \
    do {                                                                    \
        if (g_runtime_debug_level >= (lvl))                                 \
            log_write(LOG_LVL_DEBUG, __FILE__, __LINE__, __VA_ARGS__);      \
    } while (0)
#define LOG_HEXDUMP(label, data, len)                                       \
    do {                                                                    \
        if (g_runtime_debug_level >= DBG_BYTES)                             \
            log_hexdump((label), (data), (len));                            \
    } while (0)
#else
/* Compiled out: no code, no format strings, no run-time level check. */
#define LOG_DEBUG_ENABLED 0
#define LOG_DEBUG(lvl, ...)           ((void)0)
#define LOG_HEXDUMP(label, data, len) ((void)0)
#endif
```

- 編譯期：沒有 `-DENABLE_DEBUG` 時巨集展開成 `((void)0)`，程式碼和字串都不會進入執行檔。實測 release 版的 `write_all()` 裡沒有任何 logger 呼叫。
- 執行期：`-d 0/1/2` 設定 `g_runtime_debug_level`，1 印流程，2 再印 hex dump。
- `__FILE__` 和 `__LINE__` 讓每一行 debug 訊息都能對應到原始碼位置。
- `do { ... } while (0)` 讓巨集在 `if` / `else` 裡也能安全使用。
