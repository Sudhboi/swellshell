/* The GPU's smooth edge visualiser against cairo's: both draw the same
 * frames at 2880x1800 and the strips must match (antialiasing aside).
 * Skips (77) without a usable EGL device.
 * Usage: test_wallpaper_gl [outdir]   (default: current directory) */
#include <assert.h>
#include <math.h>
#include <stdio.h>
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

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : ".";
  if (!wp_gl_init(NULL) || !wp_gl_current()) {
    printf("no EGL/GLES 3 device: skipped\n");
    return 77;
  }
  const struct wallpaper_config *c = wallpaper_module.defaults;
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

  wp_gl_finish();
  return 0;
}
