/* The module interface. A module is one feature of the shell (the OSD, the
 * popup, ...). Adding one:
 *   1. src/modules/<name>/<name>.c defining `const struct swell_module
 *      <name>_module`,
 *   2. src/modules/<name>/meson.build listing its sources as <name>_src,
 *   3. <name> in the module list in meson.build and in src/modules/modules.c,
 *   4. test/<name>/meson.build, and <name> in the list in test/meson.build.
 *
 * Configuration is handled by the core: it copies `defaults`, applies the
 * [<name>] section of config.toml through `fields`, calls `configure` for
 * anything the field table can't express, and hands the result to `init`
 * (first time) or `reconfigure` (on `swell reload`). The core owns config
 * structs; a module must not keep pointers into an old one after
 * reconfigure returns. Every section also accepts `enable = true|false`. */
#ifndef SWELL_MODULE_H
#define SWELL_MODULE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "config.h"
#include "ipc.h"
#include "swell.h"

struct swell_module {
  const char *name;           /* config section and CLI word */
  const char *const *aliases; /* other CLI words, NULL-terminated, or NULL */
  const char *usage;          /* help text: one "  cmd  description" per line */
  bool needs_wayland;
  bool needs_wm;   /* sway, niri or labwc (wm.h) */
  bool needs_sway; /* sway only (sway.h) */
  bool not_niri;   /* with needs_wm: sway or labwc */
  bool off_by_default; /* runs only with `enable = true` */

  /* config */
  size_t config_size;
  const void *defaults;
  const struct cfg_field *fields;
  size_t nfields;
  const char *const *extra_keys; /* keys `configure` handles */
  void (*configure)(void *cfg, toml_datum_t section, const char *section_name);
  void (*free_config)(void *cfg);
  void (*dump_extra)(FILE *f, const void *cfg); /* for `swell config` */

  /* lifecycle (daemon) */
  bool (*init)(struct swell *sw, const void *cfg);
  void (*reconfigure)(const void *cfg);
  void (*finish)(void);
  /* Handle `swell <name> argv...` (argv[0] is the first word after the
   * module name; argc may be 0). Return 0 for success. */
  int (*command)(int argc, char **argv, struct reply *r);
  /* Optional: a new daemon replaces this one (handover.h). Write lines
   * "<name> ..." for the new daemon's module (handover_get), start any
   * goodbye, and return how long (ms) to keep running while it takes over,
   * 0 to stop now with the rest. */
  int (*handover)(struct reply *r);
  /* Optional: run a command in the client process instead of sending it to
   * the daemon. Called with have_daemon=true first; return -1 to send it
   * on. If no daemon answers it is called again with have_daemon=false.
   * Returns an exit status. sw->loop exists; Wayland and the window manager are not set up. */
  int (*run_local)(struct swell *sw, const void *cfg, bool have_daemon,
                   int argc, char **argv);
};

extern const struct swell_module *const swell_modules[];
extern const size_t swell_nmodules;

const struct swell_module *module_find(const char *word);

#endif
