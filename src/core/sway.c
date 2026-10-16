#include "sway.h"

#include <ctype.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bus.h"
#include "util.h"

struct sub {
  sway_event_fn fn;
  void *data;
  bool dead;
};

static bool connected;
static struct loop_watch *watch;
static struct sub *subs;
static int nsubs, depth;

static struct wm_workspace *ws_cache;
static int ws_count;
static bool ws_stale = true;

enum { EMPTY_UNKNOWN = -1, EMPTY_NO = 0, EMPTY_YES = 1 };
static int focused_empty = EMPTY_UNKNOWN;

/* `sway --get-socketpath`, for when the environment lacks SWAYSOCK (e.g. a
 * systemd user service started before sway exported it). */
static char *socket_from_sway(void) {
  FILE *p = popen("sway --get-socketpath 2>/dev/null", "r");
  if (!p)
    return NULL;
  char buf[512];
  char *r = fgets(buf, sizeof(buf), p) ? xstrdup(buf) : NULL;
  pclose(p);
  if (r)
    r[strcspn(r, "\n")] = '\0';
  if (r && !*r) {
    free(r);
    r = NULL;
  }
  return r;
}

static void on_events(int fd, short revents, void *data);

bool sway_init(struct loop *loop) {
  if (connected)
    return true;
  char *sock = NULL;
  const char *env = getenv("SWAYSOCK");
  if (!env || !*env)
    env = getenv("I3SOCK");
  sock = env && *env ? xstrdup(env) : socket_from_sway();
  if (!sock) {
    warn("cannot find the sway socket (SWAYSOCK unset); is sway running?");
    return false;
  }
  i3ipc_set_nopanic(true);
  /* The patched i3ipc never frees a caller-supplied path. */
  if (i3ipc_init_try(sock)) {
    i3ipc_error_print("swell: cannot connect to sway");
    free(sock);
    return false;
  }
  free(sock);
  /* i3ipc opens its sockets without SOCK_CLOEXEC; keep them out of the
   * commands we spawn. */
  fcntl(i3ipc_message_fd(), F_SETFD, FD_CLOEXEC);
  fcntl(i3ipc_event_fd(), F_SETFD, FD_CLOEXEC);
  if (loop) {
    int events[] = {I3IPC_EVENT_WORKSPACE, I3IPC_EVENT_WINDOW,
                    I3IPC_EVENT_OUTPUT};
    i3ipc_subscribe(events, ARRAY_LEN(events));
    if (i3ipc_error_code()) {
      i3ipc_error_print("swell: sway subscribe failed");
      return false;
    }
    watch = loop_add_fd(loop, i3ipc_event_fd(), POLLIN, on_events, loop);
  }
  connected = true;
  return true;
}

void sway_finish(void) {
  loop_remove_fd(watch);
  watch = NULL;
  free(ws_cache);
  ws_cache = NULL;
  ws_count = 0;
  free(subs);
  subs = NULL;
  nsubs = 0;
  connected = false;
}

bool sway_connected(void) { return connected; }

void sway_subscribe(sway_event_fn fn, void *data) {
  subs = xrealloc(subs, (nsubs + 1) * sizeof(*subs));
  subs[nsubs++] = (struct sub){fn, data, false};
}

static void sweep(void) {
  int j = 0;
  for (int i = 0; i < nsubs; ++i)
    if (!subs[i].dead)
      subs[j++] = subs[i];
  nsubs = j;
}

void sway_unsubscribe(sway_event_fn fn, void *data) {
  for (int i = 0; i < nsubs; ++i)
    if (subs[i].fn == fn && subs[i].data == data)
      subs[i].dead = true;
  if (depth == 0)
    sweep();
}

/* ---- workspaces cache ---- */

static int leading_num(const char *s) {
  if (!isdigit((unsigned char)*s))
    return -1;
  return atoi(s);
}

int sway_workspaces(const struct wm_workspace **out) {
  if (ws_stale && connected) {
    I3ipc_reply_workspaces *r = i3ipc_get_workspaces();
    if (r) {
      ws_cache = xrealloc(ws_cache, (r->workspaces_size + 1) * sizeof(*ws_cache));
      ws_count = r->workspaces_size;
      for (int i = 0; i < ws_count; ++i) {
        I3ipc_reply_workspaces_el *e = &r->workspaces[i];
        struct wm_workspace *w = &ws_cache[i];
        snprintf(w->name, sizeof(w->name), "%.*s", e->name_size, e->name);
        w->truncated = e->name_size >= (int)sizeof(w->name);
        snprintf(w->output, sizeof(w->output), "%.*s", e->output_size,
                 e->output ? e->output : "");
        w->num = leading_num(w->name);
        w->focused = e->focused;
        w->visible = e->visible;
        w->urgent = e->urgent;
      }
      free(r);
      ws_stale = false;
    } else {
      i3ipc_error_print("swell: get_workspaces");
      i3ipc_error_reinitialize(false);
    }
  }
  *out = ws_cache;
  return ws_count;
}

void sway_workspaces_invalidate(void) { ws_stale = true; }

int sway_command(const char *fmt, ...) {
  char *cmd;
  va_list ap;
  va_start(ap, fmt);
  if (vasprintf(&cmd, fmt, ap) < 0)
    die("out of memory");
  va_end(ap);
  int ret = 0;
  I3ipc_reply_command *r = i3ipc_run_command(cmd);
  if (!r) {
    i3ipc_error_print("swell: sway command failed");
    i3ipc_error_reinitialize(false);
    ret = -1;
  } else {
    for (int i = 0; i < r->commands_size; ++i) {
      if (!r->commands[i].success) {
        warn("sway: '%s': %.*s", cmd, r->commands[i].error_size,
             r->commands[i].error ? r->commands[i].error : "failed");
        ret = -1;
      }
    }
    free(r);
  }
  free(cmd);
  ws_stale = true;
  return ret;
}

/* ---- focused workspace emptiness ---- */

static bool node_empty(const I3ipc_node *ws) {
  return ws->nodes_size == 0 && ws->floating_nodes_size == 0;
}

static const I3ipc_node *find_workspace(const I3ipc_node *n, const char *name) {
  if (n->type_enum == I3IPC_NODE_TYPE_WORKSPACE && n->name &&
      strcmp(n->name, name) == 0)
    return n;
  for (int i = 0; i < n->nodes_size; ++i) {
    const I3ipc_node *r = find_workspace(&n->nodes[i], name);
    if (r)
      return r;
  }
  return NULL;
}

static void set_focused_empty(bool empty, const char *ws_name) {
  int want = empty ? EMPTY_YES : EMPTY_NO;
  if (want == focused_empty)
    return;
  focused_empty = want;
  char *env = xasprintf("SWELL_WORKSPACE=%s", ws_name ? ws_name : "");
  bus_emit(empty ? "workspace.empty" : "workspace.occupied", env, NULL);
  free(env);
}

/* Re-query which workspace is focused and whether it has windows. */
static void sync_focused_empty(void) {
  sway_workspaces_invalidate();
  const struct wm_workspace *ws;
  int n = sway_workspaces(&ws);
  const char *focused = NULL;
  for (int i = 0; i < n; ++i)
    if (ws[i].focused)
      focused = ws[i].name;
  if (!focused)
    return;
  I3ipc_reply_tree *tree = i3ipc_get_tree();
  if (!tree) {
    i3ipc_error_reinitialize(false);
    return;
  }
  const I3ipc_node *node = find_workspace(&tree->root, focused);
  if (node)
    set_focused_empty(node_empty(node), focused);
  free(tree);
}

int sway_focused_empty(void) {
  if (!connected || focused_empty == EMPTY_UNKNOWN)
    return -1;
  return focused_empty == EMPTY_YES;
}

void sway_emit_focused_state(void) {
  if (!connected)
    return;
  focused_empty = EMPTY_UNKNOWN;
  sync_focused_empty();
}

/* ---- events ---- */

static const char *const ws_change_names[] = {
    [I3IPC_WORKSPACE_CHANGE_FOCUS] = "focus",
    [I3IPC_WORKSPACE_CHANGE_INIT] = "init",
    [I3IPC_WORKSPACE_CHANGE_EMPTY] = "removed",
    [I3IPC_WORKSPACE_CHANGE_URGENT] = "urgent",
    [I3IPC_WORKSPACE_CHANGE_RELOAD] = "reload",
    [I3IPC_WORKSPACE_CHANGE_RENAME] = "rename",
    [I3IPC_WORKSPACE_CHANGE_RESTORED] = "restored",
    [I3IPC_WORKSPACE_CHANGE_MOVE] = "move",
};

static const char *const win_change_names[] = {
    [I3IPC_WINDOW_CHANGE_NEW] = "new",
    [I3IPC_WINDOW_CHANGE_CLOSE] = "close",
    [I3IPC_WINDOW_CHANGE_FOCUS] = "focus",
    [I3IPC_WINDOW_CHANGE_TITLE] = "title",
    [I3IPC_WINDOW_CHANGE_FULLSCREEN_MODE] = "fullscreen",
    [I3IPC_WINDOW_CHANGE_MOVE] = "move",
    [I3IPC_WINDOW_CHANGE_FLOATING] = "floating",
    [I3IPC_WINDOW_CHANGE_URGENT] = "urgent",
    [I3IPC_WINDOW_CHANGE_MARK] = "mark",
};

static void publish(const I3ipc_event *ev) {
  char name[64];
  if (ev->type == I3IPC_EVENT_WORKSPACE) {
    int c = ev->workspace.change_enum;
    const char *what = c >= 0 && c < (int)ARRAY_LEN(ws_change_names) &&
                               ws_change_names[c]
                           ? ws_change_names[c]
                           : "other";
    snprintf(name, sizeof(name), "workspace.%s", what);
    const I3ipc_node *cur = ev->workspace.current;
    char *env = xasprintf("SWELL_WORKSPACE=%s",
                          cur && cur->name ? cur->name : "");
    bus_emit(name, env, NULL);
    free(env);
    if (c == I3IPC_WORKSPACE_CHANGE_FOCUS && cur)
      set_focused_empty(node_empty(cur), cur->name);
  } else if (ev->type == I3IPC_EVENT_WINDOW) {
    int c = ev->window.change_enum;
    const char *what = c >= 0 && c < (int)ARRAY_LEN(win_change_names) &&
                               win_change_names[c]
                           ? win_change_names[c]
                           : "other";
    snprintf(name, sizeof(name), "window.%s", what);
    const I3ipc_node *n = &ev->window.container;
    const char *app_id = n->app_id;
    int app_len = n->app_id_size;
    if (!app_id && n->window_properties &&
        n->window_properties->window_class) {
      app_id = n->window_properties->window_class;
      app_len = n->window_properties->window_class_size;
    }
    char *e1 = xasprintf("SWELL_CON_ID=%zu", (size_t)n->id);
    char *e2 = xasprintf("SWELL_APP_ID=%.*s", app_id ? app_len : 0,
                         app_id ? app_id : "");
    char *e3 = xasprintf("SWELL_TITLE=%.*s", n->name ? n->name_size : 0,
                         n->name ? n->name : "");
    bus_emit(name, e1, e2, e3, NULL);
    free(e1), free(e2), free(e3);
    if (c == I3IPC_WINDOW_CHANGE_NEW || c == I3IPC_WINDOW_CHANGE_CLOSE ||
        c == I3IPC_WINDOW_CHANGE_MOVE)
      sync_focused_empty();
  } else if (ev->type == I3IPC_EVENT_OUTPUT) {
    bus_emit("output.change", NULL);
  }
}

static void on_events(int fd, short revents, void *data) {
  struct loop *loop = data;
  if (revents & (POLLERR | POLLHUP)) {
    warn("lost the sway IPC connection");
    loop_remove_fd(watch);
    watch = NULL;
    loop_quit(loop);
    return;
  }
  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
    I3ipc_event *ev = i3ipc_event_next(0);
    if (!ev) {
      if (i3ipc_error_code()) {
        i3ipc_error_print("swell: sway event");
        warn("lost the sway IPC connection");
        loop_remove_fd(watch);
        watch = NULL;
        loop_quit(loop);
      }
      return;
    }
    if (ev->type == I3IPC_EVENT_WORKSPACE)
      ws_stale = true;
    ++depth;
    int n = nsubs;
    for (int i = 0; i < n; ++i)
      if (!subs[i].dead)
        subs[i].fn(ev, subs[i].data);
    if (--depth == 0)
      sweep();
    publish(ev);
    free(ev);
  }
}
