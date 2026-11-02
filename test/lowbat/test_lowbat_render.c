/* Offscreen render test: draws low battery panel states to PNGs at scale 2.
 * Usage: test_lowbat_render [outdir]   (default: current directory) */
#include <assert.h>
#include <stdio.h>

#include "../png.h"
#include "lowbat/render.h"
#include "module.h"

extern const struct swell_module lowbat_module;

static cairo_t *draw(const struct lowbat_config *c, const char *dir,
                     const char *name, int pct, bool critical, double slide,
                     double pulse) {
  struct lowbat_render_state rs = {
      .pct = pct, .critical = critical, .slide = slide, .pulse = pulse};
  cairo_t *cr = canvas(c->width, lowbat_surface_height(c));
  lowbat_render_surface(cr, c, &rs);
  save_png(cr, dir, name);
  return cr;
}

static bool reddish(uint32_t px) {
  int r = px >> 24, g = (px >> 16) & 0xff, b = (px >> 8) & 0xff;
  return r > 0x90 && g < 0x60 && b < 0x60;
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : ".";
  const struct lowbat_config *c = lowbat_module.defaults;
  double cy = c->top_gap + c->height / 2.0;
  /* Inside the battery body: its left end (filled) and right end (track). */
  double fill_x = c->pad + 6, track_x = c->pad + c->body_width - 7;

  cairo_t *cr = draw(c, dir, "lowbat_low_14", 14, false, 1, 1);
  assert(pixel(cr, c->width / 2.0, c->top_gap / 2.0) == 0);
  assert(pixel(cr, 0.5, cy) == c->border);
  assert(pixel(cr, c->width - 6, c->top_gap + 4) == c->background);
  assert(reddish(pixel(cr, fill_x, cy)));
  assert(pixel(cr, track_x, cy) == c->track);
  cairo_destroy(cr);

  cr = draw(c, dir, "lowbat_critical_5", 5, true, 1, 1);
  assert(reddish(pixel(cr, fill_x, cy)));
  assert(pixel(cr, track_x, cy) == c->track);
  cairo_destroy(cr);

  /* Between pulses the outline is the text colour. */
  cr = draw(c, dir, "lowbat_low_dim", 14, false, 1, 0);
  assert(pixel(cr, c->pad + c->body_width / 2.0,
               cy - c->body_height / 2.0 + 1) == c->text);
  cairo_destroy(cr);

  /* Half slid: the bottom of the panel's place is still empty. */
  cr = draw(c, dir, "lowbat_slide_half", 14, false, 0.5, 1);
  assert(pixel(cr, c->width / 2.0, lowbat_surface_height(c) - 2) == 0);
  cairo_destroy(cr);

  /* Hidden: nothing drawn. */
  cr = draw(c, dir, "lowbat_hidden", 14, false, 0, 1);
  assert(pixel(cr, c->width / 2.0, cy) == 0);
  cairo_destroy(cr);
  return 0;
}
