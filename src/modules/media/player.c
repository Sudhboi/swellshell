/* What's playing, for the media panel: polled with playerctl (core
 * mpris.c) every interval_ms while polling is on, plus playerctl commands
 * (play-pause, ...). */
#include "player.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "mpris.h"
#include "util.h"

static struct {
  struct mpris poll;
  player_fn fn;
  void *data;
  struct media_track t;
  bool want_art;
  pid_t ctl; /* a player_command running */
} pl;

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

cairo_surface_t *player_art(unsigned *id) { return mpris_art(&pl.poll, id); }

static const char *parsed(const char *out, void *data) {
  media_parse(out, now_ms(), &pl.t);
  return pl.want_art && pl.t.has ? pl.t.art_url : "";
}

static void changed(void *data) {
  if (pl.fn)
    pl.fn(pl.data);
}

static const struct mpris_ops ops = {
    .format = "{{status}}\t{{mpris:length}}\t{{position}}\t{{mpris:artUrl}}"
              "\t{{playerName}}\t{{album}}\t{{artist}}\t{{title}}",
    .grey = false,
    .art_tag = "swell-media-art",
    .parsed = parsed,
    .changed = changed,
};

static void on_ctl(pid_t pid, int status, void *data) {
  if (pid != pl.ctl)
    return;
  pl.ctl = 0;
  /* A poll started before the command would report the old state. */
  mpris_repoll(&pl.poll);
}

void player_command(const struct media_config *c, const char *verb) {
  if (pl.ctl > 0)
    kill(pl.ctl, SIGKILL); /* the loop reaps it */
  pl.ctl = 0;
  if (!mpris_running(&pl.poll)) {
    /* Not polling: take the settings the poll after it will use. */
    pl.want_art = c->art;
    mpris_configure(&pl.poll, c->player, c->command_timeout_ms);
  }
  const char *player = pl.poll.player;
  pid_t pid = fork();
  if (pid < 0)
    return;
  if (pid == 0) {
    child_setup(-1);
    if (player[0])
      execlp("playerctl", "playerctl", "-p", player, verb, (char *)NULL);
    else
      execlp("playerctl", "playerctl", verb, (char *)NULL);
    _exit(127);
  }
  if (!loop_watch_child(pl.poll.loop, pid, on_ctl, NULL)) {
    waitpid(pid, NULL, 0);
    mpris_repoll(&pl.poll);
    return;
  }
  pl.ctl = pid;
}

void player_init(struct loop *loop, player_fn fn, void *data) {
  mpris_init(&pl.poll, loop, &ops, NULL);
  pl.fn = fn;
  pl.data = data;
}

void player_start(const struct media_config *c) {
  if (mpris_running(&pl.poll))
    return;
  if (pl.want_art != c->art)
    mpris_forget_art(&pl.poll); /* look at the cover again */
  pl.want_art = c->art;
  mpris_configure(&pl.poll, c->player, c->command_timeout_ms);
  mpris_start(&pl.poll, c->interval_ms);
}

void player_stop(void) { mpris_stop(&pl.poll); }

bool player_running(void) { return mpris_running(&pl.poll); }

void player_finish(void) {
  player_stop();
  if (pl.ctl > 0)
    kill(pl.ctl, SIGKILL);
  pl.ctl = 0;
  mpris_clear_art(&pl.poll);
  memset(&pl.t, 0, sizeof pl.t);
  pl.fn = NULL;
}
