/* wsosd: a small panel that drops down from the top whenever the focused
 * workspace changes: one dot per workspace, the focused one stretched into
 * a bar that slides to its new place. It follows the window manager's
 * workspace.* bus events and stays away while the overview is open.
 * `swell wsosd show` shows it by hand. */
#include "wsosd.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "anim.h"
#include "bus.h"
#include "loop.h"
#include "module.h"
#include "render.h"
#include "surface.h"
#include "wm.h"
#include "util.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

static const struct wsosd_config defaults = {
    .namespace = "swell-wsosd",
    .height = 36,
    .top_gap = 12,
    .corner = 0,
    .pad = 18,
    .dot = 8,
    .dot_gap = 12,
    .active_width = 28,
    .max_dots = 10,
    .background = 0x000000f0u,
    .border = 0x6a6a6affu,
    .dot_color = 0x5a5a5affu,
    .active_dark = 0xa0a0a0ffu,
    .active_light = 0xd6d6d6ffu,
    .hide_timeout_ms = 900,
    .slide_ms = 220,
    .move_ms = 200,
};

#define S struct wsosd_config
static const struct cfg_field fields[] = {
    CFG_STRING(S, namespace, "layer-shell namespace of the surface"),
    CFG_INT(S, height, 1, 4096, "panel height, logical px"),
    CFG_INT(S, top_gap, 0, 1024,
            "gap between the top edge (or bar) and the panel"),
    CFG_INT(S, corner, 0, 512, "panel corner radius (0 = square)"),
    CFG_INT(S, pad, 0, 1024, "space left and right of the row of dots"),
    CFG_INT(S, dot, 1, 512, "dot diameter"),
    CFG_INT(S, dot_gap, 0, 512, "space between dots"),
    CFG_INT(S, active_width, 1, 2048, "length of the focused workspace's bar"),
    CFG_INT(S, max_dots, 1, 256,
            "surface sized for this many workspaces (grows past it)"),
    CFG_COLOR(S, background, "panel background"),
    CFG_COLOR(S, border, "1px panel border"),
    CFG_COLOR(S, dot_color, "dots of the other workspaces"),
    CFG_COLOR(S, active_dark, "focused bar gradient, right end"),
    CFG_COLOR(S, active_light, "focused bar gradient, left end"),
    CFG_INT(S, hide_timeout_ms, 1, 600000,
            "time after the last workspace change before retracting"),
    CFG_INT(S, slide_ms, 0, 10000, "drop-down / retract animation"),
    CFG_INT(S, move_ms, 0, 10000,
            "bar sliding to the new workspace, and the panel resizing"),
};
#undef S

static struct {
  struct swell *sw;
  struct wsosd_config cfg;
  struct layer_surface *surf;
  struct loop_timer *hide_timer;
  int surf_width; /* width the shown surface was created with */
  struct tween shown, pos, count;
  int index, n; /* last workspace state shown */
  bool retracting;
  bool overview; /* the overview is open */
} w;

static void snap(struct tween *tw, double to) {
  *tw = (struct tween){.from = to, .to = to};
}

static bool draw(cairo_t *cr, void *data) {
  double now = now_fms();
  struct wsosd_render_state rs = {
      .width = w.surf_width,
      .count = tween_value(&w.count, now),
      .pos = tween_value(&w.pos, now),
      .slide = tween_value(&w.shown, now),
  };
  wsosd_render_surface(cr, &w.cfg, &rs);
  return tween_progress(&w.shown, now) < 1.0 ||
         tween_progress(&w.pos, now) < 1.0 ||
         tween_progress(&w.count, now) < 1.0;
}

static int wanted_width(int n) {
  return (int)ceil(wsosd_panel_width(&w.cfg, n > w.cfg.max_dots
                                                 ? n
                                                 : w.cfg.max_dots));
}

/* Map the surface wide enough for n workspaces; a shown surface that is
 * too narrow is replaced. */
static void show_surface(int n) {
  int width = wanted_width(n);
  if (layer_surface_shown(w.surf) && width > w.surf_width)
    layer_surface_hide(w.surf);
  if (layer_surface_shown(w.surf))
    return;
  w.surf_width = width;
  struct layer_opts opts = {
      .ns = w.cfg.namespace,
      .width = width,
      .height = wsosd_surface_height(&w.cfg),
      .layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
      /* Top, centred horizontally; zone 0 keeps it below an exclusive bar. */
      .anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP,
      .exclusive_zone = 0,
      .click_through = true,
      .keyboard = false,
  };
  layer_surface_show(w.surf, &opts);
}

static bool visible(void) {
  return layer_surface_shown(w.surf) && !w.retracting;
}

/* The focused workspace's position in the strip's order and the count. */
static bool query(int *index, int *n) {
  wm_workspaces_invalidate();
  const struct wm_workspace *list;
  int count = wm_strip_workspaces(&list);
  for (int i = 0; i < count; ++i)
    if (list[i].focused) {
      *index = i;
      *n = count;
      return true;
    }
  return false;
}

static void emit(const char *name) {
  char index[48], count[48];
  snprintf(index, sizeof index, "SWELL_WORKSPACE_INDEX=%d", w.index + 1);
  snprintf(count, sizeof count, "SWELL_WORKSPACE_COUNT=%d", w.n);
  bus_emit(name, index, count, NULL);
}

/* Drop down (or stay down) showing the current workspaces. */
static void show(void) {
  int index, n;
  if (!query(&index, &n))
    return;
  bool was_visible = visible();
  double now = now_fms();
  if (was_visible) {
    tween_retarget(&w.pos, index, w.cfg.move_ms, now);
    tween_retarget(&w.count, n, w.cfg.move_ms, now);
  } else {
    snap(&w.pos, index);
    snap(&w.count, n);
  }
  if (w.shown.to < 1.0)
    tween_retarget(&w.shown, 1.0, w.cfg.slide_ms, now);
  w.index = index;
  w.n = n;
  w.retracting = false;
  show_surface(n);
  layer_surface_redraw(w.surf);
  loop_timer_arm(w.hide_timer, w.cfg.hide_timeout_ms, 0);
  if (!was_visible)
    emit("wsosd.show");
}

/* Workspaces appeared or went away while shown: follow without extending
 * the timeout. */
static void refresh(void) {
  int index, n;
  if (!visible() || !query(&index, &n) || (index == w.index && n == w.n))
    return;
  double now = now_fms();
  tween_retarget(&w.pos, index, w.cfg.move_ms, now);
  tween_retarget(&w.count, n, w.cfg.move_ms, now);
  w.index = index;
  w.n = n;
  show_surface(n);
  layer_surface_redraw(w.surf);
}

static void retract(void) {
  if (!visible())
    return;
  tween_retarget(&w.shown, 0.0, w.cfg.slide_ms, now_fms());
  w.retracting = true;
  loop_timer_disarm(w.hide_timer);
  layer_surface_redraw(w.surf);
  emit("wsosd.hide");
}

static void on_hide_timer(struct loop_timer *t, void *data) { retract(); }

/* Destroy the surface once the retract has run. The timeout finishes it
 * even if no frame callbacks arrive (e.g. the output is off). */
static int prepare(void *data) {
  if (!w.retracting)
    return -1;
  double left = w.shown.t0 + w.shown.dur - now_fms();
  if (left > 0)
    return (int)ceil(left) + 1;
  layer_surface_hide(w.surf);
  w.retracting = false;
  return -1;
}

static void on_bus(const struct bus_event *ev, void *data) {
  if (strcmp(ev->name, "overview.open") == 0) {
    w.overview = true;
    retract();
  } else if (strcmp(ev->name, "overview.close") == 0) {
    w.overview = false;
  } else if (strcmp(ev->name, "workspace.focus") == 0) {
    if (!w.overview)
      show();
  } else if (strcmp(ev->name, "workspace.init") == 0 ||
             strcmp(ev->name, "workspace.removed") == 0 ||
             strcmp(ev->name, "workspace.move") == 0) {
    refresh();
  }
}

static bool init(struct swell *sw, const void *cfg) {
  if (!sw->wl->layer_shell) {
    warn("wsosd: the compositor lacks wlr-layer-shell");
    return false;
  }
  w.sw = sw;
  w.cfg = *(const struct wsosd_config *)cfg;
  w.retracting = false;
  w.overview = false;
  snap(&w.shown, 0);
  w.surf = layer_surface_create(sw->wl, draw, NULL);
  w.hide_timer = loop_add_timer(sw->loop, CLOCK_MONOTONIC, on_hide_timer, NULL);
  loop_add_prepare(sw->loop, 50, prepare, NULL);
  bus_subscribe(on_bus, NULL);
  return true;
}

static void reconfigure(const void *cfg) {
  w.cfg = *(const struct wsosd_config *)cfg;
  if (!layer_surface_shown(w.surf))
    return;
  /* New sizes need a new surface: re-show it, or finish a retract now. */
  layer_surface_hide(w.surf);
  if (w.retracting) {
    w.retracting = false;
    return;
  }
  show_surface(w.n);
  layer_surface_redraw(w.surf);
}

static void finish(void) {
  bus_unsubscribe(on_bus, NULL);
  loop_remove_prepare(w.sw->loop, prepare, NULL);
  loop_remove_timer(w.hide_timer);
  w.hide_timer = NULL;
  layer_surface_destroy(w.surf);
  w.surf = NULL;
}

static int command(int argc, char **argv, struct reply *r) {
  const char *cmd = argc ? argv[0] : "";
  if (argc == 1 && strcmp(cmd, "show") == 0)
    show();
  else if (argc == 1 && strcmp(cmd, "hide") == 0)
    retract();
  else if (argc == 1 && strcmp(cmd, "status") == 0)
    reply_printf(r, "%s %d/%d\n", visible() ? "shown" : "hidden", w.index + 1,
                 w.n);
  else {
    reply_printf(r, "usage: swell wsosd show|hide|status\n");
    return 1;
  }
  return 0;
}

const struct swell_module wsosd_module = {
    .name = "wsosd",
    .usage = "  show      show the workspaces now\n"
             "  hide      retract the panel\n"
             "  status    shown|hidden and focused/count\n",
    .needs_wayland = true,
    .needs_wm = true,
    .config_size = sizeof(struct wsosd_config),
    .defaults = &defaults,
    .fields = fields,
    .nfields = ARRAY_LEN(fields),
    .init = init,
    .reconfigure = reconfigure,
    .finish = finish,
    .command = command,
};
