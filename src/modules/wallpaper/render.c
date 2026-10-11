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

/* Speed lines sit on rings this far out (x logo radius). */
static const double streak_rings[] = {1.08, 1.16, 1.24};
#define STREAK_OUTER 1.24

void wp_regions(const struct wallpaper_config *c, int pw, int ph,
                struct wp_rect out[2]) {
  double x, y, w, h;
  bar_box(c, pw, &x, &y, &w, &h);
  out[0] = clamp_rect(x, y, x + w, y + h, pw, ph);
  /* The bloom and the speed lines reach past the logo. */
  double k = pw / WP_REF_WIDTH;
  double r = c->logo_radius * k * fmax(c->bloom_radius, STREAK_OUTER) +
             c->streak_width * k + 1;
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

/* a + (b - a) * t per channel, t in 0..1. */
static uint32_t mix(uint32_t a, uint32_t b, double t) {
  uint32_t out = 0;
  for (int sh = 0; sh < 32; sh += 8) {
    double ca = (a >> sh) & 0xff, cb = (b >> sh) & 0xff;
    out |= (uint32_t)lround(ca + (cb - ca) * t) << sh;
  }
  return out;
}

static void set_rgba(cairo_t *cr, uint32_t c, double alpha) {
  cairo_set_source_rgba(cr, col_r(c), col_g(c), col_b(c), col_a(c) * alpha);
}

/* A soft glow behind the logo, growing with the overspeed. */
static void draw_bloom(cairo_t *cr, const struct wallpaper_config *c,
                       double cx, double cy, double r, double over) {
  double outer = r * (1 + (c->bloom_radius - 1) * over);
  double a = c->bloom_alpha * over * col_a(c->bloom);
  cairo_pattern_t *p = cairo_pattern_create_radial(cx, cy, 0, cx, cy, outer);
  cairo_pattern_add_color_stop_rgba(p, 0, col_r(c->bloom), col_g(c->bloom),
                                    col_b(c->bloom), a);
  cairo_pattern_add_color_stop_rgba(p, 0.6, col_r(c->bloom), col_g(c->bloom),
                                    col_b(c->bloom), a * 0.6);
  cairo_pattern_add_color_stop_rgba(p, 1, col_r(c->bloom), col_g(c->bloom),
                                    col_b(c->bloom), 0);
  cairo_set_source(cr, p);
  cairo_arc(cr, cx, cy, outer, 0, 2 * M_PI);
  cairo_fill(cr);
  cairo_pattern_destroy(p);
}

/* The bloom only changes with the overspeed, but its gradient is costly
 * per pixel, so it is drawn over the background into a tile once per step
 * of the overspeed and copied in from there. */
#define BLOOM_STEPS 64

static struct {
  cairo_surface_t *tile;
  int x, y; /* buffer position */
  int pw, ph, step;
  uint32_t background, bloom;
  double logo_radius, bloom_radius, bloom_alpha;
} bloom_cache;

void wp_render_finish(void) {
  if (bloom_cache.tile)
    cairo_surface_destroy(bloom_cache.tile);
  bloom_cache.tile = NULL;
}

static void paint_bloom(cairo_t *cr, const struct wallpaper_config *c,
                        int pw, int ph, double r, double over) {
  int step = (int)lround(fmin(over, 1) * BLOOM_STEPS);
  if (step <= 0)
    return;
  double cx = pw / 2.0, cy = ph / 2.0;
  if (!bloom_cache.tile || bloom_cache.pw != pw || bloom_cache.ph != ph ||
      bloom_cache.step != step || bloom_cache.background != c->background ||
      bloom_cache.bloom != c->bloom ||
      bloom_cache.logo_radius != c->logo_radius ||
      bloom_cache.bloom_radius != c->bloom_radius ||
      bloom_cache.bloom_alpha != c->bloom_alpha) {
    wp_render_finish();
    double outer = r * (1 + (c->bloom_radius - 1) * step / BLOOM_STEPS);
    int x0 = (int)floor(cx - outer) - 1, y0 = (int)floor(cy - outer) - 1;
    int size = (int)ceil(cx + outer) + 1 - x0;
    bloom_cache.tile =
        cairo_image_surface_create(CAIRO_FORMAT_RGB24, size, size);
    cairo_t *tc = cairo_create(bloom_cache.tile);
    set_rgba(tc, c->background, 1.0);
    cairo_set_operator(tc, CAIRO_OPERATOR_SOURCE);
    cairo_paint(tc);
    cairo_set_operator(tc, CAIRO_OPERATOR_OVER);
    draw_bloom(tc, c, cx - x0, cy - y0, r, (double)step / BLOOM_STEPS);
    cairo_destroy(tc);
    bloom_cache.x = x0;
    bloom_cache.y = y0;
    bloom_cache.pw = pw;
    bloom_cache.ph = ph;
    bloom_cache.step = step;
    bloom_cache.background = c->background;
    bloom_cache.bloom = c->bloom;
    bloom_cache.logo_radius = c->logo_radius;
    bloom_cache.bloom_radius = c->bloom_radius;
    bloom_cache.bloom_alpha = c->bloom_alpha;
  }
  cairo_save(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface(cr, bloom_cache.tile, bloom_cache.x,
                           bloom_cache.y);
  int size = cairo_image_surface_get_width(bloom_cache.tile);
  cairo_rectangle(cr, bloom_cache.x, bloom_cache.y, size, size);
  cairo_fill(cr);
  cairo_restore(cr);
}

/* Arcs circling the logo, turning with it: bright at the leading end and
 * fading along a tail that lengthens with the overspeed. */
static void draw_streaks(cairo_t *cr, const struct wallpaper_config *c,
                         double cx, double cy, double r, double k,
                         const struct wp_frame *f) {
  int n = c->streak_count, segs = 8;
  double len = c->streak_length * M_PI / 180 * f->over;
  if (n <= 0 || len <= 0 || c->streak_width <= 0)
    return;
  cairo_set_line_width(cr, c->streak_width * k);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
  for (int i = 0; i < n; ++i) {
    /* Spread evenly, nudged off a regular pattern by the ring. */
    int ring = (i * 2) % 3;
    double rr = r * streak_rings[ring];
    double head = f->angle + 2 * M_PI * i / n + ring * 0.21;
    double l = len * (0.7 + 0.15 * ring);
    for (int s = 0; s < segs; ++s) {
      /* Segment s spans from s/segs to (s+1)/segs of the way back. */
      double a0 = head - f->dir * l * s / segs;
      double a1 = head - f->dir * l * (s + 1) / segs;
      set_rgba(cr, c->streak,
               c->streak_alpha * f->over * (1 - (double)s / segs));
      cairo_new_path(cr);
      if (f->dir > 0)
        cairo_arc(cr, cx, cy, rr, a1, a0);
      else
        cairo_arc(cr, cx, cy, rr, a0, a1);
      cairo_stroke(cr);
    }
  }
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
  double k = pw / WP_REF_WIDTH, r = c->logo_radius * k;
  double cx = pw / 2.0, cy = ph / 2.0;
  paint_bloom(cr, c, pw, ph, r, f->over); /* over the background, first */

  draw_bar(cr, c, pw, f);
  if (f->over > 0)
    draw_streaks(cr, c, cx, cy, r, k, f);
  nixlogo_draw(cr, cx, cy, r, f->angle, mix(c->logo, c->logo_glow, f->glow),
               mix(c->logo_alt, c->logo_glow_alt, f->glow), 1.0);
  cairo_restore(cr);
}
