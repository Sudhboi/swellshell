#include "labwc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bus.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-workspace-v1-client-protocol.h"
#include "util.h"
#include "wl.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

#define SETTLE_MS 10

struct ws {
  uint32_t key;
  char *name;
  bool active, urgent;
  bool was_urgent; /* as of the last settle */
  struct ext_workspace_handle_v1 *handle;
};

struct win {
  uint32_t key;
  char *title, *app_id;
  char *ident; /* its ext-foreign-toplevel identifier, once paired */
  bool activated, minimized;
  uint32_t ws; /* 0: not known yet */
  /* Belongs to the active workspace at the next settle: it opened, or it
   * gained focus. */
  bool claim;
  bool was_activated; /* as of the last settle */
  struct zwlr_foreign_toplevel_handle_v1 *handle;
};

/* An ext-foreign-toplevel-list entry. labwc makes one per window next to
 * the wlr one; nothing links the two, so they are paired by app_id and
 * title (the oldest window first when several match). */
struct ext {
  uint32_t key;
  char *ident, *title, *app_id;
  uint32_t win; /* 0: not paired yet */
};

static struct ws *wss; /* labwc's order */
static int nws;
static struct win *wins;
static int nwins;
static struct ext *exts;
static int nexts;
static uint64_t serial; /* bumped on every change (labwc_serial) */
static bool initial_done;

/* The list as of the last settle (keys, in order) and what was reported. */
static uint32_t *shown;
static int nshown;
static uint32_t focused_key;

static struct wm_workspace *out_cache;
static int out_count;
static bool out_stale = true;

enum { EMPTY_UNKNOWN = -1, EMPTY_NO = 0, EMPTY_YES = 1 };
static int focused_empty = EMPTY_UNKNOWN;

static struct loop *ev_loop;
static struct loop_timer *settle_timer;
static struct swell_wl *wl;
static struct ext_workspace_manager_v1 *ws_mgr;
static struct zwlr_foreign_toplevel_manager_v1 *tl_mgr;
static uint32_t next_key = 1;
static bool linked;    /* labwc_init succeeded */
static bool pool_full; /* the last target found no fresh workspace */

static bool str_eq(const char *a, const char *b) {
  return a == b || (a && b && strcmp(a, b) == 0);
}

/* ---- model ---- */

static struct ws *find_ws(uint32_t key) {
  for (int i = 0; i < nws; ++i)
    if (wss[i].key == key)
      return &wss[i];
  return NULL;
}

static struct win *find_win(uint32_t key) {
  for (int i = 0; i < nwins; ++i)
    if (wins[i].key == key)
      return &wins[i];
  return NULL;
}

static const struct ws *active_ws(void) {
  for (int i = 0; i < nws; ++i)
    if (wss[i].active)
      return &wss[i];
  return NULL;
}

/* Any window, minimized ones included: such a workspace isn't fresh. */
static bool ws_has_windows(uint32_t key) {
  for (int i = 0; i < nwins; ++i)
    if (wins[i].ws == key)
      return true;
  return false;
}

/* What the wallpaper sees: windows that are on screen. */
static bool ws_shows_windows(uint32_t key) {
  for (int i = 0; i < nwins; ++i)
    if (wins[i].ws == key && !wins[i].minimized)
      return true;
  return false;
}

static void changed(void) {
  ++serial;
  if (settle_timer && !loop_timer_armed(settle_timer))
    loop_timer_arm(settle_timer, SETTLE_MS, 0);
}

static void emit_ws(const char *event, int pos) {
  char *env = pos > 0 ? xasprintf("SWELL_WORKSPACE=%d", pos)
                      : xstrdup("SWELL_WORKSPACE=");
  bus_emit(event, env, NULL);
  free(env);
}

static void emit_win(const char *event, const struct win *w) {
  char *e1 = xasprintf("SWELL_CON_ID=%u", w->key);
  char *e2 = xasprintf("SWELL_APP_ID=%s", w->app_id ? w->app_id : "");
  char *e3 = xasprintf("SWELL_TITLE=%s", w->title ? w->title : "");
  bus_emit(event, e1, e2, e3, NULL);
  free(e1), free(e2), free(e3);
}

static void pair(void) {
  for (int i = 0; i < nexts; ++i) {
    struct ext *e = &exts[i];
    if (e->win || !e->ident)
      continue;
    struct win *best = NULL;
    for (int j = 0; j < nwins; ++j) {
      struct win *w = &wins[j];
      if (!w->ident && str_eq(w->app_id, e->app_id) &&
          str_eq(w->title, e->title) && (!best || w->key < best->key))
        best = w;
    }
    if (best) {
      best->ident = xstrdup(e->ident);
      e->win = best->key;
    }
  }
}

static void set_str(char **dst, const char *src) {
  if (str_eq(*dst, src))
    return;
  free(*dst);
  *dst = src ? xstrdup(src) : NULL;
}

void labwc_model_ext(uint32_t key, const char *ident, const char *title,
                     const char *app_id) {
  struct ext *e = NULL;
  for (int i = 0; i < nexts && !e; ++i)
    if (exts[i].key == key)
      e = &exts[i];
  if (!e) {
    exts = xrealloc(exts, (nexts + 1) * sizeof(*exts));
    e = &exts[nexts++];
    memset(e, 0, sizeof(*e));
    e->key = key;
  }
  set_str(&e->ident, ident);
  set_str(&e->title, title);
  set_str(&e->app_id, app_id);
  pair();
  changed();
}

void labwc_model_ext_closed(uint32_t key) {
  for (int i = 0; i < nexts; ++i) {
    struct ext *e = &exts[i];
    if (e->key != key)
      continue;
    struct win *w = e->win ? find_win(e->win) : NULL;
    if (w) {
      free(w->ident);
      w->ident = NULL;
    }
    free(e->ident);
    free(e->title);
    free(e->app_id);
    exts[i] = exts[--nexts];
    pair();
    changed();
    return;
  }
}

void labwc_model_ws(uint32_t key, const char *name) {
  struct ws *w = find_ws(key);
  if (!w) {
    wss = xrealloc(wss, (nws + 1) * sizeof(*wss));
    w = &wss[nws++];
    memset(w, 0, sizeof(*w));
    w->key = key;
  }
  if (!str_eq(w->name, name)) {
    free(w->name);
    w->name = name ? xstrdup(name) : NULL;
  }
  changed();
}

void labwc_model_ws_state(uint32_t key, bool active, bool urgent) {
  struct ws *w = find_ws(key);
  if (!w)
    return;
  w->active = active;
  w->urgent = urgent;
  changed();
}

void labwc_model_ws_removed(uint32_t key) {
  struct ws *w = find_ws(key);
  if (!w)
    return;
  free(w->name);
  int i = (int)(w - wss);
  memmove(&wss[i], &wss[i + 1], (nws - i - 1) * sizeof(*wss));
  --nws;
  /* labwc moves the windows of a removed workspace to the first one. */
  uint32_t first = nws ? wss[0].key : 0;
  for (int j = 0; j < nwins; ++j)
    if (wins[j].ws == key)
      wins[j].ws = first;
  changed();
}

void labwc_model_win(uint32_t key, const char *title, const char *app_id,
                     bool activated, bool minimized) {
  struct win *w = find_win(key);
  bool fresh = !w;
  if (fresh) {
    wins = xrealloc(wins, (nwins + 1) * sizeof(*wins));
    w = &wins[nwins++];
    memset(w, 0, sizeof(*w));
    w->key = key;
    /* Windows already open at startup: workspace unknown. */
    w->claim = initial_done;
  }
  bool retitled = !str_eq(w->title, title);
  if (retitled) {
    free(w->title);
    w->title = title ? xstrdup(title) : NULL;
  }
  if (!str_eq(w->app_id, app_id)) {
    free(w->app_id);
    w->app_id = app_id ? xstrdup(app_id) : NULL;
  }
  bool focused = activated && !w->activated;
  if (focused)
    w->claim = true;
  w->activated = activated;
  w->minimized = minimized;
  if (fresh && initial_done)
    emit_win("window.new", w);
  else if (!fresh && retitled)
    emit_win("window.title", w);
  if (focused)
    emit_win("window.focus", w);
  pair();
  changed();
}

void labwc_model_win_closed(uint32_t key) {
  struct win *w = find_win(key);
  if (!w)
    return;
  struct win gone = *w;
  *w = wins[--nwins];
  for (int i = 0; i < nexts; ++i)
    if (exts[i].win == key)
      exts[i].win = 0;
  emit_win("window.close", &gone);
  free(gone.title);
  free(gone.app_id);
  free(gone.ident);
  pair();
  changed();
}

void labwc_model_initial_done(void) { initial_done = true; }

static int shown_pos(uint32_t key) {
  for (int i = 0; i < nshown; ++i)
    if (shown[i] == key)
      return i + 1;
  return 0;
}

void labwc_model_settle(void) {
  if (settle_timer)
    loop_timer_disarm(settle_timer);
  const struct ws *cur = active_ws();
  /* A window focused before and after a switch went with it (SendToDesktop
   * with follow; labwc unfocuses the old workspace's windows otherwise). */
  bool switched = cur && focused_key && cur->key != focused_key;
  if (cur)
    for (int i = 0; i < nwins; ++i) {
      struct win *w = &wins[i];
      if (w->claim || (switched && w->activated && w->was_activated))
        w->ws = cur->key;
      w->claim = false;
      w->was_activated = w->activated;
    }

  uint32_t *old = shown;
  int nold = nshown;
  shown = xcalloc(nws ? nws : 1, sizeof(*shown));
  nshown = 0;
  ++serial; /* the list may change below */
  for (int i = 0; i < nws; ++i)
    if (&wss[i] == cur || ws_has_windows(wss[i].key))
      shown[nshown++] = wss[i].key;
  out_stale = true;

  /* As sway: new ones, the focus, then the ones that went away. */
  for (int i = 0; i < nshown; ++i) {
    bool seen = false;
    for (int j = 0; j < nold && !seen; ++j)
      seen = old[j] == shown[i];
    if (!seen)
      emit_ws("workspace.init", i + 1);
  }
  if (cur && cur->key != focused_key) {
    focused_key = cur->key;
    emit_ws("workspace.focus", shown_pos(cur->key));
  }
  for (int j = 0; j < nold; ++j)
    if (!shown_pos(old[j]))
      emit_ws("workspace.removed", j + 1);
  free(old);
  for (int i = 0; i < nws; ++i) {
    if (wss[i].urgent && !wss[i].was_urgent)
      emit_ws("workspace.urgent", shown_pos(wss[i].key));
    wss[i].was_urgent = wss[i].urgent;
  }

  if (!cur)
    return;
  int want = ws_shows_windows(cur->key) ? EMPTY_NO : EMPTY_YES;
  if (want != focused_empty) {
    focused_empty = want;
    emit_ws(want == EMPTY_YES ? "workspace.empty" : "workspace.occupied",
            shown_pos(cur->key));
  }
}

static int pool_index(uint32_t key) {
  for (int i = 0; i < nws; ++i)
    if (wss[i].key == key)
      return i;
  return -1;
}

/* An empty workspace to open: the first after the last one in the list,
 * else any. */
static uint32_t fresh_ws(uint32_t cur, const char **why) {
  int last = nshown ? pool_index(shown[nshown - 1]) : -1;
  uint32_t any = 0;
  for (int i = 0; i < nws; ++i) {
    if (wss[i].key == cur || ws_has_windows(wss[i].key))
      continue;
    if (i > last)
      return wss[i].key;
    if (!any)
      any = wss[i].key;
  }
  pool_full = !any;
  if (!any) {
    static char msg[128];
    snprintf(msg, sizeof(msg),
             "all %d labwc workspaces are in use (raise <desktops number> "
             "in rc.xml)",
             nws);
    *why = msg;
  }
  return any;
}

uint32_t labwc_model_target(const char *cmd, int num, const char **why) {
  *why = NULL;
  pool_full = false;
  const struct ws *cur = active_ws();
  int f = cur ? shown_pos(cur->key) - 1 : -1;
  if (f < 0) {
    *why = "labwc reports no active workspace";
    return 0;
  }
  bool last = f == nshown - 1;
  bool fresh_here = last && !ws_has_windows(cur->key);
  if (strcmp(cmd, "next") == 0) {
    if (!last)
      return shown[f + 1];
    return fresh_here ? 0 : fresh_ws(cur->key, why);
  }
  if (strcmp(cmd, "prev") == 0)
    return f > 0 ? shown[f - 1] : 0;
  if (num >= 1 && num <= nshown)
    return shown[num - 1];
  return fresh_here ? 0 : fresh_ws(cur->key, why);
}

void labwc_model_reset(void) {
  for (int i = 0; i < nws; ++i)
    free(wss[i].name);
  for (int i = 0; i < nwins; ++i) {
    free(wins[i].title);
    free(wins[i].app_id);
    free(wins[i].ident);
  }
  for (int i = 0; i < nexts; ++i) {
    free(exts[i].ident);
    free(exts[i].title);
    free(exts[i].app_id);
  }
  free(exts);
  exts = NULL;
  nexts = 0;
  free(wss);
  free(wins);
  free(shown);
  wss = NULL;
  wins = NULL;
  shown = NULL;
  nws = nwins = nshown = 0;
  initial_done = false;
  focused_key = 0;
  focused_empty = EMPTY_UNKNOWN;
  free(out_cache);
  out_cache = NULL;
  out_count = 0;
  out_stale = true;
}

/* ---- queries ---- */

int labwc_workspaces(const struct wm_workspace **out) {
  if (out_stale) {
    out_cache = xrealloc(out_cache, (nshown + 1) * sizeof(*out_cache));
    out_count = 0;
    for (int i = 0; i < nshown; ++i) {
      const struct ws *w = find_ws(shown[i]);
      struct wm_workspace *o = &out_cache[out_count++];
      memset(o, 0, sizeof(*o));
      snprintf(o->name, sizeof(o->name), "%d", i + 1);
      o->num = i + 1;
      o->focused = o->visible = w && w->key == focused_key;
      o->urgent = w && w->urgent;
    }
    out_stale = false;
  }
  *out = out_cache;
  return out_count;
}

int labwc_focused_empty(void) {
  return focused_empty == EMPTY_UNKNOWN ? -1 : focused_empty == EMPTY_YES;
}

void labwc_emit_focused_state(void) {
  focused_empty = EMPTY_UNKNOWN;
  labwc_model_settle();
}

static void activate_ws(struct ws *w) {
  ext_workspace_handle_v1_activate(w->handle);
  ext_workspace_manager_v1_commit(ws_mgr);
  wl_display_flush(wl->display);
}

int labwc_go(const char *cmd, int num, const char **what) {
  *what = NULL;
  if (!ws_mgr) {
    *what = "labwc workspaces need the daemon (swell daemon)";
    return -1;
  }
  labwc_model_settle();
  uint32_t key = labwc_model_target(cmd, num, what);
  struct ws *w = key ? find_ws(key) : NULL;
  if (!w || !w->handle)
    return pool_full ? -1 : 0;
  activate_ws(w);
  return 0;
}

uint64_t labwc_serial(void) { return serial; }

int labwc_window_count(void) { return nwins; }

void labwc_window_get(int i, struct labwc_window *out) {
  const struct win *w = &wins[i];
  *out = (struct labwc_window){
      .key = w->key,
      .ws = w->ws,
      .title = w->title,
      .app_id = w->app_id,
      .ident = w->ident,
      .activated = w->activated,
      .minimized = w->minimized,
  };
}

uint32_t labwc_workspace_key(int i) {
  return i >= 0 && i < nshown ? shown[i] : 0;
}

void labwc_focus_window(uint32_t key) {
  struct win *w = find_win(key);
  if (!w || !w->handle || !wl->seat)
    return;
  zwlr_foreign_toplevel_handle_v1_activate(w->handle, wl->seat);
  wl_display_flush(wl->display);
}

void labwc_close_window(uint32_t key) {
  struct win *w = find_win(key);
  if (!w || !w->handle)
    return;
  zwlr_foreign_toplevel_handle_v1_close(w->handle);
  wl_display_flush(wl->display);
}

void labwc_focus_workspace(uint32_t key) {
  if (!ws_mgr)
    return;
  if (!key) {
    const char *why;
    key = labwc_model_target("goto", nshown + 1, &why);
  }
  struct ws *w = key ? find_ws(key) : NULL;
  if (w && w->handle)
    activate_ws(w);
}

/* ---- ext-workspace ---- */

static void group_capabilities(void *data, struct ext_workspace_group_handle_v1 *g,
                               uint32_t caps) {}
static void group_output_enter(void *data, struct ext_workspace_group_handle_v1 *g,
                               struct wl_output *o) {}
static void group_output_leave(void *data, struct ext_workspace_group_handle_v1 *g,
                               struct wl_output *o) {}
static void group_workspace_enter(void *data,
                                  struct ext_workspace_group_handle_v1 *g,
                                  struct ext_workspace_handle_v1 *ws) {}
static void group_workspace_leave(void *data,
                                  struct ext_workspace_group_handle_v1 *g,
                                  struct ext_workspace_handle_v1 *ws) {}
static void group_removed(void *data, struct ext_workspace_group_handle_v1 *g) {
  ext_workspace_group_handle_v1_destroy(g);
}

static const struct ext_workspace_group_handle_v1_listener group_listener = {
    .capabilities = group_capabilities,
    .output_enter = group_output_enter,
    .output_leave = group_output_leave,
    .workspace_enter = group_workspace_enter,
    .workspace_leave = group_workspace_leave,
    .removed = group_removed,
};

static uint32_t key_of(void *data) { return (uint32_t)(uintptr_t)data; }

static void ws_id(void *data, struct ext_workspace_handle_v1 *h,
                  const char *id) {}
static void ws_name(void *data, struct ext_workspace_handle_v1 *h,
                    const char *name) {
  labwc_model_ws(key_of(data), name);
}
static void ws_coordinates(void *data, struct ext_workspace_handle_v1 *h,
                           struct wl_array *c) {}
static void ws_state(void *data, struct ext_workspace_handle_v1 *h,
                     uint32_t state) {
  labwc_model_ws_state(key_of(data),
                       state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE,
                       state & EXT_WORKSPACE_HANDLE_V1_STATE_URGENT);
}
static void ws_capabilities(void *data, struct ext_workspace_handle_v1 *h,
                            uint32_t caps) {}
static void ws_removed(void *data, struct ext_workspace_handle_v1 *h) {
  labwc_model_ws_removed(key_of(data));
  ext_workspace_handle_v1_destroy(h);
}

static const struct ext_workspace_handle_v1_listener ws_listener = {
    .id = ws_id,
    .name = ws_name,
    .coordinates = ws_coordinates,
    .state = ws_state,
    .capabilities = ws_capabilities,
    .removed = ws_removed,
};

static void mgr_group(void *data, struct ext_workspace_manager_v1 *m,
                      struct ext_workspace_group_handle_v1 *g) {
  ext_workspace_group_handle_v1_add_listener(g, &group_listener, NULL);
}

static void mgr_workspace(void *data, struct ext_workspace_manager_v1 *m,
                          struct ext_workspace_handle_v1 *h) {
  uint32_t key = next_key++;
  labwc_model_ws(key, NULL);
  find_ws(key)->handle = h;
  ext_workspace_handle_v1_add_listener(h, &ws_listener,
                                       (void *)(uintptr_t)key);
}

static void mgr_done(void *data, struct ext_workspace_manager_v1 *m) {
  changed();
}

static void mgr_finished(void *data, struct ext_workspace_manager_v1 *m) {
  ext_workspace_manager_v1_destroy(m);
  if (m == ws_mgr)
    ws_mgr = NULL;
}

static const struct ext_workspace_manager_v1_listener mgr_listener = {
    .workspace_group = mgr_group,
    .workspace = mgr_workspace,
    .done = mgr_done,
    .finished = mgr_finished,
};

/* ---- wlr-foreign-toplevel ---- */

/* State between a handle's done events. */
struct toplevel {
  struct toplevel *next;
  struct zwlr_foreign_toplevel_handle_v1 *handle;
  uint32_t key;
  char *title, *app_id;
  bool activated, minimized;
};

static struct toplevel *toplevels;

static void toplevel_free(struct toplevel *t) {
  for (struct toplevel **p = &toplevels; *p; p = &(*p)->next)
    if (*p == t) {
      *p = t->next;
      break;
    }
  zwlr_foreign_toplevel_handle_v1_destroy(t->handle);
  free(t->title);
  free(t->app_id);
  free(t);
}

static void tl_title(void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
                     const char *title) {
  struct toplevel *t = data;
  free(t->title);
  t->title = xstrdup(title);
}
static void tl_app_id(void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
                      const char *app_id) {
  struct toplevel *t = data;
  free(t->app_id);
  t->app_id = xstrdup(app_id);
}
static void tl_output_enter(void *data,
                            struct zwlr_foreign_toplevel_handle_v1 *h,
                            struct wl_output *o) {}
static void tl_output_leave(void *data,
                            struct zwlr_foreign_toplevel_handle_v1 *h,
                            struct wl_output *o) {}
static void tl_state(void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
                     struct wl_array *states) {
  struct toplevel *t = data;
  t->activated = t->minimized = false;
  uint32_t *s;
  wl_array_for_each(s, states) {
    if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED)
      t->activated = true;
    else if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED)
      t->minimized = true;
  }
}
static void tl_done(void *data, struct zwlr_foreign_toplevel_handle_v1 *h) {
  struct toplevel *t = data;
  labwc_model_win(t->key, t->title, t->app_id, t->activated, t->minimized);
  find_win(t->key)->handle = h;
}
static void tl_closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *h) {
  struct toplevel *t = data;
  labwc_model_win_closed(t->key);
  toplevel_free(t);
}
static void tl_parent(void *data, struct zwlr_foreign_toplevel_handle_v1 *h,
                      struct zwlr_foreign_toplevel_handle_v1 *parent) {}

static const struct zwlr_foreign_toplevel_handle_v1_listener tl_listener = {
    .title = tl_title,
    .app_id = tl_app_id,
    .output_enter = tl_output_enter,
    .output_leave = tl_output_leave,
    .state = tl_state,
    .done = tl_done,
    .closed = tl_closed,
    .parent = tl_parent,
};

static void tlm_toplevel(void *data, struct zwlr_foreign_toplevel_manager_v1 *m,
                         struct zwlr_foreign_toplevel_handle_v1 *h) {
  struct toplevel *t = xcalloc(1, sizeof(*t));
  t->key = next_key++;
  t->handle = h;
  t->next = toplevels;
  toplevels = t;
  zwlr_foreign_toplevel_handle_v1_add_listener(h, &tl_listener, t);
}

static void tlm_finished(void *data,
                         struct zwlr_foreign_toplevel_manager_v1 *m) {
  zwlr_foreign_toplevel_manager_v1_destroy(m);
  if (m == tl_mgr)
    tl_mgr = NULL;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener tlm_listener = {
    .toplevel = tlm_toplevel,
    .finished = tlm_finished,
};

/* ---- ext-foreign-toplevel-list (identifiers for thumbnails) ---- */

struct ext_toplevel {
  struct ext_toplevel *next;
  struct ext_foreign_toplevel_handle_v1 *handle;
  uint32_t key;
  char *ident, *title, *app_id;
};

static struct ext_foreign_toplevel_list_v1 *ext_list;
static struct ext_toplevel *ext_toplevels;

static void ext_toplevel_free(struct ext_toplevel *t) {
  for (struct ext_toplevel **p = &ext_toplevels; *p; p = &(*p)->next)
    if (*p == t) {
      *p = t->next;
      break;
    }
  ext_foreign_toplevel_handle_v1_destroy(t->handle);
  free(t->ident);
  free(t->title);
  free(t->app_id);
  free(t);
}

static void et_closed(void *data, struct ext_foreign_toplevel_handle_v1 *h) {
  struct ext_toplevel *t = data;
  labwc_model_ext_closed(t->key);
  ext_toplevel_free(t);
}
static void et_done(void *data, struct ext_foreign_toplevel_handle_v1 *h) {
  struct ext_toplevel *t = data;
  labwc_model_ext(t->key, t->ident, t->title, t->app_id);
}
static void et_title(void *data, struct ext_foreign_toplevel_handle_v1 *h,
                     const char *title) {
  struct ext_toplevel *t = data;
  free(t->title);
  t->title = xstrdup(title);
}
static void et_app_id(void *data, struct ext_foreign_toplevel_handle_v1 *h,
                      const char *app_id) {
  struct ext_toplevel *t = data;
  free(t->app_id);
  t->app_id = xstrdup(app_id);
}
static void et_identifier(void *data, struct ext_foreign_toplevel_handle_v1 *h,
                          const char *ident) {
  struct ext_toplevel *t = data;
  free(t->ident);
  t->ident = xstrdup(ident);
}

static const struct ext_foreign_toplevel_handle_v1_listener et_listener = {
    .closed = et_closed,
    .done = et_done,
    .title = et_title,
    .app_id = et_app_id,
    .identifier = et_identifier,
};

static void el_toplevel(void *data, struct ext_foreign_toplevel_list_v1 *l,
                        struct ext_foreign_toplevel_handle_v1 *h) {
  struct ext_toplevel *t = xcalloc(1, sizeof(*t));
  t->key = next_key++;
  t->handle = h;
  t->next = ext_toplevels;
  ext_toplevels = t;
  ext_foreign_toplevel_handle_v1_add_listener(h, &et_listener, t);
}

static void el_finished(void *data, struct ext_foreign_toplevel_list_v1 *l) {
  ext_foreign_toplevel_list_v1_destroy(l);
  if (l == ext_list)
    ext_list = NULL;
}

static const struct ext_foreign_toplevel_list_v1_listener el_listener = {
    .toplevel = el_toplevel,
    .finished = el_finished,
};

/* ---- setup ---- */

static void on_settle(struct loop_timer *t, void *data) {
  labwc_model_settle();
}

bool labwc_detect(void) {
  const char *pid = getenv("LABWC_PID");
  const char *desk = getenv("XDG_CURRENT_DESKTOP");
  return (pid && *pid) || (desk && strstr(desk, "labwc"));
}

bool labwc_init(struct loop *loop, struct swell_wl *w) {
  if (linked)
    return true;
  if (!loop || !w) {
    linked = true;
    return true;
  }
  if (!w->workspace_manager_name) {
    warn("labwc: no ext-workspace-v1 (labwc 0.8.3 or later has it)");
    return false;
  }
  wl = w;
  ws_mgr = wl_registry_bind(w->registry, w->workspace_manager_name,
                            &ext_workspace_manager_v1_interface, 1);
  ext_workspace_manager_v1_add_listener(ws_mgr, &mgr_listener, NULL);
  if (w->wlr_toplevel_manager_name) {
    tl_mgr = wl_registry_bind(
        w->registry, w->wlr_toplevel_manager_name,
        &zwlr_foreign_toplevel_manager_v1_interface,
        w->wlr_toplevel_manager_version < 3 ? w->wlr_toplevel_manager_version
                                            : 3);
    zwlr_foreign_toplevel_manager_v1_add_listener(tl_mgr, &tlm_listener,
                                                  NULL);
  } else {
    warn("labwc: no wlr-foreign-toplevel-management; every workspace will "
         "look empty");
  }
  ext_list = wl_bind_toplevel_list(w);
  if (ext_list)
    ext_foreign_toplevel_list_v1_add_listener(ext_list, &el_listener, NULL);
  /* Globals' first state, then each handle's. */
  wl_display_roundtrip(w->display);
  wl_display_roundtrip(w->display);
  labwc_model_initial_done();
  ev_loop = loop;
  settle_timer = loop_add_timer(loop, CLOCK_MONOTONIC, on_settle, NULL);
  labwc_model_settle();
  linked = true;
  return true;
}

void labwc_finish(void) {
  if (settle_timer)
    loop_remove_timer(settle_timer);
  settle_timer = NULL;
  ev_loop = NULL;
  for (int i = 0; i < nws; ++i)
    if (wss[i].handle)
      ext_workspace_handle_v1_destroy(wss[i].handle);
  while (toplevels)
    toplevel_free(toplevels);
  while (ext_toplevels)
    ext_toplevel_free(ext_toplevels);
  if (ext_list) {
    ext_foreign_toplevel_list_v1_stop(ext_list);
    ext_foreign_toplevel_list_v1_destroy(ext_list);
  }
  ext_list = NULL;
  if (ws_mgr)
    ext_workspace_manager_v1_destroy(ws_mgr);
  if (tl_mgr)
    zwlr_foreign_toplevel_manager_v1_destroy(tl_mgr);
  ws_mgr = NULL;
  tl_mgr = NULL;
  wl = NULL;
  linked = false;
  labwc_model_reset();
}
