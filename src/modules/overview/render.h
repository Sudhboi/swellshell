/* overview: drawing an output's overview with cairo/pango. */
#ifndef SWELL_OVERVIEW_RENDER_H
#define SWELL_OVERVIEW_RENDER_H

#include <cairo.h>

#include "overview.h"

/* cr must already be scaled so that one unit is one logical pixel; scale is
 * the logical-to-device factor (used to size thumbnails). */
void ov_render_output(struct ov *ov, struct output *out, cairo_t *cr,
                      double scale);
/* The background alone (the wallpaper's map if it can paint one, and the
 * scrim), same cr convention, painted over whatever cr holds. */
/* Draw the workspace names once at scale off screen, so the first zoom
 * frames don't rasterise their glyphs (the slowest of them). */
void ov_render_warm(struct ov *ov, struct output *out, double scale);
void ov_render_background(struct ov *ov, struct output *out, cairo_t *cr,
                          double scale);

#endif
