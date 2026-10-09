/* osd command parsing: nixosd's FIFO line semantics. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ipc.h"
#include "module.h"
#include "osd/osd.h"

extern const struct swell_module osd_module;

static struct osd_event parse_ok(const char *line, int max) {
  struct osd_event ev;
  bool ok = osd_parse_line(line, max, &ev);
  if (!ok)
    fprintf(stderr, "expected '%s' to parse\n", line);
  assert(ok);
  return ev;
}

static void parse_bad(const char *line) {
  struct osd_event ev;
  bool ok = osd_parse_line(line, 200, &ev);
  if (ok)
    fprintf(stderr, "expected '%s' to fail\n", line);
  assert(!ok);
}

int main(void) {
  struct osd_event e;
  e = parse_ok("40", 200);
  assert(e.kind == OSD_VOLUME && e.level == 40 && !e.muted);
  e = parse_ok("  40 mute", 200); /* a bare number ignores the flag */
  assert(e.kind == OSD_VOLUME && e.level == 40 && !e.muted);
  e = parse_ok("vol 40 mute", 200);
  assert(e.kind == OSD_VOLUME && e.level == 40 && e.muted);
  e = parse_ok("VOLUME 55 MUTE", 200);
  assert(e.kind == OSD_VOLUME && e.level == 55 && e.muted);
  e = parse_ok("vol 55 loud", 200);
  assert(e.kind == OSD_VOLUME && !e.muted);
  e = parse_ok("Bri 80", 200);
  assert(e.kind == OSD_BRIGHTNESS && e.level == 80 && !e.muted);
  e = parse_ok("brightness 50 mute", 200); /* brightness ignores mute */
  assert(e.kind == OSD_BRIGHTNESS && e.level == 50 && !e.muted);
  e = parse_ok("brightness 300", 200);
  assert(e.level == 200);
  e = parse_ok("vol -5", 200);
  assert(e.level == 0);
  e = parse_ok("vol 180", 150); /* clamps to max_pct */
  assert(e.level == 150);
  e = parse_ok("250", 200);
  assert(e.level == 200);

  parse_bad("");
  parse_bad("vol");
  parse_bad("vol abc");
  parse_bad("foo 40");
  parse_bad("volumes 40");
  parse_bad("mute");

  /* The command reports unknown input as an error. */
  struct reply r = {0};
  char *argv[] = {"foo", "bar", NULL};
  assert(osd_module.command(2, argv, &r) != 0);
  assert(r.buf && strstr(r.buf, "cannot parse 'foo bar'"));
  reply_free(&r);
  r = (struct reply){0};
  assert(osd_module.command(0, argv, &r) != 0);
  reply_free(&r);

  printf("osd parse: ok\n");
  return 0;
}
