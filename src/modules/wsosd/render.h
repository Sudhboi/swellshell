/* Cairo drawing of one workspace OSD frame. */
#ifndef SWELL_WSOSD_RENDER_H
#define SWELL_WSOSD_RENDER_H

#include <cairo.h>

#include "wsosd.h"

struct wsosd_render_state {
  int width;    /* surface width; the panel is centred in it */
  double count; /* workspaces (tweened); a fractional last slot shrinks */
  double pos;   /* focused slot, 0-based (tweened); the bar sits here */
  double slide; /* drop-down position, 0 above the top edge .. 1 in place */
};

/* Draw the whole layer surface (width x height + top_gap). The caller has
 * already applied the output scale and cleared the surface. */
void wsosd_render_surface(cairo_t *cr, const struct wsosd_config *c,
                          const struct wsosd_render_state *s);

#endif
