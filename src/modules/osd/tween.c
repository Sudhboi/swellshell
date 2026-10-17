#include "tween.h"

#include <math.h>

#include "anim.h"

void osd_anim_event(struct osd_anim *a, const struct osd_config *c,
                    const struct osd_event *ev, double now) {
  a->kind = ev->kind;
  a->pct = ev->level;
  a->muted = ev->muted;
  tween_retarget(&a->level, ev->level / 100.0, c->tween_ms, now);
  if (a->shown.to < 1.0)
    tween_retarget(&a->shown, 1.0, c->slide_ms, now);
}

void osd_anim_retract(struct osd_anim *a, const struct osd_config *c,
                      double now) {
  tween_retarget(&a->shown, 0.0, c->slide_ms, now);
}

bool osd_anim_sample(const struct osd_anim *a, const struct osd_config *c,
                     double now, struct osd_render_state *rs) {
  rs->kind = a->kind;
  rs->pct = a->pct;
  rs->muted = a->muted;
  rs->level = tween_value(&a->level, now);
  rs->slide = tween_value(&a->shown, now);
  rs->alpha = rs->slide > 0.0 ? 1.0 : 0.0;
  /* Above 100% (and not muted), pulse red; keeps frames coming while shown. */
  bool pulsing = a->pct > 100 && rs->level > 1.0 &&
                 !(a->kind == OSD_VOLUME && a->muted) && c->pulse_ms > 0;
  rs->pulse = pulsing ? 0.5 - 0.5 * cos(2 * M_PI * now / c->pulse_ms) : 0.0;
  return pulsing || tween_progress(&a->level, now) < 1.0 ||
         tween_progress(&a->shown, now) < 1.0;
}

bool osd_anim_hidden(const struct osd_anim *a, double now) {
  return a->shown.to == 0.0 && tween_progress(&a->shown, now) >= 1.0;
}

int osd_anim_retract_left(const struct osd_anim *a, double now) {
  if (a->shown.to != 0.0)
    return -1;
  double left = a->shown.t0 + a->shown.dur - now;
  return left > 0 ? (int)ceil(left) : 0;
}
