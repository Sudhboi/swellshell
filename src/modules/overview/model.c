#include "model.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "i3ipc.h"
#include "sway.h"
#include "util.h"

/* ---- model ---------------------------------------------------------- */

struct builder {
  struct model *m;
  struct workspace *ws;
  double spread_gap;
};

/* Maps tree coordinates to overview coordinates: p' = p * s + t.
 * Used to spread the children of tabbed/stacked containers side by side so
 * hidden tabs get their own slot in the overview. */
struct xform {
  double sx, sy, tx, ty;
};

static struct rect apply(struct xform f, int x, int y, int w, int h) {
  return (struct rect){(int)lround(x * f.sx + f.tx),
                       (int)lround(y * f.sy + f.ty), (int)lround(w * f.sx),
                       (int)lround(h * f.sy)};
}

static char *dup_str(const char *s, int size) {
  return s ? xstrndup(s, size) : NULL;
}

static bool is_view(const I3ipc_node *n) {
  return n->nodes_size == 0 && n->floating_nodes_size == 0 &&
         (n->pid_set || n->app_id || n->window_properties);
}

static void add_window(struct builder *b, const I3ipc_node *n, bool floating,
                       struct xform f) {
  struct workspace *ws = b->ws;
  struct window *w = xcalloc(1, sizeof(*w));
  w->con_id = (long)n->id;
  w->title = dup_str(n->name, n->name_size);
  w->app_id = dup_str(n->app_id, n->app_id_size);
  if (!w->app_id && n->window_properties && n->window_properties->window_class)
    w->app_id = dup_str(n->window_properties->window_class,
                        n->window_properties->window_class_size);
  w->ident = dup_str(n->foreign_toplevel_identifier,
                     n->foreign_toplevel_identifier_size);
  /* relative to the workspace */
  f.tx -= ws->rect.x;
  f.ty -= ws->rect.y;
  w->rect = apply(f, n->rect.x, n->rect.y, n->rect.width, n->rect.height);
  if (n->window_rect.width > 0 && n->window_rect.height > 0)
    w->content = apply(f, n->rect.x + n->window_rect.x,
                       n->rect.y + n->window_rect.y, n->window_rect.width,
                       n->window_rect.height);
  else
    w->content = w->rect;
  w->floating = floating;
  /* sway always reports "visible" for views; default to true otherwise */
  w->visible = n->visible_set ? n->visible : true;
  w->focused = n->focused;
  w->urgent = n->urgent;
  w->fullscreen = n->fullscreen_mode != 0;
  w->ws = ws;

  ws->windows =
      xrealloc(ws->windows, (ws->nwindows + 1) * sizeof(*ws->windows));
  ws->windows[ws->nwindows++] = w;
  struct model *m = b->m;
  m->windows = xrealloc(m->windows, (m->nwindows + 1) * sizeof(*m->windows));
  m->windows[m->nwindows++] = w;
  if (w->focused)
    m->focused_con = w->con_id;
}

/* Place n children of size cw x ch in a grid inside the container rect r,
 * choosing the column count that lets them be largest. Children keep their
 * aspect ratio; the last row is centered. Writes the transform for child i. */
static struct xform spread_cell(I3ipc_rect r, int n, int i, I3ipc_rect c,
                                double gap) {
  double cw = c.width, ch = c.height;
  int cols = 1;
  double best = 0;
  for (int k = 1; k <= n; ++k) {
    int rows = (n + k - 1) / k;
    double s = fmin((r.width - (k + 1) * gap) / k / cw,
                    (r.height - (rows + 1) * gap) / rows / ch);
    if (s > best) {
      best = s;
      cols = k;
    }
  }
  int rows = (n + cols - 1) / cols;
  double s = best > 0 ? best : 0.01;
  double cell_w = (r.width - gap) / cols;
  double cell_h = (r.height - gap) / rows;

  int row = i / cols, col = i % cols;
  int in_row = row == rows - 1 ? n - row * cols : cols;
  double row_x = r.x + (r.width - in_row * cell_w) / 2; /* center short rows */
  double x = row_x + col * cell_w + gap / 2 + (cell_w - gap - cw * s) / 2;
  double y =
      r.y + gap / 2 + row * cell_h + gap / 2 + (cell_h - gap - ch * s) / 2;
  return (struct xform){s, s, x - c.x * s, y - c.y * s};
}

static void walk(struct builder *b, const I3ipc_node *n, bool floating,
                 struct xform f) {
  if (is_view(n)) {
    add_window(b, n, floating, f);
    return;
  }
  bool spread = (n->layout_enum == I3IPC_NODE_LAYOUT_TABBED ||
                 n->layout_enum == I3IPC_NODE_LAYOUT_STACKED) &&
                n->nodes_size > 1 && n->type_enum != I3IPC_NODE_TYPE_WORKSPACE;
  for (int i = 0; i < n->nodes_size; ++i) {
    const I3ipc_node *c = &n->nodes[i];
    struct xform cf = f;
    if (spread && c->rect.width > 0 && c->rect.height > 0) {
      /* hidden tabs get their own cell, so every tab is visible */
      struct xform k =
          spread_cell(n->rect, n->nodes_size, i, c->rect, b->spread_gap);
      /* compose: outer f after inner k */
      cf = (struct xform){f.sx * k.sx, f.sy * k.sy, f.sx * k.tx + f.tx,
                          f.sy * k.ty + f.ty};
    }
    walk(b, c, floating, cf);
  }
  for (int i = 0; i < n->floating_nodes_size; ++i)
    walk(b, &n->floating_nodes[i], true, f);
}

/* Bottom-most first: hidden tabs, tiled, floating, fullscreen. */
static int draw_rank(const struct window *w) {
  if (w->fullscreen && w->visible)
    return 4;
  if (w->floating)
    return 3;
  return w->visible ? 2 : 1;
}

static void sort_draw_order(struct workspace *ws) {
  /* insertion sort, stable; workspaces have few windows */
  for (int i = 1; i < ws->nwindows; ++i) {
    struct window *w = ws->windows[i];
    int j = i - 1;
    while (j >= 0 && draw_rank(ws->windows[j]) > draw_rank(w)) {
      ws->windows[j + 1] = ws->windows[j];
      --j;
    }
    ws->windows[j + 1] = w;
  }
}

static struct workspace *add_workspace(struct model *m) {
  struct workspace *ws = xcalloc(1, sizeof(*ws));
  ws->id = -1;
  m->workspaces =
      xrealloc(m->workspaces, (m->nworkspaces + 1) * sizeof(*m->workspaces));
  m->workspaces[m->nworkspaces++] = ws;
  return ws;
}

static int parse_num(const char *name) {
  char *end;
  long v = strtol(name, &end, 10);
  return (end != name && v >= 0) ? (int)v : -1;
}

static bool num_used(struct model *m, int num) {
  for (int i = 0; i < m->nworkspaces; ++i)
    if (m->workspaces[i]->num == num)
      return true;
  return false;
}

bool ov_model_load(struct model *m, bool new_slot, double spread_gap) {
  memset(m, 0, sizeof(*m));
  m->focused_con = -1;

  I3ipc_reply_tree *tree = i3ipc_get_tree();
  I3ipc_reply_workspaces *wss = tree ? i3ipc_get_workspaces() : NULL;
  if (!tree || !wss) {
    i3ipc_error_print("swell[overview]: IPC query failed");
    i3ipc_error_reinitialize(false);
    free(tree);
    free(wss);
    return false;
  }

  struct builder b = {.m = m, .spread_gap = spread_gap};
  const I3ipc_node *root = &tree->root;
  for (int i = 0; i < root->nodes_size; ++i) {
    const I3ipc_node *out = &root->nodes[i];
    if (out->type_enum != I3IPC_NODE_TYPE_OUTPUT || !out->name ||
        strncmp(out->name, "__i3", 4) == 0)
      continue;

    m->outputs = xrealloc(m->outputs, (m->noutputs + 1) * sizeof(*m->outputs));
    struct sway_output_info *so = &m->outputs[m->noutputs++];
    so->name = dup_str(out->name, out->name_size);
    so->rect = (struct rect){out->rect.x, out->rect.y, out->rect.width,
                             out->rect.height};

    for (int j = 0; j < out->nodes_size; ++j) {
      const I3ipc_node *wn = &out->nodes[j];
      if (wn->type_enum != I3IPC_NODE_TYPE_WORKSPACE || !wn->name)
        continue;
      struct workspace *ws = add_workspace(m);
      ws->id = (long)wn->id;
      ws->name = dup_str(wn->name, wn->name_size);
      ws->num = wn->num_set ? wn->num : parse_num(ws->name);
      ws->output_name = xstrdup(so->name);
      ws->rect = (struct rect){wn->rect.x, wn->rect.y, wn->rect.width,
                               wn->rect.height};
      for (int k = 0; k < wss->workspaces_size; ++k) {
        I3ipc_reply_workspaces_el *el = &wss->workspaces[k];
        if (el->name && strcmp(el->name, ws->name) == 0) {
          ws->focused = el->focused;
          ws->visible = el->visible;
          ws->urgent = el->urgent;
        }
      }
      b.ws = ws;
      walk(&b, wn, false, (struct xform){1, 1, 0, 0});
      sort_draw_order(ws);
    }
  }

  if (new_slot) {
    int num = 1;
    while (num_used(m, num))
      ++num;
    for (int i = 0; i < m->noutputs; ++i) {
      struct workspace *ws = add_workspace(m);
      ws->placeholder = true;
      ws->num = num;
      char buf[16];
      snprintf(buf, sizeof(buf), "%d", num);
      ws->name = xstrdup(buf);
      ws->output_name = xstrdup(m->outputs[i].name);
      ws->rect = m->outputs[i].rect;
    }
  }

  free(tree);
  free(wss);
  return true;
}

void ov_model_free(struct model *m) {
  for (int i = 0; i < m->nwindows; ++i) {
    struct window *w = m->windows[i];
    free(w->title);
    free(w->app_id);
    free(w->ident);
    free(w);
  }
  for (int i = 0; i < m->nworkspaces; ++i) {
    struct workspace *ws = m->workspaces[i];
    free(ws->name);
    free(ws->output_name);
    free(ws->windows);
    free(ws);
  }
  for (int i = 0; i < m->noutputs; ++i)
    free(m->outputs[i].name);
  free(m->outputs);
  free(m->windows);
  free(m->workspaces);
  memset(m, 0, sizeof(*m));
  m->focused_con = -1;
}

struct window *ov_model_find_window(struct model *m, long con_id) {
  for (int i = 0; i < m->nwindows; ++i)
    if (m->windows[i]->con_id == con_id)
      return m->windows[i];
  return NULL;
}

struct workspace *ov_model_find_workspace(struct model *m, long id) {
  for (int i = 0; id >= 0 && i < m->nworkspaces; ++i)
    if (m->workspaces[i]->id == id)
      return m->workspaces[i];
  return NULL;
}

/* ---- commands ------------------------------------------------------- */

/* Quote a workspace name for use in a sway command. */
static void quote(char *dst, size_t size, const char *src) {
  size_t o = 0;
  if (o + 1 < size)
    dst[o++] = '"';
  for (; *src && o + 3 < size; ++src) {
    if (*src == '"' || *src == '\\')
      dst[o++] = '\\';
    dst[o++] = *src;
  }
  if (o + 1 < size)
    dst[o++] = '"';
  dst[o] = '\0';
}

static void workspace_ref(char *dst, size_t size, struct workspace *ws) {
  if (ws->placeholder)
    snprintf(dst, size, "number %d", ws->num);
  else
    quote(dst, size, ws->name);
}

void ov_model_focus_window(struct window *win) {
  sway_command("[con_id=%ld] focus", win->con_id);
}

void ov_model_focus_workspace(struct workspace *ws) {
  char ref[512], out[512];
  workspace_ref(ref, sizeof(ref), ws);
  if (ws->placeholder) {
    /* create the new workspace on the output it was shown on */
    quote(out, sizeof(out), ws->output_name);
    sway_command("focus output %s; workspace %s", out, ref);
  } else {
    sway_command("workspace %s", ref);
  }
}

void ov_model_move_window(struct window *win, struct workspace *ws) {
  char ref[512];
  workspace_ref(ref, sizeof(ref), ws);
  if (ws->placeholder) {
    /* sway creates a new workspace on the focused output */
    char out[512];
    quote(out, sizeof(out), ws->output_name);
    sway_command("focus output %s; [con_id=%ld] move container to workspace %s",
                 out, win->con_id, ref);
    return;
  }
  sway_command("[con_id=%ld] move container to workspace %s", win->con_id,
               ref);
}

void ov_model_kill_window(struct window *win) {
  sway_command("[con_id=%ld] kill", win->con_id);
}

/* Append "rename workspace FROM to TO" to the growing command *cmd. */
static void append_rename(char **cmd, size_t *len, const char *from,
                          const char *to) {
  char qf[512], qt[512];
  quote(qf, sizeof(qf), from);
  quote(qt, sizeof(qt), to);
  size_t add = strlen(qf) + strlen(qt) + 32;
  *cmd = xrealloc(*cmd, *len + add);
  *len += snprintf(*cmd + *len, add, "%srename workspace %s to %s",
                   *len ? "; " : "", qf, qt);
}

char *ov_renumber_command(const char *const *names, const int *nums, int n,
                          char **new_names) {
  for (int i = 0; i < n; ++i) {
    const char *suffix = names[i] + strspn(names[i], "0123456789");
    new_names[i] = xasprintf("%d%s", nums[i], suffix);
  }

  /* Rename through temporary names: sway refuses a rename onto a name that
   * is still taken. All in one command list, so sway applies it at once
   * (and the workspaces module's renumbering sees only the end result). */
  char *cmd = NULL;
  size_t len = 0;
  for (int i = 0; i < n; ++i) {
    if (strcmp(names[i], new_names[i]) == 0)
      continue;
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "swayverview-reorder-%d", i);
    append_rename(&cmd, &len, names[i], tmp);
  }
  for (int i = 0; i < n; ++i) {
    if (strcmp(names[i], new_names[i]) == 0)
      continue;
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "swayverview-reorder-%d", i);
    append_rename(&cmd, &len, tmp, new_names[i]);
  }
  return cmd;
}

bool ov_reorder_plan(void **list, const int *nums, int n, int from, int slot,
                     int *sorted) {
  if (from < 0 || from >= n || slot < 0 || slot >= n || slot == from)
    return false;
  /* the numbers stay in place, the workspaces move between them */
  for (int i = 0; i < n; ++i) {
    int j = i;
    for (; j > 0 && sorted[j - 1] > nums[i]; --j)
      sorted[j] = sorted[j - 1];
    sorted[j] = nums[i];
  }
  void *moved = list[from];
  if (slot > from)
    memmove(&list[from], &list[from + 1], (slot - from) * sizeof(*list));
  else
    memmove(&list[slot + 1], &list[slot], (from - slot) * sizeof(*list));
  list[slot] = moved;
  return true;
}

void ov_model_renumber_workspaces(struct workspace **order, const int *nums,
                                  int n) {
  const char **names = xcalloc(n, sizeof(*names));
  char **new_names = xcalloc(n, sizeof(*new_names));
  for (int i = 0; i < n; ++i)
    names[i] = order[i]->name;
  char *cmd = ov_renumber_command(names, nums, n, new_names);
  if (cmd)
    sway_command("%s", cmd);
  free(cmd);

  /* so the selection/drag survive the reload by their new names */
  for (int i = 0; i < n; ++i) {
    free(order[i]->name);
    order[i]->name = new_names[i];
    order[i]->num = nums[i];
  }
  free(new_names);
  free(names);
}
