/* The popup's contents drawn in the wallpaper (render_peek.c): only inside
 * their regions, never over the snowflake or the title, in the
 * wallpaper's colours, and repainted right by partial frames.
 * Usage: test_wallpaper_peek [outdir] */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "info.h"
#include "module.h"
#include "peek.h"
#include "wallpaper/wallpaper.h"

extern const struct swell_module wallpaper_module;

#define W 2880
#define H 1800

static const char *dir = ".";

static uint32_t px(cairo_surface_t *s, int x, int y) {
  cairo_surface_flush(s);
  const uint32_t *row =
      (const uint32_t *)(cairo_image_surface_get_data(s) +
                         y * cairo_image_surface_get_stride(s));
  return row[x] & 0xffffff;
}

static cairo_surface_t *render(const struct wallpaper_config *c,
                               const struct wp_frame *f, const char *name) {
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W, H);
  cairo_t *cr = cairo_create(s);
  wp_render(cr, c, W, H, f, true, false, false);
  cairo_destroy(cr);
  if (name) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.png", dir, name);
    assert(cairo_surface_write_to_png(s, path) == CAIRO_STATUS_SUCCESS);
  }
  return s;
}

static bool in_rect(const struct wp_rect *r, int x, int y) {
  return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

static struct info info(void) {
  struct info in;
  memset(&in, 0, sizeof in);
  in.now = 1791295200; /* Tuesday, 6 October 2026 */
  in.has_battery = true;
  in.battery = 82;
  in.has_power = true;
  in.power_w = 9.12;
  in.has_volume = true;
  in.volume = 40;
  in.has_brightness = true;
  in.brightness = 60;
  in.net = NET_WIFI;
  strcpy(in.net_name, "home");
  in.net_signal = 78;
  in.cpu = 12;
  in.mem_used_gib = 6.1;
  in.mem_total_gib = 31.2;
  in.uptime_s = 3 * 3600 + 25 * 60;
  in.ws_count = 5;
  for (int i = 0; i < 5; ++i) {
    snprintf(in.ws[i].name, sizeof in.ws[i].name, "%d", i + 1);
    in.ws[i].num = i + 1;
  }
  in.ws[1].focused = true;
  return in;
}

struct diff {
  int total;                /* pixels that differ (sampled) */
  int per[WP_PEEK_REGIONS]; /* of them, in each region */
  int logo;                 /* within the snowflake's reach */
  int coloured;             /* of them, not grey */
};

/* Where b differs from a (every other pixel); each difference must lie in
 * a peek region. */
static struct diff compare(const struct wallpaper_config *c, cairo_surface_t *a,
                           cairo_surface_t *b) {
  struct wp_rect r[WP_PEEK_REGIONS];
  wp_peek_regions(c, W, H, r);
  double reach = wp_logo_reach(c, W);
  struct diff d = {0};
  for (int y = 0; y < H; y += 2)
    for (int x = 0; x < W; x += 2) {
      uint32_t pa = px(a, x, y), pb = px(b, x, y);
      if (pa == pb)
        continue;
      bool any = false;
      for (int i = 0; i < WP_PEEK_REGIONS; ++i)
        if (in_rect(&r[i], x, y)) {
          d.per[i]++;
          any = true;
        }
      if (!any) {
        fprintf(stderr, "differs outside the regions at %d,%d\n", x, y);
        assert(0);
      }
      d.total++;
      d.logo += hypot(x - W / 2.0, y - H / 2.0) < reach;
      int rr = pb >> 16 & 0xff, gg = pb >> 8 & 0xff, bb = pb & 0xff;
      d.coloured += abs(rr - gg) > 24 || abs(gg - bb) > 24 || abs(rr - bb) > 24;
    }
  return d;
}

/* The box round every pixel where b differs from a. */
struct box {
  int x0, y0, x1, y1, n;
};
static struct box diff_box(cairo_surface_t *a, cairo_surface_t *b) {
  struct box r = {W, H, -1, -1, 0};
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
      if (px(a, x, y) != px(b, x, y)) {
        r.x0 = x < r.x0 ? x : r.x0;
        r.y0 = y < r.y0 ? y : r.y0;
        r.x1 = x > r.x1 ? x : r.x1;
        r.y1 = y > r.y1 ? y : r.y1;
        r.n++;
      }
  return r;
}

static void test_style(const struct wallpaper_config *c, const char *style) {
  struct info in = info();
  struct wp_frame base = {.percent = 60, .level = 1, .angle = 0.3, .dir = 1};
  cairo_surface_t *plain = render(c, &base, NULL);
  char name[64];

  /* The regions: left and right of the snowflake, the bottom below the
   * title, all inside the buffer and clear of the logo. */
  struct wp_rect r[WP_PEEK_REGIONS];
  wp_peek_regions(c, W, H, r);
  double reach = wp_logo_reach(c, W);
  for (int i = 0; i < WP_PEEK_REGIONS; ++i) {
    if (i >= 3) { /* the side column, terrain's labels: none under flanks */
      assert(r[i].w == 0 && r[i].h == 0);
      continue;
    }
    assert(r[i].w > 0 && r[i].h > 0);
    assert(r[i].x >= 0 && r[i].y >= 0 && r[i].x + r[i].w <= W &&
           r[i].y + r[i].h <= H);
  }
  assert(r[0].x + r[0].w < W / 2.0 - reach);
  assert(r[1].x > W / 2.0 + reach);
  assert(r[2].y > H / 2.0 + reach);
  /* The title's box (wp_regions' text region, without a shake's room) */
  struct wallpaper_config still = *c;
  still.react_shake = false;
  struct wp_rect rr[WP_REGIONS];
  wp_regions(&still, W, H, false, true, rr);
  const struct wp_rect *title = &rr[4];
  /* The left holds only the clock and date; the stats row and the
   * workspaces sit at the bottom, below the title. */
  assert(r[0].y + r[0].h < H / 2.0 + 1.25 * c->peek_clock_size);
  assert(r[2].y >= title->y + title->h);

  /* Popped out: nothing. */
  struct wp_frame f = base;
  f.peek = &in;
  f.peek_low_pct = 15;
  cairo_surface_t *s = render(c, &f, NULL);
  struct diff d = compare(c, plain, s);
  assert(d.total == 0);
  cairo_surface_destroy(s);

  /* Compact: the clock on the left, small levels on the right; nothing at
   * the bottom, nothing near the snowflake. */
  f.peek_pop = 1;
  snprintf(name, sizeof name, "wallpaper_%s_peek_compact", style);
  s = render(c, &f, name);
  d = compare(c, plain, s);
  printf("%s compact: %d (left %d, right %d, bottom %d)\n", style, d.total,
         d.per[0], d.per[1], d.per[2]);
  assert(d.per[0] > 2000 && d.per[1] > 100 && d.per[2] == 0 && d.logo == 0);
  int compact_right = d.per[1];
  cairo_surface_t *compact = s;

  /* Through the hold a thin line grows under the clock from its left
   * edge, in the left column only. */
  f.peek_hold = 0.5;
  snprintf(name, sizeof name, "wallpaper_%s_peek_hold", style);
  s = render(c, &f, name);
  d = compare(c, plain, s);
  assert(d.logo == 0 && d.per[1] == compact_right && d.per[2] == 0);
  struct box half = diff_box(compact, s);
  cairo_surface_destroy(s);
  f.peek_hold = 1;
  s = render(c, &f, NULL);
  struct box whole = diff_box(compact, s);
  cairo_surface_destroy(s);
  cairo_surface_destroy(compact);
  printf("%s hold: half %d..%d x %d..%d, whole %d..%d x %d..%d\n", style,
         half.x0, half.x1, half.y0, half.y1, whole.x0, whole.x1, whole.y0,
         whole.y1);
  assert(half.n > 0 && whole.n > half.n);
  assert(in_rect(&r[0], half.x0, half.y0) &&
         in_rect(&r[0], whole.x1, whole.y1));
  assert(whole.y1 - whole.y0 <= 2 * c->peek_hold_width + 4);
  assert(abs(half.x0 - whole.x0) <= 3);
  double ratio = (double)(half.x1 - half.x0) / (whole.x1 - whole.x0);
  assert(ratio > 0.4 && ratio < 0.6);

  /* Expanded: everything, the line gone, bigger levels, the bottom. */
  f.peek_hold = 1;
  f.peek_expand = 1;
  snprintf(name, sizeof name, "wallpaper_%s_peek_full", style);
  s = render(c, &f, name);
  d = compare(c, plain, s);
  printf("%s full: %d (left %d, right %d, bottom %d), coloured %d\n", style,
         d.total, d.per[0], d.per[1], d.per[2], d.coloured);
  assert(d.per[0] > 4000 && d.per[1] > compact_right && d.per[2] > 200);
  assert(d.logo == 0);
  /* The wallpaper's colours: topo's gradient, classic's greys. */
  if (wp_topo(c))
    assert(d.coloured > d.total / 20);
  else
    assert(d.coloured == 0);
  struct wp_frame unheld = f;
  unheld.peek_hold = 0;
  cairo_surface_t *u = render(c, &unheld, NULL);
  assert(diff_box(s, u).n == 0);
  cairo_surface_destroy(u);

  /* Without workspaces the bottom still shows the stats row (expanded
   * only). */
  struct info nows = in;
  nows.ws_count = 0;
  struct wp_frame nf = f;
  nf.peek = &nows;
  cairo_surface_t *n = render(c, &nf, NULL);
  struct diff dn = compare(c, plain, n);
  assert(dn.per[2] > 200);
  cairo_surface_destroy(n);
  nf.peek_expand = 0;
  nf.peek_hold = 0;
  n = render(c, &nf, NULL);
  dn = compare(c, plain, n);
  assert(dn.per[2] == 0);
  cairo_surface_destroy(n);

  /* Partial frames: drawn over the plain frame it matches the whole one,
   * and drawn plain over it (peek_area) it clears back. */
  cairo_surface_t *t = render(c, &base, NULL);
  struct wp_frame pf = f;
  pf.peek_area = true;
  cairo_t *cr = cairo_create(t);
  wp_render(cr, c, W, H, &pf, false, false, false);
  cairo_destroy(cr);
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3)
      assert(px(t, x, y) == px(s, x, y));
  struct wp_frame back = base;
  back.peek_area = true;
  cr = cairo_create(t);
  wp_render(cr, c, W, H, &back, false, false, false);
  cairo_destroy(cr);
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3)
      assert(px(t, x, y) == px(plain, x, y));
  cairo_surface_destroy(t);
  cairo_surface_destroy(s);

  /* Low battery shows in peek_warn. */
  struct info low = in;
  low.battery = 9;
  f.peek = &low;
  s = render(c, &f, NULL);
  int warn = 0;
  uint32_t wc = c->peek_warn >> 8;
  for (int y = r[1].y; y < r[1].y + r[1].h; y += 2)
    for (int x = r[1].x; x < r[1].x + r[1].w; x += 2)
      warn += px(s, x, y) == wc;
  assert(warn > 20);
  cairo_surface_destroy(s);
  cairo_surface_destroy(plain);
}

/* Under the GPU (map and snowflake on their own surfaces) the buffer is
 * clear but for what cairo draws: the contents still only in the regions. */
static void test_gpu_frame(const struct wallpaper_config *c) {
  struct info in = info();
  struct wp_frame base = {.percent = 60,
                          .level = 1,
                          .angle = 0.3,
                          .dir = 1,
                          .map_under = true,
                          .logo_gpu = true};
  cairo_surface_t *plain = render(c, &base, NULL);
  struct wp_frame f = base;
  f.peek = &in;
  f.peek_pop = f.peek_expand = 1;
  f.peek_hold = 0.5;
  cairo_surface_t *s = render(c, &f, NULL);
  struct diff d = compare(c, plain, s);
  assert(d.per[0] > 4000 && d.logo == 0);
  cairo_surface_destroy(s);
  /* Under the GPU's map the lines show the hold: no line under the clock. */
  f.peek_expand = 0;
  f.peek_hold = 0;
  s = render(c, &f, NULL);
  f.peek_hold = 0.5;
  cairo_surface_t *h = render(c, &f, NULL);
  assert(diff_box(s, h).n == 0);
  cairo_surface_destroy(h);
  cairo_surface_destroy(s);
  cairo_surface_destroy(plain);
}

/* The frame's peek fields from the popup's timeline: none without one or
 * with a window open (the plain wallpaper), else its values at now. */
static void test_frame(void) {
  struct info in = info();
  struct peek pk = {.info = &in,
                    .battery_low_pct = 20,
                    .pop_ms = 200,
                    .close_ms = 100,
                    .expand_ms = 300,
                    .expand_delay_ms = 2000,
                    .hold_line = true};
  struct wp_frame f;
  memset(&f, 0, sizeof f);
  f.peek_pop = 0.7; /* stale */
  wp_peek_frame(NULL, true, 1000, &f);
  assert(!f.peek && f.peek_pop == 0 && f.peek_expand == 0 && f.peek_hold == 0);
  peek_open(&pk, true, 1000);
  wp_peek_frame(&pk, false, 1100, &f);
  assert(!f.peek && f.peek_pop == 0);
  wp_peek_frame(&pk, true, 1100, &f);
  struct peek_values v;
  peek_values(&pk, 1100, &v);
  assert(f.peek == &in && f.peek_low_pct == 20);
  assert(f.peek_pop == v.pop && f.peek_hold == v.hold && f.peek_expand == 0);
  peek_expand(&pk, 3000);
  wp_peek_frame(&pk, true, 3150, &f);
  assert(f.peek_expand > 0 && f.peek_expand < 1 && f.peek_pop == 1);
}

/* The topo lines gather in towards the snowflake through the hold, spring
 * back out (overshooting) after the expand, and settle. */
static void test_gather(void) {
  struct info in = info();
  struct peek pk = {.info = &in,
                    .pop_ms = 200,
                    .close_ms = 100,
                    .expand_ms = 300,
                    .expand_delay_ms = 2000,
                    .hold_line = true};
  struct wp_frame f;
  memset(&f, 0, sizeof f);
  f.peek_gather = 0.5; /* stale */
  wp_peek_frame(NULL, true, 1000, &f);
  assert(f.peek_gather == 0);
  peek_open(&pk, true, 1000);
  wp_peek_frame(&pk, false, 2000, &f);
  assert(f.peek_gather == 0); /* occupied: the plain wallpaper */
  /* Rising with the hold (pop done past 200 ms). */
  double g[3];
  const int64_t at[3] = {1500, 2000, 3000};
  for (int i = 0; i < 3; ++i) {
    wp_peek_frame(&pk, true, at[i] - 1, &f);
    g[i] = f.peek_gather;
  }
  printf("gather through the hold: %.3f %.3f %.3f\n", g[0], g[1], g[2]);
  assert(0 < g[0] && g[0] < g[1] && g[1] < g[2] && g[2] > 0.99);
  assert(wp_peek_moving(&pk, 2000));
  /* The expand: fully in, then out past the rest, then still. */
  peek_expand(&pk, 3000);
  wp_peek_frame(&pk, true, 3000, &f);
  assert(fabs(f.peek_gather - 1) < 1e-9);
  double low = 1;
  for (int64_t t = 3100; t <= 3500; t += 10) {
    wp_peek_frame(&pk, true, t, &f);
    low = fmin(low, f.peek_gather);
  }
  printf("gather overshoot: %.3f\n", low);
  assert(low < -0.05);
  assert(wp_peek_moving(&pk, 3800)); /* the expand tween is done by 3300 */
  wp_peek_frame(&pk, true, 4600, &f);
  assert(f.peek_gather == 0 && !wp_peek_moving(&pk, 4600));
  /* Let go before the expand: it goes back out with the closing pop. */
  struct peek early = pk;
  peek_open(&early, true, 10000);
  peek_close(&early, 11000);
  wp_peek_frame(&early, true, 11000, &f);
  double before = f.peek_gather;
  wp_peek_frame(&early, true, 11050, &f);
  struct peek_values v;
  peek_values(&early, 11050, &v);
  assert(before > 0 && f.peek_gather < before && f.peek_gather > 0);
  assert(fabs(f.peek_gather - before * v.pop) < 1e-9);
}

/* "side": the hold moves the snowflake right and grows it, the flanks'
 * parts fade as it goes, the expand shows them all in one column on the
 * left, the lines dim. */
static void test_side(const struct wallpaper_config *c, const char *style,
                      bool gpu) {
  struct wallpaper_config flanks = *c;
  strcpy(flanks.peek_layout, "flanks");
  struct info in = info();
  struct wp_frame base = {.percent = 60,
                          .level = 1,
                          .angle = 0.3,
                          .dir = 1,
                          .map_under = gpu,
                          .logo_gpu = gpu};
  cairo_surface_t *plain = render(c, &base, NULL);
  char name[64];

  /* The column: on screen, left of where the snowflake goes. */
  struct wp_rect r[WP_PEEK_REGIONS];
  wp_peek_regions(c, W, H, r);
  const struct wp_rect *col = &r[3];
  assert(col->w > 0 && col->h > 0 && col->x >= 0 && col->y >= 0 &&
         col->x + col->w <= W && col->y + col->h <= H);
  double logo_left =
      c->peek_side_x * W - c->logo_radius * W / WP_REF_WIDTH * c->peek_side_scale;
  assert(col->x + col->w < logo_left);
  assert(col->x < W / 4);

  /* Compact, not held: the flanks' look. */
  struct wp_frame f = base;
  f.peek = &in;
  f.peek_low_pct = 15;
  f.peek_pop = 1;
  cairo_surface_t *s = render(c, &f, NULL);
  cairo_surface_t *fl = render(&flanks, &f, NULL);
  assert(diff_box(s, fl).n == 0);
  cairo_surface_destroy(fl);
  cairo_surface_destroy(s);

  /* Placement: in the middle at 0, at the side place and size at 1. */
  double cx, cy, ls;
  wp_logo_place(c, W, H, &f, &cx, &cy, &ls);
  assert(cx == W / 2.0 && cy == H / 2.0 && ls == 1);
  f.peek_side = 1;
  wp_logo_place(c, W, H, &f, &cx, &cy, &ls);
  assert(fabs(cx - c->peek_side_x * W) < 1e-9 &&
         fabs(cy - c->peek_side_y * H) < 1e-9 && ls == c->peek_side_scale);
  wp_logo_place(&flanks, W, H, &f, &cx, &cy, &ls);
  assert(cx == W / 2.0 && ls == 1);
  struct wp_rect box = wp_logo_box(c, W, H, &f);
  assert(box.x + box.w == W && box.w > W / 3);
  assert(wp_map_brightness(c, &f) < 1 - c->peek_side_dim + 1e-9);
  assert(wp_map_brightness(&flanks, &f) == 1);

  /* Half way through the hold: the flanks' parts half faded, nothing in
   * the column yet. */
  f.peek_hold = 0.5;
  f.peek_side = 0.5;
  snprintf(name, sizeof name, "wallpaper_%s%s_peek_side_hold", style,
           gpu ? "_gpu" : "");
  s = render(c, &f, name);
  cairo_surface_destroy(s);

  /* Expanded: the column; nothing of the flanks' parts. */
  f.peek_hold = 1;
  f.peek_side = 1;
  f.peek_expand = 1;
  snprintf(name, sizeof name, "wallpaper_%s%s_peek_side_full", style,
           gpu ? "_gpu" : "");
  s = render(c, &f, name);
  struct wp_frame bare = base;
  bare.peek = &in; /* the snowflake aside, the map dimmed, no text */
  bare.peek_side = 1;
  bare.peek_pop = 1e-9;
  cairo_surface_t *aside = render(c, &bare, NULL);
  struct box text = diff_box(aside, s);
  printf("%s%s side: column %d..%d x %d..%d (region %d..%d x %d..%d)\n",
         style, gpu ? " gpu" : "", text.x0, text.x1, text.y0, text.y1, col->x,
         col->x + col->w, col->y, col->y + col->h);
  assert(text.n > 20000);
  assert(in_rect(col, text.x0, text.y0) && in_rect(col, text.x1, text.y1));
  /* without the GPU the snowflake is in the buffer, moved */
  if (!gpu) {
    long moved = 0;
    for (int y = H / 2 - 300; y < H / 2 + 300; y += 4)
      for (int x = W / 2 - 300; x < W / 2 + 300; x += 4)
        moved += px(aside, x, y) != px(plain, x, y);
    assert(moved > 1000);
  }
  cairo_surface_destroy(aside);

  /* Partial frames over the GPU (the snowflake on its own surface): drawn
   * over the plain frame it matches the whole one. */
  if (gpu) {
    cairo_surface_t *t = render(c, &base, NULL);
    struct wp_frame pf = f;
    pf.peek_area = true;
    cairo_t *cr = cairo_create(t);
    wp_render(cr, c, W, H, &pf, false, false, false);
    cairo_destroy(cr);
    assert(diff_box(t, s).n == 0);
    cairo_surface_destroy(t);
  }

  /* Low battery shows in peek_warn, in the column. */
  struct info low = in;
  low.battery = 9;
  f.peek = &low;
  cairo_surface_t *w = render(c, &f, NULL);
  int warn = 0;
  uint32_t wc = c->peek_warn >> 8;
  for (int y = col->y; y < col->y + col->h; y += 2)
    for (int x = col->x; x < col->x + col->w; x += 2)
      warn += px(w, x, y) == wc;
  assert(warn > 20);
  cairo_surface_destroy(w);
  cairo_surface_destroy(s);
  cairo_surface_destroy(plain);
}

/* The snowflake's move aside from the timeline: none popping, along the
 * hold, all the way once expanded, back with a close. */
static void test_side_frame(void) {
  struct info in = info();
  struct peek pk = {.info = &in,
                    .pop_ms = 200,
                    .close_ms = 100,
                    .expand_ms = 300,
                    .expand_delay_ms = 2000,
                    .hold_line = true};
  struct wp_frame f;
  memset(&f, 0, sizeof f);
  f.peek_side = 0.5; /* stale */
  wp_peek_frame(NULL, true, 1000, &f);
  assert(f.peek_side == 0);
  peek_open(&pk, true, 1000);
  wp_peek_frame(&pk, true, 1000, &f);
  assert(f.peek_side == 0);
  double last = 0;
  for (int64_t t = 1200; t < 3000; t += 200) {
    wp_peek_frame(&pk, true, t, &f);
    assert(f.peek_side > last && f.peek_side < 1);
    last = f.peek_side;
  }
  peek_expand(&pk, 3000);
  wp_peek_frame(&pk, true, 3000, &f);
  assert(f.peek_side == 1);
  wp_peek_frame(&pk, true, 5000, &f);
  assert(f.peek_side == 1);
  peek_close(&pk, 6000);
  wp_peek_frame(&pk, true, 6050, &f);
  assert(f.peek_side > 0 && f.peek_side < 1);
  wp_peek_frame(&pk, true, 6100, &f);
  assert(f.peek_side == 0);
  wp_peek_frame(&pk, false, 5000, &f); /* a window: none */
  assert(f.peek_side == 0);
}

int main(int argc, char **argv) {
  dir = argc > 1 ? argv[1] : ".";
  setenv("TZ", "UTC", 1);
  test_frame();
  test_gather();
  test_side_frame();
  const struct wallpaper_config *def = wallpaper_module.defaults;
  assert(wp_topo(def) && !strcmp(def->peek_layout, "terrain"));
  /* (terrain without the GPU is side; the side tests name it) */
  struct wallpaper_config side_c = *def;
  strcpy(side_c.peek_layout, "side");
  const struct wallpaper_config *side = &side_c;
  /* The flanks layout (all of it as before). */
  struct wallpaper_config topo = *side;
  strcpy(topo.peek_layout, "flanks");
  test_style(&topo, "topo");
  struct wallpaper_config classic = topo;
  strcpy(classic.style, "classic");
  test_style(&classic, "classic");
  test_gpu_frame(&topo);
  test_side(side, "topo", false);
  test_side(side, "topo", true);
  struct wallpaper_config side_classic = *side;
  strcpy(side_classic.style, "classic");
  test_side(&side_classic, "classic", false);
  wp_render_finish();
  printf("wallpaper peek: ok\n");
  return 0;
}
