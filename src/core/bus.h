/* Internal event bus. Modules and the sway layer emit named events
 * ("osd.show", "workspace.focus", ...) with KEY=VALUE details; subscribers
 * (the runner, for one) get every event synchronously. */
#ifndef SWELL_BUS_H
#define SWELL_BUS_H

struct bus_event {
  const char *name;
  /* NULL-terminated "KEY=VALUE" strings, e.g. "SWELL_WORKSPACE=3". */
  char *const *env;
};

typedef void (*bus_fn)(const struct bus_event *ev, void *data);

void bus_subscribe(bus_fn fn, void *data);
void bus_unsubscribe(bus_fn fn, void *data);

/* Emit name; the variadic part is "KEY=VALUE" strings ending with NULL. */
void bus_emit(const char *name, ...) __attribute__((sentinel));
void bus_emitv(const char *name, char *const env[]);

#endif
