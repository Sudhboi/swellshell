/* wsfade's motion: the snapshot's opacity and horizontal offset. */
#ifndef SWELL_WSFADE_CURVE_H
#define SWELL_WSFADE_CURVE_H

#include <stdbool.h>

#include "anim.h"

struct wsfade_curve {
  struct tween alpha, x; /* x in logical px */
};

/* Fade out over ms while drifting drift px away from dir (+1: the old
 * workspace leaves to the left, as if the next one came in from the
 * right). */
static inline void wsfade_start(struct wsfade_curve *c, int dir, double drift,
                                double ms, double now) {
  c->alpha = (struct tween){.from = 1, .to = 0, .t0 = now, .dur = ms};
  c->x = (struct tween){.from = 0, .to = -dir * drift, .t0 = now, .dur = ms};
}

/* Another switch while fading: from wherever it is, fade the rest of the
 * way in the share of ms the remaining opacity is worth, drifting dir. */
static inline void wsfade_retarget(struct wsfade_curve *c, int dir,
                                   double drift, double ms, double now) {
  double a = tween_value(&c->alpha, now);
  double x = tween_value(&c->x, now);
  tween_retarget(&c->alpha, 0, ms * a, now);
  tween_retarget(&c->x, x - dir * drift * a, ms * a, now);
}

static inline bool wsfade_done(const struct wsfade_curve *c, double now) {
  return tween_progress(&c->alpha, now) >= 1;
}

#endif
