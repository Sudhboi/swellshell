/* Cairo drawing of the popup. */
#ifndef SWELL_POPUP_RENDER_H
#define SWELL_POPUP_RENDER_H

#include <cairo.h>

#include "info.h"
#include "popup.h"

/* Draws in logical width x popup_surface_height() coordinates, with the
 * panel resting on y = height; the caller has already scaled and cleared
 * the surface. pop (0..1) slides the panel up from below the surface bottom,
 * expand (0..1) grows the clock-only panel upward to the full details panel,
 * media (0..1) opens the media row (and the panel with it); it is normally
 * in->has_media, tweened by the caller when that changes. */
void popup_render(cairo_t *cr, const struct popup_config *cfg,
                  const struct info *in, double pop, double expand,
                  double media);

#endif
