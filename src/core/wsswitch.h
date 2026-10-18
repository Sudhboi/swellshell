/* A workspace switch the workspaces module is about to make, handed to
 * whoever animates it (wsfade): it covers the output with a picture of the
 * old workspace first and runs the switch behind it. */
#ifndef SWELL_WSSWITCH_H
#define SWELL_WSSWITCH_H

#include <stdbool.h>

typedef void (*wsswitch_go_fn)(void *data);

struct wsswitch_sink {
  /* dir: +1 towards later workspaces, -1 earlier, 0 nowhere (the same
   * workspace; not to be animated). output: the focused output's name.
   * A switch the sink still holds must run before this one. False declines
   * (go then runs at once); true promises to call go(data) exactly once,
   * possibly before returning. */
  bool (*begin)(int dir, const char *output, wsswitch_go_fn go, void *data);
};
void wsswitch_set_sink(const struct wsswitch_sink *s); /* NULL: none */

/* Switch by calling go(data), through the sink if there is one. */
void wsswitch_run(int dir, const char *output, wsswitch_go_fn go, void *data);

#endif
