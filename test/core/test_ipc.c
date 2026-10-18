/* Control socket: framing, limits, and a client that never sends EOF. The
 * server runs in this process's loop; a forked child is the client and
 * exits non-zero on the first failed check; its exit ends the loop. */
#include <assert.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ipc.h"
#include "loop.h"
#include "util.h"

static char sock_path[108];
static struct loop *loop;
static int client_status = -1; /* -1 until the client exits */

/* Echo the arguments joined by '|'; "fail" returns an error. */
static int handler(int argc, char **argv, struct reply *r, void *data) {
  for (int i = 0; i < argc; ++i)
    reply_printf(r, "%s%s", i ? "|" : "", argv[i]);
  return strcmp(argv[0], "fail") == 0;
}

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
      _exit(1);                                                                \
    }                                                                          \
  } while (0)

static int raw_connect(void) {
  struct sockaddr_un a = {.sun_family = AF_UNIX};
  snprintf(a.sun_path, sizeof(a.sun_path), "%s", sock_path);
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  CHECK(fd >= 0 && connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0);
  return fd;
}

/* Send len bytes raw, shut down, and read the whole reply (static buffer). */
static const char *raw(const char *buf, size_t len) {
  static char out[16384];
  int fd = raw_connect();
  size_t off = 0;
  while (off < len) {
    ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
    if (n <= 0)
      break; /* the daemon may close early (too long) */
    off += n;
  }
  shutdown(fd, SHUT_WR);
  size_t got = 0;
  ssize_t n;
  while (got < sizeof(out) - 1 &&
         (n = read(fd, out + got, sizeof(out) - 1 - got)) > 0)
    got += n;
  out[got] = '\0';
  close(fd);
  return out;
}

static void client(void) {
  struct reply r = {0};
  bool ok = false;

  /* spaces, unicode and an empty argument survive */
  char *a1[] = {"echo", "a b", "\xc3\xbc\xe2\x9c\x93", ""};
  CHECK(ipc_client_send(sock_path, 4, a1, &r, &ok) == 0);
  CHECK(ok && strcmp(r.buf, "echo|a b|\xc3\xbc\xe2\x9c\x93|") == 0);
  reply_free(&r);

  char *a2[] = {"fail", "x"};
  CHECK(ipc_client_send(sock_path, 2, a2, &r, &ok) == 0);
  CHECK(!ok && strcmp(r.buf, "fail|x") == 0);
  reply_free(&r);

  /* empty request; missing final NUL */
  CHECK(strcmp(raw("", 0), "error\nempty request\n") == 0);
  CHECK(strcmp(raw("echo\0abc", 8), "ok\necho|abc") == 0);

  /* exactly 8 KiB is accepted, one byte more is refused */
  static char big[8193];
  memset(big, 'x', sizeof(big));
  memcpy(big, "echo", 5);
  const char *rep = raw(big, 8192);
  CHECK(strncmp(rep, "ok\necho|xxx", 11) == 0 && strlen(rep) == 3 + 5 + 8187);
  CHECK(strcmp(raw(big, 8193), "error\nrequest too long\n") == 0);

  /* a client that never sends EOF doesn't block anyone else */
  int idle = raw_connect();
  CHECK(write(idle, "echo\0half", 9) == 9);
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  char *a3[] = {"echo", "after-idle"};
  CHECK(ipc_client_send(sock_path, 2, a3, &r, &ok) == 0);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  CHECK(ok && strcmp(r.buf, "echo|after-idle") == 0);
  CHECK(t1.tv_sec - t0.tv_sec < 2);
  reply_free(&r);
  /* ... and is answered once it finally does (header and body may come
   * in separate reads) */
  shutdown(idle, SHUT_WR);
  char buf[64] = {0};
  size_t got = 0;
  ssize_t n;
  while ((n = read(idle, buf + got, sizeof(buf) - 1 - got)) > 0)
    got += n;
  CHECK(strcmp(buf, "ok\necho|half") == 0);
  close(idle);

  /* many concurrent connections, answered in any order */
  int fds[100];
  for (int i = 0; i < 100; ++i) {
    fds[i] = raw_connect();
    char req[32];
    int n = snprintf(req, sizeof(req), "echo%c%d", '\0', i);
    CHECK(write(fds[i], req, n + 1) == n + 1);
    shutdown(fds[i], SHUT_WR);
  }
  for (int i = 99; i >= 0; --i) {
    char want[32], got[32] = {0};
    snprintf(want, sizeof(want), "ok\necho|%d", i);
    size_t off = 0;
    ssize_t n;
    while ((n = read(fds[i], got + off, sizeof(got) - 1 - off)) > 0)
      off += n;
    CHECK(strcmp(got, want) == 0);
    close(fds[i]);
  }

  /* nothing listening */
  CHECK(ipc_client_send("/nonexistent/swell.sock", 1, a3, &r, &ok) == -1);
  _exit(0);
}

static void on_client_exit(pid_t pid, int status, void *data) {
  client_status = status;
  loop_quit(loop);
}

static void on_guard(struct loop_timer *t, void *data) {
  fprintf(stderr, "timed out\n");
  loop_quit(loop);
}

int main(void) {
  snprintf(sock_path, sizeof(sock_path), "/tmp/swell-test-ipc-%d.sock",
           (int)getpid());
  loop = loop_create();
  assert(ipc_server_start(loop, sock_path, false, NULL, handler, NULL));
  /* a second server on the same path refuses without --replace */
  assert(!ipc_server_start(loop, sock_path, false, NULL, handler, NULL));

  struct loop_timer *guard =
      loop_add_timer(loop, CLOCK_MONOTONIC, on_guard, NULL);
  loop_timer_arm(guard, 10000, 0);

  pid_t pid = fork();
  if (pid == 0)
    client();
  assert(loop_watch_child(loop, pid, on_client_exit, NULL));
  loop_run(loop);
  ipc_server_stop();
  assert(access(sock_path, F_OK) != 0); /* unlinked */

  loop_remove_timer(guard);
  loop_destroy(loop);
  if (client_status < 0) { /* timed out: the client is still running */
    kill(pid, SIGKILL);
    return 1;
  }
  return WIFEXITED(client_status) && WEXITSTATUS(client_status) == 0 ? 0 : 1;
}
