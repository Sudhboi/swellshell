/* overview: a GNOME-style workspace overview with live window thumbnails
 * (ported from swayverview). Shared state of the module's files. */
#ifndef SWELL_OVERVIEW_H
#define SWELL_OVERVIEW_H

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
  double spread_gap;
  double drag_threshold;
  int zoom_delay_ms, zoom_anim_ms;
  double zoom_max_width, zoom_max_height, zoom_min_gain, zoom_margin;

  uint32_t background, tile, tile_border, tile_visible, accent, urgent;
  uint32_t window, window_border, hover, text, text_dim;
  uint32_t focused_border, drop_fill, new_slot_fill, new_slot_drop_fill;
  uint32_t shadow, zoom_shadow, zoom_title_band;
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

#endif
