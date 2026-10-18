/* The workspace-switch hand-off (wsswitch.c). */
#include <assert.h>
#include <stdio.h>

#include "wsswitch.h"

static int ran;
static void go(void *data) { ++*(int *)data; }

static bool decline(int dir, const char *output, wsswitch_go_fn g, void *d) {
  return false;
}

static wsswitch_go_fn held;
static void *held_data;
static int begun_dir;
static bool hold(int dir, const char *output, wsswitch_go_fn g, void *d) {
  begun_dir = dir;
  held = g;
  held_data = d;
  return true;
}

int main(void) {
  /* no sink: at once */
  wsswitch_run(1, "X", go, &ran);
  assert(ran == 1);

  /* a sink that declines: at once, once */
  static const struct wsswitch_sink no = {.begin = decline};
  wsswitch_set_sink(&no);
  wsswitch_run(1, "X", go, &ran);
  assert(ran == 2);

  /* a sink that defers: not yet, then once when it says */
  static const struct wsswitch_sink later = {.begin = hold};
  wsswitch_set_sink(&later);
  wsswitch_run(-1, "X", go, &ran);
  assert(ran == 2 && begun_dir == -1 && held);
  held(held_data);
  assert(ran == 3);

  /* nowhere to go: still handed over, so a held switch can go first */
  held = NULL;
  wsswitch_run(0, "X", go, &ran);
  assert(ran == 3 && begun_dir == 0 && held);
  held(held_data);
  assert(ran == 4);

  wsswitch_set_sink(NULL);
  puts("ok");
  return 0;
}
