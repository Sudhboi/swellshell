/* Offscreen render test of the wallpaper at 2880x1800 (the reference
 * size), plus the pulse shape and the partial-repaint regions.
 * Usage: test_wallpaper_render [outdir]   (default: current directory) */
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "module.h"
#include "wallpaper/wallpaper.h"

extern const struct swell_module wallpaper_module;

#define W 2880
#define H 1800

static uint32_t px(cairo_surface_t *s, int x, int y) {
  cairo_surface_flush(s);
  const uint32_t *row = (const uint32_t *)(cairo_image_surface_get_data(s) +
                                           y * cairo_image_surface_get_stride(s));
  return row[x] & 0xffffff;
}

static cairo_surface_t *render(const struct wallpaper_config *c,
                               struct wp_frame f, const char *dir,
                               const char *name) {
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W, H);
  cairo_t *cr = cairo_create(s);
  wp_render(cr, c, W, H, &f, true, false);
  cairo_destroy(cr);
  if (name) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.png", dir, name);
    assert(cairo_surface_write_to_png(s, path) == CAIRO_STATUS_SUCCESS);
  }
  return s;
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : ".";
  const struct wallpaper_config *c = wallpaper_module.defaults;

  /* The script's pulse: resting at the start, full for the hold around
   * the middle, back to resting at the end. */
  assert(fabs(wp_bar_level(c, 0) - c->bar_min) < 0.01);
  assert(wp_bar_level(c, 0.5) == 1);
  assert(wp_bar_level(c, 0.5 + c->pulse_hold_s / 2 / c->pulse_period_s) == 1);
  assert(wp_bar_level(c, 0.25) < wp_bar_level(c, 0.4));
  assert(fabs(wp_bar_level(c, 0.3) - wp_bar_level(c, 0.7)) < 1e-9);
  assert(wp_pulse_period(c, 50) == c->pulse_period_s);
  assert(wp_pulse_period(c, 100) == 2 * c->pulse_period_s);
  assert(wp_pulse_period(c, 0) == c->pulse_period_min_s);

  /* Regions: the bar's outline box and the logo tile, inside the buffer. */
  struct wp_rect r[2];
  wp_regions(c, W, H, false, r);
  assert(r[0].x <= 1136 && r[0].x + r[0].w >= 1744);
  assert(r[0].y <= 6 && r[0].y + r[0].h >= 24);
  assert(r[1].x <= 1440 - 225 && r[1].x + r[1].w >= 1440 + 225);

  struct wp_frame f = {.percent = 50, .level = 1, .angle = 0};
  cairo_surface_t *s = render(c, f, dir, "wallpaper_50_peak");
  assert(px(s, 1300, 15) == 0xffffff);       /* filled half */
  assert(px(s, 1600, 15) == 0x000000);       /* empty half, inside the gap */
  assert(px(s, 1600, 7) == 0xffffff);        /* outline */
  assert(px(s, 10, 10) == 0x000000);         /* background */
  assert(px(s, 1440, 900) == 0x000000);      /* the snowflake's hole */
  cairo_surface_destroy(s);

  f = (struct wp_frame){.percent = 100, .level = c->bar_min, .angle = 0};
  cairo_surface_destroy(render(c, f, dir, "wallpaper_100_rest"));
  f = (struct wp_frame){.percent = 3, .level = 1, .angle = M_PI / 7};
  cairo_surface_destroy(render(c, f, dir, "wallpaper_3_turned"));

  /* The overspeed bloom, speed lines and glow change only the logo tile. */
  f = (struct wp_frame){.percent = 50, .level = 1, .angle = 0};
  cairo_surface_t *a = render(c, f, dir, NULL);
  f.glow = f.over = 1;
  f.dir = 1;
  cairo_surface_t *t = render(c, f, dir, "wallpaper_overspeed");
  int changed = 0;
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3)
      if (px(a, x, y) != px(t, x, y)) {
        assert(x >= r[1].x && x < r[1].x + r[1].w);
        assert(y >= r[1].y && y < r[1].y + r[1].h);
        changed++;
      }
  assert(changed > 0);
  cairo_surface_destroy(a);
  cairo_surface_destroy(t);

  /* A partial repaint over an older frame equals a full repaint. */
  a = render(c, (struct wp_frame){80, 0.4, 1.0}, dir, NULL);
  cairo_t *cr = cairo_create(a);
  struct wp_frame g = {20, 0.9, 2.0, .glow = 0.5, .over = 0.7, .dir = 1};
  wp_render(cr, c, W, H, &g, false, false);
  cairo_destroy(cr);
  cairo_surface_t *b = render(c, g, dir, NULL);
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3)
      assert(px(a, x, y) == px(b, x, y));
  cairo_surface_destroy(a);
  cairo_surface_destroy(b);

  /* The visualiser ring: outside the small logo tile, inside the ring's. */
  struct wp_rect big[2];
  wp_regions(c, W, H, true, big);
  assert(big[1].w > r[1].w && big[1].h > r[1].h);
  assert(big[1].y >= 0 && big[1].y + big[1].h <= H);
  f = (struct wp_frame){.percent = 50, .level = 1, .angle = 0};
  a = render(c, f, dir, NULL);
  f.nbars = c->viz_bars;
  for (int i = 0; i < f.nbars; ++i)
    f.bars[i] = 0.5f + 0.5f * (float)sin(i * 0.37);
  t = render(c, f, dir, "wallpaper_viz");
  int outside_small = 0;
  changed = 0;
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3)
      if (px(a, x, y) != px(t, x, y)) {
        assert(x >= big[1].x && x < big[1].x + big[1].w);
        assert(y >= big[1].y && y < big[1].y + big[1].h);
        outside_small += x < r[1].x || x >= r[1].x + r[1].w || y < r[1].y ||
                         y >= r[1].y + r[1].h;
        changed++;
      }
  assert(changed > 0 && outside_small > 0);
  /* Bass at the bottom, mirrored: the bottom bars sit just below the logo
   * on either side of the vertical, and the ring is left/right symmetric. */
  double k = W / WP_REF_WIDTH;
  int below = (int)(H / 2 + c->logo_radius * k * c->viz_inner + 20);
  int off = (int)(M_PI * 0.5 / c->viz_bars * c->logo_radius * k *
                  c->viz_inner);
  assert(px(t, W / 2 + off, below) != 0 && px(t, W / 2 - off, below) != 0);
  cairo_surface_destroy(a);

  /* A partial repaint with the ring (over a frame with other bars) equals a
   * full repaint, and so does clearing the bars with the ring's area. */
  a = render(c, f, dir, NULL);
  for (int i = 0; i < f.nbars; ++i)
    f.bars[i] = 1 - f.bars[i];
  cr = cairo_create(a);
  wp_render(cr, c, W, H, &f, false, false);
  cairo_destroy(cr);
  b = render(c, f, dir, NULL);
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3)
      assert(px(a, x, y) == px(b, x, y));
  cairo_surface_destroy(b);
  f.nbars = 0;
  cr = cairo_create(a);
  wp_render(cr, c, W, H, &f, false, true);
  cairo_destroy(cr);
  b = render(c, f, dir, NULL);
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3)
      assert(px(a, x, y) == px(b, x, y));
  cairo_surface_destroy(a);
  cairo_surface_destroy(b);
  cairo_surface_destroy(t);
  return 0;
}
