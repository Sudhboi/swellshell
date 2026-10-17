/* Shared by the cairo drawing files (render.c, render_logo.c, render_hex.c,
 * render_viz.c); the rest of the module uses wallpaper.h. */
#ifndef SWELL_WALLPAPER_RENDER_H
#define SWELL_WALLPAPER_RENDER_H

#include <cairo.h>
#include <math.h>
#include <stdint.h>

#include "draw.h"
#include "wallpaper.h"

/* The snowflake's hexagonal hole: its corners sit at the lambdas' inner
 * points, this far out (x the radius) and at this angle. */
#define HOLE 0.39046
#define HOLE_ANGLE 3.08629 /* atan2(0.02158, -0.38986) */

/* The outermost speed line ring (x logo radius). */
#define STREAK_OUTER 1.24

static inline void pill(cairo_t *cr, double x, double y, double w, double h) {
  rounded_rect(cr, x, y, w, h, fmin(w, h) / 2.0);
}

/* a + (b - a) * t per channel, t in 0..1. */
static inline uint32_t mix(uint32_t a, uint32_t b, double t) {
  uint32_t out = 0;
  for (int sh = 0; sh < 32; sh += 8) {
    double ca = (a >> sh) & 0xff, cb = (b >> sh) & 0xff;
    out |= (uint32_t)lround(ca + (cb - ca) * t) << sh;
  }
  return out;
}

/* render.c */
/* The box x0..x1, y0..y1 grown by a pixel and cut to the buffer. */
struct wp_rect wp_clamp_rect(double x0, double y0, double x1, double y1, int pw,
                             int ph);
/* The strip side s (0 left, 1 right) of the edge bars fills at full. */
void wp_edge_box(const struct wallpaper_config *c, int pw, int ph, int s,
                 double *x0, double *y0, double *x1, double *y1);

/* render_logo.c */
/* Three box blurs of radius b over an A8 image: close to a Gaussian. */
void wp_blur_a8(unsigned char *d, int w, int h, int stride, int b);
/* The glass snowflake's outline colours, lit towards white by its glow. */
void wp_topo_edges(const struct wallpaper_config *c, const struct wp_frame *f,
                   uint32_t edge[3]);
/* The outline's gradient from cx - e, cy - e to cx + e, cy + e. */
cairo_pattern_t *wp_topo_edge_pattern(const uint32_t edge[3], double cx,
                                      double cy, double e, double a,
                                      bool solid);
void wp_paint_bloom(cairo_t *cr, const struct wallpaper_config *c, int pw,
                    int ph, double r, double over);
void wp_draw_streaks(cairo_t *cr, const struct wallpaper_config *c, double cx,
                     double cy, double r, double k, const struct wp_frame *f);
void wp_draw_glass_logo(cairo_t *cr, const struct wallpaper_config *c,
                        double cx, double cy, double r0, double r, double k,
                        const struct wp_frame *f);
void wp_render_logo_finish(void);

/* render_hex.c */
void wp_draw_hex(cairo_t *cr, const struct wallpaper_config *c, double cx,
                 double cy, double r, double rp, const struct wp_frame *f);
void wp_render_hex_finish(void);

/* render_viz.c */
/* The farthest any ring bar reaches, px. */
double wp_ring_outer(const struct wallpaper_config *c, double r, double k);
void wp_draw_ring(cairo_t *cr, const struct wallpaper_config *c, int pw, int ph,
                  double r, double k, const struct wp_frame *f);
void wp_draw_edges(cairo_t *cr, const struct wallpaper_config *c, int pw,
                   int ph, const struct wp_frame *f);
void wp_draw_edges_smooth(cairo_t *cr, const struct wallpaper_config *c, int pw,
                          int ph, const struct wp_frame *f);
void wp_render_viz_finish(void);

#endif
