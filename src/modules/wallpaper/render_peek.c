/* The popup's contents drawn in the wallpaper, in place of its panel on an
 * empty workspace: the clock and date left of the snowflake, battery,
 * volume and brightness as gauges right of it, network and stats in a row
 * over the workspace strip at the bottom, and the expand delay traced by a
 * line growing under the clock. With peek_layout "side" the hold moves the
 * snowflake aside instead (wp_logo_place), these fade as it goes, and the
 * expand stacks all of them in one column on the left. In the wallpaper's colours: topo's gradient
 * and glass, or classic's greys. Text is cached as masks (with a dark halo
 * under topo, like the title's) and coloured as it's painted, so the
 * tweens only move and fade them. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <pango/pangocairo.h>

#include "icons.h"
#include "render.h"
#include "util.h"

/* ---- text masks ---- */

#define TILES 48

struct tile {
  /* the key */
  char text[256];
  double size, maxw; /* px; maxw 0: no limit, else ellipsised to it */
  bool bold, halo;
  /* A8 masks of the text and its blurred halo (NULL without one) */
  cairo_surface_t *ink, *glow;
  double ox, base; /* the layout's origin x and baseline in them */
  double w;        /* its advance */
  double ink_w;    /* the origin to the ink's right edge (icons overhang) */
  unsigned used;
};
static struct tile tiles[TILES];
static unsigned tile_clock;
static char tile_font[128];

static void tile_free(struct tile *t) {
  if (t->ink)
    cairo_surface_destroy(t->ink);
  if (t->glow)
    cairo_surface_destroy(t->glow);
  memset(t, 0, sizeof *t);
}

static void tile_draw(const struct wallpaper_config *c, struct tile *t) {
  cairo_surface_t *one = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
  cairo_t *mc = cairo_create(one);
  PangoLayout *l = pango_cairo_create_layout(mc);
  PangoFontDescription *fd = pango_font_description_from_string(c->peek_font);
  pango_font_description_set_absolute_size(fd, t->size * PANGO_SCALE);
  pango_font_description_set_weight(fd, t->bold ? PANGO_WEIGHT_BOLD
                                                : PANGO_WEIGHT_NORMAL);
  pango_layout_set_font_description(l, fd);
  pango_font_description_free(fd);
  pango_layout_set_single_paragraph_mode(l, TRUE);
  if (t->maxw > 0) {
    pango_layout_set_width(l, (int)(t->maxw * PANGO_SCALE));
    pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
  }
  pango_layout_set_text(l, t->text, -1);
  PangoRectangle ink, log;
  pango_layout_get_pixel_extents(l, &ink, &log);
  /* Nerd Font icons overhang their advance: the mask holds both. */
  int x0 = ink.width > 0 ? MIN(ink.x, log.x) : log.x;
  int y0 = ink.height > 0 ? MIN(ink.y, log.y) : log.y;
  int x1 = MAX(ink.x + ink.width, log.x + log.width);
  int y1 = MAX(ink.y + ink.height, log.y + log.height);
  int pad = t->halo ? (int)ceil(0.3 * t->size) : 1;
  int w = x1 - x0 + 2 * pad, h = y1 - y0 + 2 * pad;
  t->ox = pad - x0;
  t->base = pad - y0 + (double)pango_layout_get_baseline(l) / PANGO_SCALE;
  t->w = log.width;
  t->ink_w = x1;
  t->ink = cairo_image_surface_create(CAIRO_FORMAT_A8, w, h);
  cairo_t *tc = cairo_create(t->ink);
  cairo_move_to(tc, t->ox, pad - y0);
  pango_cairo_update_layout(tc, l);
  pango_cairo_show_layout(tc, l);
  cairo_destroy(tc);
  g_object_unref(l);
  cairo_destroy(mc);
  cairo_surface_destroy(one);
  if (!t->halo)
    return;
  /* The halo: the text's shape blurred, thin strokes strengthened. */
  t->glow = cairo_image_surface_create(CAIRO_FORMAT_A8, w, h);
  tc = cairo_create(t->glow);
  cairo_set_source_surface(tc, t->ink, 0, 0);
  cairo_paint(tc);
  cairo_destroy(tc);
  cairo_surface_flush(t->glow);
  unsigned char *d = cairo_image_surface_get_data(t->glow);
  int st = cairo_image_surface_get_stride(t->glow);
  wp_blur_a8(d, w, h, st, (int)ceil(pad / 3.0));
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      int v = d[y * st + x] * 4;
      d[y * st + x] = (unsigned char)(v > 255 ? 255 : v);
    }
  cairo_surface_mark_dirty(t->glow);
}

/* The mask of s at size px, drawn once and kept while it's in use. */
static const struct tile *tile(const struct wallpaper_config *c, const char *s,
                               double size, bool bold, double maxw) {
  bool halo = wp_topo(c) && c->topo_text_backdrop > 0;
  if (strcmp(tile_font, c->peek_font)) {
    wp_render_peek_finish();
    copy_str(tile_font, sizeof tile_font, c->peek_font);
  }
  struct tile *lru = &tiles[0];
  for (int i = 0; i < TILES; ++i) {
    struct tile *t = &tiles[i];
    if (t->ink && t->size == size && t->maxw == maxw && t->bold == bold &&
        t->halo == halo && !strcmp(t->text, s)) {
      t->used = ++tile_clock;
      return t;
    }
    if (!t->ink || (lru->ink && t->used < lru->used))
      lru = t;
  }
  tile_free(lru);
  copy_str(lru->text, sizeof lru->text, s);
  lru->size = size;
  lru->maxw = maxw;
  lru->bold = bold;
  lru->halo = halo;
  tile_draw(c, lru);
  lru->used = ++tile_clock;
  return lru;
}

/* The clock's sign over the terrain (wp_peek_clock_mask). */
static struct {
  char text[16], font[128];
  double size;
  cairo_surface_t *s;
  double ink_w, ink_h, base;
  unsigned id;
} clk;

void wp_render_peek_finish(void) {
  for (int i = 0; i < TILES; ++i)
    tile_free(&tiles[i]);
  if (clk.s)
    cairo_surface_destroy(clk.s);
  unsigned id = clk.id;
  memset(&clk, 0, sizeof clk);
  clk.id = id;
}

bool wp_peek_clock_mask(const struct wallpaper_config *c,
                        const struct info *in, double size,
                        struct wp_clock_mask *m) {
  char hhmm[16];
  struct tm tm;
  localtime_r(&in->now, &tm);
  strftime(hhmm, sizeof hhmm, "%H:%M", &tm);
  if (!clk.s || clk.size != size || strcmp(clk.text, hhmm) ||
      strcmp(clk.font, c->peek_font)) {
    if (clk.s)
      cairo_surface_destroy(clk.s);
    copy_str(clk.text, sizeof clk.text, hhmm);
    copy_str(clk.font, sizeof clk.font, c->peek_font);
    clk.size = size;
    cairo_surface_t *one = cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1);
    cairo_t *mc = cairo_create(one);
    PangoLayout *l = pango_cairo_create_layout(mc);
    PangoFontDescription *fd = pango_font_description_from_string(c->peek_font);
    pango_font_description_set_absolute_size(fd, size * PANGO_SCALE);
    pango_font_description_set_weight(fd, PANGO_WEIGHT_BOLD);
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    pango_layout_set_text(l, hhmm, -1);
    PangoRectangle ink;
    pango_layout_get_pixel_extents(l, &ink, NULL);
    /* Blurred a little: its edge is where it's half, smooth at any
     * distance; clear round it. */
    int b = (int)ceil(0.02 * size), pad = 3 * b + 2;
    int w = MAX(ink.width, 1) + 2 * pad, h = MAX(ink.height, 1) + 2 * pad;
    clk.s = cairo_image_surface_create(CAIRO_FORMAT_A8, w, h);
    cairo_t *tc = cairo_create(clk.s);
    cairo_move_to(tc, pad - ink.x, pad - ink.y);
    pango_cairo_update_layout(tc, l);
    pango_cairo_show_layout(tc, l);
    cairo_destroy(tc);
    g_object_unref(l);
    cairo_destroy(mc);
    cairo_surface_destroy(one);
    cairo_surface_flush(clk.s);
    unsigned char *d = cairo_image_surface_get_data(clk.s);
    int st = cairo_image_surface_get_stride(clk.s);
    wp_blur_a8(d, w, h, st, b);
    cairo_surface_mark_dirty(clk.s);
    clk.ink_w = ink.width;
    clk.ink_h = ink.height;
    clk.base = pad + MAX(ink.height, 1);
    ++clk.id;
  }
  m->data = cairo_image_surface_get_data(clk.s);
  m->w = cairo_image_surface_get_width(clk.s);
  m->h = cairo_image_surface_get_height(clk.s);
  m->stride = cairo_image_surface_get_stride(clk.s);
  m->ink_w = clk.ink_w;
  m->ink_h = clk.ink_h;
  m->base = clk.base;
  m->id = clk.id;
  return true;
}

/* Paint t with its origin's baseline at x, y (whole px, so it stays sharp)
 * from the current source, its halo under it at alpha a. */
static void paint(cairo_t *cr, const struct wallpaper_config *c,
                  const struct tile *t, double x, double y, double a) {
  double tx = round(x - t->ox), ty = round(y - t->base);
  if (t->glow) {
    cairo_pattern_t *src = cairo_get_source(cr);
    cairo_pattern_reference(src);
    cairo_set_source_rgba(cr, 0, 0, 0, c->topo_text_backdrop * a);
    cairo_mask_surface(cr, t->glow, tx, ty);
    cairo_set_source(cr, src);
    cairo_pattern_destroy(src);
  }
  cairo_mask_surface(cr, t->ink, tx, ty);
}

static void paint_col(cairo_t *cr, const struct wallpaper_config *c,
                      const struct tile *t, double x, double y, uint32_t col,
                      double a) {
  set_rgba(cr, col, a);
  paint(cr, c, t, x, y, a);
}

/* ---- colours ---- */

struct look {
  bool topo;
  uint32_t text, dim, line, track, edge[3];
};

static void look(const struct wallpaper_config *c, struct look *l) {
  l->topo = wp_topo(c);
  l->text = l->topo ? c->topo_text : c->media_text;
  l->dim = l->topo ? c->topo_text_dim : c->media_dim;
  l->line = c->media_line;
  l->track = l->topo ? c->topo_logo_fill : c->media_track;
  l->edge[0] = c->topo_edge_a;
  l->edge[1] = c->topo_edge_mid;
  l->edge[2] = c->topo_edge_b;
}

/* The edge colours left to right over x0..x1, opaque times a. */
static cairo_pattern_t *across(const struct look *l, double x0, double x1,
                               double a) {
  cairo_pattern_t *p = cairo_pattern_create_linear(x0, 0, x1, 0);
  for (int t = 0; t < 3; ++t)
    cairo_pattern_add_color_stop_rgba(p, t / 2.0, col_r(l->edge[t]),
                                      col_g(l->edge[t]), col_b(l->edge[t]), a);
  return p;
}

/* ---- layout ---- */

struct layout {
  double k, cx, cy, reach, slide;
  double sc, ss, sd, sr, sw; /* clock, seconds, date, row, workspace px */
  double lx0, lx1, rx0, rx1; /* the side columns */
  /* left: the clock's baseline clock-only and expanded, then the date's
   * (expanded); extent with the halo */
  double clock_c, clock_e, date;
  struct wp_rect left;
  /* right: each level's label baseline and gauge top, the gauge's height
   * clock-only and expanded */
  double label[3], bar[3], bar_c, bar_e;
  struct wp_rect right;
  /* the bottom: the stats row's baseline and widest, the workspace
   * strip's middle, cell height, widest */
  double stats, bmax, sy, sh, smax;
  struct wp_rect strip;
  bool strip_left; /* the workspaces start at lx0 (side), else round cx */
};

/* "side": one column left of the snowflake's place. g holds it as the
 * flanks' parts read it: the gauges between rx0 and rx1 at label/bar, the
 * workspaces from lx0 at sy, at most smax wide. From the top: the clock's
 * baseline, the date's, the gauges, network's and the stats' two rows'. */
struct side {
  struct layout g;
  double x0, x1, clock, date, net, row[2];
  struct wp_rect rect;
};

static void lay_out(const struct wallpaper_config *c, int pw, int ph,
                    struct layout *L) {
  double k = L->k = pw / WP_REF_WIDTH;
  L->strip_left = false;
  L->cx = pw / 2.0;
  L->cy = ph / 2.0;
  L->reach = wp_logo_reach(c, pw);
  L->slide = c->peek_slide * k;
  L->sc = c->peek_clock_size * k;
  L->ss = c->peek_seconds_size * k;
  L->sd = c->peek_date_size * k;
  L->sr = c->peek_row_size * k;
  L->sw = 0.9 * L->sr;
  /* A shake moves everything by up to this much. */
  double sh = c->react_shake ? ceil(c->react_shake_px * k) : 0;

  /* The columns fill the space between the screen's sides (and classic's
   * edge visualiser) and the snowflake, each centred in its half. */
  double margin = 80 * k;
  if (!wp_topo(c) && c->viz && c->viz_edges)
    margin = fmax(margin, (c->viz_edge_gap + c->viz_edge_length + 20) * k);
  double inner = L->cx - L->reach - 120 * k;
  double avail = fmax(inner - margin, 1);
  double colw = fmin(avail, c->peek_max_width * k);
  L->lx0 = margin + (avail - colw) / 2;
  L->lx1 = L->lx0 + colw;
  L->rx0 = pw - L->lx1;
  L->rx1 = pw - L->lx0;

  /* Left, expanded: clock and date, centred on the snowflake; clock-only,
   * the clock's cap is. */
  double cap = 0.72 * L->sc;
  double date = cap + 0.2 * L->sc + 0.95 * L->sd;
  double bottom = date + 0.25 * L->sd;
  double top = L->cy - bottom / 2;
  L->clock_e = top + cap;
  L->clock_c = L->cy + cap / 2;
  L->date = top + date;
  double pad = ceil(0.3 * L->sc) + sh;
  L->left = wp_clamp_rect(
      L->lx0 - L->slide - pad, fmin(L->clock_e, L->clock_c) - 1.0 * L->sc - pad,
      L->lx1 + pad,
      fmax(L->clock_c + 0.35 * L->sc, L->date + 0.4 * L->sd) + pad, pw, ph);

  /* Right: three labelled gauges, centred on the snowflake. */
  L->bar_c = 0.1 * L->sr;
  L->bar_e = 0.4 * L->sr;
  double gstep = 2.6 * L->sr;
  double rh = 0.8 * L->sr + 2 * gstep + 0.5 * L->sr + L->bar_e;
  double rtop = L->cy - rh / 2;
  for (int i = 0; i < 3; ++i) {
    L->label[i] = rtop + 0.8 * L->sr + i * gstep;
    L->bar[i] = L->label[i] + 0.5 * L->sr;
  }
  pad = ceil(0.3 * L->sr) + 2 * k + sh;
  L->right = wp_clamp_rect(L->rx0 - pad, rtop - 0.5 * L->sr - pad,
                           L->rx1 + L->slide + pad, rtop + rh + pad, pw, ph);

  /* The bottom, under the title: the stats row over the workspaces. */
  L->sh = 1.7 * L->sw;
  L->sy = ph - 150 * k;
  L->smax = fmin(c->media_max_width * k, pw - 2 * margin);
  L->stats = L->sy - L->sh / 2 - 1.1 * L->sr;
  L->bmax = pw - 2 * margin;
  pad = ceil(0.3 * L->sr) + 2 * k + sh;
  L->strip = wp_clamp_rect(
      L->cx - L->bmax / 2 - pad, L->stats - 1.0 * L->sr - pad,
      L->cx + L->bmax / 2 + pad, L->sy + L->sh / 2 + L->slide + pad, pw, ph);
}

/* The side column may show: "side", or "terrain" falling back to it. */
static bool sided(const struct wallpaper_config *c) {
  return wp_peek_layout(c) != WP_LAYOUT_FLANKS;
}

static void lay_out_side(const struct wallpaper_config *c, int pw, int ph,
                         struct side *S) {
  struct layout *g = &S->g;
  lay_out(c, pw, ph, g);
  double k = g->k, sr = g->sr;
  /* From the margin to short of the snowflake's place. */
  double logo = c->peek_side_x * pw - c->logo_radius * k * c->peek_side_scale;
  S->x0 = c->peek_side_margin * k;
  S->x1 = fmin(S->x0 + c->peek_side_width * k, logo - 120 * k);
  if (S->x1 < S->x0 + 1)
    S->x1 = S->x0 + 1;
  double gap = 1.6 * sr; /* between the groups */
  /* Stacked from 0, then centred on the snowflake's place. */
  double y = 0.72 * g->sc;
  S->clock = y;
  S->date = y += 0.2 * g->sc + 0.95 * g->sd;
  y += 0.25 * g->sd + gap;
  double gstep = 2.6 * sr;
  for (int i = 0; i < 3; ++i) {
    g->label[i] = y + 0.8 * sr + i * gstep;
    g->bar[i] = g->label[i] + 0.5 * sr;
  }
  y = g->bar[2] + g->bar_e + gap;
  S->net = y += 0.8 * sr;
  S->row[0] = y += 1.6 * sr;
  S->row[1] = y += 1.6 * sr;
  y += 0.3 * sr + gap;
  g->sy = y + g->sh / 2;
  double total = y + g->sh;
  double dy = round(c->peek_side_y * ph - total / 2);
  S->clock += dy;
  S->date += dy;
  for (int i = 0; i < 3; ++i) {
    g->label[i] += dy;
    g->bar[i] += dy;
  }
  S->net += dy;
  S->row[0] += dy;
  S->row[1] += dy;
  g->sy += dy;
  g->rx0 = g->lx0 = S->x0;
  g->rx1 = g->lx1 = S->x1;
  g->smax = S->x1 - S->x0;
  g->strip_left = true;
  double sh = c->react_shake ? ceil(c->react_shake_px * k) : 0;
  double pad = ceil(0.3 * g->sc) + 2 * k + sh;
  S->rect = wp_clamp_rect(S->x0 - g->slide - pad, dy - pad, S->x1 + pad,
                          dy + total + pad, pw, ph);
}

static struct wp_rect terrain_rect(const struct wallpaper_config *c, int pw,
                                   int ph, const struct layout *L);

void wp_peek_regions(const struct wallpaper_config *c, int pw, int ph,
                     struct wp_rect out[WP_PEEK_REGIONS]) {
  struct layout L;
  lay_out(c, pw, ph, &L);
  out[0] = L.left;
  out[1] = L.right;
  out[2] = L.strip;
  out[3] = out[4] = (struct wp_rect){0, 0, 0, 0};
  if (sided(c)) {
    struct side S;
    lay_out_side(c, pw, ph, &S);
    out[3] = S.rect;
  }
  if (wp_peek_layout(c) == WP_LAYOUT_TERRAIN)
    out[4] = terrain_rect(c, pw, ph, &L);
}

static void clip_to(cairo_t *cr, const struct wp_rect *r) {
  cairo_rectangle(cr, r->x, r->y, r->w, r->h);
  cairo_clip(cr);
}

static double clamp01(double v) { return fmin(fmax(v, 0), 1); }

/* ---- the parts ---- */

/* A run of text pieces centred on cx at baseline y; icons are spaced by
 * their ink and a gap, as their glyphs overhang. */
struct piece {
  const char *s;
  bool dim;
  bool icon;
};

static double piece_w(const struct tile *t, bool icon, const struct layout *L) {
  return icon ? fmax(t->w, t->ink_w) + 0.35 * L->sr : t->w;
}

static void pieces(cairo_t *cr, const struct wallpaper_config *c,
                   const struct look *l, const struct layout *L,
                   const struct piece *p, int n, double cx, double y,
                   double a) {
  const struct tile *t[16];
  double w = 0;
  for (int i = 0; i < n; ++i) {
    t[i] = tile(c, p[i].s, L->sr, false, 0);
    w += piece_w(t[i], p[i].icon, L);
  }
  double x = cx - w / 2;
  for (int i = 0; i < n; ++i) {
    paint_col(cr, c, t[i], x, y, p[i].dim ? l->dim : l->text, a);
    x += piece_w(t[i], p[i].icon, L);
  }
}

/* A run of pieces starting at x. */
static void pieces_from(cairo_t *cr, const struct wallpaper_config *c,
                        const struct look *l, const struct layout *L,
                        const struct piece *p, int n, double x, double y,
                        double a) {
  double w = 0;
  for (int i = 0; i < n; ++i)
    w += piece_w(tile(c, p[i].s, L->sr, false, 0), p[i].icon, L);
  pieces(cr, c, l, L, p, n, x + w / 2, y, a);
}

/* The clock's text: HH:MM, :SS, and the date in words. */
static void clock_text(const struct info *in, char hhmm[16], char ss[8],
                       char date[96]) {
  struct tm tm;
  char wday[32], rest[32];
  localtime_r(&in->now, &tm);
  strftime(hhmm, 16, "%H:%M", &tm);
  strftime(ss, 8, ":%S", &tm);
  strftime(wday, sizeof wday, "%A", &tm);
  strftime(rest, sizeof rest, "%B %Y", &tm);
  snprintf(date, 96, "%s, %d %s", wday, tm.tm_mday, rest);
}

/* HH:MM with its baseline's left at x, y: in the edge gradient (topo) or
 * the text colour. */
static void paint_hhmm(cairo_t *cr, const struct wallpaper_config *c,
                       const struct look *l, const struct layout *L,
                       const struct tile *hm, double x, double base,
                       double a) {
  if (l->topo) {
    double cap = 0.72 * L->sc;
    cairo_pattern_t *p = wp_topo_edge_pattern(
        l->edge, x + hm->w / 2, base - cap / 2, hm->w / 2, a, true);
    cairo_set_source(cr, p);
    paint(cr, c, hm, x, base, a);
    cairo_pattern_destroy(p);
  } else {
    paint_col(cr, c, hm, x, base, l->text, a);
  }
}

/* hold: how far the expand delay has run, traced by a line growing under
 * HH:MM from its left edge; like the popup's hold line, it clears out in
 * the first half of the expand. */
static void draw_left(cairo_t *cr, const struct wallpaper_config *c,
                      const struct look *l, const struct layout *L,
                      const struct info *in, double a, double e, double hold,
                      double off) {
  char hhmm[16], ss[8], date[96];
  clock_text(in, hhmm, ss, date);

  double colw = L->lx1 - L->lx0, mid = (L->lx0 + L->lx1) / 2 - off;
  double base = L->clock_c + (L->clock_e - L->clock_c) * e;
  const struct tile *hm = tile(c, hhmm, L->sc, true, 0);
  const struct tile *sec = tile(c, ss, L->ss, false, 0);
  double gap = 0.1 * L->ss;
  /* Clock-only HH:MM is centred alone; expanding, it makes room for :SS. */
  double x = mid - hm->w / 2 - (gap + sec->w) / 2 * e;
  paint_hhmm(cr, c, l, L, hm, x, base, a);
  double lh = c->peek_hold_width * L->k, la = a * (1 - 2 * e);
  if (hold > 0 && la > 0 && lh > 0) {
    pill(cr, x, base + 0.12 * L->sc, hm->w * clamp01(hold), lh);
    cairo_pattern_t *p = NULL;
    if (l->topo)
      cairo_set_source(cr, p = across(l, x, x + hm->w, la));
    else
      set_rgba(cr, l->line, la);
    cairo_fill(cr);
    if (p)
      cairo_pattern_destroy(p);
  }
  if (e <= 0)
    return;
  double ae = a * e;
  paint_col(cr, c, sec, x + hm->w + gap, base, l->dim, ae);
  double dy = base - L->clock_e; /* the rest follows the clock */
  const struct tile *dt = tile(c, date, L->sd, false, colw);
  paint_col(cr, c, dt, mid - dt->w / 2, L->date + dy, l->dim, ae);
}

/* Network's icon, name and signal into p (sig and name hold the text);
 * how many. */
static int net_pieces(const struct info *in, struct piece *p, char sig[16],
                      char name[64]) {
  int n = 2;
  copy_str(name, 64, in->net_name);
  switch (in->net) {
  case NET_WIFI:
    p[0] = (struct piece){icon_wifi(in->net_signal), false, true};
    p[1] = (struct piece){name[0] ? name : "wifi", false, false};
    if (in->net_signal >= 0) {
      snprintf(sig, 16, "  %d%%", in->net_signal);
      p[n++] = (struct piece){sig, true, false};
    }
    break;
  case NET_WIRED:
    p[0] = (struct piece){ICON_WIRED, false, true};
    p[1] = (struct piece){name[0] ? name : "wired", false, false};
    break;
  case NET_OFFLINE:
    p[0] = (struct piece){ICON_OFFLINE, true, true};
    p[1] = (struct piece){"offline", true, false};
    break;
  default:
    p[0] = (struct piece){ICON_NET_UNK, true, true};
    p[1] = (struct piece){name[0] ? name : "unknown", true, false};
    break;
  }
  return n;
}

/* CPU and the battery's power, memory and uptime, as text. */
static void stat_text(const struct info *in, char cpu[16], char pwr[24],
                      char mem[32], char up[32]) {
  long m = in->uptime_s / 60, h = m / 60, d = h / 24;
  if (in->cpu < 0)
    snprintf(cpu, 16, "--");
  else
    snprintf(cpu, 16, "%d%%", in->cpu);
  snprintf(pwr, 24, "%s%.2fW", in->power_charging ? "▲" : "", in->power_w);
  snprintf(mem, 32, "%.1f/%.1fG", in->mem_used_gib, in->mem_total_gib);
  if (d > 0)
    snprintf(up, 32, "%ldd %ldh", d, h % 24);
  else if (h > 0)
    snprintf(up, 32, "%ldh %02ldm", h, m % 60);
  else
    snprintf(up, 32, "%ldm", m);
}

/* Network and the stats in one row over the workspace strip. */
static void draw_stats(cairo_t *cr, const struct wallpaper_config *c,
                       const struct look *l, const struct layout *L,
                       const struct info *in, double a, double off) {
  char sig[16], name[64];
  struct piece p[16];
  int n = net_pieces(in, p, sig, name);
  char cpu[16], pwr[24], mem[32], up[32];
  stat_text(in, cpu, pwr, mem, up);
  p[n++] = (struct piece){"     CPU ", true, false};
  p[n++] = (struct piece){cpu, false, false};
  if (in->has_power) {
    p[n++] = (struct piece){"     PWR ", true, false};
    p[n++] = (struct piece){pwr, false, false};
  }
  p[n++] = (struct piece){"     RAM ", true, false};
  p[n++] = (struct piece){mem, false, false};
  p[n++] = (struct piece){"     up ", true, false};
  p[n++] = (struct piece){up, false, false};
  pieces(cr, c, l, L, p, n, L->cx, L->stats + off, a);
}

/* A gauge filled to frac: topo's is glass in the gradient outline (which
 * comes with the expand), filled in the gradient; classic's a grey track
 * and fill. fill_col (if not 0) replaces the fill's colours. */
static void gauge(cairo_t *cr, const struct look *l, double x, double y,
                  double w, double h, double frac, uint32_t fill_col,
                  double edge, double k, double a) {
  pill(cr, x, y, w, h);
  set_rgba(cr, l->track, a);
  cairo_fill(cr);
  double fw = w * clamp01(frac);
  if (fw > 0) {
    pill(cr, x, y, fw, h);
    cairo_pattern_t *p = NULL;
    if (fill_col)
      set_rgba(cr, fill_col, a);
    else if (l->topo)
      cairo_set_source(cr, p = across(l, x, x + w, a));
    else
      set_rgba(cr, l->line, a);
    cairo_fill(cr);
    if (p)
      cairo_pattern_destroy(p);
  }
  double b = 2 * k;
  if (!l->topo || edge <= 0 || h <= 2 * b)
    return;
  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
  pill(cr, x - b, y - b, w + 2 * b, h + 2 * b);
  pill(cr, x, y, w, h);
  cairo_pattern_t *p = cairo_pattern_create_linear(x, 0, x + w, 0);
  for (int t = 0; t < 3; ++t)
    cairo_pattern_add_color_stop_rgba(p, t / 2.0, col_r(l->edge[t]),
                                      col_g(l->edge[t]), col_b(l->edge[t]),
                                      col_a(l->edge[t]) * a * edge);
  cairo_set_source(cr, p);
  cairo_fill(cr);
  cairo_pattern_destroy(p);
  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
}

static void draw_right(cairo_t *cr, const struct wallpaper_config *c,
                       const struct look *l, const struct layout *L,
                       const struct info *in, int low_pct, double a, double e,
                       double off) {
  bool low = in->has_battery && !in->charging && in->battery <= low_pct;
  const struct {
    const char *icon;
    bool has;
    int pct;
    bool warn, dim;
  } lv[3] = {
      {icon_battery(in), in->has_battery, in->battery, low, false},
      {icon_volume(in), in->has_volume, in->volume, false, in->muted},
      {ICON_BRIGHT, in->has_brightness, in->brightness, false, false},
  };
  double x = L->rx0 + off, w = L->rx1 - L->rx0;
  double h = L->bar_c + (L->bar_e - L->bar_c) * e;
  for (int i = 0; i < 3; ++i) {
    char val[16];
    if (lv[i].has)
      snprintf(val, sizeof val, "%d%%", lv[i].pct);
    else
      snprintf(val, sizeof val, "--");
    /* Clock-only the levels are quiet; they brighten as they grow. */
    uint32_t col = lv[i].warn                ? c->peek_warn
                   : lv[i].dim || !lv[i].has ? l->dim
                                             : mix(l->dim, l->text, e);
    const struct tile *it = tile(c, lv[i].icon, L->sr, false, 0);
    const struct tile *vt = tile(c, val, L->sr, false, 0);
    paint_col(cr, c, it, x, L->label[i], col, a);
    paint_col(cr, c, vt, x + w - vt->w, L->label[i], col, a);
    gauge(cr, l, x, L->bar[i], w, h, lv[i].has ? lv[i].pct / 100.0 : 0,
          lv[i].warn  ? c->peek_warn
          : lv[i].dim ? l->dim
                      : 0,
          e, L->k, a);
  }
}

static void draw_strip(cairo_t *cr, const struct wallpaper_config *c,
                       const struct look *l, const struct layout *L,
                       const struct info *in, double a, double off) {
  int n = in->ws_count < WS_MAX ? in->ws_count : WS_MAX;
  if (n <= 0)
    return;
  double cw = 1.8 * L->sw, gap = 0.35 * L->sw;
  int f = 0;
  for (int i = 0; i < n; ++i) {
    const struct tile *t =
        tile(c, in->ws[i].name, L->sw, in->ws[i].focused, L->smax / 3);
    cw = fmax(cw, t->w + 1.2 * L->sw);
    if (in->ws[i].focused)
      f = i;
  }
  cw = fmin(cw, L->smax);
  /* As many as fit, centred on the focused one where possible. */
  int fit = (int)((L->smax + gap) / (cw + gap));
  int m = n < fit ? n : fit > 0 ? fit : 1;
  int first = f - (m - 1) / 2;
  if (first + m > n)
    first = n - m;
  if (first < 0)
    first = 0;
  double x = L->strip_left ? L->lx0 + off
                           : L->cx - (m * cw + (m - 1) * gap) / 2;
  double y = L->sy - L->sh / 2 + (L->strip_left ? 0 : off);
  for (int i = first; i < first + m; ++i, x += cw + gap) {
    const struct workspace *ws = &in->ws[i];
    if (ws->focused) /* an empty gauge: glass, or a grey pill */
      gauge(cr, l, x, y, cw, L->sh, 0, 0, 1, L->k, a);
    const struct tile *t = tile(c, ws->name, L->sw, ws->focused, L->smax / 3);
    uint32_t col = ws->urgent ? c->peek_warn : ws->focused ? l->text : l->dim;
    paint_col(cr, c, t, x + (cw - t->w) / 2, L->sy + 0.36 * L->sw + off, col,
              a);
  }
}

/* "side": the column, moved left by off. */
static void draw_side(cairo_t *cr, const struct wallpaper_config *c,
                      const struct look *l, const struct side *S,
                      const struct info *in, int low_pct, double a,
                      double off) {
  const struct layout *L = &S->g;
  double x = S->x0 - off, colw = S->x1 - S->x0;
  /* HH:MM:SS over the date */
  char hhmm[16], ss[8], date[96];
  clock_text(in, hhmm, ss, date);
  const struct tile *hm = tile(c, hhmm, L->sc, true, 0);
  const struct tile *sec = tile(c, ss, L->ss, false, 0);
  paint_hhmm(cr, c, l, L, hm, x, S->clock, a);
  paint_col(cr, c, sec, x + hm->w + 0.1 * L->ss, S->clock, l->dim, a);
  const struct tile *dt = tile(c, date, L->sd, false, colw);
  paint_col(cr, c, dt, x, S->date, l->dim, a);
  /* the levels, as the flanks' expanded */
  draw_right(cr, c, l, L, in, low_pct, a, 1, -off);
  /* network, then the stats two to a row */
  char sig[16], name[64];
  struct piece p[16];
  int n = net_pieces(in, p, sig, name);
  pieces_from(cr, c, l, L, p, n, x, S->net, a);
  char cpu[16], pwr[24], mem[32], up[32];
  stat_text(in, cpu, pwr, mem, up);
  struct piece st[4][2] = {{{"CPU  ", true, false}, {cpu, false, false}},
                           {{"PWR  ", true, false}, {pwr, false, false}},
                           {{"RAM  ", true, false}, {mem, false, false}},
                           {{"up  ", true, false}, {up, false, false}}};
  for (int i = 0, j = 0; i < 4; ++i) {
    if (i == 1 && !in->has_power)
      continue;
    pieces_from(cr, c, l, L, st[i], 2, x + (j % 2) * colw / 2, S->row[j / 2],
                a);
    ++j;
  }
  /* the workspaces */
  draw_strip(cr, c, l, L, in, a, -off);
}

/* ---- "terrain": labels pinned to the landscape ---- */

/* Where the labels hang at T: over each mesa's top, the seconds right of
 * HH:MM on its plateau, the date on the ground in front of it. False for
 * one behind the eye. */
struct pins {
  double mx[3], my[3];
  bool mesa[3];
  double sx, sy, dx, dy;
  bool secs, date;
};

static void pin(const struct wallpaper_config *c, int pw, int ph,
                const struct wp_terrain *T, struct pins *P) {
  for (int i = 0; i < 3; ++i) {
    double z = wp_terrain_z(c, pw, ph, T, T->mesa_x[i], T->mesa_y[i]);
    P->mesa[i] = wp_cam_project(&T->cam, T->mesa_x[i], T->mesa_y[i], z,
                                &P->mx[i], &P->my[i], NULL);
  }
  /* The digits' box: from the mask if it's made, else from the size. */
  double hw = 1.3 * T->clk_size, ih = 0.72 * T->clk_size;
  if (clk.s && clk.size == T->clk_size) {
    hw = clk.ink_w / 2;
    ih = clk.ink_h;
  }
  ih *= T->clk_stretch;
  /* the seconds off the sign's right edge, high up; the date on the
   * ground in front of its foot */
  double p[3];
  wp_terrain_clock_point(T, hw + 0.1 * T->clk_size, 0.72 * ih, p);
  P->secs = wp_cam_project(&T->cam, p[0], p[1], p[2], &P->sx, &P->sy, NULL);
  /* (along the ground towards the eye: r turned a quarter) */
  double x = T->clk_x - T->clk_r[1] * 0.5 * T->clk_size;
  double y = T->clk_y + T->clk_r[0] * 0.5 * T->clk_size;
  double z = wp_terrain_z(c, pw, ph, T, x, y);
  P->date = wp_cam_project(&T->cam, x, y, z, &P->dx, &P->dy, NULL);
}

/* Everywhere they can be, as the camera comes and goes and the mesas
 * rise and fall. */
static struct wp_rect terrain_rect(const struct wallpaper_config *c, int pw,
                                   int ph, const struct layout *L) {
  double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
  struct wp_frame f = {0};
  for (int v = 0; v < 2; ++v) {
    f.terrain_vals[0] = f.terrain_vals[1] = f.terrain_vals[2] = v;
    for (int i = 0; i <= 8; ++i) {
      struct wp_terrain T;
      wp_terrain_at_t(c, pw, ph, &f, i / 8.0, &T);
      struct pins P;
      pin(c, pw, ph, &T, &P);
      double xs[5] = {P.mx[0], P.mx[1], P.mx[2], P.sx, P.dx};
      double ys[5] = {P.my[0], P.my[1], P.my[2], P.sy, P.dy};
      bool ok[5] = {P.mesa[0], P.mesa[1], P.mesa[2], P.secs, P.date};
      for (int j = 0; j < 5; ++j)
        if (ok[j]) {
          x0 = fmin(x0, xs[j]), x1 = fmax(x1, xs[j]);
          y0 = fmin(y0, ys[j]), y1 = fmax(y1, ys[j]);
        }
    }
  }
  if (x0 > x1)
    return (struct wp_rect){0, 0, 0, 0};
  /* the widest label (the date) either way, the tallest over its pin */
  double sh = c->react_shake ? ceil(c->react_shake_px * L->k) : 0;
  double padx = 14 * L->sd + sh, pady = 3.2 * L->sr + L->ss + sh;
  return wp_clamp_rect(x0 - padx, y0 - pady, x1 + padx, y1 + pady, pw, ph);
}

/* The labels, and the stats and workspaces along the bottom. */
static void draw_terrain(cairo_t *cr, const struct wallpaper_config *c,
                         const struct look *l, const struct layout *L, int pw,
                         int ph, const struct wp_frame *f, double a) {
  const struct info *in = f->peek;
  struct wp_terrain T;
  wp_terrain_at(c, pw, ph, f, &T);
  struct pins P;
  pin(c, pw, ph, &T, &P);
  cairo_save(cr);
  clip_to(cr, &(struct wp_rect){0, 0, pw, ph});
  /* over each mesa: what it measures, its value */
  bool low = in->has_battery && !in->charging && in->battery <= f->peek_low_pct;
  char val[3][16];
  if (in->cpu < 0)
    snprintf(val[0], 16, "--");
  else
    snprintf(val[0], 16, "%d%%", in->cpu);
  if (in->mem_total_gib > 0)
    snprintf(val[1], 16, "%d%%",
             (int)lround(100 * in->mem_used_gib / in->mem_total_gib));
  else
    snprintf(val[1], 16, "--");
  if (in->has_battery)
    snprintf(val[2], 16, "%d%%", in->battery);
  else
    snprintf(val[2], 16, "--");
  const char *what[3] = {"CPU", "RAM", icon_battery(in)};
  for (int i = 0; i < 3; ++i) {
    if (!P.mesa[i])
      continue;
    const struct tile *vt = tile(c, val[i], L->sr, true, 0);
    const struct tile *wt = tile(c, what[i], 0.75 * L->sr, false, 0);
    double base = P.my[i] - 0.9 * L->sr;
    paint_col(cr, c, vt, P.mx[i] - vt->w / 2, base,
              i == 2 && low ? c->peek_warn : l->text, a);
    paint_col(cr, c, wt, P.mx[i] - fmax(wt->w, wt->ink_w) / 2,
              base - 1.15 * L->sr, l->dim, a);
  }
  /* the seconds on the clock's plateau, the date in front of it */
  char hhmm[16], ss[8], date[96];
  clock_text(in, hhmm, ss, date);
  if (P.secs) {
    const struct tile *st = tile(c, ss, L->ss, false, 0);
    paint_col(cr, c, st, P.sx, P.sy, l->dim, a);
  }
  if (P.date) {
    const struct tile *dt = tile(c, date, L->sd, false, 0);
    paint_col(cr, c, dt, P.dx - dt->w / 2, P.dy, l->dim, a);
  }
  cairo_restore(cr);
  /* along the bottom: network, volume, brightness, power, uptime */
  char sig[16], name[64];
  struct piece p[16];
  int n = net_pieces(in, p, sig, name);
  char cpu[16], pwr[24], mem[32], up[32], vol[16], bri[16];
  stat_text(in, cpu, pwr, mem, up);
  snprintf(vol, sizeof vol, in->has_volume ? "%d%%" : "--", in->volume);
  snprintf(bri, sizeof bri, in->has_brightness ? "%d%%" : "--",
           in->brightness);
  p[n++] = (struct piece){"     ", true, false};
  p[n++] = (struct piece){icon_volume(in), in->muted, true};
  p[n++] = (struct piece){vol, in->muted, false};
  p[n++] = (struct piece){"   ", true, false};
  p[n++] = (struct piece){ICON_BRIGHT, false, true};
  p[n++] = (struct piece){bri, false, false};
  if (in->has_power) {
    p[n++] = (struct piece){"     PWR ", true, false};
    p[n++] = (struct piece){pwr, false, false};
  }
  p[n++] = (struct piece){"     up ", true, false};
  p[n++] = (struct piece){up, false, false};
  cairo_save(cr);
  clip_to(cr, &L->strip);
  pieces(cr, c, l, L, p, n, L->cx, L->stats, a);
  draw_strip(cr, c, l, L, in, a, 0);
  cairo_restore(cr);
}

void wp_draw_peek(cairo_t *cr, const struct wallpaper_config *c, int pw, int ph,
                  const struct wp_frame *f) {
  if (!f->peek || f->peek_pop <= 0)
    return;
  struct layout L;
  lay_out(c, pw, ph, &L);
  struct look l;
  look(c, &l);
  double a = clamp01(f->peek_pop), e = clamp01(f->peek_expand);
  /* They slide in from the sides as they show. */
  double off = (1 - a) * L.slide;
  /* "side": they fade as the snowflake moves aside, and stay out once the
   * column has shown (so not back while it closes); "terrain", as the map
   * tips. */
  enum wp_layout lay = wp_peek_layout(c);
  bool terrain = lay == WP_LAYOUT_TERRAIN && f->terrain;
  bool side = lay != WP_LAYOUT_FLANKS && !terrain;
  /* (over the terrain they're gone before its clock and mesas rise) */
  double moved = terrain ? clamp01(wp_peek_terrain(c, f) / 0.15)
                         : wp_peek_side(c, f);
  double fa = side || terrain ? a * (1 - moved) * (1 - e) : a;
  cairo_save(cr);
  if (fa > 0) {
    cairo_save(cr);
    clip_to(cr, &L.left);
    /* Over the GPU's map the topo lines show the hold (they gather in);
     * the snowflake does it, moving aside. */
    double hold =
        side || terrain || (l.topo && f->map_under) ? 0 : f->peek_hold;
    draw_left(cr, c, &l, &L, f->peek, fa, e, hold, off);
    cairo_restore(cr);
    cairo_save(cr);
    clip_to(cr, &L.right);
    draw_right(cr, c, &l, &L, f->peek, f->peek_low_pct, fa, e, off);
    cairo_restore(cr);
    if (e > 0) {
      /* The stats and the workspaces rise from below with the expand. */
      cairo_save(cr);
      clip_to(cr, &L.strip);
      draw_stats(cr, c, &l, &L, f->peek, fa * e, (1 - e) * L.slide);
      draw_strip(cr, c, &l, &L, f->peek, fa * e, (1 - e) * L.slide);
      cairo_restore(cr);
    }
  }
  if (terrain && e > 0)
    draw_terrain(cr, c, &l, &L, pw, ph, f, a * e);
  if (side && e > 0) {
    /* The column slides in from the left with the expand. */
    struct side S;
    lay_out_side(c, pw, ph, &S);
    clip_to(cr, &S.rect);
    draw_side(cr, c, &l, &S, f->peek, f->peek_low_pct, a * e,
              (1 - e) * L.slide);
  }
  cairo_restore(cr);
}
