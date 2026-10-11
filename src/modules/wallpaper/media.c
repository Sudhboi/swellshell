/* What's playing, for the wallpaper: playerctl run every media_interval_ms
 * while the module animates, its output read on the daemon loop. Frozen or
 * dozing, nothing runs. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "loop.h"
#include "util.h"
#include "wallpaper.h"

/* A playerctl still running after this long is killed. */
#define KILL_MS 3000

static struct {
  struct loop *loop;
  struct loop_timer *timer;
  char player[128];
  pid_t pid;
  bool watched; /* the loop reaps pid; else we waitpid */
  int fd;
  struct loop_watch *watch;
  int64_t started;
  char buf[2048];
  size_t len;
  struct wp_media m;
} md = {.fd = -1};

static void copy_str(char *dst, size_t n, const char *src) {
  snprintf(dst, n, "%s", src);
}

bool wp_media_parse(const char *line, int64_t now, struct wp_media *m) {
  char buf[2048];
  copy_str(buf, sizeof buf, line);
  buf[strcspn(buf, "\n")] = '\0';
  /* status, length, position, artist, title: the title takes the rest, so
   * a tab in it survives */
  char *f[5];
  f[0] = buf;
  for (int i = 1; i < 5; ++i) {
    f[i] = strchr(f[i - 1], '\t');
    if (!f[i]) {
      memset(m, 0, sizeof *m);
      return false;
    }
    *f[i]++ = '\0';
  }
  if (!f[4][0] || (strcmp(f[0], "Playing") && strcmp(f[0], "Paused"))) {
    memset(m, 0, sizeof *m);
    return false;
  }
  m->has = true;
  m->playing = !strcmp(f[0], "Playing");
  m->len_us = strtoll(f[1], NULL, 10);
  if (m->len_us < 0)
    m->len_us = 0;
  m->pos_us = strtoll(f[2], NULL, 10);
  if (m->pos_us < 0)
    m->pos_us = 0;
  m->at_ms = now;
  copy_str(m->artist, sizeof m->artist, f[3]);
  copy_str(m->title, sizeof m->title, f[4]);
  return true;
}

double wp_media_progress(const struct wp_media *m, int64_t now) {
  if (!m->has || m->len_us <= 0)
    return -1;
  double pos = m->pos_us + (m->playing ? (now - m->at_ms) * 1000.0 : 0);
  double p = pos / m->len_us;
  return p < 0 ? 0 : p > 1 ? 1 : p;
}

const struct wp_media *wp_media_get(void) { return &md.m; }

static void child_close(void) {
  loop_remove_fd(md.watch);
  md.watch = NULL;
  if (md.fd >= 0)
    close(md.fd);
  md.fd = -1;
}

/* Kill a running playerctl and forget it. */
static void child_kill(void) {
  if (md.pid > 0) {
    kill(md.pid, SIGKILL);
    if (!md.watched)
      waitpid(md.pid, NULL, 0);
  }
  md.pid = 0;
  child_close();
}

static void on_child(pid_t pid, int status, void *data) {
  if (md.pid == pid)
    md.pid = 0;
}

static void on_output(int fd, short revents, void *data) {
  ssize_t r;
  while ((r = read(md.fd, md.buf + md.len, sizeof(md.buf) - 1 - md.len)) > 0)
    md.len += (size_t)r;
  if (r < 0 && errno == EAGAIN && md.len < sizeof(md.buf) - 1)
    return;
  md.buf[md.len] = '\0';
  child_close();
  if (md.pid > 0 && !md.watched) {
    waitpid(md.pid, NULL, 0);
    md.pid = 0;
  }
  /* No player: playerctl prints nothing (and fails). */
  wp_media_parse(md.buf, now_ms(), &md.m);
}

static void poll_player(void) {
  if (md.fd >= 0 || md.pid > 0) {
    if (now_ms() - md.started < KILL_MS)
      return;
    child_kill();
  }
  const char *argv[8];
  int n = 0;
  argv[n++] = "playerctl";
  if (md.player[0]) {
    argv[n++] = "-p";
    argv[n++] = md.player;
  }
  argv[n++] = "metadata";
  argv[n++] = "--format";
  argv[n++] = "{{status}}\t{{mpris:length}}\t{{position}}\t{{artist}}\t"
              "{{title}}";
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
    /* The daemon blocks signals for its signalfd and ignores SIGPIPE;
     * don't pass that on. */
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    signal(SIGPIPE, SIG_DFL);
    dup2(p[1], STDOUT_FILENO);
    int null = open("/dev/null", O_RDWR);
    if (null >= 0) {
      dup2(null, STDIN_FILENO);
      dup2(null, STDERR_FILENO);
    }
    execvp(argv[0], (char *const *)argv);
    _exit(127);
  }
  close(p[1]);
  fcntl(p[0], F_SETFL, O_NONBLOCK);
  md.pid = pid;
  md.watched = loop_watch_child(md.loop, pid, on_child, NULL);
  md.fd = p[0];
  md.len = 0;
  md.watch = loop_add_fd(md.loop, md.fd, POLLIN, on_output, NULL);
  md.started = now_ms();
}

static void on_timer(struct loop_timer *t, void *data) { poll_player(); }

void wp_media_start(struct loop *loop, const struct wallpaper_config *c) {
  if (md.timer)
    return;
  md.loop = loop;
  copy_str(md.player, sizeof md.player, c->media_player);
  md.timer = loop_add_timer(loop, CLOCK_MONOTONIC, on_timer, NULL);
  loop_timer_arm(md.timer, c->media_interval_ms, c->media_interval_ms);
  poll_player();
}

void wp_media_stop(void) {
  if (md.timer)
    loop_remove_timer(md.timer);
  md.timer = NULL;
  child_kill();
}

void wp_media_clear(void) { memset(&md.m, 0, sizeof md.m); }
