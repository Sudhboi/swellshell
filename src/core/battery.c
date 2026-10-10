#include "battery.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SUPPLY_DIR "/sys/class/power_supply"

static bool read_attr(const char *supply, const char *attr, char *buf,
                      size_t n) {
  char path[512];
  snprintf(path, sizeof(path), SUPPLY_DIR "/%s/%s", supply, attr);
  FILE *f = fopen(path, "re");
  if (!f)
    return false;
  bool ok = fgets(buf, (int)n, f) != NULL;
  fclose(f);
  if (ok)
    buf[strcspn(buf, "\n")] = '\0';
  return ok;
}

bool battery_read(int *percent, bool *charging) {
  DIR *d = opendir(SUPPLY_DIR);
  if (!d)
    return false;
  bool found = false;
  struct dirent *e;
  while (!found && (e = readdir(d))) {
    if (e->d_name[0] == '.')
      continue;
    char buf[64];
    if (!read_attr(e->d_name, "type", buf, sizeof(buf)) ||
        strcmp(buf, "Battery") != 0)
      continue;
    if (read_attr(e->d_name, "scope", buf, sizeof(buf)) &&
        strcmp(buf, "Device") == 0)
      continue;
    char *end;
    if (!read_attr(e->d_name, "capacity", buf, sizeof(buf)))
      continue;
    long cap = strtol(buf, &end, 10);
    if (end == buf)
      continue;
    char status[32] = "";
    read_attr(e->d_name, "status", status, sizeof(status));
    *percent = (int)cap;
    *charging = !strcmp(status, "Charging") || !strcmp(status, "Full") ||
                !strcmp(status, "Not charging");
    found = true;
  }
  closedir(d);
  return found;
}
