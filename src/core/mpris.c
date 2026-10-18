#include "mpris.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "art.h"
#include "util.h"

void mpris_init(struct mpris *m, struct loop *loop, const struct mpris_ops *ops,
                void *data) {
  /* art_id keeps counting, so a cover cached by id before can't match. */
  *m = (struct mpris){.loop = loop,
                      .ops = ops,
                      .data = data,
                      .fd = -1,
                      .ffd = -1,
                      .art_id = m->art_id};
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

static void delivered(struct mpris *m, const char *out) {
  /* No player: playerctl prints nothing (and fails), or an empty line
   * when following. */
  update_art(m, m->ops->parsed(out, m->data));
  changed(m);
}

/* Start `playerctl [-p player] metadata [--follow] --format ...` with its
 * stdout on a non-blocking pipe; false if it couldn't. */
static bool spawn(struct mpris *m, bool follow, pid_t *pid, int *fd) {
  const char *argv[9];
  int n = 0;
  argv[n++] = "playerctl";
  if (m->player[0]) {
    argv[n++] = "-p";
    argv[n++] = m->player;
  }
  argv[n++] = "metadata";
  if (follow)
    argv[n++] = "--follow";
  argv[n++] = "--format";
  argv[n++] = m->ops->format;
  argv[n] = NULL;
  int p[2];
  if (pipe2(p, O_CLOEXEC) < 0)
    return false;
  pid_t parent = getpid();
  *pid = fork();
  if (*pid < 0) {
    close(p[0]);
    close(p[1]);
    return false;
  }
  if (*pid == 0) {
    /* A follower may wait for a player forever: it goes with us, however
     * we go. */
    if (follow && (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0 || getppid() != parent))
      _exit(1);
    child_setup(p[1]);
    execvp(argv[0], (char *const *)argv);
    _exit(127);
  }
  close(p[1]);
  fcntl(p[0], F_SETFL, O_NONBLOCK);
  *fd = p[0];
  return true;
}

/* -- the poll: the state now -- */

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
  if (!m->followed)
    delivered(m, m->buf);
}

static void poll_player(struct mpris *m) {
  child_kill(m);
  m->followed = false;
  pid_t pid;
  if (!spawn(m, false, &pid, &m->fd))
    return;
  m->pid = pid;
  m->watched = loop_watch_child(m->loop, pid, on_child, m);
  m->len = 0;
  m->watch = loop_add_fd(m->loop, m->fd, POLLIN, on_output, m);
  m->started = now_ms();
}

/* -- the follower: every change -- */

static void follow_close(struct mpris *m) {
  loop_remove_fd(m->fwatch);
  m->fwatch = NULL;
  if (m->ffd >= 0)
    close(m->ffd);
  m->ffd = -1;
}

static void follow_kill(struct mpris *m) {
  if (m->fpid > 0) {
    kill(m->fpid, SIGKILL);
    if (!m->fwatched)
      waitpid(m->fpid, NULL, 0);
  }
  m->fpid = 0;
  follow_close(m);
}

static void on_follow_child(pid_t pid, int status, void *data) {
  struct mpris *m = data;
  if (m->fpid == pid)
    m->fpid = 0;
}

static void on_follow(int fd, short revents, void *data) {
  struct mpris *m = data;
  ssize_t r;
  while ((r = read(m->ffd, m->fbuf + m->flen, sizeof(m->fbuf) - 1 - m->flen)) >
         0) {
    m->flen += (size_t)r;
    /* Each whole line, with its newline (as the poll gives it). */
    char *nl;
    while ((nl = memchr(m->fbuf, '\n', m->flen))) {
      size_t n = (size_t)(nl - m->fbuf) + 1;
      char line[sizeof m->fbuf];
      memcpy(line, m->fbuf, n);
      line[n] = '\0';
      memmove(m->fbuf, m->fbuf + n, m->flen - n);
      m->flen -= n;
      m->followed = true;
      delivered(m, line);
      if (m->ffd != fd)
        return; /* stopped or restarted from the callback */
    }
    if (m->flen == sizeof(m->fbuf) - 1)
      m->flen = 0; /* no line fits: drop it */
  }
  if (r < 0 && errno == EAGAIN)
    return;
  /* It exited: the timer starts another. */
  follow_close(m);
  if (m->fpid > 0 && !m->fwatched) {
    waitpid(m->fpid, NULL, 0);
    m->fpid = 0;
  }
}

static void follow_player(struct mpris *m) {
  follow_kill(m);
  pid_t pid;
  if (!spawn(m, true, &pid, &m->ffd))
    return;
  m->fpid = pid;
  m->fwatched = loop_watch_child(m->loop, pid, on_follow_child, m);
  m->flen = 0;
  m->fwatch = loop_add_fd(m->loop, m->ffd, POLLIN, on_follow, m);
}

static void on_timer(struct loop_timer *t, void *data) {
  struct mpris *m = data;
  if ((m->fd >= 0 || m->pid > 0) && now_ms() - m->started >= m->kill_ms)
    child_kill(m); /* hung */
  if (m->ffd < 0 && m->fpid <= 0)
    follow_player(m);
}

void mpris_start(struct mpris *m, int interval_ms) {
  if (m->timer)
    return;
  m->timer = loop_add_timer(m->loop, CLOCK_MONOTONIC, on_timer, m);
  loop_timer_arm(m->timer, interval_ms, interval_ms);
  poll_player(m);
  follow_player(m);
}

void mpris_stop(struct mpris *m) {
  if (m->timer)
    loop_remove_timer(m->timer);
  m->timer = NULL;
  child_kill(m);
  follow_kill(m);
  if (m->curl > 0)
    m->art_url[0] = '\0'; /* fetch it again next time */
  curl_kill(m);
}

bool mpris_running(const struct mpris *m) { return m->timer != NULL; }

void mpris_repoll(struct mpris *m) {
  poll_player(m);
  follow_player(m);
}
