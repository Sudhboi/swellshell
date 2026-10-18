/* What's playing in the snowflake's hole: the cover in a hexagon and
 * the progress line round its border. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "draw.h"
#include "render.h"
#include "wallpaper.h"

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
    art_cache.tile = cairo_image_surface_create(CAIRO_FORMAT_RGB24, side, side);
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
  cairo_pattern_t *p =
      wp_topo_edge_pattern(edge, 0, 0, r * M_SQRT1_2, a, solid);
  cairo_set_source(cr, p);
  cairo_stroke(cr);
  cairo_pattern_destroy(p);
  cairo_restore(cr);
}

bool wp_hex_shows(const struct wallpaper_config *c, const struct wp_frame *f) {
  if (f->media_alpha <= 0)
    return false;
  bool art = c->media_art && f->art && c->media_art_alpha > 0;
  bool line = c->media_line_width > 0 && f->progress >= 0;
  return art || line;
}

/* What's playing, in the snowflake's hole (resting radius r, now rp): the
 * cover in grey, with a line round its border, clockwise from the top, as
 * far as the track has played. The hexagon, the cover and the line turn with
 * the snowflake. Under topo the line takes the outline's colours (fixed to the
 * screen, spread across the hexagon): the track faint, the progress opaque,
 * like the bar. */
void wp_draw_hex(cairo_t *cr, const struct wallpaper_config *c, double cx,
                 double cy, double r, double rp, const struct wp_frame *f) {
  double alpha = fmin(f->media_alpha, 1);
  if (alpha <= 0)
    return;
  double hr = HOLE * c->media_hex_size * rp;
  double lw = c->media_line_width * rp / c->logo_radius;
  bool line = lw > 0 && f->progress >= 0;
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
      wp_topo_edges(c, f, edge);
    hex_path(cr, hl);
    /* Under topo the track is a faint trace of the progress's colours. */
    stroke_hex(cr, topo ? edge : NULL, c->media_track, f->angle, hr,
               topo ? 0.3 * alpha : alpha, topo);
    if (f->progress > 0)
      hex_trace(cr, hl, 0, fmin(f->progress, 1));
    stroke_hex(cr, topo ? edge : NULL, c->media_line, f->angle, hr, alpha,
               true);
  }
  cairo_restore(cr);
}

void wp_render_hex_finish(void) { art_cache_free(); }
