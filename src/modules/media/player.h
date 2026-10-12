/* The track playing, read with playerctl, and its cover. */
#ifndef SWELL_MEDIA_PLAYER_H
#define SWELL_MEDIA_PLAYER_H

#include <cairo.h>
#include <stdbool.h>
#include <stdint.h>

#include "loop.h"
#include "media.h"

struct media_track {
  bool has;     /* a player has a track */
  bool playing; /* else paused */
  int64_t len_us, pos_us; /* length 0: unknown */
  int64_t at_ms;          /* when pos_us was read (monotonic) */
  char player[64];        /* its name, without the ".instance…" suffix */
  char album[256], artist[256], title[256];
  char art_url[1024];
};

/* Parse a playerctl line "status\tlength\tposition\tart url\tplayer\t
 * album\tartist\ttitle" read at now. Returns t->has (false, and t zeroed,
 * without a track). */
bool media_parse(const char *line, int64_t now, struct media_track *t);
/* The position at now in µs, extrapolated while playing and kept within
 * the length when there is one. */
int64_t media_position(const struct media_track *t, int64_t now);
/* Whether a and b are the same track (not just a newer position). */
bool media_same_track(const struct media_track *a,
                      const struct media_track *b);

/* Called after a poll's result is in (the track may be unchanged). */
typedef void (*player_fn)(void *data);

void player_init(struct loop *loop, player_fn fn, void *data);
/* Poll every interval_ms from now on (a no-op if polling), with c's
 * player, cover and timeout settings. */
void player_start(const struct media_config *c);
/* Stop polling, kill playerctl and curl; the track is kept. */
void player_stop(void);
bool player_running(void);
/* Run `playerctl [-p player] verb` (play-pause, …) and poll as soon as it
 * exits, so the new state is in right away. */
void player_command(const struct media_config *c, const char *verb);
void player_finish(void);
const struct media_track *player_track(void);
/* The track's cover (RGB24, a centred square), or NULL; id changes
 * whenever it does. */
cairo_surface_t *player_art(unsigned *id);

#endif
