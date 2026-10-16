#include "niri.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "bus.h"
#include "cJSON.h"
#include "util.h"

#define REQUEST_TIMEOUT_MS 500
#define INITIAL_STATE_TIMEOUT_MS 1000

struct ws {
  uint64_t id;
  int idx;
  char *name; /* NULL if unnamed */
  char *output;
  bool urgent, active, focused;
};

struct win {
  uint64_t id;
  uint64_t ws_id; /* 0: none (ids start at 1) */
  char *title, *app_id;
  bool focused, floating, urgent;
};

static char *sock_path;
static int ev_fd = -1;
static struct loop *ev_loop;
static struct loop_watch *watch;
static char *buf;
static size_t buf_len, buf_cap;

static struct ws *wss;
static int nws;
static struct win *wins;
static int nwins;
static bool have_ws, have_wins, overview_open;
static uint64_t focused_ws; /* last one reported by workspace.focus */

static struct wm_workspace *out_cache;
static int out_count;
static bool out_stale = true;

enum { EMPTY_UNKNOWN = -1, EMPTY_NO = 0, EMPTY_YES = 1 };
static int focused_empty = EMPTY_UNKNOWN;

/* ---- JSON helpers ---- */

static uint64_t j_id(const cJSON *o, const char *key) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  return cJSON_IsNumber(v) && v->valuedouble > 0 ? (uint64_t)v->valuedouble
                                                 : 0;
}

static bool j_bool(const cJSON *o, const char *key) {
  return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, key));
}

/* A string field, or NULL for null/missing. Caller frees. */
static char *j_str(const cJSON *o, const char *key) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  return cJSON_IsString(v) ? xstrdup(v->valuestring) : NULL;
}

static bool str_eq(const char *a, const char *b) {
  return a == b || (a && b && strcmp(a, b) == 0);
}

/* ---- model ---- */

static void ws_free(struct ws *w) {
  free(w->name);
  free(w->output);
}

static void win_free(struct win *w) {
  free(w->title);
  free(w->app_id);
}

static struct ws *find_ws(uint64_t id) {
  for (int i = 0; i < nws; ++i)
    if (wss[i].id == id)
      return &wss[i];
  return NULL;
}

static struct win *find_win(uint64_t id) {
  for (int i = 0; i < nwins; ++i)
    if (wins[i].id == id)
      return &wins[i];
  return NULL;
}

static bool ws_has_windows(uint64_t id) {
  for (int i = 0; i < nwins; ++i)
    if (wins[i].ws_id == id)
      return true;
  return false;
}

static struct ws parse_ws(const cJSON *o) {
  const cJSON *idx = cJSON_GetObjectItemCaseSensitive(o, "idx");
  return (struct ws){
      .id = j_id(o, "id"),
      .idx = cJSON_IsNumber(idx) ? idx->valueint : 0,
      .name = j_str(o, "name"),
      .output = j_str(o, "output"),
      .urgent = j_bool(o, "is_urgent"),
      .active = j_bool(o, "is_active"),
      .focused = j_bool(o, "is_focused"),
  };
}

static struct win parse_win(const cJSON *o) {
  return (struct win){
      .id = j_id(o, "id"),
      .ws_id = j_id(o, "workspace_id"),
      .title = j_str(o, "title"),
      .app_id = j_str(o, "app_id"),
      .focused = j_bool(o, "is_focused"),
      .floating = j_bool(o, "is_floating"),
      .urgent = j_bool(o, "is_urgent"),
  };
}

/* SWELL_WORKSPACE: the name, else the index. */
static char *ws_label(const struct ws *w) {
  return w->name ? xstrdup(w->name) : xasprintf("%d", w->idx);
}

static void emit_ws(const char *event, const struct ws *w) {
  char *label = w ? ws_label(w) : xstrdup("");
  char *env = xasprintf("SWELL_WORKSPACE=%s", label);
  bus_emit(event, env, NULL);
  free(env);
  free(label);
}

static void emit_win(const char *event, const struct win *w) {
  char *e1 = xasprintf("SWELL_CON_ID=%llu", (unsigned long long)w->id);
  char *e2 = xasprintf("SWELL_APP_ID=%s", w->app_id ? w->app_id : "");
  char *e3 = xasprintf("SWELL_TITLE=%s", w->title ? w->title : "");
  bus_emit(event, e1, e2, e3, NULL);
  free(e1), free(e2), free(e3);
}

static const struct ws *current_ws(void) {
  for (int i = 0; i < nws; ++i)
    if (wss[i].focused)
      return &wss[i];
  return NULL;
}

/* Report workspace.empty/occupied on a transition of the focused
 * workspace's state, once both workspaces and windows are known. */
static void sync_focused_empty(void) {
  if (!have_ws || !have_wins)
    return;
  const struct ws *cur = current_ws();
  if (!cur)
    return;
  int want = ws_has_windows(cur->id) ? EMPTY_NO : EMPTY_YES;
  if (want == focused_empty)
    return;
  focused_empty = want;
  emit_ws(want == EMPTY_YES ? "workspace.empty" : "workspace.occupied", cur);
}

/* workspace.focus when the focused workspace changed. */
static void sync_focus(void) {
  const struct ws *cur = current_ws();
  if (!cur || cur->id == focused_ws)
    return;
  focused_ws = cur->id;
  emit_ws("workspace.focus", cur);
}

static void on_workspaces_changed(const cJSON *list) {
  int n = cJSON_GetArraySize(list);
  struct ws *nw = xcalloc(n ? n : 1, sizeof(*nw));
  int k = 0;
  const cJSON *it;
  cJSON_ArrayForEach(it, list) {
    if (cJSON_IsObject(it))
      nw[k++] = parse_ws(it);
  }
  struct ws *old = wss;
  int nold = nws;
  wss = nw;
  nws = k;
  have_ws = true;
  out_stale = true;

  /* Events after the model is updated, so subscribers see the new state. */
  for (int i = 0; i < nws; ++i) {
    const struct ws *o = NULL;
    for (int j = 0; j < nold && !o; ++j)
      if (old[j].id == wss[i].id)
        o = &old[j];
    if (!o)
      emit_ws("workspace.init", &wss[i]);
    else {
      if (!str_eq(o->output, wss[i].output) || o->idx != wss[i].idx)
        emit_ws("workspace.move", &wss[i]);
      if (!str_eq(o->name, wss[i].name))
        emit_ws("workspace.rename", &wss[i]);
      if (!o->urgent && wss[i].urgent)
        emit_ws("workspace.urgent", &wss[i]);
    }
  }
  for (int j = 0; j < nold; ++j) {
    if (!find_ws(old[j].id))
      emit_ws("workspace.removed", &old[j]);
    ws_free(&old[j]);
  }
  free(old);
  sync_focus();
  sync_focused_empty();
}

static void on_workspace_activated(const cJSON *o) {
  struct ws *w = find_ws(j_id(o, "id"));
  if (!w)
    return;
  bool focused = j_bool(o, "focused");
  for (int i = 0; i < nws; ++i) {
    if (&wss[i] != w && str_eq(wss[i].output, w->output))
      wss[i].active = false;
    if (focused)
      wss[i].focused = false;
  }
  w->active = true;
  if (focused)
    w->focused = true;
  out_stale = true;
  sync_focus();
  sync_focused_empty();
}

static void on_workspace_urgency(const cJSON *o) {
  struct ws *w = find_ws(j_id(o, "id"));
  if (!w)
    return;
  w->urgent = j_bool(o, "urgent");
  out_stale = true;
  emit_ws("workspace.urgent", w);
}

static void unfocus_windows_except(uint64_t id) {
  for (int i = 0; i < nwins; ++i)
    if (wins[i].id != id)
      wins[i].focused = false;
}

static void on_windows_changed(const cJSON *list) {
  int n = cJSON_GetArraySize(list);
  struct win *nw = xcalloc(n ? n : 1, sizeof(*nw));
  int k = 0;
  const cJSON *it;
  cJSON_ArrayForEach(it, list) {
    if (cJSON_IsObject(it))
      nw[k++] = parse_win(it);
  }
  struct win *old = wins;
  int nold = nwins;
  bool initial = !have_wins;
  wins = nw;
  nwins = k;
  have_wins = true;
  /* The first list is the state at connection time, not new windows. */
  if (!initial) {
    for (int i = 0; i < nwins; ++i) {
      bool seen = false;
      for (int j = 0; j < nold && !seen; ++j)
        seen = old[j].id == wins[i].id;
      if (!seen)
        emit_win("window.new", &wins[i]);
    }
    for (int j = 0; j < nold; ++j)
      if (!find_win(old[j].id))
        emit_win("window.close", &old[j]);
  }
  for (int j = 0; j < nold; ++j)
    win_free(&old[j]);
  free(old);
  sync_focused_empty();
}

static void on_window_opened_or_changed(const cJSON *o) {
  struct win nw = parse_win(o);
  if (!nw.id) {
    win_free(&nw);
    return;
  }
  if (nw.focused)
    unfocus_windows_except(nw.id);
  struct win *w = find_win(nw.id);
  if (!w) {
    wins = xrealloc(wins, (nwins + 1) * sizeof(*wins));
    wins[nwins++] = nw;
    w = &wins[nwins - 1];
    emit_win("window.new", w);
    if (w->focused)
      emit_win("window.focus", w);
  } else {
    struct win old = *w;
    *w = nw;
    if (!str_eq(old.title, w->title))
      emit_win("window.title", w);
    if (old.ws_id != w->ws_id)
      emit_win("window.move", w);
    if (old.floating != w->floating)
      emit_win("window.floating", w);
    if (!old.urgent && w->urgent)
      emit_win("window.urgent", w);
    if (!old.focused && w->focused)
      emit_win("window.focus", w);
    win_free(&old);
  }
  sync_focused_empty();
}

static void on_window_closed(const cJSON *o) {
  struct win *w = find_win(j_id(o, "id"));
  if (!w)
    return;
  struct win gone = *w;
  *w = wins[--nwins];
  emit_win("window.close", &gone);
  win_free(&gone);
  sync_focused_empty();
}

static void on_window_focus(const cJSON *o) {
  uint64_t id = j_id(o, "id");
  unfocus_windows_except(id);
  struct win *w = find_win(id);
  if (w && !w->focused) {
    w->focused = true;
    emit_win("window.focus", w);
  }
}

static void on_window_urgency(const cJSON *o) {
  struct win *w = find_win(j_id(o, "id"));
  if (!w)
    return;
  w->urgent = j_bool(o, "urgent");
  emit_win("window.urgent", w);
}

static void on_overview(const cJSON *o) {
  bool open = j_bool(o, "is_open");
  if (open == overview_open)
    return;
  overview_open = open;
  bus_emit(open ? "overview.open" : "overview.close", NULL);
}

void niri_handle_line(const char *line) {
  cJSON *root = cJSON_Parse(line);
  /* {"Variant":{...fields}} */
  const cJSON *ev = root && cJSON_IsObject(root) ? root->child : NULL;
  if (!ev || !ev->string || !cJSON_IsObject(ev)) {
    cJSON_Delete(root);
    return;
  }
  const char *k = ev->string;
  if (strcmp(k, "WorkspacesChanged") == 0)
    on_workspaces_changed(cJSON_GetObjectItemCaseSensitive(ev, "workspaces"));
  else if (strcmp(k, "WorkspaceActivated") == 0)
    on_workspace_activated(ev);
  else if (strcmp(k, "WorkspaceUrgencyChanged") == 0)
    on_workspace_urgency(ev);
  else if (strcmp(k, "WindowsChanged") == 0)
    on_windows_changed(cJSON_GetObjectItemCaseSensitive(ev, "windows"));
  else if (strcmp(k, "WindowOpenedOrChanged") == 0)
    on_window_opened_or_changed(
        cJSON_GetObjectItemCaseSensitive(ev, "window"));
  else if (strcmp(k, "WindowClosed") == 0)
    on_window_closed(ev);
  else if (strcmp(k, "WindowFocusChanged") == 0)
    on_window_focus(ev);
  else if (strcmp(k, "WindowUrgencyChanged") == 0)
    on_window_urgency(ev);
  else if (strcmp(k, "OverviewOpenedOrClosed") == 0)
    on_overview(ev);
  cJSON_Delete(root);
}

void niri_reset(void) {
  for (int i = 0; i < nws; ++i)
    ws_free(&wss[i]);
  for (int i = 0; i < nwins; ++i)
    win_free(&wins[i]);
  free(wss);
  free(wins);
  wss = NULL;
  wins = NULL;
  nws = nwins = 0;
  have_ws = have_wins = overview_open = false;
  focused_ws = 0;
  focused_empty = EMPTY_UNKNOWN;
  free(out_cache);
  out_cache = NULL;
  out_count = 0;
  out_stale = true;
}

/* ---- queries ---- */

static int ws_order(const void *a, const void *b) {
  const struct ws *x = *(const struct ws *const *)a;
  const struct ws *y = *(const struct ws *const *)b;
  int c = strcmp(x->output ? x->output : "", y->output ? y->output : "");
  return c ? c : (x->idx > y->idx) - (x->idx < y->idx);
}

/* niri keeps one empty workspace after the last on every output. Sway has
 * no such thing, so it's shown only while in use (focused or visible). */
static bool hidden_trailing(const struct ws *w) {
  if (w->name || w->focused || w->active || ws_has_windows(w->id))
    return false;
  for (int i = 0; i < nws; ++i)
    if (str_eq(wss[i].output, w->output) && wss[i].idx > w->idx)
      return false;
  return true;
}

int niri_workspaces(const struct wm_workspace **out) {
  if (out_stale) {
    const struct ws **sorted = xcalloc(nws ? nws : 1, sizeof(*sorted));
    for (int i = 0; i < nws; ++i)
      sorted[i] = &wss[i];
    qsort(sorted, nws, sizeof(*sorted), ws_order);
    out_cache = xrealloc(out_cache, (nws + 1) * sizeof(*out_cache));
    out_count = 0;
    for (int i = 0; i < nws; ++i) {
      const struct ws *w = sorted[i];
      if (hidden_trailing(w))
        continue;
      struct wm_workspace *o = &out_cache[out_count++];
      memset(o, 0, sizeof(*o));
      char *label = ws_label(w);
      snprintf(o->name, sizeof(o->name), "%s", label);
      o->truncated = strlen(label) >= sizeof(o->name);
      free(label);
      snprintf(o->output, sizeof(o->output), "%s", w->output ? w->output : "");
      o->num = w->idx;
      o->focused = w->focused;
      o->visible = w->active;
      o->urgent = w->urgent;
    }
    free(sorted);
    out_stale = false;
  }
  *out = out_cache;
  return out_count;
}

int niri_focused_empty(void) {
  return focused_empty == EMPTY_UNKNOWN ? -1 : focused_empty == EMPTY_YES;
}

void niri_emit_focused_state(void) {
  focused_empty = EMPTY_UNKNOWN;
  sync_focused_empty();
}

/* ---- sockets ---- */

static int connect_socket(void) {
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  if (strlen(sock_path) >= sizeof(addr.sun_path)) {
    warn("niri: socket path too long: %s", sock_path);
    return -1;
  }
  strcpy(addr.sun_path, sock_path);
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static bool write_all(int fd, const char *s, size_t n) {
  while (n) {
    ssize_t w = write(fd, s, n);
    if (w < 0 && errno == EINTR)
      continue;
    if (w <= 0)
      return false;
    s += w;
    n -= w;
  }
  return true;
}

/* Read what's available into buf. Returns false on EOF or error. */
static bool fill(int fd) {
  for (;;) {
    if (buf_cap - buf_len < 4096) {
      buf_cap = buf_cap ? buf_cap * 2 : 8192;
      buf = xrealloc(buf, buf_cap);
    }
    ssize_t r = read(fd, buf + buf_len, buf_cap - buf_len - 1);
    if (r > 0) {
      buf_len += r;
      continue;
    }
    if (r < 0 && errno == EINTR)
      continue;
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return true;
    return false;
  }
}

/* Take the next complete line out of buf; NULL if none. Caller frees. */
static char *next_line(void) {
  char *nl = memchr(buf, '\n', buf_len);
  if (!nl)
    return NULL;
  size_t n = nl - buf;
  char *line = xstrndup(buf, n);
  memmove(buf, nl + 1, buf_len - n - 1);
  buf_len -= n + 1;
  return line;
}

/* Wait for one line on a blocking-ish fd, up to timeout_ms. */
static char *read_line(int fd, int timeout_ms) {
  int64_t until = now_ms() + timeout_ms;
  for (;;) {
    char *line = next_line();
    if (line)
      return line;
    int left = (int)(until - now_ms());
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    if (left <= 0 || poll(&pfd, 1, left) <= 0 || !fill(fd))
      return NULL;
  }
}

/* "Ok" reply, or warn with niri's error. */
static bool reply_ok(const char *line, const char *what) {
  cJSON *r = cJSON_Parse(line);
  bool ok = r && cJSON_GetObjectItemCaseSensitive(r, "Ok");
  if (!ok) {
    const cJSON *err = r ? cJSON_GetObjectItemCaseSensitive(r, "Err") : NULL;
    warn("niri: '%s': %s", what,
         cJSON_IsString(err) ? err->valuestring : "bad reply");
  }
  cJSON_Delete(r);
  return ok;
}

int niri_request(const char *json) {
  if (!sock_path)
    return -1;
  int fd = connect_socket();
  if (fd < 0) {
    warn("niri: cannot connect to %s", sock_path);
    return -1;
  }
  fcntl(fd, F_SETFL, O_NONBLOCK);
  int ret = -1;
  /* The request socket has its own buffer; keep the event stream's. */
  char *saved = buf;
  size_t saved_len = buf_len, saved_cap = buf_cap;
  buf = NULL;
  buf_len = buf_cap = 0;
  if (write_all(fd, json, strlen(json)) && write_all(fd, "\n", 1)) {
    char *line = read_line(fd, REQUEST_TIMEOUT_MS);
    if (line && reply_ok(line, json))
      ret = 0;
    else if (!line)
      warn("niri: no reply to '%s'", json);
    free(line);
  } else {
    warn("niri: cannot send '%s'", json);
  }
  free(buf);
  buf = saved;
  buf_len = saved_len;
  buf_cap = saved_cap;
  close(fd);
  return ret;
}

static void lost(void) {
  warn("lost the niri IPC connection");
  loop_remove_fd(watch);
  watch = NULL;
  loop_quit(ev_loop);
}

static void on_events(int fd, short revents, void *data) {
  bool alive = fill(fd);
  char *line;
  while ((line = next_line())) {
    niri_handle_line(line);
    free(line);
  }
  if (!alive || (revents & (POLLERR | POLLHUP)))
    lost();
}

static bool start_events(struct loop *loop) {
  ev_fd = connect_socket();
  if (ev_fd < 0) {
    warn("cannot connect to niri at %s", sock_path);
    return false;
  }
  fcntl(ev_fd, F_SETFL, O_NONBLOCK);
  const char req[] = "\"EventStream\"\n";
  char *line = NULL;
  if (write_all(ev_fd, req, sizeof(req) - 1))
    line = read_line(ev_fd, REQUEST_TIMEOUT_MS);
  bool ok = line && reply_ok(line, "EventStream");
  free(line);
  if (!ok) {
    close(ev_fd);
    ev_fd = -1;
    return false;
  }
  /* niri sends the full state first; have it before modules start. */
  int64_t until = now_ms() + INITIAL_STATE_TIMEOUT_MS;
  while (!(have_ws && have_wins)) {
    line = read_line(ev_fd, (int)(until - now_ms()));
    if (!line)
      break;
    niri_handle_line(line);
    free(line);
  }
  if (!(have_ws && have_wins))
    warn("niri: no initial workspace/window state");
  ev_loop = loop;
  watch = loop_add_fd(loop, ev_fd, POLLIN, on_events, NULL);
  /* Lines that arrived with the initial state. */
  while ((line = next_line())) {
    niri_handle_line(line);
    free(line);
  }
  return true;
}

bool niri_init(struct loop *loop) {
  if (sock_path)
    return true;
  const char *env = getenv("NIRI_SOCKET");
  if (!env || !*env)
    return false;
  sock_path = xstrdup(env);
  if (loop && !start_events(loop)) {
    free(sock_path);
    sock_path = NULL;
    return false;
  }
  return true;
}

void niri_finish(void) {
  loop_remove_fd(watch);
  watch = NULL;
  if (ev_fd >= 0)
    close(ev_fd);
  ev_fd = -1;
  ev_loop = NULL;
  free(buf);
  buf = NULL;
  buf_len = buf_cap = 0;
  free(sock_path);
  sock_path = NULL;
  niri_reset();
}
