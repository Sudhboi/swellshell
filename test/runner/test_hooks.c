/* Hook parsing, defaults and event matching. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "hooks.h"

static struct cfg_doc load(const char *text) {
  char path[] = "/tmp/swell-hooks-XXXXXX";
  FILE *f = fdopen(mkstemp(path), "w");
  fputs(text, f);
  fclose(f);
  struct cfg_doc doc;
  cfg_load(&doc, path);
  remove(path);
  return doc;
}

int main(void) {
  struct hook_list l = {0};
  hooks_set_defaults(&l);
  assert(l.n == 2);
  assert(hook_matches(&l.hooks[0], "workspace.empty"));
  assert(!hook_matches(&l.hooks[0], "workspace.occupied"));
  assert(l.hooks[0].detach && !l.hooks[1].detach);
  assert(strcmp(l.hooks[1].run[0], "awww pause") == 0);
  hooks_free(&l);

  struct cfg_doc doc = load("[runner]\n"
                            "[[runner.hook]]\non = [\"window.*\", \"osd.show\"]\n"
                            "run = \"echo hi\"\n"
                            "[[runner.hook]]\non = \"popup.show\"\n"
                            "run = [\"a\", \"b\"]\ndetach = true\n"
                            "[[runner.hook]]\nrun = \"no trigger\"\n");
  assert(hooks_parse(&l, cfg_section(&doc, "runner"), "runner"));
  assert(l.n == 2); /* the third is invalid */
  assert(hook_matches(&l.hooks[0], "window.new"));
  assert(hook_matches(&l.hooks[0], "osd.show"));
  assert(!hook_matches(&l.hooks[0], "osd.hide"));
  assert(l.hooks[1].nrun == 2 && l.hooks[1].detach);
  hooks_free(&l);
  cfg_free(&doc);

  doc = load("[runner]\nhook = []\n");
  assert(hooks_parse(&l, cfg_section(&doc, "runner"), "runner") && l.n == 0);
  cfg_free(&doc);

  doc = load("[runner]\ntimeout_ms = 5\n");
  assert(!hooks_parse(&l, cfg_section(&doc, "runner"), "runner"));
  cfg_free(&doc);
  puts("hooks ok");
  return 0;
}
