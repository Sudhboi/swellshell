/* One background layer surface per output, drawn natively at the
 * fractional scale (wp_fractional_scale_v1 + viewporter), double-buffered,
 * repainting and damaging only the bar and the logo after the first frame.
 * Frame callbacks pace the animation and are only requested while the
 * module animates, so a frozen wallpaper costs nothing. While the smooth
 * edge visualiser shows, the GPU draws it (gl.c) into two transparent
 * subsurfaces over the strips; they exist only while bars show. */
#include <math.h>
#include <stdlib.h>
#include <time.h>

#include "buffer.h"
#include "fractional-scale-v1-client-protocol.h"
#include "loop.h"
#include "util.h"
#include "viewporter-client-protocol.h"
#include "wallpaper.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/* A GPU-drawn edge strip: a synchronised subsurface over the strip. */
struct strip {
  struct wl_surface *surface;
  struct wl_subsurface *sub;
  struct wp_viewport *viewport;
  struct wp_gl_win *win;
  int lx, ly, lw, lh; /* logical, in the parent */
  int bw, bh;         /* buffer px */
};

struct output {
  struct wl_list link;
  struct swell_output *so;
  struct wl_surface *surface;
  struct zwlr_layer_surface_v1 *layer;
  struct wp_viewport *viewport;
  struct wp_fractional_scale_v1 *fractional;
  struct wl_callback *frame_cb;
  struct loop_timer *fps_timer; /* fps cap: the delayed next frame */
  struct buffer *bufs[2];
  bool stale[2]; /* needs a full repaint before partial ones are valid */
  bool ringed[2]; /* holds visualiser bars, outside the small logo tile */
  struct strip strips[2]; /* left, right; surface NULL unless shown */
  int width, height; /* logical, from configure */
  uint32_t scale120;  /* preferred fractional scale, 0 if unknown */
  bool configured;
  bool retry;         /* a draw found both buffers busy */
  int64_t last_draw;  /* ms */
};

static struct swell_wl *wl;
static struct wl_list outputs; /* output.link */
static bool gl_broken; /* a GPU draw failed: the CPU draws from now on */

static void draw(struct output *out, bool full);

/* ---- frame pacing ---- */

static void request_frame(struct output *out);

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
  struct output *out = data;
  wl_callback_destroy(cb);
  out->frame_cb = NULL;
  if (!wp_animating() && !out->retry)
    return; /* frozen: stop here, the last frame stays */
  int fps = wp_fps_cap();
  int64_t wait = fps > 0 ? out->last_draw + 1000 / fps - now_ms() : 0;
  if (wait > 1 && !out->retry)
    loop_timer_arm(out->fps_timer, wait, 0);
  else
    draw(out, false);
}

static const struct wl_callback_listener frame_listener = {.done = frame_done};

static void request_frame(struct output *out) {
  if (out->frame_cb)
    return;
  out->frame_cb = wl_surface_frame(out->surface);
  wl_callback_add_listener(out->frame_cb, &frame_listener, out);
}

static void on_fps_timer(struct loop_timer *t, void *data) {
  struct output *out = data;
  if (out->surface && wp_animating())
    draw(out, false);
}

/* ---- drawing ---- */

static int output_scale_px(struct output *out, int logical) {
  if (out->scale120 && out->viewport)
    return (int)lround(logical * out->scale120 / 120.0);
  return logical * (out->so->scale > 0 ? out->so->scale : 1);
}

static int get_buffer(struct output *out, int pw, int ph) {
  for (int i = 0; i < 2; ++i) {
    struct buffer *b = out->bufs[i];
    if (b && b->busy)
      continue;
    if (b && (b->width != pw || b->height != ph)) {
      buffer_destroy(b);
      b = out->bufs[i] = NULL;
    }
    if (!b) {
      b = out->bufs[i] = buffer_create(wl->shm, pw, ph, WL_SHM_FORMAT_XRGB8888);
      if (!b)
        return -1;
      out->stale[i] = true;
      out->ringed[i] = false;
    }
    return i;
  }
  return -1;
}

/* ---- GPU edge strips ---- */

static bool gpu_edges(void) {
  const struct wallpaper_config *c = wp_config();
  return c->viz && c->viz_edges && c->viz_edge_smooth && c->viz_gpu &&
         !gl_broken && wl->subcompositor && wp_gl_init(wl->display);
}

static void strip_destroy(struct strip *st) {
  wp_gl_win_destroy(st->win);
  if (st->viewport)
    wp_viewport_destroy(st->viewport);
  if (st->sub)
    wl_subsurface_destroy(st->sub);
  if (st->surface)
    wl_surface_destroy(st->surface);
  *st = (struct strip){0};
}

static void strips_destroy(struct output *out) {
  for (int s = 0; s < 2; ++s)
    strip_destroy(&out->strips[s]);
}

static long gcd(long a, long b) { return b ? gcd(b, a % b) : a; }

/* Draw side s over buffer rect r of the pw x ph parent buffer. The strip
 * covers r in whole logical px, widened so its corner lands on a whole
 * buffer px where the scale allows. False if the GPU failed. */
static bool strip_draw(struct output *out, int s, struct wp_rect r, int pw,
                       int ph, const struct wp_frame *f) {
  struct strip *st = &out->strips[s];
  bool fractional = out->scale120 && out->viewport;
  double k = (double)pw / out->width; /* buffer px per logical px */
  int m = fractional ? (int)(120 / gcd(out->scale120, 120)) : 1;
  int lx0 = (int)floor(r.x / k / m) * m, ly0 = (int)floor(r.y / k / m) * m;
  int lx1 = (int)ceil((r.x + r.w) / k / m) * m;
  int ly1 = (int)ceil((r.y + r.h) / k / m) * m;
  lx1 = lx1 < out->width ? lx1 : out->width;
  ly1 = ly1 < out->height ? ly1 : out->height;
  int lw = lx1 - lx0, lh = ly1 - ly0;
  if (lw <= 0 || lh <= 0)
    return true;
  int bw = fractional ? (int)lround(lw * k) : lw * (int)lround(k);
  int bh = fractional ? (int)lround(lh * k) : lh * (int)lround(k);
  if (!st->surface) {
    st->surface = wl_compositor_create_surface(wl->compositor);
    st->sub = wl_subcompositor_get_subsurface(wl->subcompositor, st->surface,
                                              out->surface);
    /* Pointer input goes through to the wallpaper. */
    struct wl_region *none = wl_compositor_create_region(wl->compositor);
    wl_surface_set_input_region(st->surface, none);
    wl_region_destroy(none);
    if (fractional)
      st->viewport = wp_viewporter_get_viewport(wl->viewporter, st->surface);
    st->win = wp_gl_win_create(st->surface, bw, bh);
    if (!st->win)
      return false;
    st->lx = st->ly = -1;
  } else if (bw != st->bw || bh != st->bh) {
    wp_gl_win_resize(st->win, bw, bh);
  }
  st->bw = bw;
  st->bh = bh;
  if (lx0 != st->lx || ly0 != st->ly)
    wl_subsurface_set_position(st->sub, lx0, ly0);
  if (st->viewport)
    wp_viewport_set_destination(st->viewport, lw, lh);
  else
    wl_surface_set_buffer_scale(st->surface, (int)lround(k));
  st->lx = lx0;
  st->ly = ly0;
  st->lw = lw;
  st->lh = lh;
  return wp_gl_edge(st->win, wp_config(), pw, ph, f, s, lx0 * k, ly0 * k,
                    lw * k / bw, lh * k / bh, bw, bh);
}

/* Show the frame's edge bars in the strips (false: the GPU failed). */
static bool strips_draw(struct output *out, int pw, int ph,
                        const struct wp_frame *f) {
  struct wp_rect r[WP_REGIONS];
  wp_regions(wp_config(), pw, ph, true, r);
  for (int s = 0; s < 2; ++s)
    if (!strip_draw(out, s, r[2 + s], pw, ph, f))
      return false;
  return true;
}

const char *wp_surfaces_viz_renderer(void) {
  if (!wp_config()->viz_gpu)
    return "cpu";
  if (gl_broken || !wl->subcompositor || !wp_gl_init(wl->display))
    return "cpu (no GPU)";
  return "gpu";
}

static void draw(struct output *out, bool full) {
  if (!out->surface || !out->configured || out->width <= 0)
    return;
  int pw = output_scale_px(out, out->width);
  int ph = output_scale_px(out, out->height);
  if (full)
    out->stale[0] = out->stale[1] = true;
  int i = get_buffer(out, pw, ph);
  if (i < 0) {
    /* Both busy; one is released by the time the next frame is due. */
    out->retry = true;
    request_frame(out);
    wl_surface_commit(out->surface);
    return;
  }
  out->retry = false;
  struct buffer *b = out->bufs[i];
  bool whole = out->stale[i];

  int64_t now = now_ms();
  struct wp_frame f;
  wp_frame_at(now, &f);
  /* While either buffer holds visualiser bars, repaint and damage their
   * larger area (the ring's or the edge strips): that clears this buffer's
   * old bars and covers the difference from the other one. */
  /* The GPU's strips take the edge bars off the cairo buffer. */
  bool gpu = f.nbars > 0 && gpu_edges();
  if (gpu && !strips_draw(out, pw, ph, &f)) {
    warn("wallpaper: GPU drawing failed; the visualiser is back on the CPU");
    gl_broken = true;
    gpu = false;
  }
  if (!gpu)
    strips_destroy(out);
  struct wp_frame cf = f;
  if (gpu)
    cf.nbars = 0;
  bool ring = cf.nbars > 0 || out->ringed[0] || out->ringed[1];
  cairo_t *cr = cairo_create(b->cairo);
  wp_render(cr, wp_config(), pw, ph, &cf, whole, ring);
  cairo_destroy(cr);
  cairo_surface_flush(b->cairo);
  out->stale[i] = false;
  out->ringed[i] = cf.nbars > 0;

  if (out->scale120 && out->viewport) {
    wl_surface_set_buffer_scale(out->surface, 1);
    wp_viewport_set_destination(out->viewport, out->width, out->height);
  } else {
    wl_surface_set_buffer_scale(out->surface, pw / out->width);
  }
  wl_surface_attach(out->surface, b->wl_buffer, 0, 0);
  /* The other buffer was last painted with the same background, so only
   * the bar and the logo can differ from what the compositor has. */
  struct wp_rect r[WP_REGIONS];
  wp_regions(wp_config(), pw, ph, ring, r);
  if (whole)
    wl_surface_damage_buffer(out->surface, 0, 0, pw, ph);
  else
    for (int k = 0; k < WP_REGIONS; ++k)
      if (r[k].w > 0 && r[k].h > 0)
        wl_surface_damage_buffer(out->surface, r[k].x, r[k].y, r[k].w,
                                 r[k].h);
  if (wp_animating())
    request_frame(out);
  wl_surface_commit(out->surface);
  b->busy = true;
  out->last_draw = now;
  wp_count_frame(now);
}

/* ---- surfaces ---- */

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer,
                            uint32_t serial, uint32_t w, uint32_t h) {
  struct output *out = data;
  zwlr_layer_surface_v1_ack_configure(layer, serial);
  bool changed = !out->configured || out->width != (int)w ||
                 out->height != (int)h;
  out->width = w;
  out->height = h;
  out->configured = true;
  if (changed)
    draw(out, true);
}

static void destroy_surface(struct output *out);

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer) {
  destroy_surface(data);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

static void preferred_scale(void *data, struct wp_fractional_scale_v1 *f,
                            uint32_t scale120) {
  struct output *out = data;
  if (out->scale120 == scale120)
    return;
  out->scale120 = scale120;
  draw(out, true);
}

static const struct wp_fractional_scale_v1_listener fractional_listener = {
    .preferred_scale = preferred_scale,
};

static void create_surface(struct output *out) {
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
      ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, wp_config()->namespace);
  zwlr_layer_surface_v1_add_listener(out->layer, &layer_listener, out);
  zwlr_layer_surface_v1_set_anchor(out->layer,
                                   ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                       ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                       ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                       ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
  zwlr_layer_surface_v1_set_size(out->layer, 0, 0);
  zwlr_layer_surface_v1_set_exclusive_zone(out->layer, -1);
  zwlr_layer_surface_v1_set_keyboard_interactivity(
      out->layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
  wl_set_input(wl, out->surface, &wp_input, out);
  wl_surface_commit(out->surface);
}

static void destroy_surface(struct output *out) {
  loop_timer_disarm(out->fps_timer);
  strips_destroy(out);
  if (out->surface)
    wl_clear_input(wl, out->surface);
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
    buffer_destroy(out->bufs[i]);
    out->bufs[i] = NULL;
  }
  out->frame_cb = NULL;
  out->fractional = NULL;
  out->viewport = NULL;
  out->layer = NULL;
  out->surface = NULL;
  out->configured = false;
  out->retry = false;
  out->scale120 = 0;
  out->width = out->height = 0;
}

static void on_output(void *data, struct swell_output *so, bool added) {
  if (added) {
    struct output *out = xcalloc(1, sizeof(*out));
    out->so = so;
    out->fps_timer =
        loop_add_timer(wl->loop, CLOCK_MONOTONIC, on_fps_timer, out);
    wl_list_insert(outputs.prev, &out->link);
    create_surface(out);
    return;
  }
  struct output *out, *tmp;
  wl_list_for_each_safe(out, tmp, &outputs, link) {
    if (out->so != so)
      continue;
    destroy_surface(out);
    loop_remove_timer(out->fps_timer);
    wl_list_remove(&out->link);
    free(out);
  }
}

void wp_surfaces_init(struct swell_wl *w) {
  wl = w;
  wl_list_init(&outputs);
  struct swell_output *so;
  wl_list_for_each(so, &wl->outputs, link) if (so->done) on_output(NULL, so, true);
  wl_add_output_listener(wl, on_output, NULL);
}

void wp_surfaces_finish(void) {
  wl_remove_output_listener(wl, on_output, NULL);
  struct output *out, *tmp;
  wl_list_for_each_safe(out, tmp, &outputs, link) on_output(NULL, out->so, false);
}

void wp_surfaces_recreate(void) {
  struct output *out;
  wl_list_for_each(out, &outputs, link) {
    destroy_surface(out);
    create_surface(out);
  }
}

void wp_surfaces_draw(bool full) {
  struct output *out;
  wl_list_for_each(out, &outputs, link) {
    loop_timer_disarm(out->fps_timer);
    draw(out, full);
  }
}

int wp_surfaces_count(void) {
  int n = 0;
  struct output *out;
  wl_list_for_each(out, &outputs, link) n += out->surface != NULL;
  return n;
}
