#include "mpris.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "art.h"
#include "util.h"

void mpris_init(struct mpris *m, struct loop *loop, const struct mpris_ops *ops,
                void *data) {
  /* art_id keeps counting, so a cover cached by id before can't match. */
  *m = (struct mpris){
      .loop = loop, .ops = ops, .data = data, .fd = -1, .art_id = m->art_id};
}

void mpris_configure(struct mpris *m, const char *player, int kill_ms) {
  copy_str(m->player, sizeof m->player, player);
  m->kill_ms = kill_ms;
}

static void changed(struct mpris *m) {
  if (m->ops->changed)
    m->ops->changed(m->data);
}

/* --- the cover --- */

cairo_surface_t *mpris_art(const struct mpris *m, unsigned *id) {
  *id = m->art_id;
  return m->art;
}

static void set_art(struct mpris *m, cairo_surface_t *art) {
  if (m->art)
    cairo_surface_destroy(m->art);
  m->art = art;
  m->art_id++;
}

void mpris_forget_art(struct mpris *m) { m->art_url[0] = '\0'; }

void mpris_clear_art(struct mpris *m) {
  set_art(m, NULL);
  m->art_url[0] = '\0';
}

static void curl_kill(struct mpris *m) {
  if (m->curl > 0)
    kill(m->curl, SIGKILL); /* the loop reaps it */
  m->curl = 0;
  if (m->art_tmp[0])
    unlink(m->art_tmp);
}

static void on_curl(pid_t pid, int status, void *data) {
  struct mpris *m = data;
  if (pid != m->curl)
    return; /* killed: a newer cover or a stop */
  m->curl = 0;
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
    set_art(m, art_load(m->art_tmp, m->ops->grey));
  unlink(m->art_tmp);
  changed(m);
}

static void fetch_art(struct mpris *m, const char *url) {
  snprintf(m->art_tmp, sizeof m->art_tmp, "%s/%s-%d", runtime_dir(),
           m->ops->art_tag, (int)getpid());
  pid_t pid = fork();
  if (pid < 0)
    return;
  if (pid == 0) {
    child_setup(-1);
    execlp("curl", "curl", "-sfL", "--max-time", "10", "--max-filesize",
           "20000000", "-o", m->art_tmp, "--", url, (char *)NULL);
    _exit(127);
  }
  if (!loop_watch_child(m->loop, pid, on_curl, m)) {
    /* Nobody would reap it in time: give the cover up. */
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return;
  }
  m->curl = pid;
}

/* Follow the track's cover URL. */
static void update_art(struct mpris *m, const char *url) {
  if (!strcmp(url, m->art_url))
    return;
  copy_str(m->art_url, sizeof m->art_url, url);
  curl_kill(m);
  set_art(m, NULL);
  char path[1024];
  if (art_file_path(url, path, sizeof path))
    set_art(m, art_load(path, m->ops->grey));
  else if (!strncmp(url, "http://", 7) || !strncmp(url, "https://", 8))
    fetch_art(m, url);
}

/* --- playerctl --- */

static void child_close(struct mpris *m) {
  loop_remove_fd(m->watch);
  m->watch = NULL;
  if (m->fd >= 0)
    close(m->fd);
  m->fd = -1;
}

/* Kill a running playerctl and forget it. */
static void child_kill(struct mpris *m) {
  if (m->pid > 0) {
    kill(m->pid, SIGKILL);
    if (!m->watched)
      waitpid(m->pid, NULL, 0);
  }
  m->pid = 0;
  child_close(m);
}

static void on_child(pid_t pid, int status, void *data) {
  struct mpris *m = data;
  if (m->pid == pid)
    m->pid = 0;
}

static void on_output(int fd, short revents, void *data) {
  struct mpris *m = data;
  ssize_t r;
  while ((r = read(m->fd, m->buf + m->len, sizeof(m->buf) - 1 - m->len)) > 0)
    m->len += (size_t)r;
  if (r < 0 && errno == EAGAIN && m->len < sizeof(m->buf) - 1)
    return;
  m->buf[m->len] = '\0';
  child_close(m);
  if (m->pid > 0 && !m->watched) {
    waitpid(m->pid, NULL, 0);
    m->pid = 0;
  }
  /* No player: playerctl prints nothing (and fails). */
  update_art(m, m->ops->parsed(m->buf, m->data));
  changed(m);
}

static void poll_player(struct mpris *m) {
  if (m->fd >= 0 || m->pid > 0) {
    if (now_ms() - m->started < m->kill_ms)
      return;
    child_kill(m);
  }
  const char *argv[8];
  int n = 0;
  argv[n++] = "playerctl";
  if (m->player[0]) {
    argv[n++] = "-p";
    argv[n++] = m->player;
  }
  argv[n++] = "metadata";
  argv[n++] = "--format";
  argv[n++] = m->ops->format;
  argv[n] = NULL;
  int p[2];
  if (pipe2(p, O_CLOEXEC) < 0)
    return;
  pid_t pid = fork();
  if (pid < 0) {
    close(p[0]);
    close(p[1]);
    return;
  }
  if (pid == 0) {
    child_setup(p[1]);
    execvp(argv[0], (char *const *)argv);
    _exit(127);
  }
  close(p[1]);
  fcntl(p[0], F_SETFL, O_NONBLOCK);
  m->pid = pid;
  m->watched = loop_watch_child(m->loop, pid, on_child, m);
  m->fd = p[0];
  m->len = 0;
  m->watch = loop_add_fd(m->loop, m->fd, POLLIN, on_output, m);
  m->started = now_ms();
}

static void on_timer(struct loop_timer *t, void *data) { poll_player(data); }

void mpris_start(struct mpris *m, int interval_ms) {
  if (m->timer)
    return;
  m->timer = loop_add_timer(m->loop, CLOCK_MONOTONIC, on_timer, m);
  loop_timer_arm(m->timer, interval_ms, interval_ms);
  poll_player(m);
}

void mpris_stop(struct mpris *m) {
  if (m->timer)
    loop_remove_timer(m->timer);
  m->timer = NULL;
  child_kill(m);
  if (m->curl > 0)
    m->art_url[0] = '\0'; /* fetch it again next time */
  curl_kill(m);
}

bool mpris_running(const struct mpris *m) { return m->timer != NULL; }

void mpris_repoll(struct mpris *m) {
  child_kill(m);
  poll_player(m);
}
