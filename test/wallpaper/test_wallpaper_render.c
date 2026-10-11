/* Offscreen render test of the wallpaper at 2880x1800 (the reference
 * size), plus the pulse shape and the partial-repaint regions.
 * Usage: test_wallpaper_render [outdir]   (default: current directory) */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
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
  struct wp_rect r[WP_REGIONS];
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

  /* The bar pulses with the logo: at a full pulse it reaches further
   * out, inside its region, and partial repaints over the resting bar
   * equal full ones (shaken too). */
  f = (struct wp_frame){.percent = 50, .level = 1, .angle = 0};
  cairo_surface_t *rest = render(c, f, dir, NULL);
  assert(c->bar_pulse);
  {
    struct wp_rect br[WP_REGIONS];
    wp_regions(c, W, H, false, br);
    int bw = (int)((c->bar_width + 2 * (c->bar_gap + c->bar_border)) * W /
                   WP_REF_WIDTH);
    /* Just inside the pulsed bar's outline, past the resting bar's end. */
    int edge = (int)(W / 2 + bw / 2.0 * (1 + c->react_pulse_scale)) - 2;
    assert(edge > W / 2 + bw / 2 + 2 && px(rest, edge, 15) == 0x000000);
    for (int step = 1; step <= 2; ++step) {
      f.pulse = 1;
      f.shake_y = step == 2 ? c->react_shake_px : 0;
      s = render(c, f, dir, step == 1 ? "wallpaper_bar_pulse" : NULL);
      if (step == 1)
        assert(px(s, edge, 15) != 0x000000);
      for (int y = 0; y < H / 4; ++y)
        for (int x = 0; x < W; x += 2)
          if (px(s, x, y) != px(rest, x, y))
            assert((x >= br[0].x && x < br[0].x + br[0].w &&
                    y >= br[0].y && y < br[0].y + br[0].h) ||
                   (x >= br[1].x && x < br[1].x + br[1].w &&
                    y >= br[1].y && y < br[1].y + br[1].h));
      cairo_surface_t *cp = render(c, (struct wp_frame){.percent = 50,
                                                        .level = 1},
                                   dir, NULL);
      cairo_t *pc = cairo_create(cp);
      wp_render(pc, c, W, H, &f, false, false);
      cairo_destroy(pc);
      for (int y = 0; y < 200; ++y)
        for (int x = 0; x < W; x += 2)
          assert(px(cp, x, y) == px(s, x, y));
      cairo_surface_destroy(cp);
      cairo_surface_destroy(s);
    }
  }
  cairo_surface_destroy(rest);

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

  /* The visualiser ring (viz_edges off): outside the small logo tile,
   * inside the ring's. */
  struct wallpaper_config ring_cfg = *c;
  ring_cfg.viz_edges = false;
  const struct wallpaper_config *rg = &ring_cfg;
  struct wp_rect big[WP_REGIONS];
  wp_regions(rg, W, H, true, big);
  assert(big[1].w > r[1].w && big[1].h > r[1].h);
  assert(big[1].y >= 0 && big[1].y + big[1].h <= H);
  f = (struct wp_frame){.percent = 50, .level = 1, .angle = 0};
  a = render(rg, f, dir, NULL);
  f.nbars = rg->viz_bars;
  for (int i = 0; i < f.nbars; ++i)
    f.bars[0][i] = f.bars[1][i] = 0.5f + 0.5f * (float)sin(i * 0.37);
  t = render(rg, f, dir, "wallpaper_viz");
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
  /* Bass at the bottom: the bottom bars cross the viz_radius line below
   * the logo on either side of the vertical, reaching in and out from it,
   * and nothing is drawn inside where the inner ends can reach. */
  double k = W / WP_REF_WIDTH, rc = rg->logo_radius * k * rg->viz_radius;
  double half = 0.5 * rg->viz_length * k * 0.5; /* the bass bars are >= 0.5 */
  int off = (int)(M_PI * 0.5 / rg->viz_bars * rc);
  for (int d = -1; d <= 1; ++d) {
    int y = (int)(H / 2 + rc + d * half * 0.8 *
                                  (d < 0 ? rg->viz_inward : 1 - rg->viz_inward));
    assert(px(t, W / 2 + off, y) != 0 && px(t, W / 2 - off, y) != 0);
  }
  int inside = (int)(H / 2 + rc - rg->viz_length * k * rg->viz_inward * 1.1);
  assert(px(t, W / 2 + off, inside) == 0);
  /* The same levels on both sides: jitter keeps the halves from mirroring
   * exactly; without it (the default) the ring is perfectly symmetric. */
  struct wallpaper_config jit = *rg;
  jit.viz_jitter = 1;
  cairo_surface_t *p = render(&jit, f, dir, "wallpaper_viz_jitter");
  int asym = 0;
  for (int y = big[1].y; y < big[1].y + big[1].h; y += 2)
    for (int x = W / 2 + 1; x < big[1].x + big[1].w; x += 2)
      asym += px(p, x, y) != px(p, W - 1 - x, y);
  assert(asym > 100);
  int jittered = asym;
  asym = 0;
  for (int y = big[1].y; y < big[1].y + big[1].h; y += 2)
    for (int x = W / 2 + 1; x < big[1].x + big[1].w; x += 2)
      if (y > H / 2 + 300 * k || y < H / 2 - 300 * k) /* not the logo */
        asym += px(t, x, y) != px(t, W - 1 - x, y);
  /* Only edge rasterisation differs. */
  assert(asym * 10 < jittered);
  cairo_surface_destroy(p);
  cairo_surface_destroy(a);

  /* A partial repaint with the ring (over a frame with other bars) equals a
   * full repaint, and so does clearing the bars with the ring's area. */
  a = render(rg, f, dir, NULL);
  for (int i = 0; i < f.nbars; ++i) {
    f.bars[0][i] = 1 - f.bars[0][i];
    f.bars[1][i] = 0.3f;
  }
  cr = cairo_create(a);
  wp_render(cr, rg, W, H, &f, false, false);
  cairo_destroy(cr);
  b = render(rg, f, dir, NULL);
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3)
      assert(px(a, x, y) == px(b, x, y));
  cairo_surface_destroy(b);
  f.nbars = 0;
  cr = cairo_create(a);
  wp_render(cr, rg, W, H, &f, false, true);
  cairo_destroy(cr);
  b = render(rg, f, dir, NULL);
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3)
      assert(px(a, x, y) == px(b, x, y));
  cairo_surface_destroy(a);
  cairo_surface_destroy(b);
  cairo_surface_destroy(t);

  /* The edge bars (the default): the left channel up the left side and the
   * right up the right, growing in from the edge by their level, bass at
   * the bottom; all inside the edge strips, none near the logo. */
  assert(c->viz_edges);
  struct wp_rect e[WP_REGIONS];
  wp_regions(c, W, H, true, e);
  assert(e[1].x == r[1].x && e[1].w == r[1].w); /* no ring around the logo */
  assert(e[2].x == 0 && e[2].w > 0 && e[3].x + e[3].w == W);
  assert(r[2].w == 0 && r[3].w == 0); /* no strips without bars */
  f = (struct wp_frame){.percent = 50, .level = 1, .angle = 0};
  a = render(c, f, dir, NULL);
  f.nbars = c->viz_bars;
  for (int i = 0; i < f.nbars; ++i) {
    f.bars[0][i] = 0.5f;
    f.bars[1][i] = 0.25f;
  }
  t = render(c, f, dir, "wallpaper_viz_edges");
  double ek = W / WP_REF_WIDTH, elen = c->viz_edge_length * ek;
  double eslot = (H - 2 * c->viz_edge_margin * ek) / c->viz_bars;
  int ey = (int)(H - c->viz_edge_margin * ek - eslot / 2); /* the bass bar */
  int gx = (int)(c->viz_edge_gap * ek);
  assert(px(t, gx + 2, ey) != 0 && px(t, (int)(gx + elen * 0.5) - 3, ey) != 0);
  assert(px(t, (int)(gx + elen * 0.5) + 3, ey) == 0);
  assert(px(t, W - 1 - gx - 2, ey) != 0);
  assert(px(t, (int)(W - gx - elen * 0.25) + 3, ey) != 0);
  assert(px(t, (int)(W - gx - elen * 0.25) - 3, ey) == 0);
  /* Smooth (the default): one shape, no gaps between the slots; as bars
   * there are. */
  assert(c->viz_edge_smooth);
  int gaps = 0;
  assert(c->viz_edge_margin == 0);
  for (int y = 0; y < H; ++y) /* the full height, top and bottom rows too */
    gaps += px(t, gx + 2, y) == 0 || px(t, W - 1 - gx - 2, y) == 0;
  assert(gaps == 0);
  /* Shaken up or down, the ends still don't show. */
  for (int d = -1; d <= 1; d += 2) {
    struct wp_frame sf = f;
    sf.shake_y = d * c->react_shake_px;
    cairo_surface_t *st = render(c, sf, dir, NULL);
    assert(px(st, gx + 20, 0) != 0 && px(st, gx + 20, H - 1) != 0);
    cairo_surface_destroy(st);
  }
  struct wallpaper_config bars_cfg = *c;
  bars_cfg.viz_edge_smooth = false;
  cairo_surface_t *bt = render(&bars_cfg, f, dir, "wallpaper_viz_edge_bars");
  for (int y = (int)(c->viz_edge_margin * ek) + 1;
       y < H - (int)(c->viz_edge_margin * ek) - 1; ++y)
    gaps += px(bt, gx + 2, y) == 0;
  assert(gaps > c->viz_bars);
  assert(px(bt, gx + 2, ey) != 0 && px(bt, (int)(gx + elen * 0.5) + 3, ey) == 0);
  cairo_surface_destroy(bt);
  /* The treble bar sits at the top, under the free margin. */
  struct wallpaper_config margin_cfg = bars_cfg;
  margin_cfg.viz_edge_margin = 60;
  bt = render(&margin_cfg, f, dir, NULL);
  int ty = (int)(60 * ek + (H - 120 * ek) / c->viz_bars / 2);
  assert(px(bt, gx + 2, ty) != 0);
  assert(px(bt, gx + 2, (int)(60 * ek) - 3) == 0);
  cairo_surface_destroy(bt);
  /* The smooth curve spreads each level over its neighbours: alternating
   * full and empty bars come out as a gentle ripple, not spikes. */
  {
    struct wp_frame zf = f;
    for (int i = 0; i < zf.nbars; ++i)
      zf.bars[0][i] = (float)(i % 2);
    struct wallpaper_config sharp = *c;
    sharp.viz_edge_blur = 0;
    double ripple[2];
    for (int k2 = 0; k2 < 2; ++k2) {
      cairo_surface_t *zt = render(k2 ? c : &sharp, zf, dir, NULL);
      int ext[2];
      for (int b2 = 0; b2 < 2; ++b2) {
        int zy = (int)(H - eslot * (20 + b2 + 0.5)); /* bars 20 and 21 */
        int x = gx;
        while (x < W / 2 && px(zt, x, zy) != 0)
          ++x;
        ext[b2] = x - gx;
      }
      ripple[k2] = fabs((double)(ext[1] - ext[0])) / elen;
      cairo_surface_destroy(zt);
    }
    assert(ripple[0] > 0.8 && ripple[1] < 0.2);
  }
  changed = 0;
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3)
      if (px(a, x, y) != px(t, x, y)) {
        bool in = false;
        for (int i = 2; i < 4; ++i)
          in |= x >= e[i].x && x < e[i].x + e[i].w && y >= e[i].y &&
                y < e[i].y + e[i].h;
        assert(in);
        changed++;
      }
  assert(changed > 0);
  /* Partial repaints over other bars, and clearing them, equal full ones. */
  for (int i = 0; i < f.nbars; ++i)
    f.bars[0][i] = f.bars[1][i] = 0.5f + 0.5f * (float)sin(i * 0.37);
  cr = cairo_create(t);
  wp_render(cr, c, W, H, &f, false, false);
  cairo_destroy(cr);
  b = render(c, f, dir, NULL);
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3)
      assert(px(t, x, y) == px(b, x, y));
  cairo_surface_destroy(b);
  f.nbars = 0;
  cr = cairo_create(t);
  wp_render(cr, c, W, H, &f, false, true);
  cairo_destroy(cr);
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3)
      assert(px(t, x, y) == px(a, x, y));
  cairo_surface_destroy(a);
  cairo_surface_destroy(t);

  /* The logo reacting: a pulse, slid lambdas and the glow change only the
   * small logo tile (and the bar, which pulses too), and a partial repaint
   * over them equals a full one. */
  f = (struct wp_frame){.percent = 50, .level = 1, .angle = 0.3};
  a = render(c, f, dir, NULL);
  f.pulse = 1;
  f.glow = 0.6;
  for (int i = 0; i < 6; ++i)
    f.slide[i] = c->react_slide_dist * (i + 1) / 6.0;
  t = render(c, f, dir, "wallpaper_react");
  changed = 0;
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3)
      if (px(a, x, y) != px(t, x, y)) {
        if (y < r[0].y + r[0].h)
          continue; /* the pulsed bar, checked above */
        assert(x >= r[1].x && x < r[1].x + r[1].w);
        assert(y >= r[1].y && y < r[1].y + r[1].h);
        changed++;
      }
  assert(changed > 0);
  /* The most slid lambda's tip is out past the resting logo's radius. */
  double rk = c->logo_radius * k;
  int lit = 0;
  for (double ang = 0; ang < 2 * M_PI; ang += 0.01) {
    int x = (int)(W / 2 + cos(ang) * rk * 1.04);
    int y = (int)(H / 2 + sin(ang) * rk * 1.04);
    lit += px(t, x, y) != 0;
    assert(px(a, x, y) == 0);
  }
  assert(lit > 0);
  cr = cairo_create(a);
  wp_render(cr, c, W, H, &f, false, false);
  cairo_destroy(cr);
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3)
      assert(px(a, x, y) == px(t, x, y));
  cairo_surface_destroy(a);
  cairo_surface_destroy(t);

  /* A shake moves the bar and the logo (with its bloom and pulse) by whole
   * pixels, all inside the regions, and a partial repaint equals a full
   * one, whichever way it shakes. The bar's own pulse is off, so it only
   * moves. */
  struct wallpaper_config still_bar = *c;
  still_bar.bar_pulse = false;
  const struct wallpaper_config *sc = &still_bar;
  for (int d = -1; d <= 1; d += 2) {
    double sh = sc->react_shake_px * d;
    f = (struct wp_frame){.percent = 50, .level = 1, .angle = 0.3,
                          .pulse = 1, .over = 1, .dir = 1};
    a = render(sc, f, dir, NULL);
    f.shake_x = sh;
    f.shake_y = fabs(sh); /* down: up would push the bar off the top */
    t = render(sc, f, dir, d > 0 ? "wallpaper_shake" : NULL);
    int dx = (int)round(sh * k), dy = (int)round(fabs(sh) * k);
    assert(px(t, 1600 + dx, 7 + dy) == 0xffffff); /* the moved outline */
    assert(px(t, 1600, 7) == 0x000000);
    changed = 0;
    for (int y = 0; y < H; y += 3)
      for (int x = 0; x < W; x += 3)
        if (px(a, x, y) != px(t, x, y)) {
          bool in = false;
          for (int i = 0; i < 2; ++i)
            in |= x >= r[i].x && x < r[i].x + r[i].w && y >= r[i].y &&
                  y < r[i].y + r[i].h;
          assert(in);
          changed++;
        }
    assert(changed > 0);
    cr = cairo_create(a);
    wp_render(cr, sc, W, H, &f, false, false);
    cairo_destroy(cr);
    for (int y = 0; y < H; y += 3)
      for (int x = 0; x < W; x += 3)
        assert(px(a, x, y) == px(t, x, y));
    cairo_surface_destroy(a);
    cairo_surface_destroy(t);
  }
  return 0;
}
