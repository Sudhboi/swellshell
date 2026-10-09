/* overview: enlarged preview of the window under the pointer. After
 * zoom_delay_ms the hovered window grows (zoom_anim_ms, ease-out) and
 * shrinks back the same way when left. */
#ifndef SWELL_OVERVIEW_ZOOM_H
#define SWELL_OVERVIEW_ZOOM_H

#include "overview.h"

/* Call whenever ov->hover changes. */
void ov_zoom_hover_changed(struct ov *ov);
/* Recompute the preview box after a relayout; drops it if the window went
 * away. */
void ov_zoom_relayout(struct ov *ov);
/* Start the preview once the pointer has rested long enough; keeps
 * repainting while it animates. */
void ov_zoom_tick(struct ov *ov, int64_t now);
/* Milliseconds until ov_zoom_tick has work to do, or -1. */
int ov_zoom_timeout(struct ov *ov, int64_t now);

/* The preview box at time now, if one is shown on out. */
bool ov_zoom_box(struct ov *ov, struct output *out, int64_t now,
                 struct box *box);
/* Whether (x, y) on out is inside the fully grown preview. */
bool ov_zoom_contains(struct ov *ov, struct output *out, double x, double y);

#endif
