/* peek_layout "terrain": while the popup's key is held the camera tips the
 * flat contour map into the landscape its lines were cut from, seen in
 * perspective. The snowflake hovers over a summit raised under it and its
 * shadow falls on the slopes; HH:MM is raised out of the ground in front;
 * CPU, memory and battery stand beside it as mesas, one contour per 10%.
 * This is where those are: the GPU's map (gl.c) marches its rays through
 * them and the CPU (render_peek.c) pins labels to them.
 *
 * World space is the buffer's px with z up; at t 0 the eye looks straight
 * down from focal px over the middle, so every ground point shows where
 * the flat map draws it. */
#include <math.h>
#include <string.h>

#include "wallpaper.h"

enum wp_layout wp_peek_layout(const struct wallpaper_config *c) {
  if (!strcmp(c->peek_layout, "flanks"))
    return WP_LAYOUT_FLANKS;
  if (!strcmp(c->peek_layout, "terrain"))
    return WP_LAYOUT_TERRAIN;
  return WP_LAYOUT_SIDE;
}

double wp_peek_terrain(const struct wallpaper_config *c,
                       const struct wp_frame *f) {
  if (!f || !f->peek || !f->terrain ||
      wp_peek_layout(c) != WP_LAYOUT_TERRAIN)
    return 0;
  return fmin(fmax(f->peek_side, 0), 1);
}

static double smooth(double a, double b, double x) {
  double t = fmin(fmax((x - a) / (b - a), 0), 1);
  return t * t * (3 - 2 * t);
}

static double dot3(const double a[3], const double b[3]) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

bool wp_cam_project(const struct wp_cam *m, double x, double y, double z,
                    double *sx, double *sy, double *scale) {
  double v[3] = {x - m->o[0], y - m->o[1], z - m->o[2]};
  double depth = dot3(v, m->fw);
  if (depth < 1)
    return false;
  double s = m->focal / depth;
  *sx = m->cx + dot3(v, m->r) * s;
  *sy = m->cy + dot3(v, m->d) * s;
  if (scale)
    *scale = s;
  return true;
}

/* Tipped by pitch about the screen's x axis (the eye goes down the
 * screen and drops), then turned by yaw about the middle, looking at the
 * ground there (tz high), so the summit sits the same on every map. */
static void camera(const struct wallpaper_config *c, int pw, int ph,
                   double t, double tz, struct wp_cam *m) {
  double pitch = c->peek_terrain_pitch * M_PI / 180 * t;
  double yaw = c->peek_terrain_yaw * M_PI / 180 * t;
  double cp = cos(pitch), sp = sin(pitch), cy = cos(yaw), sy = sin(yaw);
  double r[3] = {1, 0, 0}, d[3] = {0, cp, -sp}, fw[3] = {0, -sp, -cp};
  double *out[3] = {m->r, m->d, m->fw}, *in[3] = {r, d, fw};
  for (int i = 0; i < 3; ++i) {
    out[i][0] = in[i][0] * cy - in[i][1] * sy;
    out[i][1] = in[i][0] * sy + in[i][1] * cy;
    out[i][2] = in[i][2];
  }
  m->focal = c->peek_terrain_focal * ph;
  m->cx = pw / 2.0;
  m->cy = ph / 2.0;
  for (int i = 0; i < 3; ++i)
    m->o[i] = (i == 0 ? m->cx : i == 1 ? m->cy : tz) - m->fw[i] * m->focal;
}

double wp_terrain_ground(const struct wallpaper_config *c, int pw, int ph,
                         double x, double y) {
  const struct wp_topo_grid *g = wp_topo_heights(c, pw, ph);
  double gx = (x - g->origin) / g->cell, gy = (y - g->origin) / g->cell;
  gx = fmin(fmax(gx, 0), g->nx - 1.001);
  gy = fmin(fmax(gy, 0), g->ny - 1.001);
  int i = (int)gx, j = (int)gy;
  double u = gx - i, v = gy - j;
  const float *h = g->h + (size_t)j * g->nx + i;
  return (h[0] * (1 - u) + h[1] * u) * (1 - v) +
         (h[g->nx] * (1 - u) + h[g->nx + 1] * u) * v;
}

void wp_terrain_at(const struct wallpaper_config *c, int pw, int ph,
                   const struct wp_frame *f, struct wp_terrain *T) {
  wp_terrain_at_t(c, pw, ph, f, wp_peek_terrain(c, f), T);
}

void wp_terrain_at_t(const struct wallpaper_config *c, int pw, int ph,
                     const struct wp_frame *f, double t,
                     struct wp_terrain *T) {
  memset(T, 0, sizeof *T);
  double k = pw / WP_REF_WIDTH;
  T->t = t;
  T->tf = smooth(0.1, 0.8, t);
  T->unit = c->peek_terrain_height * k * t;
  camera(c, pw, ph, t,
         T->unit * wp_terrain_ground(c, pw, ph, pw / 2.0, ph / 2.0), &T->cam);
  double yaw = c->peek_terrain_yaw * M_PI / 180 * t;
  double cy = cos(yaw), sy = sin(yaw);
  /* The light comes from far behind on the left, turned with the eye. */
  double lx = -0.3, ly = -0.5, lz = 1.0, ll = sqrt(lx * lx + ly * ly + 1);
  T->light[0] = (lx * cy - ly * sy) / ll;
  T->light[1] = (lx * sy + ly * cy) / ll;
  T->light[2] = lz / ll;

  double dx, dy;
  wp_logo_offset(c, pw, ph, f, &dx, &dy);
  T->lx = pw / 2.0 + dx;
  T->ly = ph / 2.0 + dy;
  T->sum_h = c->peek_terrain_summit * T->tf;
  T->sum_r = fmax(c->peek_terrain_summit_r * k, 1);
  double top = 1 + T->sum_h;

  /* The rest stand in front of the summit, turned to face the eye. */
  double mx = pw / 2.0, my = ph / 2.0;
#define PLACE(ox, oy, X, Y)                                                    \
  do {                                                                         \
    double ax = (ox) * k, ay = (oy) * k;                                       \
    X = mx + ax * cy - ay * sy;                                                \
    Y = my + ax * sy + ay * cy;                                                \
  } while (0)
  PLACE(c->peek_terrain_clock_x, c->peek_terrain_clock_y, T->clk_x, T->clk_y);
  T->clk_size = c->peek_terrain_clock_size * k;
  T->clk_depth = c->peek_terrain_clock_depth * k;
  /* Hinged up by a about its foot, facing the eye. Seen from pitch, its
   * height shows cos(pitch - a) of itself: stretched back. */
  double a = T->tf * M_PI / 2, ca = cos(a), sa = sin(a);
  double away[2] = {sy, -cy}; /* along the ground, away from the eye */
  T->clk_r[0] = cy, T->clk_r[1] = sy, T->clk_r[2] = 0;
  T->clk_u[0] = ca * away[0], T->clk_u[1] = ca * away[1], T->clk_u[2] = sa;
  /* n = u x r */
  T->clk_n[0] = T->clk_u[1] * T->clk_r[2] - T->clk_u[2] * T->clk_r[1];
  T->clk_n[1] = T->clk_u[2] * T->clk_r[0] - T->clk_u[0] * T->clk_r[2];
  T->clk_n[2] = T->clk_u[0] * T->clk_r[1] - T->clk_u[1] * T->clk_r[0];
  double pitch = c->peek_terrain_pitch * M_PI / 180 * t;
  T->clk_stretch = 1 / fmax(fabs(cos(pitch - a)), 0.4);

  T->mesa_r = fmax(c->peek_terrain_mesa_r * k, 1);
  int levels = c->topo_levels > 0 ? c->topo_levels : 1;
  for (int i = 0; i < 3; ++i) {
    PLACE(c->peek_terrain_mesa_x + i * c->peek_terrain_mesa_gap,
          c->peek_terrain_mesa_y, T->mesa_x[i], T->mesa_y[i]);
    /* Just over the contour below the ground there, so its pad shows no
     * line and each 10% up the side crosses one. */
    double v = wp_terrain_ground(c, pw, ph, T->mesa_x[i], T->mesa_y[i]) *
                   levels -
               0.5;
    T->mesa_floor[i] = (floor(v) + 0.52) / levels;
    double val = fmin(fmax(f ? f->terrain_vals[i] : 0, 0), 1);
    T->mesa_top[i] = T->mesa_floor[i] + val * 10.0 / levels;
    top = fmax(top, T->mesa_top[i]);
  }
#undef PLACE
  T->zmax = T->unit * top + 1;
  T->lz = T->unit * (wp_terrain_ground(c, pw, ph, T->lx, T->ly) + T->sum_h) +
          c->peek_terrain_hover * k * t;
  T->clk_o[0] = T->clk_x;
  T->clk_o[1] = T->clk_y;
  /* on the highest ground under it, so none of it sinks in */
  T->clk_o[2] = 0;
  for (int i = -8; i <= 8; ++i) {
    double u = i / 8.0 * 1.4 * T->clk_size;
    T->clk_o[2] =
        fmax(T->clk_o[2], wp_terrain_z(c, pw, ph, T, T->clk_x + T->clk_r[0] * u,
                                       T->clk_y + T->clk_r[1] * u));
  }
}

void wp_terrain_clock_point(const struct wp_terrain *T, double u, double v,
                            double p[3]) {
  for (int i = 0; i < 3; ++i)
    p[i] = T->clk_o[i] + T->clk_r[i] * u + T->clk_u[i] * v;
}

double wp_terrain_z(const struct wallpaper_config *c, int pw, int ph,
                    const struct wp_terrain *T, double x, double y) {
  double h = wp_terrain_ground(c, pw, ph, x, y);
  double sx = x - T->lx, sy = y - T->ly;
  h += T->sum_h * exp(-(sx * sx + sy * sy) / (T->sum_r * T->sum_r));
  for (int i = 0; i < 3; ++i) {
    double r = hypot(x - T->mesa_x[i], y - T->mesa_y[i]);
    double pad = 1 - smooth(T->mesa_r, 1.35 * T->mesa_r, r);
    h += (T->mesa_floor[i] - h) * pad * T->tf;
    double col = 1 - smooth(0.72 * T->mesa_r, T->mesa_r, r);
    h += (T->mesa_top[i] - h) * col * T->tf;
  }
  return T->unit * h;
}
