/* The daemon's single Wayland connection: globals, outputs, and the seat.
 *
 * Pointer and keyboard events are routed to whoever registered a handler
 * for the wl_surface that has pointer/keyboard focus (wl_set_input). The
 * xkb keymap/state are kept up to date here; handlers translate keys with
 * wl->xkb_state. */
#ifndef SWELL_WL_H
#define SWELL_WL_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "loop.h"

struct swell_output {
  struct wl_list link; /* swell_wl.outputs */
  uint32_t global;
  struct wl_output *wl_output;
  char *name;    /* NULL until the compositor sends it */
  int32_t scale; /* integer wl_output scale */
  bool done;     /* initial burst of output events received */
};

struct swell_input {
  void (*pointer_enter)(void *data, struct wl_surface *s, double x, double y);
  void (*pointer_leave)(void *data, struct wl_surface *s);
  void (*pointer_motion)(void *data, double x, double y);
  /* button is a linux/input-event-codes.h BTN_* code */
  void (*pointer_button)(void *data, uint32_t button, bool pressed);
  void (*keyboard_enter)(void *data, struct wl_surface *s);
  void (*keyboard_leave)(void *data, struct wl_surface *s);
  /* keycode is the evdev code + 8 (an xkb keycode) */
  void (*key)(void *data, xkb_keycode_t keycode, bool pressed);
};

typedef void (*wl_output_fn)(void *data, struct swell_output *out,
                             bool added);

struct swell_wl {
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_compositor *compositor;
  struct wl_subcompositor *subcompositor;
  struct wl_shm *shm;
  struct zwlr_layer_shell_v1 *layer_shell;
  struct wp_viewporter *viewporter;
  struct wp_fractional_scale_manager_v1 *fractional;
  struct wp_alpha_modifier_v1 *alpha_modifier; /* NULL if unsupported */
  struct wp_cursor_shape_manager_v1 *cursor_shape;
  struct ext_idle_notifier_v1 *idle_notifier; /* NULL if unsupported */
  struct ext_foreign_toplevel_image_capture_source_manager_v1 *source_manager;
  struct ext_output_image_capture_source_manager_v1 *output_source_manager;
  struct ext_image_copy_capture_manager_v1 *copy_manager;
  /* ext_foreign_toplevel_list_v1 is bound on demand (wl_bind_toplevel_list)
   * because its listener must be attached at bind time. */
  uint32_t toplevel_list_name, toplevel_list_version;

  struct wl_seat *seat;
  struct wl_pointer *pointer;
  struct wl_keyboard *keyboard;
  struct wp_cursor_shape_device_v1 *cursor_device;
  uint32_t pointer_serial; /* of the last pointer enter */
  struct wl_surface *pointer_focus, *keyboard_focus;

  struct xkb_context *xkb;
  struct xkb_keymap *keymap;
  struct xkb_state *xkb_state;
  int repeat_rate, repeat_delay; /* keys/s, ms */

  struct wl_list outputs; /* swell_output.link */

  struct loop *loop;
  struct loop_watch *watch;
};

/* Connect, bind globals and register with the loop. NULL on failure. */
struct swell_wl *wl_create(struct loop *loop);
void wl_destroy(struct swell_wl *wl);

/* Route input for surface to handler (data is passed back). */
void wl_set_input(struct swell_wl *wl, struct wl_surface *surface,
                  const struct swell_input *handler, void *data);
/* Forget surface; call before destroying it. */
void wl_clear_input(struct swell_wl *wl, struct wl_surface *surface);

/* Set the cursor shape (WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_*) on our surface. */
void wl_set_cursor(struct swell_wl *wl, uint32_t shape);

/* Called with added=true once an output is announced (after its name and
 * scale arrived) and added=false right before it goes away. */
void wl_add_output_listener(struct swell_wl *wl, wl_output_fn fn, void *data);
void wl_remove_output_listener(struct swell_wl *wl, wl_output_fn fn,
                               void *data);
struct swell_output *wl_output_find(struct swell_wl *wl, struct wl_output *o);

/* Bind a new ext_foreign_toplevel_list_v1 (NULL if unsupported). The caller
 * must add its listener immediately and destroy it when done. */
struct ext_foreign_toplevel_list_v1 *wl_bind_toplevel_list(struct swell_wl *wl);

#endif
