#include "loop.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util.h"

struct loop_watch {
  struct loop *loop;
  int fd;
  short events;
  loop_fd_fn fn;
  void *data;
  bool dead;
  void (*destroy)(struct loop_watch *w); /* frees data at loop_destroy */
};

struct loop_timer {
  struct loop_watch *watch;
  loop_timer_fn fn;
  void *data;
  bool armed;
};

struct prepare {
  int priority;
  loop_prepare_fn fn;
  void *data;
  bool dead;
};

struct child {
  struct loop_watch *watch;
  pid_t pid;
  loop_child_fn fn;
  void *data;
};

struct loop {
  struct loop_watch **watches;
  int nwatches;
  struct prepare *prepares;
  int nprepares;
  bool running;
  bool dirty; /* dead watches/prepares to sweep */
  int signal_fd;
  struct pollfd *pfds;
  int npfds_cap;
};

static void on_signal(int fd, short revents, void *data) {
  struct loop *loop = data;
  struct signalfd_siginfo si;
  while (read(fd, &si, sizeof(si)) == sizeof(si)) {
    if (si.ssi_signo == SIGINT || si.ssi_signo == SIGTERM)
      loop->running = false;
  }
}

struct loop *loop_create(void) {
  struct loop *loop = xcalloc(1, sizeof(*loop));
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  sigprocmask(SIG_BLOCK, &mask, NULL);
  signal(SIGPIPE, SIG_IGN);
  loop->signal_fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
  if (loop->signal_fd >= 0)
    loop_add_fd(loop, loop->signal_fd, POLLIN, on_signal, loop);
  loop->running = true;
  return loop;
}

static void sweep(struct loop *loop) {
  if (!loop->dirty)
    return;
  int j = 0;
  for (int i = 0; i < loop->nwatches; ++i) {
    if (loop->watches[i]->dead)
      free(loop->watches[i]);
    else
      loop->watches[j++] = loop->watches[i];
  }
  loop->nwatches = j;
  j = 0;
  for (int i = 0; i < loop->nprepares; ++i)
    if (!loop->prepares[i].dead)
      loop->prepares[j++] = loop->prepares[i];
  loop->nprepares = j;
  loop->dirty = false;
}

void loop_destroy(struct loop *loop) {
  if (!loop)
    return;
  for (int i = 0; i < loop->nwatches; ++i) {
    struct loop_watch *w = loop->watches[i];
    if (!w->dead && w->destroy)
      w->destroy(w);
    w->dead = true;
  }
  loop->dirty = true;
  sweep(loop);
  if (loop->signal_fd >= 0)
    close(loop->signal_fd);
  free(loop->watches);
  free(loop->prepares);
  free(loop->pfds);
  free(loop);
}

void loop_quit(struct loop *loop) { loop->running = false; }
bool loop_running(struct loop *loop) { return loop->running; }

struct loop_watch *loop_add_fd(struct loop *loop, int fd, short events,
                               loop_fd_fn fn, void *data) {
  struct loop_watch *w = xcalloc(1, sizeof(*w));
  *w = (struct loop_watch){loop, fd, events, fn, data, false, NULL};
  loop->watches =
      xrealloc(loop->watches, (loop->nwatches + 1) * sizeof(*loop->watches));
  loop->watches[loop->nwatches++] = w;
  return w;
}

void loop_watch_set_events(struct loop_watch *w, short events) {
  w->events = events;
}

void loop_remove_fd(struct loop_watch *w) {
  if (!w)
    return;
  w->dead = true;
  w->loop->dirty = true;
}

/* ---- timers ---- */

static void on_timer(int fd, short revents, void *data) {
  struct loop_timer *t = data;
  uint64_t n;
  if (read(fd, &n, sizeof(n)) != sizeof(n))
    return;
  struct itimerspec cur;
  if (timerfd_gettime(fd, &cur) == 0 && cur.it_value.tv_sec == 0 &&
      cur.it_value.tv_nsec == 0)
    t->armed = false;
  t->fn(t, t->data);
}

struct loop_timer *loop_add_timer(struct loop *loop, int clock,
                                  loop_timer_fn fn, void *data) {
  int fd = timerfd_create(clock, TFD_NONBLOCK | TFD_CLOEXEC);
  if (fd < 0)
    die("timerfd_create: %s", strerror(errno));
  struct loop_timer *t = xcalloc(1, sizeof(*t));
  t->fn = fn;
  t->data = data;
  t->watch = loop_add_fd(loop, fd, POLLIN, on_timer, t);
  return t;
}

static struct timespec ms_ts(int64_t ms) {
  return (struct timespec){ms / 1000, (ms % 1000) * 1000000L};
}

void loop_timer_arm(struct loop_timer *t, int64_t ms, int64_t interval_ms) {
  struct itimerspec its = {0};
  if (ms > 0) {
    its.it_value = ms_ts(ms);
    its.it_interval = ms_ts(interval_ms > 0 ? interval_ms : 0);
  }
  timerfd_settime(t->watch->fd, 0, &its, NULL);
  t->armed = ms > 0;
}

void loop_timer_arm_abs(struct loop_timer *t, const struct itimerspec *its) {
  timerfd_settime(t->watch->fd, TFD_TIMER_ABSTIME, its, NULL);
  t->armed = its->it_value.tv_sec || its->it_value.tv_nsec;
}

void loop_timer_disarm(struct loop_timer *t) { loop_timer_arm(t, 0, 0); }
bool loop_timer_armed(struct loop_timer *t) { return t->armed; }

void loop_remove_timer(struct loop_timer *t) {
  if (!t)
    return;
  close(t->watch->fd);
  loop_remove_fd(t->watch);
  free(t);
}

/* ---- prepare hooks ---- */

void loop_add_prepare(struct loop *loop, int priority, loop_prepare_fn fn,
                      void *data) {
  loop->prepares =
      xrealloc(loop->prepares, (loop->nprepares + 1) * sizeof(*loop->prepares));
  int i = loop->nprepares++;
  while (i > 0 && loop->prepares[i - 1].priority > priority) {
    loop->prepares[i] = loop->prepares[i - 1];
    --i;
  }
  loop->prepares[i] = (struct prepare){priority, fn, data, false};
}

void loop_remove_prepare(struct loop *loop, loop_prepare_fn fn, void *data) {
  for (int i = 0; i < loop->nprepares; ++i)
    if (loop->prepares[i].fn == fn && loop->prepares[i].data == data) {
      loop->prepares[i].dead = true;
      loop->dirty = true;
    }
}

/* ---- children ---- */

static void on_child(int fd, short revents, void *data) {
  struct child *c = data;
  int status = 0;
  pid_t r = waitpid(c->pid, &status, WNOHANG);
  if (r == 0)
    return; /* spurious */
  close(fd);
  loop_remove_fd(c->watch);
  if (c->fn)
    c->fn(c->pid, r < 0 ? -1 : status, c->data);
  free(c);
}

static void child_destroy(struct loop_watch *w) {
  struct child *c = w->data;
  close(w->fd);
  free(c);
}

bool loop_watch_child(struct loop *loop, pid_t pid, loop_child_fn fn,
                      void *data) {
  int fd = (int)syscall(SYS_pidfd_open, pid, 0);
  if (fd < 0)
    return false;
  struct child *c = xcalloc(1, sizeof(*c));
  c->pid = pid;
  c->fn = fn;
  c->data = data;
  c->watch = loop_add_fd(loop, fd, POLLIN, on_child, c);
  c->watch->destroy = child_destroy;
  return true;
}

/* ---- running ---- */

void loop_run(struct loop *loop) {
  while (loop->running) {
    int timeout = -1;
    for (int i = 0; i < loop->nprepares; ++i) {
      struct prepare *p = &loop->prepares[i];
      if (p->dead)
        continue;
      int t = p->fn(p->data);
      if (t >= 0 && (timeout < 0 || t < timeout))
        timeout = t;
    }
    if (!loop->running)
      break;
    sweep(loop);

    int n = loop->nwatches;
    if (n > loop->npfds_cap) {
      loop->npfds_cap = n * 2;
      loop->pfds = xrealloc(loop->pfds, loop->npfds_cap * sizeof(*loop->pfds));
    }
    for (int i = 0; i < n; ++i)
      loop->pfds[i] = (struct pollfd){loop->watches[i]->fd,
                                      loop->watches[i]->events, 0};
    if (poll(loop->pfds, n, timeout) < 0) {
      if (errno == EINTR)
        continue;
      warn("poll: %s", strerror(errno));
      break;
    }
    /* Callbacks may add watches (appended past n) or remove some (marked
     * dead), so index the snapshot only. */
    for (int i = 0; i < n && loop->running; ++i) {
      struct loop_watch *w = loop->watches[i];
      if (!w->dead && loop->pfds[i].revents)
        w->fn(w->fd, loop->pfds[i].revents, w->data);
    }
    sweep(loop);
  }
}
