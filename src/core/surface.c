#include "surface.h"

#include <stdlib.h>
#include <string.h>

#include "buffer.h"
#include "util.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

struct layer_surface {
  struct swell_wl *wl;
  layer_draw_fn draw;
  void *data;
  struct layer_opts opts;

  struct wl_surface *surface;
  struct zwlr_layer_surface_v1 *layer;
  struct wl_callback *frame_cb;
  struct buffer *bufs[2];
  bool configured, dirty;
  /* From wl_surface.enter. Like the old nixosd and popup, we draw at the
   * integer output scale (2 on a 1.5x output) and let the compositor
   * downscale; native fractional rendering hints text differently. */
  int int_scale;
};

static void draw_frame(struct layer_surface *s);

int layer_surface_scale(struct layer_surface *s) {
  return s->int_scale > 0 ? s->int_scale : 1;
}

static struct buffer *get_buffer(struct layer_surface *s, int w, int h) {
  for (int i = 0; i < 2; ++i) {
    struct buffer *b = s->bufs[i];
    if (b && b->busy)
      continue;
    if (b && (b->width != w || b->height != h)) {
      buffer_destroy(b);
      b = s->bufs[i] = NULL;
    }
    if (!b)
      b = s->bufs[i] = buffer_create(s->wl->shm, w, h, WL_SHM_FORMAT_ARGB8888);
    return b;
  }
  return NULL;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
  struct layer_surface *s = data;
  wl_callback_destroy(cb);
  s->frame_cb = NULL;
  if (s->dirty)
    draw_frame(s);
}

static const struct wl_callback_listener frame_listener = {.done = frame_done};

static void draw_frame(struct layer_surface *s) {
  if (!s->surface || !s->configured)
    return;
  int scale = layer_surface_scale(s);
  int pw = s->opts.width * scale;
  int ph = s->opts.height * scale;
  struct buffer *b = get_buffer(s, pw, ph);
  if (!b) {
    /* Both buffers are busy; a release arrives with the next frame. */
    s->dirty = true;
    if (!s->frame_cb) {
      s->frame_cb = wl_surface_frame(s->surface);
      wl_callback_add_listener(s->frame_cb, &frame_listener, s);
      wl_surface_commit(s->surface);
    }
    return;
  }

  cairo_t *cr = cairo_create(b->cairo);
  cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  cairo_scale(cr, scale, scale);
  bool more = s->draw(cr, s->data);
  cairo_destroy(cr);
  cairo_surface_flush(b->cairo);

  s->dirty = more;
  if (more && !s->frame_cb) {
    s->frame_cb = wl_surface_frame(s->surface);
    wl_callback_add_listener(s->frame_cb, &frame_listener, s);
  }
  wl_surface_set_buffer_scale(s->surface, scale);
  wl_surface_attach(s->surface, b->wl_buffer, 0, 0);
  wl_surface_damage_buffer(s->surface, 0, 0, pw, ph);
  wl_surface_commit(s->surface);
  b->busy = true;
}

void layer_surface_redraw(struct layer_surface *s) {
  s->dirty = true;
  if (!s->frame_cb)
    draw_frame(s);
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer,
                            uint32_t serial, uint32_t w, uint32_t h) {
  struct layer_surface *s = data;
  zwlr_layer_surface_v1_ack_configure(layer, serial);
  s->configured = true;
  if (!s->frame_cb)
    draw_frame(s);
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer) {
  layer_surface_hide(data);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

static void surface_enter(void *data, struct wl_surface *surface,
                          struct wl_output *o) {
  struct layer_surface *s = data;
  struct swell_output *out = wl_output_find(s->wl, o);
  if (out && out->scale != s->int_scale) {
    s->int_scale = out->scale;
    layer_surface_redraw(s);
  }
}

static void surface_leave(void *data, struct wl_surface *surface,
                          struct wl_output *o) {}

static const struct wl_surface_listener surface_listener = {
    .enter = surface_enter,
    .leave = surface_leave,
};

struct layer_surface *layer_surface_create(struct swell_wl *wl,
                                           layer_draw_fn draw, void *data) {
  struct layer_surface *s = xcalloc(1, sizeof(*s));
  s->wl = wl;
  s->draw = draw;
  s->data = data;
  s->int_scale = 1;
  return s;
}

void layer_surface_destroy(struct layer_surface *s) {
  if (!s)
    return;
  layer_surface_hide(s);
  free(s);
}

void layer_surface_show(struct layer_surface *s, const struct layer_opts *opts) {
  struct swell_wl *wl = s->wl;
  if (s->surface || !wl->layer_shell)
    return;
  s->opts = *opts;
  s->surface = wl_compositor_create_surface(wl->compositor);
  wl_surface_add_listener(s->surface, &surface_listener, s);
  if (opts->click_through) {
    struct wl_region *empty = wl_compositor_create_region(wl->compositor);
    wl_surface_set_input_region(s->surface, empty);
    wl_region_destroy(empty);
  }
  s->layer = zwlr_layer_shell_v1_get_layer_surface(
      wl->layer_shell, s->surface, NULL, opts->layer, opts->ns);
  zwlr_layer_surface_v1_add_listener(s->layer, &layer_listener, s);
  zwlr_layer_surface_v1_set_size(s->layer, opts->width, opts->height);
  zwlr_layer_surface_v1_set_anchor(s->layer, opts->anchor);
  zwlr_layer_surface_v1_set_exclusive_zone(s->layer, opts->exclusive_zone);
  zwlr_layer_surface_v1_set_margin(s->layer, opts->margin_top,
                                   opts->margin_right, opts->margin_bottom,
                                   opts->margin_left);
  zwlr_layer_surface_v1_set_keyboard_interactivity(
      s->layer, opts->keyboard
                    ? ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE
                    : ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
  wl_surface_commit(s->surface);
  s->configured = false;
}

void layer_surface_hide(struct layer_surface *s) {
  if (s->frame_cb) {
    wl_callback_destroy(s->frame_cb);
    s->frame_cb = NULL;
  }
  if (s->layer) {
    zwlr_layer_surface_v1_destroy(s->layer);
    s->layer = NULL;
  }
  if (s->surface) {
    wl_clear_input(s->wl, s->surface);
    wl_surface_destroy(s->surface);
    s->surface = NULL;
  }
  for (int i = 0; i < 2; ++i) {
    buffer_destroy(s->bufs[i]);
    s->bufs[i] = NULL;
  }
  s->configured = false;
  s->dirty = false;
}

bool layer_surface_shown(struct layer_surface *s) { return s->surface != NULL; }
