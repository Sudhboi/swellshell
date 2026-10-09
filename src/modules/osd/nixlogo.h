/* The Nix snowflake: six lambdas, alternating two shades. */
#ifndef SWELL_OSD_NIXLOGO_H
#define SWELL_OSD_NIXLOGO_H

#include <cairo.h>
#include <stdbool.h>

#include "osd.h"

/* Draw the snowflake centred at (cx, cy) with outer radius r, rotated by
 * angle radians. grey draws it in c->muted. alpha multiplies opacity. */
void nixlogo_draw(cairo_t *cr, const struct osd_config *c, double cx,
                  double cy, double r, double angle, bool grey, double alpha);

#endif
