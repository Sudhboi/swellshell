/* Cairo drawing of the wallpaper: a port of make_black_bar_nixos.py.
 * This file: the damage regions, the battery bar, the title, the CRT
 * and the frame put together; render_logo.c, render_hex.c and
 * render_viz.c draw the rest. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <pango/pangocairo.h>

#include "config.h"
#include "draw.h"
#include "nixlogo.h"
#include "render.h"
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

struct wp_rect wp_clamp_rect(double x0, double y0, double x1, double y1, int pw,
                             int ph) {
  int ix0 = (int)fmax(floor(x0) - 1, 0), iy0 = (int)fmax(floor(y0) - 1, 0);
  int ix1 = (int)fmin(ceil(x1) + 1, pw), iy1 = (int)fmin(ceil(y1) + 1, ph);
  return (struct wp_rect){ix0, iy0, ix1 > ix0 ? ix1 - ix0 : 0,
                          iy1 > iy0 ? iy1 - iy0 : 0};
}

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

/* The strip side s (0 left, 1 right) of the edge bars fills at full. */
void wp_edge_box(const struct wallpaper_config *c, int pw, int ph, int s,
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
  wp_regions_ex(c, pw, ph, ring, text, false, false, out);
}

void wp_regions_ex(const struct wallpaper_config *c, int pw, int ph, bool ring,
                   bool text, bool logo_gpu, bool logo_box,
                   struct wp_rect out[WP_REGIONS]) {
  double x, y, w, h;
  bar_box(c, pw, &x, &y, &w, &h);
  /* A shake moves everything by up to this much. */
  double sh = c->react_shake ? ceil(c->react_shake_px * pw / WP_REF_WIDTH) : 0;
  out[0] = wp_clamp_rect(x - sh, y - sh, x + w + sh, y + h + sh, pw, ph);
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
                    (fmax(1.8 * c->topo_shadow_px, 0.75 * c->topo_neon_px) +
                     c->topo_edge_width / 2) *
                        k +
                    4);
  if (ring && !c->viz_edges)
    r = fmax(r, wp_ring_outer(c, c->logo_radius * k, k) + 1);
  /* The GPU's snowflake leaves only the hexagon in the middle. */
  if (logo_gpu && !logo_box)
    r = c->media ? HOLE * c->media_hex_size * c->logo_radius * k *
                           (1 + c->react_pulse_scale) +
                       2
                 : 0;
  r += sh;
  out[1] = r > 0 ? wp_clamp_rect(pw / 2.0 - r, ph / 2.0 - r, pw / 2.0 + r,
                                 ph / 2.0 + r, pw, ph)
                 : (struct wp_rect){0, 0, 0, 0};
  for (int s = 0; s < 2; ++s) {
    out[2 + s] = (struct wp_rect){0, 0, 0, 0};
    if (ring && c->viz_edges) {
      double x0, y0, x1, y1;
      wp_edge_box(c, pw, ph, s, &x0, &y0, &x1, &y1);
      /* The smooth curve's line reaches past a full level by half its
       * width. */
      double rim = c->viz_edge_smooth ? c->viz_edge_rim_width * k / 2 : 0;
      if (s)
        x0 -= rim;
      else
        x1 += rim;
      out[2 + s] = wp_clamp_rect(x0 - sh, y0 - sh, x1 + sh, y1 + sh, pw, ph);
    }
  }
  out[4] = (struct wp_rect){0, 0, 0, 0};
  if (text) {
    text_box(c, pw, ph, &x, &y, &w, &h);
    out[4] = wp_clamp_rect(x - sh, y - sh, x + w + sh, y + h + sh, pw, ph);
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

/* What the topo shows do to the map that cairo can: black outside in_r
 * and inside out_r, and cpu_dim over the lot. */
static void show_map_mask(cairo_t *cr, int pw, int ph,
                          const struct wp_show_map *m) {
  double k = pw / WP_REF_WIDTH, cx = pw / 2.0, cy = ph / 2.0;
  if (m->cpu_dim > 0) {
    cairo_set_source_rgba(cr, 0, 0, 0, fmin(m->cpu_dim, 1));
    cairo_paint(cr);
  }
  double ri = m->in_r * k, ro = m->out_r * k;
  if (ri <= 0) {
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);
  } else if (ri < hypot(pw, ph)) {
    cairo_pattern_t *p = cairo_pattern_create_radial(cx, cy, 0, cx, cy, ri);
    cairo_pattern_add_color_stop_rgba(
        p, fmax(1 - fmax(m->in_band, 1) * k / ri, 0), 0, 0, 0, 0);
    cairo_pattern_add_color_stop_rgba(p, 1, 0, 0, 0, 1);
    cairo_pattern_set_extend(p, CAIRO_EXTEND_PAD);
    cairo_set_source(cr, p);
    cairo_paint(cr);
    cairo_pattern_destroy(p);
  }
  if (ro > 0) {
    cairo_pattern_t *p = cairo_pattern_create_radial(cx, cy, 0, cx, cy, ro);
    cairo_pattern_add_color_stop_rgba(
        p, fmax(1 - fmax(m->out_band, 1) * k / ro, 0), 0, 0, 0, 1);
    cairo_pattern_add_color_stop_rgba(p, 1, 0, 0, 0, 0);
    cairo_pattern_set_extend(p, CAIRO_EXTEND_PAD);
    cairo_set_source(cr, p);
    cairo_paint(cr);
    cairo_pattern_destroy(p);
  }
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

/* The title and artist's opacity: gone as the snowflake moves aside for
 * the popup's details. */
static double text_alpha(const struct wallpaper_config *c,
                         const struct wp_frame *f) {
  return fmin(f->media_alpha, 1) * (1 - wp_peek_side(c, f));
}

static void draw_text(cairo_t *cr, const struct wallpaper_config *c, int pw,
                      int ph, const struct wp_frame *f) {
  if (!c->media_title || text_alpha(c, f) <= 0 || !f->title)
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
      wp_blur_a8(cairo_image_surface_get_data(m), tw2, th2,
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
  cairo_paint_with_alpha(cr, text_alpha(c, f));
  cairo_restore(cr);
}

static uint64_t fnv(uint64_t h, const char *s) {
  for (; *s; ++s)
    h = (h ^ (unsigned char)*s) * 0x100000001b3ull;
  return (h ^ 0xff) * 0x100000001b3ull; /* the end, so "ab","c" != "a","bc" */
}

void wp_text_key(const struct wallpaper_config *c, int pw,
                 const struct wp_frame *f, struct wp_text_key *k) {
  /* What draw_text() draws, and where. */
  *k = (struct wp_text_key){0};
  if (!c->media_title || text_alpha(c, f) <= 0 || !f->title)
    return;
  double s = pw / WP_REF_WIDTH;
  k->shown = true;
  k->alpha = text_alpha(c, f);
  k->x = lround(f->shake_x * s);
  k->y = lround(f->shake_y * s);
  k->text =
      fnv(fnv(0xcbf29ce484222325ull, f->title), f->artist ? f->artist : "");
}

bool wp_text_key_eq(const struct wp_text_key *a, const struct wp_text_key *b) {
  return a->shown == b->shown && a->alpha == b->alpha && a->x == b->x &&
         a->y == b->y && a->text == b->text;
}

void wp_render_finish(void) {
  wp_render_logo_finish();
  wp_render_viz_finish();
  text_cache_free();
  wp_render_peek_finish();
  wp_render_hex_finish();
  wp_topo_finish();
  wp_logo_finish();
}

void wp_render(cairo_t *cr, const struct wallpaper_config *c, int pw, int ph,
               const struct wp_frame *f, bool full, bool ring, bool text) {
  cairo_save(cr);
  if (!full) {
    struct wp_rect r[WP_REGIONS];
    wp_regions_ex(c, pw, ph, ring || f->nbars > 0,
                  !f->text_kept &&
                      (text || (c->media_title && text_alpha(c, f) > 0)),
                  f->logo_gpu, f->logo_box, r);
    for (int i = 0; i < WP_REGIONS; ++i)
      if (r[i].w > 0 && r[i].h > 0)
        cairo_rectangle(cr, r[i].x, r[i].y, r[i].w, r[i].h);
    if (f->peek_area) {
      struct wp_rect pr[WP_PEEK_REGIONS];
      wp_peek_regions(c, pw, ph, pr);
      for (int i = 0; i < WP_PEEK_REGIONS; ++i)
        if (pr[i].w > 0 && pr[i].h > 0)
          cairo_rectangle(cr, pr[i].x, pr[i].y, pr[i].w, pr[i].h);
    }
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
    /* The lines shake with everything else and drift at rest; the edge
     * stretches in. */
    double k0 = pw / WP_REF_WIDTH;
    cairo_set_source_surface(cr, wp_topo_background(c, pw, ph),
                             round(f->shake_x * k0) + round(f->drift_x * k0),
                             round(f->shake_y * k0) + round(f->drift_y * k0));
    cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
    cairo_paint(cr);
  } else {
    set_rgba(cr, c->background, 1.0);
    cairo_paint(cr);
  }
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  /* Windows and the shows take the map away and bring it back. */
  double dim = 1 - wp_map_brightness(c, f);
  if (topo && !f->map_under && dim > 0) {
    cairo_set_source_rgba(cr, 0, 0, 0, fmin(dim, 1));
    cairo_paint(cr);
  }
  if (topo && !f->map_under && f->smap.on)
    show_map_mask(cr, pw, ph, &f->smap);
  double k = pw / WP_REF_WIDTH, r = c->logo_radius * k;
  /* A shake moves everything over the background, by whole pixels so the
   * cached tiles stay sharp. */
  cairo_translate(cr, round(f->shake_x * k), round(f->shake_y * k));
  /* The snowflake, and what's drawn round it, wander while idle. */
  double dx, dy, cx, cy, ls;
  wp_logo_offset(c, pw, ph, f, &dx, &dy);
  /* (and move aside for the popup's details, growing) */
  wp_logo_place(c, pw, ph, f, &cx, &cy, &ls);
  /* over the background, first */
  /* (Under topo the snowflake's outline glows instead.) */
  cairo_save(cr);
  cairo_translate(cr, dx, dy);
  if (!topo)
    wp_paint_bloom(cr, c, pw, ph, r, fmax(f->over, f->flash));
  if (f->nbars > 0 && !c->viz_edges)
    wp_draw_ring(cr, c, pw, ph, r, k, f);
  cairo_restore(cr);
  if (f->nbars > 0 && c->viz_edges && c->viz_edge_smooth)
    wp_draw_edges_smooth(cr, c, pw, ph, f);
  else if (f->nbars > 0 && c->viz_edges)
    wp_draw_edges(cr, c, pw, ph, f);

  draw_text(cr, c, pw, ph, f);
  wp_draw_peek(cr, c, pw, ph, f);
  draw_bar(cr, c, pw, f);
  if (f->over > 0)
    wp_draw_streaks(cr, c, cx, cy, r * ls, k, f);
  /* The logo reacts to the music: it grows on bass hits and its lambdas
   * slide out (both 0 without music). */
  double rp = r * ls * (1 + c->react_pulse_scale * f->pulse) *
              (1 - fmin(fmax(f->logo_shrink, -0.5), 1));
  wp_draw_hex(cr, c, cx, cy, r, rp, f);
  if (rp > 0 && topo && f->logo_gpu)
    ; /* the GPU draws it over the buffer */
  else if (rp > 0 && topo)
    wp_draw_glass_logo(cr, c, cx, cy, r, rp, k, f);
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
