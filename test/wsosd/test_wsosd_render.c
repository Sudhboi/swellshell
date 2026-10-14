/* Offscreen render test: draws workspace OSD states to PNGs at scale 2.
 * Usage: test_wsosd_render [outdir]   (default: current directory) */
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "../png.h"
#include "module.h"
#include "wsosd/render.h"

extern const struct swell_module wsosd_module;

static int surface_width(const struct wsosd_config *c) {
  return (int)ceil(wsosd_panel_width(c, c->max_dots));
}

static cairo_t *draw(const struct wsosd_config *c, const char *dir,
                     const char *name, double count, double pos,
                     double slide) {
  struct wsosd_render_state rs = {.width = surface_width(c),
                                  .count = count,
                                  .pos = pos,
                                  .slide = slide};
  cairo_t *cr = canvas(rs.width, wsosd_surface_height(c));
  wsosd_render_surface(cr, c, &rs);
  save_png(cr, dir, name);
  return cr;
}

/* Centre of slot k with the bar fully on slot active (counts n). */
static double slot_x(const struct wsosd_config *c, int width, int n,
                     int active, int k) {
  double pw = round(wsosd_panel_width(c, n));
  double x = round((width - pw) / 2) + c->pad;
  for (int i = 0; i < k; ++i)
    x += (i == active ? c->active_width : c->dot) + c->dot_gap;
  return x + (k == active ? c->active_width : c->dot) / 2.0;
}

static bool greyish_light(uint32_t px) {
  return (px & 0xff) == 0xff && (px >> 24) >= 0xa0;
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : ".";
  const struct wsosd_config *c = wsosd_module.defaults;
  int width = surface_width(c);
  double cy = c->top_gap + c->height / 2.0;

  cairo_t *cr = draw(c, dir, "wsosd_5_at_3", 5, 2, 1);
  /* The gap above the panel and the sides beside it are empty. */
  assert(pixel(cr, width / 2.0, c->top_gap / 2.0) == 0);
  assert(pixel(cr, 2, cy) == 0);
  double pw = round(wsosd_panel_width(c, 5));
  double px = round((width - pw) / 2);
  assert(pixel(cr, px + 0.5, cy) == c->border);
  assert(pixel(cr, px + 4, c->top_gap + 4) == c->background);
  /* Other workspaces are dots, the focused one a light bar. */
  assert(pixel(cr, slot_x(c, width, 5, 2, 0), cy) == c->dot_color);
  assert(pixel(cr, slot_x(c, width, 5, 2, 4), cy) == c->dot_color);
  assert(greyish_light(pixel(cr, slot_x(c, width, 5, 2, 2), cy)));
  /* Between dots is background. */
  assert(pixel(cr, slot_x(c, width, 5, 2, 0) + c->dot / 2.0 + c->dot_gap / 2.0,
               cy) == c->background);
  cairo_destroy(cr);

  cairo_destroy(draw(c, dir, "wsosd_1", 1, 0, 1));
  cairo_destroy(draw(c, dir, "wsosd_3_at_1", 3, 0, 1));

  cr = draw(c, dir, "wsosd_10_at_10", 10, 9, 1);
  /* At max_dots the panel fills the surface. */
  assert(pixel(cr, 0.5, cy) == c->border);
  assert(greyish_light(pixel(cr, slot_x(c, width, 10, 9, 9), cy)));
  cairo_destroy(cr);

  /* Half-way between slots 2 and 3 both are half-stretched and lit. */
  cr = draw(c, dir, "wsosd_5_moving", 5, 2.5, 1);
  assert(pixel(cr, slot_x(c, width, 5, 2, 0), cy) == c->dot_color);
  assert(pixel(cr, slot_x(c, width, 5, 2, 4), cy) == c->dot_color);
  cairo_destroy(cr);

  /* A sixth workspace growing in. */
  cairo_destroy(draw(c, dir, "wsosd_5_growing", 5.5, 5, 1));

  /* Half-way down the panel's bottom is at top_gap - surface_h/2 + height;
   * below that it is empty. */
  cr = draw(c, dir, "wsosd_slide_half", 5, 2, 0.5);
  double bottom = c->top_gap - wsosd_surface_height(c) / 2.0 + c->height;
  assert(pixel(cr, width / 2.0, bottom + 2) == 0);
  assert(pixel(cr, width / 2.0, bottom - 0.25) == c->border);
  cairo_destroy(cr);

  /* Hidden draws nothing. */
  cr = draw(c, dir, "wsosd_hidden", 5, 2, 0);
  assert(pixel(cr, width / 2.0, cy) == 0);
  cairo_destroy(cr);
  return 0;
}
