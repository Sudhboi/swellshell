/* Cairo drawing of the wallpaper: a port of make_black_bar_nixos.py. */
#include <math.h>

#include "config.h"
#include "nixlogo.h"
#include "wallpaper.h"

double wp_pulse_period(const struct wallpaper_config *c, double percent) {
  return fmax(c->pulse_period_s * percent / 50.0, c->pulse_period_min_s);
}

/* The script's bar_level() with the time measured as a fraction of the
 * period: its 50 % shape (Gaussian of width pulse_sigma_s, flat for
 * pulse_hold_s around the peak) stretched to the period. */
double wp_bar_level(const struct wallpaper_config *c, double phase) {
  double d = fabs(phase - 0.5) * c->pulse_period_s;
  d = fmax(d - c->pulse_hold_s / 2.0, 0.0);
  double g = d / c->pulse_sigma_s;
  return c->bar_min + (1.0 - c->bar_min) * exp(-0.5 * g * g);
}

/* Bar outline box (with the gap and the outline) in buffer px. */
static void bar_box(const struct wallpaper_config *c, int pw, double *x,
                    double *y, double *w, double *h) {
  double k = pw / WP_REF_WIDTH;
  double pad = c->bar_gap + c->bar_border;
  *w = (c->bar_width + 2 * pad) * k;
  *h = (c->bar_height + 2 * pad) * k;
  *x = pw / 2.0 - *w / 2.0;
  *y = (c->bar_top - pad) * k;
}

static struct wp_rect clamp_rect(double x0, double y0, double x1, double y1,
                                 int pw, int ph) {
  int ix0 = (int)fmax(floor(x0) - 1, 0), iy0 = (int)fmax(floor(y0) - 1, 0);
  int ix1 = (int)fmin(ceil(x1) + 1, pw), iy1 = (int)fmin(ceil(y1) + 1, ph);
  return (struct wp_rect){ix0, iy0, ix1 > ix0 ? ix1 - ix0 : 0,
                          iy1 > iy0 ? iy1 - iy0 : 0};
}

void wp_regions(const struct wallpaper_config *c, int pw, int ph,
                struct wp_rect out[2]) {
  double x, y, w, h;
  bar_box(c, pw, &x, &y, &w, &h);
  out[0] = clamp_rect(x, y, x + w, y + h, pw, ph);
  double r = c->logo_radius * pw / WP_REF_WIDTH + 1;
  out[1] = clamp_rect(pw / 2.0 - r, ph / 2.0 - r, pw / 2.0 + r, ph / 2.0 + r,
                      pw, ph);
}

static void pill(cairo_t *cr, double x, double y, double w, double h) {
  double r = fmin(w, h) / 2.0;
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
  cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
  cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
  cairo_close_path(cr);
}

static void set_rgba(cairo_t *cr, uint32_t c, double alpha) {
  cairo_set_source_rgba(cr, col_r(c), col_g(c), col_b(c), col_a(c) * alpha);
}

/* render_bar_mask(): a pill outline around the whole bar and, inside the
 * gap, a pill filled to percent, all at the pulse brightness. */
static void draw_bar(cairo_t *cr, const struct wallpaper_config *c, int pw,
                     const struct wp_frame *f) {
  double k = pw / WP_REF_WIDTH;
  double x, y, w, h;
  bar_box(c, pw, &x, &y, &w, &h);
  double b = c->bar_border * k, pad = (c->bar_gap + c->bar_border) * k;
  pill(cr, x, y, w, h);
  pill(cr, x + b, y + b, w - 2 * b, h - 2 * b);
  double fw = c->bar_width * k * fmin(fmax(f->percent, 0), 100) / 100.0;
  if (fw > 0)
    pill(cr, x + pad, y + pad, fw, c->bar_height * k);
  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
  set_rgba(cr, c->bar, f->level);
  cairo_fill(cr);
}

void wp_render(cairo_t *cr, const struct wallpaper_config *c, int pw, int ph,
               const struct wp_frame *f, bool full) {
  cairo_save(cr);
  if (!full) {
    struct wp_rect r[2];
    wp_regions(c, pw, ph, r);
    for (int i = 0; i < 2; ++i)
      cairo_rectangle(cr, r[i].x, r[i].y, r[i].w, r[i].h);
    cairo_clip(cr);
  }
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  set_rgba(cr, c->background, 1.0);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

  draw_bar(cr, c, pw, f);
  nixlogo_draw(cr, pw / 2.0, ph / 2.0, c->logo_radius * pw / WP_REF_WIDTH,
               f->angle, c->logo, c->logo_alt, 1.0);
  cairo_restore(cr);
}
