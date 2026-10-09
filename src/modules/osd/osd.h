/* osd: volume/brightness OSD with a rotating Nix snowflake (formerly
 * nixosd). Shared declarations of the module's files. */
#ifndef SWELL_OSD_H
#define SWELL_OSD_H

#include <stdbool.h>
#include <stdint.h>

/* Every tunable; the defaults (osd.c) are nixosd's config.h. All sizes are
 * logical pixels, colours 0xRRGGBBAA. */
struct osd_config {
  char namespace[64]; /* layer-shell namespace */
  /* panel */
  int width, height, corner, top_gap;
  /* ring and logo */
  int ring_cy, ring_radius, ring_width, logo_radius;
  /* text row */
  int text_baseline;
  char font[128];
  int font_size;
  /* colours */
  uint32_t background, border, track, nix_dark, nix_light, ring_dark,
      ring_light, over_dark, over_light, pulse_light, pulse_dark, muted, text;
  double logo_pulse_alpha;
  /* behaviour */
  int max_pct;
  int hide_timeout_ms, tween_ms, slide_ms, pulse_ms;
};

/* The surface includes the gap; the panel slides down into it. */
static inline int osd_surface_height(const struct osd_config *c) {
  return c->height + c->top_gap;
}

enum osd_kind { OSD_VOLUME, OSD_BRIGHTNESS };

struct osd_event {
  enum osd_kind kind;
  int level; /* 0..max_pct */
  bool muted;
};

/* Parse "vol N [mute]", "volume N [mute]", "bri N", "brightness N" or a bare
 * "N" (volume); words are case-insensitive, N is clamped to 0..max_pct. */
bool osd_parse_line(const char *line, int max_pct, struct osd_event *ev);

#endif
