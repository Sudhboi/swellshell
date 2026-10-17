/* The GPU's smooth edge visualiser against cairo's: both draw the same
 * frames at 2880x1800 and the strips must match (antialiasing aside).
 * Skips (77) without a usable EGL device.
 * Usage: test_wallpaper_gl [outdir]   (default: current directory) */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <GLES3/gl3.h>

#include "module.h"
#include "wallpaper/wallpaper.h"

extern const struct swell_module wallpaper_module;

#define W 2880
#define H 1800

struct diff {
  int max;       /* largest channel difference, 0..255 */
  double mean;   /* per channel */
  long over;     /* pixels off by more than 24 in some channel */
  long lit_cpu, lit_gpu; /* pixels brighter than the background */
};

/* Draw side s with the GPU over strip rect r and compare it, over the
 * background, with cairo's frame ref. Writes the GPU's strip as a PNG. */
static struct diff compare(const struct wallpaper_config *c,
                           const struct wp_frame *f, cairo_surface_t *ref,
                           int s, struct wp_rect r, const char *dir,
                           const char *name) {
  GLuint fbo, rb;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glGenRenderbuffers(1, &rb);
  glBindRenderbuffer(GL_RENDERBUFFER, rb);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, r.w, r.h);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_RENDERBUFFER, rb);
  assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
  wp_gl_edge_draw(c, W, H, f, s, r.x, r.y, 1, 1, r.w, r.h);
  unsigned char *px = malloc((size_t)r.w * r.h * 4);
  glReadPixels(0, 0, r.w, r.h, GL_RGBA, GL_UNSIGNED_BYTE, px);
  assert(glGetError() == GL_NO_ERROR);
  glDeleteFramebuffers(1, &fbo);
  glDeleteRenderbuffers(1, &rb);

  cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_RGB24, r.w, r.h);
  cairo_surface_flush(ref);
  const unsigned char *rd = cairo_image_surface_get_data(ref);
  int rs = cairo_image_surface_get_stride(ref);
  unsigned char *od = cairo_image_surface_get_data(out);
  int os = cairo_image_surface_get_stride(out);
  uint32_t bg = c->background;
  int bgc[3] = {(bg >> 24) & 0xff, (bg >> 16) & 0xff, (bg >> 8) & 0xff};
  struct diff d = {0};
  double sum = 0;
  for (int y = 0; y < r.h; ++y)
    for (int x = 0; x < r.w; ++x) {
      /* GL rows run bottom-up. */
      const unsigned char *g = px + ((size_t)(r.h - 1 - y) * r.w + x) * 4;
      uint32_t cv = *(const uint32_t *)(rd + (r.y + y) * rs + (r.x + x) * 4);
      int cc[3] = {(cv >> 16) & 0xff, (cv >> 8) & 0xff, cv & 0xff};
      int gc[3], worst = 0;
      for (int k = 0; k < 3; ++k) {
        gc[k] = g[k] + bgc[k] * (255 - g[3]) / 255;
        int e = abs(gc[k] - cc[k]);
        worst = e > worst ? e : worst;
        sum += e;
      }
      d.max = worst > d.max ? worst : d.max;
      d.over += worst > 24;
      d.lit_cpu += cc[0] != bgc[0] || cc[1] != bgc[1] || cc[2] != bgc[2];
      d.lit_gpu += gc[0] != bgc[0] || gc[1] != bgc[1] || gc[2] != bgc[2];
      *(uint32_t *)(od + y * os + x * 4) = gc[0] << 16 | gc[1] << 8 | gc[2];
    }
  d.mean = sum / ((double)r.w * r.h * 3);
  cairo_surface_mark_dirty(out);
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s.png", dir, name);
  assert(cairo_surface_write_to_png(out, path) == CAIRO_STATUS_SUCCESS);
  cairo_surface_destroy(out);
  free(px);
  return d;
}

static cairo_surface_t *render(const struct wallpaper_config *c,
                               const struct wp_frame *f) {
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W, H);
  cairo_t *cr = cairo_create(s);
  wp_render(cr, c, W, H, f, true, false, false);
  cairo_destroy(cr);
  return s;
}

/* Both sides of f must match cairo's. */
static void check(const struct wallpaper_config *c, const struct wp_frame *f,
                  const char *dir, const char *name) {
  cairo_surface_t *ref = render(c, f);
  struct wp_rect r[WP_REGIONS];
  wp_regions(c, W, H, true, false, r);
  for (int s = 0; s < 2; ++s) {
    char n[128];
    snprintf(n, sizeof(n), "%s_%s", name, s ? "right" : "left");
    struct diff d = compare(c, f, ref, s, r[2 + s], dir, n);
    long px = (long)r[2 + s].w * r[2 + s].h;
    printf("%-28s max %3d  mean %.3f  off %ld/%ld  lit cpu %ld gpu %ld\n", n,
           d.max, d.mean, d.over, px, d.lit_cpu, d.lit_gpu);
    /* Only the antialiased boundary may differ, and little. */
    assert(d.mean < 0.25);
    assert(d.over * 1000 < px);
    assert(labs(d.lit_cpu - d.lit_gpu) * 100 <= d.lit_cpu / 2 + 200);
  }
  cairo_surface_destroy(ref);
}

/* The topo map drawn by the GPU, as an RGB24 surface (written as a PNG). */
static cairo_surface_t *gpu_map(const struct wallpaper_config *c,
                                const struct wp_frame *f, const char *dir,
                                const char *name) {
  GLuint fbo, rb;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glGenRenderbuffers(1, &rb);
  glBindRenderbuffer(GL_RENDERBUFFER, rb);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_RENDERBUFFER, rb);
  assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
  wp_gl_map_draw(c, W, H, f, 0, 0, 1, 1, W, H);
  unsigned char *px = malloc((size_t)W * H * 4);
  glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
  assert(glGetError() == GL_NO_ERROR);
  glDeleteFramebuffers(1, &fbo);
  glDeleteRenderbuffers(1, &rb);
  cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W, H);
  unsigned char *od = cairo_image_surface_get_data(out);
  int os = cairo_image_surface_get_stride(out);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const unsigned char *g = px + ((size_t)(H - 1 - y) * W + x) * 4;
      *(uint32_t *)(od + y * os + x * 4) = g[0] << 16 | g[1] << 8 | g[2];
    }
  cairo_surface_mark_dirty(out);
  free(px);
  if (name) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.png", dir, name);
    assert(cairo_surface_write_to_png(out, path) == CAIRO_STATUS_SUCCESS);
  }
  return out;
}

static int lum(cairo_surface_t *s, int x, int y) {
  uint32_t v = ((const uint32_t *)(cairo_image_surface_get_data(s) +
                                   y * cairo_image_surface_get_stride(s)))[x];
  return ((v >> 16) & 0xff) + ((v >> 8) & 0xff) + (v & 0xff);
}

/* The GPU's map against cairo's: the same lines (antialiasing and the
 * smoothing aside); a ring moves them only where it passes; dimmed, black. */
static void check_map(const char *dir) {
  const struct wallpaper_config *c = wallpaper_module.defaults;
  struct wp_frame f = {.percent = 50, .level = 1};
  cairo_surface_t *gm = gpu_map(c, &f, dir, "wallpaper_gl_map");
  cairo_surface_t *cm = wp_topo_background(c, W, H);
  cairo_surface_flush(cm);
  /* Lit pixels: most of each one's lit pixels have a lit one of the other
   * close by. */
  long lit[2] = {0}, near[2] = {0};
  for (int y = 4; y < H - 4; y += 3)
    for (int x = 4; x < W - 4; x += 3)
      for (int s = 0; s < 2; ++s) {
        cairo_surface_t *a = s ? cm : gm, *b = s ? gm : cm;
        if (lum(a, x, y) < 120)
          continue;
        lit[s]++;
        bool found = false;
        for (int dy = -3; dy <= 3 && !found; ++dy)
          for (int dx = -3; dx <= 3 && !found; ++dx)
            found = lum(b, x + dx, y + dy) >= 60;
        near[s] += found;
      }
  fprintf(stderr, "map lit gpu %ld (%ld near cpu), cpu %ld (%ld near gpu)\n", lit[0],
         near[0], lit[1], near[1]);
  assert(lit[0] > 1000 && lit[1] > 1000);
  assert(near[0] * 100 > lit[0] * 95 && near[1] * 100 > lit[1] * 90);
  assert(labs(lit[0] - lit[1]) * 4 < lit[1]);

  /* A ring 600 px out: lines move near it, not near the centre or far out. */
  struct wp_frame rf = f;
  rf.nrings = 1;
  rf.rings[0].radius = 600;
  rf.rings[0].size = 1;
  cairo_surface_t *rm = gpu_map(c, &rf, dir, "wallpaper_gl_map_ring");
  long in = 0, out = 0;
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3) {
      double r = hypot(x - W / 2.0, y - H / 2.0);
      bool differ = abs(lum(gm, x, y) - lum(rm, x, y)) > 60;
      if (fabs(r - 600) < 100)
        in += differ;
      else if (r < 250 || r > 1100)
        out += differ;
    }
  fprintf(stderr, "ring: %ld changed in its band, %ld away\n", in, out);
  assert(in > 500 && out == 0);
  cairo_surface_destroy(rm);

  /* Shaken: the same lines, moved by whole pixels (the derivatives' pixel
   * pairs shift, so antialiasing may differ a little), and lines along the
   * edges it uncovers, from past them. */
  struct wp_frame sf = f;
  sf.shake_x = 9;
  sf.shake_y = -6;
  cairo_surface_t *sm = gpu_map(c, &sf, dir, NULL);
  long off = 0, n = 0;
  for (int y = 0; y < H - 6; y += 3)
    for (int x = 0; x < W - 9; x += 3, ++n)
      off += abs(lum(sm, x + 9, y) - lum(gm, x, y + 6)) > 90;
  int edge = 0;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < 9; ++x)
      edge += lum(sm, x, y) > 60;
  fprintf(stderr, "shaken: %ld of %ld off, %d lit in the uncovered edge\n",
          off, n, edge);
  assert(off * 1000 < n && edge > 20);
  cairo_surface_destroy(sm);

  /* Faded out by a window: black. */
  struct wp_frame df = f;
  df.map_fade = 1;
  cairo_surface_t *dm = gpu_map(c, &df, dir, NULL);
  for (int y = 0; y < H; y += 7)
    for (int x = 0; x < W; x += 7)
      assert(lum(dm, x, y) == 0);
  cairo_surface_destroy(dm);
  cairo_surface_destroy(gm);

  /* Turned a quarter: the left colour (peach) at the top, the right one
   * (blue) at the bottom, on both. */
  struct wallpaper_config tc = *c;
  tc.topo_angle = 90;
  cairo_surface_t *tm[2] = {gpu_map(&tc, &f, dir, "wallpaper_gl_map_turned"),
                            wp_topo_background(&tc, W, H)};
  cairo_surface_flush(tm[1]);
  for (int s = 0; s < 2; ++s) {
    long warm[2] = {0}, cool[2] = {0}; /* top, bottom quarter */
    for (int y = 0; y < H; y += 2)
      for (int x = 0; x < W; x += 2) {
        int q = y < H / 4 ? 0 : y >= H * 3 / 4 ? 1 : -1;
        uint32_t v = ((const uint32_t *)(cairo_image_surface_get_data(tm[s]) +
                                         y * cairo_image_surface_get_stride(
                                                 tm[s])))[x];
        int r = (v >> 16) & 0xff, b = v & 0xff;
        if (q < 0 || lum(tm[s], x, y) < 120)
          continue;
        warm[q] += r > b + 40;
        cool[q] += b > r + 40;
      }
    fprintf(stderr, "turned %s: top %ld warm %ld cool, bottom %ld warm %ld "
            "cool\n", s ? "cpu" : "gpu", warm[0], cool[0], warm[1], cool[1]);
    assert(warm[0] > 10 * (cool[0] + 1) && cool[1] > 10 * (warm[1] + 1));
  }
  cairo_surface_destroy(tm[0]);
}

/* The GPU's snowflake over the logo box of a flat grey map (dimmed as the
 * frame dims the map), as an RGB24 surface of the whole buffer, black
 * outside the box. Written as a PNG (the box only) when name is given. */
static cairo_surface_t *gpu_logo(const struct wallpaper_config *c,
                                 const struct wp_frame *f, const char *dir,
                                 const char *name) {
  struct wp_rect r = wp_logo_box(c, W, H, f);
  GLuint fbo, rb;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glGenRenderbuffers(1, &rb);
  glBindRenderbuffer(GL_RENDERBUFFER, rb);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, r.w, r.h);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_RENDERBUFFER, rb);
  assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
  wp_gl_logo_draw(c, W, H, f, r.x, r.y, 1, 1, r.w, r.h);
  unsigned char *px = malloc((size_t)r.w * r.h * 4);
  glReadPixels(0, 0, r.w, r.h, GL_RGBA, GL_UNSIGNED_BYTE, px);
  assert(glGetError() == GL_NO_ERROR);
  glDeleteFramebuffers(1, &fbo);
  glDeleteRenderbuffers(1, &rb);
  int bg = (int)lround(((c->topo_bg_left >> 24) & 0xff) * wp_map_brightness(f));
  cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W, H);
  unsigned char *od = cairo_image_surface_get_data(out);
  int os = cairo_image_surface_get_stride(out);
  for (int y = 0; y < r.h; ++y)
    for (int x = 0; x < r.w; ++x) {
      /* Premultiplied over the map, as the compositor does it. */
      const unsigned char *g = px + ((size_t)(r.h - 1 - y) * r.w + x) * 4;
      int v[3];
      for (int k = 0; k < 3; ++k) {
        v[k] = g[k] + bg * (255 - g[3]) / 255;
        v[k] = v[k] > 255 ? 255 : v[k];
      }
      *(uint32_t *)(od + (r.y + y) * os + (r.x + x) * 4) =
          v[0] << 16 | v[1] << 8 | v[2];
    }
  cairo_surface_mark_dirty(out);
  free(px);
  if (name) {
    cairo_surface_t *box = cairo_surface_create_for_rectangle(out, r.x, r.y,
                                                              r.w, r.h);
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.png", dir, name);
    assert(cairo_surface_write_to_png(box, path) == CAIRO_STATUS_SUCCESS);
    cairo_surface_destroy(box);
  }
  return out;
}

/* Sum of the channels' light over the logo box. */
static long box_light(cairo_surface_t *s, struct wp_rect r) {
  long sum = 0;
  for (int y = r.y; y < r.y + r.h; ++y)
    for (int x = r.x; x < r.x + r.w; ++x)
      sum += lum(s, x, y);
  return sum;
}

/* The GPU's snowflake of frame f against cairo's (over the same grey map):
 * nearly every pixel agrees, and the outline's colours sit alike (pink at
 * the top left, blue at the bottom right). */
static void compare_logo(const struct wallpaper_config *c,
                         const struct wp_frame *f, const char *dir,
                         const char *name) {
  struct wp_rect r = wp_logo_box(c, W, H, f);
  double dx, dy;
  wp_logo_offset(c, W, H, f, &dx, &dy);
  cairo_surface_t *gs = gpu_logo(c, f, dir, name);
  cairo_surface_t *cs = render(c, f);
  int bg = lum(cs, r.x + 1, r.y + 1);
  long changed = 0, off = 0;
  double sum = 0;
  long pink[2] = {0}, blue[2] = {0}; /* [cpu, gpu] at the top left, ... */
  long pink_br[2] = {0}, blue_br[2] = {0}; /* ... and the bottom right */
  for (int y = r.y; y < r.y + r.h; ++y)
    for (int x = r.x; x < r.x + r.w; ++x) {
      uint32_t a = ((const uint32_t *)(cairo_image_surface_get_data(cs) +
                                       y * cairo_image_surface_get_stride(
                                               cs)))[x];
      uint32_t b = ((const uint32_t *)(cairo_image_surface_get_data(gs) +
                                       y * cairo_image_surface_get_stride(
                                               gs)))[x];
      int worst = 0;
      for (int k = 0; k < 24; k += 8) {
        int e = abs((int)((a >> k) & 0xff) - (int)((b >> k) & 0xff));
        worst = e > worst ? e : worst;
        sum += e;
      }
      changed += lum(cs, x, y) != bg || lum(gs, x, y) != bg;
      off += worst > 32;
      for (int s = 0; s < 2; ++s) {
        uint32_t v = s ? b : a;
        int rr = (v >> 16) & 0xff, bb = v & 0xff;
        bool tl = x < W / 2 + dx - 80 && y < H / 2 + dy - 80;
        bool br = x > W / 2 + dx + 80 && y > H / 2 + dy + 80;
        /* Lit outline pixels: pink (red over blue) or blue. */
        if (lum(s ? gs : cs, x, y) < bg + 150)
          continue;
        if (tl) {
          pink[s] += rr > bb + 5;
          blue[s] += bb > rr + 40;
        } else if (br) {
          pink_br[s] += rr > bb + 5;
          blue_br[s] += bb > rr + 40;
        }
      }
    }
  double mean = sum / ((double)r.w * r.h * 3);
  fprintf(stderr, "%-24s off %ld/%ld changed, mean %.3f; top left pink %ld/%ld blue "
         "%ld/%ld, bottom right pink %ld/%ld blue %ld/%ld (cpu/gpu)\n",
         name ? name : "logo", off, changed, mean, pink[0], pink[1], blue[0],
         blue[1], pink_br[0], pink_br[1], blue_br[0], blue_br[1]);
  assert(changed > 1000);
  assert(off * 100 < changed * 5);
  assert(mean < 1.5);
  for (int s = 0; s < 2; ++s)
    assert(pink[s] > 4 * blue[s] && blue_br[s] > 4 * pink_br[s]);
  assert(labs(pink[0] - pink[1]) * 5 < pink[0] &&
         labs(blue_br[0] - blue_br[1]) * 5 < blue_br[0]);
  cairo_surface_destroy(gs);
  cairo_surface_destroy(cs);
}

/* The mean light of the GPU snowflake's pixels 20..30 px outside it, at
 * the top left (side 0) or the bottom right (1), at rest (unturned,
 * unpulsed, unshaken). */
static double outside_light(const struct wallpaper_config *c,
                            cairo_surface_t *s, int side) {
  const struct wp_logo_sdf *sdf = wp_logo_sdf(c, W);
  long sum = 0, n = 0;
  for (int j = 0; j < sdf->h; ++j)
    for (int i = 0; i < sdf->w; ++i) {
      float d = sdf->d[j * sdf->w + i];
      double x = sdf->x + i + 0.5, y = sdf->y + j + 0.5;
      if (d < 20 || d > 30 || (side ? x + y < 100 : x + y > -100))
        continue;
      sum += lum(s, (int)(W / 2 + x), (int)(H / 2 + y));
      ++n;
    }
  assert(n > 100);
  return (double)sum / n;
}

/* The GPU's glass snowflake against cairo's draw_glass_logo(). */
static void check_logo(const char *dir) {
  struct wallpaper_config c =
      *(const struct wallpaper_config *)wallpaper_module.defaults;
  /* A flat grey map, nothing else in the logo's box. */
  c.topo_line_alpha = 0;
  c.topo_bg_left = c.topo_bg_right = 0x505050ffu;
  c.battery_bar = false;
  struct wp_frame f = {.percent = 50, .level = 1};
  compare_logo(&c, &f, dir, "wallpaper_gl_logo");
  struct wp_frame t = f;
  t.angle = 0.3;
  t.glow = 0.6;
  compare_logo(&c, &t, dir, "wallpaper_gl_logo_turned");
  struct wp_frame p = t;
  p.pulse = 1;
  p.shake_x = 9.4;
  p.shake_y = -6.2;
  compare_logo(&c, &p, dir, "wallpaper_gl_logo_pulsed");
  struct wp_frame n = f;
  n.over = 1;
  n.glow = 1;
  compare_logo(&c, &n, dir, "wallpaper_gl_logo_neon");
  struct wp_frame w = f;
  w.map_fade = 0.6;
  compare_logo(&c, &w, dir, "wallpaper_gl_logo_window");
  /* The idle screensaver: dimmed as with a window, off the centre. */
  struct wp_frame v = w;
  v.map_fade = 1;
  v.angle = 0.7;
  v.wander_x = 0.83;
  v.wander_y = -0.61;
  compare_logo(&c, &v, dir, "wallpaper_gl_logo_wander");
  /* At the far corners its box just fits, unclipped. */
  struct wp_rect mid = wp_logo_box(&c, W, H, NULL);
  for (int s = -1; s <= 1; s += 2) {
    struct wp_frame e = f;
    e.wander_x = e.wander_y = s;
    struct wp_rect b = wp_logo_box(&c, W, H, &e);
    assert(b.w == mid.w && b.h == mid.h);
    assert(s < 0 ? b.x >= 0 && b.x <= 3 && b.y >= 0 && b.y <= 3
                 : b.x + b.w <= W && b.x + b.w >= W - 3 &&
                       b.y + b.h <= H && b.y + b.h >= H - 3);
  }

  /* A window open: the snowflake (over black) under half as bright. */
  struct wp_rect r = wp_logo_box(&c, W, H, NULL);
  w.map_fade = 1;
  cairo_surface_t *a = gpu_logo(&c, &f, dir, NULL);
  cairo_surface_t *b = gpu_logo(&c, &w, dir, NULL);
  struct wp_frame black = f;
  black.bg_fade = 1; /* the empty map's light, to take away */
  cairo_surface_t *z = gpu_logo(&c, &black, dir, NULL);
  long lb = box_light(b, r), lz = box_light(z, r);
  fprintf(stderr, "logo light over black: %ld, with a window %ld\n", lz, lb);
  assert(lb > 0 && lb * 2 < lz);
  cairo_surface_destroy(b);
  cairo_surface_destroy(z);

  /* The glow adds light round the outline; without it none, and the
   * shadow darkens the bottom right (its blur barely reaches the top
   * left). */
  b = gpu_logo(&c, &n, dir, NULL);
  int bg = 3 * 0x50;
  double tl = outside_light(&c, a, 0), brl = outside_light(&c, a, 1);
  double ntl = outside_light(&c, b, 0);
  fprintf(stderr, "outside: top left %.1f, bottom right %.1f, lit top left %.1f "
         "(map %d)\n",
         tl, brl, ntl, bg);
  assert(tl <= bg && tl > bg - 4 && brl < bg - 10 && ntl > bg + 20);
  cairo_surface_destroy(a);
  cairo_surface_destroy(b);

  /* In a show (lambdas apart) it draws nothing: the CPU does. */
  struct wp_frame sf = f;
  sf.lambda_fade[2] = 0.5;
  a = gpu_logo(&c, &sf, dir, NULL);
  long lit = 0;
  for (int y = r.y; y < r.y + r.h; y += 2)
    for (int x = r.x; x < r.x + r.w; x += 2)
      lit += lum(a, x, y) != bg;
  assert(lit == 0);
  cairo_surface_destroy(a);
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : ".";
  if (!wp_gl_init(NULL) || !wp_gl_current()) {
    printf("no EGL/GLES 3 device: skipped\n");
    return 77;
  }
  /* The classic style; the topo one is tested on its own. */
  struct wallpaper_config classic =
      *(const struct wallpaper_config *)wallpaper_module.defaults;
  strcpy(classic.style, "classic");
  const struct wallpaper_config *c = &classic;
  struct wp_frame f = {.percent = 50, .level = 1, .nbars = c->viz_bars};

  /* Music: louder bass, a wiggle on top, different per side. */
  for (int i = 0; i < f.nbars; ++i) {
    double base = 0.85 - 0.6 * i / f.nbars;
    f.bars[0][i] = (float)(base + 0.15 * sin(i * 0.9));
    f.bars[1][i] = (float)(base * 0.9 + 0.15 * sin(i * 1.3 + 1));
  }
  check(c, &f, dir, "wallpaper_gl_music");

  /* Shaken: the shape moves by whole pixels with everything else. */
  struct wp_frame sf = f;
  sf.shake_x = 9.4;
  sf.shake_y = -6.2;
  check(c, &sf, dir, "wallpaper_gl_shaken");

  /* Sharp: alternating full and empty bars, no blur. */
  struct wallpaper_config sharp = *c;
  sharp.viz_edge_blur = 0;
  struct wp_frame zf = f;
  for (int i = 0; i < zf.nbars; ++i)
    zf.bars[0][i] = zf.bars[1][i] = (float)(i % 2);
  check(&sharp, &zf, dir, "wallpaper_gl_sharp");

  /* Full on both sides, and silence (nothing at all). */
  struct wp_frame ff = f;
  for (int i = 0; i < ff.nbars; ++i)
    ff.bars[0][i] = ff.bars[1][i] = 1;
  check(c, &ff, dir, "wallpaper_gl_full");
  struct wp_frame qf = f;
  for (int i = 0; i < qf.nbars; ++i)
    qf.bars[0][i] = qf.bars[1][i] = 0;
  check(c, &qf, dir, "wallpaper_gl_silent");

  check_map(dir);
  check_logo(dir);
  wp_gl_finish();
  wp_render_finish();
  return 0;
}
