/* Cairo helpers shared by the render code. Colours are 0xRRGGBBAA. */
#ifndef SWELL_DRAW_H
#define SWELL_DRAW_H

#include <cairo.h>
#include <math.h>
#include <stdint.h>

#include "config.h"

/* Set the cairo source from c, its alpha times alpha. */
static inline void set_rgba(cairo_t *cr, uint32_t c, double alpha) {
  cairo_set_source_rgba(cr, col_r(c), col_g(c), col_b(c), col_a(c) * alpha);
}

static inline void add_stop(cairo_pattern_t *p, double off, uint32_t c) {
  cairo_pattern_add_color_stop_rgba(p, off, col_r(c), col_g(c), col_b(c),
                                    col_a(c));
}

/* Blend two colours per channel, t = 0..1 from a to b. */
static inline uint32_t col_mix(uint32_t a, uint32_t b, double t) {
  uint32_t out = 0;
  for (int sh = 0; sh < 32; sh += 8) {
    double ca = (a >> sh) & 0xff, cb = (b >> sh) & 0xff;
    out |= (uint32_t)(ca + (cb - ca) * t + 0.5) << sh;
  }
  return out;
}

/* A rounded rectangle sub-path with corner radius r (not clamped: callers
 * keep r within half of w and h). */
static inline void rounded_rect(cairo_t *cr, double x, double y, double w,
                                double h, double r) {
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
  cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
  cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
  cairo_close_path(cr);
}

#endif
