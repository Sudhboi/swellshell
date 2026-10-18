#include "render.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <stdio.h>

#include "backdrop.h"
#include "capture.h"
#include "config.h"
#include "draw.h"
#include "layout.h"
#include "util.h"
#include "zoom.h"

static void rounded(cairo_t *cr, struct box b, double r) {
  r = fmin(r, fmin(b.w, b.h) / 2);
  rounded_rect(cr, b.x, b.y, b.w, b.h, r);
}

/* The selection's gradient across b, top left to bottom right. */
static void set_accent(cairo_t *cr, const struct overview_config *cfg,
                       struct box b, double alpha) {
  cairo_pattern_t *p =
      cairo_pattern_create_linear(b.x, b.y, b.x + b.w, b.y + b.h);
  const uint32_t c[3] = {cfg->accent_a, cfg->accent_mid, cfg->accent_b};
  for (int i = 0; i < 3; ++i)
    cairo_pattern_add_color_stop_rgba(p, i / 2.0, col_r(c[i]), col_g(c[i]),
                                      col_b(c[i]), col_a(c[i]) * alpha);
  cairo_set_source(cr, p);
  cairo_pattern_destroy(p);
}

/* Path of a border lying fully inside b; false if there is no room. */
static bool inner_path(cairo_t *cr, struct box b, double r, double width) {
  struct box in = {b.x + width / 2, b.y + width / 2, b.w - width, b.h - width};
  if (in.w <= 0 || in.h <= 0)
    return false;
  rounded(cr, in, fmax(0, r - width / 2));
  cairo_set_line_width(cr, width);
  return true;
}

static void inner_border(cairo_t *cr, struct box b, double r, double width,
                         uint32_t c, double alpha) {
  if (!inner_path(cr, b, r, width))
    return;
  set_rgba(cr, c, alpha);
  cairo_stroke(cr);
}

static void accent_border(cairo_t *cr, const struct overview_config *cfg,
                          struct box b, double r, double width, double alpha) {
  if (!inner_path(cr, b, r, width))
    return;
  set_accent(cr, cfg, b, alpha);
  cairo_stroke(cr);
}

/* Three widening rings of c, a little lower than b. Opaque, b covers their
 * middle: only what shows round it is filled (the whole rings blended over
 * a big tile cost more than the rest of the frame). */
static void soft_shadow(cairo_t *cr, struct box b, double r, uint32_t c,
                        double alpha, bool opaque) {
  cairo_save(cr);
  if (opaque)
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
  for (int i = 3; i >= 1; --i) {
    double g = i * 4;
    rounded(cr, (struct box){b.x - g / 2, b.y - g / 2 + 4, b.w + g, b.h + g},
            r + g / 2);
    if (opaque)
      rounded(cr, b, r);
    set_rgba(cr, c, alpha);
    cairo_fill(cr);
  }
  cairo_restore(cr);
}

/* Start a group bounded to b: an unclipped group is a full-output scratch
 * surface, which made every drag frame several times slower. */
static void push_group_in(cairo_t *cr, struct box b) {
  cairo_save(cr);
  cairo_rectangle(cr, floor(b.x) - 1, floor(b.y) - 1, ceil(b.w) + 3,
                  ceil(b.h) + 3);
  cairo_clip(cr);
  cairo_push_group(cr);
}

static void pop_group_with_alpha(cairo_t *cr, double alpha) {
  cairo_pop_group_to_source(cr);
  cairo_paint_with_alpha(cr, alpha);
  cairo_restore(cr);
}

enum align { ALIGN_LEFT, ALIGN_CENTER };

static void draw_text(cairo_t *cr, const char *font, const char *text,
                      struct box b, enum align align, uint32_t c, double alpha,
                      bool bold) {
  if (!text || !*text || b.w <= 4 || alpha <= 0)
    return;
  PangoLayout *layout = pango_cairo_create_layout(cr);
  PangoFontDescription *desc = pango_font_description_from_string(font);
  if (bold)
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
  pango_layout_set_font_description(layout, desc);
  pango_layout_set_text(layout, text, -1);
  pango_layout_set_single_paragraph_mode(layout, true);
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
  pango_layout_set_width(layout, (int)(b.w * PANGO_SCALE));
  pango_layout_set_alignment(layout, align == ALIGN_CENTER ? PANGO_ALIGN_CENTER
                                                           : PANGO_ALIGN_LEFT);
  int tw, th;
  pango_layout_get_pixel_size(layout, &tw, &th);
  set_rgba(cr, c, alpha);
  cairo_move_to(cr, b.x, b.y + (b.h - th) / 2);
  pango_cairo_show_layout(cr, layout);
  pango_font_description_free(desc);
  g_object_unref(layout);
}

static const char *window_label(struct window *w) {
  return w->app_id && *w->app_id ? w->app_id : (w->title ? w->title : "?");
}

/* Letterbox box b to the aspect ratio of w's latest frame. */
static struct box fit_frame(struct window *w, struct box b) {
  int fw, fh;
  if (ov_capture_size(w->cap, &fw, &fh) && fw > 0 && fh > 0) {
    double s = fmin(b.w / fw, b.h / fh);
    if (fabs(s * fw - b.w) > 2 || fabs(s * fh - b.h) > 2)
      b = (struct box){b.x + (b.w - s * fw) / 2, b.y + (b.h - s * fh) / 2,
                       s * fw, s * fh};
  }
  return b;
}

/* Window contents into container box b / content box cb, rounded by r. The
 * thumbnail is rendered at the resolution of res (cb if NULL) and scaled to
 * fit, so an animated box doesn't regenerate it every frame. The placeholder
 * behind it is drawn at alpha, the thumbnail at thumb_alpha. */
static void draw_window_body(struct ov *ov, cairo_t *cr, struct window *w,
                             struct box b, struct box cb, const struct box *res,
                             double scale, double r, double alpha,
                             double thumb_alpha) {
  cairo_save(cr);
  rounded(cr, b, r);
  cairo_clip(cr);
  set_rgba(cr, ov->cfg.window, alpha);
  cairo_paint(cr);

  /* Letterbox when the slot's shape differs from the window's, e.g. for
   * tabs spread side by side. */
  cb = fit_frame(w, cb);
  struct box rb = res ? fit_frame(w, *res) : cb;
  int pw = (int)lround(rb.w * scale), ph = (int)lround(rb.h * scale);
  if (res && fabs(cb.w * scale - pw) < 1 && fabs(cb.h * scale - ph) < 1) {
    /* Drawn at full size: align to device pixels so every thumbnail pixel
     * maps to exactly one screen pixel instead of being smeared by a
     * fractional offset. */
    cb = (struct box){round(cb.x * scale) / scale, round(cb.y * scale) / scale,
                      pw / scale, ph / scale};
  }
  cairo_surface_t *thumb = ov_capture_thumbnail(w->cap, pw, ph);
  if (thumb) {
    cairo_rectangle(cr, cb.x, cb.y, cb.w, cb.h);
    cairo_clip(cr);
    cairo_translate(cr, cb.x, cb.y);
    cairo_scale(cr, cb.w / pw, cb.h / ph);
    cairo_set_source_surface(cr, thumb, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), res ? CAIRO_FILTER_BILINEAR
                                                       : CAIRO_FILTER_FAST);
    cairo_paint_with_alpha(cr, thumb_alpha);
  } else {
    struct box tb = {b.x + 6, b.y, b.w - 12, b.h};
    draw_text(cr, ov->cfg.font, window_label(w), tb, ALIGN_CENTER,
              ov->cfg.text_dim, alpha, false);
  }
  cairo_restore(cr);
}

/* How a workspace is drawn: chrome = alpha of everything but the
 * thumbnails, corner = share of the corners' radius; thumb_scale > 0
 * renders thumbnails at that scale of their laid-out size (a workspace
 * zooming out of the screen); thumb_alpha = the thumbnails' alpha;
 * own_label: the caller draws the name (unscaled, see draw_moved_label). */
struct look {
  double chrome;
  double corner;
  double thumb_scale;
  double thumb_alpha;
  bool own_label;
};

static const struct look settled = {1, 1, 0, 1, false};

static void draw_window(struct ov *ov, cairo_t *cr, struct window *w,
                        double scale, struct look lk) {
  const struct overview_config *cfg = &ov->cfg;
  double r = cfg->window_radius * lk.corner, a = lk.chrome;
  bool dragging = ov->drag.active && ov->drag.press.win == w;
  if (dragging)
    push_group_in(cr, w->box);

  if (lk.thumb_scale > 0)
    draw_window_body(ov, cr, w, w->box, w->cbox, &w->cbox, lk.thumb_scale, r,
                     a, lk.thumb_alpha);
  else
    draw_window_body(ov, cr, w, w->box, w->cbox, NULL, scale, r, a,
                     lk.thumb_alpha);

  if (w->urgent)
    inner_border(cr, w->box, r, 2, cfg->urgent, a);
  else
    inner_border(cr, w->box, r, 1, cfg->window_border, a);
  if (ov->selected.win == w)
    accent_border(cr, cfg, w->box, r, 3, a);
  else if (ov->hover.win == w && !ov->drag.active)
    inner_border(cr, w->box, r, 2, cfg->hover, a);

  if (dragging)
    pop_group_with_alpha(cr, cfg->drag_source_alpha);
}

static bool dragging_workspace(struct ov *ov, struct workspace *ws) {
  return ov->drag.active && !ov->drag.press.win && ov->drag.press.ws == ws;
}

static void draw_placeholder(struct ov *ov, cairo_t *cr, struct workspace *ws,
                             bool drop_target, double radius, double a) {
  const struct overview_config *cfg = &ov->cfg;
  rounded(cr, ws->box, radius);
  set_rgba(cr, drop_target ? cfg->new_slot_drop_fill : cfg->new_slot_fill, a);
  cairo_fill_preserve(cr);
  double dash[] = {8, 6};
  cairo_set_dash(cr, dash, 2, 0);
  cairo_set_line_width(cr, 2);
  if (drop_target || ov->hover.ws == ws || ov->selected.ws == ws)
    set_accent(cr, cfg, ws->box, a);
  else
    set_rgba(cr, cfg->tile_border, a);
  cairo_stroke(cr);
  cairo_set_dash(cr, NULL, 0, 0);
  double cx = ws->box.x + ws->box.w / 2, cy = ws->box.y + ws->box.h / 2;
  double arm = fmin(ws->box.w, ws->box.h) * 0.08;
  cairo_move_to(cr, cx - arm, cy);
  cairo_line_to(cr, cx + arm, cy);
  cairo_move_to(cr, cx, cy - arm);
  cairo_line_to(cr, cx, cy + arm);
  cairo_set_line_width(cr, 3);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  set_rgba(cr, cfg->text_dim, a);
  cairo_stroke(cr);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
  if (ov->selected.ws == ws && !ov->selected.win)
    accent_border(cr, cfg, ws->box, radius, 3, a);
}

/* The workspace's name: the focused one bright and bold, the rest dim. */
static void draw_label(struct ov *ov, cairo_t *cr, struct workspace *ws,
                       double alpha) {
  const struct overview_config *cfg = &ov->cfg;
  char label[256];
  if (ws->placeholder)
    snprintf(label, sizeof(label), "New workspace");
  else
    snprintf(label, sizeof(label), "%s", ws->name);
  uint32_t lc = ws->urgent    ? cfg->urgent
                : ws->focused ? cfg->text
                              : cfg->text_dim;
  draw_text(cr, cfg->font, label,
            (struct box){ws->label.x + 2, ws->label.y, ws->label.w - 4,
                         ws->label.h - 6},
            ALIGN_LEFT, lc, alpha, ws->focused);
}

/* The name at its own size with its corner moved to x, y: text drawn at a
 * new scale every frame is rasterised afresh each time (pango's glyph cache
 * misses), which took half of every zoom frame. */
static void draw_moved_label(struct ov *ov, cairo_t *cr, struct workspace *ws,
                             double x, double y, double alpha) {
  cairo_save(cr);
  cairo_translate(cr, x - ws->label.x, y - ws->label.y);
  draw_label(ov, cr, ws, alpha);
  cairo_restore(cr);
}

static void draw_workspace(struct ov *ov, cairo_t *cr, struct workspace *ws,
                           double scale, struct look lk) {
  const struct overview_config *cfg = &ov->cfg;
  const double radius = cfg->tile_radius * lk.corner, a = lk.chrome;
  bool faded = dragging_workspace(ov, ws);
  if (faded)
    push_group_in(cr, (struct box){ws->box.x, ws->label.y, ws->box.w,
                                   ws->box.y + ws->box.h - ws->label.y});
  bool drop_target = ov->drag.active && ov->hover.ws == ws &&
                     ov->drag.press.win && ov->drag.press.win->ws != ws;

  if (!lk.own_label)
    draw_label(ov, cr, ws, a);

  if (ws->placeholder) {
    draw_placeholder(ov, cr, ws, drop_target, radius, a);
    if (faded)
      pop_group_with_alpha(cr, cfg->drag_source_alpha);
    return;
  }

  soft_shadow(cr, ws->box, radius, cfg->tile_shadow, a,
              a >= 1 && col_a(cfg->tile) >= 1);

  /* tile background, then windows clipped to the rounded tile */
  cairo_save(cr);
  rounded(cr, ws->box, radius);
  cairo_clip(cr);
  set_rgba(cr, cfg->tile, a);
  cairo_paint(cr);
  for (int i = 0; i < ws->nwindows; ++i)
    draw_window(ov, cr, ws->windows[i], scale, lk);
  if (drop_target) {
    set_rgba(cr, cfg->drop_fill, a);
    cairo_paint(cr);
  }
  cairo_restore(cr);

  /* one strong outline at most: the selection (or a drop) in the gradient,
   * the focused workspace only a light rim */
  if (drop_target || (ov->selected.ws == ws && !ov->selected.win)) {
    accent_border(cr, cfg, ws->box, radius, 3, a);
  } else if (ov->hover.ws == ws && !ov->hover.win && !ov->drag.active) {
    inner_border(cr, ws->box, radius, 2, cfg->hover, a);
  } else {
    uint32_t bc = ws->focused   ? cfg->focused_border
                  : ws->visible ? cfg->tile_visible
                                : cfg->tile_border;
    inner_border(cr, ws->box, radius, ws->focused ? 1.5 : 1, bc, a);
  }

  if (faded)
    pop_group_with_alpha(cr, cfg->drag_source_alpha);
}

/* While a workspace is dragged: a bar in the gap where it would land, and
 * the workspace itself following the pointer. */
static void draw_workspace_drag(struct ov *ov, struct output *out, cairo_t *cr,
                                double scale) {
  const struct overview_config *cfg = &ov->cfg;
  struct workspace *ws = ov->drag.press.ws;
  if (!ov->drag.active || ov->drag.press.win || !ws || ws->output != out)
    return;

  struct workspace *t = ov->hover.ws;
  if (ov->ptr_output == out && ov_layout_drop_slot(ws, t) >= 0) {
    bool after = false;
    /* if ws comes before t it lands after t, else before it */
    for (int i = 0; !t->placeholder && i < out->nworkspaces; ++i) {
      if (out->workspaces[i] == ws)
        after = true;
      else if (out->workspaces[i] == t)
        break;
    }
    double x = after ? t->box.x + t->box.w + cfg->layout_gap / 2
                     : t->box.x - cfg->layout_gap / 2;
    struct box bar = {x - 3, t->label.y, 6,
                      t->box.y + t->box.h - t->label.y};
    rounded(cr, bar, 3);
    set_accent(cr, cfg, bar, 1);
    cairo_fill(cr);
  }

  if (ov->ptr_output != out)
    return;
  double dx = ov->px - ov->drag.grab_dx - ws->box.x;
  double dy = ov->py - ov->drag.grab_dy - ws->box.y;
  struct box b = {ws->box.x + dx, ws->box.y + dy, ws->box.w, ws->box.h};
  double radius = cfg->tile_radius;

  rounded(cr, (struct box){b.x + 4, b.y + 6, b.w, b.h}, radius);
  set_rgba(cr, cfg->shadow, 1);
  cairo_fill(cr);

  push_group_in(cr, (struct box){b.x, b.y - cfg->label_height, b.w,
                                 b.h + cfg->label_height});
  cairo_save(cr);
  rounded(cr, b, radius);
  cairo_clip(cr);
  set_rgba(cr, cfg->tile, 1);
  cairo_paint(cr);
  cairo_translate(cr, dx, dy);
  for (int i = 0; i < ws->nwindows; ++i) {
    struct window *w = ws->windows[i];
    draw_window_body(ov, cr, w, w->box, w->cbox, NULL, scale,
                     cfg->window_radius, 1, 1);
    inner_border(cr, w->box, cfg->window_radius, 1, cfg->window_border, 1);
  }
  cairo_restore(cr);
  accent_border(cr, cfg, b, radius, 2, 1);
  draw_text(cr, cfg->font, ws->name,
            (struct box){b.x + 2, b.y - cfg->label_height, b.w - 4,
                         cfg->label_height - 6},
            ALIGN_LEFT, cfg->text, 1, true);
  pop_group_with_alpha(cr, cfg->drag_ghost_alpha);
}

static void draw_drag_ghost(struct ov *ov, struct output *out, cairo_t *cr,
                            double scale) {
  const struct overview_config *cfg = &ov->cfg;
  struct window *w = ov->drag.press.win;
  if (!ov->drag.active || !w || ov->ptr_output != out)
    return;
  double dx = ov->px - ov->drag.grab_dx - w->box.x;
  double dy = ov->py - ov->drag.grab_dy - w->box.y;
  struct box b = {w->box.x + dx, w->box.y + dy, w->box.w, w->box.h};
  struct box cb = {w->cbox.x + dx, w->cbox.y + dy, w->cbox.w, w->cbox.h};
  double r = cfg->window_radius;

  /* soft shadow */
  rounded(cr, (struct box){b.x + 4, b.y + 6, b.w, b.h}, r);
  set_rgba(cr, cfg->shadow, 1);
  cairo_fill(cr);

  push_group_in(cr, b);
  draw_window_body(ov, cr, w, b, cb, NULL, scale, r, 1, 1);
  accent_border(cr, cfg, b, r, 2, 1);
  pop_group_with_alpha(cr, cfg->drag_ghost_alpha);
}

static void draw_zoom(struct ov *ov, struct output *out, cairo_t *cr,
                      double scale) {
  const struct overview_config *cfg = &ov->cfg;
  struct box b;
  if (!ov_zoom_box(ov, out, now_ms(), &b))
    return;
  struct window *w = ov->zoom.win;
  const double radius = cfg->window_radius + 2;

  soft_shadow(cr, b, radius, cfg->zoom_shadow, 1, col_a(cfg->window) >= 1);

  cairo_save(cr);
  rounded(cr, b, radius);
  cairo_clip(cr);
  draw_window_body(ov, cr, w, b, b, &ov->zoom.big, scale, radius, 1, 1);

  /* title band along the bottom */
  double band = fmin(34, b.h * 0.2);
  cairo_rectangle(cr, b.x, b.y + b.h - band, b.w, band);
  set_rgba(cr, cfg->zoom_title_band, 1);
  cairo_fill(cr);
  draw_text(cr, cfg->font, w->title ? w->title : window_label(w),
            (struct box){b.x + 12, b.y + b.h - band, b.w - 24, band},
            ALIGN_LEFT, cfg->text, 1, false);
  cairo_restore(cr);

  if (ov->selected.win == w)
    accent_border(cr, cfg, b, radius, 3, 1);
  else
    inner_border(cr, b, radius, 2, cfg->hover, 1);
}

static void draw_caption(struct ov *ov, struct output *out, cairo_t *cr,
                         double alpha) {
  const struct overview_config *cfg = &ov->cfg;
  struct window *w = ov->hover.win ? ov->hover.win : ov->selected.win;
  if (ov->drag.active)
    w = ov->drag.press.win;
  if (!w || w->ws->output != out)
    return;
  char text[1024];
  if (w->app_id && w->title)
    snprintf(text, sizeof(text), "%s  —  %s", w->title, w->app_id);
  else
    snprintf(text, sizeof(text), "%s", window_label(w));
  struct box b = {cfg->layout_pad,
                  out->height - cfg->layout_pad - cfg->caption_height / 2,
                  out->width - 2 * cfg->layout_pad, cfg->caption_height};
  draw_text(cr, cfg->font, text, b, ALIGN_CENTER, cfg->text, alpha, false);
}

void ov_render_background(struct ov *ov, struct output *out, cairo_t *cr,
                          double scale) {
  const struct backdrop_source *bd = backdrop_get_source();
  bool painted = ov->cfg.backdrop && bd &&
                 bd->paint(cr, out->width, out->height, scale);
  set_rgba(cr, painted ? ov->cfg.background : ov->cfg.background_plain, 1);
  cairo_paint(cr);
}

/* Without the subsurface: the background composed once at buffer
 * resolution and copied into every frame (scaling the map up costs ~25 ms a
 * frame on a 2880x1800 output). */
static cairo_surface_t *background(struct ov *ov, struct output *out,
                                   double scale) {
  int pw = (int)lround(out->width * scale), ph = (int)lround(out->height * scale);
  cairo_surface_t *s = out->background;
  if (s && cairo_image_surface_get_width(s) == pw &&
      cairo_image_surface_get_height(s) == ph)
    return s;
  if (s)
    cairo_surface_destroy(s);
  s = out->background = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
  cairo_t *cr = cairo_create(s);
  cairo_scale(cr, scale, scale);
  ov_render_background(ov, out, cr, scale);
  cairo_destroy(cr);
  out->bg_ready = true;
  return s;
}

static void draw_background(struct ov *ov, struct output *out, cairo_t *cr,
                            double scale, double k) {
  cairo_save(cr);
  cairo_identity_matrix(cr); /* pixel for pixel */
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  if (out->bg_surface) {
    cairo_set_source_rgba(cr, 0, 0, 0, 0); /* it shows from below */
    cairo_paint(cr);
  } else if (k >= 1) {
    cairo_set_source_surface(cr, background(ov, out, scale), 0, 0);
    cairo_paint(cr);
  } else {
    cairo_surface_t *bg = background(ov, out, scale); /* before the zoom */
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    if (k > 0) {
      cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
      cairo_set_source_surface(cr, bg, 0, 0);
      cairo_paint_with_alpha(cr, k);
    }
  }
  cairo_restore(cr);
}

void ov_render_warm(struct ov *ov, struct output *out, double scale) {
  for (int i = 0; i < out->nworkspaces; ++i) {
    struct workspace *ws = out->workspaces[i];
    int w = (int)ceil(ws->label.w * scale), h = (int)ceil(ws->label.h * scale);
    if (w <= 0 || h <= 0)
      continue;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_A8, w, h);
    cairo_t *cr = cairo_create(s);
    cairo_scale(cr, scale, scale);
    cairo_translate(cr, -ws->label.x, -ws->label.y);
    draw_label(ov, cr, ws, 1);
    cairo_destroy(cr);
    cairo_surface_destroy(s);
  }
}

/* The screen box ws's windows really occupy, in out's coordinates. */
static struct box screen_box(struct workspace *ws, struct output *out) {
  if (ws->rect.w > 0 && ws->rect.h > 0)
    return (struct box){ws->rect.x - out->gx, ws->rect.y - out->gy, ws->rect.w,
                        ws->rect.h};
  return (struct box){0, 0, out->width, out->height};
}

static double lerp(double a, double b, double t) { return a + (b - a) * t; }

/* The shown workspace partway (k) between the screen and its tile: its
 * windows sit where they really are at 0, thumbnails opaque, the rest fading
 * in with k. */
static void draw_hero(struct ov *ov, struct output *out, cairo_t *cr,
                      struct workspace *ws, double scale, double k) {
  struct box from = screen_box(ws, out), to = ws->inner;
  if (to.w <= 0 || to.h <= 0)
    return;
  struct box cur = {lerp(from.x, to.x, k), lerp(from.y, to.y, k),
                    lerp(from.w, to.w, k), lerp(from.h, to.h, k)};
  double fx = cur.w / to.w, fy = cur.h / to.h;
  cairo_save(cr);
  cairo_translate(cr, cur.x, cur.y);
  cairo_scale(cr, fx, fy);
  cairo_translate(cr, -to.x, -to.y);
  /* thumbnails at the screen's resolution, scaled down as it shrinks */
  draw_workspace(ov, cr, ws, scale,
                 (struct look){k, k, scale * fmax(from.w / to.w, 1), 1, true});
  cairo_restore(cr);
  draw_moved_label(ov, cr, ws, cur.x + (ws->label.x - to.x) * fx,
                   cur.y + (ws->label.y - to.y) * fy, k);
}

/* Any other workspace: fading in and growing from a little smaller. Drawn
 * straight at alpha k rather than through a group (a scratch surface per
 * tile per frame); the tile showing faintly through its thumbnails
 * mid-fade doesn't show. */
static void draw_fading(struct ov *ov, cairo_t *cr, struct workspace *ws,
                        double scale, double k) {
  double s = 0.94 + 0.06 * k;
  double cx = ws->box.x + ws->box.w / 2, cy = ws->box.y + ws->box.h / 2;
  cairo_save(cr);
  cairo_translate(cr, cx, cy);
  cairo_scale(cr, s, s);
  cairo_translate(cr, -cx, -cy);
  draw_workspace(ov, cr, ws, scale, (struct look){k, 1, 0, k, true});
  cairo_restore(cr);
  draw_moved_label(ov, cr, ws, cx + (ws->label.x - cx) * s,
                   cy + (ws->label.y - cy) * s, k);
}

void ov_render_output(struct ov *ov, struct output *out, cairo_t *cr,
                      double scale) {
  double k = ov_anim_value(ov, now_ms());
  draw_background(ov, out, cr, scale, k);

  if (k < 1) {
    struct workspace *hero = ov_anim_hero(ov, out);
    for (int i = 0; i < out->nworkspaces; ++i)
      if (out->workspaces[i] != hero && k > 0)
        draw_fading(ov, cr, out->workspaces[i], scale, k);
    if (hero)
      draw_hero(ov, out, cr, hero, scale, k);
    return;
  }

  for (int i = 0; i < out->nworkspaces; ++i)
    draw_workspace(ov, cr, out->workspaces[i], scale, settled);
  draw_caption(ov, out, cr, 1);
  draw_zoom(ov, out, cr, scale);
  draw_drag_ghost(ov, out, cr, scale);
  draw_workspace_drag(ov, out, cr, scale);
}
