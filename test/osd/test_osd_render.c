/* Offscreen render test: draws OSD states to PNGs at scale 2.
 * Usage: test_osd_render [outdir]   (default: current directory) */
#include <assert.h>
#include <stdio.h>

#include "../png.h"
#include "module.h"
#include "osd/render.h"

extern const struct swell_module osd_module;

static cairo_t *draw(const struct osd_config *c, const char *dir,
                     const char *name, struct osd_render_state rs) {
  cairo_t *cr = canvas(c->width, osd_surface_height(c));
  osd_render_surface(cr, c, &rs);
  save_png(cr, dir, name);
  return cr;
}

static struct osd_render_state vol(int pct) {
  return (struct osd_render_state){.kind = OSD_VOLUME, .level = pct / 100.0,
                                   .pct = pct, .alpha = 1, .slide = 1};
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : ".";
  const struct osd_config *c = osd_module.defaults;

  cairo_t *cr = draw(c, dir, "osd_volume_40", vol(40));
  /* The gap above the panel is empty; the panel is the background colour
   * with its border. */
  assert(pixel(cr, c->width / 2.0, c->top_gap / 2.0) == 0);
  assert(pixel(cr, 4, c->top_gap + 4) == c->background);
  assert(pixel(cr, 0.5, c->top_gap + 100) == c->border);
  assert(image_width(cr) == c->width * SCALE);
  cairo_destroy(cr);

  struct osd_render_state rs = vol(75);
  rs.muted = true;
  cairo_destroy(draw(c, dir, "osd_volume_75_muted", rs));

  rs = vol(80);
  rs.kind = OSD_BRIGHTNESS;
  cairo_destroy(draw(c, dir, "osd_brightness_80", rs));

  rs = vol(10);
  cairo_destroy(draw(c, dir, "osd_volume_10", rs));

  rs = vol(150);
  rs.pulse = 1;
  cairo_destroy(draw(c, dir, "osd_volume_150_pulse", rs));
  rs.pulse = 0;
  cairo_destroy(draw(c, dir, "osd_volume_150", rs));

  /* Half-way through the drop-down the panel's bottom is at
   * top_gap - surface_h/2 + height. */
  rs = vol(40);
  rs.slide = 0.5;
  cr = draw(c, dir, "osd_slide_half", rs);
  double bottom = c->top_gap - osd_surface_height(c) / 2.0 + c->height;
  assert(pixel(cr, 4, bottom - 4) == c->background);
  assert(pixel(cr, 4, bottom + 4) == 0);
  cairo_destroy(cr);

  /* Hidden: nothing is drawn. */
  rs.slide = 0;
  rs.alpha = 0;
  cr = draw(c, dir, "osd_hidden", rs);
  assert(pixel(cr, c->width / 2.0, osd_surface_height(c) / 2.0) == 0);
  cairo_destroy(cr);

  printf("osd render: ok\n");
  return 0;
}
