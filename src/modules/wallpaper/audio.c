/* PipeWire capture of the sink's monitor for the visualiser.
 *
 * The connection only exists while the wallpaper animates: freezing tears
 * all of it down, so a frozen wallpaper still costs nothing. The capture
 * stream is passive, so it never keeps the sink (and the sound card) awake
 * by itself: with nothing playing the graph idles and no data arrives. The
 * PipeWire loop runs inside ours (its fd in our poll), and the stream is
 * not RT, so samples arrive on our thread. */
#include <errno.h>
#include <poll.h>
#include <string.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>

#include "loop.h"
#include "util.h"
#include "wallpaper.h"

/* Samples kept: room for one analysis plus a quantum or two. */
#define RING 4096
/* Audio older than this is stale: the bars fall. */
#define FRESH_MS 250

static struct {
  struct loop *loop;
  struct loop_watch *watch;
  struct pw_loop *pl;
  struct pw_context *ctx;
  struct pw_core *core;
  struct spa_hook core_listener;
  struct pw_stream *stream;
  struct spa_hook stream_listener;
  bool broken; /* the core failed: torn down after the current iterate */
  bool warned;
  bool pw_inited;

  float ring[RING];
  int head; /* next write */
  int rate;
  int64_t last_data; /* ms */
} a;

static void on_param_changed(void *data, uint32_t id,
                             const struct spa_pod *param) {
  if (!param || id != SPA_PARAM_Format)
    return;
  struct spa_audio_info_raw info;
  if (spa_format_audio_raw_parse(param, &info) >= 0 && info.rate > 0)
    a.rate = (int)info.rate;
}

static void on_process(void *data) {
  struct pw_buffer *b = pw_stream_dequeue_buffer(a.stream);
  if (!b)
    return;
  struct spa_data *d = &b->buffer->datas[0];
  if (d->data && d->chunk) {
    uint32_t off = SPA_MIN(d->chunk->offset, d->maxsize);
    uint32_t size = SPA_MIN(d->chunk->size, d->maxsize - off);
    const float *s = SPA_PTROFF(d->data, off, const float);
    int n = (int)(size / sizeof(float));
    int64_t now = now_ms();
    if (now - a.last_data > FRESH_MS) {
      /* After a pause, don't analyse what played before it. */
      memset(a.ring, 0, sizeof(a.ring));
    }
    if (n > 0)
      a.last_data = now;
    for (int i = n > RING ? n - RING : 0; i < n; ++i) {
      a.ring[a.head] = s[i];
      a.head = (a.head + 1) % RING;
    }
  }
  pw_stream_queue_buffer(a.stream, b);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .param_changed = on_param_changed,
    .process = on_process,
};

static void on_core_error(void *data, uint32_t id, int seq, int res,
                          const char *message) {
  if (id != PW_ID_CORE)
    return;
  if (!a.warned)
    warn("wallpaper: pipewire: %s", message ? message : spa_strerror(res));
  a.warned = true;
  if (res == -EPIPE)
    a.broken = true;
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .error = on_core_error,
};

static void on_pw_fd(int fd, short revents, void *data) {
  pw_loop_iterate(a.pl, 0);
  if (a.broken || (revents & (POLLERR | POLLHUP)))
    wp_audio_stop(); /* the next animate tries again */
}

static bool connect_stream(const struct wallpaper_config *c) {
  struct pw_properties *props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
      PW_KEY_MEDIA_ROLE, "Music", PW_KEY_STREAM_CAPTURE_SINK, "true",
      PW_KEY_NODE_NAME, "swell-wallpaper", PW_KEY_NODE_DESCRIPTION,
      "swell wallpaper visualiser", PW_KEY_NODE_PASSIVE, "true",
      /* About 47 updates a second, so the bars move smoothly. */
      PW_KEY_NODE_LATENCY, "1024/48000", NULL);
  if (c->viz_target[0])
    pw_properties_set(props, PW_KEY_TARGET_OBJECT, c->viz_target);
  a.stream = pw_stream_new(a.core, "swell-wallpaper", props);
  if (!a.stream)
    return false;
  pw_stream_add_listener(a.stream, &a.stream_listener, &stream_events, NULL);

  uint8_t buf[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
  /* Mono float at the graph's rate: PipeWire downmixes. */
  struct spa_audio_info_raw info = {.format = SPA_AUDIO_FORMAT_F32,
                                    .channels = 1};
  info.position[0] = SPA_AUDIO_CHANNEL_MONO;
  const struct spa_pod *params[1] = {
      spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
  return pw_stream_connect(a.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                           PW_STREAM_FLAG_AUTOCONNECT |
                               PW_STREAM_FLAG_MAP_BUFFERS,
                           params, 1) >= 0;
}

void wp_audio_init(void) {
  if (!a.pw_inited)
    pw_init(NULL, NULL);
  a.pw_inited = true;
}

void wp_audio_finish(void) {
  wp_audio_stop();
  if (a.pw_inited)
    pw_deinit();
  a.pw_inited = false;
}

void wp_audio_start(struct loop *loop, const struct wallpaper_config *c) {
  if (a.pl || !a.pw_inited)
    return;
  a.loop = loop;
  a.broken = false;
  a.rate = 48000;
  a.last_data = 0;
  a.pl = pw_loop_new(NULL);
  if (!a.pl)
    return;
  pw_loop_enter(a.pl);
  a.watch = loop_add_fd(loop, pw_loop_get_fd(a.pl), POLLIN, on_pw_fd, NULL);
  a.ctx = pw_context_new(a.pl, NULL, 0);
  if (a.ctx)
    a.core = pw_context_connect(a.ctx, NULL, 0);
  if (!a.core) {
    if (!a.warned)
      warn("wallpaper: no pipewire, the visualiser stays off: %s",
           strerror(errno));
    a.warned = true;
    wp_audio_stop();
    return;
  }
  pw_core_add_listener(a.core, &a.core_listener, &core_events, NULL);
  if (!connect_stream(c)) {
    warn("wallpaper: pipewire: could not create the capture stream");
    wp_audio_stop();
    return;
  }
  a.warned = false; /* report the next failure again */
}

void wp_audio_stop(void) {
  if (!a.pl)
    return;
  if (a.stream)
    pw_stream_destroy(a.stream);
  a.stream = NULL;
  if (a.core) {
    spa_hook_remove(&a.core_listener);
    pw_core_disconnect(a.core);
  }
  a.core = NULL;
  if (a.ctx)
    pw_context_destroy(a.ctx);
  a.ctx = NULL;
  if (a.watch)
    loop_remove_fd(a.watch);
  a.watch = NULL;
  pw_loop_leave(a.pl);
  pw_loop_destroy(a.pl);
  a.pl = NULL;
}

const char *wp_audio_state(void) {
  if (!a.stream)
    return "off";
  return now_ms() - a.last_data <= FRESH_MS ? "capturing" : "waiting";
}

bool wp_audio_samples(float *out, int *rate) {
  if (!a.stream || now_ms() - a.last_data > FRESH_MS)
    return false;
  for (int i = 0; i < WP_VIZ_FFT; ++i)
    out[i] = a.ring[(a.head - WP_VIZ_FFT + i + RING) % RING];
  *rate = a.rate;
  return true;
}
