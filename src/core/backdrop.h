/* Whoever can paint the wallpaper's look under an overlay that hides the
 * windows (the overview), since the real wallpaper is behind them. */
#ifndef SWELL_BACKDROP_H
#define SWELL_BACKDROP_H

#include <cairo.h>
#include <stdbool.h>

struct backdrop_source {
  /* Paint over logical 0, 0, w, h of cr, which maps one logical px to
   * scale buffer px. False: nothing painted (no such look right now). */
  bool (*paint)(cairo_t *cr, int w, int h, double scale);
};
void backdrop_set_source(const struct backdrop_source *s); /* NULL: none */
const struct backdrop_source *backdrop_get_source(void);

#endif
