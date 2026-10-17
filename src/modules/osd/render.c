/* Cairo drawing of one OSD frame. */
#include "render.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "draw.h"
#include "nixlogo.h"

/* Nerd Font icons, UTF-8 */
static const char ICON_VOL_HIGH[] = "\U000F057E";
static const char ICON_VOL_MED[] = "\U000F0580";
static const char ICON_VOL_LOW[] = "\U000F057F";
static const char ICON_MUTED[] = "\U000F0581";
static const char ICON_BRIGHT[] = "\U000F00E0";

static const char *pick_icon(const struct osd_render_state *s) {
  if (s->kind == OSD_BRIGHTNESS)
    return ICON_BRIGHT;
  if (s->muted)
    return ICON_MUTED;
  if (s->pct >= 66)
    return ICON_VOL_HIGH;
  if (s->pct >= 33)
    return ICON_VOL_MED;
  return ICON_VOL_LOW;
}

/* Stroke one arc of the ring from the top, 0..1 of a full turn. */
static void stroke_arc(cairo_t *cr, const struct osd_config *c, double frac,
                       bool muted, uint32_t light, uint32_t dark) {
  const double cx = c->width / 2.0, cy = c->ring_cy, a0 = -M_PI / 2;
  const double r = c->ring_radius;

  cairo_arc(cr, cx, cy, r, a0, a0 + frac * 2 * M_PI);
  if (muted) {
    set_rgba(cr, c->muted, 1.0);
    cairo_stroke(cr);
    return;
  }
  /* Diagonal light-to-dark gradient across the ring */
  cairo_pattern_t *g =
      cairo_pattern_create_linear(cx - r, cy - r, cx + r, cy + r);
  add_stop(g, 0.0, light);
  add_stop(g, 1.0, dark);
  cairo_set_source(cr, g);
  cairo_stroke(cr);
  cairo_pattern_destroy(g);
}

static void draw_ring(cairo_t *cr, const struct osd_config *c,
                      const struct osd_render_state *s) {
  bool muted = s->kind == OSD_VOLUME && s->muted;

  cairo_set_line_width(cr, c->ring_width);
  set_rgba(cr, c->track, 1.0);
  cairo_arc(cr, c->width / 2.0, c->ring_cy, c->ring_radius, 0, 2 * M_PI);
  cairo_stroke(cr);

  if (s->level <= 0.001)
    return;

  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  double lap = s->level > 1.0 ? 1.0 : s->level;
  stroke_arc(cr, c, lap, muted, c->ring_light, c->ring_dark);

  /* Above 100%: a brighter second lap over the first. */
  double over = s->level - 1.0;
  if (over > 0.001)
    stroke_arc(cr, c, over > 1.0 ? 1.0 : over, muted,
               col_mix(c->over_light, c->pulse_light, s->pulse),
               col_mix(c->over_dark, c->pulse_dark, s->pulse));
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
}

static void draw_text(cairo_t *cr, const struct osd_config *c,
                      const struct osd_render_state *s) {
  char buf[64];
  bool muted = s->kind == OSD_VOLUME && s->muted;

  if (muted)
    snprintf(buf, sizeof buf, "%s  muted", pick_icon(s));
  else
    snprintf(buf, sizeof buf, "%s  %d%%", pick_icon(s), s->pct);

  cairo_select_font_face(cr, c->font, CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, c->font_size);

  cairo_text_extents_t ext;
  cairo_text_extents(cr, buf, &ext);
  double x = (c->width - ext.x_advance) / 2.0;

  set_rgba(cr, col_mix(c->text, c->pulse_light, s->pulse), 1.0);
  cairo_move_to(cr, x, c->text_baseline);
  cairo_show_text(cr, buf);
  cairo_new_path(cr);
}

void osd_render_frame(cairo_t *cr, const struct osd_config *c,
                      const struct osd_render_state *s) {
  if (s->alpha <= 0.0)
    return;

  cairo_save(cr);
  cairo_push_group(cr);

  set_rgba(cr, c->background, 1.0);
  rounded_rect(cr, 0.5, 0.5, c->width - 1, c->height - 1, c->corner);
  cairo_fill_preserve(cr);
  set_rgba(cr, c->border, 1.0);
  cairo_set_line_width(cr, 1);
  cairo_stroke(cr);

  draw_ring(cr, c, s);
  cairo_push_group(cr);
  bool grey = s->kind == OSD_VOLUME && s->muted;
  /* Muted: both shades in c->muted, the light lambdas at 75%. */
  uint32_t dark = grey ? c->muted : c->nix_dark;
  uint32_t light = grey ? (c->muted & ~0xffu) |
                              (uint32_t)lround((c->muted & 0xff) * 0.75)
                        : c->nix_light;
  nixlogo_draw(cr, c->width / 2.0, c->ring_cy, c->logo_radius,
               s->level * 2 * M_PI, dark, light, 1.0);
  cairo_pattern_t *logo = cairo_pop_group(cr);
  cairo_set_source(cr, logo);
  cairo_paint(cr);
  /* Above 100%: tint the logo red, masked to its shape so the
   * light/dark facets still show through. */
  if (s->pulse > 0.0) {
    set_rgba(cr, c->pulse_light, s->pulse * c->logo_pulse_alpha);
    cairo_mask(cr, logo);
  }
  cairo_pattern_destroy(logo);
  draw_text(cr, c, s);

  cairo_pop_group_to_source(cr);
  cairo_paint_with_alpha(cr, s->alpha);
  cairo_restore(cr);
}

void osd_render_surface(cairo_t *cr, const struct osd_config *c,
                        const struct osd_render_state *s) {
  cairo_save(cr);
  /* Slide the panel down from above the top edge into the gap below it. */
  cairo_translate(cr, 0, c->top_gap - (1.0 - s->slide) * osd_surface_height(c));
  osd_render_frame(cr, c, s);
  cairo_restore(cr);
}
