#include "capture.h"

#include <stdlib.h>
#include <string.h>

#include "buffer.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "util.h"

struct toplevel {
  struct wl_list link;
  struct ov *ov;
  struct ext_foreign_toplevel_handle_v1 *handle;
  char *ident;
  bool announced;
};

enum capture_state {
  CAPTURE_WAIT_CONSTRAINTS, /* waiting for the session's "done" */
  CAPTURE_IDLE,             /* ready to request a frame */
  CAPTURE_PENDING,          /* a frame is in flight */
  CAPTURE_DEAD,             /* stopped or unusable */
};

struct capture {
  struct wl_list link;
  struct ov *ov;
  char *ident;
  bool used;

  struct ext_image_capture_source_v1 *source;
  struct ext_image_copy_capture_session_v1 *session;
  struct ext_image_copy_capture_frame_v1 *frame;
  enum capture_state state;

  /* constraints, collected until "done" */
  uint32_t width, height;
  uint32_t format; /* best offered shm format, 0 if none usable */
  int format_rank;
  bool fresh_constraints; /* "done" arrived since the last frame request */

  struct buffer *bufs[2];
  int back;        /* index of the buffer being captured into */
  bool has_front;  /* bufs[!back] holds a complete frame */
  bool got_frame;  /* at least one frame arrived, ever */
  int64_t last_frame;

  /* Downscaled copies of the latest frame. Two slots so the tile and the
   * hover preview don't evict each other. */
  struct {
    cairo_surface_t *surface;
    bool stale;
    uint64_t used; /* for LRU */
  } thumbs[2];
  uint64_t thumb_clock;
};

/* ---- foreign toplevel list ------------------------------------------- */

static void toplevel_closed(void *data,
                            struct ext_foreign_toplevel_handle_v1 *handle) {
  struct toplevel *tl = data;
  wl_list_remove(&tl->link);
  ext_foreign_toplevel_handle_v1_destroy(tl->handle);
  free(tl->ident);
  free(tl);
}

static void toplevel_done(void *data,
                          struct ext_foreign_toplevel_handle_v1 *handle) {
  struct toplevel *tl = data;
  /* A window may have shown up in the model before its handle did. */
  if (!tl->announced)
    tl->ov->model_dirty = true;
  tl->announced = true;
}

static void toplevel_title(void *data,
                           struct ext_foreign_toplevel_handle_v1 *handle,
                           const char *title) {}

static void toplevel_app_id(void *data,
                            struct ext_foreign_toplevel_handle_v1 *handle,
                            const char *app_id) {}

static void toplevel_identifier(void *data,
                                struct ext_foreign_toplevel_handle_v1 *handle,
                                const char *ident) {
  struct toplevel *tl = data;
  free(tl->ident);
  tl->ident = xstrdup(ident);
}

static const struct ext_foreign_toplevel_handle_v1_listener toplevel_listener = {
    .closed = toplevel_closed,
    .done = toplevel_done,
    .title = toplevel_title,
    .app_id = toplevel_app_id,
    .identifier = toplevel_identifier,
};

static void list_toplevel(void *data, struct ext_foreign_toplevel_list_v1 *list,
                          struct ext_foreign_toplevel_handle_v1 *handle) {
  struct ov *ov = data;
  struct toplevel *tl = xcalloc(1, sizeof(*tl));
  tl->ov = ov;
  tl->handle = handle;
  ext_foreign_toplevel_handle_v1_add_listener(handle, &toplevel_listener, tl);
  wl_list_insert(&ov->toplevels, &tl->link);
}

static void list_finished(void *data,
                          struct ext_foreign_toplevel_list_v1 *list) {}

static const struct ext_foreign_toplevel_list_v1_listener list_listener = {
    .toplevel = list_toplevel,
    .finished = list_finished,
};

static struct toplevel *find_toplevel(struct ov *ov, const char *ident) {
  struct toplevel *tl;
  wl_list_for_each(tl, &ov->toplevels, link) {
    if (tl->ident && strcmp(tl->ident, ident) == 0)
      return tl;
  }
  return NULL;
}

/* ---- capture sessions ------------------------------------------------ */

static void capture_stop(struct capture *cap) {
  if (cap->frame) {
    ext_image_copy_capture_frame_v1_destroy(cap->frame);
    cap->frame = NULL;
  }
  if (cap->session) {
    ext_image_copy_capture_session_v1_destroy(cap->session);
    cap->session = NULL;
  }
  if (cap->source) {
    ext_image_capture_source_v1_destroy(cap->source);
    cap->source = NULL;
  }
  cap->state = CAPTURE_DEAD;
}

static void capture_destroy(struct capture *cap) {
  capture_stop(cap);
  for (int i = 0; i < 2; ++i)
    buffer_destroy(cap->bufs[i]);
  for (int i = 0; i < 2; ++i)
    if (cap->thumbs[i].surface)
      cairo_surface_destroy(cap->thumbs[i].surface);
  wl_list_remove(&cap->link);
  free(cap->ident);
  free(cap);
}

static void frame_transform(void *data,
                            struct ext_image_copy_capture_frame_v1 *frame,
                            uint32_t transform) {}

static void frame_damage(void *data,
                         struct ext_image_copy_capture_frame_v1 *frame,
                         int32_t x, int32_t y, int32_t w, int32_t h) {}

static void frame_presentation_time(
    void *data, struct ext_image_copy_capture_frame_v1 *frame,
    uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {}

static void frame_ready(void *data,
                        struct ext_image_copy_capture_frame_v1 *frame) {
  struct capture *cap = data;
  ext_image_copy_capture_frame_v1_destroy(cap->frame);
  cap->frame = NULL;

  cap->back = !cap->back;
  cap->has_front = true;
  cap->got_frame = true;
  cap->thumbs[0].stale = cap->thumbs[1].stale = true;
  cap->last_frame = now_ms();
  cap->state = CAPTURE_IDLE;

  if (!cap->ov->cfg.live)
    capture_stop(cap); /* keep the image, stop updating */
  ov_redraw(cap->ov);
}

static void frame_failed(void *data,
                         struct ext_image_copy_capture_frame_v1 *frame,
                         uint32_t reason) {
  struct capture *cap = data;
  ext_image_copy_capture_frame_v1_destroy(cap->frame);
  cap->frame = NULL;
  switch (reason) {
  case EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS:
    /* the new constraints may have arrived before this event, or follow */
    cap->state =
        cap->fresh_constraints ? CAPTURE_IDLE : CAPTURE_WAIT_CONSTRAINTS;
    break;
  case EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED:
    capture_stop(cap);
    break;
  default:
    /* transient; try again later */
    cap->last_frame = now_ms();
    cap->state = CAPTURE_IDLE;
    break;
  }
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
    .transform = frame_transform,
    .damage = frame_damage,
    .presentation_time = frame_presentation_time,
    .ready = frame_ready,
    .failed = frame_failed,
};

static void session_buffer_size(void *data,
                                struct ext_image_copy_capture_session_v1 *s,
                                uint32_t width, uint32_t height) {
  struct capture *cap = data;
  cap->width = width;
  cap->height = height;
}

static bool format_swaps_rb(uint32_t format) {
  return format == WL_SHM_FORMAT_ABGR8888 || format == WL_SHM_FORMAT_XBGR8888;
}

static void session_shm_format(void *data,
                               struct ext_image_copy_capture_session_v1 *s,
                               uint32_t format) {
  struct capture *cap = data;
  int rank = buffer_format_rank(format);
  if (rank > cap->format_rank) {
    cap->format_rank = rank;
    cap->format = format;
  }
}

static void session_dmabuf_device(void *data,
                                  struct ext_image_copy_capture_session_v1 *s,
                                  struct wl_array *device) {}

static void session_dmabuf_format(void *data,
                                  struct ext_image_copy_capture_session_v1 *s,
                                  uint32_t format,
                                  struct wl_array *modifiers) {}

static void session_done(void *data,
                         struct ext_image_copy_capture_session_v1 *s) {
  struct capture *cap = data;
  uint32_t format = cap->format;
  /* constraints are re-sent in full before every "done" */
  cap->format = 0;
  cap->format_rank = 0;
  if (!format || !cap->width || !cap->height) {
    capture_stop(cap);
    return;
  }
  cap->format = format;
  for (int i = 0; i < 2; ++i) {
    struct buffer *b = cap->bufs[i];
    if (b && (b->width != (int)cap->width || b->height != (int)cap->height ||
              b->format != format)) {
      if (i == !cap->back && cap->has_front)
        continue; /* keep showing the old frame until a new one arrives */
      buffer_destroy(b);
      cap->bufs[i] = NULL;
    }
  }
  cap->fresh_constraints = true;
  if (cap->state == CAPTURE_WAIT_CONSTRAINTS)
    cap->state = CAPTURE_IDLE;
}

static void session_stopped(void *data,
                            struct ext_image_copy_capture_session_v1 *s) {
  capture_stop(data);
}

static const struct ext_image_copy_capture_session_v1_listener
    session_listener = {
        .buffer_size = session_buffer_size,
        .shm_format = session_shm_format,
        .dmabuf_device = session_dmabuf_device,
        .dmabuf_format = session_dmabuf_format,
        .done = session_done,
        .stopped = session_stopped,
};

static void request_frame(struct capture *cap) {
  struct ov *ov = cap->ov;
  uint32_t format = cap->format;
  struct buffer *b = cap->bufs[cap->back];
  if (b && (b->width != (int)cap->width || b->height != (int)cap->height ||
            b->format != format)) {
    buffer_destroy(b);
    b = NULL;
  }
  if (!b) {
    b = buffer_create(ov->wl->shm, cap->width, cap->height, format);
    if (!b) {
      capture_stop(cap);
      return;
    }
    cap->bufs[cap->back] = b;
  }

  cap->fresh_constraints = false;
  cap->frame = ext_image_copy_capture_session_v1_create_frame(cap->session);
  ext_image_copy_capture_frame_v1_add_listener(cap->frame, &frame_listener,
                                               cap);
  ext_image_copy_capture_frame_v1_attach_buffer(cap->frame, b->wl_buffer);
  ext_image_copy_capture_frame_v1_damage_buffer(cap->frame, 0, 0, b->width,
                                                b->height);
  ext_image_copy_capture_frame_v1_capture(cap->frame);
  cap->state = CAPTURE_PENDING;
}

static struct capture *capture_create(struct ov *ov, struct toplevel *tl) {
  struct capture *cap = xcalloc(1, sizeof(*cap));
  cap->ov = ov;
  cap->ident = xstrdup(tl->ident);
  cap->state = CAPTURE_WAIT_CONSTRAINTS;
  cap->source = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
      ov->wl->source_manager, tl->handle);
  cap->session = ext_image_copy_capture_manager_v1_create_session(
      ov->wl->copy_manager, cap->source, 0);
  ext_image_copy_capture_session_v1_add_listener(cap->session,
                                                 &session_listener, cap);
  wl_list_insert(&ov->captures, &cap->link);
  return cap;
}

/* ---- public API ------------------------------------------------------ */

static bool capture_supported(struct ov *ov) {
  return ov->cfg.capture && ov->toplevel_list && ov->wl->source_manager &&
         ov->wl->copy_manager;
}

void ov_capture_init(struct ov *ov) {
  wl_list_init(&ov->toplevels);
  wl_list_init(&ov->captures);
  ov->toplevel_list = NULL;
  if (!ov->cfg.capture)
    return;
  /* The listener must be attached right at bind time, or the initial
   * toplevel events are lost. The caller does a roundtrip for them. */
  ov->toplevel_list = wl_bind_toplevel_list(ov->wl);
  if (ov->toplevel_list)
    ext_foreign_toplevel_list_v1_add_listener(ov->toplevel_list,
                                              &list_listener, ov);
  static bool warned;
  if (!capture_supported(ov) && !warned) {
    warned = true;
    warn("compositor lacks ext-image-copy-capture/ext-foreign-toplevel-list;"
         " thumbnails disabled");
  }
}

void ov_capture_finish(struct ov *ov) {
  struct capture *cap, *ctmp;
  wl_list_for_each_safe(cap, ctmp, &ov->captures, link) capture_destroy(cap);
  struct toplevel *tl, *ttmp;
  wl_list_for_each_safe(tl, ttmp, &ov->toplevels, link) {
    wl_list_remove(&tl->link);
    ext_foreign_toplevel_handle_v1_destroy(tl->handle);
    free(tl->ident);
    free(tl);
  }
  if (ov->toplevel_list) {
    ext_foreign_toplevel_list_v1_stop(ov->toplevel_list);
    ext_foreign_toplevel_list_v1_destroy(ov->toplevel_list);
    ov->toplevel_list = NULL;
  }
}

void ov_capture_bind_model(struct ov *ov) {
  if (!capture_supported(ov))
    return;

  struct capture *cap, *tmp;
  wl_list_for_each(cap, &ov->captures, link) cap->used = false;

  for (int i = 0; i < ov->model.nwindows; ++i) {
    struct window *w = ov->model.windows[i];
    w->cap = NULL;
    if (!w->ident)
      continue;
    wl_list_for_each(cap, &ov->captures, link) {
      if (strcmp(cap->ident, w->ident) == 0) {
        w->cap = cap;
        break;
      }
    }
    if (!w->cap) {
      struct toplevel *tl = find_toplevel(ov, w->ident);
      if (tl)
        w->cap = capture_create(ov, tl);
    }
    if (w->cap)
      w->cap->used = true;
  }

  wl_list_for_each_safe(cap, tmp, &ov->captures, link) {
    if (!cap->used)
      capture_destroy(cap);
  }
}

static bool surface_is(cairo_surface_t *s, int w, int h) {
  return s && cairo_image_surface_get_width(s) == w &&
         cairo_image_surface_get_height(s) == h;
}

cairo_surface_t *ov_capture_thumbnail(struct capture *cap, int w, int h) {
  if (!cap || !cap->has_front || w <= 0 || h <= 0)
    return NULL;
  struct buffer *front = cap->bufs[!cap->back];
  if (!front)
    return NULL;

  /* reuse a slot of this size, else the least recently used one */
  int slot = -1;
  for (int i = 0; i < 2; ++i)
    if (surface_is(cap->thumbs[i].surface, w, h))
      slot = i;
  if (slot < 0)
    slot = cap->thumbs[0].used <= cap->thumbs[1].used ? 0 : 1;
  cap->thumbs[slot].used = ++cap->thumb_clock;
  cairo_surface_t *thumb = cap->thumbs[slot].surface;
  if (surface_is(thumb, w, h) && !cap->thumbs[slot].stale)
    return thumb;

  /* Downscale once per frame/size instead of on every repaint. */
  if (!surface_is(thumb, w, h)) {
    if (thumb)
      cairo_surface_destroy(thumb);
    thumb = cap->thumbs[slot].surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  }
  cairo_surface_flush(front->cairo);
  cairo_surface_mark_dirty(front->cairo);

  cairo_t *cr = cairo_create(thumb);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_scale(cr, (double)w / front->width, (double)h / front->height);
  cairo_set_source_surface(cr, front->cairo, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
  cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_flush(thumb);

  if (format_swaps_rb(front->format)) {
    /* scaling is channel-agnostic, so swap red/blue on the small image */
    unsigned char *px = cairo_image_surface_get_data(thumb);
    int stride = cairo_image_surface_get_stride(thumb);
    for (int y = 0; y < h; ++y) {
      uint32_t *row = (uint32_t *)(px + (size_t)y * stride);
      for (int x = 0; x < w; ++x) {
        uint32_t p = row[x];
        row[x] = (p & 0xff00ff00) | ((p & 0xff) << 16) | ((p >> 16) & 0xff);
      }
    }
    cairo_surface_mark_dirty(thumb);
  }

  cap->thumbs[slot].stale = false;
  return thumb;
}

bool ov_capture_size(struct capture *cap, int *w, int *h) {
  struct buffer *front = cap && cap->has_front ? cap->bufs[!cap->back] : NULL;
  if (!front)
    return false;
  *w = front->width;
  *h = front->height;
  return true;
}

static int frame_interval(struct ov *ov) {
  int fps = ov->cfg.fps > 0 ? ov->cfg.fps : 1;
  return 1000 / fps;
}

void ov_capture_tick(struct ov *ov, int64_t now) {
  struct capture *cap;
  wl_list_for_each(cap, &ov->captures, link) {
    if (cap->state != CAPTURE_IDLE)
      continue;
    if (cap->got_frame && now - cap->last_frame < frame_interval(ov))
      continue;
    request_frame(cap);
  }
}

int ov_capture_timeout(struct ov *ov, int64_t now) {
  int best = -1;
  struct capture *cap;
  wl_list_for_each(cap, &ov->captures, link) {
    if (cap->state != CAPTURE_IDLE)
      continue;
    int64_t due = cap->got_frame ? cap->last_frame + frame_interval(ov) : now;
    int t = due > now ? (int)(due - now) : 0;
    if (best < 0 || t < best)
      best = t;
  }
  return best;
}

bool ov_capture_settled(struct ov *ov) {
  struct capture *cap;
  wl_list_for_each(cap, &ov->captures, link) {
    if (!cap->got_frame && cap->state != CAPTURE_DEAD)
      return false;
  }
  return true;
}
