/* popup: a "hold to peek" overlay. `swell popup show` slides a panel up
 * from the bottom with a big clock; held for expand_delay_ms it grows to the
 * full details (date, levels, network, media, stats, workspaces).
 * `swell popup hide` slides it back down. */
#include "popup.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "anim.h"
#include "bus.h"
#include "info.h"
#include "loop.h"
#include "module.h"
#include "render.h"
#include "surface.h"
#include "sway.h"
#include "util.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

static const struct popup_config defaults = {
    .namespace = "popup",
    .width = 560,
    .height = 529,
    .pad = 36,
    .bottom_gap = 12,
    .font = "IosevkaTerm Nerd Font",
    .font_clock = 112,
    .font_seconds = 30,
    .font_date = 20,
    .font_row = 18,
    .font_ws = 16,
    .background = 0x000000f0u,
    .border = 0x6a6a6affu,
    .track = 0x333333ffu,
    .bar_dark = 0xa0a0a0ffu,
    .bar_light = 0xd6d6d6ffu,
    .text = 0xf2f2f2ffu,
    .dim = 0x949494ffu,
    .muted = 0x5a5a5affu,
    .ws_focus = 0x2e2e2effu,
    .warn = 0xff3b3bffu,
    .battery_low_pct = 15,
    .refresh_ms = 1000,
    .command_timeout_ms = 3000,
    .pop_ms = 220,
    .close_ms = 220,
    .expand_delay_ms = 2000,
    .expand_ms = 260,
    .media_ms = 260,
    .gap_date = 28,
    .gap_sep = 34,
    .gap_cols = 34,
    .gap_bar = 14,
    .bar_h = 4,
    .col_inset = 14,
    .gap_rows = 40,
    .row_step = 42,
    .sec_gap = 4,
    .icon_gap = 8,
    .mini_font = 15,
    .mini_gap = 7,
    .ws_h = 32,
    .ws_min_w = 36,
    .ws_text_pad = 11,
    .ws_gap = 2,
    .gap_ws = 26,
    .wsc_min_w = 30,
    .wsc_max_w = 92,
    .wsc_inset = 2,
    .wsc_arrow = 4,
    .ws_arrow_w = 16,
};

#define S struct popup_config
static const struct cfg_field fields[] = {
    CFG_STRING(S, namespace, "layer-shell namespace of the surface"),
    CFG_INT(S, width, 1, 4096, "panel width, logical px"),
    CFG_INT(S, height, 1, 4096,
            "tallest panel (with a media row) and the surface height"),
    CFG_INT(S, pad, 0, 1024, "inner padding of the panel"),
    CFG_INT(S, bottom_gap, 0, 1024,
            "gap between the bottom edge (or bar) and the panel"),
    CFG_STRING(S, font, "font family (cairo toy font API)"),
    CFG_INT(S, font_clock, 1, 1024, "HH:MM size"),
    CFG_INT(S, font_seconds, 1, 1024, ":SS size"),
    CFG_INT(S, font_date, 1, 1024, "date size"),
    CFG_INT(S, font_row, 1, 1024, "level, row and workspace strip size"),
    CFG_INT(S, font_ws, 1, 1024, "compact workspace column size"),
    CFG_COLOR(S, background, "panel background"),
    CFG_COLOR(S, border, "1px panel border"),
    CFG_COLOR(S, track, "bar tracks and separators"),
    CFG_COLOR(S, bar_dark, "level bar gradient, left end"),
    CFG_COLOR(S, bar_light, "level bar gradient, right end"),
    CFG_COLOR(S, text, "main text"),
    CFG_COLOR(S, dim, "secondary text"),
    CFG_COLOR(S, muted, "labels, muted and missing values"),
    CFG_COLOR(S, ws_focus, "focused workspace cell"),
    CFG_COLOR(S, warn, "low battery and urgent workspaces"),
    CFG_INT(S, battery_low_pct, 0, 100,
            "battery at or below this (discharging) is shown in warn"),
    CFG_INT(S, refresh_ms, 100, 600000, "refresh period while shown"),
    CFG_INT(S, command_timeout_ms, 100, 600000,
            "kill wpctl/nmcli/playerctl if one is still running after this"),
    CFG_INT(S, pop_ms, 0, 10000, "slide up into place"),
    CFG_INT(S, close_ms, 0, 10000, "slide back down"),
    CFG_INT(S, expand_delay_ms, 0, 600000,
            "hold this long to reveal the details"),
    CFG_INT(S, expand_ms, 0, 10000, "clock-only to full panel"),
    CFG_INT(S, media_ms, 0, 10000, "media row opening/closing while expanded"),
    CFG_DOUBLE(S, gap_date, 0, 1024, "clock baseline to date cap top"),
    CFG_DOUBLE(S, gap_sep, 0, 1024, "date baseline to separator"),
    CFG_DOUBLE(S, gap_cols, 0, 1024,
               "separator to level cap top (and last row to footer)"),
    CFG_DOUBLE(S, gap_bar, 0, 1024, "level baseline to bar top"),
    CFG_DOUBLE(S, bar_h, 0, 1024, "level bar height"),
    CFG_DOUBLE(S, col_inset, 0, 1024, "bar inset inside each level column"),
    CFG_DOUBLE(S, gap_rows, 0, 1024, "bar bottom to first row cap top"),
    CFG_DOUBLE(S, row_step, 0, 1024, "row baseline to baseline"),
    CFG_DOUBLE(S, sec_gap, 0, 1024, "HH:MM to :SS"),
    CFG_DOUBLE(S, icon_gap, 0, 1024, "icon ink to its text"),
    CFG_DOUBLE(S, mini_font, 1, 1024, "compact-panel level indicator size"),
    CFG_DOUBLE(S, mini_gap, 0, 1024, "mini icon ink to its value"),
    CFG_DOUBLE(S, ws_h, 0, 1024, "workspace strip cell height"),
    CFG_DOUBLE(S, ws_min_w, 0, 4096, "workspace strip cell minimum width"),
    CFG_DOUBLE(S, ws_text_pad, 0, 1024, "text inset each side of a cell"),
    CFG_DOUBLE(S, ws_gap, 0, 1024, "gap between strip cells"),
    CFG_DOUBLE(S, gap_ws, 0, 1024, "lower separator to the strip"),
    CFG_DOUBLE(S, wsc_min_w, 0, 4096, "compact column cell minimum width"),
    CFG_DOUBLE(S, wsc_max_w, 0, 4096, "compact column cell maximum width"),
    CFG_DOUBLE(S, wsc_inset, 0, 1024, "compact cells inset inside their row"),
    CFG_DOUBLE(S, wsc_arrow, 0, 1024, "\"more\" arrow half-width"),
    CFG_DOUBLE(S, ws_arrow_w, 0, 1024, "strip edge room for a \"more\" arrow"),
};
#undef S

static struct {
  struct swell *sw;
  struct popup_config cfg;
  struct layer_surface *surf;
  struct loop_timer *tick;   /* CLOCK_REALTIME, just after each second */
  struct loop_timer *expand; /* expand_delay_ms after show */
  struct info info;
  int64_t pop_at;          /* ms, CLOCK_MONOTONIC: start of the pop tween */
  double pop_from, pop_to; /* 0 -> 1 open, x -> 0 close */
  bool closing;            /* surface is destroyed once the close ends */
  int64_t expand_at;       /* ms, 0 until the details are revealed */
  int64_t media_at;        /* ms: start of the media row tween */
  double media_from, media_to;
} p;

static double tween(int64_t t0, int ms, int64_t now, bool *more) {
  return ease_out_cubic(anim_progress(t0, ms, now, more));
}

static int pop_ms(void) { return p.closing ? p.cfg.close_ms : p.cfg.pop_ms; }

static double pop_value(int64_t now, bool *more) {
  double t = tween(p.pop_at, pop_ms(), now, more);
  return p.pop_from + (p.pop_to - p.pop_from) * t;
}

static double media_value(int64_t now, bool *more) {
  double t = tween(p.media_at, p.cfg.media_ms, now, more);
  return p.media_from + (p.media_to - p.media_from) * t;
}

/* Follow info.has_media: tween the media row while the details are
 * showing, otherwise just snap to it. */
static void sync_media(void) {
  double to = p.info.has_media ? 1 : 0;
  if (to == p.media_to)
    return;
  bool more = false;
  int64_t now = now_ms();
  bool showing = p.expand_at && layer_surface_shown(p.surf);
  p.media_from = showing ? media_value(now, &more) : to;
  p.media_to = to;
  p.media_at = now;
}

static bool draw(cairo_t *cr, void *data) {
  int64_t now = now_ms();
  bool more = false;
  double pop = pop_value(now, &more);
  double expand =
      p.expand_at ? tween(p.expand_at, p.cfg.expand_ms, now, &more) : 0;
  double media = media_value(now, &more);
  popup_render(cr, &p.cfg, &p.info, pop, expand, media);
  return more;
}

static void show_surface(void) {
  struct layer_opts opts = {
      .ns = p.cfg.namespace,
      .width = p.cfg.width,
      .height = popup_surface_height(&p.cfg),
      .layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
      /* Bottom, centred horizontally; zone 0 keeps it above an exclusive
       * bar. */
      .anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
      .exclusive_zone = 0,
      .click_through = true,
      .keyboard = false,
  };
  layer_surface_show(p.surf, &opts);
}

/* Tick just after each wall-clock second so the seconds change on time. */
static void arm_tick(bool on) {
  if (!on) {
    loop_timer_disarm(p.tick);
    return;
  }
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  struct itimerspec its = {
      .it_value = {.tv_sec = now.tv_sec + 1, .tv_nsec = 5000000L},
      .it_interval = {.tv_sec = p.cfg.refresh_ms / 1000,
                      .tv_nsec = (p.cfg.refresh_ms % 1000) * 1000000L},
  };
  loop_timer_arm_abs(p.tick, &its);
}

/* Pull sway's (cached) workspaces into the info; true if they changed. */
static bool refresh_workspaces(void) {
  if (!p.sw->sway)
    return false;
  const struct sway_workspace *ws;
  int n = sway_workspaces(&ws);
  return info_set_workspaces(&p.info, ws, n);
}

/* Start a pop tween from wherever the current one is. */
static void retarget_pop(double to, bool closing) {
  bool more = false;
  int64_t now = now_ms();
  p.pop_from = pop_value(now, &more);
  p.pop_to = to;
  p.pop_at = now;
  p.closing = closing;
}

static bool visible(void) { return layer_surface_shown(p.surf) && !p.closing; }

static void set_shown(bool show) {
  if (show == visible())
    return;
  if (show) {
    if (!layer_surface_shown(p.surf)) {
      p.pop_at = now_ms();
      p.pop_from = 0;
      p.expand_at = 0;
      show_surface();
    }
    /* Reopening mid-close resumes from the current position and keeps the
     * details if they were already revealed. */
    retarget_pop(1, false);
    info_refresh(&p.info, p.cfg.refresh_ms, p.cfg.command_timeout_ms);
    if (p.sw->sway)
      sway_workspaces_invalidate();
    refresh_workspaces();
    if (!p.expand_at)
      loop_timer_arm(p.expand, p.cfg.expand_delay_ms > 0
                                   ? p.cfg.expand_delay_ms
                                   : 1, 0);
  } else {
    retarget_pop(0, true);
    loop_timer_disarm(p.expand);
  }
  arm_tick(show);
  layer_surface_redraw(p.surf);
  bus_emit(show ? "popup.show" : "popup.hide", NULL);
}

static void on_tick(struct loop_timer *t, void *data) {
  if (!layer_surface_shown(p.surf))
    return;
  info_refresh(&p.info, p.cfg.refresh_ms, p.cfg.command_timeout_ms);
  refresh_workspaces();
  layer_surface_redraw(p.surf);
}

static void on_expand(struct loop_timer *t, void *data) {
  if (!visible())
    return;
  p.expand_at = now_ms();
  layer_surface_redraw(p.surf);
  bus_emit("popup.expand", NULL);
}

/* Command output lands even when hidden; redraw only if shown. */
static void on_info_changed(void *data) {
  sync_media();
  if (layer_surface_shown(p.surf))
    layer_surface_redraw(p.surf);
}

/* Workspace events keep the strip current while shown. */
static void on_sway_event(const I3ipc_event *ev, void *data) {
  if (ev->type != I3IPC_EVENT_WORKSPACE || !layer_surface_shown(p.surf))
    return;
  if (refresh_workspaces())
    layer_surface_redraw(p.surf);
}

/* Destroy the surface once the close tween has run. The timeout ends a
 * close even if no frame callbacks arrive (e.g. the output is off). */
static int prepare(void *data) {
  if (!p.closing)
    return -1;
  int64_t left = p.pop_at + p.cfg.close_ms - now_ms();
  if (left > 0)
    return (int)left;
  layer_surface_hide(p.surf);
  p.closing = false;
  return -1;
}

static bool init(struct swell *sw, const void *cfg) {
  if (!sw->wl->layer_shell) {
    warn("popup: the compositor lacks wlr-layer-shell");
    return false;
  }
  memset(&p, 0, sizeof(p));
  p.sw = sw;
  p.cfg = *(const struct popup_config *)cfg;
  p.info.cpu = -1;
  p.info.net_signal = -1;
  p.surf = layer_surface_create(sw->wl, draw, NULL);
  p.tick = loop_add_timer(sw->loop, CLOCK_REALTIME, on_tick, NULL);
  p.expand = loop_add_timer(sw->loop, CLOCK_MONOTONIC, on_expand, NULL);
  loop_add_prepare(sw->loop, 50, prepare, NULL);
  info_init(sw->loop, &p.info, on_info_changed, NULL);
  if (sw->sway)
    sway_subscribe(on_sway_event, NULL);
  else
    warn("popup: no sway connection; workspaces are not shown");
  return true;
}

static void reconfigure(const void *cfg) {
  p.cfg = *(const struct popup_config *)cfg;
  if (!layer_surface_shown(p.surf))
    return;
  /* New sizes need a new surface: re-show it, or finish a close now. */
  layer_surface_hide(p.surf);
  if (p.closing) {
    p.closing = false;
    return;
  }
  show_surface();
  arm_tick(true);
  layer_surface_redraw(p.surf);
}

static void finish(void) {
  if (p.sw->sway)
    sway_unsubscribe(on_sway_event, NULL);
  info_finish();
  loop_remove_prepare(p.sw->loop, prepare, NULL);
  loop_remove_timer(p.tick);
  loop_remove_timer(p.expand);
  p.tick = p.expand = NULL;
  layer_surface_destroy(p.surf);
  p.surf = NULL;
}

static int command(int argc, char **argv, struct reply *r) {
  const char *word = argc > 0 ? argv[0] : "";
  if (!strcmp(word, "show"))
    set_shown(true);
  else if (!strcmp(word, "hide"))
    set_shown(false);
  else if (!strcmp(word, "toggle"))
    set_shown(!visible());
  else {
    reply_printf(r, "popup: unknown command '%s'; expected show, hide or "
                    "toggle\n",
                 word);
    return 1;
  }
  return 0;
}

const struct swell_module popup_module = {
    .name = "popup",
    .usage = "  show     slide the panel up (bind to a key press)\n"
             "  hide     slide it back down (bind to the key release)\n"
             "  toggle   show or hide\n",
    .needs_wayland = true,
    .needs_sway = false, /* without sway there are just no workspaces */
    .config_size = sizeof(struct popup_config),
    .defaults = &defaults,
    .fields = fields,
    .nfields = ARRAY_LEN(fields),
    .init = init,
    .reconfigure = reconfigure,
    .finish = finish,
    .command = command,
};
