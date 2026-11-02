/* lowbat: a panel that drops down from the top when the battery runs low,
 * and stays down while it is critical. Shared declarations. */
#ifndef SWELL_LOWBAT_H
#define SWELL_LOWBAT_H

#include <stdint.h>

/* Every tunable. Sizes are logical pixels, colours 0xRRGGBBAA. */
struct lowbat_config {
  char namespace[64]; /* layer-shell namespace */
  /* when */
  int low_pct, critical_pct, interval_s;
  /* panel */
  int width, height, top_gap, corner, pad;
  /* battery glyph */
  int body_width, body_height, nub_width, nub_height;
  char font[128];
  int font_size;
  /* colours */
  uint32_t background, border, track, text, pulse_light, pulse_dark;
  /* behaviour */
  int low_timeout_ms, slide_ms, pulse_ms, pulse_count;
};

/* The surface includes the gap; the panel slides down into it. */
static inline int lowbat_surface_height(const struct lowbat_config *c) {
  return c->height + c->top_gap;
}

#endif
