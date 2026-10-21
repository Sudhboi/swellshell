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
 * along its own direction from the centre, and faded by fade[k] (0: opaque,
 * 1: gone). slide and fade may be NULL. */
void nixlogo_draw_ex(cairo_t *cr, double cx, double cy, double r,
                     double angle, uint32_t dark, uint32_t light, double alpha,
                     const double *slide, const double *fade);

/* Append lambda k's outline (k = 0 dark) to the path, in snowflake units:
 * centred on the origin, outer radius 1, unrotated, moved slide outwards
 * along its own direction. */
void nixlogo_lambda_path(cairo_t *cr, int k, double slide);
/* The unrotated lambda 0's outward direction, a unit vector: slide moves
 * lambda k along it turned by k x 60 degrees. */
void nixlogo_lambda_dir(double *dx, double *dy);
/* One lambda's outline's length at outer radius 1. */
double nixlogo_lambda_length(void);

#endif
