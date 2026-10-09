#include "render.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <stdio.h>

#include "capture.h"
#include "config.h"
#include "layout.h"
#include "util.h"
#include "zoom.h"

static void set_color(cairo_t *cr, uint32_t c) {
  cairo_set_source_rgba(cr, col_r(c), col_g(c), col_b(c), col_a(c));
}

static void rounded(cairo_t *cr, struct box b, double r) {
  r = fmin(r, fmin(b.w, b.h) / 2);
  cairo_new_sub_path(cr);
  cairo_arc(cr, b.x + b.w - r, b.y + r, r, -M_PI / 2, 0);
  cairo_arc(cr, b.x + b.w - r, b.y + b.h - r, r, 0, M_PI / 2);
  cairo_arc(cr, b.x + r, b.y + b.h - r, r, M_PI / 2, M_PI);
  cairo_arc(cr, b.x + r, b.y + r, r, M_PI, 3 * M_PI / 2);
  cairo_close_path(cr);
}

/* Stroke a border lying fully inside b. */
static void inner_border(cairo_t *cr, struct box b, double r, double width,
                         uint32_t c) {
  struct box in = {b.x + width / 2, b.y + width / 2, b.w - width, b.h - width};
  if (in.w <= 0 || in.h <= 0)
    return;
  rounded(cr, in, r);
  set_color(cr, c);
  cairo_set_line_width(cr, width);
  cairo_stroke(cr);
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
                      struct box b, enum align align, uint32_t c, bool bold) {
  if (!text || !*text || b.w <= 4)
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
  set_color(cr, c);
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

/* Window contents into container box b / content box cb. The thumbnail is
 * rendered at the resolution of res (cb if NULL) and scaled to fit, so an
 * animated box doesn't regenerate it every frame. */
static void draw_window_body(struct ov *ov, cairo_t *cr, struct window *w,
                             struct box b, struct box cb, const struct box *res,
                             double scale) {
  set_color(cr, ov->cfg.window);
  cairo_rectangle(cr, b.x, b.y, b.w, b.h);
  cairo_fill(cr);

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
    cairo_save(cr);
    cairo_rectangle(cr, cb.x, cb.y, cb.w, cb.h);
    cairo_clip(cr);
    cairo_translate(cr, cb.x, cb.y);
    cairo_scale(cr, cb.w / pw, cb.h / ph);
    cairo_set_source_surface(cr, thumb, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), res ? CAIRO_FILTER_BILINEAR
                                                       : CAIRO_FILTER_FAST);
    cairo_paint(cr);
    cairo_restore(cr);
  } else {
    struct box tb = {b.x + 6, b.y, b.w - 12, b.h};
    draw_text(cr, ov->cfg.font, window_label(w), tb, ALIGN_CENTER,
              ov->cfg.text_dim, false);
  }
}

static void draw_window(struct ov *ov, cairo_t *cr, struct window *w,
                        double scale) {
  const struct overview_config *cfg = &ov->cfg;
  bool dragging = ov->drag.active && ov->drag.press.win == w;
  if (dragging)
    push_group_in(cr, w->box);

  draw_window_body(ov, cr, w, w->box, w->cbox, NULL, scale);

  if (w->urgent)
    inner_border(cr, w->box, 0, 2, cfg->urgent);
  else
    inner_border(cr, w->box, 0, 1, cfg->window_border);
  if (ov->selected.win == w)
    inner_border(cr, w->box, 2, 3, cfg->accent);
  else if (ov->hover.win == w && !ov->drag.active)
    inner_border(cr, w->box, 2, 2, cfg->hover);

  if (dragging)
    pop_group_with_alpha(cr, cfg->drag_source_alpha);
}

static bool dragging_workspace(struct ov *ov, struct workspace *ws) {
  return ov->drag.active && !ov->drag.press.win && ov->drag.press.ws == ws;
}

static void draw_workspace(struct ov *ov, cairo_t *cr, struct workspace *ws,
                           double scale) {
  const struct overview_config *cfg = &ov->cfg;
  const double radius = 8;
  bool faded = dragging_workspace(ov, ws);
  if (faded)
    push_group_in(cr, (struct box){ws->box.x, ws->label.y, ws->box.w,
                                   ws->box.y + ws->box.h - ws->label.y});
  bool drop_target = ov->drag.active && ov->hover.ws == ws &&
                     ov->drag.press.win && ov->drag.press.win->ws != ws;

  /* label */
  char label[256];
  if (ws->placeholder)
    snprintf(label, sizeof(label), "New workspace");
  else
    snprintf(label, sizeof(label), "%s", ws->name);
  uint32_t lc = ws->urgent ? cfg->urgent : ws->focused ? cfg->accent : cfg->text;
  draw_text(cr, cfg->font, label,
            (struct box){ws->label.x + 2, ws->label.y, ws->label.w - 4,
                         ws->label.h - 6},
            ALIGN_LEFT, lc, ws->focused);

  if (ws->placeholder) {
    rounded(cr, ws->box, radius);
    set_color(cr, drop_target ? cfg->new_slot_drop_fill : cfg->new_slot_fill);
    cairo_fill_preserve(cr);
    double dash[] = {8, 6};
    cairo_set_dash(cr, dash, 2, 0);
    cairo_set_line_width(cr, 2);
    set_color(cr, (ov->hover.ws == ws || ov->selected.ws == ws)
                      ? cfg->accent
                      : cfg->tile_border);
    cairo_stroke(cr);
    cairo_set_dash(cr, NULL, 0, 0);
    double cx = ws->box.x + ws->box.w / 2, cy = ws->box.y + ws->box.h / 2;
    double arm = fmin(ws->box.w, ws->box.h) * 0.12;
    cairo_move_to(cr, cx - arm, cy);
    cairo_line_to(cr, cx + arm, cy);
    cairo_move_to(cr, cx, cy - arm);
    cairo_line_to(cr, cx, cy + arm);
    cairo_set_line_width(cr, 3);
    set_color(cr, cfg->text_dim);
    cairo_stroke(cr);
    if (ov->selected.ws == ws && !ov->selected.win)
      inner_border(cr, ws->box, radius, 3, cfg->accent);
    return;
  }

  /* tile background, then windows clipped to the rounded tile */
  cairo_save(cr);
  rounded(cr, ws->box, radius);
  cairo_clip(cr);
  set_color(cr, cfg->tile);
  cairo_paint(cr);
  for (int i = 0; i < ws->nwindows; ++i)
    draw_window(ov, cr, ws->windows[i], scale);
  if (drop_target) {
    set_color(cr, cfg->drop_fill);
    cairo_paint(cr);
  }
  cairo_restore(cr);

  /* tile border; keep the focused workspace subtler than the selection */
  uint32_t bc = ws->focused   ? cfg->focused_border
                : ws->visible ? cfg->tile_visible
                              : cfg->tile_border;
  double bw = ws->focused ? 2 : 1;
  if (drop_target) {
    bc = cfg->accent;
    bw = 3;
  }
  inner_border(cr, ws->box, radius, bw, bc);
  if (ov->selected.ws == ws && !ov->selected.win)
    inner_border(cr, ws->box, radius, 3, cfg->accent);
  else if (ov->hover.ws == ws && !ov->hover.win && !ov->drag.active)
    inner_border(cr, ws->box, radius, 2, cfg->hover);

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
    rounded(cr,
            (struct box){x - 3, t->label.y, 6,
                         t->box.y + t->box.h - t->label.y},
            3);
    set_color(cr, cfg->accent);
    cairo_fill(cr);
  }

  if (ov->ptr_output != out)
    return;
  double dx = ov->px - ov->drag.grab_dx - ws->box.x;
  double dy = ov->py - ov->drag.grab_dy - ws->box.y;
  struct box b = {ws->box.x + dx, ws->box.y + dy, ws->box.w, ws->box.h};

  rounded(cr, (struct box){b.x + 4, b.y + 6, b.w, b.h}, 8);
  set_color(cr, cfg->shadow);
  cairo_fill(cr);

  push_group_in(cr, (struct box){b.x, b.y - cfg->label_height, b.w,
                                 b.h + cfg->label_height});
  cairo_save(cr);
  rounded(cr, b, 8);
  cairo_clip(cr);
  set_color(cr, cfg->tile);
  cairo_paint(cr);
  cairo_translate(cr, dx, dy);
  for (int i = 0; i < ws->nwindows; ++i) {
    struct window *w = ws->windows[i];
    draw_window_body(ov, cr, w, w->box, w->cbox, NULL, scale);
    inner_border(cr, w->box, 0, 1, cfg->window_border);
  }
  cairo_restore(cr);
  inner_border(cr, b, 8, 2, cfg->accent);
  draw_text(cr, cfg->font, ws->name,
            (struct box){b.x + 2, b.y - cfg->label_height, b.w - 4,
                         cfg->label_height - 6},
            ALIGN_LEFT, cfg->text, true);
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

  /* soft shadow */
  rounded(cr, (struct box){b.x + 4, b.y + 6, b.w, b.h}, 4);
  set_color(cr, cfg->shadow);
  cairo_fill(cr);

  push_group_in(cr, b);
  draw_window_body(ov, cr, w, b, cb, NULL, scale);
  inner_border(cr, b, 2, 2, cfg->accent);
  pop_group_with_alpha(cr, cfg->drag_ghost_alpha);
}

static void draw_zoom(struct ov *ov, struct output *out, cairo_t *cr,
                      double scale) {
  const struct overview_config *cfg = &ov->cfg;
  struct box b;
  if (!ov_zoom_box(ov, out, now_ms(), &b))
    return;
  struct window *w = ov->zoom.win;
  const double radius = 6;

  /* soft shadow */
  for (int i = 3; i >= 1; --i) {
    double g = i * 4;
    rounded(cr, (struct box){b.x - g / 2, b.y - g / 2 + 4, b.w + g, b.h + g},
            radius + g / 2);
    set_color(cr, cfg->zoom_shadow);
    cairo_fill(cr);
  }

  cairo_save(cr);
  rounded(cr, b, radius);
  cairo_clip(cr);
  draw_window_body(ov, cr, w, b, b, &ov->zoom.big, scale);

  /* title band along the bottom */
  double band = fmin(34, b.h * 0.2);
  cairo_rectangle(cr, b.x, b.y + b.h - band, b.w, band);
  set_color(cr, cfg->zoom_title_band);
  cairo_fill(cr);
  draw_text(cr, cfg->font, w->title ? w->title : window_label(w),
            (struct box){b.x + 12, b.y + b.h - band, b.w - 24, band},
            ALIGN_LEFT, cfg->text, false);
  cairo_restore(cr);

  inner_border(cr, b, radius, ov->selected.win == w ? 3 : 2,
               ov->selected.win == w ? cfg->accent : cfg->hover);
}

static void draw_caption(struct ov *ov, struct output *out, cairo_t *cr) {
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
  draw_text(cr, cfg->font, text, b, ALIGN_CENTER, cfg->text, false);
}

void ov_render_output(struct ov *ov, struct output *out, cairo_t *cr,
                      double scale) {
  cairo_save(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  set_color(cr, ov->cfg.background);
  cairo_paint(cr);
  cairo_restore(cr);

  for (int i = 0; i < out->nworkspaces; ++i)
    draw_workspace(ov, cr, out->workspaces[i], scale);
  draw_caption(ov, out, cr);
  draw_zoom(ov, out, cr, scale);
  draw_drag_ghost(ov, out, cr, scale);
  draw_workspace_drag(ov, out, cr, scale);
}
