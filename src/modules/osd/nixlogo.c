/* The Nix snowflake, drawn as six filled lambda polygons. */
#include "nixlogo.h"

#include <stdint.h>

static const double PI = 3.14159265358979323846;

/* One lambda from NixOS/nixos-artwork logo/nix-snowflake-colours.svg,
 * centred on the snowflake centre and scaled to outer radius 1 (y down). */
static const double lambda_pts[][2] = {
    {-0.38986, 0.02158}, {0.09750, 0.86581},  {-0.12647, 0.86791},
    {-0.25659, 0.64110}, {-0.38763, 0.86670}, {-0.49891, 0.86665},
    {-0.55591, 0.76818}, {-0.36921, 0.44716}, {-0.50174, 0.21654},
};
static const int lambda_n = sizeof(lambda_pts) / sizeof(lambda_pts[0]);

/* Set the cairo source from 0xRRGGBBAA, times alpha. */
static void set_rgba(cairo_t *cr, uint32_t c, double alpha) {
  cairo_set_source_rgba(cr, ((c >> 24) & 0xff) / 255.0,
                        ((c >> 16) & 0xff) / 255.0, ((c >> 8) & 0xff) / 255.0,
                        (c & 0xff) / 255.0 * alpha);
}

void nixlogo_draw(cairo_t *cr, const struct osd_config *c, double cx,
                  double cy, double r, double angle, bool grey, double alpha) {
  cairo_save(cr);
  cairo_translate(cr, cx, cy);
  cairo_rotate(cr, angle);
  cairo_scale(cr, r, r);
  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);

  for (int k = 0; k < 6; k++) {
    /* Even k is dark in the SVG, odd k light. */
    if (grey)
      set_rgba(cr, c->muted, alpha * (k % 2 ? 0.75 : 1.0));
    else
      set_rgba(cr, k % 2 ? c->nix_light : c->nix_dark, alpha);

    cairo_save(cr);
    cairo_rotate(cr, k * PI / 3.0);
    cairo_move_to(cr, lambda_pts[0][0], lambda_pts[0][1]);
    for (int i = 1; i < lambda_n; i++)
      cairo_line_to(cr, lambda_pts[i][0], lambda_pts[i][1]);
    cairo_close_path(cr);
    cairo_restore(cr);
    cairo_fill(cr);
  }
  cairo_restore(cr);
}
