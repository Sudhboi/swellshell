/* Cairo drawing of the wallpaper: a port of make_black_bar_nixos.py. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <pango/pangocairo.h>

#include "config.h"
#include "nixlogo.h"
#include "util.h"
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

static double ring_outer(const struct wallpaper_config *c, double r,
                         double k);

/* The dark glow behind the title and artist over the topo map reaches
 * this far past them, px. */
static double text_pad(const struct wallpaper_config *c, int pw) {
  return wp_topo(c) && c->topo_text_backdrop > 0
             ? ceil(0.5 * c->media_title_size * pw / WP_REF_WIDTH)
             : 0;
}

/* The title and artist's box under the logo, in buffer px, with the glow's
 * room around it. */
static void text_box(const struct wallpaper_config *c, int pw, int ph,
                     double *x, double *y, double *w, double *h) {
  double k = pw / WP_REF_WIDTH, pad = text_pad(c, pw);
  *w = c->media_max_width * k + 2 * pad;
  *h = ceil(1.3 * (c->media_title_size + c->media_artist_size) * k) + 2 +
       2 * pad;
  *x = round(pw / 2.0 - *w / 2.0);
  *y = round(ph / 2.0 + c->logo_radius * k * (1 + c->react_pulse_scale) +
             c->media_gap * k) -
       pad;
}

static void blur_a8(unsigned char *d, int w, int h, int stride, int b);

/* Speed lines sit on rings this far out (x logo radius). */
static const double streak_rings[] = {1.08, 1.16, 1.24};
#define STREAK_OUTER 1.24

/* The strip side s (0 left, 1 right) of the edge bars fills at full. */
static void edge_box(const struct wallpaper_config *c, int pw, int ph, int s,
                     double *x0, double *y0, double *x1, double *y1) {
  double k = pw / WP_REF_WIDTH;
  double gap = c->viz_edge_gap * k, len = c->viz_edge_length * k;
  *x0 = s ? pw - gap - len : gap;
  *x1 = *x0 + len;
  *y0 = c->viz_edge_margin * k;
  *y1 = ph - *y0;
}

void wp_regions(const struct wallpaper_config *c, int pw, int ph, bool ring,
                bool text, struct wp_rect out[WP_REGIONS]) {
  double x, y, w, h;
  bar_box(c, pw, &x, &y, &w, &h);
  /* A shake moves everything by up to this much. */
  double sh = c->react_shake ? ceil(c->react_shake_px * pw / WP_REF_WIDTH) : 0;
  out[0] = clamp_rect(x - sh, y - sh, x + w + sh, y + h + sh, pw, ph);
  /* The bloom and the speed lines reach past the logo, the visualiser
   * further still. */
  double k = pw / WP_REF_WIDTH;
  double r = c->logo_radius * k * fmax(c->bloom_radius, STREAK_OUTER) +
             c->streak_width * k + 1;
  /* The logo itself at its biggest: pulsed, with its lambdas slid out. */
  r = fmax(r, c->logo_radius * k * (1 + c->react_pulse_scale) *
                      (1 + c->react_slide_dist) +
                  2);
  /* The glass snowflake's shadow and outline reach past it. */
  if (wp_topo(c))
    r = fmax(r, c->logo_radius * k * (1 + c->react_pulse_scale) *
                        (1 + c->react_slide_dist) +
                    (1.8 * c->topo_shadow_px + c->topo_edge_width / 2) * k +
                    4);
  if (ring && !c->viz_edges)
    r = fmax(r, ring_outer(c, c->logo_radius * k, k) + 1);
  r += sh;
  out[1] = clamp_rect(pw / 2.0 - r, ph / 2.0 - r, pw / 2.0 + r, ph / 2.0 + r,
                      pw, ph);
  for (int s = 0; s < 2; ++s) {
    out[2 + s] = (struct wp_rect){0, 0, 0, 0};
    if (ring && c->viz_edges) {
      double x0, y0, x1, y1;
      edge_box(c, pw, ph, s, &x0, &y0, &x1, &y1);
      /* The smooth curve's line reaches past a full level by half its
       * width. */
      double rim = c->viz_edge_smooth ? c->viz_edge_rim_width * k / 2 : 0;
      if (s)
        x0 -= rim;
      else
        x1 += rim;
      out[2 + s] = clamp_rect(x0 - sh, y0 - sh, x1 + sh, y1 + sh, pw, ph);
    }
  }
  out[4] = (struct wp_rect){0, 0, 0, 0};
  if (text) {
    text_box(c, pw, ph, &x, &y, &w, &h);
    out[4] = clamp_rect(x - sh, y - sh, x + w + sh, y + h + sh, pw, ph);
  }
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
  bool topo; /* transparent around the bloom, drawn over the map */
  uint32_t background, bloom;
  double logo_radius, bloom_radius, bloom_alpha;
} bloom_cache;

static void bloom_cache_free(void) {
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
      bloom_cache.step != step || bloom_cache.topo != wp_topo(c) ||
      bloom_cache.background != c->background ||
      bloom_cache.bloom != c->bloom ||
      bloom_cache.logo_radius != c->logo_radius ||
      bloom_cache.bloom_radius != c->bloom_radius ||
      bloom_cache.bloom_alpha != c->bloom_alpha) {
    bloom_cache_free();
    double outer = r * (1 + (c->bloom_radius - 1) * step / BLOOM_STEPS);
    int x0 = (int)floor(cx - outer) - 1, y0 = (int)floor(cy - outer) - 1;
    int size = (int)ceil(cx + outer) + 1 - x0;
    bool topo = wp_topo(c);
    bloom_cache.tile = cairo_image_surface_create(
        topo ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24, size, size);
    cairo_t *tc = cairo_create(bloom_cache.tile);
    if (!topo) {
      set_rgba(tc, c->background, 1.0);
      cairo_set_operator(tc, CAIRO_OPERATOR_SOURCE);
      cairo_paint(tc);
      cairo_set_operator(tc, CAIRO_OPERATOR_OVER);
    }
    draw_bloom(tc, c, cx - x0, cy - y0, r, (double)step / BLOOM_STEPS);
    cairo_destroy(tc);
    bloom_cache.x = x0;
    bloom_cache.y = y0;
    bloom_cache.pw = pw;
    bloom_cache.ph = ph;
    bloom_cache.step = step;
    bloom_cache.topo = topo;
    bloom_cache.background = c->background;
    bloom_cache.bloom = c->bloom;
    bloom_cache.logo_radius = c->logo_radius;
    bloom_cache.bloom_radius = c->bloom_radius;
    bloom_cache.bloom_alpha = c->bloom_alpha;
  }
  cairo_save(cr);
  if (!bloom_cache.topo)
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

/* The visualiser: radial bars around the logo, bass at the bottom, climbing
 * both sides to the treble at the top. Each bar grows both ways from the
 * viz_radius line, viz_inward of it inwards. viz_jitter moves each bar's
 * angle, width and length a little, so the ring isn't a perfect circle. The bars never turn, so all of them are drawn once at full
 * length (antialiasing them is the costly part) into an alpha sprite, and
 * each frame paints the colour through it, clipped to one cheap aliased quad
 * per bar cut to its length: 3x faster. */
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
static double ring_outer(const struct wallpaper_config *c, double r,
                         double k) {
  return r * c->viz_radius + c->viz_length * k * (1 - c->viz_inward) *
                                 (1 + JIT_LENGTH * c->viz_jitter);
}

static void draw_ring(cairo_t *cr, const struct wallpaper_config *c, int pw,
                      int ph, double r, double k, const struct wp_frame *f) {
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
    double outer = ring_outer(c, r, k) + 2 * hw;
    int x0 = (int)floor(cx - outer) - 1, y0 = (int)floor(cy - outer) - 1;
    int size = (int)ceil(cx + outer) + 1 - x0;
    ring_cache.sprite =
        cairo_image_surface_create(CAIRO_FORMAT_A8, size, size);
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
static void draw_edges(cairo_t *cr, const struct wallpaper_config *c, int pw,
                       int ph, const struct wp_frame *f) {
  int n = f->nbars;
  cairo_save(cr);
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
  cairo_new_path(cr);
  for (int s = 0; s < 2; ++s) {
    double x0, y0, x1, y1;
    edge_box(c, pw, ph, s, &x0, &y0, &x1, &y1);
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
  edge_box(c, pw, ph, s, &e->x0, &e->y0, &e->x1, &e->y1);
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
 * the screen's side, filled with a fade and traced with a line. The control points are clamped to the strip,
 * so the curve (inside their hull) never leaves it. */
static void draw_edges_smooth(cairo_t *cr, const struct wallpaper_config *c,
                              int pw, int ph, const struct wp_frame *f) {
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

/* render_bar_mask(): a pill outline around the whole bar and, inside the
 * gap, a pill filled to percent, all at the pulse brightness. */
static void draw_bar(cairo_t *cr, const struct wallpaper_config *c, int pw,
                     const struct wp_frame *f) {
  if (!c->battery_bar)
    return;
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
  double a = f->level * (1 - fmin(fmax(f->bar_fade, 0), 1));
  if (!wp_topo(c)) {
    set_rgba(cr, c->bar, a);
    cairo_fill(cr);
    return;
  }
  /* The topo style: like the snowflake, dark glass in an outline coloured
   * left to right, the fill in the same colours at full strength. */
  cairo_new_path(cr);
  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
  pill(cr, x + b, y + b, w - 2 * b, h - 2 * b);
  set_rgba(cr, c->topo_logo_fill, 1 - fmin(fmax(f->bar_fade, 0), 1));
  cairo_fill(cr);
  const uint32_t edge[3] = {c->topo_edge_a, c->topo_edge_mid, c->topo_edge_b};
  for (int part = 0; part < 2; ++part) {
    cairo_pattern_t *p = cairo_pattern_create_linear(x, 0, x + w, 0);
    for (int t = 0; t < 3; ++t) {
      /* The outline as faint as the snowflake's, the fill opaque. */
      double ea = part ? 1 : col_a(edge[t]);
      cairo_pattern_add_color_stop_rgba(p, t / 2.0, col_r(edge[t]),
                                        col_g(edge[t]), col_b(edge[t]),
                                        ea * a);
    }
    cairo_set_source(cr, p);
    if (part == 0) {
      cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
      pill(cr, x, y, w, h);
      pill(cr, x + b, y + b, w - 2 * b, h - 2 * b);
    } else if (fw > 0) {
      cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
      pill(cr, x + pad, y + pad, fw, c->bar_height * k);
    }
    cairo_fill(cr);
    cairo_pattern_destroy(p);
  }
  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
}

/* The power-off CRT: a bright pill (a dot or a line) in a soft glow. */
static void draw_crt(cairo_t *cr, const struct wallpaper_config *c,
                     double cx, double cy, double k,
                     const struct wp_frame *f) {
  double w = f->crt_w * k, h = f->crt_h * k, a = fmin(f->crt_alpha, 1);
  static const double glow[][2] = {{4, 0.08}, {2, 0.15}, {0, 1}};
  for (size_t i = 0; i < sizeof(glow) / sizeof(glow[0]); ++i) {
    double pad = glow[i][0] * h;
    pill(cr, cx - w / 2 - pad, cy - h / 2 - pad, w + 2 * pad, h + 2 * pad);
    set_rgba(cr, c->crt_color, a * glow[i][1]);
    cairo_fill(cr);
  }
}

/* The title and artist, drawn once per track (and size and config) into a
 * transparent tile that each frame paints at the fade's alpha. */
static struct {
  cairo_surface_t *tile;
  int pw, ph;
  char title[256], artist[256];
  struct wallpaper_config cfg;
} text_cache;

static void text_cache_free(void) {
  if (text_cache.tile)
    cairo_surface_destroy(text_cache.tile);
  text_cache.tile = NULL;
}

/* One centred line of text in colour col at the top of y, ellipsised to w. */
static double text_line(cairo_t *cr, PangoLayout *l,
                        PangoFontDescription *fd, double size, uint32_t col,
                        const char *s, double w, double y) {
  pango_font_description_set_absolute_size(fd, size * PANGO_SCALE);
  pango_layout_set_font_description(l, fd);
  pango_layout_set_width(l, (int)(w * PANGO_SCALE));
  pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
  pango_layout_set_alignment(l, PANGO_ALIGN_CENTER);
  pango_layout_set_single_paragraph_mode(l, TRUE);
  pango_layout_set_text(l, s, -1);
  set_rgba(cr, col, 1.0);
  cairo_move_to(cr, 0, y);
  pango_cairo_show_layout(cr, l);
  int lh;
  pango_layout_get_pixel_size(l, NULL, &lh);
  return y + lh;
}

static void draw_text(cairo_t *cr, const struct wallpaper_config *c, int pw,
                      int ph, const struct wp_frame *f) {
  if (!c->media_title || f->media_alpha <= 0 || !f->title)
    return;
  const char *artist = f->artist ? f->artist : "";
  double x, y, w, h;
  text_box(c, pw, ph, &x, &y, &w, &h);
  if (!text_cache.tile || text_cache.pw != pw || text_cache.ph != ph ||
      strcmp(text_cache.title, f->title) ||
      strcmp(text_cache.artist, artist) ||
      memcmp(&text_cache.cfg, c, sizeof *c)) {
    text_cache_free();
    text_cache.tile = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                 (int)ceil(w), (int)h);
    cairo_t *tc = cairo_create(text_cache.tile);
    double k = pw / WP_REF_WIDTH, pad = text_pad(c, pw);
    bool topo = wp_topo(c);
    PangoLayout *l = pango_cairo_create_layout(tc);
    PangoFontDescription *fd = pango_font_description_from_string(c->media_font);
    cairo_translate(tc, pad, pad);
    double tw = w - 2 * pad;
    double ty = text_line(tc, l, fd, c->media_title_size * k,
                          topo ? c->topo_text : c->media_text, f->title, tw, 0);
    if (artist[0])
      text_line(tc, l, fd, c->media_artist_size * k,
                topo ? c->topo_text_dim : c->media_dim, artist, tw,
                ty + 0.1 * c->media_artist_size * k);
    pango_font_description_free(fd);
    g_object_unref(l);
    cairo_destroy(tc);
    if (pad > 0) {
      /* The glow: the text's shape, blurred, in black under it. */
      int tw2 = cairo_image_surface_get_width(text_cache.tile);
      int th2 = cairo_image_surface_get_height(text_cache.tile);
      cairo_surface_t *m = cairo_image_surface_create(CAIRO_FORMAT_A8, tw2, th2);
      cairo_t *mc = cairo_create(m);
      cairo_set_source_surface(mc, text_cache.tile, 0, 0);
      cairo_paint(mc);
      cairo_destroy(mc);
      cairo_surface_flush(m);
      blur_a8(cairo_image_surface_get_data(m), tw2, th2,
              cairo_image_surface_get_stride(m), (int)ceil(pad / 3));
      /* Blurred thin strokes come out faint: strengthen them. */
      unsigned char *d = cairo_image_surface_get_data(m);
      int st = cairo_image_surface_get_stride(m);
      for (int y = 0; y < th2; ++y)
        for (int x = 0; x < tw2; ++x) {
          int v = d[y * st + x] * 4;
          d[y * st + x] = (unsigned char)(v > 255 ? 255 : v);
        }
      cairo_surface_mark_dirty(m);
      tc = cairo_create(text_cache.tile);
      cairo_set_operator(tc, CAIRO_OPERATOR_DEST_OVER);
      cairo_set_source_rgba(tc, 0, 0, 0, c->topo_text_backdrop);
      cairo_mask_surface(tc, m, 0, 0);
      cairo_destroy(tc);
      cairo_surface_destroy(m);
    }
    text_cache.pw = pw;
    text_cache.ph = ph;
    snprintf(text_cache.title, sizeof text_cache.title, "%s", f->title);
    snprintf(text_cache.artist, sizeof text_cache.artist, "%s", artist);
    text_cache.cfg = *c;
  }
  cairo_save(cr);
  cairo_set_source_surface(cr, text_cache.tile, x, y);
  cairo_rectangle(cr, x, y, ceil(w), h);
  cairo_clip(cr);
  cairo_paint_with_alpha(cr, fmin(f->media_alpha, 1));
  cairo_restore(cr);
}

/* The snowflake's hexagonal hole: its corners sit at the lambdas' inner
 * points, this far out (x the radius) and at this angle. */
#define HOLE 0.39046
#define HOLE_ANGLE 3.08629 /* atan2(0.02158, -0.38986) */

/* The cover scaled to the hexagon at its resting size, made once per cover
 * and size. */
static struct {
  cairo_surface_t *tile;
  unsigned id;
  int side;
} art_cache;

static void art_cache_free(void) {
  if (art_cache.tile)
    cairo_surface_destroy(art_cache.tile);
  art_cache.tile = NULL;
}

static cairo_surface_t *art_tile(const struct wp_frame *f, int side) {
  if (!f->art || side <= 0)
    return NULL;
  if (!art_cache.tile || art_cache.id != f->art_id || art_cache.side != side) {
    art_cache_free();
    art_cache.tile =
        cairo_image_surface_create(CAIRO_FORMAT_RGB24, side, side);
    cairo_t *tc = cairo_create(art_cache.tile);
    double s = (double)side / cairo_image_surface_get_width(f->art);
    cairo_scale(tc, s, s);
    cairo_set_source_surface(tc, f->art, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(tc), CAIRO_FILTER_GOOD);
    cairo_set_operator(tc, CAIRO_OPERATOR_SOURCE);
    cairo_paint(tc);
    cairo_destroy(tc);
    art_cache.id = f->art_id;
    art_cache.side = side;
  }
  return art_cache.tile;
}

static void hex_path(cairo_t *cr, double hr) {
  for (int i = 0; i < 6; ++i) {
    double a = HOLE_ANGLE + i * M_PI / 3;
    cairo_line_to(cr, hr * cos(a), hr * sin(a));
  }
  cairo_close_path(cr);
}

/* The cover filling a hexagon of radius hr, in its own (turning) frame. */
static void hex_art(cairo_t *cr, cairo_surface_t *art, double hr) {
  int side = cairo_image_surface_get_width(art);
  cairo_pattern_t *p = cairo_pattern_create_for_surface(art);
  cairo_matrix_t m;
  cairo_matrix_init_translate(&m, side / 2.0, side / 2.0);
  cairo_matrix_scale(&m, side / (2 * hr), side / (2 * hr));
  cairo_pattern_set_matrix(p, &m);
  cairo_pattern_set_filter(p, CAIRO_FILTER_BILINEAR);
  cairo_set_source(cr, p);
  cairo_pattern_destroy(p);
}

/* The point t (0..1) of the way clockwise round a hexagon of radius hr,
 * from the middle of its top edge (at angle 0). */
static void hex_point(double hr, double t, double *x, double *y) {
  /* The top edge runs from corner 1 to corner 2; its middle is 1/12 of the
   * way round before corner 2, each edge 1/6. */
  double at = t * 6 + 0.5; /* in edges from corner 1; 6.5 is back at 0 */
  int e = (int)floor(at);
  double u = at - e;
  double a = HOLE_ANGLE + (1 + e) * M_PI / 3, b = a + M_PI / 3;
  *x = hr * (cos(a) + (cos(b) - cos(a)) * u);
  *y = hr * (sin(a) + (sin(b) - sin(a)) * u);
}

/* The border from t0 to t1 (0 <= t0 <= t1 <= 1) of the way round. */
static void hex_trace(cairo_t *cr, double hr, double t0, double t1) {
  double x, y;
  hex_point(hr, t0, &x, &y);
  cairo_move_to(cr, x, y);
  /* The corners in between, at 1/12, 3/12, ... 11/12. */
  for (int k = 0; k < 6; ++k) {
    double tc = (2 * k + 1) / 12.0;
    if (tc > t0 && tc < t1) {
      hex_point(hr, tc, &x, &y);
      cairo_line_to(cr, x, y);
    }
  }
  hex_point(hr, t1, &x, &y);
  cairo_line_to(cr, x, y);
}

/* The glass snowflake's outline colours, lit towards white by its glow. */
static void topo_edges(const struct wallpaper_config *c,
                       const struct wp_frame *f, uint32_t edge[3]) {
  double lit = 0.35 * fmin(fmax(f->glow, 0), 1);
  edge[0] = mix(c->topo_edge_a, 0xffffffffu, lit);
  edge[1] = mix(c->topo_edge_mid, 0xffffffffu, lit);
  edge[2] = mix(c->topo_edge_b, 0xffffffffu, lit);
}

/* The outline's gradient, top left to bottom right, from cx - e, cy - e to
 * cx + e, cy + e: each colour's alpha times a, or opaque times a when
 * solid. */
static cairo_pattern_t *topo_edge_pattern(const uint32_t edge[3], double cx,
                                          double cy, double e, double a,
                                          bool solid) {
  cairo_pattern_t *p =
      cairo_pattern_create_linear(cx - e, cy - e, cx + e, cy + e);
  for (int t = 0; t < 3; ++t)
    cairo_pattern_add_color_stop_rgba(p, t / 2.0, col_r(edge[t]),
                                      col_g(edge[t]), col_b(edge[t]),
                                      (solid ? 1 : col_a(edge[t])) * a);
  return p;
}

/* Stroke the hexagon's path (radius r, drawn turned by angle about the
 * snowflake's centre, the origin) in col, or in the outline's gradient
 * across it when edge. */
static void stroke_hex(cairo_t *cr, const uint32_t *edge, uint32_t col,
                       double angle, double r, double a, bool solid) {
  if (!edge) {
    set_rgba(cr, col, a);
    cairo_stroke(cr);
    return;
  }
  /* The gradient doesn't turn: set it unturned (the path is kept). */
  cairo_save(cr);
  cairo_rotate(cr, -angle);
  cairo_pattern_t *p = topo_edge_pattern(edge, 0, 0, r * M_SQRT1_2, a, solid);
  cairo_set_source(cr, p);
  cairo_stroke(cr);
  cairo_pattern_destroy(p);
  cairo_restore(cr);
}

/* What's playing, in the snowflake's hole (resting radius r, now rp): the
 * cover in grey, with a line round its border, clockwise from the top, as
 * far as the track has played. Without a track length a sixth of it runs
 * round. The hexagon, the cover and the line turn with the snowflake.
 * Under topo the line takes the outline's colours (fixed to the screen,
 * spread across the hexagon):
 * the track as faint as the outline, the progress opaque, like the bar. */
static void draw_hex(cairo_t *cr, const struct wallpaper_config *c, double cx,
                     double cy, double r, double rp,
                     const struct wp_frame *f) {
  double alpha = fmin(f->media_alpha, 1);
  if (alpha <= 0)
    return;
  double hr = HOLE * c->media_hex_size * rp;
  double lw = c->media_line_width * rp / c->logo_radius;
  bool line = lw > 0 && (f->progress >= 0 || f->chase >= 0);
  cairo_surface_t *art =
      c->media_art ? art_tile(f, (int)ceil(2 * HOLE * c->media_hex_size * r))
                   : NULL;
  if (!art && !line)
    return;
  cairo_save(cr);
  cairo_translate(cr, cx, cy);
  cairo_rotate(cr, f->angle);
  if (art && c->media_art_alpha > 0) {
    cairo_save(cr);
    hex_path(cr, hr);
    cairo_clip(cr);
    hex_art(cr, art, hr);
    cairo_paint_with_alpha(cr, alpha * c->media_art_alpha);
    cairo_restore(cr);
  }
  if (line) {
    /* The line lies just inside the hexagon's edge. */
    double hl = hr - lw / 2;
    cairo_set_line_width(cr, lw);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
    bool topo = wp_topo(c);
    uint32_t edge[3];
    if (topo)
      topo_edges(c, f, edge);
    hex_path(cr, hl);
    stroke_hex(cr, topo ? edge : NULL, c->media_track, f->angle, hr, alpha,
               false);
    if (f->progress > 0) {
      hex_trace(cr, hl, 0, fmin(f->progress, 1));
    } else if (f->progress < 0) {
      double t0 = fmod(f->chase, 6) / 6, t1 = t0 + 1 / 6.0;
      hex_trace(cr, hl, t0, fmin(t1, 1));
      if (t1 > 1)
        hex_trace(cr, hl, 0, t1 - 1);
    }
    stroke_hex(cr, topo ? edge : NULL, c->media_line, f->angle, hr, alpha,
               true);
  }
  cairo_restore(cr);
}

/* The glass snowflake's shadow: a silhouette at the resting radius,
 * blurred, as a mask turned and pulsed with the snowflake. [0] is the
 * whole snowflake; [1] lambda 0 alone, drawn under each lambda in turn
 * while they slide or fade apart (in a show). */
struct shadow {
  cairo_surface_t *mask; /* A8 */
  double x, y;           /* its top left, px from the snowflake's centre */
  double r, blur;        /* resting radius, blur radius, px */
};
static struct shadow shadow_cache[2];

static void shadow_cache_free(void) {
  for (int i = 0; i < 2; ++i) {
    if (shadow_cache[i].mask)
      cairo_surface_destroy(shadow_cache[i].mask);
    shadow_cache[i].mask = NULL;
  }
}

/* Three box blurs of radius b over an A8 image, rows then columns: close
 * to a Gaussian. */
static void blur_a8(unsigned char *d, int w, int h, int stride, int b) {
  unsigned char *line = xcalloc(w > h ? w : h, 1);
  for (int pass = 0; pass < 6; ++pass) {
    bool rows = pass % 2 == 0;
    int len = rows ? w : h, count = rows ? h : w;
    int step = rows ? 1 : stride;
    for (int j = 0; j < count; ++j) {
      unsigned char *p = d + (rows ? j * stride : j);
      for (int i = 0; i < len; ++i)
        line[i] = p[i * step];
      int sum = 0;
      for (int i = 0; i <= b && i < len; ++i)
        sum += line[i];
      for (int i = 0; i < len; ++i) {
        p[i * step] = (unsigned char)(sum / (2 * b + 1));
        int in = i + b + 1, out = i - b;
        sum += (in < len ? line[in] : 0) - (out >= 0 ? line[out] : 0);
      }
    }
  }
  free(line);
}

/* The silhouette's path: all six lambdas or lambda 0, radius r. */
static void silhouette(cairo_t *cr, double r, bool whole) {
  cairo_save(cr);
  cairo_scale(cr, r, r);
  for (int k = 0; k < (whole ? 6 : 1); ++k)
    nixlogo_lambda_path(cr, k, 0);
  cairo_restore(cr);
}

static const struct shadow *shadow_mask(int which, double r, double blur) {
  struct shadow *sc = &shadow_cache[which];
  if (sc->mask && sc->r == r && sc->blur == blur)
    return sc;
  if (sc->mask)
    cairo_surface_destroy(sc->mask);
  cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
  cairo_t *pc = cairo_create(probe);
  silhouette(pc, r, which == 0);
  double x0, y0, x1, y1;
  cairo_path_extents(pc, &x0, &y0, &x1, &y1);
  cairo_destroy(pc);
  cairo_surface_destroy(probe);
  int b = (int)ceil(blur / 3);
  double pad = 3 * b + 2;
  x0 = floor(x0 - pad);
  y0 = floor(y0 - pad);
  int w = (int)ceil(x1 + pad - x0), h = (int)ceil(y1 + pad - y0);
  cairo_surface_t *m = cairo_image_surface_create(CAIRO_FORMAT_A8, w, h);
  cairo_t *mc = cairo_create(m);
  cairo_translate(mc, -x0, -y0);
  silhouette(mc, r, which == 0);
  cairo_fill(mc);
  cairo_destroy(mc);
  cairo_surface_flush(m);
  if (b > 0)
    blur_a8(cairo_image_surface_get_data(m), w, h,
            cairo_image_surface_get_stride(m), b);
  cairo_surface_mark_dirty(m);
  *sc = (struct shadow){m, x0, y0, r, blur};
  return sc;
}

/* The topo style's snowflake (resting radius r0, now r): dark glass
 * lambdas with the contour lines showing through, a gradient outline fixed
 * to the screen (it doesn't turn with them) and a soft shadow down and to
 * the right. */
static void draw_glass_logo(cairo_t *cr, const struct wallpaper_config *c,
                            double cx, double cy, double r0, double r,
                            double k, const struct wp_frame *f) {
  double alpha[6];
  for (int i = 0; i < 6; ++i)
    alpha[i] = 1 - fmin(fmax(f->lambda_fade[i], 0), 1);
  double s = r / r0; /* sizes follow the pulse and the shrink */

  double sd = c->topo_shadow_px * k * s;
  if (sd > 0 && c->topo_shadow_alpha > 0) {
    /* One mask for the lot unless the lambdas slide or fade apart. */
    bool whole = true;
    for (int i = 0; i < 6; ++i)
      whole = whole && f->slide[i] == 0 && alpha[i] == 1;
    const struct shadow *m =
        shadow_mask(whole ? 0 : 1, r0, c->topo_shadow_px * k);
    double dx, dy;
    nixlogo_lambda_dir(&dx, &dy);
    for (int i = 0; i < (whole ? 1 : 6); ++i) {
      if (alpha[i] <= 0)
        continue;
      cairo_save(cr);
      cairo_translate(cr, cx + 0.7 * sd, cy + 0.7 * sd);
      cairo_rotate(cr, f->angle + i * M_PI / 3);
      cairo_scale(cr, r, r);
      cairo_translate(cr, dx * f->slide[i], dy * f->slide[i]);
      cairo_scale(cr, 1 / r0, 1 / r0);
      cairo_set_source_rgba(cr, 0, 0, 0, c->topo_shadow_alpha * alpha[i]);
      /* It's blurred: the nearest pixel is as good as any filter. */
      cairo_pattern_t *mp = cairo_pattern_create_for_surface(m->mask);
      cairo_matrix_t mm;
      cairo_matrix_init_translate(&mm, -m->x, -m->y);
      cairo_pattern_set_matrix(mp, &mm);
      cairo_pattern_set_filter(mp, CAIRO_FILTER_NEAREST);
      cairo_mask(cr, mp);
      cairo_pattern_destroy(mp);
      cairo_restore(cr);
    }
  }

  uint32_t fill = mix(c->topo_logo_fill, c->topo_logo_glow, f->glow);
  uint32_t edge[3];
  topo_edges(c, f, edge);
  double ew = c->topo_edge_width * k * s;
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
  /* The lambdas alike (all of them, but in a show) in one path. */
  bool done[6] = {false};
  for (int i = 0; i < 6; ++i) {
    if (done[i] || alpha[i] <= 0)
      continue;
    double a = alpha[i];
    cairo_new_path(cr);
    for (int j = i; j < 6; ++j)
      if (!done[j] && alpha[j] == a) {
        done[j] = true;
        cairo_save(cr);
        cairo_translate(cr, cx, cy);
        cairo_rotate(cr, f->angle);
        cairo_scale(cr, r, r);
        nixlogo_lambda_path(cr, j, f->slide[j]);
        cairo_restore(cr);
      }
    set_rgba(cr, fill, a);
    if (ew <= 0) {
      cairo_fill(cr);
      continue;
    }
    cairo_fill_preserve(cr);
    /* Over the lambdas' extent (not the bounding box's corners). */
    cairo_pattern_t *p = topo_edge_pattern(edge, cx, cy, 0.6 * r, a, false);
    cairo_set_source(cr, p);
    cairo_set_line_width(cr, ew);
    cairo_stroke(cr);
    cairo_pattern_destroy(p);
  }
}

void wp_render_finish(void) {
  bloom_cache_free();
  ring_cache_free();
  text_cache_free();
  art_cache_free();
  wp_topo_finish();
  shadow_cache_free();
}

void wp_render(cairo_t *cr, const struct wallpaper_config *c, int pw, int ph,
               const struct wp_frame *f, bool full, bool ring, bool text) {
  cairo_save(cr);
  if (!full) {
    struct wp_rect r[WP_REGIONS];
    wp_regions(c, pw, ph, ring || f->nbars > 0,
               text || (c->media_title && f->media_alpha > 0), r);
    for (int i = 0; i < WP_REGIONS; ++i)
      if (r[i].w > 0 && r[i].h > 0)
        cairo_rectangle(cr, r[i].x, r[i].y, r[i].w, r[i].h);
    cairo_clip(cr);
  }
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  if (f->fade >= 1) { /* the end of a power-off: black */
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
    return;
  }
  bool topo = wp_topo(c);
  if (topo && f->map_under) {
    /* The GPU draws the map on a surface underneath. */
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
  } else if (topo) {
    /* The lines shake with everything else; the edge stretches in. */
    double k0 = pw / WP_REF_WIDTH;
    cairo_set_source_surface(cr, wp_topo_background(c, pw, ph),
                             round(f->shake_x * k0), round(f->shake_y * k0));
    cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
    cairo_paint(cr);
  } else {
    set_rgba(cr, c->background, 1.0);
    cairo_paint(cr);
  }
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  /* Windows and the shows take the map away and bring it back. */
  double dim = 1 - wp_map_brightness(f);
  if (topo && !f->map_under && dim > 0) {
    cairo_set_source_rgba(cr, 0, 0, 0, fmin(dim, 1));
    cairo_paint(cr);
  }
  double k = pw / WP_REF_WIDTH, r = c->logo_radius * k;
  /* A shake moves everything over the background, by whole pixels so the
   * cached tiles stay sharp. */
  cairo_translate(cr, round(f->shake_x * k), round(f->shake_y * k));
  double cx = pw / 2.0, cy = ph / 2.0;
  /* over the background, first */
  paint_bloom(cr, c, pw, ph, r, fmax(f->over, f->flash));
  if (f->nbars > 0 && c->viz_edges && c->viz_edge_smooth)
    draw_edges_smooth(cr, c, pw, ph, f);
  else if (f->nbars > 0 && c->viz_edges)
    draw_edges(cr, c, pw, ph, f);
  else if (f->nbars > 0)
    draw_ring(cr, c, pw, ph, r, k, f);

  draw_text(cr, c, pw, ph, f);
  draw_bar(cr, c, pw, f);
  if (f->over > 0)
    draw_streaks(cr, c, cx, cy, r, k, f);
  /* The logo reacts to the music: it grows on bass hits and its lambdas
   * slide out (both 0 without music). */
  double rp = r * (1 + c->react_pulse_scale * f->pulse) *
              (1 - fmin(fmax(f->logo_shrink, 0), 1));
  draw_hex(cr, c, cx, cy, r, rp, f);
  if (rp > 0 && topo)
    draw_glass_logo(cr, c, cx, cy, r, rp, k, f);
  else if (rp > 0)
    nixlogo_draw_ex(cr, cx, cy, rp, f->angle,
                    mix(c->logo, c->logo_glow, f->glow),
                    mix(c->logo_alt, c->logo_glow_alt, f->glow), 1.0, f->slide,
                    f->lambda_fade);
  if (f->crt_alpha > 0 && f->crt_w > 0 && f->crt_h > 0)
    draw_crt(cr, c, cx, cy, k, f);
  if (f->fade > 0) {
    cairo_set_source_rgba(cr, 0, 0, 0, f->fade);
    cairo_paint(cr);
  }
  cairo_restore(cr);
}
