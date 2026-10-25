/* overview: pure parts (workspace reorder planning, layout, hit testing,
 * navigation, drop slots) without Wayland or sway. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "overview/layout.h"
#include "overview/model.h"
#include "util.h"

static void test_renumber(void) {
  const char *names[] = {"2:web", "1", "4", "mail"};
  int nums[] = {1, 2, 4, 5};
  char *out[4];
  char *cmd = ov_renumber_command(names, nums, 4, out);
  assert(strcmp(out[0], "1:web") == 0);
  assert(strcmp(out[1], "2") == 0);
  assert(strcmp(out[2], "4") == 0); /* unchanged: no rename */
  assert(strcmp(out[3], "5mail") == 0);
  const char *want =
      "rename workspace \"2:web\" to \"swayverview-reorder-0\"; "
      "rename workspace \"1\" to \"swayverview-reorder-1\"; "
      "rename workspace \"mail\" to \"swayverview-reorder-3\"; "
      "rename workspace \"swayverview-reorder-0\" to \"1:web\"; "
      "rename workspace \"swayverview-reorder-1\" to \"2\"; "
      "rename workspace \"swayverview-reorder-3\" to \"5mail\"";
  if (strcmp(cmd, want) != 0) {
    fprintf(stderr, "got  %s\nwant %s\n", cmd, want);
    abort();
  }
  free(cmd);
  for (int i = 0; i < 4; ++i)
    free(out[i]);

  /* nothing to do */
  const char *same[] = {"1", "2"};
  int same_nums[] = {1, 2};
  assert(ov_renumber_command(same, same_nums, 2, out) == NULL);
  free(out[0]), free(out[1]);

  /* quoting */
  const char *q[] = {"1:a\"b"};
  int qn[] = {3};
  cmd = ov_renumber_command(q, qn, 1, out);
  assert(strstr(cmd, "\"1:a\\\"b\"") && strcmp(out[0], "3:a\"b") == 0);
  free(cmd), free(out[0]);
}

static void test_reorder_plan(void) {
  char a, b, c, d;
  void *list[] = {&a, &b, &c, &d};
  int nums[] = {1, 2, 3, 7};
  int sorted[4];
  /* move the first to the third slot: b c a d, numbers stay in place */
  assert(ov_reorder_plan(list, nums, 4, 0, 2, sorted));
  assert(list[0] == &b && list[1] == &c && list[2] == &a && list[3] == &d);
  assert(sorted[0] == 1 && sorted[1] == 2 && sorted[2] == 3 && sorted[3] == 7);
  /* back again */
  assert(ov_reorder_plan(list, nums, 4, 2, 0, sorted));
  assert(list[0] == &a && list[1] == &b && list[2] == &c);
  /* unsorted numbers get sorted */
  int un[] = {5, 2, 9};
  assert(ov_reorder_plan(list, un, 3, 1, 2, sorted));
  assert(sorted[0] == 2 && sorted[1] == 5 && sorted[2] == 9);
  /* no-ops and out of range */
  assert(!ov_reorder_plan(list, nums, 4, 1, 1, sorted));
  assert(!ov_reorder_plan(list, nums, 4, 1, 4, sorted));
  assert(!ov_reorder_plan(list, nums, 4, -1, 0, sorted));
}

/* ---- layout ---- */

static struct ov ov;
static struct output out;

static struct workspace *add_ws(const char *name, int num, bool placeholder) {
  struct model *m = &ov.model;
  struct workspace *ws = xcalloc(1, sizeof(*ws));
  ws->id = placeholder ? -1 : 100 + m->nworkspaces;
  ws->name = xstrdup(name);
  ws->num = num;
  ws->placeholder = placeholder;
  ws->output_name = xstrdup("HEADLESS-1");
  ws->rect = (struct rect){0, 0, 1536, 864};
  m->workspaces =
      xrealloc(m->workspaces, (m->nworkspaces + 1) * sizeof(*m->workspaces));
  m->workspaces[m->nworkspaces++] = ws;
  return ws;
}

static struct window *add_win(struct workspace *ws, long id, struct rect r) {
  struct model *m = &ov.model;
  struct window *w = xcalloc(1, sizeof(*w));
  w->con_id = id;
  w->rect = w->content = r;
  w->visible = true;
  w->ws = ws;
  ws->windows =
      xrealloc(ws->windows, (ws->nwindows + 1) * sizeof(*ws->windows));
  ws->windows[ws->nwindows++] = w;
  m->windows = xrealloc(m->windows, (m->nwindows + 1) * sizeof(*m->windows));
  m->windows[m->nwindows++] = w;
  return w;
}

static void center(struct box b, double *x, double *y) {
  *x = b.x + b.w / 2;
  *y = b.y + b.h / 2;
}

static void test_layout(void) {
  ov.cfg = (struct overview_config){
      .layout_pad = 48, .layout_gap = 36, .label_height = 30,
      .caption_height = 56};
  wl_list_init(&ov.outputs);
  out.ov = &ov;
  out.name = "HEADLESS-1";
  out.width = 1536;
  out.height = 864;
  wl_list_insert(&ov.outputs, &out.link);
  ov.model.focused_con = -1;
  ov.model.outputs = xcalloc(1, sizeof(*ov.model.outputs));
  ov.model.outputs[0] = (struct sway_output_info){"HEADLESS-1",
                                                   {0, 0, 1536, 864}};
  ov.model.noutputs = 1;

  struct workspace *w1 = add_ws("1", 1, false);
  struct workspace *w2 = add_ws("2:web", 2, false);
  struct workspace *mail = add_ws("mail", -1, false);
  struct workspace *slot = add_ws("3", 3, true);
  struct window *left = add_win(w1, 11, (struct rect){0, 0, 768, 864});
  struct window *right = add_win(w1, 12, (struct rect){768, 0, 768, 864});
  struct window *web = add_win(w2, 21, (struct rect){0, 0, 1536, 864});
  right->focused = true;
  ov.model.focused_con = 12;

  ov_layout_all(&ov);
  assert(out.nworkspaces == 4);
  for (int i = 0; i < 4; ++i) {
    struct workspace *ws = out.workspaces[i];
    assert(ws->output == &out);
    assert(ws->box.w > 0 && ws->box.h > 0);
    /* inside the output, below its label, aspect kept */
    assert(ws->box.x >= 0 && ws->box.x + ws->box.w <= out.width);
    assert(ws->label.y + ws->label.h == ws->box.y);
    assert(ws->box.w <= out.width * 0.45 + 1e-9);
    double aspect = ws->box.w / ws->box.h;
    assert(aspect > 1536.0 / 864 - 0.01 && aspect < 1536.0 / 864 + 0.01);
  }
  double x, y;
  /* windows scale with their tile */
  assert(left->box.x == w1->box.x && left->box.w * 2 - w1->box.w < 1e-6);
  assert(w1->inner.x == w1->box.x && w1->inner.w == w1->box.w);

  /* inset and gaps: windows inside the inset, apart, still hit */
  ov.cfg.tile_inset = 6;
  ov.cfg.window_gap = 4;
  ov_layout_all(&ov);
  assert(w1->inner.x >= w1->box.x + 6 - 1e-6);
  assert(w1->inner.y >= w1->box.y + 6 - 1e-6);
  assert(w1->inner.x + w1->inner.w <= w1->box.x + w1->box.w - 6 + 1e-6);
  assert(w1->inner.y + w1->inner.h <= w1->box.y + w1->box.h - 6 + 1e-6);
  assert(fabs(w1->inner.w / w1->inner.h - 1536.0 / 864) < 1e-6);
  assert(fabs(left->box.x - (w1->inner.x + 2)) < 1e-6);
  assert(right->box.x - (left->box.x + left->box.w) > 3.9);
  assert(left->cbox.x >= left->box.x && left->cbox.w <= left->box.w);
  double gx = (left->box.x + left->box.w + right->box.x) / 2, gy;
  center(right->box, &x, &gy);
  assert(ov_layout_hit(&out, gx, gy).win == NULL);
  assert(ov_layout_hit(&out, x, gy).win == right);
  ov.cfg.tile_inset = ov.cfg.window_gap = 0;
  ov_layout_all(&ov);

  center(right->box, &x, &y);
  struct target t = ov_layout_hit(&out, x, y);
  assert(t.win == right && t.ws == w1);
  center(w1->label, &x, &y);
  t = ov_layout_hit(&out, x, y);
  assert(t.ws == w1 && !t.win);
  center(slot->box, &x, &y);
  t = ov_layout_hit(&out, x, y);
  assert(t.ws == slot && !t.win);
  t = ov_layout_hit(&out, 2, 2); /* background */
  assert(!t.ws && !t.win);
  t = ov_layout_hit(NULL, 2, 2);
  assert(!t.ws && !t.win);

  /* initial selection: the focused window */
  t = ov_layout_initial(&ov);
  assert(t.win == right);

  /* navigation: left of right is left; Tab order wraps */
  t = ov_layout_navigate(&ov, (struct target){right, w1}, -1, 0);
  assert(t.win == left);
  t = ov_layout_step(&ov, (struct target){left, w1}, 1);
  assert(t.win == right);
  t = ov_layout_step(&ov, (struct target){left, w1}, -1);
  assert(t.ws == slot && !t.win); /* wraps to the last target */
  t = ov_layout_step(&ov, (struct target){right, w1}, 1);
  assert(t.win == web);
  assert(ov_layout_step_workspace(&ov, w1, 1) == w2);
  assert(ov_layout_step_workspace(&ov, w1, -1) == NULL);

  /* reordering: numbered workspaces only, the slot means "the end" */
  assert(ov_layout_reorderable(w1) && ov_layout_reorderable(w2));
  assert(!ov_layout_reorderable(mail) && !ov_layout_reorderable(slot));
  struct workspace **list;
  int n = ov_layout_reorder_list(w1, &list);
  assert(n == 2 && list[0] == w1 && list[1] == w2);
  free(list);
  assert(ov_layout_drop_slot(w1, w2) == 1);
  assert(ov_layout_drop_slot(w2, w1) == 0);
  assert(ov_layout_drop_slot(w1, slot) == 1);
  assert(ov_layout_drop_slot(w2, slot) == -1);
  assert(ov_layout_drop_slot(w1, w1) == -1);
  assert(ov_layout_drop_slot(mail, w1) == -1);

  ov.model.outputs[0].name = NULL; /* static string */
  ov_model_free(&ov.model);
  free(out.workspaces);
}

/* labwc: windows spread in a grid, each at its size or shrunk to its cell,
 * none overlapping, all inside the workspace. */
static int sizes[][2] = {{800, 600}, {3000, 400}, {400, 900}};

static bool fake_size(struct window *w, int *width, int *height) {
  if (w->con_id > 3)
    return false; /* no capture: 60% of the workspace */
  *width = sizes[w->con_id - 1][0];
  *height = sizes[w->con_id - 1][1];
  return true;
}

static void test_spread(void) {
  struct workspace ws = {.rect = {0, 0, 1920, 1080}};
  struct window wins[4];
  struct window *list[4];
  for (int i = 0; i < 4; ++i) {
    wins[i] = (struct window){.con_id = i + 1, .ws = &ws};
    list[i] = &wins[i];
  }
  ws.windows = list;

  ws.nwindows = 1;
  ov_model_spread(&ws, 40, fake_size);
  /* one window that fits: its own size, centred */
  assert(wins[0].rect.w == 800 && wins[0].rect.h == 600);
  assert(wins[0].rect.x == 560 && wins[0].rect.y == 240);
  assert(memcmp(&wins[0].rect, &wins[0].content, sizeof(struct rect)) == 0);

  ws.nwindows = 4;
  ov_model_spread(&ws, 40, fake_size);
  for (int i = 0; i < 4; ++i) {
    struct rect r = wins[i].rect;
    assert(r.x >= 40 && r.y >= 40 && r.x + r.w <= 1880 && r.y + r.h <= 1040);
    assert(r.w > 0 && r.h > 0);
    for (int j = 0; j < i; ++j) {
      struct rect q = wins[j].rect;
      assert(r.x >= q.x + q.w || q.x >= r.x + r.w || r.y >= q.y + q.h ||
             q.y >= r.y + r.h);
    }
  }
  /* the wide one keeps its shape */
  assert(fabs((double)wins[1].rect.w / wins[1].rect.h - 7.5) < 0.2);
  /* no size: 60% of the workspace's shape */
  assert(fabs((double)wins[3].rect.w / wins[3].rect.h - 1920.0 / 1080) < 0.05);
}

int main(void) {
  test_renumber();
  test_spread();
  test_reorder_plan();
  test_layout();
  printf("overview tests passed\n");
  return 0;
}
