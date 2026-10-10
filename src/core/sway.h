/* The daemon's sway IPC connection (i3ipc-simple, which keeps one command
 * socket and one event socket as global state).
 *
 * Commands and queries (i3ipc_get_tree() etc.) may be used directly by any
 * module once sway_init() succeeded. Events are read here and fanned out to
 * subscribers, and also published on the bus:
 *   workspace.focus | workspace.init | workspace.removed | workspace.rename
 *   | workspace.move | workspace.urgent | workspace.reload
 *       SWELL_WORKSPACE=<name of the current workspace>
 *   workspace.empty / workspace.occupied
 *       the focused workspace lost its last window / gained its first one,
 *       or focus moved between an empty and a non-empty workspace. Only
 *       transitions are reported (plus once at startup). SWELL_WORKSPACE.
 *   window.new | window.close | window.focus | window.title | window.move
 *   | window.fullscreen | window.floating | window.urgent | window.mark
 *       SWELL_CON_ID, SWELL_APP_ID, SWELL_TITLE
 *   output.change */
#ifndef SWELL_SWAY_H
#define SWELL_SWAY_H

#include <stdbool.h>

#include "i3ipc.h"
#include "loop.h"

/* Connect ($SWAYSOCK, $I3SOCK, then `sway --get-socketpath`) and subscribe
 * to workspace/window/output events. With loop NULL no events are read
 * (one-shot clients). */
bool sway_init(struct loop *loop);
void sway_finish(void);
bool sway_connected(void);

typedef void (*sway_event_fn)(const I3ipc_event *ev, void *data);
void sway_subscribe(sway_event_fn fn, void *data);
void sway_unsubscribe(sway_event_fn fn, void *data);

struct sway_workspace {
  char name[256];
  int num; /* -1 if the name doesn't start with a number */
  bool focused, visible, urgent;
  bool truncated; /* the name didn't fit in name[] */
  char output[64];
};

/* Workspaces in sway's order. Cached, refreshed after workspace events.
 * Returns the count; *out stays valid until the next call. */
int sway_workspaces(const struct sway_workspace **out);
/* Force the next sway_workspaces() call to re-query. */
void sway_workspaces_invalidate(void);

/* Publish the focused workspace's state (workspace.empty/occupied) on the
 * bus now, even if it did not change since the last report. */
void sway_emit_focused_state(void);

/* The last reported state of the focused workspace: 1 empty, 0 occupied,
 * -1 unknown (not connected, or not queried yet). */
int sway_focused_empty(void);

/* Run a sway command (printf-style). Returns 0 if every command
 * succeeded, otherwise warns and returns -1. */
int sway_command(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
