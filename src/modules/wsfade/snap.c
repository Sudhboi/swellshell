#include "snap.h"

#include <stdlib.h>

#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "util.h"

struct snap {
  struct swell_wl *wl;
  snap_fn fn;
  void *data;
  struct ext_image_capture_source_v1 *source;
  struct ext_image_copy_capture_session_v1 *session;
  struct ext_image_copy_capture_frame_v1 *frame;
  uint32_t width, height, format;
  int format_rank;
  struct buffer *buf;
  bool called;
};

static void stop(struct snap *s) {
  if (s->frame)
    ext_image_copy_capture_frame_v1_destroy(s->frame);
  if (s->session)
    ext_image_copy_capture_session_v1_destroy(s->session);
  if (s->source)
    ext_image_capture_source_v1_destroy(s->source);
  s->frame = NULL;
  s->session = NULL;
  s->source = NULL;
}

/* Last thing done for an event: fn may destroy s. */
static void finish(struct snap *s, bool ok) {
  stop(s);
  if (!ok) {
    buffer_destroy(s->buf);
    s->buf = NULL;
  }
  if (s->called)
    return;
  s->called = true;
  s->fn(s, s->buf, s->data);
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
  finish(data, true);
}

static void frame_failed(void *data,
                         struct ext_image_copy_capture_frame_v1 *frame,
                         uint32_t reason) {
  finish(data, false);
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
    .transform = frame_transform,
    .damage = frame_damage,
    .presentation_time = frame_presentation_time,
    .ready = frame_ready,
    .failed = frame_failed,
};

static void session_buffer_size(void *data,
                                struct ext_image_copy_capture_session_v1 *ses,
                                uint32_t width, uint32_t height) {
  struct snap *s = data;
  s->width = width;
  s->height = height;
}

static void session_shm_format(void *data,
                               struct ext_image_copy_capture_session_v1 *ses,
                               uint32_t format) {
  struct snap *s = data;
  int rank = buffer_format_rank(format);
  if (rank > s->format_rank) {
    s->format_rank = rank;
    s->format = format;
  }
}

static void session_dmabuf_device(void *data,
                                  struct ext_image_copy_capture_session_v1 *ses,
                                  struct wl_array *device) {}

static void session_dmabuf_format(void *data,
                                  struct ext_image_copy_capture_session_v1 *ses,
                                  uint32_t format, struct wl_array *modifiers) {
}

/* The first set of constraints: capture one frame with them. */
static void session_done(void *data,
                         struct ext_image_copy_capture_session_v1 *ses) {
  struct snap *s = data;
  if (s->frame)
    return;
  if (!s->format || !s->width || !s->height) {
    finish(s, false);
    return;
  }
  s->buf = buffer_create(s->wl->shm, s->width, s->height, s->format);
  if (!s->buf) {
    finish(s, false);
    return;
  }
  s->frame = ext_image_copy_capture_session_v1_create_frame(s->session);
  ext_image_copy_capture_frame_v1_add_listener(s->frame, &frame_listener, s);
  ext_image_copy_capture_frame_v1_attach_buffer(s->frame, s->buf->wl_buffer);
  ext_image_copy_capture_frame_v1_damage_buffer(s->frame, 0, 0, s->width,
                                                s->height);
  ext_image_copy_capture_frame_v1_capture(s->frame);
}

static void session_stopped(void *data,
                            struct ext_image_copy_capture_session_v1 *ses) {
  finish(data, false);
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

struct snap *snap_start(struct swell_wl *wl, struct wl_output *output,
                        snap_fn fn, void *data) {
  if (!wl->output_source_manager || !wl->copy_manager)
    return NULL;
  struct snap *s = xcalloc(1, sizeof(*s));
  s->wl = wl;
  s->fn = fn;
  s->data = data;
  s->source = ext_output_image_capture_source_manager_v1_create_source(
      wl->output_source_manager, output);
  /* options 0: without the cursor, which stays live above the picture */
  s->session = ext_image_copy_capture_manager_v1_create_session(
      wl->copy_manager, s->source, 0);
  ext_image_copy_capture_session_v1_add_listener(s->session, &session_listener,
                                                 s);
  return s;
}

void snap_destroy(struct snap *s) {
  if (!s)
    return;
  stop(s);
  buffer_destroy(s->buf);
  free(s);
}
