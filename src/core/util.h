/* Small helpers shared by every part of swell. */
#ifndef SWELL_UTIL_H
#define SWELL_UTIL_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

void *xcalloc(size_t n, size_t size);
void *xrealloc(void *ptr, size_t size);
char *xstrdup(const char *s);            /* NULL in, NULL out */
char *xstrndup(const char *s, size_t n); /* NULL in, NULL out */
char *xasprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Copy src into dst[n], truncating; dst is always terminated. */
void copy_str(char *dst, size_t n, const char *src);

/* CLOCK_MONOTONIC milliseconds. */
int64_t now_ms(void);
/* The same clock in fractional milliseconds, for tweens. */
double now_fms(void);

/* Messages go to stderr prefixed with "swell: " (or "swell[module]: "). */
void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
void warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void vwarn_prefixed(const char *prefix, const char *fmt, va_list ap);

/* Runtime directory for sockets: $XDG_RUNTIME_DIR or /tmp. */
const char *runtime_dir(void);

/* In a forked child, before exec: the daemon blocks signals for its
 * signalfd and ignores SIGPIPE; undo both so the child doesn't inherit
 * them. */
void child_reset_signals(void);
/* child_reset_signals, then stdout to out_fd (or /dev/null if out_fd < 0)
 * and stdin and stderr to /dev/null. */
void child_setup(int out_fd);

/* Run `sh -c cmd` as a child of ours (not detached). extra_env is a
 * NULL-terminated list of "KEY=VALUE" strings added to the environment, or
 * NULL. Returns the pid, or -1. */
pid_t spawn_shell(const char *cmd, char *const extra_env[]);
/* Run `sh -c cmd` fully detached (double fork, new session): it outlives us
 * and never becomes our zombie. */
void spawn_shell_detached(const char *cmd, char *const extra_env[]);

#endif
