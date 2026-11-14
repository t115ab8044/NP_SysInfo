# Description of Key Source Code

[← Back to README](../README.md) · [System Design](system-design.md) · [Execution Screenshots](screenshots.md)

All the snippets below come from the source code in this repo; `/* ... */` marks parts left out because they do not matter for the point being made. Follow the **Location** links to see the full code.

## 1. Parent main loop: it only accepts connections and forks

**Location:** [`main()` in `server.c`](../server.c#L634-L677)

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

- Each connection gets its own `fork()`ed child; the project uses no threads anywhere.
- After `fork()` both sides hold the connection's fd: the parent must `close(cfd)`, and the child closes the listening socket.
- `set_signal()` does not set `SA_RESTART`, so SIGCHLD makes `accept()` return `EINTR`. The parent goes back to the top of the loop and first writes the children it just reaped to the log.
- When the connection limit (`-m`, default 32) is reached, the connection is rejected directly, without a fork (Section 8).
- After `Ctrl+C` the loop ends, and `shut_down()` waits for the children to finish (mechanism [5b], Section 7).

## 2. SIGCHLD handler: reaping children (mechanism [1])

**Location:** [`on_sigchld()` in `server.c`](../server.c#L112-L129); the log is written by [`log_reaped()` and `report_reaped_children()`](../server.c#L143-L162)

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

- Signals do not queue: if another child exits before SIGCHLD is handled, it only sets the same pending bit again. One SIGCHLD can stand for several children, so `waitpid` has to be a loop that keeps reaping until none are left.
- With `--reap-once` the loop `break`s after reaping one child, which is the common mistake of "one `waitpid` call per signal". When 12 children exit at once, only 2 are reaped, the rest become `<defunct>`, and `g_active` stays at 10.
- `WNOHANG` keeps the handler from blocking.
- `printf()` cannot be called in a signal handler, so the handler only stores the PID and status in a ring buffer; the main loop decodes them with `WIFEXITED`/`WIFSIGNALED` and writes the log.
- The handler can interrupt any code, so it saves and restores `errno`.

## 3. Child setup (mechanisms [5a], [5b], [2b])

**Location:** [`run_child()` in `server.c`](../server.c#L378-L410)

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

- **[5a]** When the parent gets `kill -9` it cannot tell anyone, so the kernel takes over: `PR_SET_PDEATHSIG` makes the child receive `SIGTERM` as soon as the parent dies. After setting it, the child checks `getppid()`: if the parent died between `fork()` and `prctl()`, the child has already been re-parented and the setting will never fire, so the child must exit on its own.
- **[5b]** The SIGINT from `Ctrl+C` goes to the whole foreground process group, so the child receives it too. The child ignores it and lets the parent decide when to stop serving. `fork()` copies the parent's handler, which is meant for the parent, so the child sets SIGINT explicitly here.
- **[2b]** `TCP_USER_TIMEOUT` is 3000 ms: when sent data has gone more than 3 seconds without an ACK, the kernel gives up the connection.
- The exit code passed to `_exit(rc)` gives the reason the child ended, and the parent writes it to the log when it reaps the child.

## 4. Write failures: EPIPE and ETIMEDOUT (mechanisms [2a], [2b])

**Location:** [`main()` in `server.c`](../server.c#L609-L611), [`send_snapshots()`](../server.c#L290-L327)

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

- **[2a]** By default, a `write()` to a closed connection gets SIGPIPE, which kills the process. Once SIGPIPE is ignored, `write()` returns -1 with errno set to `EPIPE` instead. Signal dispositions are inherited through `fork()`, so setting it once in the parent is enough. After the client disconnects, the first write only makes the other side answer with an RST; the second write is the one that gets `EPIPE`. `WATCH` writes at regular intervals, so it reproduces this reliably.
- **[2b]** If the client's network suddenly vanishes, nothing at all comes back from the other side: no RST and no FIN. `write()` only puts the data into the send buffer, so it keeps succeeding. With `TCP_USER_TIMEOUT` set, the kernel gives up the connection and the next `write()` returns `ETIMEDOUT`.

## 5. Request length limit and request deadline (mechanisms [3], [4])

**Location:** [`read_line_timeout()` in `utils.c`](../utils.c#L164-L210), its caller [`read_request()`](../server.c#L250-L287), and the control version [`read_line_unlimited()`](../server.c#L182-L247)

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

- **[3]** The buffer has a fixed size (`MAX_LINE` 128). If 127 bytes have been read without a `\n`, the function returns `READ_TOO_LONG`, and the server replies `ERROR request too long` and exits. The control version `read_line_unlimited()` doubles its buffer each time until it sees a `\n`; the 16 MiB request with no newline makes it grow to 32 MiB.
- **[4]** The deadline is for "the whole line", not for "each read": the cut-off time is computed when reading starts, and after that each byte waits only for the time that is left. Slowloris sends 1 byte every 0.5 seconds, so each `poll` gets data quickly, but once the 3 seconds are up the function returns `READ_TIMEOUT`, and the server replies `ERROR request timeout` and exits with code 4.
- It reads 1 byte at a time, so the data after `\n` stays in the socket and is not read by mistake.
- `read_line()` is just `read_line_timeout(..., -1)`; the client uses it, with no deadline, to read replies.

## 6. Parsing the request (mechanism [3])

**Location:** [`serve_client()` in `server.c`](../server.c#L336-L376)

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

- `%15s` limits the length of the command, so it cannot write past the end of `cmd`.
- `%3d` and `%5d` read at most 3 and 5 digits, exactly enough for the limits 100 and 10000. Without the widths, `WATCH 4294967297 100` overflows the `int`; glibc keeps only the low 32 bits, so it becomes `WATCH 1 100` and is accepted (a problem found during testing).
- One extra `%1s` is read: if anything is left after the numbers, `fields` is 4 and the request is rejected. When a number is longer than its width, `%1s` also reads the remaining digits, so that request is rejected too.
- `count` and `interval_ms` both have lower and upper limits: `WATCH 100 0` (100 lines pouring out at once) and `WATCH 100000 1000` (holding a child for 27.8 hours) are both rejected.
- `--no-validate` keeps the version with no width and no range checks, as the Without control.

## 7. Graceful shutdown on Ctrl+C (mechanism [5b])

**Location:** [`on_sigint()` in `server.c`](../server.c#L436-L443), [`shut_down()`](../server.c#L457-L485)

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

- On the first `Ctrl+C`, the handler calls `close()` on the listening socket. `close()` is async-signal-safe; even if the signal lands just after the main loop checks `g_interrupts` and before it calls `accept()`, `accept()` fails at once because the fd is already closed, so it cannot block. New clients get connection refused immediately.
- The parent then waits with a blocking `waitpid()` for the children to end one by one; `ECHILD` means they have all finished, and the parent exits with code 0.
- The parent is still alive while it waits, so [5a]'s pdeathsig does not fire. After more than 30 seconds (`alarm`), or on a second `Ctrl+C` (`g_interrupts > 1`), `waitpid()` is interrupted and the parent exits at once; pdeathsig takes care of the remaining children.

## 8. Connection limit: the parent rejects directly

**Location:** [`reject_busy()` in `server.c`](../server.c#L419-L427)

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

- `g_active` is the number of children not yet reaped: it goes up by 1 after a fork and down by 1 when a child is reaped. The reap records are processed once before the check, so the number is not too high.
- This is the only place where the parent writes to a client, so it is extra careful: `MSG_DONTWAIT` keeps it from blocking, and `MSG_NOSIGNAL` keeps SIGPIPE from killing it.
- The limit can only cap the number of children; it cannot stop Slowloris, because 32 slow clients can still fill every slot. That is why [4]'s request deadline is also needed.

## 9. `write_all`: writing all the data

**Location:** [`write_all()` in `utils.c`](../utils.c#L135-L154)

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

- One `write()` may not write everything (a short write), so the function keeps writing until all n bytes are written.
- It retries when a signal interrupts it (`EINTR`); for any other error it returns -1 and keeps `errno`, so the caller can tell `EPIPE` from `ETIMEDOUT`.
- The server and the client both use this function through `libutils.so`.

## 10. Two-level debug macros

**Location:** [`utils.h`](../utils.h#L62-L79)

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

- Compile time: without `-DENABLE_DEBUG` the macros expand to `((void)0)`, so neither the code nor the strings get into the executable. The measurements show no logger calls at all inside the release build's `write_all()`.
- Runtime: `-d 0/1/2` sets `g_runtime_debug_level`; 1 prints the flow, and 2 also prints hex dumps.
- `__FILE__` and `__LINE__` let every debug line be traced back to its place in the source.
- `do { ... } while (0)` lets the macro be used safely inside `if` / `else` too.
