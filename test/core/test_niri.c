/* The niri backend's model and bus events, fed event-stream lines as niri
 * sends them (no socket). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "bus.h"
#include "niri.h"

static char log_buf[8192];

static const char *env_get(char *const *env, const char *key) {
  size_t n = strlen(key);
  for (; env && *env; ++env)
    if (strncmp(*env, key, n) == 0 && (*env)[n] == '=')
      return *env + n + 1;
  return "";
}

/* "name(detail) " per event; detail = workspace or window id. */
static void on_bus(const struct bus_event *ev, void *data) {
  const char *d = strncmp(ev->name, "window.", 7) == 0
                      ? env_get(ev->env, "SWELL_CON_ID")
                      : env_get(ev->env, "SWELL_WORKSPACE");
  size_t len = strlen(log_buf);
  snprintf(log_buf + len, sizeof(log_buf) - len, "%s(%s) ", ev->name, d);
}

static void feed(const char *line) {
  log_buf[0] = '\0';
  niri_handle_line(line);
}

static void expect(const char *want) {
  if (strcmp(log_buf, want) != 0) {
    fprintf(stderr, "events:\n  got  '%s'\n  want '%s'\n", log_buf, want);
    assert(0);
  }
}

#define WS(id, idx, name, out, active, focused)                                \
  "{\"id\":" #id ",\"idx\":" #idx ",\"name\":" name ",\"output\":\"" out      \
  "\",\"is_urgent\":false,\"is_active\":" #active ",\"is_focused\":" #focused \
  ",\"active_window_id\":null}"

#define WIN(id, ws, title, focused)                                            \
  "{\"id\":" #id ",\"title\":\"" title "\",\"app_id\":\"zenity\","             \
  "\"pid\":1,\"workspace_id\":" #ws ",\"is_focused\":" #focused               \
  ",\"is_floating\":false,\"is_urgent\":false,\"layout\":{},"                  \
  "\"focus_timestamp\":null}"

static void check_strip(const char *want) {
  const struct wm_workspace *ws;
  int n = niri_workspaces(&ws);
  char got[512] = "";
  for (int i = 0; i < n; ++i) {
    size_t len = strlen(got);
    snprintf(got + len, sizeof(got) - len, "%s%s:%s%s", i ? " " : "",
             ws[i].output, ws[i].name, ws[i].focused ? "*" : "");
  }
  if (strcmp(got, want) != 0) {
    fprintf(stderr, "workspaces:\n  got  '%s'\n  want '%s'\n", got, want);
    assert(0);
  }
}

int main(void) {
  bus_subscribe(on_bus, NULL);

  /* Initial state: two workspaces on DP-1 (2 is niri's trailing empty one),
   * one on HDMI-A-1. Nothing is known empty until windows arrive. */
  feed("{\"WorkspacesChanged\":{\"workspaces\":["
       WS(2, 2, "null", "DP-1", false, false) ","
       WS(1, 1, "null", "DP-1", true, true) ","
       WS(3, 1, "\"chat\"", "HDMI-A-1", true, false)
       "]}}");
  expect("workspace.init(2) workspace.init(1) workspace.init(chat) "
         "workspace.focus(1) ");
  assert(niri_focused_empty() == -1);
  check_strip("DP-1:1* HDMI-A-1:chat");

  feed("{\"WindowsChanged\":{\"windows\":[" WIN(10, 1, "A", true) "]}}");
  expect("workspace.occupied(1) "); /* no window.new for existing ones */
  assert(niri_focused_empty() == 0);

  /* Unknown events and garbage are ignored. */
  feed("{\"KeyboardLayoutsChanged\":{\"keyboard_layouts\":{\"names\":[],"
       "\"current_idx\":0}}}");
  expect("");
  feed("{\"Ok\":\"Handled\"}");
  expect("");
  feed("not json");
  expect("");
  feed("{\"WorkspaceActivated\":{\"id\":999,\"focused\":true}}");
  expect("");

  /* Switch to the trailing empty workspace: it shows while focused. */
  feed("{\"WorkspaceActivated\":{\"id\":2,\"focused\":true}}");
  expect("workspace.focus(2) workspace.empty(2) ");
  assert(niri_focused_empty() == 1);
  check_strip("DP-1:1 DP-1:2* HDMI-A-1:chat");

  /* A window opens there; niri adds a new trailing workspace. */
  feed("{\"WindowOpenedOrChanged\":{\"window\":" WIN(11, 2, "B", true) "}}");
  expect("window.new(11) window.focus(11) workspace.occupied(2) ");
  feed("{\"WorkspacesChanged\":{\"workspaces\":["
       WS(1, 1, "null", "DP-1", false, false) ","
       WS(2, 2, "null", "DP-1", true, true) ","
       WS(4, 3, "null", "DP-1", false, false) ","
       WS(3, 1, "\"chat\"", "HDMI-A-1", true, false)
       "]}}");
  expect("workspace.init(3) ");
  check_strip("DP-1:1 DP-1:2* HDMI-A-1:chat");

  /* Title change and focus back to A. */
  feed("{\"WindowOpenedOrChanged\":{\"window\":" WIN(11, 2, "B2", true) "}}");
  expect("window.title(11) ");
  feed("{\"WindowFocusChanged\":{\"id\":10}}");
  expect("window.focus(10) ");
  feed("{\"WindowFocusChanged\":{\"id\":null}}");
  expect("");

  /* B moves to workspace 1, leaving 2 empty while focused. */
  feed("{\"WindowOpenedOrChanged\":{\"window\":" WIN(11, 1, "B2", false) "}}");
  expect("window.move(11) workspace.empty(2) ");
  feed("{\"WindowClosed\":{\"id\":11}}");
  expect("window.close(11) "); /* still empty: no repeat */
  feed("{\"WindowClosed\":{\"id\":11}}");
  expect("");

  /* Focus the other output's workspace (occupied? no windows: empty). */
  feed("{\"WorkspaceActivated\":{\"id\":3,\"focused\":true}}");
  expect("workspace.focus(chat) ");
  check_strip("DP-1:1 DP-1:2 HDMI-A-1:chat*");

  /* niri removes 2 and renumbers 4 to idx 2; chat moves to DP-1. */
  feed("{\"WorkspacesChanged\":{\"workspaces\":["
       WS(1, 1, "null", "DP-1", true, false) ","
       WS(4, 2, "null", "DP-1", false, false) ","
       WS(3, 3, "\"chat\"", "DP-1", false, true)
       "]}}");
  expect("workspace.move(2) workspace.move(chat) workspace.removed(2) ");
  check_strip("DP-1:1 DP-1:2 DP-1:chat*");

  /* Re-emit on request. */
  log_buf[0] = '\0';
  niri_emit_focused_state();
  expect("workspace.empty(chat) ");

  feed("{\"WorkspaceUrgencyChanged\":{\"id\":1,\"urgent\":true}}");
  expect("workspace.urgent(1) ");

  feed("{\"OverviewOpenedOrClosed\":{\"is_open\":false}}");
  expect("");
  feed("{\"OverviewOpenedOrClosed\":{\"is_open\":true}}");
  expect("overview.open() ");
  feed("{\"OverviewOpenedOrClosed\":{\"is_open\":false}}");
  expect("overview.close() ");

  /* A later full window list reports differences. */
  feed("{\"WindowsChanged\":{\"windows\":[" WIN(12, 3, "C", true) "]}}");
  expect("window.new(12) window.close(10) workspace.occupied(chat) ");

  niri_reset();
  assert(niri_focused_empty() == -1);
  check_strip("");
  puts("ok");
  return 0;
}
