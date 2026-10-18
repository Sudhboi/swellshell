#include "wl.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "alpha-modifier-v1-client-protocol.h"
#include "cursor-shape-v1-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-idle-notify-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "util.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

struct route {
  struct wl_surface *surface;
  const struct swell_input *handler;
  void *data;
};

struct output_listener {
  wl_output_fn fn;
  void *data;
};

/* Per-connection extras kept out of the public struct. */
struct wl_private {
  struct swell_wl pub;
  struct route *routes;
  int nroutes;
  struct output_listener *olisteners;
  int nolisteners;
};

static struct wl_private *priv(struct swell_wl *wl) {
  return (struct wl_private *)wl;
}

static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

static struct route *route_for(struct swell_wl *wl, struct wl_surface *s) {
  struct wl_private *p = priv(wl);
  for (int i = 0; s && i < p->nroutes; ++i)
    if (p->routes[i].surface == s)
      return &p->routes[i];
  return NULL;
}

void wl_set_input(struct swell_wl *wl, struct wl_surface *surface,
                  const struct swell_input *handler, void *data) {
  struct route *r = route_for(wl, surface);
  if (!r) {
    struct wl_private *p = priv(wl);
    p->routes = xrealloc(p->routes, (p->nroutes + 1) * sizeof(*p->routes));
    r = &p->routes[p->nroutes++];
  }
  *r = (struct route){surface, handler, data};
}

void wl_clear_input(struct swell_wl *wl, struct wl_surface *surface) {
  struct wl_private *p = priv(wl);
  for (int i = 0; i < p->nroutes; ++i)
    if (p->routes[i].surface == surface)
      p->routes[i--] = p->routes[--p->nroutes];
  if (wl->pointer_focus == surface)
    wl->pointer_focus = NULL;
  if (wl->keyboard_focus == surface)
    wl->keyboard_focus = NULL;
}

void wl_set_cursor(struct swell_wl *wl, uint32_t shape) {
  if (wl->cursor_device && wl->pointer_serial)
    wp_cursor_shape_device_v1_set_shape(wl->cursor_device, wl->pointer_serial,
                                        shape);
}

/* ---- outputs ---- */

void wl_add_output_listener(struct swell_wl *wl, wl_output_fn fn, void *data) {
  struct wl_private *p = priv(wl);
  p->olisteners =
      xrealloc(p->olisteners, (p->nolisteners + 1) * sizeof(*p->olisteners));
  p->olisteners[p->nolisteners++] = (struct output_listener){fn, data};
}

void wl_remove_output_listener(struct swell_wl *wl, wl_output_fn fn,
                               void *data) {
  struct wl_private *p = priv(wl);
  for (int i = 0; i < p->nolisteners; ++i)
    if (p->olisteners[i].fn == fn && p->olisteners[i].data == data)
      p->olisteners[i--] = p->olisteners[--p->nolisteners];
}

static void notify_output(struct swell_wl *wl, struct swell_output *out,
                          bool added) {
  struct wl_private *p = priv(wl);
  for (int i = 0; i < p->nolisteners; ++i)
    p->olisteners[i].fn(p->olisteners[i].data, out, added);
}

struct swell_output *wl_output_find(struct swell_wl *wl, struct wl_output *o) {
  struct swell_output *out;
  wl_list_for_each(out, &wl->outputs, link) if (out->wl_output == o) return out;
  return NULL;
}

struct output_ctx {
  struct swell_wl *wl;
  struct swell_output *out;
};

static void output_geometry(void *data, struct wl_output *o, int32_t x,
                            int32_t y, int32_t pw, int32_t ph, int32_t sub,
                            const char *make, const char *model, int32_t tr) {}
static void output_mode(void *data, struct wl_output *o, uint32_t flags,
                        int32_t w, int32_t h, int32_t refresh) {}
static void output_done(void *data, struct wl_output *o) {
  struct output_ctx *c = data;
  if (!c->out->done) {
    c->out->done = true;
    notify_output(c->wl, c->out, true);
  }
}
static void output_scale(void *data, struct wl_output *o, int32_t factor) {
  struct output_ctx *c = data;
  c->out->scale = factor > 0 ? factor : 1;
}
static void output_name(void *data, struct wl_output *o, const char *name) {
  struct output_ctx *c = data;
  free(c->out->name);
  c->out->name = xstrdup(name);
}
static void output_description(void *data, struct wl_output *o,
                               const char *d) {}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

static void output_destroy(struct swell_wl *wl, struct swell_output *out) {
  if (out->done)
    notify_output(wl, out, false);
  struct output_ctx *c = wl_output_get_user_data(out->wl_output);
  if (wl_output_get_version(out->wl_output) >= 3)
    wl_output_release(out->wl_output);
  else
    wl_output_destroy(out->wl_output);
  free(c);
  wl_list_remove(&out->link);
  free(out->name);
  free(out);
}

/* ---- pointer ---- */

static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial,
                          struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
  struct swell_wl *wl = data;
  wl->pointer_serial = serial;
  wl->pointer_focus = s;
  struct route *r = route_for(wl, s);
  if (r && r->handler->pointer_enter)
    r->handler->pointer_enter(r->data, s, wl_fixed_to_double(x),
                              wl_fixed_to_double(y));
}

static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial,
                          struct wl_surface *s) {
  struct swell_wl *wl = data;
  struct route *r = route_for(wl, s);
  if (wl->pointer_focus == s)
    wl->pointer_focus = NULL;
  if (r && r->handler->pointer_leave)
    r->handler->pointer_leave(r->data, s);
}

static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time,
                           wl_fixed_t x, wl_fixed_t y) {
  struct swell_wl *wl = data;
  struct route *r = route_for(wl, wl->pointer_focus);
  if (r && r->handler->pointer_motion)
    r->handler->pointer_motion(r->data, wl_fixed_to_double(x),
                               wl_fixed_to_double(y));
}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial,
                           uint32_t time, uint32_t button, uint32_t state) {
  struct swell_wl *wl = data;
  struct route *r = route_for(wl, wl->pointer_focus);
  if (r && r->handler->pointer_button)
    r->handler->pointer_button(r->data, button,
                               state == WL_POINTER_BUTTON_STATE_PRESSED);
}

static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time,
                         uint32_t axis, wl_fixed_t value) {}
static void pointer_frame(void *data, struct wl_pointer *p) {}
static void pointer_axis_source(void *data, struct wl_pointer *p,
                                uint32_t source) {}
static void pointer_axis_stop(void *data, struct wl_pointer *p, uint32_t time,
                              uint32_t axis) {}
static void pointer_axis_discrete(void *data, struct wl_pointer *p,
                                  uint32_t axis, int32_t discrete) {}
static void pointer_axis_value120(void *data, struct wl_pointer *p,
                                  uint32_t axis, int32_t value120) {}
static void pointer_axis_relative_direction(void *data, struct wl_pointer *p,
                                            uint32_t axis, uint32_t dir) {}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
    .axis_value120 = pointer_axis_value120,
    .axis_relative_direction = pointer_axis_relative_direction,
};

/* ---- keyboard ---- */

static void keyboard_keymap(void *data, struct wl_keyboard *k, uint32_t format,
                            int32_t fd, uint32_t size) {
  struct swell_wl *wl = data;
  if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
    close(fd);
    return;
  }
  char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (map == MAP_FAILED)
    return;
  struct xkb_keymap *keymap = xkb_keymap_new_from_string(
      wl->xkb, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
  munmap(map, size);
  if (!keymap)
    return;
  xkb_state_unref(wl->xkb_state);
  xkb_keymap_unref(wl->keymap);
  wl->keymap = keymap;
  wl->xkb_state = xkb_state_new(keymap);
}

static void keyboard_enter(void *data, struct wl_keyboard *k, uint32_t serial,
                           struct wl_surface *s, struct wl_array *keys) {
  struct swell_wl *wl = data;
  wl->keyboard_focus = s;
  struct route *r = route_for(wl, s);
  if (r && r->handler->keyboard_enter)
    r->handler->keyboard_enter(r->data, s);
}

static void keyboard_leave(void *data, struct wl_keyboard *k, uint32_t serial,
                           struct wl_surface *s) {
  struct swell_wl *wl = data;
  struct route *r = route_for(wl, s);
  if (wl->keyboard_focus == s)
    wl->keyboard_focus = NULL;
  if (r && r->handler->keyboard_leave)
    r->handler->keyboard_leave(r->data, s);
}

static void keyboard_key(void *data, struct wl_keyboard *k, uint32_t serial,
                         uint32_t time, uint32_t key, uint32_t state) {
  struct swell_wl *wl = data;
  struct route *r = route_for(wl, wl->keyboard_focus);
  if (r && r->handler->key && wl->xkb_state)
    r->handler->key(r->data, key + 8,
                    state == WL_KEYBOARD_KEY_STATE_PRESSED);
}

static void keyboard_modifiers(void *data, struct wl_keyboard *k,
                               uint32_t serial, uint32_t depressed,
                               uint32_t latched, uint32_t locked,
                               uint32_t group) {
  struct swell_wl *wl = data;
  if (wl->xkb_state)
    xkb_state_update_mask(wl->xkb_state, depressed, latched, locked, 0, 0,
                          group);
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *k,
                                 int32_t rate, int32_t delay) {
  struct swell_wl *wl = data;
  wl->repeat_rate = rate;
  wl->repeat_delay = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

/* ---- seat ---- */

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
  struct swell_wl *wl = data;
  bool has_pointer = caps & WL_SEAT_CAPABILITY_POINTER;
  bool has_keyboard = caps & WL_SEAT_CAPABILITY_KEYBOARD;

  if (has_pointer && !wl->pointer) {
    wl->pointer = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(wl->pointer, &pointer_listener, wl);
    if (wl->cursor_shape)
      wl->cursor_device =
          wp_cursor_shape_manager_v1_get_pointer(wl->cursor_shape, wl->pointer);
  } else if (!has_pointer && wl->pointer) {
    if (wl->cursor_device)
      wp_cursor_shape_device_v1_destroy(wl->cursor_device);
    wl->cursor_device = NULL;
    wl_pointer_release(wl->pointer);
    wl->pointer = NULL;
    wl->pointer_focus = NULL;
  }

  if (has_keyboard && !wl->keyboard) {
    wl->keyboard = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(wl->keyboard, &keyboard_listener, wl);
  } else if (!has_keyboard && wl->keyboard) {
    wl_keyboard_release(wl->keyboard);
    wl->keyboard = NULL;
    wl->keyboard_focus = NULL;
  }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

/* ---- registry ---- */

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t version) {
  struct swell_wl *wl = data;
  if (strcmp(iface, wl_compositor_interface.name) == 0) {
    wl->compositor = wl_registry_bind(reg, name, &wl_compositor_interface,
                                      min_u32(version, 4));
  } else if (strcmp(iface, wl_subcompositor_interface.name) == 0) {
    wl->subcompositor =
        wl_registry_bind(reg, name, &wl_subcompositor_interface, 1);
  } else if (strcmp(iface, wl_shm_interface.name) == 0) {
    wl->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
  } else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
    wl->layer_shell = wl_registry_bind(
        reg, name, &zwlr_layer_shell_v1_interface, min_u32(version, 4));
  } else if (strcmp(iface, wp_viewporter_interface.name) == 0) {
    wl->viewporter = wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
  } else if (strcmp(iface, wp_fractional_scale_manager_v1_interface.name) ==
             0) {
    wl->fractional = wl_registry_bind(
        reg, name, &wp_fractional_scale_manager_v1_interface, 1);
  } else if (strcmp(iface, wp_alpha_modifier_v1_interface.name) == 0) {
    wl->alpha_modifier =
        wl_registry_bind(reg, name, &wp_alpha_modifier_v1_interface, 1);
  } else if (strcmp(iface, wp_cursor_shape_manager_v1_interface.name) == 0) {
    wl->cursor_shape =
        wl_registry_bind(reg, name, &wp_cursor_shape_manager_v1_interface, 1);
  } else if (strcmp(iface, ext_idle_notifier_v1_interface.name) == 0) {
    wl->idle_notifier =
        wl_registry_bind(reg, name, &ext_idle_notifier_v1_interface, 1);
  } else if (strcmp(iface, ext_foreign_toplevel_list_v1_interface.name) == 0) {
    wl->toplevel_list_name = name;
    wl->toplevel_list_version = version;
  } else if (strcmp(iface,
                    ext_foreign_toplevel_image_capture_source_manager_v1_interface
                        .name) == 0) {
    wl->source_manager = wl_registry_bind(
        reg, name,
        &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
  } else if (strcmp(iface, ext_image_copy_capture_manager_v1_interface.name) ==
             0) {
    wl->copy_manager = wl_registry_bind(
        reg, name, &ext_image_copy_capture_manager_v1_interface, 1);
  } else if (strcmp(iface, wl_seat_interface.name) == 0) {
    if (!wl->seat) {
      wl->seat =
          wl_registry_bind(reg, name, &wl_seat_interface, min_u32(version, 7));
      wl_seat_add_listener(wl->seat, &seat_listener, wl);
    }
  } else if (strcmp(iface, wl_output_interface.name) == 0) {
    if (version < 4)
      warn("wl_output v%u lacks output names; multi-output may misbehave",
           version);
    struct swell_output *out = xcalloc(1, sizeof(*out));
    struct output_ctx *c = xcalloc(1, sizeof(*c));
    c->wl = wl;
    c->out = out;
    out->global = name;
    out->scale = 1;
    out->wl_output =
        wl_registry_bind(reg, name, &wl_output_interface, min_u32(version, 4));
    wl_output_add_listener(out->wl_output, &output_listener, c);
    wl_list_insert(wl->outputs.prev, &out->link);
  }
}

static void registry_global_remove(void *data, struct wl_registry *reg,
                                   uint32_t name) {
  struct swell_wl *wl = data;
  struct swell_output *out, *tmp;
  wl_list_for_each_safe(out, tmp, &wl->outputs, link) {
    if (out->global == name)
      output_destroy(wl, out);
  }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

struct ext_foreign_toplevel_list_v1 *wl_bind_toplevel_list(struct swell_wl *wl) {
  if (!wl->toplevel_list_name)
    return NULL;
  return wl_registry_bind(wl->registry, wl->toplevel_list_name,
                          &ext_foreign_toplevel_list_v1_interface, 1);
}

/* ---- loop integration ---- */

static int wl_prepare(void *data) {
  struct swell_wl *wl = data;
  if (wl_display_dispatch_pending(wl->display) < 0 ||
      (wl_display_flush(wl->display) < 0 && errno != EAGAIN)) {
    warn("lost the Wayland connection");
    loop_quit(wl->loop);
  }
  return -1;
}

static void wl_readable(int fd, short revents, void *data) {
  struct swell_wl *wl = data;
  /* Read what is there and dispatch it, never wait: wl_display_dispatch()
   * blocks until the default queue gets an event, and what woke us may be
   * only Mesa's (EGL buffer releases on its own queue), which hung the whole
   * loop until the compositor next sent us something. */
  if ((revents & POLLIN) &&
      (wl_display_prepare_read(wl->display) != 0 ||
       wl_display_read_events(wl->display) >= 0) &&
      wl_display_dispatch_pending(wl->display) >= 0)
    return;
  if (revents & (POLLIN | POLLERR | POLLHUP)) {
    warn("lost the Wayland connection");
    loop_remove_fd(wl->watch);
    wl->watch = NULL;
    loop_quit(wl->loop);
  }
}

struct swell_wl *wl_create(struct loop *loop) {
  struct wl_private *p = xcalloc(1, sizeof(*p));
  struct swell_wl *wl = &p->pub;
  wl_list_init(&wl->outputs);
  wl->repeat_rate = 25;
  wl->repeat_delay = 600;
  wl->display = wl_display_connect(NULL);
  if (!wl->display) {
    warn("cannot connect to the Wayland display");
    free(p);
    return NULL;
  }
  wl->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  wl->registry = wl_display_get_registry(wl->display);
  wl_registry_add_listener(wl->registry, &registry_listener, wl);
  wl_display_roundtrip(wl->display); /* globals */
  wl_display_roundtrip(wl->display); /* output names/scales, seat caps */

  if (!wl->compositor || !wl->shm) {
    warn("compositor lacks wl_compositor/wl_shm");
    wl_destroy(wl);
    return NULL;
  }
  if (!wl->layer_shell)
    warn("compositor lacks wlr-layer-shell; nothing can be shown");
  wl->loop = loop;
  if (loop) {
    wl->watch = loop_add_fd(loop, wl_display_get_fd(wl->display), POLLIN,
                            wl_readable, wl);
    loop_add_prepare(loop, 100, wl_prepare, wl);
  }
  return wl;
}

void wl_destroy(struct swell_wl *wl) {
  if (!wl)
    return;
  struct wl_private *p = priv(wl);
  if (wl->loop) {
    loop_remove_fd(wl->watch);
    loop_remove_prepare(wl->loop, wl_prepare, wl);
  }
  struct swell_output *out, *tmp;
  wl_list_for_each_safe(out, tmp, &wl->outputs, link) output_destroy(wl, out);
  if (wl->cursor_device)
    wp_cursor_shape_device_v1_destroy(wl->cursor_device);
  if (wl->pointer)
    wl_pointer_release(wl->pointer);
  if (wl->keyboard)
    wl_keyboard_release(wl->keyboard);
  if (wl->seat)
    wl_seat_release(wl->seat);
  if (wl->copy_manager)
    ext_image_copy_capture_manager_v1_destroy(wl->copy_manager);
  if (wl->source_manager)
    ext_foreign_toplevel_image_capture_source_manager_v1_destroy(
        wl->source_manager);
  if (wl->idle_notifier)
    ext_idle_notifier_v1_destroy(wl->idle_notifier);
  if (wl->cursor_shape)
    wp_cursor_shape_manager_v1_destroy(wl->cursor_shape);
  if (wl->fractional)
    wp_fractional_scale_manager_v1_destroy(wl->fractional);
  if (wl->alpha_modifier)
    wp_alpha_modifier_v1_destroy(wl->alpha_modifier);
  if (wl->viewporter)
    wp_viewporter_destroy(wl->viewporter);
  if (wl->layer_shell)
    zwlr_layer_shell_v1_destroy(wl->layer_shell);
  if (wl->shm)
    wl_shm_destroy(wl->shm);
  if (wl->subcompositor)
    wl_subcompositor_destroy(wl->subcompositor);
  if (wl->compositor)
    wl_compositor_destroy(wl->compositor);
  xkb_state_unref(wl->xkb_state);
  xkb_keymap_unref(wl->keymap);
  xkb_context_unref(wl->xkb);
  wl_registry_destroy(wl->registry);
  wl_display_flush(wl->display);
  wl_display_disconnect(wl->display);
  free(p->routes);
  free(p->olisteners);
  free(p);
}
