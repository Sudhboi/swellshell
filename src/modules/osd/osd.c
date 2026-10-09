/* osd: a volume/brightness overlay that drops down from the top with a
 * ring filling to the level and a Nix snowflake rotated to level x 360deg
 * (formerly nixosd). Driven by `swell osd vol N [mute]` / `bri N`. */
#include "osd.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bus.h"
#include "loop.h"
#include "module.h"
#include "render.h"
#include "surface.h"
#include "tween.h"
#include "util.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

static const struct osd_config defaults = {
    .namespace = "nixosd",
    .width = 220,
    .height = 260,
    .corner = 0,
    .top_gap = 12,
    .ring_cy = 110,
    .ring_radius = 88,
    .ring_width = 10,
    .logo_radius = 64,
    .text_baseline = 238,
    .font = "IosevkaTerm Nerd Font",
    .font_size = 22,
    .background = 0x000000f0u,
    .border = 0x6a6a6affu,
    .track = 0x333333ffu,
    .nix_dark = 0x8c8c8cffu,
    .nix_light = 0xcfcfcfffu,
    .ring_dark = 0xa0a0a0ffu,
    .ring_light = 0xd6d6d6ffu,
    .over_dark = 0xeeeeeeffu,
    .over_light = 0xffffffffu,
    .pulse_light = 0xff3b3bffu,
    .pulse_dark = 0xb81414ffu,
    .muted = 0x5a5a5affu,
    .text = 0xf2f2f2ffu,
    .logo_pulse_alpha = 0.75,
    .max_pct = 200,
    .hide_timeout_ms = 1200,
    .tween_ms = 260,
    .slide_ms = 220,
    .pulse_ms = 900,
};

#define S struct osd_config
static const struct cfg_field fields[] = {
    CFG_STRING(S, namespace, "layer-shell namespace of the surface"),
    CFG_INT(S, width, 1, 4096, "panel width, logical px"),
    CFG_INT(S, height, 1, 4096, "panel height, logical px"),
    CFG_INT(S, corner, 0, 512, "panel corner radius (0 = square)"),
    CFG_INT(S, top_gap, 0, 1024,
            "gap between the top edge (or bar) and the panel"),
    CFG_INT(S, ring_cy, 0, 4096, "ring centre, px from the panel top"),
    CFG_INT(S, ring_radius, 1, 2048, "ring radius"),
    CFG_INT(S, ring_width, 1, 512, "ring stroke width"),
    CFG_INT(S, logo_radius, 1, 2048, "snowflake radius, tip to centre"),
    CFG_INT(S, text_baseline, 0, 4096,
            "baseline of the icon + percentage text, px from the panel top"),
    CFG_STRING(S, font, "font family (cairo toy font API)"),
    CFG_INT(S, font_size, 1, 512, "text size"),
    CFG_COLOR(S, background, "panel background"),
    CFG_COLOR(S, border, "1px panel border"),
    CFG_COLOR(S, track, "unfilled ring track"),
    CFG_COLOR(S, nix_dark, "snowflake dark lambdas"),
    CFG_COLOR(S, nix_light, "snowflake light lambdas"),
    CFG_COLOR(S, ring_dark, "ring gradient, dark end"),
    CFG_COLOR(S, ring_light, "ring gradient, light end"),
    CFG_COLOR(S, over_dark, "second lap above 100%, dark end"),
    CFG_COLOR(S, over_light, "second lap above 100%, light end"),
    CFG_COLOR(S, pulse_light,
              "above 100%: the over-lap, text and logo pulse to this"),
    CFG_COLOR(S, pulse_dark, "above 100%: pulse colour of the dark end"),
    CFG_COLOR(S, muted, "ring and logo when muted"),
    CFG_COLOR(S, text, "icon and percentage text"),
    CFG_DOUBLE(S, logo_pulse_alpha, 0, 1,
               "strength of the red tint on the logo above 100%"),
    CFG_INT(S, max_pct, 1, 1000,
            "highest accepted level, percent (above 100 starts a second lap)"),
    CFG_INT(S, hide_timeout_ms, 1, 600000,
            "time after the last update before retracting"),
    CFG_INT(S, tween_ms, 0, 10000, "level change animation"),
    CFG_INT(S, slide_ms, 0, 10000, "drop-down / retract animation"),
    CFG_INT(S, pulse_ms, 1, 60000, "one red pulse cycle above 100%"),
};
#undef S

static struct {
  struct swell *sw;
  struct osd_config cfg;
  struct layer_surface *surf;
  struct loop_timer *hide_timer;
  struct osd_anim anim;
  bool retracting;
} o;

static double now_fms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static bool draw(cairo_t *cr, void *data) {
  struct osd_render_state rs;
  bool more = osd_anim_sample(&o.anim, &o.cfg, now_fms(), &rs);
  osd_render_surface(cr, &o.cfg, &rs);
  return more;
}

static void show_surface(void) {
  struct layer_opts opts = {
      .ns = o.cfg.namespace,
      .width = o.cfg.width,
      .height = osd_surface_height(&o.cfg),
      .layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
      /* Top, centred horizontally; zone 0 keeps it below an exclusive bar. */
      .anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP,
      .exclusive_zone = 0,
      .click_through = true,
      .keyboard = false,
  };
  layer_surface_show(o.surf, &opts);
}

static bool visible(void) {
  return layer_surface_shown(o.surf) && !o.retracting;
}

static void on_event(const struct osd_event *ev) {
  bool was_visible = visible();
  osd_anim_event(&o.anim, &o.cfg, ev, now_fms());
  o.retracting = false;
  show_surface();
  layer_surface_redraw(o.surf);
  loop_timer_arm(o.hide_timer, o.cfg.hide_timeout_ms, 0);
  if (!was_visible) {
    char kind[32], level[32], muted[32];
    snprintf(kind, sizeof kind, "SWELL_OSD_KIND=%s",
             ev->kind == OSD_BRIGHTNESS ? "brightness" : "volume");
    snprintf(level, sizeof level, "SWELL_OSD_LEVEL=%d", ev->level);
    snprintf(muted, sizeof muted, "SWELL_OSD_MUTED=%d", ev->muted ? 1 : 0);
    bus_emit("osd.show", kind, level, muted, NULL);
  }
}

static void on_hide_timer(struct loop_timer *t, void *data) {
  if (!layer_surface_shown(o.surf) || o.retracting)
    return;
  osd_anim_retract(&o.anim, &o.cfg, now_fms());
  o.retracting = true;
  layer_surface_redraw(o.surf);
  bus_emit("osd.hide", NULL);
}

/* Destroy the surface once the retract has run. The timeout finishes it
 * even if no frame callbacks arrive (e.g. the output is off). */
static int prepare(void *data) {
  if (!o.retracting)
    return -1;
  double now = now_fms();
  if (!osd_anim_hidden(&o.anim, now))
    return osd_anim_retract_left(&o.anim, now) + 1;
  layer_surface_hide(o.surf);
  o.retracting = false;
  return -1;
}

static bool init(struct swell *sw, const void *cfg) {
  if (!sw->wl->layer_shell) {
    warn("osd: the compositor lacks wlr-layer-shell");
    return false;
  }
  o.sw = sw;
  o.cfg = *(const struct osd_config *)cfg;
  o.anim = (struct osd_anim){0};
  o.retracting = false;
  o.surf = layer_surface_create(sw->wl, draw, NULL);
  o.hide_timer = loop_add_timer(sw->loop, CLOCK_MONOTONIC, on_hide_timer, NULL);
  loop_add_prepare(sw->loop, 50, prepare, NULL);
  return true;
}

static void reconfigure(const void *cfg) {
  o.cfg = *(const struct osd_config *)cfg;
  if (!layer_surface_shown(o.surf))
    return;
  /* New sizes need a new surface: re-show it, or finish a retract now. */
  layer_surface_hide(o.surf);
  if (o.retracting) {
    o.retracting = false;
    return;
  }
  show_surface();
  layer_surface_redraw(o.surf);
}

static void finish(void) {
  loop_remove_prepare(o.sw->loop, prepare, NULL);
  loop_remove_timer(o.hide_timer);
  o.hide_timer = NULL;
  layer_surface_destroy(o.surf);
  o.surf = NULL;
}

static int command(int argc, char **argv, struct reply *r) {
  char line[256] = "";
  size_t len = 0;
  for (int i = 0; i < argc && len < sizeof(line) - 1; ++i) {
    int n = snprintf(line + len, sizeof(line) - len, "%s%s", i ? " " : "",
                     argv[i]);
    if (n < 0)
      break;
    len += (size_t)n;
  }
  struct osd_event ev;
  if (argc == 0 || !osd_parse_line(line, o.cfg.max_pct, &ev)) {
    reply_printf(r,
                 "osd: cannot parse '%s'; expected vol|volume N [mute], "
                 "bri|brightness N, or N\n",
                 line);
    return 1;
  }
  on_event(&ev);
  return 0;
}

const struct swell_module osd_module = {
    .name = "osd",
    .usage = "  vol|volume N [mute]   show the volume (0..max_pct)\n"
             "  bri|brightness N      show the brightness\n"
             "  N                     same as 'vol N'\n",
    .needs_wayland = true,
    .config_size = sizeof(struct osd_config),
    .defaults = &defaults,
    .fields = fields,
    .nfields = ARRAY_LEN(fields),
    .init = init,
    .reconfigure = reconfigure,
    .finish = finish,
    .command = command,
};
