/*
 * testlib.h - helpers shared by the C test programs in tests/.
 *
 * Every test starts real processes with fork() + exec() and never goes through
 * a shell. The helpers cover process control, text filtering with <regex.h>,
 * and reading process and socket state from /proc.
 */
#ifndef TESTLIB_H
#define TESTLIB_H

#ifndef __linux__
#error "the test programs need Linux (for example WSL)"
#endif

#include <stddef.h>
#include <sys/types.h>

/* An argv or flag list built in place: CMD("ps", "-o", "pid=") */
#define CMD(...) ((const char *const[]){ __VA_ARGS__, NULL })
#define NO_FLAGS ((const char *const[]){ NULL })

#define TL_PRINTF(i, j) __attribute__((format(printf, i, j)))

extern int tl_fails;
extern int tl_port;
extern char tl_port_str[16];
extern pid_t tl_server_pid;

/* Program setup: go to the project root, read PORT, optionally copy output to a log. */
void tl_init(int default_port);
void tl_set_port(int port);
void tl_tee(const char *path);
int tl_finish(int rc);
int tl_env_int(const char *name, int fallback);
void tl_die(const char *fmt, ...) TL_PRINTF(1, 2);
char *tl_fmt(const char *fmt, ...) TL_PRINTF(1, 2);
const char *tl_self_path(void);

/* Report format */
void tl_heading(const char *fmt, ...) TL_PRINTF(1, 2);  /* ==== title ==== */
void tl_section(const char *fmt, ...) TL_PRINTF(1, 2);  /* --- title ---   */
void tl_banner(const char *fmt, ...) TL_PRINTF(1, 2);   /* title between two rules */
void tl_expect(int ok, const char *fmt, ...) TL_PRINTF(2, 3);
int tl_print_result(void);

/* Processes. `out` is a file for stdout + stderr, or NULL to inherit them. */
pid_t tl_spawn_opts(const char *dir, const char *out, int append, const char *const argv[]);
pid_t tl_spawn(const char *out, const char *const argv[]);
int tl_wait(pid_t pid);
int tl_run(const char *out, const char *const argv[]);
int tl_which(const char *name);
void tl_sleep(double seconds);
long long tl_now_ms(void);
char *tl_capture(const char *const argv[]);         /* stdout + stderr */
char *tl_capture_out(const char *const argv[]);     /* stdout only */

/* Server under test */
int tl_start_server(const char *dir, const char *log, const char *const flags[]);
void tl_stop_server(void);

/* /proc */
int tl_port_in_use(int port);
int tl_established(int port);
int tl_children(pid_t ppid, pid_t *out, int max, int *zombies);
int tl_child_count(void);
int tl_zombie_count(void);

/* Text. Returned strings live until the program exits. */
char *tl_read(const char *path);
void tl_write(const char *path, const char *text, int append);
char *tl_grep(const char *text, const char *ere, int invert);
int tl_count(const char *text, const char *ere);
int tl_file_count(const char *path, const char *ere);
char *tl_match(const char *text, const char *ere);
int tl_nlines(const char *text);
char *tl_head(const char *text, int n);
char *tl_tail(const char *text, int n);
void tl_print_lines(const char *text, const char *prefix, int cut);
void tl_print_file(const char *path);
void tl_append_prefixed(const char *src, const char *prefix, const char *dst);
int tl_distinct_pids(const char *text, pid_t *out, int max);
char *tl_join(const char *const items[]);

/* Environment */
char *tl_date(void);
char *tl_kernel(void);
char *tl_compiler(void);
char *tl_mktemp(const char *prefix);
void tl_rmrf(const char *path);

#endif /* TESTLIB_H */
