/* Cairo drawing of one OSD frame. */
#ifndef SWELL_OSD_RENDER_H
#define SWELL_OSD_RENDER_H

#include <cairo.h>
#include <stdbool.h>

#include "osd.h"

struct osd_render_state {
  enum osd_kind kind;
  double level; /* displayed level 0..max_pct/100 (tweened); ring fill and
                   logo angle = level * 2pi */
  int pct;      /* target percentage, shown as text */
  bool muted;
  double alpha; /* overall opacity 0..1 */
  double slide; /* drop-down position, 0 above the top edge .. 1 in place */
  double pulse; /* red pulse 0..1, nonzero only above 100% */
};

/* Draw the panel in logical coordinates (width x height). The caller has
 * already applied the output scale and cleared the surface. */
void osd_render_frame(cairo_t *cr, const struct osd_config *c,
                      const struct osd_render_state *s);

/* Draw the whole layer surface (width x height + top_gap): the panel
 * slides down from above the top edge into the gap below it. */
void osd_render_surface(cairo_t *cr, const struct osd_config *c,
                        const struct osd_render_state *s);

#endif
