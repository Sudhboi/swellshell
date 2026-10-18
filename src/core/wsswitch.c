/* The animator of workspace switches, if any. */
#include "wsswitch.h"

#include <stddef.h>

static const struct wsswitch_sink *sink;

void wsswitch_set_sink(const struct wsswitch_sink *s) { sink = s; }

void wsswitch_run(int dir, const char *output, wsswitch_go_fn go, void *data) {
  if (!sink || !sink->begin(dir, output, go, data))
    go(data);
}
