#include "zoom.h"

#include <math.h>

#include "layout.h"
#include "util.h"

/* Where the preview of w should end up, or false if it isn't worth it. */
static bool target_box(struct window *w, struct box *out_box) {
  struct output *out = w->ws ? w->ws->output : NULL;
  if (!out || w->cbox.w <= 0 || w->cbox.h <= 0)
    return false;
  const struct overview_config *cfg = &out->ov->cfg;
  struct box from = w->cbox;
  double gain = fmin(out->width * cfg->zoom_max_width / from.w,
                     out->height * cfg->zoom_max_height / from.h);
  /* never beyond the window's real size */
  if (w->content.w > 0)
    gain = fmin(gain, w->content.w / from.w);
  if (gain < cfg->zoom_min_gain)
    return false;

  double bw = from.w * gain, bh = from.h * gain;
  double x = from.x + from.w / 2 - bw / 2;
  double y = from.y + from.h / 2 - bh / 2;
  /* keep it on screen */
  x = fmax(cfg->zoom_margin, fmin(x, out->width - cfg->zoom_margin - bw));
  y = fmax(cfg->zoom_margin, fmin(y, out->height - cfg->zoom_margin - bh));
  *out_box = (struct box){x, y, bw, bh};
  return true;
}

static double progress(struct ov *ov, int64_t now) {
  if (ov->cfg.zoom_anim_ms <= 0)
    return 1;
  double t = (double)(now - ov->zoom.start) / ov->cfg.zoom_anim_ms;
  if (t >= 1)
    return 1;
  if (t <= 0)
    return 0;
  return 1 - pow(1 - t, 3); /* ease-out cubic */
}

static struct box current_box(struct ov *ov, int64_t now) {
  double p = progress(ov, now);
  struct box a = ov->zoom.from, b = ov->zoom.to;
  return (struct box){a.x + (b.x - a.x) * p, a.y + (b.y - a.y) * p,
                      a.w + (b.w - a.w) * p, a.h + (b.h - a.h) * p};
}

/* Animate from wherever the preview is now towards its grown box. */
static void grow(struct ov *ov, struct window *w, struct box big,
                 int64_t now) {
  bool same = ov->zoom.win == w;
  ov->zoom.from = same ? current_box(ov, now) : w->cbox;
  ov->zoom.win = w;
  ov->zoom.closing = false;
  ov->zoom.start = now;
  ov->zoom.to = ov->zoom.big = big;
  ov_redraw(ov);
}

/* Animate from wherever the preview is now back into its tile. */
static void shrink(struct ov *ov, int64_t now) {
  ov->zoom.from = current_box(ov, now);
  ov->zoom.to = ov->zoom.win->cbox;
  ov->zoom.closing = true;
  ov->zoom.start = now;
  ov_redraw(ov);
}

void ov_zoom_hover_changed(struct ov *ov) {
  int64_t now = now_ms();
  struct window *w = ov->zoom.win;
  if (w && !ov->zoom.closing && w != ov->hover.win) {
    shrink(ov, now);
  } else if (w && ov->zoom.closing && w == ov->hover.win) {
    grow(ov, w, ov->zoom.big, now); /* came back while shrinking */
  }
  ov->zoom.hover_since = now;
}

void ov_zoom_relayout(struct ov *ov) {
  struct window *w = ov->zoom.win;
  if (!w)
    return;
  struct box big;
  if (!target_box(w, &big)) {
    ov->zoom.win = NULL;
    return;
  }
  ov->zoom.big = big;
  if (ov->zoom.closing) {
    ov->zoom.to = w->cbox;
  } else {
    ov->zoom.from = w->cbox;
    ov->zoom.to = big;
  }
}

void ov_zoom_tick(struct ov *ov, int64_t now) {
  struct window *h = ov->hover.win;
  if (ov->drag.active) {
    if (ov->zoom.win) {
      ov->zoom.win = NULL;
      ov_redraw(ov);
    }
    return;
  }
  if (ov->zoom.win && ov->zoom.closing && progress(ov, now) >= 1) {
    ov->zoom.win = NULL; /* fully shrunk */
    ov_redraw(ov);
  }
  /* a new preview replaces one that is still shrinking */
  if (h && (!ov->zoom.win || (ov->zoom.closing && ov->zoom.win != h)) &&
      now - ov->zoom.hover_since >= ov->cfg.zoom_delay_ms) {
    struct box big;
    if (target_box(h, &big))
      grow(ov, h, big, now);
  }
  if (ov->zoom.win && progress(ov, now) < 1 && ov->zoom.win->ws->output)
    ov_output_redraw(ov->zoom.win->ws->output);
}

int ov_zoom_timeout(struct ov *ov, int64_t now) {
  if (ov->drag.active)
    return -1;
  if (ov->zoom.win && progress(ov, now) < 1)
    return 8;
  if (ov->hover.win && (!ov->zoom.win || ov->zoom.win != ov->hover.win)) {
    int64_t left = ov->zoom.hover_since + ov->cfg.zoom_delay_ms - now;
    return left > 0 ? (int)left : 0;
  }
  return -1;
}

bool ov_zoom_box(struct ov *ov, struct output *out, int64_t now,
              struct box *box) {
  struct window *w = ov->zoom.win;
  if (!w || ov->drag.active || w->ws->output != out)
    return false;
  *box = current_box(ov, now);
  return true;
}

bool ov_zoom_contains(struct ov *ov, struct output *out, double x, double y) {
  struct window *w = ov->zoom.win;
  if (!w || ov->zoom.closing || ov->drag.active || w->ws->output != out)
    return false;
  struct box b = ov->zoom.big;
  return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}
