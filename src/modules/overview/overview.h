/* overview: a GNOME-style workspace overview with live window thumbnails
 * (ported from swayverview). Shared state of the module's files. */
#ifndef SWELL_OVERVIEW_H
#define SWELL_OVERVIEW_H

#include <cairo.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "loop.h"
#include "swell.h"
#include "wl.h"

/* Every tunable; see the field table in overview.c for docs. Applied on the
 * next open. */
struct overview_config {
  bool capture;            /* live thumbnails at all */
  bool live;               /* keep updating thumbnails after the first frame */
  int fps;                 /* thumbnail update rate when live */
  bool new_workspace_slot; /* show the "New workspace" slot */
  char font[128];

  double layout_pad, layout_gap, label_height, caption_height;
  double tile_radius, tile_inset, window_radius, window_gap;
  double spread_gap;
  double drag_threshold;
  int zoom_delay_ms, zoom_anim_ms;
  int open_anim_ms;
  bool backdrop; /* the wallpaper's map under the background */
  double zoom_max_width, zoom_max_height, zoom_min_gain, zoom_margin;

  uint32_t background, background_plain, tile, tile_border, tile_visible, accent, urgent;
  uint32_t accent_a, accent_mid, accent_b; /* the selection's gradient */
  uint32_t window, window_border, hover, text, text_dim;
  uint32_t focused_border, drop_fill, new_slot_fill, new_slot_drop_fill;
  uint32_t shadow, tile_shadow, zoom_shadow, zoom_title_band;
  double drag_source_alpha, drag_ghost_alpha;
};

struct rect {
  int x, y, w, h;
};

/* A box on an output surface, in surface-local logical pixels. */
struct box {
  double x, y, w, h;
};

struct capture;
struct output;
struct workspace;

struct window {
  long con_id;
  char *title, *app_id, *ident; /* ident = foreign toplevel identifier */
  struct rect rect;    /* whole container, relative to its workspace */
  struct rect content; /* client area, relative to its workspace */
  bool floating, visible, focused, urgent, fullscreen;
  struct workspace *ws;
  struct capture *cap; /* NULL when no thumbnail is available */

  struct box box;  /* laid-out container box */
  struct box cbox; /* laid-out content box */
};

struct workspace {
  long id; /* sway con_id (stable across renames), -1 for the placeholder */
  char *name;
  int num; /* -1 when the name is not numeric */
  bool focused, visible, urgent;
  bool placeholder; /* the "new workspace" slot at the end of each output */
  char *output_name;
  struct rect rect;        /* absolute, logical */
  struct window **windows; /* in drawing order (bottom first) */
  int nwindows;

  struct output *output;
  struct box box;
  struct box label;
  struct box inner; /* where its windows are laid out, inside box */
};

struct sway_output_info {
  char *name;
  struct rect rect;
};

/* Snapshot of sway's tree, rebuilt after every sway event. */
struct model {
  struct sway_output_info *outputs;
  int noutputs;
  struct workspace **workspaces;
  int nworkspaces;
  struct window **windows;
  int nwindows;
  long focused_con; /* -1 if no window has focus */
};

/* Selection target: a window, or a workspace with no window selected. */
struct target {
  struct window *win;
  struct workspace *ws;
};

/* One overlay per output while open (or a bare entry in dump mode). */
struct output {
  struct wl_list link; /* ov.outputs */
  struct ov *ov;
  struct swell_output *so; /* the core's output (name, integer scale) */
  char *name;

  struct wl_surface *surface;
  struct zwlr_layer_surface_v1 *layer;
  struct wp_viewport *viewport;
  struct wp_fractional_scale_v1 *fractional;
  uint32_t scale120; /* preferred fractional scale, 0 if unknown */

  int width, height; /* logical size */
  bool configured, dirty;
  struct wl_callback *frame_cb;
  struct buffer *buffers[2];
  struct buffer *half_buffers[2]; /* the zoom's half-size frames */
  bool warm; /* the names drawn once at the zoom's size (see render.c) */
  /* The map and scrim, drawn once per open at buffer resolution: on a
   * subsurface below, which the compositor fades (alpha-modifier), else
   * copied into every frame from this cache. */
  struct wl_surface *bg_surface;
  struct wl_subsurface *bg_sub;
  struct wp_viewport *bg_viewport;
  struct wp_alpha_modifier_surface_v1 *bg_alpha;
  struct buffer *bg_buffer;
  uint32_t bg_multiplier; /* last one set */
  cairo_surface_t *background;
  bool bg_ready; /* drawn at the current size: the zoom may start */

  /* layout, rebuilt by ov_layout_all() */
  struct workspace **workspaces;
  int nworkspaces;
  int gx, gy; /* position in sway's global layout */
};

struct ov {
  struct overview_config cfg; /* copied at open */
  struct swell *sw;
  struct swell_wl *wl;
  bool open;
  bool close_pending; /* close at the next prepare (from input handlers) */
  bool dump;          /* rendering for `overview dump` */
  const char *dump_path, *dump_output;

  struct ext_foreign_toplevel_list_v1 *toplevel_list;
  struct wl_list outputs;   /* struct output.link */
  struct wl_list toplevels; /* struct toplevel.link */
  struct wl_list captures;  /* struct capture.link */

  struct model model;
  bool model_dirty;

  struct loop_timer *repeat;
  xkb_keysym_t repeat_sym;

  /* pointer / interaction state */
  struct output *ptr_output;
  double px, py;
  struct target hover;
  struct target selected;
  /* enlarged preview of the hovered window */
  struct {
    struct window *win;  /* previewed window (growing, shown or shrinking) */
    bool closing;        /* shrinking back into its tile */
    int64_t hover_since; /* when ov->hover.win last changed */
    int64_t start;       /* when the current animation started */
    struct box from, to; /* animation endpoints, surface-local */
    struct box big;      /* fully grown box */
  } zoom;

  /* open/close: the shown workspace zooms out of the screen, back in */
  struct {
    int64_t opened; /* when open began (the zoom waits for thumbnails) */
    int64_t start;  /* when the current direction began, 0: not yet */
    double from;    /* progress 0..1 at start */
    int dir;        /* 1 opening, -1 closing, 0 still */
    /* the workspace a close zooms into, else each output's visible one */
    bool into_set, into_placeholder;
    long into_id;
    bool close_emitted; /* overview.close went out when the close began */
  } anim;

  struct {
    bool pressed, active;
    struct target press;
    struct output *output;
    double start_x, start_y;
    double grab_dx, grab_dy; /* pointer offset inside the dragged box */
  } drag;
};

/* Mark every output as needing a repaint. */
void ov_redraw(struct ov *ov);
void ov_output_redraw(struct output *out);
/* Close at the next loop iteration (safe from input handlers). */
void ov_request_close(struct ov *ov);
/* The open/close animation at now: 0 closed (the screen as it is) .. 1
 * open. */
double ov_anim_value(struct ov *ov, int64_t now);
/* The workspace that zooms on out (NULL: none). */
struct workspace *ov_anim_hero(struct ov *ov, struct output *out);

#endif
