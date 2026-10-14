/* wsosd: a small panel that drops down from the top when the focused
 * workspace changes, with one dot per workspace and a wide bar on the
 * focused one. Shared declarations of the module's files. */
#ifndef SWELL_WSOSD_H
#define SWELL_WSOSD_H

#include <stdint.h>

/* Every tunable. Sizes are logical pixels, colours 0xRRGGBBAA. */
struct wsosd_config {
  char namespace[64]; /* layer-shell namespace */
  /* panel */
  int height, top_gap, corner, pad;
  /* row */
  int dot, dot_gap, active_width, max_dots;
  /* colours */
  uint32_t background, border, dot_color, active_dark, active_light;
  /* behaviour */
  int hide_timeout_ms, slide_ms, move_ms;
};

/* Width of the panel for n workspaces: n - 1 dots, the focused bar and
 * the gaps between them, padded. */
static inline double wsosd_panel_width(const struct wsosd_config *c,
                                       double n) {
  if (n < 1)
    n = 1;
  return 2.0 * c->pad + (n - 1) * (c->dot + c->dot_gap) + c->active_width;
}

/* The surface includes the gap; the panel slides down into it. */
static inline int wsosd_surface_height(const struct wsosd_config *c) {
  return c->height + c->top_gap;
}

#endif
