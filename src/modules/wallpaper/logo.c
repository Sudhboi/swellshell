/* The glass snowflake's distance field for the GPU (gl.c): per buffer px
 * of the resting, unturned snowflake, the distance to the nearest lambda
 * edge, negative inside. The shader turns and pulses it, fills where it is
 * negative and strokes the outline where it is near 0. Built once per size
 * and outline width (a few ms) and cached like the map. */
#include <math.h>
#include <stdlib.h>

#include "nixlogo.h"
#include "util.h"
#include "wallpaper.h"

/* The lambdas' edges in snowflake units, from the flattened paths. */
#define MAX_SEGS 128
static struct seg {
  double ax, ay, bx, by;
} segs[MAX_SEGS];
static int nsegs;
static int seg_end[6]; /* lambda k's segments end at seg_end[k] */

static void load_segments(void) {
  if (nsegs)
    return;
  cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
  cairo_t *cr = cairo_create(probe);
  for (int k = 0; k < 6; ++k) {
    cairo_new_path(cr);
    nixlogo_lambda_path(cr, k, 0);
    cairo_path_t *p = cairo_copy_path_flat(cr);
    double x0 = 0, y0 = 0, x = 0, y = 0;
    for (int i = 0; i < p->num_data; i += p->data[i].header.length) {
      cairo_path_data_t *d = &p->data[i];
      double nx, ny;
      switch (d->header.type) {
      case CAIRO_PATH_MOVE_TO:
        x0 = x = d[1].point.x;
        y0 = y = d[1].point.y;
        continue;
      case CAIRO_PATH_LINE_TO:
        nx = d[1].point.x;
        ny = d[1].point.y;
        break;
      case CAIRO_PATH_CLOSE_PATH:
        nx = x0;
        ny = y0;
        break;
      default:
        continue;
      }
      if ((nx != x || ny != y) && nsegs < MAX_SEGS)
        segs[nsegs++] = (struct seg){x, y, nx, ny};
      x = nx;
      y = ny;
    }
    cairo_path_destroy(p);
    seg_end[k] = nsegs;
  }
  cairo_destroy(cr);
  cairo_surface_destroy(probe);
}

/* Distance from (x, y) to the edges and whether it is inside a lambda, in
 * snowflake units. */
static double field(double x, double y) {
  double d2 = INFINITY;
  bool inside = false;
  int k = 0;
  bool in_k = false;
  for (int i = 0; i < nsegs; ++i) {
    const struct seg *s = &segs[i];
    double ex = s->bx - s->ax, ey = s->by - s->ay;
    double t = ((x - s->ax) * ex + (y - s->ay) * ey) / (ex * ex + ey * ey);
    t = fmin(fmax(t, 0), 1);
    double dx = x - s->ax - ex * t, dy = y - s->ay - ey * t;
    d2 = fmin(d2, dx * dx + dy * dy);
    /* Crossings of a ray to the right, per lambda. */
    if ((s->ay > y) != (s->by > y) &&
        x < s->ax + (y - s->ay) / (s->by - s->ay) * ex)
      in_k = !in_k;
    if (i + 1 == seg_end[k]) {
      inside = inside || in_k;
      in_k = false;
      ++k;
    }
  }
  return inside ? -sqrt(d2) : sqrt(d2);
}

#define CACHE 4
static struct entry {
  struct wp_logo_sdf sdf;
  double r0;
  int pad;
} cache[CACHE];
static int cache_next;
static unsigned ids;

const struct wp_logo_sdf *wp_logo_sdf(const struct wallpaper_config *c,
                                      int pw) {
  double k = pw / WP_REF_WIDTH, r0 = c->logo_radius * k;
  /* Room for the widest outline and the antialiasing round it: the edge
   * texels must read as clear outside. */
  int pad = (int)ceil(fmax(c->topo_edge_width, c->topo_window_edge_width) *
                      k / 2) +
            3;
  for (int i = 0; i < CACHE; ++i)
    if (cache[i].sdf.d && cache[i].r0 == r0 && cache[i].pad == pad)
      return &cache[i].sdf;
  load_segments();
  double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
  for (int i = 0; i < nsegs; ++i) {
    x0 = fmin(x0, fmin(segs[i].ax, segs[i].bx));
    y0 = fmin(y0, fmin(segs[i].ay, segs[i].by));
    x1 = fmax(x1, fmax(segs[i].ax, segs[i].bx));
    y1 = fmax(y1, fmax(segs[i].ay, segs[i].by));
  }
  struct entry *e = &cache[cache_next];
  cache_next = (cache_next + 1) % CACHE;
  free(e->sdf.d);
  struct wp_logo_sdf *s = &e->sdf;
  s->x = floor(x0 * r0) - pad;
  s->y = floor(y0 * r0) - pad;
  s->w = (int)(ceil(x1 * r0) + pad - s->x);
  s->h = (int)(ceil(y1 * r0) + pad - s->y);
  s->d = xcalloc((size_t)s->w * s->h, sizeof(*s->d));
  for (int j = 0; j < s->h; ++j)
    for (int i = 0; i < s->w; ++i)
      s->d[j * s->w + i] =
          (float)(field((s->x + i + 0.5) / r0, (s->y + j + 0.5) / r0) * r0);
  s->id = ++ids;
  e->r0 = r0;
  e->pad = pad;
  return s;
}

void wp_logo_finish(void) {
  for (int i = 0; i < CACHE; ++i)
    free(cache[i].sdf.d);
  for (int i = 0; i < CACHE; ++i)
    cache[i] = (struct entry){0};
  nsegs = 0;
}
