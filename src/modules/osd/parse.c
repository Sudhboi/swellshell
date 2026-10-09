/* Command parsing, with nixosd's FIFO line semantics. */
#include <stdio.h>
#include <strings.h>

#include "osd.h"

static int clamp_pct(long v, int max_pct) {
  return v < 0 ? 0 : v > max_pct ? max_pct : (int)v;
}

bool osd_parse_line(const char *line, int max_pct, struct osd_event *ev) {
  char word[16] = "", flag[16] = "";
  long v;

  ev->kind = OSD_VOLUME;
  ev->muted = false;

  if (sscanf(line, " %ld", &v) == 1) {
    ev->level = clamp_pct(v, max_pct);
    return true;
  }
  int n = sscanf(line, " %15s %ld %15s", word, &v, flag);
  if (n < 2)
    return false;
  if (!strcasecmp(word, "vol") || !strcasecmp(word, "volume"))
    ev->kind = OSD_VOLUME;
  else if (!strcasecmp(word, "bri") || !strcasecmp(word, "brightness"))
    ev->kind = OSD_BRIGHTNESS;
  else
    return false;
  ev->level = clamp_pct(v, max_pct);
  ev->muted = n == 3 && ev->kind == OSD_VOLUME && !strcasecmp(flag, "mute");
  return true;
}
