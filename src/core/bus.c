#include "bus.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>

#include "util.h"

struct sub {
  bus_fn fn;
  void *data;
  bool dead;
};

static struct sub *subs;
static int nsubs;
static int depth; /* emit nesting; removal is deferred while > 0 */

void bus_subscribe(bus_fn fn, void *data) {
  subs = xrealloc(subs, (nsubs + 1) * sizeof(*subs));
  subs[nsubs++] = (struct sub){fn, data, false};
}

static void sweep(void) {
  int j = 0;
  for (int i = 0; i < nsubs; ++i)
    if (!subs[i].dead)
      subs[j++] = subs[i];
  nsubs = j;
}

void bus_unsubscribe(bus_fn fn, void *data) {
  for (int i = 0; i < nsubs; ++i)
    if (subs[i].fn == fn && subs[i].data == data)
      subs[i].dead = true;
  if (depth == 0)
    sweep();
}

void bus_emitv(const char *name, char *const env[]) {
  static char *const empty[] = {NULL};
  struct bus_event ev = {name, env ? env : empty};
  ++depth;
  int n = nsubs; /* subscribers added during emit miss this event */
  for (int i = 0; i < n; ++i)
    if (!subs[i].dead)
      subs[i].fn(&ev, subs[i].data);
  if (--depth == 0)
    sweep();
}

void bus_emit(const char *name, ...) {
  char *env[16];
  int n = 0;
  va_list ap;
  va_start(ap, name);
  char *s;
  while ((s = va_arg(ap, char *)) && n < (int)ARRAY_LEN(env) - 1)
    env[n++] = s;
  va_end(ap);
  env[n] = NULL;
  bus_emitv(name, env);
}
