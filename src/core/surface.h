/* A single layer-shell surface that is created on show and destroyed on
 * hide, with double-buffered shm, frame-callback pacing and fractional
 * scaling (wp_fractional_scale_v1 + viewporter, falling back to the integer
 * output scale). Used by the osd and the popup. */
#ifndef SWELL_SURFACE_H
#define SWELL_SURFACE_H

#include <cairo.h>
#include <stdbool.h>
#include <stdint.h>

#include "wl.h"

/* Draw one frame in logical coordinates; the surface is already cleared and
 * scaled. Return true to be called again on the next frame callback. */
typedef bool (*layer_draw_fn)(cairo_t *cr, void *data);

struct layer_opts {
  const char *ns;        /* layer-shell namespace */
  int width, height;     /* logical size */
  uint32_t layer;        /* ZWLR_LAYER_SHELL_V1_LAYER_* */
  uint32_t anchor;       /* ZWLR_LAYER_SURFACE_V1_ANCHOR_* bits */
  int32_t exclusive_zone;
  int32_t margin_top, margin_right, margin_bottom, margin_left;
  bool click_through;    /* empty input region */
  bool keyboard;         /* exclusive keyboard interactivity */
};

struct layer_surface;

struct layer_surface *layer_surface_create(struct swell_wl *wl,
                                           layer_draw_fn draw, void *data);
void layer_surface_destroy(struct layer_surface *s);

/* Create and map the surface with opts (copied). No-op if shown. */
void layer_surface_show(struct layer_surface *s, const struct layer_opts *opts);
/* Destroy the wl_surface and its buffers. */
void layer_surface_hide(struct layer_surface *s);
bool layer_surface_shown(struct layer_surface *s);
/* Schedule a redraw (immediate unless a frame callback is pending). */
void layer_surface_redraw(struct layer_surface *s);
/* The current buffer scale (fractional when supported). */
double layer_surface_scale(struct layer_surface *s);

#endif
