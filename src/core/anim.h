/* Tween helpers shared by the animated modules. */
#ifndef SWELL_ANIM_H
#define SWELL_ANIM_H

#include <stdbool.h>
#include <stdint.h>

/* Ease-out cubic. */
static inline double ease_out_cubic(double t) {
  if (t <= 0)
    return 0;
  if (t >= 1)
    return 1;
  t = 1 - t;
  return 1 - t * t * t;
}

/* Linear progress 0..1 of an animation of duration ms started at t0; sets
 * *more when it is unfinished. */
static inline double anim_progress(int64_t t0, int ms, int64_t now,
                                   bool *more) {
  if (ms <= 0)
    return 1;
  double t = (double)(now - t0) / ms;
  if (t >= 1)
    return 1;
  if (more)
    *more = true;
  return t < 0 ? 0 : t;
}

#endif
