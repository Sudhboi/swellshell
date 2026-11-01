/* A plymouth boot splash in the topo look: swell's own glass snowflake
 * baked to PNGs, and a script that turns it on black. */
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

/* The snowflake turned by j steps of WP_PLY_LOGO_STEP degrees, centred in a
 * box x box ARGB image. The outline's gradient stays put, so the frames
 * repeat every 60 degrees like the snowflake. */
static cairo_surface_t *logo_frame(const struct wallpaper_config *c, int pw,
                                   int box, int j) {
  cairo_surface_t *s =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, box, box);
  cairo_t *cr = cairo_create(s);
  double k = pw / WP_REF_WIDTH, r = c->logo_radius * k;
  struct wp_frame f = {.angle = j * WP_PLY_LOGO_STEP * M_PI / 180};
  wp_draw_glass_logo(cr, c, box / 2.0, box / 2.0, r, r, k, &f);
  cairo_destroy(cr);
  return s;
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

/* The script. Plymouth calls refresh 50 times a second. */
static const char *script =
    "# swell topo: written by `swell plymouth`, don't edit.\n"
    "pw = %d; nlogo = %d; step = %g;\n"
    "spin = %g; fade_in = %g;\n"
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
    "for (i = 0; i < nlogo; i++) {\n"
    "  img = Image(\"logo-\" + i + \".png\");\n"
    "  logo.img[i] = fit(img, Math.Int(img.GetWidth() * k + 0.5),\n"
    "                    Math.Int(img.GetHeight() * k + 0.5));\n"
    "}\n"
    "logo.sprite = Sprite();\n"
    "logo.sprite.SetZ(1);\n"
    "logo.sprite.SetOpacity(0);\n"
    "t = 0;\n"
    "tick = 0;\n"
    "\n"
    "fun refresh() {\n"
    "  global.t += 1 / 50;\n"
    "  global.tick++;\n"
    "  logo.sprite.SetOpacity(Math.Min(t / fade_in, 1));\n"
    "  # The nearest baked frame, turned the rest of the way; every other\n"
    "  # refresh (25 Hz), as turning an image costs a few ms.\n"
    "  if (tick %% 2)\n"
    "    return;\n"
    "  a = t * spin;\n"
    "  a -= Math.Int(a / 60) * 60;\n"
    "  j = Math.Int(a / step + 0.5);\n"
    "  rest = a - j * step;\n"
    "  if (j >= nlogo)\n"
    "    j = 0;\n"
    "  img = logo.img[j];\n"
    "  if (rest != 0)\n"
    "    img = img.Rotate(rest * Math.Pi / 180);\n"
    "  logo.sprite.SetImage(img);\n"
    "  logo.sprite.SetX(Math.Int((sw - img.GetWidth()) / 2));\n"
    "  logo.sprite.SetY(Math.Int((sh - img.GetHeight()) / 2));\n"
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
  /* The frames cropped to the smallest square round the centre that holds
   * what any of them draws at any turn (Image.Rotate keeps the size, and
   * its cost goes with the area). */
  double k = pw / WP_REF_WIDTH;
  int box = 2 * (int)ceil(c->logo_radius * k * 1.25 +
                          (c->topo_shadow_px + c->topo_neon_px) * k + 4);
  cairo_surface_t *frames[WP_PLY_LOGO];
  double reach = 0;
  for (int j = 0; j < WP_PLY_LOGO; ++j) {
    frames[j] = logo_frame(c, pw, box, j);
    reach = fmax(reach, logo_reach(frames[j]));
  }
  int half = (int)ceil(reach) + 2, side = 2 * half;
  int ret = 0;
  for (int j = 0; j < WP_PLY_LOGO; ++j) {
    cairo_surface_t *s =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, side, side);
    cairo_t *cr = cairo_create(s);
    cairo_set_source_surface(cr, frames[j], half - box / 2, half - box / 2);
    cairo_paint(cr);
    cairo_destroy(cr);
    char name[32];
    snprintf(name, sizeof name, "logo-%d.png", j);
    if (ret == 0)
      ret = save(s, dir, name);
    cairo_surface_destroy(s);
    cairo_surface_destroy(frames[j]);
  }
  if (ret != 0)
    return -1;

  uint32_t t = c->topo_text;
  if (write_text(dir, "swell-topo.script", script, pw, WP_PLY_LOGO,
                 WP_PLY_LOGO_STEP, c->spin_speed,
                 WP_PLY_FADE_IN, col_r(t), col_g(t), col_b(t),
                 col_a(t)) != 0)
    return -1;
  return write_text(dir, "swell-topo.plymouth",
                    "[Plymouth Theme]\n"
                    "Name=swell topo\n"
                    "Description=swell's glass snowflake\n"
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
