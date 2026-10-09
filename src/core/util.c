#include "util.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

void *xcalloc(size_t n, size_t size) {
  void *p = calloc(n ? n : 1, size ? size : 1);
  if (!p)
    die("out of memory");
  return p;
}

void *xrealloc(void *ptr, size_t size) {
  void *p = realloc(ptr, size ? size : 1);
  if (!p)
    die("out of memory");
  return p;
}

char *xstrdup(const char *s) { return xstrndup(s, s ? strlen(s) : 0); }

char *xstrndup(const char *s, size_t n) {
  if (!s)
    return NULL;
  char *p = strndup(s, n);
  if (!p)
    die("out of memory");
  return p;
}

char *xasprintf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *s = NULL;
  if (vasprintf(&s, fmt, ap) < 0)
    die("out of memory");
  va_end(ap);
  return s;
}

int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void vwarn_prefixed(const char *prefix, const char *fmt, va_list ap) {
  if (prefix)
    fprintf(stderr, "swell[%s]: ", prefix);
  else
    fputs("swell: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
}

void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vwarn_prefixed(NULL, fmt, ap);
  va_end(ap);
  exit(1);
}

void warn(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vwarn_prefixed(NULL, fmt, ap);
  va_end(ap);
}

const char *runtime_dir(void) {
  const char *dir = getenv("XDG_RUNTIME_DIR");
  return dir && *dir ? dir : "/tmp";
}

static void exec_shell(const char *cmd, char *const extra_env[]) {
  /* The daemon blocks some signals for its signalfd; children must not
   * inherit that. */
  sigset_t none;
  sigemptyset(&none);
  sigprocmask(SIG_SETMASK, &none, NULL);
  signal(SIGPIPE, SIG_DFL);
  for (int i = 0; extra_env && extra_env[i]; ++i)
    putenv(extra_env[i]);
  execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
  _exit(127);
}

pid_t spawn_shell(const char *cmd, char *const extra_env[]) {
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    exec_shell(cmd, extra_env);
  }
  if (pid < 0)
    warn("fork: cannot run '%s'", cmd);
  return pid;
}

void spawn_shell_detached(const char *cmd, char *const extra_env[]) {
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    if (fork() == 0)
      exec_shell(cmd, extra_env);
    _exit(0);
  } else if (pid > 0) {
    waitpid(pid, NULL, 0);
  } else {
    warn("fork: cannot run '%s'", cmd);
  }
}
