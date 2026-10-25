/* The labwc backend's model: window-to-workspace guessing, the dynamic
 * workspace list over labwc's fixed pool, bus events and next/prev/goto
 * targets. Fed the model calls the protocol listeners make (no Wayland). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "bus.h"
#include "labwc.h"

static char log_buf[8192];

static const char *env_get(char *const *env, const char *key) {
  size_t n = strlen(key);
  for (; env && *env; ++env)
    if (strncmp(*env, key, n) == 0 && (*env)[n] == '=')
      return *env + n + 1;
  return "";
}

/* "name(detail) " per event; detail = list number or window key. */
static void on_bus(const struct bus_event *ev, void *data) {
  const char *d = strncmp(ev->name, "window.", 7) == 0
                      ? env_get(ev->env, "SWELL_CON_ID")
                      : env_get(ev->env, "SWELL_WORKSPACE");
  size_t len = strlen(log_buf);
  snprintf(log_buf + len, sizeof(log_buf) - len, "%s(%s) ", ev->name, d);
}

static void expect(const char *want) {
  if (strcmp(log_buf, want) != 0) {
    fprintf(stderr, "events:\n  got  '%s'\n  want '%s'\n", log_buf, want);
    assert(0);
  }
  log_buf[0] = '\0';
}

/* The list as "1 2* 3", * = focused. Names are list positions, whichever
 * pool workspaces they are; targets name the pool keys. */
static void check_list(const char *want) {
  const struct wm_workspace *ws;
  int n = labwc_workspaces(&ws);
  char got[256] = "";
  for (int i = 0; i < n; ++i) {
    size_t len = strlen(got);
    snprintf(got + len, sizeof(got) - len, "%s%.8s%s", i ? " " : "", ws[i].name,
             ws[i].focused ? "*" : "");
    assert(ws[i].num == i + 1);
  }
  if (strcmp(got, want) != 0) {
    fprintf(stderr, "workspaces:\n  got  '%s'\n  want '%s'\n", got, want);
    assert(0);
  }
}

/* Workspaces are keys 101..10N, windows 1.. */
static void activate(int ws, int pool) {
  for (int i = 1; i <= pool; ++i)
    labwc_model_ws_state(100 + i, i == ws, false);
}

static void target(const char *cmd, int num, unsigned want,
                   const char *want_why) {
  const char *why;
  unsigned got = labwc_model_target(cmd, num, &why);
  if (got != want || (want_why ? !why || !strstr(why, want_why) : !!why)) {
    fprintf(stderr, "%s %d: got %u (%s), want %u (%s)\n", cmd, num, got,
            why ? why : "-", want, want_why ? want_why : "-");
    assert(0);
  }
}

int main(void) {
  bus_subscribe(on_bus, NULL);
  char name[32];

  /* Pool of 4, workspace 1 active, one window already open (unknown
   * workspace: it isn't focused). */
  for (int i = 1; i <= 4; ++i) {
    snprintf(name, sizeof(name), "Workspace %d", i);
    labwc_model_ws(100 + i, name);
  }
  activate(1, 4);
  labwc_model_win(1, "old", "zenity", false, false);
  labwc_model_initial_done();
  labwc_model_settle();
  expect("workspace.init(1) workspace.focus(1) workspace.empty(1) ");
  check_list("1*");
  assert(labwc_focused_empty() == 1);
  /* Empty and last: nowhere new to go. */
  target("next", 0, 0, NULL);
  target("prev", 0, 0, NULL);

  /* A window opens on 1. */
  labwc_model_win(2, "a", "zenity", true, false);
  expect("window.new(2) window.focus(2) ");
  labwc_model_settle();
  expect("workspace.occupied(1) ");
  assert(labwc_focused_empty() == 0);
  /* Occupied and last: next opens a fresh one, the first after it. */
  target("next", 0, 102, NULL);
  target("goto", 5, 102, NULL);
  target("goto", 1, 101, NULL);

  /* Switch to 2. labwc sends the focus change before the workspace state
   * here; settling once counts it as one step. Window 2 loses focus, the
   * unknown old window stays unknown. */
  labwc_model_win(2, "a", "zenity", false, false);
  activate(2, 4);
  labwc_model_settle();
  expect("workspace.init(2) workspace.focus(2) workspace.empty(2) ");
  check_list("1 2*");
  target("next", 0, 0, NULL);
  target("prev", 0, 101, NULL);

  /* The old window gets focus: labwc switched to its workspace (3). It is
   * focused before the workspace state arrives; it still lands on 3. */
  labwc_model_win(1, "old", "zenity", true, false);
  activate(3, 4);
  expect("window.focus(1) ");
  labwc_model_settle();
  /* 2 was empty and is left: it drops out, 3 joins in its place. */
  expect("workspace.init(2) workspace.focus(2) workspace.removed(2) "
         "workspace.occupied(2) ");
  check_list("1 2*");
  /* Last and occupied: the fresh one is 4 (after 3). */
  target("next", 0, 104, NULL);

  /* Go to 4, open a window there: the pool is full. */
  labwc_model_win(1, "old", "zenity", false, false);
  activate(4, 4);
  labwc_model_win(3, "b", "zenity", true, false);
  labwc_model_settle();
  expect("window.new(3) window.focus(3) workspace.init(3) "
         "workspace.focus(3) ");
  check_list("1 2 3*");
  target("next", 0, 102, NULL); /* the gap */
  labwc_model_win(4, "c", "zenity", false, false);
  labwc_model_settle(); /* opened in the background: still on 4 */
  log_buf[0] = '\0';

  /* Fill 2 too, then nothing is fresh. (labwc unfocuses the windows of
   * the workspace it leaves.) */
  labwc_model_win(3, "b", "zenity", false, false);
  activate(2, 4);
  labwc_model_win(5, "d", "zenity", true, false);
  labwc_model_settle();
  log_buf[0] = '\0';
  check_list("1 2* 3 4");
  labwc_model_win(5, "d", "zenity", false, false);
  activate(4, 4);
  labwc_model_settle();
  log_buf[0] = '\0';
  target("next", 0, 0, "in use");
  target("goto", 9, 0, "in use");

  /* Minimizing every window on the focused workspace: the wallpaper sees
   * it empty, but it stays in the list. */
  labwc_model_win(3, "b", "zenity", false, true);
  labwc_model_win(4, "c", "zenity", false, true);
  labwc_model_settle();
  expect("workspace.empty(4) ");
  check_list("1 2 3 4*");
  labwc_model_win(4, "c", "zenity", true, false);
  labwc_model_settle();
  expect("window.focus(4) workspace.occupied(4) ");

  /* Closing the last windows on 4 and leaving it drops it. */
  labwc_model_win_closed(3);
  labwc_model_win_closed(4);
  expect("window.close(3) window.close(4) ");
  labwc_model_settle();
  expect("workspace.empty(4) ");
  activate(1, 4);
  labwc_model_settle();
  expect("workspace.focus(1) workspace.removed(4) workspace.occupied(1) ");
  check_list("1* 2 3");

  /* A retitle is a window.title, not a focus. */
  labwc_model_win(2, "a2", "zenity", false, false);
  expect("window.title(2) ");

  /* A workspace removed from rc.xml: its windows go to the first one. */
  labwc_model_ws_removed(102);
  labwc_model_settle();
  expect("workspace.removed(2) ");
  check_list("1* 2");

  /* Re-emitting the state for a new subscriber. */
  labwc_emit_focused_state();
  expect("workspace.occupied(1) ");

  /* SendToDesktop with follow: window 2 stays focused while labwc switches
   * to 4 (empty), so it moved there; 1 empties out (window 5 went to 1 when
   * 2 was removed, so it stays). */
  labwc_model_win(2, "a2", "zenity", true, false);
  labwc_model_settle();
  log_buf[0] = '\0';
  activate(4, 4);
  labwc_model_settle();
  expect("workspace.init(3) workspace.focus(3) ");
  check_list("1 2 3*");
  /* Switching away normally unfocuses it: it stays on 4. */
  labwc_model_win(2, "a2", "zenity", false, false);
  activate(1, 4);
  labwc_model_settle();
  expect("workspace.focus(1) ");
  check_list("1* 2 3");

  labwc_model_reset();

  /* Pairing with ext-foreign-toplevel-list entries: by app_id and title,
   * the oldest window first; an entry may come before its window. */
  labwc_model_ws(101, "1");
  labwc_model_ws_state(101, true, false);
  labwc_model_initial_done();
  labwc_model_ext(50, "id-a", "term", "foot");
  labwc_model_win(1, "term", "foot", false, false);
  labwc_model_win(2, "term", "foot", true, false);
  labwc_model_win(3, "web", "firefox", false, false);
  labwc_model_ext(51, "id-b", "term", "foot");
  labwc_model_ext(52, "id-c", "web", "firefox");
  labwc_model_settle();
  const char *want[] = {NULL, "id-a", "id-b", "id-c"};
  assert(labwc_window_count() == 3);
  for (int i = 0; i < 3; ++i) {
    struct labwc_window w;
    labwc_window_get(i, &w);
    assert(w.ws == 101);
    assert(w.ident && strcmp(w.ident, want[w.key]) == 0);
  }
  /* Window 1 closes: its entry goes too; a new one pairs with the new
   * window, not with the one still paired. */
  labwc_model_win_closed(1);
  labwc_model_ext_closed(50);
  labwc_model_win(4, "term", "foot", false, false);
  labwc_model_ext(53, "id-d", "term", "foot");
  for (int i = 0; i < labwc_window_count(); ++i) {
    struct labwc_window w;
    labwc_window_get(i, &w);
    if (w.key == 2)
      assert(strcmp(w.ident, "id-b") == 0);
    if (w.key == 4)
      assert(strcmp(w.ident, "id-d") == 0);
  }
  assert(labwc_workspace_key(0) == 101 && labwc_workspace_key(1) == 0);
  /* Settling is a change too (the overview reloads on the serial). */
  uint64_t before = labwc_serial();
  labwc_model_settle();
  assert(labwc_serial() != before);
  labwc_model_reset();
  return 0;
}
