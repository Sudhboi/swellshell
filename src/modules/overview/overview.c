/* overview: a GNOME-style workspace overview for sway with live window
 * thumbnails (ported from swayverview).
 *
 * Closed, it holds nothing: `open` creates a fullscreen overlay per output,
 * binds a fresh foreign toplevel list, starts the captures, subscribes to
 * sway events (which mark the model dirty), loads the model and takes the
 * keyboard. `close` (also Esc, a background or right click, or activating a
 * target) tears all of that down again, after the shown workspace has zoomed
 * back into the screen (open_anim_ms; the keyboard is let go at once).
 *
 * While open, a prepare hook reloads the model when dirty, requests due
 * capture frames, advances the hover zoom and paints dirty outputs (paced by
 * frame callbacks), and returns the next timeout. */
#include <malloc.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "alpha-modifier-v1-client-protocol.h"
#include "anim.h"
#include "buffer.h"
#include "bus.h"
#include "capture.h"
#include "fractional-scale-v1-client-protocol.h"
#include "input.h"
#include "layout.h"
#include "model.h"
#include "module.h"
#include "overview.h"
#include "render.h"
#include "sway.h"
#include "util.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "zoom.h"

/* ---- configuration ---------------------------------------------------- */

#define CFG struct overview_config

static const CFG defaults = {
    .capture = true,
    .live = true,
    .fps = 30,
    .new_workspace_slot = true,
    .font = "IosevkaTerm Nerd Font 12",

    .layout_pad = 48,
    .layout_gap = 36,
    .label_height = 30,
    .caption_height = 56,
    .tile_radius = 12,
    .tile_inset = 6,
    .window_radius = 6,
    .window_gap = 4,
    .spread_gap = 24,
    .drag_threshold = 8,
    .zoom_delay_ms = 250,
    .zoom_anim_ms = 140,
    .zoom_max_width = 0.55,
    .zoom_max_height = 0.65,
    .zoom_min_gain = 1.2,
    .zoom_margin = 16,
    .open_anim_ms = 180,
    .backdrop = true,

    /* slate neutrals like the wallpaper's glass, the snowflake's outline
     * gradient (wallpaper topo_edge_*) for the selection */
    .background = 0x0b0b10b8,
    .background_plain = 0x0f0f14f0,
    .tile = 0x1c1d26ff,
    .tile_border = 0x2e3040ff,
    .tile_visible = 0x6c6f85ff,
    .accent = 0x89b4faff,
    .accent_a = 0xf5c2e7ff,
    .accent_mid = 0xfab387ff,
    .accent_b = 0x89b4faff,
    .urgent = 0xed5454ff,
    .window = 0x2a2c3aff,
    .window_border = 0xffffff14,
    .hover = 0xffffffa6,
    .text = 0xebebf2ff,
    .text_dim = 0x8a8ca0ff,
    .focused_border = 0xffffff40,
    .drop_fill = 0x89b4fa2e,
    .new_slot_fill = 0xffffff08,
    .new_slot_drop_fill = 0x89b4fa26,
    .shadow = 0x00000059,
    .tile_shadow = 0x00000029,
    .zoom_shadow = 0x0000001f,
    .zoom_title_band = 0x0d0d12c7,
    .drag_source_alpha = 0.3,
    .drag_ghost_alpha = 0.9,
};

static const struct cfg_field fields[] = {
    CFG_BOOL(CFG, capture, "live window thumbnails (false: outlines only)"),
    CFG_BOOL(CFG, live, "keep thumbnails updating (false: capture each once)"),
    CFG_INT(CFG, fps, 1, 240, "live thumbnail update rate"),
    CFG_BOOL(CFG, new_workspace_slot, "show the \"New workspace\" slot"),
    CFG_STRING(CFG, font, "pango font description"),
    CFG_DOUBLE(CFG, layout_pad, 0, 1000, "margin around the grid (px)"),
    CFG_DOUBLE(CFG, layout_gap, 0, 1000, "gap between workspace tiles (px)"),
    CFG_DOUBLE(CFG, label_height, 0, 500, "workspace name row height (px)"),
    CFG_DOUBLE(CFG, caption_height, 0, 500,
               "space for the window title at the bottom (px)"),
    CFG_DOUBLE(CFG, tile_radius, 0, 200, "workspace tile corner radius (px)"),
    CFG_DOUBLE(CFG, tile_inset, 0, 200,
               "space between a tile's edge and its windows (px)"),
    CFG_DOUBLE(CFG, window_radius, 0, 200, "window corner radius (px)"),
    CFG_DOUBLE(CFG, window_gap, 0, 200, "gap between windows in a tile (px)"),
    CFG_DOUBLE(CFG, spread_gap, 0, 500,
               "gap between spread-out tabbed/stacked windows (tree px)"),
    CFG_DOUBLE(CFG, drag_threshold, 0, 500,
               "pointer travel that starts a drag (px)"),
    CFG_INT(CFG, zoom_delay_ms, 0, 10000,
            "hover time before the window preview grows"),
    CFG_INT(CFG, zoom_anim_ms, 0, 5000, "preview grow/shrink duration"),
    CFG_DOUBLE(CFG, zoom_max_width, 0.05, 1,
               "preview width limit, fraction of the output"),
    CFG_DOUBLE(CFG, zoom_max_height, 0.05, 1,
               "preview height limit, fraction of the output"),
    CFG_DOUBLE(CFG, zoom_min_gain, 1, 100,
               "no preview unless it grows at least this much"),
    CFG_DOUBLE(CFG, zoom_margin, 0, 1000,
               "preview distance from the output edges (px)"),
    CFG_INT(CFG, open_anim_ms, 0, 5000,
            "open/close animation: the shown workspace zooms out of and back "
            "into the screen (0 = off)"),
    CFG_BOOL(CFG, backdrop,
             "the wallpaper's topo map under the background (topo style "
             "only; the windows hide the real one)"),
    CFG_COLOR(CFG, background, "scrim over the wallpaper's map"),
    CFG_COLOR(CFG, background_plain,
              "scrim without the map (classic wallpaper, backdrop = false); "
              "the windows show through what it leaves"),
    CFG_COLOR(CFG, tile, "workspace tile"),
    CFG_COLOR(CFG, tile_border, "workspace tile border"),
    CFG_COLOR(CFG, tile_visible, "border of a visible (unfocused) workspace"),
    CFG_COLOR(CFG, accent, "focused workspace name, drop tints"),
    CFG_COLOR(CFG, accent_a, "selection/drop marker gradient, top left"),
    CFG_COLOR(CFG, accent_mid, "selection/drop marker gradient, middle"),
    CFG_COLOR(CFG, accent_b, "selection/drop marker gradient, bottom right"),
    CFG_COLOR(CFG, urgent, "urgent workspace name / window border"),
    CFG_COLOR(CFG, window, "window placeholder behind thumbnails"),
    CFG_COLOR(CFG, window_border, "window border"),
    CFG_COLOR(CFG, hover, "hovered window/workspace border"),
    CFG_COLOR(CFG, text, "text"),
    CFG_COLOR(CFG, text_dim, "dim text (labels without thumbnails, +)"),
    CFG_COLOR(CFG, focused_border, "border of the focused workspace"),
    CFG_COLOR(CFG, drop_fill, "tint of a workspace a window is dragged over"),
    CFG_COLOR(CFG, new_slot_fill, "\"New workspace\" slot fill"),
    CFG_COLOR(CFG, new_slot_drop_fill,
              "\"New workspace\" slot fill while a window is dragged over it"),
    CFG_COLOR(CFG, shadow, "shadow under dragged windows/workspaces"),
    CFG_COLOR(CFG, tile_shadow, "each of the 3 shadow rings under a tile"),
    CFG_COLOR(CFG, zoom_shadow, "each of the 3 shadow rings of the preview"),
    CFG_COLOR(CFG, zoom_title_band, "title band at the bottom of the preview"),
    CFG_DOUBLE(CFG, drag_source_alpha, 0, 1,
               "opacity of a window/workspace while it is dragged away"),
    CFG_DOUBLE(CFG, drag_ghost_alpha, 0, 1,
               "opacity of the dragged window/workspace under the pointer"),
};

/* ---- state ------------------------------------------------------------ */

static struct ov ov;
static struct swell *sw_;
static CFG cfg_current; /* the latest config; ov.cfg is copied at open */

static void ov_close(void);

void ov_output_redraw(struct output *out) { out->dirty = true; }

void ov_redraw(struct ov *o) {
  struct output *out;
  wl_list_for_each(out, &o->outputs, link) ov_output_redraw(out);
}

void ov_request_close(struct ov *o) { o->close_pending = true; }

/* ---- open/close animation ---------------------------------------------- */

/* How long the zoom out waits for the shown windows' first thumbnails, so it
 * starts from what is on screen instead of grey placeholders. */
#define OPEN_WAIT_MS 150

static double anim_linear(struct ov *o, int64_t now) {
  if (o->anim.dir == 0 || o->anim.start == 0 || o->cfg.open_anim_ms <= 0)
    return o->anim.from;
  double p = o->anim.from + o->anim.dir * (double)(now - o->anim.start) /
                                o->cfg.open_anim_ms;
  return p < 0 ? 0 : p > 1 ? 1 : p;
}

/* Eased both ways: quick off the mark, gentle at the end (opening ends
 * open, closing ends on the screen). */
static double anim_ease(double p, int dir) {
  return dir < 0 ? 1 - ease_out_cubic(1 - p) : ease_out_cubic(p);
}

/* p with anim_ease(p, dir) == v. */
static double anim_uneased(double v, int dir) {
  if (dir < 0)
    return cbrt(v);
  return 1 - cbrt(1 - v);
}

double ov_anim_value(struct ov *o, int64_t now) {
  return anim_ease(anim_linear(o, now), o->anim.dir);
}

struct workspace *ov_anim_hero(struct ov *o, struct output *out) {
  struct workspace *visible = NULL;
  for (int i = 0; i < out->nworkspaces; ++i) {
    struct workspace *ws = out->workspaces[i];
    if (o->anim.dir < 0 && o->anim.into_set &&
        ws->placeholder == o->anim.into_placeholder &&
        (ws->placeholder || ws->id == o->anim.into_id))
      return ws;
    if (ws->visible && !ws->placeholder && !visible)
      visible = ws;
  }
  return visible;
}

/* Head in direction dir from wherever the animation is now. */
static void anim_turn(struct ov *o, int dir, int64_t now) {
  o->anim.from = anim_uneased(ov_anim_value(o, now), dir); /* no jump */
  o->anim.start = now;
  o->anim.dir = dir;
}

/* Every shown output has its background drawn (the first draw is slow; it
 * goes in a frame before the zoom, not into it). */
static bool backgrounds_ready(struct ov *o, int64_t now) {
  if (now - o->anim.opened >= 1000)
    return true; /* an output that never configured */
  struct output *out;
  wl_list_for_each(out, &o->outputs, link) {
    if (out->surface && !out->bg_ready)
      return false;
  }
  return true;
}

/* Advance; false once a close has finished. */
static bool anim_tick(struct ov *o, int64_t now) {
  if (o->anim.dir > 0 && o->anim.start == 0 &&
      (ov_capture_settled(o) || now - o->anim.opened >= OPEN_WAIT_MS) &&
      backgrounds_ready(o, now))
    o->anim.start = now;
  double p = anim_linear(o, now);
  if (o->anim.dir < 0 && p <= 0)
    return false;
  if (o->anim.dir > 0 && p >= 1) {
    o->anim.from = 1;
    o->anim.dir = 0;
    ov_redraw(o); /* the settled look (sharp thumbnails, captions) */
  } else if (o->anim.dir != 0) {
    ov_redraw(o);
  }
  return true;
}

static int anim_timeout(struct ov *o) { return o->anim.dir != 0 ? 8 : -1; }

static void set_keyboard(struct ov *o, bool take) {
  struct output *out;
  wl_list_for_each(out, &o->outputs, link) {
    if (!out->layer)
      continue;
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        out->layer, take ? ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE
                         : ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    wl_surface_commit(out->surface);
  }
}

/* ---- per-output overlays ---------------------------------------------- */

static void output_destroy_background(struct output *out) {
  if (out->bg_alpha)
    wp_alpha_modifier_surface_v1_destroy(out->bg_alpha);
  if (out->bg_viewport)
    wp_viewport_destroy(out->bg_viewport);
  if (out->bg_sub)
    wl_subsurface_destroy(out->bg_sub);
  if (out->bg_surface)
    wl_surface_destroy(out->bg_surface);
  buffer_destroy(out->bg_buffer);
  if (out->background)
    cairo_surface_destroy(out->background);
  out->bg_alpha = NULL;
  out->bg_viewport = NULL;
  out->bg_sub = NULL;
  out->bg_surface = NULL;
  out->bg_buffer = NULL;
  out->background = NULL;
  out->bg_ready = false;
}

static void output_destroy_surface(struct output *out) {
  output_destroy_background(out);
  if (out->surface)
    wl_clear_input(out->ov->wl, out->surface);
  if (out->frame_cb)
    wl_callback_destroy(out->frame_cb);
  if (out->fractional)
    wp_fractional_scale_v1_destroy(out->fractional);
  if (out->viewport)
    wp_viewport_destroy(out->viewport);
  if (out->layer)
    zwlr_layer_surface_v1_destroy(out->layer);
  if (out->surface)
    wl_surface_destroy(out->surface);
  for (int i = 0; i < 2; ++i) {
    buffer_destroy(out->buffers[i]);
    buffer_destroy(out->half_buffers[i]);
    out->buffers[i] = out->half_buffers[i] = NULL;
  }
  out->frame_cb = NULL;
  out->fractional = NULL;
  out->viewport = NULL;
  out->layer = NULL;
  out->surface = NULL;
  out->configured = false;
}

static void output_destroy(struct output *out) {
  struct ov *o = out->ov;
  if (o->ptr_output == out)
    o->ptr_output = NULL;
  if (o->drag.output == out)
    o->drag.output = NULL;
  output_destroy_surface(out);
  for (int i = 0; i < out->nworkspaces; ++i)
    out->workspaces[i]->output = NULL;
  free(out->workspaces);
  wl_list_remove(&out->link);
  free(out->name);
  free(out);
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer,
                            uint32_t serial, uint32_t w, uint32_t h) {
  struct output *out = data;
  zwlr_layer_surface_v1_ack_configure(layer, serial);
  if (out->width != (int)w || out->height != (int)h) {
    out->width = w;
    out->height = h;
    out->ov->model_dirty = true; /* relayout */
  }
  out->configured = true;
  ov_output_redraw(out);
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer) {
  output_destroy_surface(data);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

static void preferred_scale(void *data, struct wp_fractional_scale_v1 *f,
                            uint32_t scale120) {
  struct output *out = data;
  out->scale120 = scale120;
  ov_output_redraw(out);
}

static const struct wp_fractional_scale_v1_listener fractional_listener = {
    .preferred_scale = preferred_scale,
};

static void output_create_surface(struct output *out) {
  struct swell_wl *wl = out->ov->wl;
  if (out->surface || !wl->layer_shell)
    return;
  out->surface = wl_compositor_create_surface(wl->compositor);
  if (wl->viewporter)
    out->viewport = wp_viewporter_get_viewport(wl->viewporter, out->surface);
  if (wl->fractional && out->viewport) {
    out->fractional = wp_fractional_scale_manager_v1_get_fractional_scale(
        wl->fractional, out->surface);
    wp_fractional_scale_v1_add_listener(out->fractional, &fractional_listener,
                                        out);
  }
  out->layer = zwlr_layer_shell_v1_get_layer_surface(
      wl->layer_shell, out->surface, out->so->wl_output,
      ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "swell-overview");
  zwlr_layer_surface_v1_add_listener(out->layer, &layer_listener, out);
  zwlr_layer_surface_v1_set_anchor(out->layer,
                                   ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                       ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                       ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                       ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
  zwlr_layer_surface_v1_set_size(out->layer, 0, 0);
  zwlr_layer_surface_v1_set_exclusive_zone(out->layer, -1);
  zwlr_layer_surface_v1_set_keyboard_interactivity(
      out->layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
  wl_set_input(wl, out->surface, &ov_input_handler, out);

  /* the background below, faded by the compositor */
  if (wl->subcompositor && wl->alpha_modifier && wl->viewporter) {
    out->bg_surface = wl_compositor_create_surface(wl->compositor);
    out->bg_sub = wl_subcompositor_get_subsurface(
        wl->subcompositor, out->bg_surface, out->surface);
    wl_subsurface_place_below(out->bg_sub, out->surface);
    out->bg_viewport =
        wp_viewporter_get_viewport(wl->viewporter, out->bg_surface);
    out->bg_alpha =
        wp_alpha_modifier_v1_get_surface(wl->alpha_modifier, out->bg_surface);
    struct wl_region *none = wl_compositor_create_region(wl->compositor);
    wl_surface_set_input_region(out->bg_surface, none);
    wl_region_destroy(none);
  }
  wl_surface_commit(out->surface);
}

static struct output *output_add(struct ov *o, struct swell_output *so) {
  struct output *out = xcalloc(1, sizeof(*out));
  out->ov = o;
  out->so = so;
  out->name = xstrdup(so->name);
  /* newest first, as swayverview's registry did: navigation walks the
   * list in reverse, i.e. in announcement order */
  wl_list_insert(&o->outputs, &out->link);
  return out;
}

static void on_output(void *data, struct swell_output *so, bool added) {
  struct ov *o = data;
  if (added) {
    struct output *out = output_add(o, so);
    if (o->open)
      output_create_surface(out); /* hotplugged while open */
  } else {
    struct output *out, *tmp;
    wl_list_for_each_safe(out, tmp, &o->outputs, link) {
      if (out->so == so)
        output_destroy(out);
    }
  }
  o->model_dirty = true;
}

/* ---- painting ----------------------------------------------------------- */

static double output_scale(struct output *out) {
  if (out->scale120)
    return out->scale120 / 120.0;
  return out->so->scale > 0 ? out->so->scale : 1;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
  struct output *out = data;
  wl_callback_destroy(cb);
  out->frame_cb = NULL;
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

/* A free buffer of pair (kept apart so switching sizes for the zoom
 * doesn't allocate). */
static struct buffer *output_get_buffer(struct output *out,
                                        struct buffer *pair[2], int w, int h) {
  for (int i = 0; i < 2; ++i) {
    struct buffer *b = pair[i];
    if (b && b->busy)
      continue;
    if (b && (b->width != w || b->height != h)) {
      buffer_destroy(b);
      b = pair[i] = NULL;
    }
    if (!b)
      b = pair[i] =
          buffer_create(out->ov->wl->shm, w, h, WL_SHM_FORMAT_ARGB8888);
    return b;
  }
  return NULL;
}

/* The background subsurface: drawn when the output's size changes, at half
 * the buffer resolution (the map is drawn at that anyway; the compositor
 * scales it up), faded with the open/close animation; committed before the
 * parent, which applies it. */
static void output_render_background(struct output *out, int pw, int ph,
                                     double scale) {
  struct ov *o = out->ov;
  bool commit = false;
  int bw = (pw + 1) / 2, bh = (ph + 1) / 2;
  if (!out->bg_buffer || out->bg_buffer->width != bw ||
      out->bg_buffer->height != bh) {
    buffer_destroy(out->bg_buffer);
    out->bg_buffer = buffer_create(o->wl->shm, bw, bh, WL_SHM_FORMAT_ARGB8888);
    if (!out->bg_buffer)
      return;
    cairo_t *cr = cairo_create(out->bg_buffer->cairo);
    cairo_scale(cr, (double)bw / out->width, (double)bh / out->height);
    ov_render_background(o, out, cr, scale);
    cairo_destroy(cr);
    cairo_surface_flush(out->bg_buffer->cairo);
    wl_surface_set_buffer_scale(out->bg_surface, 1);
    wp_viewport_set_destination(out->bg_viewport, out->width, out->height);
    wl_surface_attach(out->bg_surface, out->bg_buffer->wl_buffer, 0, 0);
    wl_surface_damage_buffer(out->bg_surface, 0, 0, bw, bh);
    out->bg_ready = true;
    commit = true;
  }
  uint32_t m = (uint32_t)lround(ov_anim_value(o, now_ms()) * UINT32_MAX);
  if (commit || m != out->bg_multiplier) {
    wp_alpha_modifier_surface_v1_set_multiplier(out->bg_alpha, m);
    out->bg_multiplier = m;
    commit = true;
  }
  if (commit)
    wl_surface_commit(out->bg_surface);
}

static void output_render(struct output *out) {
  double scale = output_scale(out);
  bool fractional = out->scale120 && out->viewport;
  if (!fractional)
    scale = out->so->scale > 0 ? out->so->scale : 1;
  int pw = (int)lround(out->width * scale);
  int ph = (int)lround(out->height * scale);

  /* Zooming in or out: half the pixels each way, scaled up by the
   * compositor (a full-size frame took longer than a 120 Hz frame; the
   * softness doesn't show in motion). */
  bool zooming = out->ov->anim.dir != 0 && out->viewport;
  double rscale = zooming ? scale / 2 : scale;
  int rw = zooming ? (int)lround(out->width * rscale) : pw;
  int rh = zooming ? (int)lround(out->height * rscale) : ph;

  struct buffer *buf = output_get_buffer(
      out, zooming ? out->half_buffers : out->buffers, rw, rh);
  if (!buf)
    return; /* both busy; retry on the next frame */
  if (out->bg_surface)
    output_render_background(out, pw, ph, scale);

  if (zooming && !out->warm) {
    ov_render_warm(out->ov, out, rscale);
    out->warm = true;
  }
  cairo_t *cr = cairo_create(buf->cairo);
  cairo_scale(cr, rscale, rscale);
  ov_render_output(out->ov, out, cr, rscale);
  cairo_destroy(cr);
  cairo_surface_flush(buf->cairo);

  if (fractional || zooming) {
    wl_surface_set_buffer_scale(out->surface, 1);
    wp_viewport_set_destination(out->viewport, out->width, out->height);
  } else {
    if (out->viewport)
      wp_viewport_set_destination(out->viewport, -1, -1);
    wl_surface_set_buffer_scale(out->surface, (int)scale);
  }
  wl_surface_attach(out->surface, buf->wl_buffer, 0, 0);
  wl_surface_damage_buffer(out->surface, 0, 0, rw, rh);
  out->frame_cb = wl_surface_frame(out->surface);
  wl_callback_add_listener(out->frame_cb, &frame_listener, out);
  wl_surface_commit(out->surface);
  buf->busy = true;
  out->dirty = false;
}

static void render_dirty(struct ov *o) {
  struct output *out;
  wl_list_for_each(out, &o->outputs, link) {
    if (out->dirty && out->surface && out->configured && !out->frame_cb &&
        out->width > 0)
      output_render(out);
  }
}

/* ---- model reload --------------------------------------------------------- */

/* A target remembered across a reload: windows by con_id, workspaces by
 * their con_id (stable when the workspaces module renames them), then by
 * name, and the placeholder by output. */
struct target_ref {
  long con_id;
  long ws_id;
  char *ws_name;
  char *ws_output;
  bool placeholder;
};

static struct target_ref ref_of(struct target t) {
  struct target_ref r = {.con_id = -1, .ws_id = -1};
  if (t.win)
    r.con_id = t.win->con_id;
  if (t.ws) {
    r.ws_id = t.ws->id;
    r.ws_name = xstrdup(t.ws->name);
    r.ws_output = xstrdup(t.ws->output_name);
    r.placeholder = t.ws->placeholder;
  }
  return r;
}

static void ref_free(struct target_ref *r) {
  free(r->ws_name);
  free(r->ws_output);
  r->ws_name = r->ws_output = NULL;
}

static struct target resolve(struct ov *o, struct target_ref *r) {
  struct target t = {0};
  struct model *m = &o->model;
  if (r->con_id >= 0) {
    struct window *w = ov_model_find_window(m, r->con_id);
    if (w && w->ws->output)
      t = (struct target){w, w->ws};
  } else if (r->ws_name) {
    struct workspace *ws = r->placeholder ? NULL
                                          : ov_model_find_workspace(m, r->ws_id);
    if (ws && ws->output)
      t.ws = ws;
    for (int i = 0; !t.ws && i < m->nworkspaces; ++i) {
      ws = m->workspaces[i];
      if (ws->placeholder == r->placeholder && ws->output &&
          (r->placeholder ? strcmp(ws->output_name, r->ws_output)
                          : strcmp(ws->name, r->ws_name)) == 0)
        t.ws = ws;
    }
  }
  ref_free(r);
  return t;
}

static bool reload(struct ov *o, bool first) {
  struct target_ref sel = ref_of(o->selected);
  struct target_ref press = ref_of(o->drag.press);
  long hover_con = o->hover.win ? o->hover.win->con_id : -1;
  long zoom_con = o->zoom.win ? o->zoom.win->con_id : -1;

  struct model fresh;
  if (!ov_model_load(&fresh, o->cfg.new_workspace_slot, o->cfg.spread_gap)) {
    ref_free(&sel);
    ref_free(&press);
    o->model_dirty = false; /* the next sway event retries */
    return false;           /* keep the old model */
  }
  /* pointers into the old model die here */
  o->hover = o->selected = o->drag.press = (struct target){0};
  o->zoom.win = NULL;
  ov_model_free(&o->model);
  o->model = fresh;
  o->model_dirty = false;

  /* outputs only know their size once configured; in dump mode fake it */
  if (o->dump) {
    struct output *out;
    wl_list_for_each(out, &o->outputs, link) {
      for (int i = 0; i < o->model.noutputs; ++i) {
        if (out->name && strcmp(out->name, o->model.outputs[i].name) == 0) {
          out->width = o->model.outputs[i].rect.w;
          out->height = o->model.outputs[i].rect.h;
        }
      }
    }
  }

  ov_capture_bind_model(o);
  ov_layout_all(o);

  if (first) {
    ref_free(&sel);
    o->selected = ov_layout_initial(o);
  } else {
    o->selected = resolve(o, &sel);
  }
  if (!o->selected.ws)
    o->selected = ov_layout_initial(o);

  bool dragging = o->drag.pressed;
  o->drag.press = resolve(o, &press);
  if (dragging && !o->drag.press.ws)
    o->drag.pressed = o->drag.active = false;
  o->zoom.win = zoom_con >= 0 ? ov_model_find_window(&o->model, zoom_con)
                              : NULL;
  if (o->zoom.win && !o->zoom.win->ws->output)
    o->zoom.win = NULL;
  ov_zoom_relayout(o);
  ov_input_update_hover(o, hover_con);
  ov_redraw(o);
  return true;
}

/* ---- open / close --------------------------------------------------------- */

static void on_sway_event(const I3ipc_event *ev, void *data) {
  struct ov *o = data;
  o->model_dirty = true;
}

/* Start closing (the zoom back in), or close now when there is none. */
static bool begin_close(struct ov *o, int64_t now) {
  if (o->anim.dir < 0)
    return true;
  if (o->cfg.open_anim_ms <= 0 || anim_linear(o, now) <= 0)
    return false;
  anim_turn(o, -1, now);
  o->drag.pressed = o->drag.active = false;
  set_keyboard(o, false); /* keys go to the windows again right away */
  o->anim.close_emitted = true;
  bus_emit("overview.close", NULL);
  ov_redraw(o);
  return true;
}

static int min_timeout(int a, int b) {
  return b >= 0 && (a < 0 || b < a) ? b : a;
}

static int prepare(void *data) {
  struct ov *o = data;
  if (o->close_pending && !begin_close(o, now_ms())) {
    ov_close();
    return -1;
  }
  if (!o->open)
    return -1;
  /* events another module's roundtrip queued would otherwise wait for the
   * core's flush hook, which runs after us */
  wl_display_dispatch_pending(o->wl->display);
  if (o->close_pending && !begin_close(o, now_ms())) {
    ov_close();
    return -1;
  }
  if (o->model_dirty)
    reload(o, false);
  int64_t now = now_ms();
  if (!anim_tick(o, now)) {
    ov_close();
    return -1;
  }
  /* no fresh thumbnails while zooming: each one is scaled again for the
   * zoom, the frames that took longest */
  bool zooming = o->anim.dir != 0 && o->anim.start != 0;
  if (!zooming)
    ov_capture_tick(o, now);
  ov_zoom_tick(o, now);
  render_dirty(o);

  int timeout = zooming ? -1 : ov_capture_timeout(o, now);
  timeout = min_timeout(timeout, ov_zoom_timeout(o, now));
  return min_timeout(timeout, anim_timeout(o));
}

static void reset_state(struct ov *o) {
  struct swell_wl *wl = o->wl;
  struct swell *sw = o->sw;
  memset(o, 0, sizeof(*o));
  o->wl = wl;
  o->sw = sw;
  o->model.focused_con = -1;
  o->repeat_sym = XKB_KEY_NoSymbol;
  wl_list_init(&o->outputs);
  wl_list_init(&o->toplevels);
  wl_list_init(&o->captures);
}

/* Start tracking toplevels and outputs and load the model (no surfaces). */
static bool begin(struct ov *o) {
  ov_capture_init(o);
  wl_display_roundtrip(o->wl->display); /* initial foreign toplevels */
  struct swell_output *so;
  wl_list_for_each(so, &o->wl->outputs, link) {
    if (so->done)
      output_add(o, so);
  }
  return reload(o, true);
}

/* Undo begin() and everything open added. */
static void teardown(struct ov *o) {
  struct output *out, *tmp;
  wl_list_for_each_safe(out, tmp, &o->outputs, link) output_destroy(out);
  ov_capture_finish(o);
  ov_model_free(&o->model);
  reset_state(o);
}

static int ov_open(struct reply *r) {
  if (ov.open && ov.anim.dir < 0) {
    /* closing: turn round */
    int64_t now = now_ms();
    anim_turn(&ov, 1, now);
    ov.close_pending = false;
    ov.anim.into_set = false;
    ov.anim.close_emitted = false;
    set_keyboard(&ov, true);
    ov_redraw(&ov);
    bus_emit("overview.open", NULL);
    return 0;
  }
  if (ov.open)
    return 0;
  ov.sw = sw_;
  ov.wl = sw_->wl;
  reset_state(&ov);
  ov.cfg = cfg_current;
  if (!ov.wl->layer_shell) {
    if (r)
      reply_printf(r, "the compositor lacks wlr-layer-shell\n");
    return 1;
  }
  if (!begin(&ov)) {
    teardown(&ov);
    if (r)
      reply_printf(r, "cannot read the sway tree\n");
    return 1;
  }
  ov.open = true;
  ov.anim.opened = now_ms();
  ov.anim.dir = ov.cfg.open_anim_ms > 0 ? 1 : 0;
  ov.anim.from = ov.anim.dir ? 0 : 1;
  wl_add_output_listener(ov.wl, on_output, &ov);
  sway_subscribe(on_sway_event, &ov);
  loop_add_prepare(sw_->loop, 50, prepare, &ov);
  ov.repeat = loop_add_timer(sw_->loop, CLOCK_MONOTONIC, ov_input_repeat, &ov);
  struct output *out;
  wl_list_for_each(out, &ov.outputs, link) output_create_surface(out);
  bus_emit("overview.open", NULL);
  return 0;
}

static void ov_close(void) {
  if (!ov.open)
    return;
  bool emitted = ov.anim.close_emitted;
  wl_remove_output_listener(ov.wl, on_output, &ov);
  sway_unsubscribe(on_sway_event, &ov);
  loop_remove_prepare(sw_->loop, prepare, &ov);
  loop_remove_timer(ov.repeat);
  ov.repeat = NULL;
  teardown(&ov); /* clears ov.open */
  wl_display_flush(ov.wl->display);
  malloc_trim(0); /* hand the frame/thumbnail memory back while closed */
  if (!emitted)
    bus_emit("overview.close", NULL);
}

/* ---- dump (runs in the client) ---------------------------------------------- */

static int dump(struct ov *o) {
  /* give thumbnails a moment to arrive */
  struct wl_display *display = o->wl->display;
  int64_t deadline = now_ms() + 1500;
  while (!ov_capture_settled(o) && now_ms() < deadline) {
    ov_capture_tick(o, now_ms());
    if (o->model_dirty)
      reload(o, false);
    wl_display_flush(display);
    struct pollfd pfd = {.fd = wl_display_get_fd(display), .events = POLLIN};
    if (poll(&pfd, 1, 50) > 0) {
      if (wl_display_dispatch(display) < 0)
        break;
    } else {
      wl_display_dispatch_pending(display);
    }
  }

  struct output *out, *target = NULL;
  wl_list_for_each(out, &o->outputs, link) {
    if (!o->dump_output ||
        (out->name && strcmp(out->name, o->dump_output) == 0))
      target = out;
  }
  if (!target || target->width <= 0) {
    warn("overview: no output to dump");
    return 1;
  }
  cairo_surface_t *surface = cairo_image_surface_create(
      CAIRO_FORMAT_ARGB32, target->width, target->height);
  cairo_t *cr = cairo_create(surface);
  ov_render_output(o, target, cr, 1.0);
  cairo_destroy(cr);
  cairo_status_t st = cairo_surface_write_to_png(surface, o->dump_path);
  cairo_surface_destroy(surface);
  if (st != CAIRO_STATUS_SUCCESS) {
    warn("overview: cannot write %s: %s", o->dump_path,
         cairo_status_to_string(st));
    return 1;
  }
  return 0;
}

static int run_local(struct swell *sw, const void *cfg, bool have_daemon,
                     int argc, char **argv) {
  if (argc < 1 || strcmp(argv[0], "dump") != 0)
    return -1;
  if (argc < 2 || argc > 3) {
    fprintf(stderr, "usage: swell overview dump FILE.png [OUTPUT]\n");
    return 2;
  }
  if (!sw->wl)
    sw->wl = wl_create(sw->loop);
  if (!sw->wl)
    return 1;
  if (sw->wm == WM_NONE)
    sw->wm = wm_init(NULL);
  if (sw->wm != WM_SWAY) {
    if (sw->wm != WM_NONE)
      fprintf(stderr, "swell: the overview needs sway\n");
    return 1;
  }

  ov.sw = sw;
  ov.wl = sw->wl;
  reset_state(&ov);
  memcpy(&ov.cfg, cfg, sizeof(ov.cfg));
  ov.dump = true;
  ov.anim.from = 1; /* fully open */
  ov.dump_path = argv[1];
  ov.dump_output = argc > 2 ? argv[2] : NULL;
  int ret = 1;
  if (begin(&ov))
    ret = dump(&ov);
  else
    warn("overview: cannot read the sway tree");
  teardown(&ov);
  wl_display_roundtrip(sw->wl->display);
  return ret;
}

/* ---- module ----------------------------------------------------------------- */

static bool init(struct swell *sw, const void *cfg) {
  sw_ = sw;
  memcpy(&cfg_current, cfg, sizeof(cfg_current));
  ov.sw = sw;
  ov.wl = sw->wl;
  reset_state(&ov);
  return true;
}

static void reconfigure(const void *cfg) {
  memcpy(&cfg_current, cfg, sizeof(cfg_current)); /* used at the next open */
}

static void finish(void) {
  ov_close();
  sw_ = NULL;
}

static int command(int argc, char **argv, struct reply *r) {
  const char *cmd = argc > 0 ? argv[0] : "toggle";
  if (argc > 1) {
    reply_printf(r, "usage: swell overview [open|close|toggle]\n");
    return 1;
  }
  if (strcmp(cmd, "open") == 0)
    return ov_open(r);
  bool shown = ov.open && ov.anim.dir >= 0; /* closing counts as closed */
  if (strcmp(cmd, "close") == 0) {
    if (ov.open)
      ov_request_close(&ov); /* the next prepare starts the zoom back in */
    return 0;
  }
  if (strcmp(cmd, "toggle") == 0) {
    if (shown) {
      ov_request_close(&ov);
      return 0;
    }
    return ov_open(r);
  }
  if (strcmp(cmd, "status") == 0) {
    reply_printf(r, "%s\n", shown ? "open" : "closed");
    return 0;
  }
  reply_printf(r, "unknown overview command '%s' (open, close, toggle, "
                  "status, dump FILE.png [OUTPUT])\n",
               cmd);
  return 1;
}

const struct swell_module overview_module = {
    .name = "overview",
    .usage =
        "  [toggle]  show the workspace overview, or close it if shown\n"
        "  open      show it (no-op if shown)\n"
        "  close     close it (no-op if closed)\n"
        "  status    print open or closed\n"
        "  dump FILE.png [OUTPUT]\n"
        "            render one output to a PNG (runs locally, for testing)\n"
        "  Mouse: click a window to focus it, a workspace to switch; drag a\n"
        "  window onto another workspace (or New workspace) to move it; drag\n"
        "  a workspace by its name to reorder; middle-click kills a window;\n"
        "  right-click or a background click closes.\n"
        "  Keys: arrows/hjkl select, Tab/Shift+Tab cycle, Enter/Space focus,\n"
        "  Shift+arrows (H/L) move the window to the previous/next workspace,\n"
        "  Ctrl+arrows (Ctrl+h/l) move the workspace, 1-9/0 jump, Esc/q "
        "close.\n",
    .needs_wayland = true,
    .needs_sway = true,
    .config_size = sizeof(CFG),
    .defaults = &defaults,
    .fields = fields,
    .nfields = ARRAY_LEN(fields),
    .init = init,
    .reconfigure = reconfigure,
    .finish = finish,
    .command = command,
    .run_local = run_local,
};
