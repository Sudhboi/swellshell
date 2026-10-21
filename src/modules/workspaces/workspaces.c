/* workspaces: GNOME/niri-style dynamic workspaces (formerly dynwork).
 *
 * On sway, workspaces are always numbered 1..n in order: whenever they
 * change (an overview reorder, the last window closing, `swaymsg workspace
 * ...`) they are renamed back to 1..n after a short quiet period. `next`
 * past the last workspace creates a new one instead of wrapping around.
 *
 * niri already works this way, so there the commands map onto its actions
 * and nothing is renumbered. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bus.h"
#include "module.h"
#include "niri.h"
#include "plan.h"
#include "sway.h"
#include "util.h"
#include "wsswitch.h"

struct workspaces_config {
  bool renumber;
  int debounce_ms;
};

static const struct workspaces_config defaults = {
    .renumber = true,
    .debounce_ms = 30,
};

static const struct cfg_field fields[] = {
    CFG_BOOL(struct workspaces_config, renumber,
             "keep workspaces numbered 1..n, renaming them after every change"),
    CFG_INT(struct workspaces_config, debounce_ms, 0, 5000,
            "wait this long after the last workspace event before renumbering"),
};

static struct {
  struct swell *sw;
  struct workspaces_config cfg;
  struct loop_timer *debounce;
} ws;

/* Rename the workspaces to 1..n in order. Names that are already right are
 * left alone, so running this in response to our own rename events sends
 * nothing. */
static void renumber(void) {
  sway_workspaces_invalidate();
  const struct wm_workspace *list;
  int n = sway_workspaces(&list);
  if (n <= 0)
    return;
  const char **names = xcalloc(n, sizeof(*names));
  for (int i = 0; i < n; ++i) {
    if (list[i].truncated) {
      warn("workspaces: a workspace name is too long to rename; not "
           "renumbering");
      free(names);
      return;
    }
    names[i] = list[i].name;
  }
  char *cmd = ws_plan_commands(names, n);
  free(names);
  if (cmd) {
    sway_command("%s", cmd);
    free(cmd);
    bus_emit("workspaces.renumber", NULL);
  }
}

static void maybe_renumber(void) {
  if (ws.cfg.renumber && ws.sw->wm == WM_SWAY)
    renumber();
}

/* The focused workspace's number (they are numbered after renumber()), or
 * -1 if none is focused. */
static int focused_number(int *count) {
  sway_workspaces_invalidate();
  const struct wm_workspace *list;
  int n = sway_workspaces(&list);
  if (count)
    *count = n;
  for (int i = 0; i < n; ++i)
    if (list[i].focused)
      return atoi(list[i].name);
  return -1;
}

static void go_next(void) {
  int count;
  focused_number(&count);
  maybe_renumber();
  if (focused_number(NULL) == count)
    sway_command("workspace %d", count + 1);
  else
    sway_command("workspace next");
  maybe_renumber();
}

static void go_prev(void) {
  maybe_renumber();
  if (focused_number(NULL) > 1)
    sway_command("workspace prev");
  maybe_renumber();
}

static void go_to(int num) {
  sway_command("workspace %d", num);
  maybe_renumber();
}

static void run_next(void *data) { go_next(); }
static void run_prev(void *data) { go_prev(); }
static void run_goto(void *data) { go_to((int)(intptr_t)data); }

/* Switch through whoever animates it (wsswitch.h). dir is where the target
 * lies from the focused workspace: +1 later, -1 earlier, 0 nowhere. */
static void animated(int dir, wsswitch_go_fn go, void *data) {
  sway_workspaces_invalidate();
  const struct wm_workspace *list;
  int n = sway_workspaces(&list);
  char output[sizeof list->output] = "";
  for (int i = 0; i < n; ++i)
    if (list[i].focused)
      copy_str(output, sizeof output, list[i].output);
  wsswitch_run(dir, output, go, data);
}

static void switch_sway(const char *cmd, int num) {
  if (strcmp(cmd, "next") == 0) {
    /* next from an empty last workspace only swaps it for another empty
     * one: nothing to see, so not animated. */
    int count;
    maybe_renumber();
    bool fresh = focused_number(&count) == count && sway_focused_empty() == 1;
    animated(fresh ? 0 : 1, run_next, NULL);
  } else if (strcmp(cmd, "prev") == 0) {
    maybe_renumber();
    animated(focused_number(NULL) > 1 ? -1 : 0, run_prev, NULL);
  } else {
    int cur = focused_number(NULL);
    animated((num > cur) - (num < cur), run_goto, (void *)(intptr_t)num);
  }
}

/* niri: its own focus actions do the right thing at either end. */
static int niri_go(const char *action) {
  char *req = xasprintf("{\"Action\":%s}", action);
  int ret = niri_request(req) == 0 ? 0 : 1;
  free(req);
  return ret;
}

static int dispatch_niri(const char *cmd, int num, struct reply *r) {
  if (strcmp(cmd, "next") == 0)
    return niri_go("{\"FocusWorkspaceDown\":{}}");
  if (strcmp(cmd, "prev") == 0)
    return niri_go("{\"FocusWorkspaceUp\":{}}");
  if (strcmp(cmd, "goto") == 0) {
    if (num > 255) {
      reply_printf(r, "niri has no workspace %d\n", num);
      return 1;
    }
    char *a = xasprintf(
        "{\"FocusWorkspace\":{\"reference\":{\"Index\":%d}}}", num);
    int ret = niri_go(a);
    free(a);
    return ret;
  }
  reply_printf(r, "niri numbers workspaces itself; nothing to reorder\n");
  return 0;
}

static int dispatch(int argc, char **argv, struct reply *r) {
  if (argc < 1) {
    reply_printf(r, "usage: swell ws next|prev|goto N|reorder\n");
    return 1;
  }
  const char *cmd = argv[0];
  bool known = argc == 1 && (strcmp(cmd, "next") == 0 ||
                             strcmp(cmd, "prev") == 0 ||
                             strcmp(cmd, "reorder") == 0);
  int num = 0;
  if (strcmp(cmd, "goto") == 0) {
    num = argc == 2 ? ws_parse_number(argv[1]) : 0;
    if (num <= 0) {
      reply_printf(r, "usage: swell ws goto N (N a workspace number >= 1)\n");
      return 1;
    }
    known = true;
  }
  if (!known) {
    reply_printf(r, "unknown workspace command '%s' (expected next, prev, "
                    "goto N or reorder)\n",
                 cmd);
    return 1;
  }
  if (ws.sw->wm == WM_NIRI)
    return dispatch_niri(cmd, num, r);
  if (strcmp(cmd, "reorder") == 0)
    renumber();
  else
    switch_sway(cmd, num);
  return 0;
}

/* ---- daemon ---- */

static void on_debounce(struct loop_timer *t, void *data) { maybe_renumber(); }

static void on_sway(const I3ipc_event *ev, void *data) {
  if (ev->type != I3IPC_EVENT_WORKSPACE || !ws.cfg.renumber)
    return;
  /* Wait for a quiet period so one action's burst (focus/empty/init)
   * results in a single renumber. */
  if (ws.cfg.debounce_ms > 0)
    loop_timer_arm(ws.debounce, ws.cfg.debounce_ms, 0);
  else
    renumber();
}

static bool init(struct swell *sw, const void *cfg) {
  ws.sw = sw;
  ws.cfg = *(const struct workspaces_config *)cfg;
  ws.debounce = loop_add_timer(sw->loop, CLOCK_MONOTONIC, on_debounce, NULL);
  if (sw->wm == WM_SWAY)
    sway_subscribe(on_sway, NULL);
  maybe_renumber();
  return true;
}

static void reconfigure(const void *cfg) {
  ws.cfg = *(const struct workspaces_config *)cfg;
  maybe_renumber();
}

static void finish(void) {
  if (ws.sw->wm == WM_SWAY)
    sway_unsubscribe(on_sway, NULL);
  loop_remove_timer(ws.debounce);
  ws.debounce = NULL;
}

static int command(int argc, char **argv, struct reply *r) {
  return dispatch(argc, argv, r);
}

/* Without a daemon, act directly (dynwork's standalone mode). */
static int run_local(struct swell *sw, const void *cfg, bool have_daemon,
                     int argc, char **argv) {
  if (have_daemon)
    return -1;
  sw->wm = wm_init(NULL);
  if (sw->wm == WM_NONE)
    return 1;
  ws.sw = sw;
  ws.cfg = *(const struct workspaces_config *)cfg;
  struct reply r = {0};
  int ret = dispatch(argc, argv, &r);
  if (r.len)
    fputs(r.buf, ret ? stderr : stdout);
  reply_free(&r);
  return ret;
}

static const char *const aliases[] = {"ws", NULL};

const struct swell_module workspaces_module = {
    .name = "workspaces",
    .aliases = aliases,
    .usage = "  next      go to the next workspace, creating one after the last\n"
             "  prev      go to the previous workspace (stops at 1)\n"
             "  goto N    go to workspace N\n"
             "  reorder   renumber the workspaces 1..n now (sway)\n",
    .needs_wm = true,
    .config_size = sizeof(struct workspaces_config),
    .defaults = &defaults,
    .fields = fields,
    .nfields = ARRAY_LEN(fields),
    .init = init,
    .reconfigure = reconfigure,
    .finish = finish,
    .command = command,
    .run_local = run_local,
};
