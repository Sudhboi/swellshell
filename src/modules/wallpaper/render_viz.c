/* The visualiser drawn by cairo: the ring of bars round the logo, and
 * the bars or smooth curves along the screen's sides. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "draw.h"
#include "render.h"
#include "wallpaper.h"

/* The visualiser: radial bars around the logo, bass at the bottom, climbing
 * both sides to the treble at the top. Each bar grows both ways from the
 * viz_radius line, viz_inward of it inwards. viz_jitter moves each bar's
 * angle, width and length a little, so the ring isn't a perfect circle. The
 * bars never turn, so all of them are drawn once at full length (antialiasing
 * them is the costly part) into an alpha sprite, and each frame paints the
 * colour through it, clipped to one cheap aliased quad per bar cut to its
 * length: 3x faster. */
static struct {
  cairo_surface_t *sprite; /* A8 */
  int x, y;                /* buffer position */
  int pw, ph, bars;
  double logo_radius, radius, length, inward, width, jitter;
} ring_cache;

static void ring_cache_free(void) {
  if (ring_cache.sprite)
    cairo_surface_destroy(ring_cache.sprite);
  ring_cache.sprite = NULL;
}

/* How far viz_jitter = 1 moves a bar: its angle (x its slot), width and
 * full length (x their own). Angle and width stay small enough that
 * neighbours never touch at the default viz_width. */
#define JIT_ANGLE 0.12
#define JIT_WIDTH 0.2
#define JIT_LENGTH 0.08

/* Where bar i on side s (0 left, 1 right) of n sits. */
struct bar_geo {
  double a;   /* angle, radians clockwise from +x (y down) */
  double len; /* full length, px */
  double hw;  /* half width, px */
};

static struct bar_geo bar_geo(const struct wallpaper_config *c, double len,
                              double hw, int n, int s, int i) {
  double j = c->viz_jitter, side = s ? 1 : -1;
  double slot = M_PI / n;
  return (struct bar_geo){
      .a = M_PI / 2 +
           side * slot * (i + 0.5 + JIT_ANGLE * j * wp_viz_hash(s, i, 0)),
      .len = len * (1 + JIT_LENGTH * j * wp_viz_hash(s, i, 2)),
      .hw = hw * (1 + JIT_WIDTH * j * wp_viz_hash(s, i, 3)),
  };
}

/* The quad along angle a from r0 to r1, hw wide on either side. */
static void bar_quad(cairo_t *cr, double cx, double cy, double a, double r0,
                     double r1, double hw) {
  double ca = cos(a), sa = sin(a);
  double nx = -sa * hw, ny = ca * hw; /* across the bar */
  cairo_move_to(cr, cx + ca * r0 + nx, cy + sa * r0 + ny);
  cairo_line_to(cr, cx + ca * r1 + nx, cy + sa * r1 + ny);
  cairo_line_to(cr, cx + ca * r1 - nx, cy + sa * r1 - ny);
  cairo_line_to(cr, cx + ca * r0 - nx, cy + sa * r0 - ny);
  cairo_close_path(cr);
}

/* The farthest any bar reaches, px. */
double wp_ring_outer(const struct wallpaper_config *c, double r, double k) {
  return r * c->viz_radius + c->viz_length * k * (1 - c->viz_inward) *
                                 (1 + JIT_LENGTH * c->viz_jitter);
}

void wp_draw_ring(cairo_t *cr, const struct wallpaper_config *c, int pw, int ph,
                  double r, double k, const struct wp_frame *f) {
  int n = f->nbars;
  double cx = pw / 2.0, cy = ph / 2.0;
  double rc = r * c->viz_radius, len = c->viz_length * k;
  double in = c->viz_inward, out = 1 - in;
  /* Each bar's share where it is narrowest: its inner end at full. */
  double slot = M_PI * fmax(rc - len * in * (1 + JIT_LENGTH), 1) / n;
  double hw = c->viz_width * slot / 2; /* half a bar's width */
  if (!ring_cache.sprite || ring_cache.pw != pw || ring_cache.ph != ph ||
      ring_cache.bars != n || ring_cache.logo_radius != c->logo_radius ||
      ring_cache.radius != c->viz_radius ||
      ring_cache.length != c->viz_length ||
      ring_cache.inward != c->viz_inward || ring_cache.width != c->viz_width ||
      ring_cache.jitter != c->viz_jitter) {
    ring_cache_free();
    double outer = wp_ring_outer(c, r, k) + 2 * hw;
    int x0 = (int)floor(cx - outer) - 1, y0 = (int)floor(cy - outer) - 1;
    int size = (int)ceil(cx + outer) + 1 - x0;
    ring_cache.sprite = cairo_image_surface_create(CAIRO_FORMAT_A8, size, size);
    cairo_t *sc = cairo_create(ring_cache.sprite);
    for (int s = 0; s < 2; ++s)
      for (int i = 0; i < n; ++i) {
        struct bar_geo g = bar_geo(c, len, hw, n, s, i);
        bar_quad(sc, cx - x0, cy - y0, g.a, rc - g.len * in, rc + g.len * out,
                 g.hw);
      }
    cairo_fill(sc); /* the default source is opaque black: full alpha */
    cairo_destroy(sc);
    ring_cache.x = x0;
    ring_cache.y = y0;
    ring_cache.pw = pw;
    ring_cache.ph = ph;
    ring_cache.bars = n;
    ring_cache.logo_radius = c->logo_radius;
    ring_cache.radius = c->viz_radius;
    ring_cache.length = c->viz_length;
    ring_cache.inward = c->viz_inward;
    ring_cache.width = c->viz_width;
    ring_cache.jitter = c->viz_jitter;
  }
  /* The cut keeps the sprite's antialiased sides: a little wider than the
   * bar, but short of where the next bar can be. */
  double j = c->viz_jitter;
  double gap = slot * (1 - 2 * JIT_ANGLE * j) - 2 * hw * (1 + JIT_WIDTH * j);
  double margin = fmax(fmin(1.5, gap / 2), 0);
  cairo_save(cr);
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
  cairo_new_path(cr);
  for (int s = 0; s < 2; ++s)
    for (int i = 0; i < n; ++i) {
      if (f->bars[s][i] < 1e-3)
        continue;
      struct bar_geo g = bar_geo(c, len, hw, n, s, i);
      double lv = fmin(f->bars[s][i], 1);
      /* At full the cut goes past the sprite's antialiased ends. */
      double past = lv >= 1 ? 1 : 0;
      bar_quad(cr, cx, cy, g.a, rc - g.len * in * lv - past,
               rc + g.len * out * lv + past, g.hw + margin);
    }
  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
  cairo_clip(cr);
  set_rgba(cr, c->viz_color, c->viz_alpha);
  cairo_mask_surface(cr, ring_cache.sprite, ring_cache.x, ring_cache.y);
  cairo_restore(cr);
}

/* The visualiser along the screen's sides: the left channel on the left,
 * the right on the right, bass at the bottom climbing to the treble at the
 * top, each bar growing in from the edge. Bars are axis-aligned and on
 * whole pixels (their ends too: a pixel step is invisible at this size), so
 * they are crisp and cheap without antialiasing or the ring's sprite. */
void wp_draw_edges(cairo_t *cr, const struct wallpaper_config *c, int pw,
                   int ph, const struct wp_frame *f) {
  int n = f->nbars;
  cairo_save(cr);
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
  cairo_new_path(cr);
  for (int s = 0; s < 2; ++s) {
    double x0, y0, x1, y1;
    wp_edge_box(c, pw, ph, s, &x0, &y0, &x1, &y1);
    x0 = round(x0);
    x1 = round(x1);
    double slot = (y1 - y0) / n, len = x1 - x0;
    for (int i = 0; i < n; ++i) {
      double lv = fmin(f->bars[s][i], 1);
      if (lv < 1e-3)
        continue;
      double yc = y1 - slot * (i + 0.5);
      double top = round(yc - c->viz_width * slot / 2);
      double bot = fmax(round(yc + c->viz_width * slot / 2), top + 1);
      double l = round(len * lv);
      if (l >= 1)
        cairo_rectangle(cr, s ? x1 - l : x0, top, l, bot - top);
    }
  }
  set_rgba(cr, c->viz_color, c->viz_alpha);
  cairo_fill(cr);
  cairo_restore(cr);
}

/* Levels (capped at 1) blurred over their neighbours by a gaussian of
 * sigma bars, weights renormalised at the ends; sigma 0 copies them. */
static void blur_levels(const float *in, int n, double sigma, double *out) {
  int rad = sigma > 0 ? (int)ceil(3 * sigma) : 0;
  for (int i = 0; i < n; ++i) {
    double sum = 0, wsum = 0;
    for (int j = i - rad; j <= i + rad; ++j) {
      if (j < 0 || j >= n)
        continue;
      double d = (j - i) / (sigma > 0 ? sigma : 1);
      double wt = exp(-0.5 * d * d);
      sum += wt * fmin(in[j], 1);
      wsum += wt;
    }
    out[i] = sum / wsum;
  }
}

void wp_edge_curve(const struct wallpaper_config *c, int pw, int ph,
                   const struct wp_frame *f, int s, struct wp_edge_curve *e) {
  int n = f->nbars;
  wp_edge_box(c, pw, ph, s, &e->x0, &e->y0, &e->x1, &e->y1);
  double slot = (e->y1 - e->y0) / n, len = e->len = e->x1 - e->x0;
  /* Without a margin the shape runs off the top and bottom by the shake
   * distance, so a shake never shows its ends. */
  double ext = c->viz_edge_margin <= 0 && c->react_shake
                   ? ceil(c->react_shake_px * pw / WP_REF_WIDTH)
                   : 0;
  e->y0 -= ext;
  e->y1 += ext;
  /* Bass first, from the bottom: both ends held flat to the margins. */
  double lv[WP_VIZ_MAX] = {0};
  blur_levels(f->bars[s], n, c->viz_edge_blur, lv);
  double *py = e->py, *pl = e->pl;
  int m = 0;
  py[m] = e->y1;
  pl[m++] = lv[0] * len;
  for (int i = 0; i < n; ++i) {
    py[m] = e->y1 - ext - slot * (i + 0.5);
    pl[m++] = lv[i] * len;
  }
  py[m] = e->y0;
  pl[m++] = lv[n - 1] * len;
  e->m = m;
  for (int i = 0; i + 1 < m; ++i) {
    int a = i > 0 ? i - 1 : i, d = i + 2 < m ? i + 2 : i + 1;
    e->c1[i] = fmin(fmax(pl[i] + (pl[i + 1] - pl[a]) / 6, 0), len);
    e->c2[i] = fmin(fmax(pl[i + 1] - (pl[d] - pl[i]) / 6, 0), len);
    e->cy1[i] = py[i] + (py[i + 1] - py[a]) / 6;
    e->cy2[i] = py[i + 1] - (py[d] - py[i]) / 6;
  }
}

/* The same levels as one smooth shape per side: a curve through each
 * bar's level at its slot's centre (Catmull-Rom as Beziers), closed along
 * the screen's side, filled with a fade and traced with a line. The control
 * points are clamped to the strip, so the curve (inside their hull) never
 * leaves it. */
void wp_draw_edges_smooth(cairo_t *cr, const struct wallpaper_config *c, int pw,
                          int ph, const struct wp_frame *f) {
  for (int s = 0; s < 2; ++s) {
    struct wp_edge_curve e;
    wp_edge_curve(c, pw, ph, f, s, &e);
    double x0 = e.x0, x1 = e.x1, y0 = e.y0, y1 = e.y1, len = e.len;
    const double *py = e.py, *pl = e.pl;
    /* x of a level: in from the side the strip is on. */
#define EDGE_X(l) (s ? x1 - (l) : x0 + (l))
    cairo_new_path(cr);
    cairo_move_to(cr, EDGE_X(pl[0]), py[0]);
    for (int i = 0; i + 1 < e.m; ++i)
      cairo_curve_to(cr, EDGE_X(e.c1[i]), e.cy1[i], EDGE_X(e.c2[i]), e.cy2[i],
                     EDGE_X(pl[i + 1]), py[i + 1]);
    cairo_path_t *curve = cairo_copy_path(cr);
    cairo_line_to(cr, EDGE_X(0), y0);
    cairo_line_to(cr, EDGE_X(0), y1);
    cairo_close_path(cr);
    /* The fill fades from the screen's side towards a full level, so it
     * glows in from the edge rather than standing as a solid shape. */
    cairo_pattern_t *p =
        cairo_pattern_create_linear(EDGE_X(0), 0, EDGE_X(len), 0);
    uint32_t vc = c->viz_color;
    double va = col_a(vc) * c->viz_alpha;
    cairo_pattern_add_color_stop_rgba(p, 0, col_r(vc), col_g(vc), col_b(vc),
                                      va);
    cairo_pattern_add_color_stop_rgba(p, 1, col_r(vc), col_g(vc), col_b(vc),
                                      va * c->viz_edge_fade);
    cairo_set_source(cr, p);
    cairo_fill(cr);
    cairo_pattern_destroy(p);
    /* A brighter line along the curve, kept off the screen's side so
     * silent stretches don't draw one down the edge. */
    double rw = c->viz_edge_rim_width * pw / WP_REF_WIDTH;
    if (rw > 0) {
      cairo_save(cr);
      double cx0 = s ? x0 - rw : x0 + rw;
      cairo_rectangle(cr, cx0, y0 - rw, len, y1 - y0 + 2 * rw);
      cairo_clip(cr);
      cairo_append_path(cr, curve);
      cairo_set_line_width(cr, rw);
      cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
      cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
      set_rgba(cr, c->viz_edge_rim, c->viz_alpha);
      cairo_stroke(cr);
      cairo_restore(cr);
    }
    cairo_path_destroy(curve);
#undef EDGE_X
  }
}

void wp_render_viz_finish(void) { ring_cache_free(); }
