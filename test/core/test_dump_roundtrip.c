/* `swell config` must print valid TOML that parses back to the same values,
 * also for strings with control characters (a multi-line hook command, a
 * tab). cfg_dump and the runner's hooks_dump write such characters raw, so
 * this test currently FAILS and is registered with should_fail: true.
 * Flip that once the dumpers escape \n, \t, \r, \b, \f and other control
 * characters. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

struct demo {
  char s[64];
};
static const struct cfg_field fields[] = {
    CFG_STRING(struct demo, s, "a string"),
};

int main(void) {
  struct demo in = {.s = "line1\nline2\ttab \"q\" \\ \x01"};
  char path[] = "/tmp/swell-dump-XXXXXX";
  FILE *f = fdopen(mkstemp(path), "w");
  fputs("[demo]\n", f);
  cfg_dump(f, fields, 1, &in);
  fclose(f);

  struct cfg_doc doc;
  cfg_load(&doc, path);
  remove(path);
  if (!doc.loaded) {
    fprintf(stderr, "dumped TOML does not parse\n");
    cfg_free(&doc);
    return 1;
  }
  struct demo out = {""};
  cfg_apply(cfg_section(&doc, "demo"), "demo", fields, 1, &out, NULL);
  cfg_free(&doc);
  if (strcmp(in.s, out.s) != 0) {
    fprintf(stderr, "round trip changed the string\n");
    return 1;
  }
  return 0;
}
