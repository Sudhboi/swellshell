/* Cairo drawing of one workspace OSD frame. */
#include "render.h"

#include <math.h>
#include <stdint.h>

#include "draw.h"

/* A rounded rectangle, the radius held to half the smaller side. */
static void clamped_rect(cairo_t *cr, double x, double y, double w, double h,
                         double rad) {
  if (rad > w / 2)
    rad = w / 2;
  if (rad > h / 2)
    rad = h / 2;
  rounded_rect(cr, x, y, w, h, rad);
}

static double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

/* Slot k: how much of it exists (the last one shrinks while the count
 * tweens) and how much of the focused bar it holds. */
static double slot_size(const struct wsosd_render_state *s, int k) {
  return clamp01(s->count - k);
}
static double slot_active(const struct wsosd_render_state *s, int k) {
  return clamp01(1 - fabs(k - s->pos));
}
static double slot_width(const struct wsosd_config *c,
                         const struct wsosd_render_state *s, int k) {
  return (c->dot + (c->active_width - c->dot) * slot_active(s, k)) *
         slot_size(s, k);
}

static void draw_row(cairo_t *cr, const struct wsosd_config *c,
                     const struct wsosd_render_state *s, double x0) {
  int slots = (int)ceil(s->count);
  double row = 0;
  for (int k = 0; k < slots; ++k)
    row += slot_width(c, s, k) + (k ? c->dot_gap * slot_size(s, k) : 0);

  double x = x0 - row / 2, cy = c->height / 2.0;
  int from = (int)floor(s->pos);
  double bar_x = x;
  for (int k = 0; k < slots; ++k) {
    double size = slot_size(s, k);
    double w = slot_width(c, s, k), h = c->dot * size;
    if (k)
      x += c->dot_gap * size;
    if (k == from)
      bar_x = x + (s->pos - from) * (w + c->dot_gap);
    if (w > 0 && h > 0) {
      clamped_rect(cr, x, cy - h / 2, w, h, h / 2);
      set_rgba(cr, c->dot_color, 1.0);
      cairo_fill(cr);
    }
    x += w;
  }

  /* One bar over the row, sliding from slot to slot while the slots under
   * it stretch and shrink. Light-to-dark gradient, like the volume ring. */
  double bw = c->active_width, bh = c->dot;
  clamped_rect(cr, bar_x, cy - bh / 2, bw, bh, bh / 2);
  cairo_pattern_t *g = cairo_pattern_create_linear(bar_x, 0, bar_x + bw, 0);
  add_stop(g, 0.0, c->active_light);
  add_stop(g, 1.0, c->active_dark);
  cairo_set_source(cr, g);
  cairo_fill(cr);
  cairo_pattern_destroy(g);
}

static void draw_panel(cairo_t *cr, const struct wsosd_config *c,
                       const struct wsosd_render_state *s) {
  double pw = round(wsosd_panel_width(c, s->count));
  double x = round((s->width - pw) / 2);

  set_rgba(cr, c->background, 1.0);
  clamped_rect(cr, x + 0.5, 0.5, pw - 1, c->height - 1, c->corner);
  cairo_fill_preserve(cr);
  set_rgba(cr, c->border, 1.0);
  cairo_set_line_width(cr, 1);
  cairo_stroke(cr);

  draw_row(cr, c, s, x + pw / 2);
}

void wsosd_render_surface(cairo_t *cr, const struct wsosd_config *c,
                          const struct wsosd_render_state *s) {
  if (s->slide <= 0.0)
    return;
  cairo_save(cr);
  /* Slide the panel down from above the top edge into the gap below it. */
  cairo_translate(cr, 0,
                  c->top_gap - (1.0 - s->slide) * wsosd_surface_height(c));
  draw_panel(cr, c, s);
  cairo_restore(cr);
}
