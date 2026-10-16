/* The daemon's niri IPC connection ($NIRI_SOCKET: one JSON request per line,
 * one JSON reply per line).
 *
 * One socket is switched to niri's event stream. niri's events carry full
 * state, so this keeps its own model of workspaces and windows and never
 * queries. It publishes the same bus events as sway.h (workspace.*,
 * workspace.empty/occupied, window.*) plus overview.open / overview.close
 * for niri's own overview. SWELL_WORKSPACE is the workspace's name, or its
 * index on its output when it has none; SWELL_CON_ID is niri's window id.
 *
 * Actions use a fresh short-lived connection each. */
#ifndef SWELL_NIRI_H
#define SWELL_NIRI_H

#include <stdbool.h>

#include "loop.h"
#include "wm.h"

/* Connect to $NIRI_SOCKET. With a loop, subscribe to events and wait (up
 * to a second) for the initial workspaces and windows so the model is
 * ready on return. With loop NULL (one-shot clients) only actions work. */
bool niri_init(struct loop *loop);
void niri_finish(void);

/* See wm.h. */
int niri_workspaces(const struct wm_workspace **out);
int niri_focused_empty(void);
void niri_emit_focused_state(void);

/* Send one request, e.g. {"Action":{"FocusWorkspaceDown":{}}}. Returns 0
 * if niri answered Ok, otherwise warns and returns -1. */
int niri_request(const char *json);

/* Apply one event-stream line to the model and publish its bus events.
 * Unknown events and malformed lines are ignored. Exposed for tests. */
void niri_handle_line(const char *line);
/* Drop the model (tests). */
void niri_reset(void);

#endif
