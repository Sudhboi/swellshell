/* Hook definitions and matching (unit-tested without the daemon). */
#ifndef SWELL_RUNNER_HOOKS_H
#define SWELL_RUNNER_HOOKS_H

#include <stdbool.h>
#include <stdio.h>

#include "tomlc17.h"

struct hook {
  char **on;  /* event patterns (fnmatch globs, e.g. "window.*") */
  int non;
  char **run; /* shell commands, run in order */
  int nrun;
  bool detach; /* don't wait for the commands; start them and move on */
};

struct hook_list {
  struct hook *hooks;
  int n;
};

/* awww-kill's behaviour: pause the wallpaper daemon while the focused
 * workspace has windows. */
void hooks_set_defaults(struct hook_list *l);
/* Parse the `hook` array of tables from [runner]. Invalid entries are
 * reported and skipped. Returns false if the key is absent. */
bool hooks_parse(struct hook_list *l, toml_datum_t section, const char *sec);
void hooks_free(struct hook_list *l);
void hooks_dump(FILE *f, const struct hook_list *l);
bool hook_matches(const struct hook *h, const char *event);

#endif
