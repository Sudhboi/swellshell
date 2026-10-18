/* What's playing, from playerctl (wallpaper, media): one `playerctl
 * metadata --format ...` for the state now, then a `--follow` one printing
 * a line on every change, both on the daemon loop; each output is handed
 * to the module to parse, and the track's cover followed (a file:// one
 * read directly, an http(s) one fetched by curl first). A struct mpris is
 * one poller; the module owns it and its parsed track. */
#ifndef SWELL_MPRIS_H
#define SWELL_MPRIS_H

#include <cairo.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "loop.h"

struct mpris_ops {
  const char *format;  /* playerctl's --format */
  bool grey;           /* covers turned into their luminance */
  const char *art_tag; /* curl downloads to $XDG_RUNTIME_DIR/<tag>-<pid> */
  /* A poll's output is in (empty, or an error, without a player): parse
   * it and return the cover URL to show, "" for none. */
  const char *(*parsed)(const char *out, void *data);
  /* After a poll's result or a fetched cover is in; may be NULL. */
  void (*changed)(void *data);
};

struct mpris {
  struct loop *loop;
  const struct mpris_ops *ops;
  void *data;
  struct loop_timer *timer; /* polling */
  char player[128];         /* playerctl -p, "" for any */
  int kill_ms;              /* a playerctl still running this long is killed */
  /* the running playerctl */
  pid_t pid;
  bool watched; /* the loop reaps pid; else we waitpid */
  int fd;
  struct loop_watch *watch;
  int64_t started;
  char buf[8192];
  size_t len;
  /* the playerctl --follow: a line whenever the player changes (and each
   * second while it plays, for the position) */
  pid_t fpid;
  bool fwatched;
  int ffd;
  struct loop_watch *fwatch;
  char fbuf[8192];
  size_t flen;
  bool followed; /* it answered since the poll started: the poll is older */
  /* the cover */
  char art_url[1024]; /* the cover shown or being fetched */
  cairo_surface_t *art;
  unsigned art_id;
  pid_t curl;
  char art_tmp[512]; /* curl's download */
};

/* m is zeroed (static) or a stopped poller. */
void mpris_init(struct mpris *m, struct loop *loop, const struct mpris_ops *ops,
                void *data);
/* The player and timeout the next polls use. */
void mpris_configure(struct mpris *m, const char *player, int kill_ms);
/* Poll now and follow from now on (a no-op if following); a follower that
 * exits is restarted within interval_ms. */
void mpris_start(struct mpris *m, int interval_ms);
/* Stop following, kill playerctl and curl; the cover is kept. */
void mpris_stop(struct mpris *m);
bool mpris_running(const struct mpris *m);
/* Drop what's in flight (it may report an old state), poll and follow
 * afresh. */
void mpris_repoll(struct mpris *m);
/* The cover (RGB24, a centred square), or NULL; id changes whenever it
 * does. */
cairo_surface_t *mpris_art(const struct mpris *m, unsigned *id);
/* Look at the cover URL again on the next poll, even if unchanged. */
void mpris_forget_art(struct mpris *m);
/* Drop the cover and forget its URL. */
void mpris_clear_art(struct mpris *m);

#endif
