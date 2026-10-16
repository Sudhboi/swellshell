/* The window manager behind swell: sway (sway.h) or niri (niri.h), picked
 * once by wm_init() from the environment. Modules that only need workspaces
 * and the focused workspace's emptiness use these calls and work on both;
 * anything sway-specific (the overview, i3ipc queries) stays in sway.h.
 *
 * Both backends publish the same bus events (see sway.h for the list):
 * workspace.*, workspace.empty/occupied, window.*. niri adds overview.open /
 * overview.close for its own overview and has no output.change. */
#ifndef SWELL_WM_H
#define SWELL_WM_H

#include <stdbool.h>

#include "loop.h"

enum wm_kind { WM_NONE, WM_SWAY, WM_NIRI };

struct wm_workspace {
  char name[256];
  /* sway: the name's leading number, -1 if none; niri: the 1-based index
   * on its output. */
  int num;
  bool focused, visible, urgent;
  bool truncated; /* the name didn't fit in name[] */
  char output[64];
};

/* Connect: niri if $NIRI_SOCKET is set, otherwise sway. With loop NULL no
 * events are read (one-shot clients). Returns WM_NONE if neither works. */
enum wm_kind wm_init(struct loop *loop);
void wm_finish(void);
enum wm_kind wm_kind(void);
/* "sway", "niri" or "no". */
const char *wm_name(enum wm_kind k);

/* Workspaces in the compositor's order (niri: by output, then index; the
 * empty workspace niri keeps at the end of each output is left out unless
 * focused). Returns the count; *out stays valid until the next call. */
int wm_workspaces(const struct wm_workspace **out);
/* Force the next wm_workspaces() call to re-query (no-op on niri, whose
 * events carry the full state). */
void wm_workspaces_invalidate(void);
/* The workspaces a one-row strip (popup, wsosd) shows: all of them on sway,
 * where numbers are global; the focused output's on niri, where indices
 * restart on every output. A slice of wm_workspaces(). */
int wm_strip_workspaces(const struct wm_workspace **out);

/* The last reported state of the focused workspace: 1 empty, 0 occupied,
 * -1 unknown. */
int wm_focused_empty(void);
/* Publish workspace.empty/occupied now, even if unchanged. */
void wm_emit_focused_state(void);

#endif
