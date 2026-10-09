/* Config parsing: types, ranges, colours, unknown keys, defaults. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

struct demo {
  int width;
  double ratio;
  bool live;
  char font[32];
  uint32_t bg;
};

static const struct demo defaults = {220, 0.5, true, "Sans 11", 0x000000f0u};
static const struct cfg_field fields[] = {
    CFG_INT(struct demo, width, 1, 4096, "width"),
    CFG_DOUBLE(struct demo, ratio, 0, 1, "ratio"),
    CFG_BOOL(struct demo, live, "live"),
    CFG_STRING(struct demo, font, "font"),
    CFG_COLOR(struct demo, bg, "background"),
};

static struct demo parse(const char *toml) {
  char path[] = "/tmp/swell-test-XXXXXX";
  int fd = mkstemp(path);
  FILE *f = fdopen(fd, "w");
  fputs(toml, f);
  fclose(f);
  struct cfg_doc doc;
  cfg_load(&doc, path);
  struct demo d = defaults;
  cfg_apply(cfg_section(&doc, "demo"), "demo", fields,
            sizeof(fields) / sizeof(fields[0]), &d, NULL);
  cfg_free(&doc);
  remove(path);
  return d;
}

static bool same(struct demo a, struct demo b) {
  return a.width == b.width && a.ratio == b.ratio && a.live == b.live &&
         strcmp(a.font, b.font) == 0 && a.bg == b.bg;
}

int main(void) {
  struct demo d = parse("");
  assert(same(d, defaults));

  d = parse("[demo]\nwidth = 300\nratio = 1\nlive = false\n"
            "font = \"Iosevka 12\"\nbg = \"#112233\"\n");
  assert(d.width == 300 && d.ratio == 1.0 && !d.live);
  assert(strcmp(d.font, "Iosevka 12") == 0);
  assert(d.bg == 0x112233ffu);

  d = parse("[demo]\nbg = \"#11223344\"\n");
  assert(d.bg == 0x11223344u);

  /* bad values keep the defaults */
  d = parse("[demo]\nwidth = 0\nratio = \"x\"\nlive = 1\nbg = \"red\"\n"
            "font = \"this font name is definitely longer than 32 bytes\"\n"
            "bogus = 1\n");
  assert(same(d, defaults));

  /* a broken file means defaults */
  d = parse("[demo\nwidth = 3\n");
  assert(same(d, defaults));

  uint32_t c;
  assert(cfg_parse_color("ff3b3b", &c) && c == 0xff3b3bffu);
  assert(!cfg_parse_color("#12345", &c));
  puts("config ok");
  return 0;
}
