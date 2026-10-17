/* The topo style's background: contour lines of a made-up landscape over a
 * dark gradient, coloured left to right. It never moves, so it is drawn
 * once per buffer size (and config) and copied from there. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "util.h"
#include "wallpaper.h"

bool wp_topo(const struct wallpaper_config *c) {
  return strcmp(c->style, "classic") != 0;
}

double wp_map_brightness(const struct wp_frame *f) {
  double a = 1 - fmin(fmax(f->map_fade, 0), 1);
  return a * (1 - fmin(fmax(f->bg_fade, 0), 1));
}

/* ---- the landscape: domain-warped fractal gradient noise ---- */

static uint32_t hash3(uint32_t a, uint32_t b, uint32_t seed) {
  uint64_t x = ((uint64_t)a << 32 | b) ^
               ((uint64_t)seed * 0x9e3779b97f4a7c15u);
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9u;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebu;
  x ^= x >> 31;
  return (uint32_t)x;
}

static double fade5(double t) { return t * t * t * (t * (t * 6 - 15) + 10); }

/* Unit gradients, evenly round the circle. */
#define NGRAD 256
static double grad[NGRAD][2];

/* Gradient noise at (x, y), about -1..1. */
static double noise(double x, double y, uint32_t seed) {
  double fx = floor(x), fy = floor(y);
  int ix = (int)fx, iy = (int)fy;
  double u = x - fx, v = y - fy;
  double d[4];
  for (int k = 0; k < 4; ++k) {
    int cx = ix + (k & 1), cy = iy + (k >> 1);
    const double *g = grad[hash3((uint32_t)cx, (uint32_t)cy, seed) % NGRAD];
    d[k] = g[0] * (u - (k & 1)) + g[1] * (v - (k >> 1));
  }
  double su = fade5(u), sv = fade5(v);
  double top = d[0] + (d[1] - d[0]) * su, bot = d[2] + (d[3] - d[2]) * su;
  return 1.414 * (top + (bot - top) * sv);
}

/* Octaves, each turned a little so the lattice doesn't show. */
static double fbm(double x, double y, uint32_t seed) {
  double sum = 0, amp = 0.5;
  const double cs = cos(0.5), sn = sin(0.5);
  for (int o = 0; o < 3; ++o) {
    sum += amp * noise(x, y, seed + o * 101);
    double nx = (cs * x - sn * y) * 2.03, ny = (sn * x + cs * y) * 2.03;
    x = nx + 17.1;
    y = ny + 3.7;
    amp *= 0.4;
  }
  return sum;
}

static double height(double x, double y, uint32_t seed, double warp) {
  double qx = fbm(x + 1.7, y + 9.2, seed + 7000);
  double qy = fbm(x + 8.3, y + 2.8, seed + 9000);
  return fbm(x + warp * qx, y + warp * qy, seed);
}

/* How far the landscape reaches past the buffer's edges, reference px:
 * room for the shake, the rings and the sway to move it in from outside. */
#define MARGIN 240.0

/* The landscape over a pw x ph buffer and MARGIN round it: nx x ny samples
 * cell px apart from (origin, origin), from 0 (lowest) to 1. */
static double *make_heights(const struct wallpaper_config *c, int pw, int ph,
                            int *nx, int *ny, double *cell, double *origin) {
  for (int i = 0; i < NGRAD; ++i) {
    grad[i][0] = cos(2 * M_PI * i / NGRAD);
    grad[i][1] = sin(2 * M_PI * i / NGRAD);
  }
  double k = pw / WP_REF_WIDTH;
  *cell = fmax(2, round(5 * k));
  int m = (int)ceil(MARGIN * k / *cell);
  *origin = -m * *cell;
  *nx = (int)ceil(pw / *cell) + 2 + 2 * m;
  *ny = (int)ceil(ph / *cell) + 2 + 2 * m;
  size_t n = (size_t)*nx * *ny;
  double *h = xcalloc(n, sizeof(double));
  double lo = INFINITY, hi = -INFINITY, sc = c->topo_scale * k;
  for (int j = 0; j < *ny; ++j)
    for (int i = 0; i < *nx; ++i) {
      double v = height((*origin + i * *cell) / sc, (*origin + j * *cell) / sc,
                        (uint32_t)c->topo_seed, c->topo_warp);
      h[j * *nx + i] = v;
      lo = fmin(lo, v);
      hi = fmax(hi, v);
    }
  for (size_t i = 0; i < n; ++i)
    h[i] = hi > lo ? (h[i] - lo) / (hi - lo) : 0;
  return h;
}

/* ---- contours: marching squares, chained into lines ---- */

struct grid {
  int nx, ny;   /* samples */
  double cell;  /* px between samples */
  double origin; /* px: the first sample's x and y */
  double *h;    /* nx * ny */
  int nh;       /* horizontal edges; vertical ones follow */
  int (*nb)[2]; /* each edge's neighbours along the line, -1: none */
  int *seen;    /* the level an edge was walked at, + 1 */
  int *touched; /* the edges crossed at this level */
  int ntouched;
  double *pts; /* x, y pairs of one line */
};

static int hedge(const struct grid *g, int i, int j) {
  return j * (g->nx - 1) + i;
}
static int vedge(const struct grid *g, int i, int j) {
  return g->nh + j * g->nx + i;
}

/* Where edge e crosses level t, in px. */
static void edge_point(const struct grid *g, int e, double t, double *x,
                       double *y) {
  int i, j, i2, j2;
  if (e < g->nh) {
    i = e % (g->nx - 1);
    j = e / (g->nx - 1);
    i2 = i + 1;
    j2 = j;
  } else {
    i = (e - g->nh) % g->nx;
    j = (e - g->nh) / g->nx;
    i2 = i;
    j2 = j + 1;
  }
  double a = g->h[j * g->nx + i], b = g->h[j2 * g->nx + i2];
  double u = (t - a) / (b - a);
  *x = g->origin + (i + (i2 - i) * u) * g->cell;
  *y = g->origin + (j + (j2 - j) * u) * g->cell;
}

static void link_edges(struct grid *g, int a, int b) {
  int ends[2] = {a, b};
  for (int s = 0; s < 2; ++s) {
    int e = ends[s], o = ends[1 - s];
    if (g->nb[e][0] < 0 && g->nb[e][1] < 0)
      g->touched[g->ntouched++] = e;
    if (g->nb[e][0] < 0)
      g->nb[e][0] = o;
    else
      g->nb[e][1] = o;
  }
}

/* The cell's edges: 0 top, 1 right, 2 bottom, 3 left. */
static void cell_segments(struct grid *g, int i, int j, double t) {
  const double *h = g->h;
  int n = g->nx;
  double v0 = h[j * n + i], v1 = h[j * n + i + 1];
  double v2 = h[(j + 1) * n + i + 1], v3 = h[(j + 1) * n + i];
  int c = (v0 >= t) | (v1 >= t) << 1 | (v2 >= t) << 2 | (v3 >= t) << 3;
  if (c == 0 || c == 15)
    return;
  int e[4] = {hedge(g, i, j), vedge(g, i + 1, j), hedge(g, i, j + 1),
              vedge(g, i, j)};
  /* The edges each case joins, in pairs; the saddles (5: v0 and v2 high,
   * 10: v1 and v3) by the middle's height. */
  static const signed char join[16][4] = {
      {-1}, {3, 0}, {0, 1}, {3, 1}, {1, 2}, {3, 0, 1, 2}, {0, 2}, {2, 3},
      {2, 3}, {0, 2}, {0, 1, 2, 3}, {1, 2}, {3, 1}, {0, 1}, {3, 0}, {-1}};
  const signed char *p = join[c];
  if ((c == 5 || c == 10) && (v0 + v1 + v2 + v3) / 4 >= t)
    p = join[15 - c];
  link_edges(g, e[p[0]], e[p[1]]);
  if (c == 5 || c == 10)
    link_edges(g, e[p[2]], e[p[3]]);
}

/* A smooth curve through the line's points: quadratic pieces from midpoint
 * to midpoint, each bent towards its point. */
static void smooth_path(cairo_t *cr, const double *p, int n, bool closed) {
#define MX(a, b) ((p[2 * (a)] + p[2 * (b)]) / 2)
#define MY(a, b) ((p[2 * (a) + 1] + p[2 * (b) + 1]) / 2)
  if (n < 2)
    return;
  int first = closed ? 0 : 1, last = closed ? n : n - 1;
  double x, y;
  if (closed) {
    x = MX(n - 1, 0);
    y = MY(n - 1, 0);
  } else {
    x = p[0];
    y = p[1];
  }
  cairo_move_to(cr, x, y);
  if (!closed)
    cairo_line_to(cr, x = MX(0, 1), y = MY(0, 1));
  for (int i = first; i < last; ++i) {
    int nx = (i + 1) % n;
    double qx = p[2 * i], qy = p[2 * i + 1];
    double ex = MX(i, nx), ey = MY(i, nx);
    cairo_curve_to(cr, x + 2.0 / 3 * (qx - x), y + 2.0 / 3 * (qy - y),
                   ex + 2.0 / 3 * (qx - ex), ey + 2.0 / 3 * (qy - ey), ex, ey);
    x = ex;
    y = ey;
  }
  if (closed)
    cairo_close_path(cr);
  else
    cairo_line_to(cr, p[2 * (n - 1)], p[2 * (n - 1) + 1]);
#undef MX
#undef MY
}

/* Walk the line through edge start, adding it to the path. */
static void walk(cairo_t *cr, struct grid *g, int start, int level,
                 double t) {
  int n = 0, prev = -1, cur = start;
  bool closed = false;
  while (cur >= 0) {
    g->seen[cur] = level + 1;
    edge_point(g, cur, t, &g->pts[2 * n], &g->pts[2 * n + 1]);
    ++n;
    int next = -1;
    for (int s = 0; s < 2; ++s) {
      int o = g->nb[cur][s];
      if (o >= 0 && o != prev && g->seen[o] != level + 1) {
        next = o;
        break;
      }
    }
    if (next < 0 && n > 2)
      for (int s = 0; s < 2; ++s)
        closed |= g->nb[cur][s] == start;
    prev = cur;
    cur = next;
  }
  smooth_path(cr, g->pts, n, closed);
}

static void draw_level(cairo_t *cr, struct grid *g, int level, double t) {
  g->ntouched = 0;
  for (int j = 0; j < g->ny - 1; ++j)
    for (int i = 0; i < g->nx - 1; ++i)
      cell_segments(g, i, j, t);
  /* Open lines from their ends first, then the loops. */
  for (int pass = 0; pass < 2; ++pass)
    for (int k = 0; k < g->ntouched; ++k) {
      int e = g->touched[k];
      if (g->seen[e] == level + 1)
        continue;
      if (pass == 0 && g->nb[e][1] >= 0)
        continue;
      walk(cr, g, e, level, t);
    }
  for (int k = 0; k < g->ntouched; ++k)
    g->nb[g->touched[k]][0] = g->nb[g->touched[k]][1] = -1;
}

static void set_stop(cairo_pattern_t *p, double at, uint32_t c, double a) {
  cairo_pattern_add_color_stop_rgba(p, at, col_r(c), col_g(c), col_b(c),
                                    col_a(c) * a);
}

static cairo_surface_t *draw_topo(const struct wallpaper_config *c, int pw,
                                  int ph) {
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, pw, ph);
  cairo_t *cr = cairo_create(s);
  cairo_pattern_t *bg = cairo_pattern_create_linear(0, 0, pw, 0);
  set_stop(bg, 0, c->topo_bg_left, 1);
  set_stop(bg, 1, c->topo_bg_right, 1);
  cairo_set_source(cr, bg);
  cairo_paint(cr);
  cairo_pattern_destroy(bg);

  double k = pw / WP_REF_WIDTH;
  if (c->topo_line_width > 0 && c->topo_line_alpha > 0) {
    struct grid g = {0};
    g.h = make_heights(c, pw, ph, &g.nx, &g.ny, &g.cell, &g.origin);
    double lo = 0, hi = 1;
    g.nh = (g.nx - 1) * g.ny;
    int ne = g.nh + g.nx * (g.ny - 1);
    g.nb = xcalloc(ne, sizeof(*g.nb));
    for (int e = 0; e < ne; ++e)
      g.nb[e][0] = g.nb[e][1] = -1;
    g.seen = xcalloc(ne, sizeof(int));
    g.touched = xcalloc(ne, sizeof(int));
    g.pts = xcalloc((size_t)2 * ne, sizeof(double));

    /* The lines' colours only change across: one row of them, repeated
     * down, is far cheaper to draw from than the gradient itself. */
    cairo_surface_t *row =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, 1);
    cairo_t *rc = cairo_create(row);
    cairo_pattern_t *ln = cairo_pattern_create_linear(0, 0, pw, 0);
    set_stop(ln, 0, c->topo_line_left, c->topo_line_alpha);
    set_stop(ln, 0.5, c->topo_line_mid, c->topo_line_alpha);
    set_stop(ln, 1, c->topo_line_right, c->topo_line_alpha);
    cairo_set_source(rc, ln);
    cairo_paint(rc);
    cairo_destroy(rc);
    cairo_pattern_destroy(ln);
    ln = cairo_pattern_create_for_surface(row);
    cairo_pattern_set_extend(ln, CAIRO_EXTEND_REPEAT);
    cairo_pattern_set_filter(ln, CAIRO_FILTER_NEAREST);
    cairo_surface_destroy(row);
    cairo_set_source(cr, ln);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    int every = c->topo_index_every;
    for (int pass = 0; pass < (every > 0 ? 2 : 1); ++pass) {
      for (int l = 0; l < c->topo_levels; ++l) {
        bool index = every > 0 && l % every == every / 2;
        if (index != (pass == 1))
          continue;
        double t = lo + (hi - lo) * (l + 0.5) / c->topo_levels;
        draw_level(cr, &g, l, t);
      }
      cairo_set_line_width(cr, c->topo_line_width * k *
                                   (pass ? c->topo_index_width : 1));
      cairo_stroke(cr);
    }
    cairo_pattern_destroy(ln);
    free(g.h);
    free(g.nb);
    free(g.seen);
    free(g.touched);
    free(g.pts);
  }
  cairo_destroy(cr);
  cairo_surface_flush(s);
  return s;
}

/* ---- the cache: one map per buffer size, a few sizes ---- */

struct topo_key {
  int pw, ph;
  uint32_t bg_left, bg_right, line_left, line_mid, line_right;
  int seed, levels, index_every;
  double scale, warp, line_width, index_width, line_alpha;
};

#define TOPO_CACHED 4
static struct {
  struct topo_key key;
  cairo_surface_t *s;
  unsigned used;
} cache[TOPO_CACHED];
static unsigned use_clock;

cairo_surface_t *wp_topo_background(const struct wallpaper_config *c, int pw,
                                    int ph) {
  struct topo_key key;
  memset(&key, 0, sizeof(key)); /* padding too: keys are memcmp'd */
  key.pw = pw;
  key.ph = ph;
  key.bg_left = c->topo_bg_left;
  key.bg_right = c->topo_bg_right;
  key.line_left = c->topo_line_left;
  key.line_mid = c->topo_line_mid;
  key.line_right = c->topo_line_right;
  key.seed = c->topo_seed;
  key.levels = c->topo_levels;
  key.index_every = c->topo_index_every;
  key.scale = c->topo_scale;
  key.warp = c->topo_warp;
  key.line_width = c->topo_line_width;
  key.index_width = c->topo_index_width;
  key.line_alpha = c->topo_line_alpha;
  int slot = -1;
  for (int i = 0; i < TOPO_CACHED; ++i)
    if (cache[i].s && memcmp(&cache[i].key, &key, sizeof(key)) == 0) {
      cache[i].used = ++use_clock;
      return cache[i].s;
    }
  /* An empty slot, or the one used longest ago. */
  for (int i = 0; i < TOPO_CACHED; ++i) {
    if (!cache[i].s) {
      slot = i;
      break;
    }
    if (slot < 0 || cache[i].used < cache[slot].used)
      slot = i;
  }
  if (cache[slot].s)
    cairo_surface_destroy(cache[slot].s);
  cache[slot].key = key;
  cache[slot].s = draw_topo(c, pw, ph);
  cache[slot].used = ++use_clock;
  return cache[slot].s;
}

/* The heights for the GPU, a few sizes. */
static struct {
  int pw, ph, seed;
  double scale, warp;
  struct wp_topo_grid grid;
  unsigned used;
} hcache[TOPO_CACHED];

const struct wp_topo_grid *wp_topo_heights(const struct wallpaper_config *c,
                                           int pw, int ph) {
  int slot = -1;
  for (int i = 0; i < TOPO_CACHED; ++i)
    if (hcache[i].grid.h && hcache[i].pw == pw && hcache[i].ph == ph &&
        hcache[i].seed == c->topo_seed && hcache[i].scale == c->topo_scale &&
        hcache[i].warp == c->topo_warp) {
      hcache[i].used = ++use_clock;
      return &hcache[i].grid;
    }
  for (int i = 0; i < TOPO_CACHED; ++i) {
    if (!hcache[i].grid.h) {
      slot = i;
      break;
    }
    if (slot < 0 || hcache[i].used < hcache[slot].used)
      slot = i;
  }
  free(hcache[slot].grid.h);
  struct wp_topo_grid *g = &hcache[slot].grid;
  double *h = make_heights(c, pw, ph, &g->nx, &g->ny, &g->cell, &g->origin);
  size_t n = (size_t)g->nx * g->ny;
  g->h = xcalloc(n, sizeof(float));
  for (size_t i = 0; i < n; ++i)
    g->h[i] = (float)h[i];
  free(h);
  g->id = ++use_clock;
  hcache[slot].pw = pw;
  hcache[slot].ph = ph;
  hcache[slot].seed = c->topo_seed;
  hcache[slot].scale = c->topo_scale;
  hcache[slot].warp = c->topo_warp;
  hcache[slot].used = use_clock;
  return g;
}

void wp_topo_finish(void) {
  for (int i = 0; i < TOPO_CACHED; ++i) {
    free(hcache[i].grid.h);
    hcache[i].grid.h = NULL;
  }
  for (int i = 0; i < TOPO_CACHED; ++i) {
    if (cache[i].s)
      cairo_surface_destroy(cache[i].s);
    cache[i].s = NULL;
  }
}
