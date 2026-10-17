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

/* A value eased (ease-out cubic) from `from` to `to` over dur ms from t0.
 * Times are now_fms() milliseconds. */
struct tween {
  double from, to, t0, dur;
};

/* Linear progress 0..1 of tw at now. */
static inline double tween_progress(const struct tween *tw, double now) {
  if (tw->dur <= 0 || now >= tw->t0 + tw->dur)
    return 1.0;
  return now <= tw->t0 ? 0.0 : (now - tw->t0) / tw->dur;
}

static inline double tween_value(const struct tween *tw, double now) {
  return tw->from +
         (tw->to - tw->from) * ease_out_cubic(tween_progress(tw, now));
}

/* Head for `to` over dur ms, starting from wherever tw is at now. */
static inline void tween_retarget(struct tween *tw, double to, double dur,
                                  double now) {
  tw->from = tween_value(tw, now);
  tw->to = to;
  tw->t0 = now;
  tw->dur = dur;
}

#endif
