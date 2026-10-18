#include "layout.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "model.h"
#include "util.h"

static bool in_box(struct box b, double x, double y) {
  return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

static struct box map_rect(struct box ws, double s, struct rect r) {
  return (struct box){ws.x + r.x * s, ws.y + r.y * s, r.w * s, r.h * s};
}

/* b made smaller by d on every side, unless that leaves too little. */
static struct box shrink(struct box b, double d) {
  if (d <= 0 || b.w <= 4 * d || b.h <= 4 * d)
    return b;
  return (struct box){b.x + d, b.y + d, b.w - 2 * d, b.h - 2 * d};
}

static struct box clip_to(struct box b, struct box c) {
  double x0 = fmax(b.x, c.x), y0 = fmax(b.y, c.y);
  double x1 = fmin(b.x + b.w, c.x + c.w), y1 = fmin(b.y + b.h, c.y + c.h);
  return (struct box){x0, y0, fmax(0, x1 - x0), fmax(0, y1 - y0)};
}

static void layout_output(struct output *out) {
  const struct overview_config *cfg = &out->ov->cfg;
  const double pad = cfg->layout_pad, gap = cfg->layout_gap;
  const double label_h = cfg->label_height;
  const double caption_h = cfg->caption_height;
  int n = out->nworkspaces;
  if (n == 0 || out->width <= 0 || out->height <= 0)
    return;

  struct rect wr = out->workspaces[0]->rect;
  double aspect = (wr.w > 0 && wr.h > 0) ? (double)wr.w / wr.h
                                         : (double)out->width / out->height;
  double area_w = out->width - 2 * pad;
  double area_h = out->height - 2 * pad - caption_h;

  /* pick the column count that gives the biggest tiles */
  int cols = 1;
  double tile_w = 0;
  for (int c = 1; c <= n; ++c) {
    int rows = (n + c - 1) / c;
    double w = (area_w - (c - 1) * gap) / c;
    double h = (area_h - rows * label_h - (rows - 1) * gap) / rows;
    double tw = fmin(w, h * aspect);
    if (tw > tile_w) {
      tile_w = tw;
      cols = c;
    }
  }
  tile_w = fmin(tile_w, out->width * 0.45);
  if (tile_w < 1)
    tile_w = 1;
  double tile_h = tile_w / aspect;
  int rows = (n + cols - 1) / cols;

  double grid_h = rows * (tile_h + label_h) + (rows - 1) * gap;
  double y0 = pad + (area_h - grid_h) / 2;

  for (int i = 0; i < n; ++i) {
    struct workspace *ws = out->workspaces[i];
    int row = i / cols, col = i % cols;
    int in_row = (row == rows - 1) ? n - row * cols : cols;
    double row_w = in_row * tile_w + (in_row - 1) * gap;
    double x0 = (out->width - row_w) / 2;

    double x = x0 + col * (tile_w + gap);
    double y = y0 + row * (tile_h + label_h + gap);
    ws->label = (struct box){x, y, tile_w, label_h};
    ws->box = (struct box){x, y + label_h, tile_w, tile_h};

    /* windows go inside the tile's inset, centred if its shape differs */
    double in = fmin(cfg->tile_inset, fmin(tile_w, tile_h) / 4);
    struct box inner = {ws->box.x + in, ws->box.y + in, tile_w - 2 * in,
                        tile_h - 2 * in};
    double s = 1;
    if (ws->rect.w > 0 && ws->rect.h > 0) {
      s = fmin(inner.w / ws->rect.w, inner.h / ws->rect.h);
      inner.x += (inner.w - s * ws->rect.w) / 2;
      inner.y += (inner.h - s * ws->rect.h) / 2;
    }
    ws->inner = inner;
    ws->inner.w = s * fmax(ws->rect.w, 0);
    ws->inner.h = s * fmax(ws->rect.h, 0);
    if (ws->inner.w <= 0 || ws->inner.h <= 0)
      ws->inner = ws->box;
    for (int j = 0; j < ws->nwindows; ++j) {
      struct window *w = ws->windows[j];
      w->box = shrink(map_rect(inner, s, w->rect), cfg->window_gap / 2);
      w->cbox = clip_to(map_rect(inner, s, w->content), w->box);
    }
  }
}

void ov_layout_all(struct ov *ov) {
  struct model *m = &ov->model;
  int nout = wl_list_length(&ov->outputs);

  struct output *out;
  wl_list_for_each(out, &ov->outputs, link) {
    free(out->workspaces);
    out->workspaces = NULL;
    out->nworkspaces = 0;
    out->gx = out->gy = 0;
    for (int i = 0; i < m->noutputs; ++i) {
      if (out->name && strcmp(out->name, m->outputs[i].name) == 0) {
        out->gx = m->outputs[i].rect.x;
        out->gy = m->outputs[i].rect.y;
      }
    }
  }

  for (int i = 0; i < m->nworkspaces; ++i) {
    struct workspace *ws = m->workspaces[i];
    ws->output = NULL;
    wl_list_for_each(out, &ov->outputs, link) {
      /* without wl_output names, a single output shows everything */
      if ((out->name && strcmp(out->name, ws->output_name) == 0) ||
          (!out->name && nout == 1)) {
        ws->output = out;
        break;
      }
    }
    if (!ws->output)
      continue;
    out = ws->output;
    out->workspaces =
        xrealloc(out->workspaces, (out->nworkspaces + 1) * sizeof(ws));
    out->workspaces[out->nworkspaces++] = ws;
  }

  wl_list_for_each(out, &ov->outputs, link) layout_output(out);
}

struct target ov_layout_hit(struct output *out, double x, double y) {
  struct target t = {0};
  if (!out)
    return t;
  for (int i = 0; i < out->nworkspaces; ++i) {
    struct workspace *ws = out->workspaces[i];
    if (!in_box(ws->box, x, y) && !in_box(ws->label, x, y))
      continue;
    t.ws = ws;
    for (int j = ws->nwindows - 1; j >= 0; --j) {
      if (in_box(ws->windows[j]->box, x, y)) {
        t.win = ws->windows[j];
        break;
      }
    }
    break;
  }
  return t;
}

bool ov_target_eq(struct target a, struct target b) {
  return a.win == b.win && a.ws == b.ws;
}

/* ---- reordering ------------------------------------------------------ */

bool ov_layout_reorderable(const struct workspace *ws) {
  return ws && !ws->placeholder && ws->num >= 0 && ws->output;
}

int ov_layout_reorder_list(struct workspace *ws, struct workspace ***list) {
  int n = 0;
  *list = NULL;
  struct output *out = ws->output;
  for (int i = 0; out && i < out->nworkspaces; ++i) {
    if (!ov_layout_reorderable(out->workspaces[i]))
      continue;
    *list = xrealloc(*list, (n + 1) * sizeof(**list));
    (*list)[n++] = out->workspaces[i];
  }
  return n;
}

int ov_layout_drop_slot(struct workspace *ws, struct workspace *target) {
  if (!ov_layout_reorderable(ws) || !target || target == ws ||
      target->output != ws->output)
    return -1;
  struct workspace **list;
  int n = ov_layout_reorder_list(ws, &list);
  int from = -1, to = -1;
  for (int i = 0; i < n; ++i) {
    if (list[i] == ws)
      from = i;
    if (list[i] == target)
      to = i;
  }
  if (target->placeholder)
    to = n - 1; /* the "new workspace" slot means the end */
  free(list);
  return to == from ? -1 : to;
}

/* ---- navigation ------------------------------------------------------ */

struct item {
  struct target t;
  double cx, cy; /* center in global logical coordinates */
};

static int cmp_window_reading(const void *a, const void *b) {
  const struct window *wa = *(struct window *const *)a;
  const struct window *wb = *(struct window *const *)b;
  if (wa->floating != wb->floating)
    return wa->floating - wb->floating;
  if (fabs(wa->box.y - wb->box.y) > 1)
    return wa->box.y < wb->box.y ? -1 : 1;
  if (fabs(wa->box.x - wb->box.x) > 1)
    return wa->box.x < wb->box.x ? -1 : 1;
  return 0;
}

static void push(struct item **items, int *n, struct target t, struct output *o,
                 struct box b) {
  *items = xrealloc(*items, (*n + 1) * sizeof(**items));
  (*items)[(*n)++] = (struct item){t, o->gx + b.x + b.w / 2, o->gy + b.y + b.h / 2};
}

/* Every selectable target: windows, plus workspaces that have none. */
static struct item *collect(struct ov *ov, int *out_n) {
  struct item *items = NULL;
  int n = 0;
  struct output *out;
  wl_list_for_each_reverse(out, &ov->outputs, link) {
    for (int i = 0; i < out->nworkspaces; ++i) {
      struct workspace *ws = out->workspaces[i];
      if (ws->nwindows == 0) {
        push(&items, &n, (struct target){NULL, ws}, out, ws->box);
        continue;
      }
      struct window **sorted = xcalloc(ws->nwindows, sizeof(*sorted));
      memcpy(sorted, ws->windows, ws->nwindows * sizeof(*sorted));
      qsort(sorted, ws->nwindows, sizeof(*sorted), cmp_window_reading);
      for (int j = 0; j < ws->nwindows; ++j)
        push(&items, &n, (struct target){sorted[j], ws}, out, sorted[j]->box);
      free(sorted);
    }
  }
  *out_n = n;
  return items;
}

static int find_item(struct item *items, int n, struct target t) {
  for (int i = 0; i < n; ++i)
    if (ov_target_eq(items[i].t, t))
      return i;
  return -1;
}

struct target ov_layout_initial(struct ov *ov) {
  struct model *m = &ov->model;
  struct window *w = ov_model_find_window(m, m->focused_con);
  if (w && w->ws->output)
    return (struct target){w, w->ws};
  for (int i = 0; i < m->nworkspaces; ++i) {
    struct workspace *ws = m->workspaces[i];
    if (ws->focused && ws->output)
      return (struct target){NULL, ws};
  }
  int n;
  struct item *items = collect(ov, &n);
  struct target t = n ? items[0].t : (struct target){0};
  free(items);
  return t;
}

struct target ov_layout_navigate(struct ov *ov, struct target cur, int dx,
                              int dy) {
  int n;
  struct item *items = collect(ov, &n);
  int ci = find_item(items, n, cur);
  struct target best = cur;
  if (ci < 0) {
    best = n ? items[0].t : (struct target){0};
    free(items);
    return best;
  }

  double best_score = INFINITY;
  struct item c = items[ci];
  for (int i = 0; i < n; ++i) {
    if (i == ci)
      continue;
    double ddx = items[i].cx - c.cx, ddy = items[i].cy - c.cy;
    double along = dx ? ddx * dx : ddy * dy;
    double ortho = dx ? fabs(ddy) : fabs(ddx);
    if (along < 1)
      continue;
    double score = along + 2 * ortho;
    if (score < best_score) {
      best_score = score;
      best = items[i].t;
    }
  }
  free(items);
  return best;
}

struct target ov_layout_step(struct ov *ov, struct target cur, int delta) {
  int n;
  struct item *items = collect(ov, &n);
  struct target t = cur;
  if (n > 0) {
    int ci = find_item(items, n, cur);
    int i = ci < 0 ? 0 : ((ci + delta) % n + n) % n;
    t = items[i].t;
  }
  free(items);
  return t;
}

struct workspace *ov_layout_step_workspace(struct ov *ov, struct workspace *ws,
                                        int delta) {
  struct workspace **all = NULL;
  int n = 0, ci = -1;
  struct output *out;
  wl_list_for_each_reverse(out, &ov->outputs, link) {
    for (int i = 0; i < out->nworkspaces; ++i) {
      all = xrealloc(all, (n + 1) * sizeof(*all));
      if (out->workspaces[i] == ws)
        ci = n;
      all[n++] = out->workspaces[i];
    }
  }
  struct workspace *res = NULL;
  if (ci >= 0 && ci + delta >= 0 && ci + delta < n)
    res = all[ci + delta];
  free(all);
  return res;
}
