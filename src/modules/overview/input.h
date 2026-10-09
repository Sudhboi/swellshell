/* overview: seat input (click, drag, middle-click kill, keyboard navigation
 * with key repeat), routed by the core per overlay surface. */
#ifndef SWELL_OVERVIEW_INPUT_H
#define SWELL_OVERVIEW_INPUT_H

#include "overview.h"

/* Register with wl_set_input(wl, out->surface, &ov_input_handler, out). */
extern const struct swell_input ov_input_handler;

/* The key-repeat timer callback (data is the struct ov). */
void ov_input_repeat(struct loop_timer *t, void *data);

/* Recompute what is under the pointer. prev_con is the con_id that was
 * hovered before (-1 for none); the preview resets if it changed. */
void ov_input_update_hover(struct ov *ov, long prev_con);

/* Focus the target in sway and close the overview. */
void ov_input_activate(struct ov *ov, struct target t);

#endif
