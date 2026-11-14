# Description of Key Source Code

[← Back to README](../README.md) · [System Design](system-design.md) · [Execution Screenshots](screenshots.md) · [Auditing Discussion](audit.md)

All the snippets below come from the source code in this repo; `/* ... */` marks parts left out because they are not relevant to the point. Follow the "Location" links to see the full code.

## 1. Parent main loop: it only accepts connections and forks

**Location:** [`main()` in `server.c`](../server.c#L458-L496)

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

- Every connection gets its own `fork()`ed child; the project uses no threads anywhere.
- After `fork()` both sides hold the connection's fd: the parent must `close(cfd)`, and the child closes the listening socket.
- `set_signal()` does not set `SA_RESTART`, so SIGCHLD makes `accept()` return `EINTR`. The parent goes back to the top of the loop and first writes the children it just reaped to the log.
- When the connection limit is reached, the client is rejected directly, without a fork (mechanism [4]).

## 2. SIGCHLD handler: reaping children (mechanism [1])

**Location:** [`on_sigchld()` in `server.c`](../server.c#L78-L93); the log is written by [`report_reaped_children()`](../server.c#L106-L122)

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

- Signals do not queue, so one SIGCHLD may stand for several children that ended. `waitpid` must therefore run in a loop until there is nothing left to reap.
- `WNOHANG` keeps the handler from blocking.
- `printf()` must not be called in a signal handler, so the handler only stores the PID and status in a ring buffer; the main loop decodes them with `WIFEXITED`/`WIFSIGNALED` and writes the log.
- The handler can interrupt any code, so it saves and restores `errno`.
- With `--no-sigchld` no handler is registered, and every finished child becomes `<defunct>`.

## 3. Child setup: exiting along with the parent (mechanism [5])

**Location:** [`run_child()` in `server.c`](../server.c#L287-L304)

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

- A parent that gets `kill -9` cannot tell anyone, so the kernel takes over: with `PR_SET_PDEATHSIG`, the child receives `SIGTERM` as soon as the parent dies.
- `getppid()` is checked after the setting: if the parent died between `fork()` and `prctl()`, the child has already been adopted and the setting will never fire, so the child exits on its own.
- The exit code in `_exit(rc)` gives the reason the child ended, and the parent writes it to the log when it reaps the child.

## 4. Ignoring SIGPIPE and handling EPIPE (mechanism [2])

**Location:** [`main()` in `server.c`](../server.c#L436-L440), [`send_snapshots()`](../server.c#L222-L251)

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

- By default, a `write()` on a connection that has been closed raises SIGPIPE, which kills the process at once. Once SIGPIPE is ignored, `write()` returns -1 with errno `EPIPE` instead.
- How a signal is handled is inherited through `fork()`, so setting it once in the parent is enough.
- After the client disconnects, the first write only makes the other side answer with RST, and the second write gets `EPIPE`. `WATCH` writes once per interval, which reproduces this reliably.

## 5. Request length limit (mechanism [3])

**Location:** [`read_line()` in `utils.c`](../utils.c#L155-L181), and the control version [`read_line_unlimited()`](../server.c#L140-L187)

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

- The buffer has a fixed size (`MAX_LINE` 128). If 127 bytes have been read and there is still no `\n`, the function returns `READ_TOO_LONG`, and the server replies `ERROR request too long` and ends.
- It reads 1 byte at a time, so data after the `\n` stays in the socket and is never read by mistake.
- The control version `read_line_unlimited()` doubles its buffer each time until it sees `\n`. A 16 MiB request with no newline grows it to 32 MiB.

## 6. Connection limit: the parent rejects directly (mechanism [4])

**Location:** [`reject_busy()` in `server.c`](../server.c#L313-L321)

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

- `g_active` is the number of children not reaped yet: it goes up by 1 after a fork and down by 1 when a child is reaped. The reap records are processed once before the check, so the number does not run high.
- This is the only place where the parent writes to a client, so it takes extra care: `MSG_DONTWAIT` keeps it from blocking, and `MSG_NOSIGNAL` keeps it from being killed by SIGPIPE.
- A rejected connection never gets a child.

## 7. Parsing the request

**Location:** [`serve_client()` in `server.c`](../server.c#L253-L285)

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

- `%15s` limits the length of the command, so it cannot write past the end of `cmd`.
- `%3d` and `%5d` read at most 3 and 5 digits, just enough for the upper limits 100 and 10000. Without the widths, `WATCH 4294967297 100` overflows `int` and is accepted as `WATCH 1 100` (found during the audit; see the [auditing discussion](audit.md)).
- One extra `%1s` is read: if there are leftover words, `fields` is 4 and the request is rejected. When a number is longer than its width, the remaining digits are also read by `%1s`, so that request is rejected as well.
- `count` and `interval_ms` both have lower and upper limits, so a client cannot ask for unlimited or overly frequent reports.

## 8. `write_all`: writing all the data

**Location:** [`write_all()` in `utils.c`](../utils.c#L134-L153)

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

- One `write()` may not write everything (a short write), so it keeps writing until all n bytes are written.
- When interrupted by a signal (`EINTR`) it retries; on any other error it returns -1 and keeps `errno`, so the caller can tell whether it was `EPIPE`.
- The server and the client both use this function through `libutils.so`.

## 9. Two-level debug macros

**Location:** [`utils.h`](../utils.h#L60-L77)

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

- Compile time: without `-DENABLE_DEBUG` the macros expand to `((void)0)`, so neither the code nor the strings reach the executable. Measured: the release build's `write_all()` contains no logger calls at all.
- Runtime: `-d 0/1/2` sets `g_runtime_debug_level`; 1 prints the flow, and 2 also prints hex dumps.
- `__FILE__` and `__LINE__` tie every debug message to its place in the source code.
- `do { ... } while (0)` makes the macro safe to use inside `if` / `else`.
