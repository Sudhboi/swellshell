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

#include "info.h"
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

/* Of b's lit pixels within r0..r1 of the centre, how many (per 1000) have
 * a lit pixel of a close to where shift(r) (px, out from the centre) puts
 * them. */
static long lit_moved(cairo_surface_t *a, cairo_surface_t *b, double r0,
                      double r1, double (*shift)(double r, double sign),
                      double sign) {
  long lit = 0, found = 0;
  for (int y = 4; y < H - 4; y += 3)
    for (int x = 4; x < W - 4; x += 3) {
      double dx = x - W / 2.0, dy = y - H / 2.0, r = hypot(dx, dy);
      if (r < r0 || r > r1 || lum(b, x, y) < 120)
        continue;
      double s = shift(r, sign);
      int ax = (int)lround(x + dx / r * s), ay = (int)lround(y + dy / r * s);
      if (ax < 3 || ay < 3 || ax >= W - 3 || ay >= H - 3)
        continue;
      lit++;
      bool hit = false;
      for (int j = -3; j <= 3 && !hit; ++j)
        for (int i = -3; i <= 3 && !hit; ++i)
          hit = lum(a, ax + i, ay + j) >= 60;
      found += hit;
    }
  return lit ? found * 1000 / lit : 0;
}

/* The gather's pull at r (the shader's, with the default config at the
 * reference width), times sign; and none. */
static double gather_shift(double r, double sign) {
  const struct wallpaper_config *c = wallpaper_module.defaults;
  return sign * c->topo_gather_px *
         exp(-fmax(r - c->logo_radius, 0) / c->topo_gather_reach);
}
static double no_shift(double r, double sign) { return 0; }

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

  /* The pointer at full push, off the centre: the lines round it move,
   * spread out (fewer lit pixels close by), and none far away. */
  struct wp_frame kf = f;
  kf.poke = 1;
  kf.poke_x = 720;
  kf.poke_y = 900;
  cairo_surface_t *km = gpu_map(c, &kf, dir, "wallpaper_gl_map_poke");
  double pr = c->topo_poke_radius;
  long near_k = 0, far_k = 0, lit_g = 0, lit_k = 0;
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3) {
      double r = hypot(x - 720.0, y - 900.0);
      bool differ = abs(lum(gm, x, y) - lum(km, x, y)) > 60;
      if (r > 0.3 * pr && r < 2 * pr)
        near_k += differ;
      else if (r > 4 * pr)
        far_k += differ;
      if (r < 0.8 * pr) {
        lit_g += lum(gm, x, y) >= 120;
        lit_k += lum(km, x, y) >= 120;
      }
    }
  fprintf(stderr, "poke: %ld changed round it, %ld far; lit inside %ld -> %ld\n",
          near_k, far_k, lit_g, lit_k);
  assert(near_k > 300 && far_k == 0 && lit_k < lit_g);
  cairo_surface_destroy(km);

  /* The spectrum: silent, nothing moves; playing, the lines bulge round the
   * logo, ripple out on the map and brighten. */
  struct wp_frame pf = f;
  pf.spec_on = true;
  cairo_surface_t *qm = gpu_map(c, &pf, dir, NULL);
  for (int y = 0; y < H; y += 5)
    for (int x = 0; x < W; x += 5)
      assert(abs(lum(gm, x, y) - lum(qm, x, y)) <= 2);
  cairo_surface_destroy(qm);
  float bars[2][48];
  for (int s = 0; s < 2; ++s)
    for (int i = 0; i < 48; ++i)
      bars[s][i] = (float)(s ? 0.9 - 0.6 * i / 47.0 : 0.3 + 0.6 * i / 47.0);
  for (int s = 0; s < 2; ++s)
    wp_viz_spectrum(bars[s], 48, pf.spec[s]);
  qm = gpu_map(c, &pf, dir, "wallpaper_gl_map_spectrum");
  long near_logo = 0, far = 0, lit_gm = 0, lit_qm = 0;
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3) {
      double r = hypot(x - W / 2.0, y - H / 2.0);
      bool differ = abs(lum(gm, x, y) - lum(qm, x, y)) > 60;
      if (r < 500)
        near_logo += differ;
      else if (r > 700)
        far += differ;
      lit_gm += lum(gm, x, y);
      lit_qm += lum(qm, x, y);
    }
  fprintf(stderr,
          "spectrum: %ld changed near the logo, %ld far, light %ld -> %ld\n",
          near_logo, far, lit_gm, lit_qm);
  assert(near_logo > 500 && far > 500 && lit_qm * 10 > lit_gm * 13);
  cairo_surface_destroy(qm);

  /* Held popup key: the lines are drawn in towards the snowflake, most
   * near it; nothing at 0; out instead while springing back (< 0). */
  struct wp_frame hf = f;
  cairo_surface_t *hm = gpu_map(c, &hf, dir, NULL);
  for (int y = 0; y < H; y += 5)
    for (int x = 0; x < W; x += 5)
      assert(abs(lum(gm, x, y) - lum(hm, x, y)) <= 2);
  cairo_surface_destroy(hm);
  hf.peek_gather = 1;
  hm = gpu_map(c, &hf, dir, "wallpaper_gl_map_gather");
  hf.peek_gather = -1;
  cairo_surface_t *om = gpu_map(c, &hf, dir, NULL);
  /* Each lit pixel came from where the pull says, near the logo (most
   * pull) and far out (less); not from where it was. */
  long near_in = lit_moved(gm, hm, 330, 800, gather_shift, 1);
  long near_still = lit_moved(gm, hm, 330, 800, no_shift, 0);
  long far_in = lit_moved(gm, hm, 1100, 1600, gather_shift, 1);
  long near_out = lit_moved(gm, om, 330, 800, gather_shift, -1);
  fprintf(stderr,
          "gather: lit from the pulled place %ld/1000 near, %ld far, %ld "
          "springing out; from the same place %ld\n",
          near_in, far_in, near_out, near_still);
  assert(near_in > 900 && far_in > 900 && near_out > 900);
  assert(near_still < 500);
  cairo_surface_destroy(om);
  cairo_surface_destroy(hm);

  /* Through the hold the lines light up from the screen's corners inward:
   * outside the front (corner to snowflake as the hold runs) brighter,
   * inside it as before; none before the hold or once expanded. */
  struct info pin;
  memset(&pin, 0, sizeof pin);
  struct wp_frame lf = f;
  lf.peek = &pin;
  lf.peek_pop = 1;
  cairo_surface_t *lm = gpu_map(c, &lf, dir, NULL);
  for (int y = 0; y < H; y += 5)
    for (int x = 0; x < W; x += 5)
      assert(abs(lum(gm, x, y) - lum(lm, x, y)) <= 2);
  cairo_surface_destroy(lm);
  lf.peek_hold = 0.5;
  lm = gpu_map(c, &lf, dir, "wallpaper_gl_map_hold_light");
  double rmax = hypot(W / 2.0, H / 2.0);
  double front = c->logo_radius + (rmax - c->logo_radius) * 0.5;
  long out_a = 0, out_b = 0, inner_diff = 0;
  for (int y = 0; y < H; y += 2)
    for (int x = 0; x < W; x += 2) {
      double r = hypot(x - W / 2.0, y - H / 2.0);
      if (r > front + 150) {
        out_a += lum(gm, x, y);
        out_b += lum(lm, x, y);
      } else if (r < front - 150) {
        inner_diff += abs(lum(gm, x, y) - lum(lm, x, y)) > 2;
      }
    }
  fprintf(stderr,
          "hold light: outside the front %ld -> %ld, %ld changed inside\n",
          out_a, out_b, inner_diff);
  assert(out_b * 10 > out_a * 13 && inner_diff == 0);
  cairo_surface_destroy(lm);
  lf.peek_expand = 1;
  lm = gpu_map(c, &lf, dir, NULL);
  for (int y = 0; y < H; y += 5)
    for (int x = 0; x < W; x += 5)
      assert(abs(lum(gm, x, y) - lum(lm, x, y)) <= 2);
  cairo_surface_destroy(lm);

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

  /* Drifting at rest: the GPU's lines moved by about the drift (it bends a
   * little), not left where they were; cairo's by it in whole pixels. */
  struct wp_frame wf = f;
  double k = W / WP_REF_WIDTH;
  wf.drift_x = 10 / k;
  wf.drift_y = -6 / k;
  cairo_surface_t *wm = gpu_map(c, &wf, dir, "wallpaper_gl_map_drift");
  long lit_w = 0, moved = 0, stayed = 0;
  for (int y = 16; y < H - 16; y += 3)
    for (int x = 16; x < W - 16; x += 3) {
      if (lum(wm, x, y) < 120)
        continue;
      lit_w++;
      bool from = false, same = false;
      for (int j = -4; j <= 4; ++j)
        for (int i = -4; i <= 4; ++i) {
          from |= lum(gm, x - 10 + i, y + 6 + j) >= 60;
          same |= abs(i) <= 1 && abs(j) <= 1 && lum(gm, x + i, y + j) >= 60;
        }
      moved += from;
      stayed += same;
    }
  fprintf(stderr,
          "drift gpu: %ld lit, %ld from the drifted place, %ld from "
          "the same\n",
          lit_w, moved, stayed);
  assert(moved * 100 > lit_w * 95 && stayed * 2 < lit_w);
  cairo_surface_destroy(wm);
  cairo_surface_t *c0 = render(c, &f), *cd = render(c, &wf);
  long cpu_n = 0, cpu_off = 0;
  for (int y = 16; y < H - 16; y += 3)
    for (int x = 16; x < W - 16; x += 3) {
      double r = hypot(x - W / 2.0, y - H / 2.0);
      if (r < c->logo_radius * k * 1.6 || y < H / 8)
        continue; /* the snowflake and the bar stay */
      cpu_n++;
      cpu_off += lum(cd, x + 10, y - 6) != lum(c0, x, y);
    }
  fprintf(stderr, "drift cpu: %ld of %ld off\n", cpu_off, cpu_n);
  assert(cpu_off * 1000 < cpu_n);
  cairo_surface_destroy(c0);
  cairo_surface_destroy(cd);

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

/* Lit pixels (of the map at least this bright) within r0..r1 of the
 * centre, per 100000 of those sampled. */
static long lit_in(cairo_surface_t *m, double r0, double r1) {
  long n = 0, lit = 0;
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3) {
      double r = hypot(x - W / 2.0, y - H / 2.0);
      if (r < r0 || r > r1)
        continue;
      n++;
      lit += lum(m, x, y) >= 60;
    }
  return n ? lit * 100000 / n : 0;
}

/* Show s's map at 0, 1/6 .. 6/6 of its length, into f and m;
 * wallpaper_gl_show_<name>_*. */
static void show_maps(const struct wallpaper_config *c, enum wp_show s,
                      struct wp_frame base, const char *dir,
                      struct wp_frame f[7], cairo_surface_t *m[7]) {
  char name[96];
  int len = wp_show_ms(c, s);
  for (int j = 0; j <= 6; ++j) {
    f[j] = base;
    wp_show_apply(c, s, len * j / 6.0, &f[j]);
    snprintf(name, sizeof name, "wallpaper_gl_show_%s_%d", wp_show_name(s), j);
    m[j] = gpu_map(c, &f[j], dir, name);
  }
}

static void free_maps(cairo_surface_t *m[7]) {
  for (int j = 0; j <= 6; ++j)
    cairo_surface_destroy(m[j]);
}

/* topo's shows on the GPU's map. */
static void check_shows(const char *dir) {
  const struct wallpaper_config *c = wallpaper_module.defaults;
  struct wp_frame base = {.percent = 50, .level = 1, .dir = 1};
  cairo_surface_t *rest = gpu_map(c, &base, NULL, NULL);
  long all = lit_in(rest, 0, 1e9);
  struct wp_frame f[7];
  cairo_surface_t *m[7];

  /* Shock: black, then lines only inside the wave, then all of them. */
  show_maps(c, WP_SHOW_SHOCK, base, dir, f, m);
  assert(lit_in(m[0], 0, 1e9) == 0);
  double r = f[2].smap.in_r;
  assert(r > 400 && r < 1400);
  assert(lit_in(m[2], 0, r - 300) * 2 > all && lit_in(m[2], r + 50, 1e9) == 0);
  assert(labs(lit_in(m[6], 0, 1e9) - all) * 10 < all);
  free_maps(m);

  /* Rise: more lines at each step, then all of them. */
  show_maps(c, WP_SHOW_RISE, base, dir, f, m);
  long prev = -1;
  for (int j = 0; j <= 2; ++j) {
    long n = lit_in(m[j], 0, 1e9);
    fprintf(stderr, "rise %d: %ld of %ld\n", j, n, all);
    assert(n > prev && n < all);
    prev = n;
  }
  assert(lit_in(m[0], 0, 1e9) * 10 < all && lit_in(m[1], 0, 1e9) * 2 < all);
  assert(labs(lit_in(m[6], 0, 1e9) - all) * 10 < all);
  free_maps(m);

  /* Unfurl: the lines drawn in round the centre (unlike the map at rest),
   * then back in place. */
  show_maps(c, WP_SHOW_UNFURL, base, dir, f, m);
  assert(f[1].smap.gather > 300);
  long differ = 0, n = 0;
  for (int y = 0; y < H; y += 3)
    for (int x = 0; x < W; x += 3)
      if (hypot(x - W / 2.0, y - H / 2.0) < 700) {
        n++;
        differ += abs(lum(m[1], x, y) - lum(rest, x, y)) > 60;
      }
  fprintf(stderr, "unfurl: %ld of %ld differ\n", differ, n);
  assert(differ * 50 > n);
  assert(labs(lit_in(m[6], 0, 1e9) - all) * 10 < all);
  free_maps(m);

  /* Drain: the dark closes in, no lines left past it, fewer each step
   * (once lit, wider, they count more at first). */
  show_maps(c, WP_SHOW_DRAIN, base, dir, f, m);
  r = f[3].smap.in_r;
  assert(r < 1300 && lit_in(m[3], r + 50, 1e9) == 0);
  prev = 1L << 30;
  for (int j = 2; j <= 5; ++j) {
    long n = lit_in(m[j], 0, 1e9);
    fprintf(stderr, "drain %d: %ld of %ld\n", j, n, all);
    assert(n <= prev);
    prev = n;
  }
  assert(prev * 10 < all);
  free_maps(m);

  /* Sink: fewer levels each step, then the wave leaves none behind it. */
  show_maps(c, WP_SHOW_SINK, base, dir, f, m);
  prev = 1L << 30;
  for (int j = 1; j <= 3; ++j) {
    long n = lit_in(m[j], 0, 1e9);
    fprintf(stderr, "sink %d: %ld of %ld\n", j, n, all);
    assert(n < prev);
    prev = n;
  }
  r = f[4].smap.out_r;
  assert(r > 300 && lit_in(m[4], 0, r - 200) == 0);
  assert(lit_in(m[5], 0, 1e9) * 10 < all);
  free_maps(m);

  /* Blast: the lines drawn in a little, then none behind the ring. */
  show_maps(c, WP_SHOW_BLAST, base, dir, f, m);
  assert(f[1].smap.gather > 0 && f[1].nrings == 0);
  r = f[2].smap.out_r;
  fprintf(stderr, "blast: ring %.0f, %ld inside, %ld outside\n", r,
          lit_in(m[2], 0, r - 200), lit_in(m[2], r + 300, 1e9));
  assert(r > 300 && lit_in(m[2], 0, r - 200) == 0);
  assert(lit_in(m[2], r + 300, 1e9) * 2 > all);
  assert(lit_in(m[4], 0, 1e9) * 10 < all);
  free_maps(m);
  cairo_surface_destroy(rest);
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
  check_shows(dir);
  check_logo(dir);
  wp_gl_finish();
  wp_render_finish();
  return 0;
}
