/* workspaces: GNOME/niri-style dynamic workspaces (formerly dynwork).
 *
 * Workspaces are always numbered 1..n in order: whenever they change (an
 * overview reorder, the last window closing, `swaymsg workspace ...`) they
 * are renamed back to 1..n after a short quiet period. `next` past the last
 * workspace creates a new one instead of wrapping around. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bus.h"
#include "module.h"
#include "plan.h"
#include "sway.h"
#include "util.h"

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
  const struct sway_workspace *list;
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
  if (ws.cfg.renumber)
    renumber();
}

/* The focused workspace's number (they are numbered after renumber()), or
 * -1 if none is focused. */
static int focused_number(int *count) {
  sway_workspaces_invalidate();
  const struct sway_workspace *list;
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

static int dispatch(int argc, char **argv, struct reply *r) {
  if (argc < 1) {
    reply_printf(r, "usage: swell ws next|prev|goto N|reorder\n");
    return 1;
  }
  const char *cmd = argv[0];
  if (strcmp(cmd, "next") == 0 && argc == 1)
    go_next();
  else if (strcmp(cmd, "prev") == 0 && argc == 1)
    go_prev();
  else if (strcmp(cmd, "reorder") == 0 && argc == 1)
    renumber();
  else if (strcmp(cmd, "goto") == 0) {
    int num = argc == 2 ? ws_parse_number(argv[1]) : 0;
    if (num <= 0) {
      reply_printf(r, "usage: swell ws goto N (N a workspace number >= 1)\n");
      return 1;
    }
    go_to(num);
  } else {
    reply_printf(r, "unknown workspace command '%s' (expected next, prev, "
                    "goto N or reorder)\n",
                 cmd);
    return 1;
  }
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
  sway_subscribe(on_sway, NULL);
  maybe_renumber();
  return true;
}

static void reconfigure(const void *cfg) {
  ws.cfg = *(const struct workspaces_config *)cfg;
  maybe_renumber();
}

static void finish(void) {
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
  if (!sway_init(NULL))
    return 1;
  sw->sway = true;
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
             "  reorder   renumber the workspaces 1..n now\n",
    .needs_sway = true,
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
