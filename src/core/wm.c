#include "wm.h"

#include <stdlib.h>
#include <string.h>

#include "labwc.h"
#include "niri.h"
#include "sway.h"

static enum wm_kind kind = WM_NONE;

enum wm_kind wm_init(struct loop *loop, struct swell_wl *wl) {
  if (kind != WM_NONE)
    return kind;
  const char *niri = getenv("NIRI_SOCKET");
  if (niri && *niri && niri_init(loop))
    kind = WM_NIRI;
  else if (labwc_detect())
    kind = labwc_init(loop, wl) ? WM_LABWC : WM_NONE;
  else if (sway_init(loop))
    kind = WM_SWAY;
  return kind;
}

void wm_finish(void) {
  if (kind == WM_SWAY)
    sway_finish();
  else if (kind == WM_NIRI)
    niri_finish();
  else if (kind == WM_LABWC)
    labwc_finish();
  kind = WM_NONE;
}

enum wm_kind wm_kind(void) { return kind; }

const char *wm_name(enum wm_kind k) {
  return k == WM_SWAY    ? "sway"
         : k == WM_NIRI  ? "niri"
         : k == WM_LABWC ? "labwc"
                         : "no";
}

int wm_workspaces(const struct wm_workspace **out) {
  switch (kind) {
  case WM_SWAY:
    return sway_workspaces(out);
  case WM_NIRI:
    return niri_workspaces(out);
  case WM_LABWC:
    return labwc_workspaces(out);
  default:
    *out = NULL;
    return 0;
  }
}

int wm_strip_workspaces(const struct wm_workspace **out) {
  int n = wm_workspaces(out);
  if (kind != WM_NIRI)
    return n;
  const struct wm_workspace *ws = *out;
  int f = 0;
  while (f < n && !ws[f].focused)
    ++f;
  if (f == n)
    return n;
  /* Sorted by output, so the focused output's workspaces are contiguous. */
  int lo = f, hi = f + 1;
  while (lo > 0 && strcmp(ws[lo - 1].output, ws[f].output) == 0)
    --lo;
  while (hi < n && strcmp(ws[hi].output, ws[f].output) == 0)
    ++hi;
  *out = ws + lo;
  return hi - lo;
}

void wm_workspaces_invalidate(void) {
  if (kind == WM_SWAY)
    sway_workspaces_invalidate();
}

int wm_focused_empty(void) {
  switch (kind) {
  case WM_SWAY:
    return sway_focused_empty();
  case WM_NIRI:
    return niri_focused_empty();
  case WM_LABWC:
    return labwc_focused_empty();
  default:
    return -1;
  }
}

void wm_emit_focused_state(void) {
  if (kind == WM_SWAY)
    sway_emit_focused_state();
  else if (kind == WM_NIRI)
    niri_emit_focused_state();
  else if (kind == WM_LABWC)
    labwc_emit_focused_state();
}
