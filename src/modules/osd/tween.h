/* Time-based tweening of the OSD level and drop-down. */
#ifndef SWELL_OSD_TWEEN_H
#define SWELL_OSD_TWEEN_H

#include <stdbool.h>

#include "osd.h"
#include "render.h"

struct tween {
  double from, to, t0, dur;
};

struct osd_anim {
  struct tween level;
  struct tween shown; /* 0 hidden .. 1 dropped down */
  enum osd_kind kind;
  int pct;
  bool muted;
};

/* Retarget toward a new event (drops down if needed). */
void osd_anim_event(struct osd_anim *a, const struct osd_config *c,
                    const struct osd_event *ev, double now);
/* Start retracting upward. */
void osd_anim_retract(struct osd_anim *a, const struct osd_config *c,
                      double now);
/* Fill rs for time now. Returns true while anything is still moving. */
bool osd_anim_sample(const struct osd_anim *a, const struct osd_config *c,
                     double now, struct osd_render_state *rs);
/* True once fully retracted. */
bool osd_anim_hidden(const struct osd_anim *a, double now);
/* ms until the retract ends (0 if done, -1 if not retracting). */
int osd_anim_retract_left(const struct osd_anim *a, double now);

#endif
