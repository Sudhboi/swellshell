/* The Nix snowflake: six lambdas, alternating two shades. Used by the osd
 * and the wallpaper. */
#ifndef SWELL_NIXLOGO_H
#define SWELL_NIXLOGO_H

#include <cairo.h>
#include <stdint.h>

/* Draw the snowflake centred at (cx, cy) with outer radius r, rotated by
 * angle radians (clockwise, y down). The lambdas alternate between dark and
 * light (0xRRGGBBAA). alpha multiplies opacity. */
void nixlogo_draw(cairo_t *cr, double cx, double cy, double r, double angle,
                  uint32_t dark, uint32_t light, double alpha);
/* The same, with lambda k (0..5, k = 0 dark) moved slide[k] x r outwards
 * along its own direction from the centre. slide may be NULL. */
void nixlogo_draw_ex(cairo_t *cr, double cx, double cy, double r,
                     double angle, uint32_t dark, uint32_t light, double alpha,
                     const double *slide);
/* The same, with lambda k in cols[k] (k goes clockwise). */
void nixlogo_draw_cols(cairo_t *cr, double cx, double cy, double r,
                       double angle, const uint32_t cols[6], double alpha,
                       const double *slide);

#endif
