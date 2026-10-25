/* The labwc backend. labwc has no IPC socket; this reads ext-workspace-v1
 * (a fixed pool of global workspaces from rc.xml <desktops>, activate only)
 * and wlr-foreign-toplevel-management (windows, but not their workspace)
 * on the daemon's Wayland connection.
 *
 * Which workspace a window is on is guessed: a window belongs to the active
 * workspace when it opens and whenever it gains focus (labwc switches to a
 * window's workspace to focus it). Windows already open when swell starts
 * belong nowhere until focused. A window that stays focused across a
 * switch went along (SendToDesktop with follow); one sent away without
 * follow stays where it was until it is focused again.
 *
 * Dynamic workspaces on top of the pool: the list (wm_workspaces) holds the
 * occupied workspaces plus the focused one, named and numbered 1..n in
 * labwc's order, and `next` past the last occupied one opens a fresh empty
 * one. workspace.init / workspace.removed are sent when one joins or leaves
 * the list. Bus events otherwise match sway.h; SWELL_WORKSPACE is the
 * workspace's number in the list, SWELL_CON_ID a per-daemon window id.
 *
 * Workspace changes are settled SETTLE_MS after the last event, so that a
 * switch and the focus change it causes count as one step whatever order
 * labwc sends them in. */
#ifndef SWELL_LABWC_H
#define SWELL_LABWC_H

#include <stdbool.h>
#include <stdint.h>

#include "loop.h"
#include "wm.h"

struct swell_wl;

/* Bind the protocols on wl and read the initial state. With loop NULL (or
 * no wl) nothing is bound: one-shot clients only learn that it's labwc. */
bool labwc_init(struct loop *loop, struct swell_wl *wl);
void labwc_finish(void);
/* $LABWC_PID is set, or $XDG_CURRENT_DESKTOP names labwc. */
bool labwc_detect(void);

/* See wm.h. */
int labwc_workspaces(const struct wm_workspace **out);
int labwc_focused_empty(void);
void labwc_emit_focused_state(void);

/* `swell ws next|prev|goto N`. Returns 0 if a workspace was activated or
 * there was nothing to do (*what says why, or NULL), -1 on failure (*what
 * is the message). */
int labwc_go(const char *cmd, int num, const char **what);

/* ---- for the overview ---- */

/* Changes whenever anything in the model may have changed. */
uint64_t labwc_serial(void);

struct labwc_window {
  uint32_t key; /* SWELL_CON_ID */
  uint32_t ws;  /* labwc_workspace_key's, 0 if not known */
  const char *title, *app_id;
  const char *ident; /* ext-foreign-toplevel identifier, NULL if unpaired */
  bool activated, minimized;
};
/* Windows in no particular order; the strings live until the next change. */
int labwc_window_count(void);
void labwc_window_get(int i, struct labwc_window *out);
/* The key of the workspace at index i of labwc_workspaces(), 0 if none. */
uint32_t labwc_workspace_key(int i);

void labwc_focus_window(uint32_t key); /* labwc switches workspace too */
void labwc_close_window(uint32_t key);
/* key 0: a fresh empty workspace, as `swell ws next` past the end. */
void labwc_focus_workspace(uint32_t key);

/* ---- the model, without Wayland (tests) ----
 * Workspaces and windows are named by caller-chosen keys (nonzero). */
void labwc_model_ws(uint32_t key, const char *name);
void labwc_model_ws_state(uint32_t key, bool active, bool urgent);
void labwc_model_ws_removed(uint32_t key);
void labwc_model_win(uint32_t key, const char *title, const char *app_id,
                     bool activated, bool minimized);
void labwc_model_win_closed(uint32_t key);
/* An ext-foreign-toplevel-list entry (paired with a window by app_id and
 * title). */
void labwc_model_ext(uint32_t key, const char *ident, const char *title,
                     const char *app_id);
void labwc_model_ext_closed(uint32_t key);
/* End of the initial state: windows seen so far were already open. */
void labwc_model_initial_done(void);
/* Apply pending changes and send the workspace events (the daemon does
 * this on a timer). */
void labwc_model_settle(void);
/* What labwc_go would activate: a workspace key, or 0 (*why says why). */
uint32_t labwc_model_target(const char *cmd, int num, const char **why);
void labwc_model_reset(void);

#endif
