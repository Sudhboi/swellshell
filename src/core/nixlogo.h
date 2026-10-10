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

#endif
