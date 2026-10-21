/* The snowflake drawn by cairo: the topo style's glass logo with its
 * shadow and neon glow, the classic bloom, and the speed lines. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "draw.h"
#include "nixlogo.h"
#include "render.h"
#include "util.h"
#include "wallpaper.h"

/* Speed lines sit on rings this far out (x logo radius). */
static const double streak_rings[] = {1.08, 1.16, 1.24};

/* A soft glow behind the logo, growing with the overspeed. */
static void draw_bloom(cairo_t *cr, const struct wallpaper_config *c, double cx,
                       double cy, double r, double over) {
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

static void bloom_cache_free(void) {
  if (bloom_cache.tile)
    cairo_surface_destroy(bloom_cache.tile);
  bloom_cache.tile = NULL;
}

void wp_paint_bloom(cairo_t *cr, const struct wallpaper_config *c, int pw,
                    int ph, double r, double over) {
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
    bloom_cache_free();
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
  cairo_set_source_surface(cr, bloom_cache.tile, bloom_cache.x, bloom_cache.y);
  int size = cairo_image_surface_get_width(bloom_cache.tile);
  cairo_rectangle(cr, bloom_cache.x, bloom_cache.y, size, size);
  cairo_fill(cr);
  cairo_restore(cr);
}

/* Arcs circling the logo, turning with it: bright at the leading end and
 * fading along a tail that lengthens with the overspeed. Under topo they
 * take the snowflake's outline colours, fixed to the screen. */
void wp_draw_streaks(cairo_t *cr, const struct wallpaper_config *c, double cx,
                     double cy, double r, double k, const struct wp_frame *f) {
  int n = c->streak_count, segs = 8;
  double len = c->streak_length * M_PI / 180 * f->over;
  if (n <= 0 || len <= 0 || c->streak_width <= 0)
    return;
  cairo_set_line_width(cr, c->streak_width * k);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
  uint32_t edge[3];
  bool topo = wp_topo(c);
  if (topo)
    wp_topo_edges(c, f, edge);
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
      double a = c->streak_alpha * f->over * (1 - (double)s / segs);
      cairo_pattern_t *p = NULL;
      if (topo) {
        p = wp_topo_edge_pattern(edge, cx, cy, r * STREAK_OUTER, a, true);
        cairo_set_source(cr, p);
      } else {
        set_rgba(cr, c->streak, a);
      }
      cairo_new_path(cr);
      if (f->dir > 0)
        cairo_arc(cr, cx, cy, rr, a1, a0);
      else
        cairo_arc(cr, cx, cy, rr, a0, a1);
      cairo_stroke(cr);
      if (p)
        cairo_pattern_destroy(p);
    }
  }
}

/* The glass snowflake's outline colours, lit towards white by its glow. */
void wp_topo_edges(const struct wallpaper_config *c, const struct wp_frame *f,
                   uint32_t edge[3]) {
  double lit = 0.35 * fmin(fmax(f->glow, 0), 1);
  edge[0] = mix(c->topo_edge_a, 0xffffffffu, lit);
  edge[1] = mix(c->topo_edge_mid, 0xffffffffu, lit);
  edge[2] = mix(c->topo_edge_b, 0xffffffffu, lit);
}

/* The outline's gradient, top left to bottom right, from cx - e, cy - e to
 * cx + e, cy + e: each colour's alpha times a, or opaque times a when
 * solid. */
cairo_pattern_t *wp_topo_edge_pattern(const uint32_t edge[3], double cx,
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

/* The glass snowflake's shadow: a silhouette at the resting radius,
 * blurred, as a mask turned and pulsed with the snowflake. [0] is the
 * whole snowflake; [1] lambda 0 alone, drawn under each lambda in turn
 * while they slide or fade apart (in a show). */
struct shadow {
  cairo_surface_t *mask; /* A8 */
  double x, y;           /* its top left, px from the snowflake's centre */
  double r, blur, line;  /* resting radius, blur radius, outline width, px */
  unsigned id;           /* changes with the mask */
};
static struct shadow shadow_cache[4];

static void shadow_cache_free(void) {
  for (int i = 0; i < 4; ++i) {
    if (shadow_cache[i].mask)
      cairo_surface_destroy(shadow_cache[i].mask);
    shadow_cache[i].mask = NULL;
  }
}

/* Three box blurs of radius b over an A8 image, rows then columns: close
 * to a Gaussian. */
void wp_blur_a8(unsigned char *d, int w, int h, int stride, int b) {
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

/* which: 0 = the six lambdas' silhouette, 1 = lambda 0's; 2 and 3 the same
 * as outlines line px wide (the topo glow). */
static const struct shadow *shadow_mask(int which, double r, double blur,
                                        double line) {
  struct shadow *sc = &shadow_cache[which];
  if (sc->mask && sc->r == r && sc->blur == blur && sc->line == line)
    return sc;
  if (sc->mask)
    cairo_surface_destroy(sc->mask);
  cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
  cairo_t *pc = cairo_create(probe);
  silhouette(pc, r, which % 2 == 0);
  double x0, y0, x1, y1;
  cairo_path_extents(pc, &x0, &y0, &x1, &y1);
  cairo_destroy(pc);
  cairo_surface_destroy(probe);
  int b = (int)ceil(blur / 3);
  double pad = 3 * b + line / 2 + 2;
  x0 = floor(x0 - pad);
  y0 = floor(y0 - pad);
  int w = (int)ceil(x1 + pad - x0), h = (int)ceil(y1 + pad - y0);
  cairo_surface_t *m = cairo_image_surface_create(CAIRO_FORMAT_A8, w, h);
  cairo_t *mc = cairo_create(m);
  cairo_translate(mc, -x0, -y0);
  silhouette(mc, r, which % 2 == 0);
  if (which >= 2) {
    cairo_set_line_width(mc, line);
    cairo_set_line_join(mc, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke(mc);
  } else {
    cairo_fill(mc);
  }
  cairo_destroy(mc);
  cairo_surface_flush(m);
  if (b > 0)
    wp_blur_a8(cairo_image_surface_get_data(m), w, h,
               cairo_image_surface_get_stride(m), b);
  cairo_surface_mark_dirty(m);
  static unsigned ids;
  *sc = (struct shadow){m, x0, y0, r, blur, line, ++ids};
  return sc;
}

/* Paint a mask (resting radius r0) under each lambda, turned, pulsed to r
 * and slid with them, offset ox, oy: black at alpha a, or the outline's
 * colours (fixed to the screen) when edge. One mask for the lot when whole. */
static void paint_lambda_mask(cairo_t *cr, const struct shadow *m, bool whole,
                              const struct wp_frame *f, const double alpha[6],
                              double cx, double cy, double ox, double oy,
                              double r0, double r, const uint32_t *edge,
                              double a) {
  double dx, dy;
  nixlogo_lambda_dir(&dx, &dy);
  for (int i = 0; i < (whole ? 1 : 6); ++i) {
    if (alpha[i] <= 0)
      continue;
    cairo_save(cr);
    cairo_pattern_t *src = NULL;
    if (edge) {
      /* Set before the turn, so the colours stay put. */
      src = wp_topo_edge_pattern(edge, cx, cy, 0.6 * r, a * alpha[i], true);
      cairo_set_source(cr, src);
      cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
    }
    cairo_translate(cr, cx + ox, cy + oy);
    cairo_rotate(cr, f->angle + i * M_PI / 3);
    cairo_scale(cr, r, r);
    cairo_translate(cr, dx * f->slide[i], dy * f->slide[i]);
    cairo_scale(cr, 1 / r0, 1 / r0);
    if (!edge)
      cairo_set_source_rgba(cr, 0, 0, 0, a * alpha[i]);
    /* It's blurred: for the dark shadow the nearest pixel is as good as
     * any filter; the bright glow shows grain without a bilinear one. */
    cairo_pattern_t *mp = cairo_pattern_create_for_surface(m->mask);
    cairo_matrix_t mm;
    cairo_matrix_init_translate(&mm, -m->x, -m->y);
    cairo_pattern_set_matrix(mp, &mm);
    cairo_pattern_set_filter(mp, edge ? CAIRO_FILTER_BILINEAR
                                      : CAIRO_FILTER_NEAREST);
    cairo_mask(cr, mp);
    cairo_pattern_destroy(mp);
    if (src)
      cairo_pattern_destroy(src);
    cairo_restore(cr);
  }
}

void wp_glass_look(const struct wallpaper_config *c, const struct wp_frame *f,
                   double k, struct wp_glass *g) {
  /* Toned down as the map fades for a window. */
  double win = fmin(fmax(f->map_fade, 0), 1);
  g->fill = mix(mix(c->topo_logo_fill, c->topo_logo_glow, f->glow),
                c->topo_window_fill, win);
  wp_topo_edges(c, f, g->edge);
  g->edge_alpha = 1 + (c->topo_window_edge_alpha - 1) * win;
  g->edge_width = (c->topo_edge_width +
                   (c->topo_window_edge_width - c->topo_edge_width) * win) *
                  k;
  /* Past spin_visible_max (and in the intro's flash) the outline glows. */
  g->neon = c->topo_neon_alpha * fmin(fmax(fmax(f->over, f->flash), 0), 1);
}

bool wp_logo_mask(const struct wallpaper_config *c, int pw, int which,
                  struct wp_logo_mask *m) {
  double k = pw / WP_REF_WIDTH, r0 = c->logo_radius * k;
  double np = c->topo_neon_px * k;
  if (r0 <= 0 ||
      (which == 0 && (c->topo_shadow_px <= 0 || c->topo_shadow_alpha <= 0)) ||
      (which == 1 && np <= 0))
    return false;
  const struct shadow *sc = which == 0
                                ? shadow_mask(0, r0, c->topo_shadow_px * k, 0)
                                : shadow_mask(2, r0, np / 2, np * 0.4);
  *m = (struct wp_logo_mask){
      .data = cairo_image_surface_get_data(sc->mask),
      .w = cairo_image_surface_get_width(sc->mask),
      .h = cairo_image_surface_get_height(sc->mask),
      .stride = cairo_image_surface_get_stride(sc->mask),
      .x = sc->x,
      .y = sc->y,
      .id = sc->id,
  };
  return true;
}

double wp_logo_reach(const struct wallpaper_config *c, int pw) {
  double k = pw / WP_REF_WIDTH;
  double sh = c->react_shake ? ceil(c->react_shake_px * pw / WP_REF_WIDTH) : 0;
  /* Pulsed to the most; the shadow (offset, blurred), the glow and the
   * outline past the lambdas, as shadow_mask() pads them. */
  double ext = fmax(fmax(1.8 * c->topo_shadow_px, 0.75 * c->topo_neon_px),
                    fmax(c->topo_edge_width, c->topo_window_edge_width) / 2) *
                   k +
               6;
  return (c->logo_radius * k + ext) * (1 + c->react_pulse_scale) + sh;
}

void wp_logo_offset(const struct wallpaper_config *c, int pw, int ph,
                    const struct wp_frame *f, double *dx, double *dy) {
  *dx = *dy = 0;
  if (!f || (f->wander_x == 0 && f->wander_y == 0))
    return;
  /* Its box (padded 1 px) stays whole on screen, so it keeps its size. */
  double r = wp_logo_reach(c, pw) + 2;
  *dx = round(f->wander_x * fmax(floor(pw / 2.0 - r), 0));
  *dy = round(f->wander_y * fmax(floor(ph / 2.0 - r), 0));
}

struct wp_rect wp_logo_box(const struct wallpaper_config *c, int pw, int ph,
                           const struct wp_frame *f) {
  double r = wp_logo_reach(c, pw), dx, dy;
  wp_logo_offset(c, pw, ph, f, &dx, &dy);
  double cx = pw / 2.0 + dx, cy = ph / 2.0 + dy;
  return wp_clamp_rect(cx - r, cy - r, cx + r, cy + r, pw, ph);
}

bool wp_logo_whole(const struct wp_frame *f) {
  if (f->logo_shrink != 0 || f->logo_untraced != 0 || f->logo_unfilled != 0)
    return false;
  for (int i = 0; i < 6; ++i)
    if (f->slide[i] != 0 || f->lambda_fade[i] != 0)
      return false;
  return true;
}

/* The topo style's snowflake (resting radius r0, now r): dark glass
 * lambdas with the contour lines showing through, a gradient outline fixed
 * to the screen (it doesn't turn with them) and a soft shadow down and to
 * the right. */
void wp_draw_glass_logo(cairo_t *cr, const struct wallpaper_config *c,
                        double cx, double cy, double r0, double r, double k,
                        const struct wp_frame *f) {
  double alpha[6];
  for (int i = 0; i < 6; ++i)
    alpha[i] = 1 - fmin(fmax(f->lambda_fade[i], 0), 1);
  double s = r / r0; /* sizes follow the pulse and the shrink */
  double filled = 1 - fmin(fmax(f->logo_unfilled, 0), 1);
  double traced = 1 - fmin(fmax(f->logo_untraced, 0), 1);

  /* One mask for the lot unless the lambdas slide or fade apart. */
  bool whole = true;
  for (int i = 0; i < 6; ++i)
    whole = whole && f->slide[i] == 0 && alpha[i] == 1;
  double sd = c->topo_shadow_px * k * s;
  if (sd > 0 && c->topo_shadow_alpha > 0 && filled > 0) {
    const struct shadow *m =
        shadow_mask(whole ? 0 : 1, r0, c->topo_shadow_px * k, 0);
    paint_lambda_mask(cr, m, whole, f, alpha, cx, cy, 0.7 * sd, 0.7 * sd, r0, r,
                      NULL, c->topo_shadow_alpha * filled);
  }

  struct wp_glass g;
  wp_glass_look(c, f, k, &g);
  uint32_t fill = g.fill, *edge = g.edge;
  double ea = g.edge_alpha;

  /* The glow: a blurred outline, cached, under the glass. */
  double np = c->topo_neon_px * k;
  g.neon *= traced;
  if (g.neon > 0 && np > 0) {
    const struct shadow *m = shadow_mask(whole ? 2 : 3, r0, np / 2, np * 0.4);
    paint_lambda_mask(cr, m, whole, f, alpha, cx, cy, 0, 0, r0, r, edge,
                      g.neon);
  }
  double ew = g.edge_width * s;
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
    set_rgba(cr, fill, a * filled);
    if (ew <= 0 || traced <= 0) {
      cairo_fill(cr);
      continue;
    }
    cairo_fill_preserve(cr);
    /* Traced: each lambda's outline drawn this far round from its start. */
    double dash[2] = {traced * nixlogo_lambda_length() * r, 0};
    dash[1] = dash[0] > 0 ? nixlogo_lambda_length() * r : 0;
    if (traced < 1)
      cairo_set_dash(cr, dash, 2, 0);
    /* Over the lambdas' extent (not the bounding box's corners). */
    cairo_pattern_t *p =
        wp_topo_edge_pattern(edge, cx, cy, 0.6 * r, a * ea, false);
    cairo_set_source(cr, p);
    cairo_set_line_width(cr, ew);
    cairo_stroke(cr);
    cairo_set_dash(cr, NULL, 0, 0);
    cairo_pattern_destroy(p);
  }
}

void wp_render_logo_finish(void) {
  bloom_cache_free();
  shadow_cache_free();
}
