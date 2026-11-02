/* lowbat: a panel that drops down from the top when the battery runs low.
 * At low_pct it pulses red and retracts after low_timeout_ms; at
 * critical_pct it pulses and then stays down, still (no frames), until the
 * battery charges or `swell lowbat hide`. The battery is read every
 * interval_s; state.c decides when to warn. */
#include "lowbat.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "anim.h"
#include "battery.h"
#include "bus.h"
#include "loop.h"
#include "module.h"
#include "render.h"
#include "state.h"
#include "surface.h"
#include "util.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/* First reading this long after start, so a warning at login drops down
 * once the session is up. */
#define FIRST_READ_MS 2000

static const struct lowbat_config defaults = {
    .namespace = "swell-lowbat",
    .low_pct = 15,
    .critical_pct = 5,
    .interval_s = 30,
    .width = 300,
    .height = 52,
    .top_gap = 12,
    .corner = 0,
    .pad = 18,
    .body_width = 44,
    .body_height = 22,
    .nub_width = 4,
    .nub_height = 10,
    .font = "IosevkaTerm Nerd Font",
    .font_size = 20,
    .background = 0x000000f0u,
    .border = 0x6a6a6affu,
    .track = 0x333333ffu,
    .text = 0xf2f2f2ffu,
    .pulse_light = 0xff3b3bffu,
    .pulse_dark = 0xb81414ffu,
    .low_timeout_ms = 6000,
    .slide_ms = 220,
    .pulse_ms = 900,
    .pulse_count = 3,
};

#define S struct lowbat_config
static const struct cfg_field fields[] = {
    CFG_STRING(S, namespace, "layer-shell namespace of the surface"),
    CFG_INT(S, low_pct, 0, 100,
            "discharging at or below this: a short warning"),
    CFG_INT(S, critical_pct, 0, 100,
            "discharging at or below this: a warning that stays until AC"),
    CFG_INT(S, interval_s, 1, 3600, "how often the battery is read"),
    CFG_INT(S, width, 1, 4096, "panel width, logical px"),
    CFG_INT(S, height, 1, 4096, "panel height, logical px"),
    CFG_INT(S, top_gap, 0, 1024,
            "gap between the top edge (or bar) and the panel"),
    CFG_INT(S, corner, 0, 512, "panel corner radius (0 = square)"),
    CFG_INT(S, pad, 0, 1024, "space left of the battery and before the text"),
    CFG_INT(S, body_width, 8, 1024, "battery outline width"),
    CFG_INT(S, body_height, 8, 1024, "battery outline height"),
    CFG_INT(S, nub_width, 0, 256, "battery terminal width"),
    CFG_INT(S, nub_height, 0, 256, "battery terminal height"),
    CFG_STRING(S, font, "font family of the text"),
    CFG_INT(S, font_size, 1, 512, "text size"),
    CFG_COLOR(S, background, "panel background"),
    CFG_COLOR(S, border, "1px panel border"),
    CFG_COLOR(S, track, "empty part of the battery"),
    CFG_COLOR(S, text, "text and battery outline between pulses"),
    CFG_COLOR(S, pulse_light, "level fill, and text/outline at a pulse peak"),
    CFG_COLOR(S, pulse_dark, "level fill's left end between pulses"),
    CFG_INT(S, low_timeout_ms, 1, 600000,
            "time the low warning stays before retracting"),
    CFG_INT(S, slide_ms, 0, 10000, "drop-down / retract animation"),
    CFG_INT(S, pulse_ms, 1, 10000, "one red pulse"),
    CFG_INT(S, pulse_count, 0, 100,
            "pulses after dropping down; then the panel holds still"),
};
#undef S

static struct {
  struct swell *sw;
  struct lowbat_config cfg;
  struct layer_surface *surf;
  struct loop_timer *hide_timer, *battery_timer;
  struct lowbat_state state;
  struct tween shown;
  double pulse_t0; /* when the pulses started */
  int pct;         /* level shown */
  bool critical;   /* the warning shown is the critical one */
  bool manual;     /* shown by `swell lowbat show`: keeps its level */
  bool retracting;
} b;

static double pulse_level(double now) {
  double t = now - b.pulse_t0, len = (double)b.cfg.pulse_ms * b.cfg.pulse_count;
  if (t >= len || t < 0)
    return 1.0;
  /* Starts and ends lit; dims in between. */
  return 0.5 + 0.5 * cos(2 * M_PI * t / b.cfg.pulse_ms);
}

static bool pulsing(double now) {
  return now - b.pulse_t0 < (double)b.cfg.pulse_ms * b.cfg.pulse_count;
}

static bool draw(cairo_t *cr, void *data) {
  double now = now_fms();
  struct lowbat_render_state rs = {
      .pct = b.pct,
      .critical = b.critical,
      .slide = tween_value(&b.shown, now),
      .pulse = pulse_level(now),
  };
  lowbat_render_surface(cr, &b.cfg, &rs);
  return tween_progress(&b.shown, now) < 1.0 || pulsing(now);
}

static bool visible(void) {
  return layer_surface_shown(b.surf) && !b.retracting;
}

static void show_surface(void) {
  if (layer_surface_shown(b.surf))
    return;
  struct layer_opts opts = {
      .ns = b.cfg.namespace,
      .width = b.cfg.width,
      .height = lowbat_surface_height(&b.cfg),
      .layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
      .anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP,
      .exclusive_zone = 0,
      .click_through = true,
      .keyboard = false,
  };
  layer_surface_show(b.surf, &opts);
}

static void emit(const char *name) {
  char pct[48];
  snprintf(pct, sizeof pct, "SWELL_BATTERY=%d", b.pct);
  bus_emit(name, pct, NULL);
}

static void show(int pct, bool critical) {
  double now = now_fms();
  b.pct = pct;
  b.critical = critical;
  b.manual = false;
  b.pulse_t0 = now;
  if (b.shown.to < 1.0)
    tween_retarget(&b.shown, 1.0, b.cfg.slide_ms, now);
  b.retracting = false;
  show_surface();
  layer_surface_redraw(b.surf);
  if (critical)
    loop_timer_disarm(b.hide_timer);
  else
    loop_timer_arm(b.hide_timer, b.cfg.low_timeout_ms, 0);
  emit(critical ? "lowbat.critical" : "lowbat.low");
}

static void retract(void) {
  if (!visible())
    return;
  tween_retarget(&b.shown, 0.0, b.cfg.slide_ms, now_fms());
  b.retracting = true;
  loop_timer_disarm(b.hide_timer);
  layer_surface_redraw(b.surf);
  emit("lowbat.hide");
}

static void on_hide_timer(struct loop_timer *t, void *data) { retract(); }

static void check(void) {
  int pct;
  bool on_ac;
  if (!battery_read(&pct, &on_ac))
    pct = -1;
  switch (
      lowbat_step(&b.state, pct, on_ac, b.cfg.low_pct, b.cfg.critical_pct)) {
  case LOWBAT_LOW:
    show(pct, false);
    break;
  case LOWBAT_CRITICAL:
    show(pct, true);
    break;
  case LOWBAT_CLEAR:
    retract();
    break;
  case LOWBAT_NONE:
    /* Keep the level on a shown panel current. */
    if (visible() && !b.manual && pct >= 0 && pct != b.pct) {
      b.pct = pct;
      layer_surface_redraw(b.surf);
    }
    break;
  }
}

static void on_battery_timer(struct loop_timer *t, void *data) { check(); }

static void arm_battery(int64_t first_ms) {
  loop_timer_arm(b.battery_timer, first_ms, b.cfg.interval_s * 1000LL);
}

/* Destroy the surface once the retract has run. The timeout finishes it
 * even if no frame callbacks arrive (e.g. the output is off). */
static int prepare(void *data) {
  if (!b.retracting)
    return -1;
  double left = b.shown.t0 + b.shown.dur - now_fms();
  if (left > 0)
    return (int)ceil(left) + 1;
  layer_surface_hide(b.surf);
  b.retracting = false;
  return -1;
}

static bool init(struct swell *sw, const void *cfg) {
  if (!sw->wl->layer_shell) {
    warn("lowbat: the compositor lacks wlr-layer-shell");
    return false;
  }
  b.sw = sw;
  b.cfg = *(const struct lowbat_config *)cfg;
  b.state = (struct lowbat_state){0};
  b.retracting = false;
  b.shown = (struct tween){0};
  b.surf = layer_surface_create(sw->wl, draw, NULL);
  b.hide_timer = loop_add_timer(sw->loop, CLOCK_MONOTONIC, on_hide_timer, NULL);
  b.battery_timer =
      loop_add_timer(sw->loop, CLOCK_MONOTONIC, on_battery_timer, NULL);
  loop_add_prepare(sw->loop, 50, prepare, NULL);
  arm_battery(FIRST_READ_MS);
  return true;
}

static void reconfigure(const void *cfg) {
  b.cfg = *(const struct lowbat_config *)cfg;
  arm_battery(b.cfg.interval_s * 1000LL);
  if (!layer_surface_shown(b.surf))
    return;
  /* New sizes need a new surface: re-show it, or finish a retract now. */
  layer_surface_hide(b.surf);
  if (b.retracting) {
    b.retracting = false;
    return;
  }
  show_surface();
  layer_surface_redraw(b.surf);
}

static void finish(void) {
  loop_remove_prepare(b.sw->loop, prepare, NULL);
  loop_remove_timer(b.hide_timer);
  loop_remove_timer(b.battery_timer);
  b.hide_timer = b.battery_timer = NULL;
  layer_surface_destroy(b.surf);
  b.surf = NULL;
}

static int usage(struct reply *r) {
  reply_printf(r,
               "usage: swell lowbat show [low|critical] [PCT]|hide|status\n");
  return 1;
}

static int command(int argc, char **argv, struct reply *r) {
  const char *cmd = argc ? argv[0] : "";
  if (strcmp(cmd, "show") == 0 && argc <= 3) {
    bool critical = false;
    int pct = -1, i = 1;
    if (i < argc && strcmp(argv[i], "critical") == 0)
      critical = true, ++i;
    else if (i < argc && strcmp(argv[i], "low") == 0)
      ++i;
    if (i < argc) {
      char *end;
      long v = strtol(argv[i], &end, 10);
      if (*end || end == argv[i] || v < 0 || v > 100)
        return usage(r);
      pct = (int)v, ++i;
    }
    if (i != argc)
      return usage(r);
    bool on_ac;
    if (pct < 0 && !battery_read(&pct, &on_ac))
      pct = 0;
    show(pct, critical);
    b.manual = true;
  } else if (argc == 1 && strcmp(cmd, "hide") == 0) {
    retract();
  } else if (argc == 1 && strcmp(cmd, "status") == 0) {
    reply_printf(r, "%s %s %d%%\n", visible() ? "shown" : "hidden",
                 !visible()   ? "none"
                 : b.critical ? "critical"
                              : "low",
                 b.pct);
  } else {
    return usage(r);
  }
  return 0;
}

const struct swell_module lowbat_module = {
    .name = "lowbat",
    .usage = "  show [low|critical] [PCT]  show the warning now\n"
             "  hide      retract the panel (until the next level)\n"
             "  status    shown|hidden, low|critical|none, level\n",
    .needs_wayland = true,
    .config_size = sizeof(struct lowbat_config),
    .defaults = &defaults,
    .fields = fields,
    .nfields = ARRAY_LEN(fields),
    .init = init,
    .reconfigure = reconfigure,
    .finish = finish,
    .command = command,
};
