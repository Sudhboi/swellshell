/* Offscreen render test (port of popup's test/render_test.c): draws a few
 * info variants and animation frames to PNGs at scale 2, and checks the
 * panel's geometry.
 * Usage: test_popup_render [outdir]   (default: current directory) */
#include <stdio.h>
#include <string.h>

#include "../png.h"
#include "info.h"
#include "module.h"
#include "popup/render.h"

extern const struct swell_module popup_module;

static struct popup_config cfg;

static int dump_anim(const struct info *in, const char *dir, const char *name,
                     double pop, double expand) {
  cairo_t *cr = canvas(cfg.width, popup_surface_height(&cfg));
  popup_render(cr, &cfg, in, pop, expand, 0);
  save_png(cr, dir, name);
  /* Fully popped, the panel's bottom edge rests at y = height. */
  int bad = pop >= 1 && (pixel(cr, 0.5, cfg.height - 4) != cfg.border ||
                         pixel(cr, 4, cfg.height + 4) != 0);
  if (bad)
    fprintf(stderr, "%s: panel not resting on y = height\n", name);
  cairo_destroy(cr);
  return bad;
}

/* The compact panel with the hold line run to hold. It traces the border
 * from the bottom-left corner: along the bottom first, and down the left
 * side last. */
static int dump_hold(const struct info *in, const char *dir, const char *name,
                     double expand, double hold) {
  cairo_t *cr = canvas(cfg.width, popup_surface_height(&cfg));
  popup_render(cr, &cfg, in, 1, expand, hold);
  save_png(cr, dir, name);
  double inset = 1 + cfg.hold_h / 2; /* the line's middle */
  int bad = 0;
  if (expand == 0) {
    bool bottom =
        pixel(cr, cfg.width / 2.0, cfg.height - inset) != cfg.background;
    bool left = pixel(cr, inset, cfg.height - 10) != cfg.background;
    bad = !bottom || left != (hold >= 1);
  }
  if (bad)
    fprintf(stderr, "%s: hold line not drawn as expected\n", name);
  cairo_destroy(cr);
  return bad;
}

static int dump(const struct info *in, const char *dir, const char *name) {
  return dump_anim(in, dir, name, 1, 1);
}

/* The logical y of the panel's top edge (the first border-coloured pixel
 * down the left edge), or -1 if no panel is drawn. Unlike the content, this
 * doesn't depend on the installed fonts. */
static double panel_top(const struct info *in, double pop, double expand) {
  cairo_t *cr = canvas(cfg.width, popup_surface_height(&cfg));
  popup_render(cr, &cfg, in, pop, expand, 0);
  double top = -1;
  for (int py = 0; py < popup_surface_height(&cfg) * SCALE; ++py)
    if (pixel(cr, 0.5, (py + 0.5) / SCALE) == cfg.border) {
      top = (double)py / SCALE;
      break;
    }
  cairo_destroy(cr);
  return top;
}

static int check(bool ok, const char *what) {
  if (!ok)
    fprintf(stderr, "check failed: %s\n", what);
  return !ok;
}

/* How pop and expand move the panel's top edge. */
static int check_geometry(const struct info *in) {
  int rc = 0;
  double compact = panel_top(in, 1, 0);
  double quarter = panel_top(in, 1, 0.25);
  double half = panel_top(in, 1, 0.5);
  double full = panel_top(in, 1, 1);
  rc |= check(compact > 0 && full >= 0, "a panel is drawn");
  rc |= check(compact > quarter && quarter > half && half > full,
              "expanding grows the panel upward");
  if (full != 0)
    fprintf(stderr, "full panel top at %.1f\n", full);
  rc |= check(full == 0, "the full panel fills the surface's height");
  rc |= check(panel_top(in, 0.5, 0) > compact,
              "half popped, the panel is lower");
  rc |= check(panel_top(in, 0, 0) < 0, "not popped, nothing is drawn");

  cairo_t *cr = canvas(cfg.width, popup_surface_height(&cfg));
  popup_render(cr, &cfg, in, 1, 1, 0);
  double mid = (full + cfg.height) / 2;
  rc |= check(pixel(cr, cfg.width - 0.5, mid) == cfg.border,
              "the right border is at the surface's right edge");
  if (full >= 1)
    rc |= check(pixel(cr, cfg.width / 2.0, full - 1) == 0,
                "nothing is drawn above the panel");
  cairo_destroy(cr);
  return rc;
}

static struct info normal(void) {
  struct info in;
  memset(&in, 0, sizeof in);
  in.now =
      1791295200 + 13 * 3600 + 47 * 60 + 9; /* fixed time, shown in local TZ */
  in.has_battery = true;
  in.battery = 78;
  in.charging = false;
  in.has_power = true;
  in.power_w = 7.4;
  in.has_volume = true;
  in.volume = 45;
  in.has_brightness = true;
  in.brightness = 60;
  in.net = NET_WIFI;
  snprintf(in.net_name, sizeof in.net_name, "home-5G");
  in.net_signal = 72;
  in.cpu = 12;
  in.mem_used_gib = 5.1;
  in.mem_total_gib = 15.5;
  in.uptime_s = 3 * 3600 + 12 * 60;
  in.ws_count = 5;
  for (int i = 0; i < in.ws_count; i++)
    in.ws[i] = (struct workspace){.num = i + 1};
  for (int i = 0; i < in.ws_count; i++)
    snprintf(in.ws[i].name, sizeof in.ws[i].name, "%d", i + 1);
  in.ws[2].focused = true;
  return in;
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : ".";
  int rc = 0;
  cfg = *(const struct popup_config *)popup_module.defaults;

  struct info a = normal();
  rc |= dump(&a, dir, "normal");
  rc |= dump_anim(&a, dir, "anim_pop_half", 0.5, 0);
  rc |= dump_anim(&a, dir, "anim_compact", 1, 0);
  rc |= dump_hold(&a, dir, "anim_hold_half", 0, 0.5);
  rc |= dump_hold(&a, dir, "anim_hold_full", 0, 1);
  rc |= dump_hold(&a, dir, "anim_hold_expand_quarter", 0.25, 1);
  rc |= dump_anim(&a, dir, "anim_expand_quarter", 1, 0.25);
  rc |= dump_anim(&a, dir, "anim_expand_half", 1, 0.5);

  struct info b = normal();
  b.muted = true;
  b.battery = 9;
  b.net = NET_OFFLINE;
  b.net_name[0] = '\0';
  b.cpu = -1;
  b.uptime_s = 7 * 60;
  b.ws_count = 2;
  b.ws[1].urgent = true;
  b.ws[2].focused = false;
  b.ws[0].focused = true;
  rc |= dump(&b, dir, "muted_low_offline");
  rc |= dump_anim(&b, dir, "anim_compact_muted_low", 1, 0);

  struct info c = normal();
  c.charging = true;
  c.has_power = true;
  c.power_charging = true;
  c.power_w = 38.71;
  c.battery = 100;
  c.volume = 130;
  c.net = NET_WIRED;
  snprintf(c.net_name, sizeof c.net_name, "Wired connection 1");
  c.uptime_s = 4 * 86400 + 5 * 3600 + 30 * 60;
  c.cpu = 100;
  c.ws_count = 12;
  for (int i = 5; i < c.ws_count; i++) {
    c.ws[i] = (struct workspace){.num = i + 1};
    snprintf(c.ws[i].name, sizeof c.ws[i].name, "%d", i + 1);
  }
  snprintf(c.ws[7].name, sizeof c.ws[7].name, "mail");
  c.ws[2].focused = false;
  c.ws[11].focused = true;
  rc |= dump(&c, dir, "long");
  /* the widest stats row: on battery, with every field long */
  struct info g = c;
  g.charging = false;
  g.power_charging = false;
  g.has_power = true;
  g.power_w = 45.62;
  g.mem_used_gib = 27.3;
  g.mem_total_gib = 31.0;
  rc |= dump(&g, dir, "wide_stats");
  /* non-Latin text falls back to fonts other than C->font */
  struct info k = normal();
  snprintf(k.net_name, sizeof k.net_name, "咖啡店 回声");
  rc |= dump(&k, dir, "cjk_net");
  rc |= dump_anim(&c, dir, "anim_compact_many_ws", 1, 0);
  rc |= dump_anim(&c, dir, "anim_expand_quarter_many_ws", 1, 0.25);

  struct info d = normal();
  d.has_battery = false;
  d.has_volume = false;
  d.has_brightness = false;
  d.net = NET_UNKNOWN;
  d.net_name[0] = '\0';
  d.charging = true;
  d.has_power = false;
  d.net_signal = 20;
  d.ws_count = 0;
  rc |= dump(&d, dir, "missing");
  rc |= dump_anim(&d, dir, "anim_compact_missing", 1, 0);

  struct info e = normal();
  e.charging = true;
  e.has_power = false;
  e.battery = 43;
  e.volume = 10;
  e.net_signal = 20;
  e.brightness = 0;
  e.uptime_s = 86400 + 59;
  rc |= dump(&e, dir, "weak_wifi");

  struct info f = normal();
  f.ws_count = 1;
  f.ws[2].focused = false;
  f.ws[0].focused = true;
  rc |= dump_anim(&f, dir, "anim_compact_one_ws", 1, 0);
  rc |= dump_anim(&f, dir, "anim_expand_quarter_one_ws", 1, 0.25);

  rc |= check_geometry(&a);

  return rc;
}
