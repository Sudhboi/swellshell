/* wsfade: a fade on sway workspace switches made with `swell ws`. The
 * output is covered with a snapshot of the old workspace, sway switches
 * behind it, and the snapshot fades out while drifting a little away from
 * where the new workspace lies. The new workspace is live underneath from
 * the first frame; input goes straight to it.
 *
 * The snapshot sits in a subsurface of an otherwise empty overlay layer
 * surface; each frame only moves it and changes its alpha multiplier, so
 * the compositor does all the work. Off by default. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "alpha-modifier-v1-client-protocol.h"
#include "bus.h"
#include "curve.h"
#include "module.h"
#include "snap.h"
#include "util.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wsswitch.h"

struct wsfade_config {
  char namespace[64]; /* layer-shell namespace */
  int fade_ms;
  int drift_px;
  int snapshot_timeout_ms;
};

static const struct wsfade_config defaults = {
    .namespace = "swell-wsfade",
    .fade_ms = 160,
    .drift_px = 60,
    .snapshot_timeout_ms = 50,
};

#define S struct wsfade_config
static const struct cfg_field fields[] = {
    CFG_STRING(S, namespace, "layer-shell namespace of the overlay"),
    CFG_INT(S, fade_ms, 1, 5000, "the old workspace fading out"),
    CFG_INT(S, drift_px, 0, 2000,
            "how far the old workspace moves while it fades, logical px"),
    CFG_INT(S, snapshot_timeout_ms, 1, 1000,
            "switch without a fade if the snapshot takes longer than this"),
};
#undef S

enum state {
  IDLE,
  SNAPPING, /* waiting for the snapshot and the layer surface's configure */
  COVERING, /* the snapshot is up; switch once it has been shown */
  FADING,
};

static struct {
  struct swell *sw;
  struct wsfade_config cfg;
  struct loop_timer *timeout;
  bool overview;
  enum state state;

  /* the switch waiting for the cover */
  wsswitch_go_fn go;
  void *go_data;
  int dir;
  int64_t asked, switched; /* ms: last begin, and its switch */

  struct snap *snap;
  struct buffer *shot;
  struct buffer *clear; /* 1x1 transparent, so the parent is mapped */
  struct wl_surface *surface, *sub_surface;
  struct zwlr_layer_surface_v1 *layer;
  struct wl_subsurface *sub;
  struct wp_viewport *viewport, *sub_viewport;
  struct wp_alpha_modifier_surface_v1 *alpha;
  struct wl_callback *frame_cb;
  int width, height;
  bool configured;

  struct wsfade_curve curve;
} f;

static void teardown(void) {
  loop_timer_disarm(f.timeout);
  if (f.frame_cb)
    wl_callback_destroy(f.frame_cb);
  if (f.alpha)
    wp_alpha_modifier_surface_v1_destroy(f.alpha);
  if (f.sub_viewport)
    wp_viewport_destroy(f.sub_viewport);
  if (f.sub)
    wl_subsurface_destroy(f.sub);
  if (f.sub_surface)
    wl_surface_destroy(f.sub_surface);
  if (f.viewport)
    wp_viewport_destroy(f.viewport);
  if (f.layer)
    zwlr_layer_surface_v1_destroy(f.layer);
  if (f.surface)
    wl_surface_destroy(f.surface);
  buffer_destroy(f.clear);
  snap_destroy(f.snap); /* frees the shot */
  f.frame_cb = NULL;
  f.alpha = NULL;
  f.sub_viewport = NULL;
  f.sub = NULL;
  f.sub_surface = NULL;
  f.viewport = NULL;
  f.layer = NULL;
  f.surface = NULL;
  f.clear = NULL;
  f.snap = NULL;
  f.shot = NULL;
  f.configured = false;
  f.state = IDLE;
}

/* Run the waiting switch, once. */
static void run_go(void) {
  wsswitch_go_fn go = f.go;
  f.go = NULL;
  if (go) {
    go(f.go_data);
    f.switched = now_ms();
  }
}

/* Give up on the fade: switch now if that is still to come. */
static void abandon(void) {
  run_go();
  teardown();
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time);

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

/* Commit the snapshot, asking for the next frame on it (the parent under
 * it is hidden, and hidden surfaces get no frame events), then the parent,
 * which applies it. */
static void commit_frame(void) {
  f.frame_cb = wl_surface_frame(f.sub_surface);
  wl_callback_add_listener(f.frame_cb, &frame_listener, NULL);
  wl_surface_commit(f.sub_surface);
  wl_surface_commit(f.surface);
}

/* Move and fade the snapshot to where the curve is now. */
static void render(double now) {
  double a = tween_value(&f.curve.alpha, now);
  int x = (int)lround(tween_value(&f.curve.x, now));
  wl_subsurface_set_position(f.sub, x, 0);
  wp_alpha_modifier_surface_v1_set_multiplier(f.alpha,
                                              (uint32_t)lround(a * UINT32_MAX));
  commit_frame();
}

/* Frames can stop (output off): end the fade on time regardless. */
static void arm_fade_timeout(double now) {
  double left = f.curve.alpha.t0 + f.curve.alpha.dur - now;
  loop_timer_arm(f.timeout, (int64_t)ceil(left > 0 ? left : 0) + 100, 0);
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
  wl_callback_destroy(cb);
  f.frame_cb = NULL;
  double now = now_fms();
  if (f.state == COVERING) {
    /* the old workspace is on screen in our overlay: switch behind it */
    run_go();
    wsfade_start(&f.curve, f.dir, f.cfg.drift_px, f.cfg.fade_ms, now);
    f.state = FADING;
    arm_fade_timeout(now);
  } else if (f.state != FADING) {
    return;
  }
  if (wsfade_done(&f.curve, now))
    teardown();
  else
    render(now);
}

/* Both the snapshot and the configure are in: put the snapshot up. */
static void cover(void) {
  if (f.state != SNAPPING || !f.shot || !f.configured)
    return;
  wl_surface_set_buffer_scale(f.sub_surface, 1);
  wp_viewport_set_destination(f.sub_viewport, f.width, f.height);
  wl_surface_attach(f.sub_surface, f.shot->wl_buffer, 0, 0);
  wl_surface_damage_buffer(f.sub_surface, 0, 0, f.shot->width, f.shot->height);
  wl_subsurface_set_position(f.sub, 0, 0);
  wp_alpha_modifier_surface_v1_set_multiplier(f.alpha, UINT32_MAX);

  wp_viewport_set_destination(f.viewport, f.width, f.height);
  wl_surface_attach(f.surface, f.clear->wl_buffer, 0, 0);
  wl_surface_damage_buffer(f.surface, 0, 0, 1, 1);
  commit_frame();
  f.state = COVERING;
}

static void on_snap(struct snap *s, struct buffer *buf, void *data) {
  if (!buf) {
    abandon();
    return;
  }
  f.shot = buf;
  cover();
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer,
                            uint32_t serial, uint32_t w, uint32_t h) {
  zwlr_layer_surface_v1_ack_configure(layer, serial);
  f.width = w;
  f.height = h;
  f.configured = w > 0 && h > 0;
  cover();
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer) {
  abandon();
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

static struct swell_output *find_output(const char *name) {
  struct swell_output *so;
  wl_list_for_each(so, &f.sw->wl->outputs, link) {
    if (so->name && strcmp(so->name, name) == 0)
      return so;
  }
  return NULL;
}

static void create_surfaces(struct swell_output *so) {
  struct swell_wl *wl = f.sw->wl;
  struct wl_region *none = wl_compositor_create_region(wl->compositor);

  f.surface = wl_compositor_create_surface(wl->compositor);
  wl_surface_set_input_region(f.surface, none);
  f.viewport = wp_viewporter_get_viewport(wl->viewporter, f.surface);
  f.layer = zwlr_layer_shell_v1_get_layer_surface(
      wl->layer_shell, f.surface, so->wl_output,
      ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, f.cfg.namespace);
  zwlr_layer_surface_v1_add_listener(f.layer, &layer_listener, NULL);
  zwlr_layer_surface_v1_set_anchor(f.layer,
                                   ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                       ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                       ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                       ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
  zwlr_layer_surface_v1_set_size(f.layer, 0, 0);
  zwlr_layer_surface_v1_set_exclusive_zone(f.layer, -1);

  f.sub_surface = wl_compositor_create_surface(wl->compositor);
  wl_surface_set_input_region(f.sub_surface, none);
  f.sub = wl_subcompositor_get_subsurface(wl->subcompositor, f.sub_surface,
                                          f.surface);
  f.sub_viewport = wp_viewporter_get_viewport(wl->viewporter, f.sub_surface);
  f.alpha = wp_alpha_modifier_v1_get_surface(wl->alpha_modifier, f.sub_surface);
  wl_region_destroy(none);
  wl_surface_commit(f.surface);
}

static bool usable(void) {
  struct swell_wl *wl = f.sw->wl;
  return wl->layer_shell && wl->subcompositor && wl->viewporter &&
         wl->alpha_modifier && wl->output_source_manager && wl->copy_manager;
}

static bool begin(int dir, const char *output, wsswitch_go_fn go, void *data) {
  /* faster than a snapshot: the held switch goes first, neither fades */
  if (f.state == SNAPPING || f.state == COVERING)
    abandon();
  if (dir == 0)
    return false;
  if (f.state == FADING) {
    /* the screen holds our overlay, not the workspace: keep the snapshot */
    go(data);
    f.switched = f.asked = now_ms();
    double now = now_fms();
    wsfade_retarget(&f.curve, dir, f.cfg.drift_px, f.cfg.fade_ms, now);
    arm_fade_timeout(now);
    return true;
  }
  if (f.overview || !usable())
    return false;
  struct swell_output *so = find_output(output);
  if (!so)
    return false;

  f.clear = buffer_create(f.sw->wl->shm, 1, 1, WL_SHM_FORMAT_ARGB8888);
  f.snap = snap_start(f.sw->wl, so->wl_output, on_snap, NULL);
  if (!f.clear || !f.snap) {
    teardown();
    return false;
  }
  memset(f.clear->data, 0, f.clear->size);
  create_surfaces(so);
  f.go = go;
  f.go_data = data;
  f.dir = dir;
  f.asked = now_ms();
  f.state = SNAPPING;
  loop_timer_arm(f.timeout, f.cfg.snapshot_timeout_ms, 0);
  return true;
}

static const struct wsswitch_sink sink = {.begin = begin};

/* Snapshot too slow, or frames stopped mid-fade. */
static void on_timeout(struct loop_timer *t, void *data) { abandon(); }

static void on_bus(const struct bus_event *ev, void *data) {
  if (strcmp(ev->name, "overview.open") == 0)
    f.overview = true;
  else if (strcmp(ev->name, "overview.close") == 0)
    f.overview = false;
}

static bool init(struct swell *sw, const void *cfg) {
  f.sw = sw;
  f.cfg = *(const struct wsfade_config *)cfg;
  if (!usable())
    warn("wsfade: the compositor lacks output capture, alpha-modifier or "
         "viewporter; switching without a fade");
  f.state = IDLE;
  f.overview = false;
  f.timeout = loop_add_timer(sw->loop, CLOCK_MONOTONIC, on_timeout, NULL);
  bus_subscribe(on_bus, NULL);
  wsswitch_set_sink(&sink);
  return true;
}

static void reconfigure(const void *cfg) {
  f.cfg = *(const struct wsfade_config *)cfg;
}

static void finish(void) {
  wsswitch_set_sink(NULL);
  abandon();
  bus_unsubscribe(on_bus, NULL);
  loop_remove_timer(f.timeout);
  f.timeout = NULL;
}

static int command(int argc, char **argv, struct reply *r) {
  if (argc != 1 || strcmp(argv[0], "status") != 0) {
    reply_printf(r, "usage: swell wsfade status\n");
    return 1;
  }
  static const char *const names[] = {"idle", "snapping", "covering", "fading"};
  reply_printf(r, "%s\n", names[f.state]);
  if (f.switched >= f.asked && f.asked)
    reply_printf(r, "last switch %lld ms after the command\n",
                 (long long)(f.switched - f.asked));
  return 0;
}

const struct swell_module wsfade_module = {
    .name = "wsfade",
    .usage = "  status    idle|snapping|covering|fading, and the last delay\n",
    .needs_wayland = true,
    .needs_sway = true,
    .off_by_default = true,
    .config_size = sizeof(struct wsfade_config),
    .defaults = &defaults,
    .fields = fields,
    .nfields = ARRAY_LEN(fields),
    .init = init,
    .reconfigure = reconfigure,
    .finish = finish,
    .command = command,
};
