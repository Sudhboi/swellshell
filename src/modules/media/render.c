/* Cairo drawing of the media panel: the cover on the left; on the right
 * the player, title, artist and album, and the progress bar with its
 * times. */
#include "render.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <string.h>

#include "draw.h"

/* Nerd Font icon, UTF-8 */
static const char ICON_MUSIC[] = "\U000F075A";

/* The config of the frame being drawn (set by media_render). */
static const struct media_config *C;

/* Text is shaped by Pango, so glyphs missing from C->font (CJK, …) fall
 * back to other fonts. maxw > 0 ellipsises at the end past it. */
static PangoLayout *layout(cairo_t *cr, const char *s, double size,
                           bool bold, double maxw) {
  PangoLayout *l = pango_cairo_create_layout(cr);
  PangoFontDescription *fd = pango_font_description_from_string(C->font);
  pango_font_description_set_absolute_size(fd, size * PANGO_SCALE);
  pango_font_description_set_weight(fd, bold ? PANGO_WEIGHT_BOLD
                                             : PANGO_WEIGHT_NORMAL);
  pango_layout_set_font_description(l, fd);
  pango_font_description_free(fd);
  pango_layout_set_single_paragraph_mode(l, TRUE);
  if (maxw > 0) {
    pango_layout_set_width(l, (int)(maxw * PANGO_SCALE));
    pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
  }
  pango_layout_set_text(l, s, -1);
  return l;
}

static double advance(cairo_t *cr, const char *s, double size) {
  PangoLayout *l = layout(cr, s, size, false, 0);
  PangoRectangle log;
  pango_layout_get_extents(l, NULL, &log);
  g_object_unref(l);
  return (double)log.width / PANGO_SCALE;
}

/* Draw s with its baseline at (x, y). */
static void text_at(cairo_t *cr, double x, double y, const char *s,
                    double size, bool bold, double maxw, uint32_t col) {
  PangoLayout *l = layout(cr, s, size, bold, maxw);
  set_rgba(cr, col, 1.0);
  cairo_move_to(cr, x, y - (double)pango_layout_get_baseline(l) / PANGO_SCALE);
  pango_cairo_show_layout(cr, l);
  cairo_new_path(cr);
  g_object_unref(l);
}

/* Height of a capital/digit above the baseline at size (C->font's metrics,
 * so the layout doesn't move with the text). */
static double cap_height(cairo_t *cr, double size) {
  cairo_select_font_face(cr, C->font, CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, size);
  cairo_text_extents_t ext;
  cairo_text_extents(cr, "H0", &ext);
  return -ext.y_bearing;
}

static void panel_path(cairo_t *cr, double x, double y, double w, double h,
                       double r) {
  if (r <= 0) {
    cairo_rectangle(cr, x, y, w, h);
    return;
  }
  if (r > w / 2)
    r = w / 2;
  if (r > h / 2)
    r = h / 2;
  rounded_rect(cr, x, y, w, h, r);
}

void media_format_time(char *out, size_t n, int64_t us) {
  long s = us > 0 ? (long)(us / 1000000) : 0;
  if (s >= 3600)
    snprintf(out, n, "%ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
  else
    snprintf(out, n, "%ld:%02ld", s / 60, s % 60);
}

bool media_render_moving(const struct media_track *t) {
  return t && t->has && t->playing && t->len_us <= 0;
}

/* Covers scaled to the square's device size, one per slot, so a frame
 * doesn't resample the full image. */
static struct art_cache {
  cairo_surface_t *s;
  unsigned id;
  int px;
} cache[2];

static cairo_surface_t *scaled_art(cairo_t *cr, const struct media_slot *sl,
                                   double side) {
  double dx = side, dy = 0;
  cairo_user_to_device_distance(cr, &dx, &dy);
  int px = (int)lround(hypot(dx, dy));
  if (px <= 0)
    return NULL;
  for (int i = 0; i < 2; ++i)
    if (cache[i].s && cache[i].id == sl->art_id && cache[i].px == px)
      return cache[i].s;
  /* Replace the entry the other slot isn't using: the older one. */
  struct art_cache *e = &cache[0];
  if (cache[0].s && (!cache[1].s || cache[1].id < cache[0].id))
    e = &cache[1];
  if (e->s)
    cairo_surface_destroy(e->s);
  e->s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, px, px);
  e->id = sl->art_id;
  e->px = px;
  cairo_t *c = cairo_create(e->s);
  double k = (double)px / cairo_image_surface_get_width(sl->art);
  cairo_scale(c, k, k);
  cairo_set_source_surface(c, sl->art, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(c), CAIRO_FILTER_GOOD);
  cairo_paint(c);
  cairo_destroy(c);
  return e->s;
}

/* The status icon in an s x s box at (x, y): a play triangle at p 0, two
 * pause bars at 1. Each half of the triangle (split down the middle)
 * stretches into one bar, corner by corner. */
static void draw_status(cairo_t *cr, double x, double y, double s, double p,
                        uint32_t col) {
  double w = s * 0.34; /* a bar's width */
  /* corners top-left, top-right, bottom-right, bottom-left, in s units */
  static const double play[2][4][2] = {
      {{0, 0}, {0.5, 0.25}, {0.5, 0.75}, {0, 1}},
      {{0.5, 0.25}, {1, 0.5}, {1, 0.5}, {0.5, 0.75}},
  };
  double bar = w / s;
  const double pause[2][4][2] = {
      {{0, 0}, {bar, 0}, {bar, 1}, {0, 1}},
      {{1 - bar, 0}, {1, 0}, {1, 1}, {1 - bar, 1}},
  };
  set_rgba(cr, col, 1.0);
  for (int h = 0; h < 2; ++h) {
    for (int k = 0; k < 4; ++k) {
      double u = play[h][k][0] + (pause[h][k][0] - play[h][k][0]) * p;
      double v = play[h][k][1] + (pause[h][k][1] - play[h][k][1]) * p;
      if (k == 0)
        cairo_move_to(cr, x + u * s, y + v * s);
      else
        cairo_line_to(cr, x + u * s, y + v * s);
    }
    cairo_close_path(cr);
  }
  cairo_fill(cr);
}

static void draw_cover(cairo_t *cr, const struct media_slot *sl, double x,
                       double y) {
  double side = C->art_size;
  if (sl->art) {
    cairo_surface_t *s = scaled_art(cr, sl, side);
    if (s) {
      cairo_save(cr);
      cairo_translate(cr, x, y);
      double k = side / cairo_image_surface_get_width(s);
      cairo_scale(cr, k, k);
      cairo_set_source_surface(cr, s, 0, 0);
      cairo_paint(cr);
      cairo_restore(cr);
      return;
    }
  }
  set_rgba(cr, C->track, 1.0);
  cairo_rectangle(cr, x, y, side, side);
  cairo_fill(cr);
  double size = side * 0.4;
  double w = advance(cr, ICON_MUSIC, size);
  text_at(cr, x + (side - w) / 2, y + (side + cap_height(cr, size)) / 2,
          ICON_MUSIC, size, false, 0, C->dim);
}

/* The bar: the played part from the left, or without a length a segment a
 * quarter of the bar wide easing from end to end every chase_ms. */
static void draw_bar(cairo_t *cr, const struct media_track *t, double p,
                     int64_t now, double x, double y, double w) {
  set_rgba(cr, C->track, 1.0);
  cairo_rectangle(cr, x, y, w, C->bar_h);
  cairo_fill(cr);
  double from, to;
  if (t->len_us > 0) {
    from = 0;
    to = w * (double)media_position(t, now) / t->len_us;
  } else {
    if (p >= 1)
      return;
    double seg = w / 4;
    double ph = C->chase_ms > 0 ? fmod((double)now, 2.0 * C->chase_ms) /
                                      C->chase_ms
                                : 0;
    double u = ph < 1 ? ph : 2 - ph;          /* there and back */
    u = (1 - cos(u * M_PI)) / 2;              /* eased at the ends */
    from = (w - seg) * u;
    to = from + seg;
  }
  if (to <= from)
    return;
  /* The gradient fades to muted as it pauses; without a length the
   * segment fades out instead. */
  bool chase = t->len_us <= 0;
  double q = chase ? 0 : p;
  cairo_pattern_t *g = cairo_pattern_create_linear(x, 0, x + w, 0);
  add_stop(g, 0.0, col_mix(C->bar_dark, C->muted, q));
  add_stop(g, 1.0, col_mix(C->bar_light, C->muted, q));
  cairo_set_source(cr, g);
  cairo_pattern_destroy(g);
  cairo_rectangle(cr, x + from, y, to - from, C->bar_h);
  if (chase) {
    cairo_save(cr);
    cairo_clip(cr);
    cairo_paint_with_alpha(cr, 1 - p);
    cairo_restore(cr);
  } else {
    cairo_fill(cr);
  }
}

/* One track's content, panel-relative. */
static void draw_slot(cairo_t *cr, const struct media_slot *sl, int64_t now) {
  const struct media_track *t = sl->t;
  double p = sl->paused < 0 ? 0 : sl->paused > 1 ? 1 : sl->paused;
  p = p < 0.5 ? 4 * p * p * p : 1 - pow(-2 * p + 2, 3) / 2; /* in-out */
  double x = C->pad;
  if (C->art) {
    draw_cover(cr, sl, C->pad, (C->height - C->art_size) / 2.0);
    x += C->art_size + C->pad;
  }
  double right = C->width - C->pad, w = right - x;
  if (w <= 0)
    return;

  /* player, with the status icon on the right */
  double y = C->pad + cap_height(cr, C->font_player);
  double iw = C->font_row * 0.75;
  draw_status(cr, right - iw, y - iw, iw, p, col_mix(C->text, C->muted, p));
  if (t->player[0])
    text_at(cr, x, y, t->player, C->font_player, false, w - iw - 8, C->dim);

  y += C->gap_title + cap_height(cr, C->font_title);
  text_at(cr, x, y, t->title, C->font_title, true, w, C->text);

  char line[600];
  if (t->artist[0] && t->album[0])
    snprintf(line, sizeof line, "%s — %s", t->artist, t->album);
  else
    snprintf(line, sizeof line, "%s", t->artist[0] ? t->artist : t->album);
  y += C->gap_artist + cap_height(cr, C->font_row);
  if (line[0])
    text_at(cr, x, y, line, C->font_row, false, w, C->dim);

  /* bar and times, from the bottom */
  double tb = C->height - C->pad;
  double bar_y = tb - cap_height(cr, C->font_time) - C->gap_time - C->bar_h;
  draw_bar(cr, t, p, now, x, bar_y, w);
  if (t->len_us > 0) {
    char a[32], b[32];
    media_format_time(a, sizeof a, media_position(t, now));
    media_format_time(b, sizeof b, t->len_us);
    uint32_t col = col_mix(C->dim, C->muted, p);
    text_at(cr, x, tb, a, C->font_time, false, 0, col);
    text_at(cr, right - advance(cr, b, C->font_time), tb, b, C->font_time,
            false, 0, col);
  }
}

static void paint_slot(cairo_t *cr, const struct media_slot *sl, int64_t now,
                       double alpha) {
  if (!sl->t || !sl->t->has || alpha <= 0)
    return;
  if (alpha >= 1) {
    draw_slot(cr, sl, now);
    return;
  }
  cairo_push_group(cr);
  draw_slot(cr, sl, now);
  cairo_pop_group_to_source(cr);
  cairo_paint_with_alpha(cr, alpha);
}

void media_render(cairo_t *cr, const struct media_config *cfg,
                  const struct media_frame *f) {
  C = cfg;
  if (f->shown <= 0)
    return;
  cairo_save(cr);
  /* Drop down: at shown 0 the panel's bottom is at the surface's top. */
  double top = C->top_gap -
               (double)(long)((1.0 - f->shown) * media_surface_height(C));
  cairo_translate(cr, 0, top);

  set_rgba(cr, C->background, 1.0);
  panel_path(cr, 0, 0, C->width, C->height, C->corner);
  cairo_fill(cr);
  set_rgba(cr, C->border, 1.0);
  cairo_set_line_width(cr, 1);
  panel_path(cr, 0.5, 0.5, C->width - 1, C->height - 1, C->corner);
  cairo_stroke(cr);

  panel_path(cr, 1, 1, C->width - 2, C->height - 2, C->corner);
  cairo_clip(cr);
  double fade = f->fade < 0 ? 0 : f->fade > 1 ? 1 : f->fade;
  if (f->prev.t && fade < 1)
    paint_slot(cr, &f->prev, f->now, 1 - fade);
  paint_slot(cr, &f->cur, f->now, f->prev.t ? fade : 1);

  cairo_restore(cr);
}
