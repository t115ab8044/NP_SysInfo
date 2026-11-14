# 關鍵原始碼說明（Description of Key Source Code）

[← 回到 README](../README.md) ・ [系統設計說明](system-design.md) ・ [執行截圖](screenshots.md)

以下片段都來自本 repo 的原始碼，`/* ... */` 表示省略與重點無關的部分。點擊「位置」的連結可以看到完整程式。

## 1. Parent 主迴圈：只負責接連線與 fork

**位置：** [`server.c` 的 `main()`](../server.c#L634-L677)

```c
for (;;) {
    /* ... */
    report_reaped_children();
    if (g_interrupts)
        break;                  /* [5b] Ctrl+C: stop accepting */
    cfd = accept(lfd, (struct sockaddr *)&addr, &addr_len);
    if (cfd < 0) {
        /* EINTR: a signal arrived (SIGCHLD to log, or Ctrl+C).
         * EBADF: Ctrl+C has closed the listening socket. */
        if (errno != EINTR && !g_interrupts)
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
return shut_down();
```

- 每條連線都 `fork()` 一個 child，整個專案沒有使用 thread。
- `fork()` 之後兩邊都持有連線的 fd：parent 要 `close(cfd)`，child 則關閉 listening socket。
- `set_signal()` 沒有設 `SA_RESTART`，所以 SIGCHLD 會讓 `accept()` 回傳 `EINTR`。parent 回到迴圈開頭，先把剛回收的 child 寫進 log。
- 達到連線上限（`-m`，預設 32）時直接拒絕，不會 fork（第 8 節）。
- `Ctrl+C` 後離開迴圈，交給 `shut_down()` 等 child 做完（機制 [5b]，第 7 節）。

## 2. SIGCHLD handler：回收 child（機制 [1]）

**位置：** [`server.c` 的 `on_sigchld()`](../server.c#L112-L129)，輸出 log 的 [`log_reaped()` 和 `report_reaped_children()`](../server.c#L143-L162)

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
        if (!g_cfg.reap_loop)
            break;                  /* --reap-once: the mistake being shown */
    }
    errno = saved_errno;
}
```

- signal 不會排隊：SIGCHLD 還沒處理時又有 child 結束，只是把同一個 pending 位元再設一次。一個 SIGCHLD 可能代表好幾個 child，所以 `waitpid` 要寫成迴圈，一直收到沒有為止。
- `--reap-once` 時迴圈收一個就 `break`，也就是常見的錯誤寫法「每個 signal 呼叫一次 `waitpid`」。12 個 child 同時結束時，只有 2 個被回收，其他都變成 `<defunct>`，`g_active` 也停在 10。
- `WNOHANG` 讓 handler 不會卡住。
- `printf()` 不能在 signal handler 裡呼叫，所以 handler 只把 PID 和 status 存進 ring buffer，由主迴圈用 `WIFEXITED`／`WIFSIGNALED` 解讀後寫 log。
- handler 可能打斷任何程式，所以要保存並還原 `errno`。

## 3. Child 初始化（機制 [5a]、[5b]、[2b]）

**位置：** [`server.c` 的 `run_child()`](../server.c#L378-L410)

```c
static void run_child(int lfd, int cfd, const char *peer, pid_t parent)
{
    int rc;

    if (g_cfg.pdeathsig) {                       /* [5a] */
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent)    /* the parent died before prctl() */
            _exit(CHILD_IO_ERROR);
    }
    if (g_cfg.graceful)                          /* [5b] */
        set_signal(SIGINT, SIG_IGN);
    close(lfd);                     /* only the parent accepts */
    set_signal(SIGCHLD, SIG_DFL);   /* the child has no children to reap */
    if (g_cfg.user_timeout) {                    /* [2b] */
        unsigned int ms = USER_TIMEOUT_MS;

        if (setsockopt(cfd, IPPROTO_TCP, TCP_USER_TIMEOUT, &ms, sizeof(ms)) < 0)
            LOG_WARN("%s: TCP_USER_TIMEOUT not set: %s", peer, strerror(errno));
    }
    rc = serve_client(cfd, peer);
    close(cfd);
    /* ... */
    _exit(rc);
}
```

- **[5a]** parent 被 `kill -9` 時無法通知任何人，所以改由 kernel 負責：`PR_SET_PDEATHSIG` 讓 parent 一死，child 就收到 `SIGTERM`。設定之後再檢查 `getppid()`：parent 如果在 `fork()` 和 `prctl()` 之間就死了，child 已經被過繼，設定不會生效，要自己結束。
- **[5b]** `Ctrl+C` 的 SIGINT 會送給整個前景 process group，child 也收得到。child 忽略它，由 parent 決定什麼時候停止服務。`fork()` 會複製 parent 的 handler，那是給 parent 用的，所以這裡要明確設定。
- **[2b]** `TCP_USER_TIMEOUT` 是 3000 ms：送出的資料超過 3 秒沒有 ACK，kernel 就放棄這條連線。
- `_exit(rc)` 的 exit code 代表結束原因，parent 回收時會寫進 log。

## 4. 寫入失敗：EPIPE 與 ETIMEDOUT（機制 [2a]、[2b]）

**位置：** [`server.c` 的 `main()`](../server.c#L609-L611)，[`send_snapshots()`](../server.c#L290-L327)

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
        if (errno == ETIMEDOUT) {
            LOG_WARN("%s: client unreachable, no ACK for %d ms (%s) -> stop before "
                     "snapshot %d/%d", peer, USER_TIMEOUT_MS, strerror(errno), seq, count);
            return CHILD_PEER_GONE;
        }
        /* ... */
    }
    if (seq < count)
        sleep_ms((unsigned)interval_ms);
}
```

- **[2a]** 對已經關閉的連線 `write()`，預設會收到 SIGPIPE 直接被殺掉。忽略之後，`write()` 改成回傳 -1、errno 為 `EPIPE`。signal 的處理方式會透過 `fork()` 繼承，所以只要在 parent 設定一次。client 斷線後，第一次寫入只會讓對方回 RST，第二次寫入才收到 `EPIPE`。`WATCH` 每隔一段時間寫一次，正好能穩定重現。
- **[2b]** client 的網路如果突然消失，對方什麼都不會送回來，沒有 RST，也沒有 FIN。`write()` 只是把資料放進送出緩衝區，所以一直成功。設定 `TCP_USER_TIMEOUT` 後，kernel 放棄連線，下一次 `write()` 回傳 `ETIMEDOUT`。

## 5. 請求長度上限與請求期限（機制 [3]、[4]）

**位置：** [`utils.c` 的 `read_line_timeout()`](../utils.c#L164-L210)，呼叫端 [`read_request()`](../server.c#L250-L287)，對照組 [`read_line_unlimited()`](../server.c#L182-L247)

```c
ssize_t read_line_timeout(int fd, char *buf, size_t cap, int timeout_ms)
{
    long long deadline = timeout_ms >= 0 ? monotonic_ms() + timeout_ms : 0;
    size_t len = 0;

    while (len < cap - 1) {
        char c;
        ssize_t r;

        if (timeout_ms >= 0) {
            /* [4] Wait only for the time left until the deadline, so a
             * client sending one byte at a time cannot stretch it. */
            struct pollfd pfd = { .fd = fd, .events = POLLIN };
            long long left = deadline - monotonic_ms();
            int ready = left > 0 ? poll(&pfd, 1, (int)left) : 0;

            if (ready == 0) {
                buf[len] = '\0';
                return READ_TIMEOUT;
            }
            /* ... */
        }
        r = read(fd, &c, 1);
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

- **[3]** 緩衝區大小固定（`MAX_LINE` 128），讀滿 127 bytes 還沒看到 `\n` 就回傳 `READ_TOO_LONG`，server 回 `ERROR request too long` 後結束。對照組 `read_line_unlimited()` 每次把緩衝區加倍，直到看到 `\n`。16 MiB 不換行的請求讓它長到 32 MiB。
- **[4]** 期限是「整行」的，不是「每次 read」的：開始讀的時候算出截止時間，之後每個 byte 都只等剩下的時間。Slowloris 每 0.5 秒送 1 byte，每次 `poll` 都很快就有資料，但 3 秒一到就回傳 `READ_TIMEOUT`，server 回 `ERROR request timeout`，以 exit 4 結束。
- 一次讀 1 byte，`\n` 之後的資料留在 socket 裡，不會被多讀走。
- `read_line()` 就是 `read_line_timeout(..., -1)`，client 讀回覆時用它，沒有期限。

## 6. 解析請求（機制 [3]）

**位置：** [`server.c` 的 `serve_client()`](../server.c#L336-L376)

```c
if (strcmp(line, "SYSINFO") == 0) {
    count = 1;
    interval_ms = 0;
} else if (g_cfg.validate) {
    int fields = sscanf(line, "%15s %3d %5d %1s", cmd, &count, &interval_ms, extra);

    if (fields < 1 || strcmp(cmd, "WATCH") != 0)
        return reject_unknown(cfd, peer, line);
    if (fields != 3 || count < 1 || count > WATCH_MAX_COUNT ||
        interval_ms < WATCH_MIN_MS || interval_ms > WATCH_MAX_MS) {
        LOG_WARN("%s: bad WATCH arguments \"%s\" -> rejected", peer, line);
        send_error(cfd, "bad arguments: WATCH <count 1-100> <ms 100-10000>");
        return CHILD_BAD_REQUEST;
    }
} else {
    /* --no-validate: plain %d, no width and no range checks */
    if (sscanf(line, "%15s %d %d", cmd, &count, &interval_ms) != 3 ||
        strcmp(cmd, "WATCH") != 0)
        return reject_unknown(cfd, peer, line);
}
```

- `%15s` 限制指令長度，避免寫出 `cmd` 的範圍。
- `%3d`、`%5d` 限制數字最多讀 3 位和 5 位，剛好容納上限 100 和 10000。沒有寬度時，`WATCH 4294967297 100` 會讓 `int` 溢位，glibc 只留下低 32 位元，變成 `WATCH 1 100` 被接受（測試時發現的問題）。
- 多讀一個 `%1s`：如果後面還有多餘的字，`fields` 會是 4，請求就被拒絕。數字超過寬度時，剩下的位數也會被 `%1s` 讀到，一樣被拒絕。
- `count` 和 `interval_ms` 都有上下限：`WATCH 100 0`（100 行一次湧出）和 `WATCH 100000 1000`（佔住 child 27.8 小時）都被拒絕。
- `--no-validate` 保留了沒有寬度、沒有範圍檢查的寫法，作為 Without 的對照組。

## 7. Ctrl+C 優雅關閉（機制 [5b]）

**位置：** [`server.c` 的 `on_sigint()`](../server.c#L436-L443)，[`shut_down()`](../server.c#L457-L485)

```c
static void on_sigint(int sig)
{
    (void)sig;
    if (g_interrupts++ == 0 && g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
}
```

```c
static int shut_down(void)
{
    /* ... */
    set_signal(SIGCHLD, SIG_DFL);   /* the waitpid() below reaps from now on */
    report_reaped_children();
    /* ... */
    set_signal(SIGALRM, on_sigalrm);
    alarm(SHUTDOWN_GRACE_SEC);
    for (;;) {
        pid = waitpid(-1, &status, 0);
        if (pid > 0)
            log_reaped(pid, status);
        else if (errno != EINTR || g_interrupts > 1 || g_grace_over)
            break;
    }
    all_done = errno == ECHILD;
    /* ... */
}
```

- 第一次 `Ctrl+C` 就在 handler 裡 `close()` 監聽 socket。`close()` 是 async-signal-safe；signal 就算剛好落在主迴圈檢查 `g_interrupts` 之後、呼叫 `accept()` 之前，`accept()` 也會因為 fd 已經關閉而立刻失敗，不會卡住。新的 client 則立刻收到 connection refused。
- 之後 parent 用阻塞的 `waitpid()` 等 child 一個一個結束，`ECHILD` 表示全部做完，以 exit 0 結束。
- parent 在等的時候還活著，所以 [5a] 的 pdeathsig 不會觸發。等超過 30 秒（`alarm`），或再按一次 `Ctrl+C`（`g_interrupts > 1`），`waitpid()` 被打斷，parent 直接結束，剩下的 child 由 pdeathsig 收掉。

## 8. 連線上限：parent 直接拒絕

**位置：** [`server.c` 的 `reject_busy()`](../server.c#L419-L427)

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
- 上限只能限制 child 的數量，擋不住 Slowloris：32 個慢速 client 一樣能佔滿名額，所以還需要 [4] 的請求期限。

## 9. `write_all`：寫完全部資料

**位置：** [`utils.c` 的 `write_all()`](../utils.c#L135-L154)

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
- 被 signal 中斷（`EINTR`）時重試；其他錯誤回傳 -1 並保留 `errno`，讓呼叫者判斷是 `EPIPE` 還是 `ETIMEDOUT`。
- server 和 client 都透過 `libutils.so` 使用這個函式。

## 10. 雙層 Debug 巨集

**位置：** [`utils.h`](../utils.h#L62-L79)

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
