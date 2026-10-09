#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/un.h>
#include <unistd.h>

#include "util.h"

#define MAX_REQUEST 8192
#define MAX_ARGS 64
#define MAX_CLIENTS 64
#define CLIENT_TIMEOUT_MS 2000
#define CLIENT_IO_TIMEOUT_S 10

void reply_printf(struct reply *r, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0)
    return;
  if (r->len + n + 1 > r->cap) {
    r->cap = (r->len + n + 1) * 2;
    r->buf = xrealloc(r->buf, r->cap);
  }
  va_start(ap, fmt);
  vsnprintf(r->buf + r->len, n + 1, fmt, ap);
  va_end(ap);
  r->len += n;
}

void reply_free(struct reply *r) {
  free(r->buf);
  *r = (struct reply){0};
}

const char *ipc_socket_path(void) {
  static char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
  const char *env = getenv("SWELL_SOCK");
  if (env && *env)
    snprintf(path, sizeof(path), "%s", env);
  else
    snprintf(path, sizeof(path), "%s/swell.sock", runtime_dir());
  return path;
}

static bool make_addr(const char *path, struct sockaddr_un *addr) {
  memset(addr, 0, sizeof(*addr));
  addr->sun_family = AF_UNIX;
  if (strlen(path) >= sizeof(addr->sun_path)) {
    warn("socket path too long: %s", path);
    return false;
  }
  strcpy(addr->sun_path, path);
  return true;
}

static int connect_to(const char *path) {
  struct sockaddr_un addr;
  if (!make_addr(path, &addr))
    return -1;
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

/* ---- client ---- */

/* send() with MSG_NOSIGNAL: a daemon that closed early must not kill us
 * with SIGPIPE. */
static bool write_all(int fd, const char *buf, size_t len) {
  while (len > 0) {
    ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    buf += n;
    len -= n;
  }
  return true;
}

int ipc_client_send(const char *path, int argc, char *const argv[],
                    struct reply *out, bool *ok) {
  int fd = connect_to(path);
  if (fd < 0)
    return -1;
  /* never hang on a stopped or wedged daemon */
  struct timeval tv = {CLIENT_IO_TIMEOUT_S, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  /* On a write failure the daemon has usually answered already
   * ("request too long"), so read the reply anyway. */
  for (int i = 0; i < argc; ++i)
    if (!write_all(fd, argv[i], strlen(argv[i]) + 1))
      break;
  shutdown(fd, SHUT_WR);

  struct reply raw = {0};
  char buf[4096];
  ssize_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0 || (n < 0 && errno == EINTR))
    if (n > 0)
      reply_printf(&raw, "%.*s", (int)n, buf);
  bool timed_out = n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
  close(fd);

  *ok = raw.buf && strncmp(raw.buf, "ok\n", 3) == 0;
  const char *text = raw.buf ? strchr(raw.buf, '\n') : NULL;
  if (text)
    reply_printf(out, "%s", text + 1);
  else if (timed_out)
    reply_printf(out, "the daemon did not answer within %d s\n",
                 CLIENT_IO_TIMEOUT_S);
  else if (!raw.buf)
    reply_printf(out, "no reply from the daemon\n");
  reply_free(&raw);
  return 0;
}

/* ---- server ---- */

struct client {
  int fd;
  struct loop_watch *watch;
  char *buf;
  size_t len;
  int64_t deadline;
  struct client *next;
};

static struct {
  int fd;
  int lock_fd;
  char path[108];
  dev_t dev;
  ino_t ino;
  struct loop *loop;
  struct loop_watch *watch;
  struct loop_timer *backoff; /* accept paused after EMFILE */
  struct client *clients;
  int nclients;
  ipc_handler_fn fn;
  void *data;
} srv = {.fd = -1, .lock_fd = -1};

static void client_close(struct client *c) {
  for (struct client **p = &srv.clients; *p; p = &(*p)->next)
    if (*p == c) {
      *p = c->next;
      break;
    }
  --srv.nclients;
  loop_remove_fd(c->watch);
  close(c->fd);
  free(c->buf);
  free(c);
}

static void client_finish(struct client *c) {
  char *argv[MAX_ARGS + 1];
  int argc = 0;
  bool too_many = false;
  for (size_t i = 0; i < c->len;) {
    if (argc == MAX_ARGS) {
      too_many = true;
      break;
    }
    argv[argc++] = c->buf + i;
    i += strlen(c->buf + i) + 1;
  }
  argv[argc] = NULL;

  struct reply r = {0};
  int ret = 1;
  if (too_many)
    reply_printf(&r, "too many arguments (at most %d)\n", MAX_ARGS);
  else if (argc == 0)
    reply_printf(&r, "empty request\n");
  else
    ret = srv.fn(argc, argv, &r, srv.data);
  const char *head = ret == 0 ? "ok\n" : "error\n";
  write_all(c->fd, head, strlen(head));
  if (r.len)
    write_all(c->fd, r.buf, r.len);
  reply_free(&r);
  client_close(c);
}

static void on_client(int fd, short revents, void *data) {
  struct client *c = data;
  char buf[1024];
  ssize_t n = read(fd, buf, sizeof(buf));
  if (n < 0 && (errno == EINTR || errno == EAGAIN))
    return;
  if (n > 0) {
    if (c->len + n > MAX_REQUEST) {
      write_all(fd, "error\nrequest too long\n", 23);
      client_close(c);
      return;
    }
    c->buf = xrealloc(c->buf, c->len + n + 1);
    memcpy(c->buf + c->len, buf, n);
    c->len += n;
    c->buf[c->len] = '\0';
    return;
  }
  if (n == 0) {
    /* make sure the last argument is terminated */
    if (c->len && c->buf[c->len - 1] != '\0') {
      c->buf = xrealloc(c->buf, c->len + 1);
      c->buf[c->len++] = '\0';
    }
    client_finish(c);
    return;
  }
  client_close(c);
}

static void on_backoff(struct loop_timer *t, void *data) {
  loop_watch_set_events(srv.watch, POLLIN);
}

static void on_accept(int fd, short revents, void *data) {
  int cfd = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
  if (cfd < 0) {
    if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS ||
        errno == ENOMEM) {
      /* The listening socket stays readable; stop polling it for a moment
       * instead of spinning. */
      loop_watch_set_events(srv.watch, 0);
      loop_timer_arm(srv.backoff, 100, 0);
    }
    return;
  }
  if (srv.nclients >= MAX_CLIENTS) {
    write_all(cfd, "error\ntoo many clients\n", 24);
    close(cfd);
    return;
  }
  struct client *c = xcalloc(1, sizeof(*c));
  c->fd = cfd;
  c->deadline = now_ms() + CLIENT_TIMEOUT_MS;
  c->watch = loop_add_fd(srv.loop, cfd, POLLIN, on_client, c);
  c->next = srv.clients;
  srv.clients = c;
  ++srv.nclients;
}

/* Drop clients that never finished their request. */
static int expire_clients(void *data) {
  int64_t now = now_ms();
  int timeout = -1;
  struct client *c = srv.clients;
  while (c) {
    struct client *next = c->next;
    if (c->deadline <= now) {
      write_all(c->fd, "error\nrequest timed out\n", 25);
      client_close(c);
    } else if (timeout < 0 || c->deadline - now < timeout) {
      timeout = (int)(c->deadline - now);
    }
    c = next;
  }
  return timeout;
}

static bool socket_alive(const char *path) {
  int fd = connect_to(path);
  if (fd < 0)
    return false;
  close(fd);
  return true;
}

/* Take $path.lock, which a daemon holds for its whole life. With replace,
 * keep asking the holder to quit until it lets go (up to 5 s). */
static int take_lock(const char *path, bool replace) {
  char lock_path[128];
  snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
  int fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) {
    warn("cannot open %s: %s", lock_path, strerror(errno));
    return -1;
  }
  if (flock(fd, LOCK_EX | LOCK_NB) == 0)
    return fd;
  if (!replace) {
    warn("a daemon is already running on %s", path);
    close(fd);
    return -1;
  }
  int64_t give_up = now_ms() + 5000, next_quit = 0;
  while (now_ms() < give_up) {
    if (now_ms() >= next_quit && socket_alive(path)) {
      struct reply r = {0};
      bool ok;
      char *quit[] = {"quit"};
      ipc_client_send(path, 1, quit, &r, &ok);
      reply_free(&r);
      next_quit = now_ms() + 200;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
      warn("replaced the running daemon");
      return fd;
    }
    usleep(10000);
  }
  warn("the running daemon on %s did not quit", path);
  close(fd);
  return -1;
}

bool ipc_server_start(struct loop *loop, const char *path, bool replace,
                      ipc_handler_fn fn, void *data) {
  struct sockaddr_un addr;
  if (!make_addr(path, &addr) || strlen(path) + 6 > sizeof(srv.path))
    return false;
  int lock_fd = take_lock(path, replace);
  if (lock_fd < 0)
    return false;

  /* We hold the lock, so whatever is at path is stale. */
  unlink(path);
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
      listen(fd, 16) < 0) {
    warn("cannot listen on %s: %s", path, strerror(errno));
    if (fd >= 0)
      close(fd);
    close(lock_fd);
    return false;
  }
  struct stat st;
  if (stat(path, &st) == 0) {
    srv.dev = st.st_dev;
    srv.ino = st.st_ino;
  }
  srv.fd = fd;
  srv.lock_fd = lock_fd;
  snprintf(srv.path, sizeof(srv.path), "%s", path);
  srv.loop = loop;
  srv.fn = fn;
  srv.data = data;
  srv.watch = loop_add_fd(loop, fd, POLLIN, on_accept, NULL);
  srv.backoff = loop_add_timer(loop, CLOCK_MONOTONIC, on_backoff, NULL);
  loop_add_prepare(loop, 0, expire_clients, NULL);
  return true;
}

void ipc_server_stop(void) {
  if (srv.fd < 0)
    return;
  while (srv.clients)
    client_close(srv.clients);
  loop_remove_fd(srv.watch);
  loop_remove_timer(srv.backoff);
  loop_remove_prepare(srv.loop, expire_clients, NULL);
  /* Remove the socket only if it is still the one we bound, and before
   * releasing the lock, which lets a replacement bind its own. */
  struct stat st;
  if (stat(srv.path, &st) == 0 && st.st_dev == srv.dev &&
      st.st_ino == srv.ino)
    unlink(srv.path);
  close(srv.fd);
  close(srv.lock_fd);
  srv.fd = srv.lock_fd = -1;
}
