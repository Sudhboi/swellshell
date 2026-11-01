/* A plymouth boot splash in the topo look: swell's own glass snowflake
 * baked to PNGs, still (as swell's intro starts it, so the hand-over
 * lines up), with its lambdas' glow going round while it loads. */
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "draw.h"
#include "render.h"
#include "wallpaper.h"

static int save(cairo_surface_t *s, const char *dir, const char *name) {
  char path[4096];
  snprintf(path, sizeof path, "%s/%s", dir, name);
  cairo_surface_flush(s);
  cairo_status_t st = cairo_surface_write_to_png(s, path);
  if (st != CAIRO_STATUS_SUCCESS) {
    fprintf(stderr, "swell: %s: %s\n", path, cairo_status_to_string(st));
    return -1;
  }
  return 0;
}

/* The snowflake at rest and unturned (as the daemon starts it), centred
 * in a box x box ARGB image; with glow >= 0, lambda glow's neon glow
 * alone instead (what the overspeed and the shows light). */
static cairo_surface_t *logo_frame(const struct wallpaper_config *c, int pw,
                                   int box, int glow) {
  cairo_surface_t *s =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, box, box);
  cairo_t *cr = cairo_create(s);
  double k = pw / WP_REF_WIDTH, r = c->logo_radius * k;
  struct wp_frame f = {0};
  struct wallpaper_config g = *c;
  if (glow >= 0) {
    g.topo_logo_fill &= 0xffffff00u;
    g.topo_edge_width = 0;
    g.topo_shadow_alpha = 0;
    f.flash = 1;
    for (int i = 0; i < 6; ++i)
      f.lambda_fade[i] = i != glow;
  }
  wp_draw_glass_logo(cr, &g, box / 2.0, box / 2.0, r, r, k, &f);
  cairo_destroy(cr);
  return s;
}

/* What an image draws: its bounding box and the centre of its alpha. */
struct drawn {
  int x0, y0, x1, y1;
  double cx, cy;
};

static struct drawn drawn(cairo_surface_t *s) {
  cairo_surface_flush(s);
  int w = cairo_image_surface_get_width(s),
      h = cairo_image_surface_get_height(s),
      st = cairo_image_surface_get_stride(s);
  const unsigned char *d = cairo_image_surface_get_data(s);
  struct drawn b = {w, h, 0, 0, 0, 0};
  double sum = 0;
  for (int y = 0; y < h; ++y) {
    const uint32_t *row = (const uint32_t *)(d + y * st);
    for (int x = 0; x < w; ++x) {
      double a = row[x] >> 24;
      if (a == 0)
        continue;
      b.x0 = x < b.x0 ? x : b.x0;
      b.y0 = y < b.y0 ? y : b.y0;
      b.x1 = x + 1 > b.x1 ? x + 1 : b.x1;
      b.y1 = y + 1 > b.y1 ? y + 1 : b.y1;
      b.cx += a * (x + 0.5);
      b.cy += a * (y + 0.5);
      sum += a;
    }
  }
  if (sum > 0)
    b.cx /= sum, b.cy /= sum;
  return b;
}

/* The w x h part of s at x, y. */
static cairo_surface_t *crop(cairo_surface_t *s, int x, int y, int w, int h) {
  cairo_surface_t *o = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create(o);
  cairo_set_source_surface(cr, s, -x, -y);
  cairo_paint(cr);
  cairo_destroy(cr);
  return o;
}

/* How far from the centre a frame draws anything, px. */
static double logo_reach(cairo_surface_t *s) {
  cairo_surface_flush(s);
  int w = cairo_image_surface_get_width(s),
      h = cairo_image_surface_get_height(s),
      st = cairo_image_surface_get_stride(s);
  const unsigned char *d = cairo_image_surface_get_data(s);
  double far = 0;
  for (int y = 0; y < h; ++y) {
    const uint32_t *row = (const uint32_t *)(d + y * st);
    for (int x = 0; x < w; ++x)
      if (row[x] >> 24)
        far = fmax(far, hypot(x + 0.5 - w / 2.0, y + 0.5 - h / 2.0) + 1);
  }
  return far;
}

/* The script. Plymouth calls refresh 50 times a second; only opacities
 * change, so it costs next to nothing. */
static const char *script =
    "# swell topo: written by `swell plymouth`, don't edit.\n"
    "pw = %d; nglow = %d; lap = %g; peak = %g;\n"
    "fade_in = %g;\n"
    "text_r = %g; text_g = %g; text_b = %g; text_a = %g;\n"
    "\n"
    "Window.SetBackgroundTopColor(0, 0, 0);\n"
    "Window.SetBackgroundBottomColor(0, 0, 0);\n"
    "sw = Window.GetWidth();\n"
    "sh = Window.GetHeight();\n"
    "# (sw is in plymouth's logical px: half the panel's at DeviceScale 2)\n"
    "k = sw / pw;\n"
    "# 12 pt at 2x, whatever the scale\n"
    "font = \"Sans \" + Math.Int(24 * k + 0.5);\n"
    "\n"
    "fun fit(img, w, h) {\n"
    "  if (img.GetWidth() == w && img.GetHeight() == h)\n"
    "    return img;\n"
    "  return img.Scale(w, h);\n"
    "}\n"
    "\n"
    "# (w, h) scaled by k, placed with its top left (x, y) px from the\n"
    "# screen's centre at the baked size\n"
    "fun place(name, x, y, z) {\n"
    "  img = Image(name);\n"
    "  img = fit(img, Math.Int(img.GetWidth() * k + 0.5),\n"
    "            Math.Int(img.GetHeight() * k + 0.5));\n"
    "  sp = Sprite(img);\n"
    "  sp.SetX(Math.Int(sw / 2 + x * k));\n"
    "  sp.SetY(Math.Int(sh / 2 + y * k));\n"
    "  sp.SetZ(z);\n"
    "  sp.SetOpacity(0);\n"
    "  return sp;\n"
    "}\n"
    "\n"
    "# The snowflake over its lambdas' glows (the glass in front, as the\n"
    "# wallpaper draws it), the glows clockwise from glow-0.\n"
    "%s"
    "t = 0;\n"
    "\n"
    "fun refresh() {\n"
    "  global.t += 1 / 50;\n"
    "  logo.SetOpacity(Math.Min(t / fade_in, 1));\n"
    "  # The glow goes round, lighting about two lambdas at a time.\n"
    "  on = Math.Clamp((t - fade_in) / fade_in, 0, 1);\n"
    "  p = Math.Max(t - fade_in, 0) / lap * nglow;\n"
    "  for (i = 0; i < nglow; i++) {\n"
    "    d = p - i + nglow;\n"
    "    d -= Math.Int((d + nglow / 2) / nglow) * nglow;\n"
    "    o = 0;\n"
    "    if (Math.Abs(d) < 1.6) {\n"
    "      c = Math.Cos(Math.Pi * d / 3.2);\n"
    "      o = peak * on * c * c;\n"
    "    }\n"
    "    glow[i].SetOpacity(o);\n"
    "  }\n"
    "}\n"
    "Plymouth.SetRefreshFunction(refresh);\n"
    "\n"
    "fun text_image(s) {\n"
    "  return Image.Text(s, text_r, text_g, text_b, text_a, font);\n"
    "}\n"
    "\n"
    "msg.sprite = Sprite();\n"
    "msg.sprite.SetZ(2);\n"
    "fun message(s) {\n"
    "  img = text_image(s);\n"
    "  msg.sprite.SetImage(img);\n"
    "  msg.sprite.SetX(Math.Int((sw - img.GetWidth()) / 2));\n"
    "  msg.sprite.SetY(Math.Int(sh * 0.85));\n"
    "}\n"
    "Plymouth.SetMessageFunction(message);\n"
    "\n"
    "# A password or question: the prompt under the snowflake, a dot a key.\n"
    "ask.sprite = Sprite();\n"
    "ask.sprite.SetZ(2);\n"
    "fun ask_show(prompt, entry) {\n"
    "  img = text_image(prompt + \"  \" + entry);\n"
    "  ask.sprite.SetImage(img);\n"
    "  ask.sprite.SetX(Math.Int((sw - img.GetWidth()) / 2));\n"
    "  ask.sprite.SetY(Math.Int(sh * 0.75));\n"
    "  ask.sprite.SetOpacity(1);\n"
    "}\n"
    "fun password(prompt, bullets) {\n"
    "  dots = \"\";\n"
    "  for (i = 0; i < bullets; i++)\n"
    "    dots += \"\xe2\x80\xa2\";\n"
    "  ask_show(prompt, dots);\n"
    "}\n"
    "fun question(prompt, entry) {\n"
    "  ask_show(prompt, entry);\n"
    "}\n"
    "fun normal() {\n"
    "  ask.sprite.SetOpacity(0);\n"
    "}\n"
    "Plymouth.SetDisplayPasswordFunction(password);\n"
    "Plymouth.SetDisplayQuestionFunction(question);\n"
    "Plymouth.SetDisplayNormalFunction(normal);\n";

static int write_text(const char *dir, const char *name, const char *fmt,
                      ...) __attribute__((format(printf, 3, 4)));
static int write_text(const char *dir, const char *name, const char *fmt,
                      ...) {
  char path[4096];
  snprintf(path, sizeof path, "%s/%s", dir, name);
  FILE *fp = fopen(path, "w");
  if (!fp) {
    fprintf(stderr, "swell: %s: %s\n", path, strerror(errno));
    return -1;
  }
  va_list ap;
  va_start(ap, fmt);
  vfprintf(fp, fmt, ap);
  va_end(ap);
  if (fclose(fp) != 0) {
    fprintf(stderr, "swell: %s: %s\n", path, strerror(errno));
    return -1;
  }
  return 0;
}

int wp_plymouth_theme(const struct wallpaper_config *c, const char *dir,
                      const char *install, int pw) {
  if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
    fprintf(stderr, "swell: %s: %s\n", dir, strerror(errno));
    return -1;
  }
  /* The snowflake cropped to the smallest square round the centre that
   * holds it, the glows to what they draw. */
  double k = pw / WP_REF_WIDTH;
  int box = 2 * (int)ceil(c->logo_radius * k * 1.25 +
                          (c->topo_shadow_px + c->topo_neon_px) * k + 4);
  cairo_surface_t *logo = logo_frame(c, pw, box, -1);
  int half = (int)ceil(logo_reach(logo)) + 2;
  cairo_surface_t *s = crop(logo, box / 2 - half, box / 2 - half, 2 * half,
                            2 * half);
  int ret = save(s, dir, "logo.png");
  cairo_surface_destroy(s);
  cairo_surface_destroy(logo);
  char sprites[2048];
  int n = snprintf(sprites, sizeof sprites,
                   "logo = place(\"logo.png\", %d, %d, 1);\n", -half, -half);

  /* Clockwise (y down) from the top. */
  cairo_surface_t *glows[6];
  struct drawn at[6];
  int order[6];
  for (int i = 0; i < 6; ++i) {
    glows[i] = logo_frame(c, pw, box, i);
    at[i] = drawn(glows[i]);
    order[i] = i;
  }
  double ang[6];
  for (int i = 0; i < 6; ++i)
    ang[i] = fmod(atan2(at[i].cx - box / 2.0, -(at[i].cy - box / 2.0)) +
                      2 * M_PI,
                  2 * M_PI);
  for (int i = 1; i < 6; ++i)
    for (int j = i; j > 0 && ang[order[j]] < ang[order[j - 1]]; --j) {
      int o = order[j];
      order[j] = order[j - 1];
      order[j - 1] = o;
    }
  for (int i = 0; i < 6; ++i) {
    const struct drawn *b = &at[order[i]];
    char name[32];
    snprintf(name, sizeof name, "glow-%d.png", i);
    cairo_surface_t *g =
        crop(glows[order[i]], b->x0, b->y0, b->x1 - b->x0, b->y1 - b->y0);
    if (ret == 0)
      ret = save(g, dir, name);
    cairo_surface_destroy(g);
    n += snprintf(sprites + n, sizeof sprites - n,
                  "glow[%d] = place(\"glow-%d.png\", %d, %d, 0);\n", i, i,
                  b->x0 - box / 2, b->y0 - box / 2);
  }
  for (int i = 0; i < 6; ++i)
    cairo_surface_destroy(glows[i]);
  if (ret != 0)
    return -1;

  uint32_t t = c->topo_text;
  if (write_text(dir, "swell-topo.script", script, pw, 6, WP_PLY_LAP,
                 WP_PLY_PEAK, WP_PLY_FADE_IN, col_r(t), col_g(t), col_b(t),
                 col_a(t), sprites) != 0)
    return -1;
  return write_text(dir, "swell-topo.plymouth",
                    "[Plymouth Theme]\n"
                    "Name=swell topo\n"
                    "Description=swell's glass snowflake, its lambdas lit in turn\n"
                    "ModuleName=script\n"
                    "\n"
                    "[script]\n"
                    "ImageDir=%s\n"
                    "ScriptFile=%s/swell-topo.script\n",
                    install, install);
}

/* `swell wallpaper plymouth DIR [WxH] [--install DIR]`, run in the client:
 * nothing to do with the daemon. Every other command goes on to it. */
int wp_run_local(struct swell *sw, const void *cfg, bool have_daemon,
                 int argc, char **argv) {
  (void)sw;
  (void)have_daemon;
  if (argc < 1 || strcmp(argv[0], "plymouth") != 0)
    return -1;
  const char *dir = NULL, *install = WP_PLY_INSTALL;
  int pw = 2880, ph = 1800;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--install") == 0 && i + 1 < argc)
      install = argv[++i];
    else if (!dir && argv[i][0] != '-')
      dir = argv[i];
    else if (sscanf(argv[i], "%dx%d", &pw, &ph) != 2 || pw < 64 || ph < 64 ||
             pw > 16384 || ph > 16384)
      dir = NULL, i = argc;
  }
  if (!dir) {
    fprintf(stderr, "usage: swell wallpaper plymouth DIR [WxH] "
                    "[--install DIR]\n");
    return 2;
  }
  const struct wallpaper_config *c = cfg;
  if (!wp_topo(c))
    fprintf(stderr, "swell: style is \"%s\"; the theme is topo's look "
                    "anyway\n",
            c->style);
  if (wp_plymouth_theme(c, dir, install, pw) != 0)
    return 1;
  printf("wrote %s (%dx%d) for %s\n"
         "install: sudo mkdir -p %s && sudo cp %s/* %s/ && "
         "sudo plymouth-set-default-theme -R swell-topo\n",
         dir, pw, ph, install, install, dir, install);
  return 0;
}
