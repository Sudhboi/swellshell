/* Offscreen render test of the wallpaper at 2880x1800 (the reference
 * size), plus the pulse shape and the partial-repaint regions.
 * Usage: test_wallpaper_render [outdir]   (default: current directory) */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>

#include "art.h"
#include "module.h"
#include "nixlogo.h"
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

/* A w x h JPEG of one colour at path. */
static void write_jpeg(const char *path, int w, int h, int r, int g, int b) {
  FILE *f = fopen(path, "wb");
  assert(f);
  struct jpeg_compress_struct ci;
  struct jpeg_error_mgr err;
  ci.err = jpeg_std_error(&err);
  jpeg_create_compress(&ci);
  jpeg_stdio_dest(&ci, f);
  ci.image_width = w;
  ci.image_height = h;
  ci.input_components = 3;
  ci.in_color_space = JCS_RGB;
  jpeg_set_defaults(&ci);
  jpeg_set_quality(&ci, 95, TRUE);
  jpeg_start_compress(&ci, TRUE);
  JSAMPLE row[3 * 400];
  for (int x = 0; x < w; ++x) {
    row[3 * x] = r;
    row[3 * x + 1] = g;
    row[3 * x + 2] = b;
  }
  while (ci.next_scanline < ci.image_height) {
    JSAMPROW rows[1] = {row};
    jpeg_write_scanlines(&ci, rows, 1);
  }
  jpeg_finish_compress(&ci);
  jpeg_destroy_compress(&ci);
  fclose(f);
}

/* The pixel t (0..1) of the way clockwise round the hexagon of radius hr
 * from the middle of its top edge, with the snowflake turned by angle. Its
 * corners sit at 176.8 + 60 i degrees (y down, unturned). */
static void rim(double hr, double t, double angle, int *x, int *y) {
  double at = t * 6 + 0.5;
  int e = (int)floor(at);
  double u = at - e;
  double a = (176.8 + 60 * (1 + e)) * M_PI / 180, b = a + M_PI / 3;
  double hx = hr * (cos(a) + (cos(b) - cos(a)) * u);
  double hy = hr * (sin(a) + (sin(b) - sin(a)) * u);
  *x = (int)floor(W / 2.0 + hx * cos(angle) - hy * sin(angle));
  *y = (int)floor(H / 2.0 + hx * sin(angle) + hy * cos(angle));
}

static cairo_surface_t *render(const struct wallpaper_config *c,
                               struct wp_frame f, const char *dir,
                               const char *name) {
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W, H);
  cairo_t *cr = cairo_create(s);
  wp_render(cr, c, W, H, &f, true, false, false);
  cairo_destroy(cr);
  if (name) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.png", dir, name);
    assert(cairo_surface_write_to_png(s, path) == CAIRO_STATUS_SUCCESS);
  }
  return s;
}

/* What a buffer must repaint for the track's details: the hexagon only
 * when it paints something, the title only when it differs from what the
 * buffer holds. */
static void test_media_kept(const struct wallpaper_config *c,
                            cairo_surface_t *cover) {
  /* A track without a length has no hexagon, title or not. (Only topo's
   * snowflake goes on the GPU.) */
  struct wp_frame f = {.percent = 50,
                       .level = 1,
                       .angle = 0.3,
                       .media_alpha = 1,
                       .progress = -1,
                       .title = "T",
                       .artist = "A",
                       .logo_gpu = wp_topo(c)};
  assert(!wp_hex_shows(c, &f));
  f.progress = 0.4;
  assert(wp_hex_shows(c, &f));
  struct wallpaper_config noline = *c;
  noline.media_line_width = 0;
  assert(!wp_hex_shows(&noline, &f));
  f.art = cover;
  assert(wp_hex_shows(&noline, &f));
  struct wallpaper_config noart = noline;
  noart.media_art = false;
  assert(!wp_hex_shows(&noart, &f));
  noline.media_art_alpha = 0;
  assert(!wp_hex_shows(&noline, &f));
  f.media_alpha = 0;
  assert(!wp_hex_shows(c, &f));
  f.media_alpha = 1;

  /* The title's key: the same through a spin, pulse, glow or progress;
   * not through a fade, a shake or another track. */
  struct wp_text_key k0, k1;
  wp_text_key(c, W, &f, &k0);
  assert(k0.shown);
  struct wp_frame g = f;
  g.angle = 1.2;
  g.pulse = 0.7;
  g.glow = 0.5;
  g.progress = 0.9;
  wp_text_key(c, W, &g, &k1);
  assert(wp_text_key_eq(&k0, &k1));
  struct wp_frame d[4] = {f, f, f, f};
  d[0].media_alpha = 0.5;
  d[1].shake_x = 10;
  d[2].title = "U";
  d[3].artist = "B";
  for (int i = 0; i < 4; ++i) {
    wp_text_key(c, W, &d[i], &k1);
    assert(!wp_text_key_eq(&k0, &k1));
  }
  /* A shake under half a pixel doesn't move the tile. */
  g = f;
  g.shake_x = 0.1;
  wp_text_key(c, W, &g, &k1);
  assert(wp_text_key_eq(&k0, &k1));
  /* Nothing shown (hidden, faded out, no title) is all the same. */
  struct wallpaper_config notitle = *c;
  notitle.media_title = false;
  wp_text_key(&notitle, W, &f, &k1);
  assert(!k1.shown);
  struct wp_frame h[2] = {f, f};
  h[0].media_alpha = 0;
  h[1].title = NULL;
  for (int i = 0; i < 2; ++i) {
    wp_text_key(c, W, &h[i], &k0);
    assert(!k0.shown && wp_text_key_eq(&k0, &k1));
  }

  /* A partial repaint keeping the title leaves its region alone, and over
   * a buffer holding the same title equals a full repaint. */
  cairo_surface_t *t = render(c, f, NULL, NULL);
  struct wp_rect r[WP_REGIONS];
  wp_regions_ex(c, W, H, false, true, f.logo_gpu, false, r);
  assert(r[4].w > 0);
  int mx = r[4].x + 3, my = r[4].y + r[4].h - 3;
  cairo_t *cr = cairo_create(t);
  cairo_set_source_rgb(cr, 1, 0, 1);
  cairo_rectangle(cr, mx, my, 1, 1);
  cairo_fill(cr);
  cairo_destroy(cr);
  g = f;
  g.angle = 0.9;
  g.pulse = 0.5;
  g.text_kept = true;
  cr = cairo_create(t);
  wp_render(cr, c, W, H, &g, false, false, false);
  cairo_destroy(cr);
  assert(px(t, mx, my) == 0xff00ff);
  g.text_kept = false;
  cr = cairo_create(t);
  wp_render(cr, c, W, H, &g, false, false, false);
  cairo_destroy(cr);
  assert(px(t, mx, my) != 0xff00ff);
  g.angle = 1.5;
  g.text_kept = true;
  cr = cairo_create(t);
  wp_render(cr, c, W, H, &g, false, false, false);
  cairo_destroy(cr);
  cairo_surface_t *b = render(c, g, NULL, NULL);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
      if (px(t, x, y) != px(b, x, y)) {
        fprintf(stderr, "%s %d,%d %06x %06x\n", c->style, x, y, px(t, x, y),
                px(b, x, y));
        abort();
      }
  cairo_surface_destroy(b);
  cairo_surface_destroy(t);
}

/* The frame t ms into show s, over a normal frame base. */
static struct wp_frame show_at(const struct wallpaper_config *c,
                               enum wp_show s, double t,
                               struct wp_frame base) {
  wp_show_apply(c, s, t, &base);
  return base;
}

static bool all_black(cairo_surface_t *s) {
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3)
      if (px(s, x, y) != 0)
        return false;
  return true;
}

/* The launch and power-off animations, written out for a look:
 * wallpaper_intro_*, wallpaper_scatter_*, wallpaper_crt_*. */
static void test_shows(const struct wallpaper_config *c, const char *dir,
                       const char *prefix) {
  struct wp_frame base = {.percent = 60, .level = 1, .angle = 0.3, .dir = 1};
  cairo_surface_t *plain = render(c, base, dir, NULL);
  char name[64];

  /* Intro: nothing at the start, the lambdas on their way in mid-flight
   * (away from the centre), and the plain frame once it's over. */
  int len = wp_show_ms(c, WP_SHOW_INTRO);
  assert(len > c->intro_ms);
  snprintf(name, sizeof name, "%s_intro_0", prefix);
  cairo_surface_t *s = render(c, show_at(c, WP_SHOW_INTRO, 0, base), dir,
                              name);
  assert(all_black(s));
  cairo_surface_destroy(s);
  for (int i = 1; i <= 5; ++i) {
    double t = len * i / 6.0;
    snprintf(name, sizeof name, "%s_intro_%d", prefix, i);
    cairo_surface_destroy(render(c, show_at(c, WP_SHOW_INTRO, t, base), dir,
                                 name));
  }
  struct wp_frame mid = show_at(c, WP_SHOW_INTRO, c->intro_ms * 0.5, base);
  assert(mid.slide[0] > 0 && mid.slide[0] < c->intro_dist);
  assert(mid.slide[5] > mid.slide[0]); /* the last one is further out */
  assert(mid.bar_fade == 1);           /* the bar waits for the lock */
  s = render(c, show_at(c, WP_SHOW_INTRO, len, base), dir, NULL);
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3)
      assert(px(s, x, y) == px(plain, x, y));
  cairo_surface_destroy(s);

  /* Scatter: the lambdas go and the bar drains; black at the end. */
  len = wp_show_ms(c, WP_SHOW_SCATTER);
  for (int i = 0; i <= 5; ++i) {
    double t = len * i / 5.0;
    snprintf(name, sizeof name, "%s_scatter_%d", prefix, i);
    s = render(c, show_at(c, WP_SHOW_SCATTER, t, base), dir, name);
    if (i == 0)
      for (int y = 0; y < H; y += 3)
        for (int x = 0; x < W; x += 3)
          assert(px(s, x, y) == px(plain, x, y));
    if (i == 5)
      assert(all_black(s));
    cairo_surface_destroy(s);
  }
  struct wp_frame sc = show_at(c, WP_SHOW_SCATTER, len * 0.5, base);
  assert(sc.slide[0] > sc.slide[5]); /* the first one leaves first */
  assert(sc.percent < base.percent);
  s = render(c, show_at(c, WP_SHOW_SCATTER, len * 2, base), dir, NULL);
  assert(all_black(s));
  cairo_surface_destroy(s);

  /* CRT: the logo shrinks to a dot, which becomes a line across the
   * screen, then nothing. */
  len = wp_show_ms(c, WP_SHOW_CRT);
  for (int i = 0; i <= 8; ++i) {
    double t = len * i / 8.0;
    snprintf(name, sizeof name, "%s_crt_%d", prefix, i);
    s = render(c, show_at(c, WP_SHOW_CRT, t, base), dir, name);
    if (i == 8)
      assert(all_black(s));
    cairo_surface_destroy(s);
  }
  struct wp_frame line = show_at(c, WP_SHOW_CRT, len * 0.78, base);
  assert(line.logo_shrink == 1 && line.crt_alpha > 0.9);
  assert(line.crt_w > W * 0.9 && line.crt_h < 10);
  snprintf(name, sizeof name, "%s_crt_line", prefix);
  s = render(c, line, dir, name);
  assert(px(s, 100, H / 2) != 0 && px(s, W - 100, H / 2) != 0);
  assert(px(s, 100, H / 2 - 60) == 0);
  /* No logo left, only the line and its glow. */
  for (int y = H / 2 - 200; y < H / 2 - 40; y += 2)
    for (int x = W / 2 - 200; x < W / 2 + 200; x += 2)
      assert(px(s, x, y) == 0);
  cairo_surface_destroy(s);
  cairo_surface_destroy(plain);
}

/* topo's shows (the map's part drawn by cairo, where there's no GPU):
 * wallpaper_topo_show_<name>_*. Each as likely; intros end on the plain
 * frame, power-offs in black by their length. */
static const enum wp_show topo_shows[] = {WP_SHOW_SHOCK, WP_SHOW_RISE,
                                          WP_SHOW_DRAIN, WP_SHOW_SINK,
                                          WP_SHOW_BLAST};
#define NTOPO ((int)(sizeof(topo_shows) / sizeof(topo_shows[0])))

static void test_topo_shows(const struct wallpaper_config *c, const char *dir) {
  int seen[WP_SHOW_COUNT] = {0}, intros = 0;
  for (int i = 0; i < 6000; ++i) {
    bool intro = i % 2;
    enum wp_show s = wp_show_pick(c, intro, (i / 2) / 3000.0);
    assert(s != WP_SHOW_CRT && wp_show_is_intro(s) == intro);
    seen[s]++;
  }
  for (int i = 0; i < NTOPO; ++i) {
    enum wp_show s = topo_shows[i];
    intros += wp_show_is_intro(s);
    assert(wp_show_named(wp_show_name(s)) == s);
  }
  for (int i = 0; i < NTOPO; ++i) {
    enum wp_show s = topo_shows[i];
    int n = wp_show_is_intro(s) ? intros : NTOPO - intros;
    assert(abs(seen[s] - 3000 / n) <= 1);
  }
  assert(wp_show_named("nope") == WP_SHOW_NONE);

  struct wp_frame base = {.percent = 60, .level = 1, .angle = 0.3, .dir = 1};
  cairo_surface_t *plain = render(c, base, dir, NULL);
  char name[96];
  for (int i = 0; i < NTOPO; ++i) {
    enum wp_show s = topo_shows[i];
    int len = wp_show_ms(c, s);
    assert(len ==
           (wp_show_is_intro(s) ? wp_show_ms(c, WP_SHOW_INTRO) : c->outro_ms));
    for (int j = 0; j <= 6; ++j) {
      snprintf(name, sizeof name, "wallpaper_topo_show_%s_%d", wp_show_name(s),
               j);
      cairo_surface_destroy(
          render(c, show_at(c, s, len * j / 6.0, base), dir, name));
    }
    cairo_surface_t *e = render(c, show_at(c, s, len, base), dir, NULL);
    if (wp_show_is_intro(s)) {
      for (int y = 0; y < H; y += 3)
        for (int x = 0; x < W; x += 3)
          assert(px(e, x, y) == px(plain, x, y));
    } else {
      assert(all_black(e));
    }
    cairo_surface_destroy(e);
    /* An intro starts on the snowflake as the boot splash leaves it: whole,
     * at rest, turned as the daemon has it, not lit; the map not yet in. */
    if (wp_show_is_intro(s)) {
      struct wp_frame f0 = show_at(c, s, 0, base);
      assert(wp_logo_whole(&f0));
      assert(f0.angle == base.angle && f0.flash == 0 && f0.glow == 0);
      assert(f0.crt_alpha == 0);
      cairo_surface_t *z = render(c, f0, dir, NULL);
      struct wp_frame bare = base;
      bare.bar_fade = 1;
      bare.percent = 0;
      bare.bg_fade = 1;
      cairo_surface_t *want = render(c, bare, dir, NULL);
      int bad = 0, bx = 0, by = 0;
      for (int y = 0; y < H; y += 3)
        for (int x = 0; x < W; x += 3)
          if (px(z, x, y) != px(want, x, y) && !bad++)
            bx = x, by = y;
      if (bad)
        fprintf(stderr, "%s at 0: %d px differ, first %d,%d: %06x vs %06x\n",
                wp_show_name(s), bad, bx, by, px(z, bx, by), px(want, bx, by));
      assert(bad == 0);
      cairo_surface_destroy(z);
      cairo_surface_destroy(want);
    }
  }
  cairo_surface_destroy(plain);
}

/* The topo style (the default): the contour map under the glass snowflake,
 * wallpaper_topo_*. */
/* The GPU snowflake's distance field: negative inside the lambdas, the
 * distance to their edges, agreeing with cairo's fill. */
/* The saver's cover over windows: in over in_ms, out over out_ms, turning
 * round wherever it is, clamped, at once with 0 ms. */
static void test_cover_step(void) {
  double c = 0;
  for (int i = 0; i < 6; ++i)
    c = wp_cover_step(c, 1, 0.2, 1200, 300);
  assert(fabs(c - 1) < 1e-9);
  c = wp_cover_step(c, 1, 0.2, 1200, 300);
  assert(c == 1);
  c = wp_cover_step(0.5, 0, 0.15, 1200, 300);
  assert(fabs(c) < 1e-9);
  c = wp_cover_step(0.5, 1, 0.3, 1200, 300); /* a quarter on */
  assert(fabs(c - 0.75) < 1e-9);
  c = wp_cover_step(c, 0, 0.15, 1200, 300); /* woke mid fade-in: back */
  assert(fabs(c - 0.25) < 1e-9);
  assert(wp_cover_step(0.3, 0, 0.01, 1200, 0) == 0);
  assert(wp_cover_step(0.3, 1, 0.01, 0, 300) == 1);
  assert(wp_cover_step(0, 0, 1, 1200, 300) == 0);
}

static void test_logo_sdf(void) {
  const struct wallpaper_config *c = wallpaper_module.defaults;
  const struct wp_logo_sdf *s = wp_logo_sdf(c, W, 1);
  double r0 = c->logo_radius;
  assert(s->w > 2 * 0.85 * r0 && s->h > 2 * 0.85 * r0);
  assert(wp_logo_sdf(c, W, 1) == s && wp_logo_sdf(c, W / 2, 1) != s);
  s = wp_logo_sdf(c, W, 1);
#define D(px, py)                                                             \
  s->d[(int)floor((py) - s->y) * s->w + (int)floor((px) - s->x)]
  /* The hole in the middle: outside, about its inner radius from an edge. */
  double hole = D(0.5, 0.5) / r0;
  assert(hole > 0.3 && hole < 0.4);
  /* The edge from lambda 0's inner corner down its long leg: ±1 px either
   * side of its middle (texel centres sit on half px). */
  double ax = -0.38986 * r0, ay = 0.02158 * r0, bx = 0.09750 * r0,
         by = 0.86581 * r0;
  double len = hypot(bx - ax, by - ay);
  double nx = (by - ay) / len, ny = -(bx - ax) / len; /* outwards */
  for (int side = -1; side <= 1; side += 2) {
    double x = (ax + bx) / 2 + side * 3 * nx, y = (ay + by) / 2 + side * 3 * ny;
    double d = D(x, y);
    assert(d * side > 1.5 && d * side < 4.5);
  }
  /* The corners of the texture are clear of the outline. */
  assert(s->d[0] > c->topo_edge_width && s->d[s->w * s->h - 1] > 0);
  /* Against cairo's fill at the same size: every texel 1.5 px or more
   * from an edge (the sharp corners' antialiasing reaches past 1) is in or
   * out alike. */
  cairo_surface_t *m = cairo_image_surface_create(CAIRO_FORMAT_A8, s->w, s->h);
  cairo_t *cr = cairo_create(m);
  cairo_translate(cr, -s->x, -s->y);
  cairo_scale(cr, r0, r0);
  for (int k = 0; k < 6; ++k)
    nixlogo_lambda_path(cr, k, 0);
  cairo_fill(cr);
  cairo_destroy(cr);
  cairo_surface_flush(m);
  const unsigned char *md = cairo_image_surface_get_data(m);
  int ms = cairo_image_surface_get_stride(m);
  long in = 0;
  for (int y = 0; y < s->h; ++y)
    for (int x = 0; x < s->w; ++x) {
      float d = s->d[y * s->w + x];
      if (d < -1.5)
        assert(md[y * ms + x] == 255), ++in;
      else if (d > 1.5)
        assert(md[y * ms + x] == 0);
    }
  assert(in > 1000);
  cairo_surface_destroy(m);
  /* Four texels per px (the snowflake grown aside): over the same box,
   * the coarse one still cached; near the edges it is exact, so within
   * its texel's 1/8 px offset of the coarse one at each coarse centre,
   * and in or out alike everywhere. */
  const struct wp_logo_sdf *f = wp_logo_sdf(c, W, 4);
  assert(f != s && f->res == 4 && wp_logo_sdf(c, W, 1) == s);
  assert(f->x == s->x && f->y == s->y && f->w == 4 * s->w &&
         f->h == 4 * s->h);
  long near = 0;
  for (int y = 0; y < s->h; ++y)
    for (int x = 0; x < s->w; ++x) {
      float d = s->d[y * s->w + x], e = f->d[(4 * y + 1) * f->w + 4 * x + 1];
      if (fabsf(d) < 4)
        assert(fabsf(d - e) < 0.2), ++near;
      if (fabsf(d) > 1)
        assert((d < 0) == (e < 0));
    }
  assert(near > 1000);
#undef D
}

static void test_topo(const char *dir) {
  const struct wallpaper_config *c = wallpaper_module.defaults;
  assert(wp_topo(c));
  struct wallpaper_config other = *c;
  strcpy(other.style, "something");
  assert(wp_topo(&other)); /* unknown styles draw as topo */

  struct wp_frame f = {.percent = 60, .level = 1, .angle = 0.3};
  cairo_surface_t *a = render(c, f, dir, "wallpaper_topo_rest");
  /* Black between the lines; the lines warmer at the left than the
   * right (the brightest pixel near each side, below the bar). */
  uint32_t l = 0, r = 0;
  int black = 0;
  for (int y = 100; y < H; y += 2)
    for (int x = 0; x < 200; x += 2) {
      uint32_t pl = px(a, x, y), pr = px(a, W - 1 - x, y);
      black += pl == 0;
      if ((pl >> 8 & 0xff) > (l >> 8 & 0xff))
        l = pl;
      if ((pr >> 8 & 0xff) > (r >> 8 & 0xff))
        r = pr;
    }
  assert(black > 1000 && l != 0 && r != 0);
  assert((l >> 16) > (r >> 16) && (l & 0xff) < (r & 0xff));

  /* The same map again from scratch; another seed, another map. */
  wp_render_finish();
  cairo_surface_t *b = render(c, f, dir, NULL);
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3)
      assert(px(a, x, y) == px(b, x, y));
  cairo_surface_destroy(b);
  other = *c;
  other.topo_seed++;
  b = render(&other, f, dir, NULL);
  int differ = 0;
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3)
      differ += px(a, x, y) != px(b, x, y);
  assert(differ > 1000);
  cairo_surface_destroy(b);

  /* Glow and overspeed change only the logo tile. */
  struct wp_rect rg[WP_REGIONS];
  wp_regions(c, W, H, false, false, rg);
  struct wp_frame g = f;
  g.glow = g.over = 1;
  g.dir = 1;
  b = render(c, g, dir, "wallpaper_topo_overspeed");
  int changed = 0;
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3)
      if (px(a, x, y) != px(b, x, y)) {
        assert(x >= rg[1].x && x < rg[1].x + rg[1].w);
        assert(y >= rg[1].y && y < rg[1].y + rg[1].h);
        changed++;
      }
  assert(changed > 0);
  cairo_surface_destroy(b);

  /* A partial repaint over an older frame, pulsed and slid, equals a full
   * one: the map under the logo, its shadow and the bloom all come back
   * right. (A shake moves the map too, so it is repainted whole.) */
  struct wp_frame h = {20, 0.9, 2.0, .glow = 0.5, .over = 0.7, .dir = 1,
                       .pulse = 1};
  for (int i = 0; i < 6; ++i)
    h.slide[i] = c->react_slide_dist;
  cairo_t *cr = cairo_create(a);
  wp_render(cr, c, W, H, &h, false, false, false);
  cairo_destroy(cr);
  b = render(c, h, dir, "wallpaper_topo_pulsed");
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3)
      assert(px(a, x, y) == px(b, x, y));
  cairo_surface_destroy(a);
  cairo_surface_destroy(b);

  /* Shaken: the lines move with everything else, by whole pixels. */
  a = render(c, f, dir, NULL);
  struct wp_frame sh = f;
  sh.shake_x = 9;
  sh.shake_y = -6;
  b = render(c, sh, dir, "wallpaper_topo_shaken");
  int moved = 0;
  for (int y = 100; y < H - 100; y += 3)
    for (int x = 100; x < 1000; x += 3) { /* clear of the logo and bar */
      assert(px(b, x + 9, y - 6) == px(a, x, y));
      moved += px(b, x, y) != px(a, x, y);
    }
  assert(moved > 100);
  cairo_surface_destroy(a);
  cairo_surface_destroy(b);

  /* A window open: the map gone, only the snowflake on black. */
  a = render(c, f, dir, NULL);
  struct wp_frame gone = f;
  gone.map_fade = 1;
  b = render(c, gone, dir, "wallpaper_topo_window");
  int lines = 0;
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3) {
      bool logo = x >= rg[1].x && x < rg[1].x + rg[1].w && y >= rg[1].y &&
                  y < rg[1].y + rg[1].h;
      bool bar = y < 40;
      if (!logo && !bar) {
        assert(px(b, x, y) == 0);
        lines += px(a, x, y) != 0;
      }
    }
  assert(lines > 1000);
  /* With the window the snowflake is toned down: far dimmer. */
  long lit[2] = {0};
  for (int y = rg[1].y; y < rg[1].y + rg[1].h; y += 2)
    for (int x = rg[1].x; x < rg[1].x + rg[1].w; x += 2)
      for (int i = 0; i < 2; ++i) {
        uint32_t v = px(i ? b : a, x, y);
        lit[i] += ((v >> 16) & 0xff) + ((v >> 8) & 0xff) + (v & 0xff);
      }
  fprintf(stderr, "snowflake light: %ld empty, %ld with a window\n", lit[0],
          lit[1]);
  assert(lit[1] * 2 < lit[0]);
  cairo_surface_destroy(a);
  cairo_surface_destroy(b);

  /* The map on the GPU's surface underneath: the buffer stays clear
   * around the snowflake, and the snowflake's glass lets it through. */
  cairo_surface_t *u = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
  cr = cairo_create(u);
  struct wp_frame uf = f;
  uf.map_under = true;
  wp_render(cr, c, W, H, &uf, true, false, false);
  cairo_destroy(cr);
  cairo_surface_flush(u);
  const uint32_t *ud = (const uint32_t *)cairo_image_surface_get_data(u);
  int us = cairo_image_surface_get_stride(u) / 4;
  assert(ud[100 * us + 100] == 0 && ud[(H - 10) * us + W - 10] == 0);
  /* inside a lambda's glass: partly see-through */
  uint32_t glass = ud[(H / 2) * us + W / 2 + (int)(0.6 * c->logo_radius)];
  int ga = glass >> 24;
  assert(ga > 0 && ga < 255);

  /* The snowflake on the GPU's surface above: the buffer clear where it
   * was, the hexagon (cover, progress) and the speed lines still drawn. */
  cairo_surface_t *lg = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
  cr = cairo_create(lg);
  struct wp_frame lf = uf;
  lf.logo_gpu = true;
  lf.media_alpha = 1;
  lf.progress = 0.4;
  lf.over = 1;
  lf.dir = 1;
  wp_render(cr, c, W, H, &lf, true, false, false);
  cairo_destroy(cr);
  cairo_surface_flush(lg);
  const uint32_t *ld = (const uint32_t *)cairo_image_surface_get_data(lg);
  assert(ld[(H / 2) * us + W / 2 + (int)(0.6 * c->logo_radius)] == 0);
  int hex = 0, streak = 0;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      double d = hypot(x - W / 2.0, y - H / 2.0);
      bool on = ld[y * us + x] >> 24;
      hex += on && d < 0.4 * c->logo_radius;
      streak += on && d > 1.05 * c->logo_radius && d < 1.3 * c->logo_radius;
    }
  assert(hex > 100 && streak > 100);
  /* Its regions: just the hexagon's box in the middle, or the whole logo
   * box while a buffer holds the CPU's snowflake. */
  struct wp_rect rl[WP_REGIONS], rb[WP_REGIONS];
  wp_regions_ex(c, W, H, false, false, true, false, rl);
  wp_regions_ex(c, W, H, false, false, true, true, rb);
  assert(rl[1].w < 0.5 * rg[1].w && rl[1].w > 0.7 * c->logo_radius);
  assert(memcmp(&rb[1], &rg[1], sizeof rb[1]) == 0);
  struct wp_rect box = wp_logo_box(c, W, H, NULL);
  assert(box.x <= rg[1].x + rg[1].w / 2 - c->logo_radius * 1.1 &&
         box.w < rg[1].w);
  /* A partial repaint over a buffer that held the CPU's snowflake (with
   * logo_box) clears it: the same as a whole one. */
  cairo_surface_t *lp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
  cr = cairo_create(lp);
  wp_render(cr, c, W, H, &uf, true, false, false);
  struct wp_frame lb = lf;
  lb.logo_box = true;
  wp_render(cr, c, W, H, &lb, false, false, false);
  cairo_destroy(cr);
  cairo_surface_flush(lp);
  const uint32_t *pd = (const uint32_t *)cairo_image_surface_get_data(lp);
  for (int y = 0; y < H; y += 2)
    for (int x = 0; x < W; x += 2)
      assert(pd[y * us + x] == ld[y * us + x]);
  cairo_surface_destroy(lp);
  cairo_surface_destroy(lg);
  cairo_surface_destroy(u);

  /* What's playing, readable over the lines: bright text in a dark glow. */
  struct wp_frame tf = f;
  tf.media_alpha = 1;
  tf.title = "A Fairly Long Song Title";
  tf.artist = "Some Artist";
  b = render(c, tf, dir, "wallpaper_topo_text");
  cairo_surface_destroy(b);

  /* The progress line in the outline's colours. */
  tf.progress = 0.4;
  b = render(c, tf, dir, "wallpaper_topo_progress");
  cairo_surface_destroy(b);

  test_shows(c, dir, "wallpaper_topo");
  test_topo_shows(c, dir);
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : ".";
  /* The classic style; the topo one is tested on its own. */
  struct wallpaper_config classic =
      *(const struct wallpaper_config *)wallpaper_module.defaults;
  strcpy(classic.style, "classic");
  const struct wallpaper_config *c = &classic;

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
  wp_regions(c, W, H, false, false, r);
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

  /* battery_bar off: nothing in the bar's box. */
  {
    struct wallpaper_config nb = *c;
    nb.battery_bar = false;
    f = (struct wp_frame){.percent = 100, .level = 1, .angle = 0};
    cairo_surface_t *on = render(c, f, dir, NULL);
    cairo_surface_t *off = render(&nb, f, dir, NULL);
    int lit = 0;
    for (int y = r[0].y; y < r[0].y + r[0].h; ++y)
      for (int x = r[0].x; x < r[0].x + r[0].w; ++x) {
        lit += px(on, x, y) != 0;
        assert(px(off, x, y) == 0);
      }
    assert(lit > 0);
    cairo_surface_destroy(on);
    cairo_surface_destroy(off);
  }

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
  wp_render(cr, c, W, H, &g, false, false, false);
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
  wp_regions(rg, W, H, true, false, big);
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
  wp_render(cr, rg, W, H, &f, false, false, false);
  cairo_destroy(cr);
  b = render(rg, f, dir, NULL);
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3)
      assert(px(a, x, y) == px(b, x, y));
  cairo_surface_destroy(b);
  f.nbars = 0;
  cr = cairo_create(a);
  wp_render(cr, rg, W, H, &f, false, true, false);
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
  wp_regions(c, W, H, true, false, e);
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
  /* The fill fades in from the side, and a brighter line traces the curve
   * at the level (left 0.5, the bass bar), but never down the side. */
  {
    int tip = (int)(gx + elen * 0.5);
    unsigned near = px(t, gx + 4, ey) & 0xff, far = px(t, tip - 8, ey) & 0xff;
    unsigned rim = 0;
    for (int x = tip - 3; x <= tip + 3; ++x)
      rim = fmax(rim, px(t, x, ey) & 0xff);
    assert(near > far && far > 0 && rim > near);
    struct wp_frame qf = f;
    for (int i = 0; i < qf.nbars; ++i)
      qf.bars[0][i] = qf.bars[1][i] = 0;
    cairo_surface_t *qt = render(c, qf, dir, NULL);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < 4; ++x)
        assert(px(qt, gx + x, y) == px(a, gx + x, y) &&
               px(qt, W - 1 - gx - x, y) == px(a, W - 1 - gx - x, y));
    cairo_surface_destroy(qt);
    /* Something music-like, to judge the look by. */
    struct wp_frame mf = f;
    for (int i = 0; i < mf.nbars; ++i) {
      double base = 0.85 - 0.6 * i / mf.nbars;
      mf.bars[0][i] = (float)(base + 0.15 * sin(i * 0.9));
      mf.bars[1][i] = (float)(base * 0.9 + 0.15 * sin(i * 1.3 + 1));
    }
    cairo_surface_destroy(render(c, mf, dir, "wallpaper_viz_edges_music"));
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
  wp_render(cr, c, W, H, &f, false, false, false);
  cairo_destroy(cr);
  b = render(c, f, dir, NULL);
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3)
      assert(px(t, x, y) == px(b, x, y));
  cairo_surface_destroy(b);
  f.nbars = 0;
  cr = cairo_create(t);
  wp_render(cr, c, W, H, &f, false, true, false);
  cairo_destroy(cr);
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3)
      assert(px(t, x, y) == px(a, x, y));
  cairo_surface_destroy(a);
  cairo_surface_destroy(t);

  /* The logo reacting: a pulse, slid lambdas and the glow change only the
   * small logo tile, and a partial repaint over them equals a full one. */
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
  wp_render(cr, c, W, H, &f, false, false, false);
  cairo_destroy(cr);
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3)
      assert(px(a, x, y) == px(t, x, y));
  cairo_surface_destroy(a);
  cairo_surface_destroy(t);

  /* A shake moves the bar and the logo (with its bloom and pulse) by whole
   * pixels, all inside the regions, and a partial repaint equals a full
   * one, whichever way it shakes. */
  for (int d = -1; d <= 1; d += 2) {
    double sh = c->react_shake_px * d;
    f = (struct wp_frame){.percent = 50, .level = 1, .angle = 0.3,
                          .pulse = 1, .over = 1, .dir = 1};
    a = render(c, f, dir, NULL);
    f.shake_x = sh;
    f.shake_y = fabs(sh); /* down: up would push the bar off the top */
    t = render(c, f, dir, d > 0 ? "wallpaper_shake" : NULL);
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
    wp_render(cr, c, W, H, &f, false, false, false);
    cairo_destroy(cr);
    for (int y = 0; y < H; y += 3)
      for (int x = 0; x < W; x += 3)
        assert(px(a, x, y) == px(t, x, y));
    cairo_surface_destroy(a);
    cairo_surface_destroy(t);
  }

  /* What's playing: playerctl's line. */
  struct wp_media m;
  assert(wp_media_parse("Playing\t240000000\t60000000\tfile:///a.png\tArtist"
                        "\tA\tsong\n",
                        1000, &m));
  assert(m.playing && m.len_us == 240000000 && m.pos_us == 60000000);
  assert(!strcmp(m.art_url, "file:///a.png"));
  assert(!strcmp(m.artist, "Artist") && !strcmp(m.title, "A\tsong"));
  assert(fabs(wp_media_progress(&m, 1000) - 0.25) < 1e-9);
  assert(fabs(wp_media_progress(&m, 13000) - 0.3) < 1e-9); /* playing on */
  assert(wp_media_progress(&m, 1000 + 400000) == 1);       /* clamped */
  assert(wp_media_parse("Paused\t240000000\t60000000\t\t\tTitle", 0, &m));
  assert(!m.playing && !m.artist[0] && !m.art_url[0]);
  assert(fabs(wp_media_progress(&m, 50000) - 0.25) < 1e-9); /* held */
  assert(wp_media_parse("Playing\t\t32\t\timase\tNIGHT DANCER", 0, &m));
  assert(m.len_us == 0 && wp_media_progress(&m, 0) == -1); /* unknown */
  assert(!wp_media_parse("Playing\t1\t0\t\tArtist\t", 0, &m) && !m.has);
  assert(!wp_media_parse("Stopped\t1\t0\t\tArtist\tTitle", 0, &m));
  assert(!wp_media_parse("Playing\t1\t0\tArtist\tTitle", 0, &m)); /* old */
  assert(!wp_media_parse("", 0, &m) && !m.has);
  assert(!wp_media_parse("No players found", 0, &m));

  /* A cover: cut to its centred square and turned grey (luminance). */
  {
    cairo_surface_t *in = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 300, 200);
    cairo_t *ic = cairo_create(in);
    cairo_set_source_rgb(ic, 1, 0, 0);
    cairo_paint(ic);
    cairo_rectangle(ic, 150, 0, 150, 200);
    cairo_set_source_rgb(ic, 0, 0, 1);
    cairo_fill(ic);
    cairo_destroy(ic);
    char path[1024];
    snprintf(path, sizeof path, "%s/wallpaper_cover_in.png", dir);
    assert(cairo_surface_write_to_png(in, path) == CAIRO_STATUS_SUCCESS);
    cairo_surface_destroy(in);
    cairo_surface_t *art = art_load(path, true);
    assert(art && cairo_image_surface_get_width(art) == 200 &&
           cairo_image_surface_get_height(art) == 200);
    assert(px(art, 10, 100) == 0x363636 && px(art, 190, 100) == 0x121212);
    assert(px(art, 99, 100) == 0x363636 && px(art, 100, 100) == 0x121212);
    cairo_surface_destroy(art);
    snprintf(path, sizeof path, "%s/no-such-cover.png", dir);
    assert(!art_load(path, true));
    /* A JPEG (a 400x250 one cut to 250x250), and broken files. */
    snprintf(path, sizeof path, "%s/wallpaper_cover_in.jpg", dir);
    write_jpeg(path, 400, 250, 200, 100, 50);
    art = art_load(path, true);
    assert(art && cairo_image_surface_get_width(art) == 250);
    int want = (int)lround(0.2126 * 200 + 0.7152 * 100 + 0.0722 * 50);
    assert(abs((int)(px(art, 125, 125) & 0xff) - want) <= 3);
    cairo_surface_destroy(art);
    FILE *jf = fopen(path, "r+b");
    assert(jf && fseek(jf, 20, SEEK_SET) == 0);
    for (int i = 0; i < 200; ++i)
      fputc(0, jf); /* the header's tables zeroed */
    fclose(jf);
    assert(!art_load(path, true) || 1); /* no crash either way */
    snprintf(path, sizeof path, "%s/wallpaper_cover_bad.png", dir);
    jf = fopen(path, "wb");
    fputs("\x89PNG\r\n\x1a\nnot really", jf);
    fclose(jf);
    assert(!art_load(path, true));
  }

  /* The details: under the logo inside their region, which is empty
   * without them, and the hexagon inside the logo tile. */
  struct wp_rect tr[WP_REGIONS];
  wp_regions(c, W, H, false, true, tr);
  assert(r[4].w == 0 && tr[4].w > 0);
  assert(tr[4].y > H / 2 + c->logo_radius * k && tr[4].y + tr[4].h < H);
  assert(tr[4].x > e[2].x + e[2].w && tr[4].x + tr[4].w < e[3].x);
  /* A cover with something to see: rings and a diagonal. */
  cairo_surface_t *cover = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 256, 256);
  {
    cairo_t *ic = cairo_create(cover);
    cairo_set_source_rgb(ic, 0.9, 0.9, 0.9);
    cairo_paint(ic);
    cairo_set_source_rgb(ic, 0.3, 0.3, 0.3);
    for (int i = 1; i < 6; i += 2) {
      cairo_arc(ic, 128, 128, i * 22, 0, 2 * M_PI);
      cairo_set_line_width(ic, 14);
      cairo_stroke(ic);
    }
    cairo_destroy(ic);
  }
  cairo_surface_t *white = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 64, 64);
  {
    cairo_t *ic = cairo_create(white);
    cairo_set_source_rgb(ic, 1, 1, 1);
    cairo_paint(ic);
    cairo_destroy(ic);
  }
  f = (struct wp_frame){.percent = 50, .level = 1, .angle = 0.3};
  a = render(c, f, dir, NULL);
  f.media_alpha = 1;
  f.title = "Dynasties and Dystopia (from the series Arcane League of "
            "Legends) and a much longer tail so that it has to be cut short";
  f.artist = "Release - Topic";
  f.progress = 0.4;
  f.art = cover;
  f.art_id = 1;
  t = render(c, f, dir, "wallpaper_media_progress");
  changed = 0;
  int texted = 0;
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3)
      if (px(a, x, y) != px(t, x, y)) {
        bool in_logo = x >= r[1].x && x < r[1].x + r[1].w && y >= r[1].y &&
                       y < r[1].y + r[1].h;
        bool in_text = x >= tr[4].x && x < tr[4].x + tr[4].w &&
                       y >= tr[4].y && y < tr[4].y + tr[4].h;
        assert(in_logo || in_text);
        texted += in_text && !in_logo;
        changed++;
      }
  assert(changed > 0 && texted > 0);
  cairo_surface_destroy(t);
  /* The line: clockwise round the hexagon's border from the middle of its
   * top edge to 40% of the way, the rest of the border the track, the cover
   * inside at media_art_alpha; all turning with the snowflake. */
  double hl = 0.39046 * c->media_hex_size * c->logo_radius * k -
              c->media_line_width * k / 2;
  uint32_t line = c->media_line >> 8, track = c->media_track >> 8;
  unsigned art_v = (unsigned)lround(255 * c->media_art_alpha);
  f.art = white;
  f.art_id = 2;
  for (double ang = 0; ang < 7; ang += 2.2) {
    f.angle = ang;
    t = render(c, f, dir, NULL);
    int x, y;
    rim(hl, 0.01, ang, &x, &y);
    assert(px(t, x, y) == line);
    rim(hl, 0.2, ang, &x, &y);
    assert(px(t, x, y) == line);
    rim(hl, 0.39, ang, &x, &y);
    assert(px(t, x, y) == line);
    rim(hl, 0.41, ang, &x, &y);
    assert(px(t, x, y) == track);
    rim(hl, 0.7, ang, &x, &y);
    assert(px(t, x, y) == track);
    rim(hl * 0.5, 0.7, ang, &x, &y); /* inside: the cover */
    assert(abs((int)(px(t, x, y) & 0xff) - (int)art_v) <= 1);
    cairo_surface_destroy(t);
  }
  /* Without a cover (or with media_art off) the inside is the hole. */
  f.angle = 0.3;
  f.art = NULL;
  struct wallpaper_config noart = *c;
  noart.media_art = false;
  for (int n = 0; n < 2; ++n) {
    if (n)
      f.art = white;
    t = render(n ? &noart : c, f, dir, NULL);
    int x, y;
    rim(hl, 0.2, 0.3, &x, &y);
    assert(px(t, x, y) == line);
    rim(hl * 0.5, 0.2, 0.3, &x, &y);
    assert(px(t, x, y) == 0);
    cairo_surface_destroy(t);
  }
  f.art = white;
  f.angle = 0;
  /* Line width 0: no line, no track. */
  struct wallpaper_config noline = *c;
  noline.media_line_width = 0;
  t = render(&noline, f, dir, NULL);
  {
    int x, y;
    rim(hl, 0.04, 0, &x, &y);
    assert(px(t, x, y) == art_v * 0x010101u);
  }
  cairo_surface_destroy(t);
  f.art = cover;
  f.art_id = 1;
  f.angle = 0.3;
  f.title = "NIGHT DANCER";
  f.artist = "imase";
  struct wp_frame pf = f;
  pf.media_alpha = c->media_paused_alpha;
  pf.progress = 0.7;
  cairo_surface_destroy(render(c, pf, dir, "wallpaper_media_paused"));
  /* A partial repaint over other details, shaken and pulsed, equals a full
   * one; clearing them leaves the plain frame. */
  t = render(c, pf, dir, NULL);
  struct wp_frame cf = f;
  cf.shake_x = cf.shake_y = c->react_shake_px;
  cf.pulse = 1;
  cr = cairo_create(t);
  wp_render(cr, c, W, H, &cf, false, false, false);
  cairo_destroy(cr);
  b = render(c, cf, dir, NULL);
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3)
      assert(px(t, x, y) == px(b, x, y));
  cairo_surface_destroy(b);
  f = (struct wp_frame){.percent = 50, .level = 1, .angle = 0.3};
  cr = cairo_create(t);
  wp_render(cr, c, W, H, &f, false, false, true);
  cairo_destroy(cr);
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3)
      assert(px(t, x, y) == px(a, x, y));
  cairo_surface_destroy(a);
  cairo_surface_destroy(t);
  test_media_kept(c, cover);
  test_media_kept(wallpaper_module.defaults, cover);
  cairo_surface_destroy(cover);
  cairo_surface_destroy(white);

  test_shows(c, dir, "wallpaper");
  test_logo_sdf();
  test_cover_step();
  test_topo(dir);
  wp_render_finish();
  return 0;
}
