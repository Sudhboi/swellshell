/* Workspaces from the core's sway cache: sorted by num, named ones last by
 * name, at most WS_MAX, flags kept. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "popup/info.h"

static struct wm_workspace ws(const char *name, int num, bool focused,
                                bool urgent) {
  struct wm_workspace w = {.num = num, .focused = focused, .urgent = urgent};
  snprintf(w.name, sizeof w.name, "%s", name);
  return w;
}

int main(void) {
  struct info in;
  memset(&in, 0, sizeof in);

  struct wm_workspace list[] = {
      ws("mail", -1, false, false), ws("10", 10, false, true),
      ws("2", 2, true, false),      ws("1:web", 1, false, false),
      ws("chat", -1, false, false), ws("3", 3, false, false),
  };
  assert(info_set_workspaces(&in, list, 6));
  const char *want[] = {"1:web", "2", "3", "10", "chat", "mail"};
  assert(in.ws_count == 6);
  for (int i = 0; i < 6; ++i)
    assert(strcmp(in.ws[i].name, want[i]) == 0);
  assert(in.ws[1].focused && !in.ws[0].focused);
  assert(in.ws[3].urgent && in.ws[3].num == 10);
  assert(in.ws[4].num == -1);

  /* Unchanged input reports no change. */
  assert(!info_set_workspaces(&in, list, 6));
  list[2].focused = false;
  list[5].focused = true;
  assert(info_set_workspaces(&in, list, 6));
  assert(in.ws[2].focused);

  /* Long names are cut to fit; at most WS_MAX are kept. */
  struct wm_workspace many[WS_MAX + 8];
  for (int i = 0; i < WS_MAX + 8; ++i)
    many[i] = ws("", i + 1, i == 0, false);
  snprintf(many[0].name, sizeof many[0].name, "%s",
           "a-very-long-workspace-name-that-goes-on-and-on");
  assert(info_set_workspaces(&in, many, WS_MAX + 8));
  assert(in.ws_count == WS_MAX);
  assert(strlen(in.ws[0].name) == sizeof(in.ws[0].name) - 1);

  /* Nothing from sway (no connection): no workspaces. */
  assert(info_set_workspaces(&in, NULL, 0));
  assert(in.ws_count == 0);

  printf("popup workspaces: ok\n");
  return 0;
}
