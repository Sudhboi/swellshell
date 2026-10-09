#include "tween.h"

#include <math.h>

#include "anim.h"

static const double PI = 3.14159265358979323846;

static double progress(const struct tween *tw, double now) {
  if (tw->dur <= 0 || now >= tw->t0 + tw->dur)
    return 1.0;
  return now <= tw->t0 ? 0.0 : (now - tw->t0) / tw->dur;
}

static double tween_value(const struct tween *tw, double now) {
  return tw->from + (tw->to - tw->from) * ease_out_cubic(progress(tw, now));
}

static void retarget(struct tween *tw, double to, double dur, double now) {
  tw->from = tween_value(tw, now);
  tw->to = to;
  tw->t0 = now;
  tw->dur = dur;
}

void osd_anim_event(struct osd_anim *a, const struct osd_config *c,
                    const struct osd_event *ev, double now) {
  a->kind = ev->kind;
  a->pct = ev->level;
  a->muted = ev->muted;
  retarget(&a->level, ev->level / 100.0, c->tween_ms, now);
  if (a->shown.to < 1.0)
    retarget(&a->shown, 1.0, c->slide_ms, now);
}

void osd_anim_retract(struct osd_anim *a, const struct osd_config *c,
                      double now) {
  retarget(&a->shown, 0.0, c->slide_ms, now);
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
  rs->pulse = pulsing ? 0.5 - 0.5 * cos(2 * PI * now / c->pulse_ms) : 0.0;
  return pulsing || progress(&a->level, now) < 1.0 ||
         progress(&a->shown, now) < 1.0;
}

bool osd_anim_hidden(const struct osd_anim *a, double now) {
  return a->shown.to == 0.0 && progress(&a->shown, now) >= 1.0;
}

int osd_anim_retract_left(const struct osd_anim *a, double now) {
  if (a->shown.to != 0.0)
    return -1;
  double left = a->shown.t0 + a->shown.dur - now;
  return left > 0 ? (int)ceil(left) : 0;
}
