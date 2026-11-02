/* Cairo drawing of one low battery panel frame: a battery outline filled
 * to the level in red, and the level as text, on the OSD's black panel. */
#include "render.h"

#include <math.h>
#include <stdio.h>

#include "draw.h"

static void clamped_rect(cairo_t *cr, double x, double y, double w, double h,
                         double rad) {
  if (rad > w / 2)
    rad = w / 2;
  if (rad > h / 2)
    rad = h / 2;
  rounded_rect(cr, x, y, w, h, rad);
}

/* Battery body at (x, cy - h/2) with its nub on the right, filled to pct. */
static void draw_battery(cairo_t *cr, const struct lowbat_config *c,
                         const struct lowbat_render_state *s, double x,
                         double cy) {
  double bw = c->body_width, bh = c->body_height;
  double y = cy - bh / 2;
  uint32_t edge = col_mix(c->text, c->pulse_light, s->pulse);

  /* Track, then the level, inside a 2 px outline with a 2 px inset. */
  double ix = x + 4, iy = y + 4, iw = bw - 8, ih = bh - 8;
  clamped_rect(cr, ix, iy, iw, ih, 2);
  set_rgba(cr, c->track, 1.0);
  cairo_fill(cr);
  int pct = s->pct < 0 ? 0 : s->pct > 100 ? 100 : s->pct;
  /* Low levels are a sliver; keep at least a visible stub. */
  double fw = fmax(iw * pct / 100.0, pct > 0 ? 3.0 : 0.0);
  if (fw > 0) {
    clamped_rect(cr, ix, iy, fw, ih, 2);
    cairo_pattern_t *g = cairo_pattern_create_linear(ix, 0, ix + iw, 0);
    add_stop(g, 0.0, col_mix(c->pulse_dark, c->pulse_light, s->pulse));
    add_stop(g, 1.0, c->pulse_light);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
  }

  clamped_rect(cr, x + 1, y + 1, bw - 2, bh - 2, 4);
  set_rgba(cr, edge, 1.0);
  cairo_set_line_width(cr, 2);
  cairo_stroke(cr);
  clamped_rect(cr, x + bw, cy - c->nub_height / 2.0, c->nub_width,
               c->nub_height, 1.5);
  cairo_fill(cr);
}

static void draw_panel(cairo_t *cr, const struct lowbat_config *c,
                       const struct lowbat_render_state *s) {
  set_rgba(cr, c->background, 1.0);
  clamped_rect(cr, 0.5, 0.5, c->width - 1, c->height - 1, c->corner);
  cairo_fill_preserve(cr);
  set_rgba(cr, c->border, 1.0);
  cairo_set_line_width(cr, 1);
  cairo_stroke(cr);

  double cy = c->height / 2.0;
  draw_battery(cr, c, s, c->pad, cy);

  char buf[64];
  if (s->critical)
    snprintf(buf, sizeof buf, "%d%%  plug in", s->pct);
  else
    snprintf(buf, sizeof buf, "%d%%  battery low", s->pct);
  cairo_select_font_face(cr, c->font, CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, c->font_size);
  cairo_text_extents_t ext;
  cairo_text_extents(cr, buf, &ext);
  double tx = c->pad + c->body_width + c->nub_width + c->pad;
  set_rgba(cr, col_mix(c->text, c->pulse_light, s->pulse), 1.0);
  cairo_move_to(cr, tx, cy - (ext.y_bearing + ext.height / 2));
  cairo_show_text(cr, buf);
  cairo_new_path(cr);
}

void lowbat_render_surface(cairo_t *cr, const struct lowbat_config *c,
                           const struct lowbat_render_state *s) {
  if (s->slide <= 0.0)
    return;
  cairo_save(cr);
  cairo_translate(cr, 0,
                  c->top_gap - (1.0 - s->slide) * lowbat_surface_height(c));
  draw_panel(cr, c, s);
  cairo_restore(cr);
}
