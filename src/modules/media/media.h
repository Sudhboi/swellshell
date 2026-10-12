/* media: what's playing, in a panel that drops down from the top while the
 * popup shows its details. Shared declarations of the module's files. */
#ifndef SWELL_MEDIA_H
#define SWELL_MEDIA_H

#include <stdbool.h>
#include <stdint.h>

/* Every tunable; the defaults are in media.c. Sizes are logical pixels,
 * colours 0xRRGGBBAA. */
struct media_config {
  char namespace[64]; /* layer-shell namespace */
  /* panel */
  int width, height, top_gap, corner, pad;
  bool art;     /* show the cover */
  int art_size; /* its square's side */
  /* fonts */
  char font[128];
  int font_player, font_title, font_row, font_time;
  /* colours */
  uint32_t background, border, track, bar_dark, bar_light, text, dim, muted;
  /* layout */
  double gap_title, gap_artist, gap_time, bar_h;
  /* player */
  char player[128]; /* playerctl -p, "": playerctl's pick */
  int interval_ms, command_timeout_ms;
  /* timing, ms */
  int slide_ms, close_ms, fade_ms, chase_ms, status_ms;
  int flash_ms; /* shown this long after `swell media play-pause` */
};

/* The layer surface is anchored to the top and includes the gap; the panel
 * drops down from above the edge into it. */
static inline int media_surface_height(const struct media_config *c) {
  return c->height + c->top_gap;
}

#endif
