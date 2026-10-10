#include "hooks.h"

#include <fnmatch.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "util.h"

/* A string or an array of strings. */
static bool string_list(toml_datum_t d, const char *sec, const char *key,
                        char ***out, int *n) {
  if (d.type == TOML_STRING) {
    *out = xcalloc(1, sizeof(char *));
    (*out)[0] = xstrdup(d.u.s);
    *n = 1;
    return true;
  }
  if (d.type == TOML_ARRAY) {
    *out = xcalloc(d.u.arr.size + 1, sizeof(char *));
    *n = 0;
    for (int i = 0; i < d.u.arr.size; ++i) {
      toml_datum_t e = d.u.arr.elem[i];
      if (e.type != TOML_STRING) {
        cfg_error(e, sec, key, "expected strings");
        continue;
      }
      (*out)[(*n)++] = xstrdup(e.u.s);
    }
    if (*n == 0)
      cfg_error(d, sec, key, "is empty");
    return *n > 0;
  }
  cfg_error(d, sec, key, "expected a string or an array of strings");
  return false;
}

static void free_strings(char **s, int n) {
  for (int i = 0; i < n; ++i)
    free(s[i]);
  free(s);
}

bool hooks_parse(struct hook_list *l, toml_datum_t section, const char *sec) {
  if (section.type != TOML_TABLE)
    return false;
  toml_datum_t arr = toml_get(section, "hook");
  if (arr.type == TOML_UNKNOWN)
    return false;
  if (arr.type != TOML_ARRAY) {
    cfg_error(arr, sec, "hook", "expected [[%s.hook]] tables", sec);
    return true; /* present but broken: no hooks rather than surprises */
  }
  for (int i = 0; i < arr.u.arr.size; ++i) {
    toml_datum_t t = arr.u.arr.elem[i];
    if (t.type != TOML_TABLE) {
      cfg_error(t, sec, "hook", "expected a table");
      continue;
    }
    struct hook h = {0};
    bool ok = true;
    for (int k = 0; k < t.u.tab.size; ++k) {
      const char *key = t.u.tab.key[k];
      toml_datum_t v = t.u.tab.value[k];
      if (strcmp(key, "on") == 0)
        ok &= string_list(v, sec, "hook.on", &h.on, &h.non);
      else if (strcmp(key, "run") == 0)
        ok &= string_list(v, sec, "hook.run", &h.run, &h.nrun);
      else if (strcmp(key, "detach") == 0 && v.type == TOML_BOOLEAN)
        h.detach = v.u.boolean;
      else {
        cfg_error(v, sec, key, "unknown or invalid hook key (ignored)");
      }
    }
    if (!h.non || !h.nrun) {
      cfg_error(t, sec, "hook", "needs both 'on' and 'run'; skipped");
      ok = false;
    }
    if (!ok) {
      free_strings(h.on, h.non);
      free_strings(h.run, h.nrun);
      continue;
    }
    l->hooks = xrealloc(l->hooks, (l->n + 1) * sizeof(*l->hooks));
    l->hooks[l->n++] = h;
  }
  return true;
}

void hooks_free(struct hook_list *l) {
  for (int i = 0; i < l->n; ++i) {
    free_strings(l->hooks[i].on, l->hooks[i].non);
    free_strings(l->hooks[i].run, l->hooks[i].nrun);
  }
  free(l->hooks);
  *l = (struct hook_list){0};
}

static void dump_string(FILE *f, const char *s) { cfg_dump_string(f, s); }

static void dump_list(FILE *f, char **s, int n) {
  fputc('[', f);
  for (int i = 0; i < n; ++i) {
    if (i)
      fputs(", ", f);
    dump_string(f, s[i]);
  }
  fputs("]\n", f);
}

void hooks_dump(FILE *f, const struct hook_list *l) {
  fputs("\n# Hooks: when an event matching `on` (a glob, e.g. \"window.*\") "
        "fires, run\n"
        "# the `run` commands with sh -c, one after another. Hooks run in a "
        "single\n"
        "# queue so their order is kept; `detach = true` starts the commands "
        "and\n"
        "# moves on without waiting. Commands get $SWELL_EVENT and the "
        "event's\n"
        "# details ($SWELL_WORKSPACE, $SWELL_APP_ID, ...). There are none by\n"
        "# default. Events: see README. For example:\n"
        "#\n"
        "#   [[runner.hook]]\n"
        "#   on = \"workspace.occupied\"\n"
        "#   run = [\"notify-send \\\"$SWELL_WORKSPACE\\\"\"]\n"
        "#   detach = true\n",
        f);
  if (l->n == 0)
    fputs("hook = []\n", f);
  for (int i = 0; i < l->n; ++i) {
    const struct hook *h = &l->hooks[i];
    fputs("\n[[runner.hook]]\non = ", f);
    if (h->non == 1) {
      dump_string(f, h->on[0]);
      fputc('\n', f);
    } else {
      dump_list(f, h->on, h->non);
    }
    fputs("run = ", f);
    dump_list(f, h->run, h->nrun);
    if (h->detach)
      fputs("detach = true\n", f);
  }
}

bool hook_matches(const struct hook *h, const char *event) {
  for (int i = 0; i < h->non; ++i)
    if (fnmatch(h->on[i], event, 0) == 0)
      return true;
  return false;
}
