/* Cairo drawing of one low battery panel frame. */
#ifndef SWELL_LOWBAT_RENDER_H
#define SWELL_LOWBAT_RENDER_H

#include <cairo.h>
#include <stdbool.h>

#include "lowbat.h"

struct lowbat_render_state {
  int pct;       /* battery level shown */
  bool critical; /* adds "plug in" */
  double slide;  /* drop-down position, 0 above the top edge .. 1 in place */
  double pulse;  /* 0 .. 1: how far the fill and text are lit red */
};

/* Draw the whole layer surface (width x height + top_gap). The caller has
 * already applied the output scale and cleared the surface. */
void lowbat_render_surface(cairo_t *cr, const struct lowbat_config *c,
                           const struct lowbat_render_state *s);

#endif
