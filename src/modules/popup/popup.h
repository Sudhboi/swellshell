/* popup: a "hold to peek" panel with a clock and system info that slides
 * up from the bottom (formerly the standalone popup tool). Shared
 * declarations of the module's files. */
#ifndef SWELL_POPUP_H
#define SWELL_POPUP_H

#include <stdint.h>

/* Every tunable; the defaults (popup.c) are popup's config.h and the layout
 * constants of its render.c. Sizes are logical pixels, colours 0xRRGGBBAA. */
struct popup_config {
  char namespace[64]; /* layer-shell namespace */
  /* panel */
  int width, height, pad, bottom_gap;
  /* fonts */
  char font[128];
  int font_clock, font_seconds, font_date, font_row, font_ws;
  /* colours */
  uint32_t background, border, track, bar_dark, bar_light, text, dim, muted,
      ws_focus, warn, hold;
  int battery_low_pct;
  /* timing, ms */
  int refresh_ms, command_timeout_ms, pop_ms, close_ms, expand_delay_ms, expand_ms;
  /* layout */
  double gap_date, gap_sep, gap_cols, gap_bar, bar_h, col_inset, gap_rows,
      row_step, sec_gap, icon_gap, mini_font, mini_gap;
  double ws_h, ws_min_w, ws_text_pad, ws_gap, gap_ws, wsc_min_w, wsc_max_w,
      wsc_inset, wsc_arrow, ws_arrow_w, hold_h;
};

/* The layer surface is anchored to the bottom and includes the gap; the
 * panel slides up from below the edge into it. */
static inline int popup_surface_height(const struct popup_config *c) {
  return c->height + c->bottom_gap;
}

#endif
