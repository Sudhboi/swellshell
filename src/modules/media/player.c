/* What's playing, for the media panel: playerctl run every interval_ms
 * while polling is on, its output read on the daemon loop, and the track's
 * cover (a file:// one read directly, an http(s) one fetched by curl
 * first). The same pattern as wallpaper/media.c. */
#include "player.h"

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

#include "art.h"
#include "util.h"

static struct {
  struct loop *loop;
  player_fn fn;
  void *data;
  struct loop_timer *timer;
  char player[128];
  int kill_ms;
  pid_t pid;
  bool watched; /* the loop reaps pid; else we waitpid */
  int fd;
  struct loop_watch *watch;
  int64_t started;
  char buf[8192];
  size_t len;
  struct media_track t;
  /* the cover */
  bool want_art;
  char art_url[1024]; /* the cover shown or being fetched */
  cairo_surface_t *art;
  unsigned art_id;
  pid_t curl;
  char art_tmp[512]; /* curl's download */
  pid_t ctl;         /* a player_command running */
} pl = {.fd = -1};

static void copy_str(char *dst, size_t n, const char *src) {
  snprintf(dst, n, "%s", src);
}

bool media_parse(const char *line, int64_t now, struct media_track *t) {
  char buf[8192];
  copy_str(buf, sizeof buf, line);
  buf[strcspn(buf, "\n")] = '\0';
  /* status, length, position, art url, player, album, artist, title: the
   * title takes the rest, so a tab in it survives */
  enum { NF = 8 };
  char *f[NF];
  f[0] = buf;
  for (int i = 1; i < NF; ++i) {
    f[i] = strchr(f[i - 1], '\t');
    if (!f[i]) {
      memset(t, 0, sizeof *t);
      return false;
    }
    *f[i]++ = '\0';
  }
  if (!f[7][0] || (strcmp(f[0], "Playing") && strcmp(f[0], "Paused"))) {
    memset(t, 0, sizeof *t);
    return false;
  }
  t->has = true;
  t->playing = !strcmp(f[0], "Playing");
  t->len_us = strtoll(f[1], NULL, 10);
  if (t->len_us < 0)
    t->len_us = 0;
  t->pos_us = strtoll(f[2], NULL, 10);
  if (t->pos_us < 0)
    t->pos_us = 0;
  t->at_ms = now;
  copy_str(t->art_url, sizeof t->art_url, f[3]);
  f[4][strcspn(f[4], ".")] = '\0'; /* "firefox.instance_1_23" */
  copy_str(t->player, sizeof t->player, f[4]);
  copy_str(t->album, sizeof t->album, f[5]);
  copy_str(t->artist, sizeof t->artist, f[6]);
  copy_str(t->title, sizeof t->title, f[7]);
  return true;
}

int64_t media_position(const struct media_track *t, int64_t now) {
  if (!t->has)
    return 0;
  int64_t pos = t->pos_us + (t->playing ? (now - t->at_ms) * 1000 : 0);
  if (pos < 0)
    pos = 0;
  if (t->len_us > 0 && pos > t->len_us)
    pos = t->len_us;
  return pos;
}

bool media_same_track(const struct media_track *a,
                      const struct media_track *b) {
  return a->has == b->has && !strcmp(a->title, b->title) &&
         !strcmp(a->artist, b->artist) && !strcmp(a->album, b->album) &&
         !strcmp(a->player, b->player);
}

const struct media_track *player_track(void) { return &pl.t; }

cairo_surface_t *player_art(unsigned *id) {
  *id = pl.art_id;
  return pl.art;
}

static void set_art(cairo_surface_t *art) {
  if (pl.art)
    cairo_surface_destroy(pl.art);
  pl.art = art;
  pl.art_id++;
}

static void curl_kill(void) {
  if (pl.curl > 0)
    kill(pl.curl, SIGKILL); /* the loop reaps it */
  pl.curl = 0;
  if (pl.art_tmp[0])
    unlink(pl.art_tmp);
}

static void on_curl(pid_t pid, int status, void *data) {
  if (pid != pl.curl)
    return; /* killed: a newer cover or a stop */
  pl.curl = 0;
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
    set_art(art_load(pl.art_tmp, false));
  unlink(pl.art_tmp);
  if (pl.fn)
    pl.fn(pl.data);
}

static void fetch_art(const char *url) {
  snprintf(pl.art_tmp, sizeof pl.art_tmp, "%s/swell-media-art-%d",
           runtime_dir(), (int)getpid());
  pid_t pid = fork();
  if (pid < 0)
    return;
  if (pid == 0) {
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    signal(SIGPIPE, SIG_DFL);
    int null = open("/dev/null", O_RDWR);
    if (null >= 0) {
      dup2(null, STDIN_FILENO);
      dup2(null, STDOUT_FILENO);
      dup2(null, STDERR_FILENO);
    }
    execlp("curl", "curl", "-sfL", "--max-time", "10", "--max-filesize",
           "20000000", "-o", pl.art_tmp, "--", url, (char *)NULL);
    _exit(127);
  }
  if (!loop_watch_child(pl.loop, pid, on_curl, NULL)) {
    /* Nobody would reap it in time: give the cover up. */
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return;
  }
  pl.curl = pid;
}

/* Follow the track's cover URL. */
static void update_art(void) {
  const char *url = pl.want_art && pl.t.has ? pl.t.art_url : "";
  if (!strcmp(url, pl.art_url))
    return;
  copy_str(pl.art_url, sizeof pl.art_url, url);
  curl_kill();
  set_art(NULL);
  char path[1024];
  if (art_file_path(url, path, sizeof path))
    set_art(art_load(path, false));
  else if (!strncmp(url, "http://", 7) || !strncmp(url, "https://", 8))
    fetch_art(url);
}

static void child_close(void) {
  loop_remove_fd(pl.watch);
  pl.watch = NULL;
  if (pl.fd >= 0)
    close(pl.fd);
  pl.fd = -1;
}

/* Kill a running playerctl and forget it. */
static void child_kill(void) {
  if (pl.pid > 0) {
    kill(pl.pid, SIGKILL);
    if (!pl.watched)
      waitpid(pl.pid, NULL, 0);
  }
  pl.pid = 0;
  child_close();
}

static void on_child(pid_t pid, int status, void *data) {
  if (pl.pid == pid)
    pl.pid = 0;
}

static void on_output(int fd, short revents, void *data) {
  ssize_t r;
  while ((r = read(pl.fd, pl.buf + pl.len, sizeof(pl.buf) - 1 - pl.len)) > 0)
    pl.len += (size_t)r;
  if (r < 0 && errno == EAGAIN && pl.len < sizeof(pl.buf) - 1)
    return;
  pl.buf[pl.len] = '\0';
  child_close();
  if (pl.pid > 0 && !pl.watched) {
    waitpid(pl.pid, NULL, 0);
    pl.pid = 0;
  }
  /* No player: playerctl prints nothing (and fails). */
  media_parse(pl.buf, now_ms(), &pl.t);
  update_art();
  if (pl.fn)
    pl.fn(pl.data);
}

static void poll_player(void) {
  if (pl.fd >= 0 || pl.pid > 0) {
    if (now_ms() - pl.started < pl.kill_ms)
      return;
    child_kill();
  }
  const char *argv[8];
  int n = 0;
  argv[n++] = "playerctl";
  if (pl.player[0]) {
    argv[n++] = "-p";
    argv[n++] = pl.player;
  }
  argv[n++] = "metadata";
  argv[n++] = "--format";
  argv[n++] = "{{status}}\t{{mpris:length}}\t{{position}}\t{{mpris:artUrl}}"
              "\t{{playerName}}\t{{album}}\t{{artist}}\t{{title}}";
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
  pl.pid = pid;
  pl.watched = loop_watch_child(pl.loop, pid, on_child, NULL);
  pl.fd = p[0];
  pl.len = 0;
  pl.watch = loop_add_fd(pl.loop, pl.fd, POLLIN, on_output, NULL);
  pl.started = now_ms();
}

static void on_timer(struct loop_timer *t, void *data) { poll_player(); }

static void on_ctl(pid_t pid, int status, void *data) {
  if (pid != pl.ctl)
    return;
  pl.ctl = 0;
  /* A poll started before the command would report the old state. */
  child_kill();
  poll_player();
}

void player_command(const struct media_config *c, const char *verb) {
  if (pl.ctl > 0)
    kill(pl.ctl, SIGKILL); /* the loop reaps it */
  pl.ctl = 0;
  if (!pl.timer) {
    /* Not polling: take the settings the poll after it will use. */
    pl.want_art = c->art;
    pl.kill_ms = c->command_timeout_ms;
    copy_str(pl.player, sizeof pl.player, c->player);
  }
  pid_t pid = fork();
  if (pid < 0)
    return;
  if (pid == 0) {
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    signal(SIGPIPE, SIG_DFL);
    int null = open("/dev/null", O_RDWR);
    if (null >= 0) {
      dup2(null, STDIN_FILENO);
      dup2(null, STDOUT_FILENO);
      dup2(null, STDERR_FILENO);
    }
    if (pl.player[0])
      execlp("playerctl", "playerctl", "-p", pl.player, verb, (char *)NULL);
    else
      execlp("playerctl", "playerctl", verb, (char *)NULL);
    _exit(127);
  }
  if (!loop_watch_child(pl.loop, pid, on_ctl, NULL)) {
    waitpid(pid, NULL, 0);
    child_kill();
    poll_player();
    return;
  }
  pl.ctl = pid;
}

void player_init(struct loop *loop, player_fn fn, void *data) {
  pl.loop = loop;
  pl.fn = fn;
  pl.data = data;
}

void player_start(const struct media_config *c) {
  if (pl.timer)
    return;
  if (pl.want_art != c->art)
    pl.art_url[0] = '\0'; /* look at the cover again */
  pl.want_art = c->art;
  pl.kill_ms = c->command_timeout_ms;
  copy_str(pl.player, sizeof pl.player, c->player);
  pl.timer = loop_add_timer(pl.loop, CLOCK_MONOTONIC, on_timer, NULL);
  loop_timer_arm(pl.timer, c->interval_ms, c->interval_ms);
  poll_player();
}

void player_stop(void) {
  if (pl.timer)
    loop_remove_timer(pl.timer);
  pl.timer = NULL;
  child_kill();
  if (pl.curl > 0)
    pl.art_url[0] = '\0'; /* fetch it again next time */
  curl_kill();
}

bool player_running(void) { return pl.timer != NULL; }

void player_finish(void) {
  player_stop();
  if (pl.ctl > 0)
    kill(pl.ctl, SIGKILL);
  pl.ctl = 0;
  set_art(NULL);
  memset(&pl.t, 0, sizeof pl.t);
  pl.art_url[0] = '\0';
  pl.fn = NULL;
}
