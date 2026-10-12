/* Cairo drawing of the media panel. */
#ifndef SWELL_MEDIA_RENDER_H
#define SWELL_MEDIA_RENDER_H

#include <cairo.h>

#include "media.h"
#include "player.h"

/* One track's look: its details and cover (NULL: none). art_id changes
 * with the cover. paused (0 playing .. 1 paused) morphs the status icon
 * and fades the bar and times toward muted; the caller tweens it when
 * t->playing changes. */
struct media_slot {
  const struct media_track *t;
  cairo_surface_t *art;
  unsigned art_id;
  double paused;
};

struct media_frame {
  double shown;  /* 0 above the screen's edge .. 1 dropped down */
  struct media_slot cur;
  struct media_slot prev; /* the track before, fading out; t NULL: none */
  double fade;            /* 0..1, cur over prev */
  int64_t now;            /* ms, monotonic: the position and the chase */
};

/* Draws in logical width x media_surface_height() coordinates, the panel
 * hanging from y = top_gap when shown; the caller has already scaled and
 * cleared the surface. */
void media_render(cairo_t *cr, const struct media_config *cfg,
                  const struct media_frame *f);

/* Whether t needs drawing every frame: playing without a length, the
 * chase runs. */
bool media_render_moving(const struct media_track *t);

/* "m:ss", or "h:mm:ss" from an hour, of us microseconds. */
void media_format_time(char *out, size_t n, int64_t us);

#endif
