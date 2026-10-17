/* The popup's contents drawn in the wallpaper, in place of its panel on an
 * empty workspace: the clock, date, network and stats in a column left of
 * the snowflake, battery, volume and brightness as gauges right of it, the
 * workspaces in a strip at the bottom, and the expand delay traced by a
 * ring round the snowflake. In the wallpaper's colours: topo's gradient
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

void wp_render_peek_finish(void) {
  for (int i = 0; i < TILES; ++i)
    tile_free(&tiles[i]);
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
   * and the rows' (expanded); extent with the halo */
  double clock_c, clock_e, date, rows[3];
  struct wp_rect left;
  /* right: each level's label baseline and gauge top, the gauge's height
   * clock-only and expanded */
  double label[3], bar[3], bar_c, bar_e;
  struct wp_rect right;
  /* the workspace strip's middle, cell height, widest */
  double sy, sh, smax;
  struct wp_rect strip;
  double ring_r, ring_w;
  struct wp_rect ring;
};

static void lay_out(const struct wallpaper_config *c, int pw, int ph,
                    struct layout *L) {
  double k = L->k = pw / WP_REF_WIDTH;
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

  /* Left, expanded: clock, date, three rows, centred on the snowflake;
   * clock-only, the clock's cap is. */
  double cap = 0.72 * L->sc;
  double date = cap + 0.2 * L->sc + 0.95 * L->sd;
  double row0 = date + 0.6 * L->sd + 1.5 * L->sr;
  double step = 1.6 * L->sr;
  double bottom = row0 + 2 * step + 0.3 * L->sr;
  double top = L->cy - bottom / 2;
  L->clock_e = top + cap;
  L->clock_c = L->cy + cap / 2;
  L->date = top + date;
  for (int i = 0; i < 3; ++i)
    L->rows[i] = top + row0 + i * step;
  double pad = ceil(0.3 * L->sc) + sh;
  L->left = wp_clamp_rect(
      L->lx0 - L->slide - pad, fmin(L->clock_e, L->clock_c) - 1.0 * L->sc - pad,
      L->lx1 + pad,
      fmax(L->clock_c + 0.35 * L->sc, L->rows[2] + 0.5 * L->sr) + pad, pw, ph);

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

  /* The workspaces along the bottom, under the title. */
  L->sh = 1.7 * L->sw;
  L->sy = ph - 150 * k;
  L->smax = fmin(c->media_max_width * k, pw - 2 * margin);
  pad = ceil(0.3 * L->sw) + 2 * k + sh;
  L->strip = wp_clamp_rect(L->cx - L->smax / 2 - pad, L->sy - L->sh / 2 - pad,
                           L->cx + L->smax / 2 + pad,
                           L->sy + L->sh / 2 + L->slide + pad, pw, ph);

  /* The hold ring, just past the snowflake at its biggest pulse (its
   * shadow and glow may reach under it), clear of the title. */
  L->ring_r =
      c->logo_radius * k * (1 + c->react_pulse_scale) + c->peek_ring_gap * k;
  L->ring_w = c->peek_ring_width * k;
  double rr = L->ring_r + L->ring_w + sh;
  L->ring =
      wp_clamp_rect(L->cx - rr, L->cy - rr, L->cx + rr, L->cy + rr, pw, ph);
}

void wp_peek_regions(const struct wallpaper_config *c, int pw, int ph,
                     struct wp_rect out[WP_PEEK_REGIONS]) {
  struct layout L;
  lay_out(c, pw, ph, &L);
  out[0] = L.left;
  out[1] = L.right;
  out[2] = L.ring;
  out[3] = L.strip;
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
  const struct tile *t[8];
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

static void draw_left(cairo_t *cr, const struct wallpaper_config *c,
                      const struct look *l, const struct layout *L,
                      const struct info *in, double a, double e, double off) {
  struct tm tm;
  char hhmm[16], ss[8], wday[32], rest[32], date[96];
  localtime_r(&in->now, &tm);
  strftime(hhmm, sizeof hhmm, "%H:%M", &tm);
  strftime(ss, sizeof ss, ":%S", &tm);
  strftime(wday, sizeof wday, "%A", &tm);
  strftime(rest, sizeof rest, "%B %Y", &tm);
  snprintf(date, sizeof date, "%s, %d %s", wday, tm.tm_mday, rest);

  double colw = L->lx1 - L->lx0, mid = (L->lx0 + L->lx1) / 2 - off;
  double base = L->clock_c + (L->clock_e - L->clock_c) * e;
  const struct tile *hm = tile(c, hhmm, L->sc, true, 0);
  const struct tile *sec = tile(c, ss, L->ss, false, 0);
  double gap = 0.1 * L->ss;
  /* Clock-only HH:MM is centred alone; expanding, it makes room for :SS. */
  double x = mid - hm->w / 2 - (gap + sec->w) / 2 * e;
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
  if (e <= 0)
    return;
  double ae = a * e;
  paint_col(cr, c, sec, x + hm->w + gap, base, l->dim, ae);
  double dy = base - L->clock_e; /* the rest follows the clock */
  const struct tile *dt = tile(c, date, L->sd, false, colw);
  paint_col(cr, c, dt, mid - dt->w / 2, L->date + dy, l->dim, ae);

  /* network */
  char sig[16], name[64];
  struct piece p[8];
  int n = 2;
  copy_str(name, sizeof name, in->net_name);
  switch (in->net) {
  case NET_WIFI:
    p[0] = (struct piece){icon_wifi(in->net_signal), false, true};
    p[1] = (struct piece){name[0] ? name : "wifi", false, false};
    if (in->net_signal >= 0) {
      snprintf(sig, sizeof sig, "  %d%%", in->net_signal);
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
  pieces(cr, c, l, L, p, n, mid, L->rows[0] + dy, ae);

  /* CPU and the battery's power; memory and uptime */
  char cpu[16], pwr[24], mem[32], up[32];
  long m = in->uptime_s / 60, h = m / 60, d = h / 24;
  if (in->cpu < 0)
    snprintf(cpu, sizeof cpu, "--");
  else
    snprintf(cpu, sizeof cpu, "%d%%", in->cpu);
  snprintf(pwr, sizeof pwr, "%s%.2fW", in->power_charging ? "▲" : "",
           in->power_w);
  snprintf(mem, sizeof mem, "%.1f/%.1fG", in->mem_used_gib, in->mem_total_gib);
  if (d > 0)
    snprintf(up, sizeof up, "%ldd %ldh", d, h % 24);
  else if (h > 0)
    snprintf(up, sizeof up, "%ldh %02ldm", h, m % 60);
  else
    snprintf(up, sizeof up, "%ldm", m);
  n = 0;
  p[n++] = (struct piece){"CPU ", true, false};
  p[n++] = (struct piece){cpu, false, false};
  if (in->has_power) {
    p[n++] = (struct piece){"   PWR ", true, false};
    p[n++] = (struct piece){pwr, false, false};
  }
  pieces(cr, c, l, L, p, n, mid, L->rows[1] + dy, ae);
  n = 0;
  p[n++] = (struct piece){"RAM ", true, false};
  p[n++] = (struct piece){mem, false, false};
  p[n++] = (struct piece){"   up ", true, false};
  p[n++] = (struct piece){up, false, false};
  pieces(cr, c, l, L, p, n, mid, L->rows[2] + dy, ae);
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
  double x = L->cx - (m * cw + (m - 1) * gap) / 2;
  double y = L->sy - L->sh / 2 + off;
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

/* The expand delay so far, clockwise from the top. */
static void draw_ring(cairo_t *cr, const struct look *l, const struct layout *L,
                      double hold, double a) {
  cairo_new_path(cr);
  cairo_arc(cr, L->cx, L->cy, L->ring_r, -M_PI / 2,
            -M_PI / 2 + 2 * M_PI * clamp01(hold));
  cairo_set_line_width(cr, L->ring_w);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_pattern_t *p = NULL;
  if (l->topo)
    cairo_set_source(cr, p = wp_topo_edge_pattern(l->edge, L->cx, L->cy,
                                                  L->ring_r, a, true));
  else
    set_rgba(cr, l->line, a);
  cairo_stroke(cr);
  if (p)
    cairo_pattern_destroy(p);
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
  cairo_save(cr);
  /* Like the popup's hold line, it clears out in the first half of the
   * expand. */
  double ring = a * (1 - 2 * e);
  if (f->peek_hold > 0 && ring > 0 && L.ring_w > 0)
    draw_ring(cr, &l, &L, f->peek_hold, ring);
  cairo_save(cr);
  clip_to(cr, &L.left);
  draw_left(cr, c, &l, &L, f->peek, a, e, off);
  cairo_restore(cr);
  cairo_save(cr);
  clip_to(cr, &L.right);
  draw_right(cr, c, &l, &L, f->peek, f->peek_low_pct, a, e, off);
  cairo_restore(cr);
  if (e > 0) {
    /* The workspaces rise from below with the expand. */
    clip_to(cr, &L.strip);
    draw_strip(cr, c, &l, &L, f->peek, a * e, (1 - e) * L.slide);
  }
  cairo_restore(cr);
}
