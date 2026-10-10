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

static bool read_num(const char *supply, const char *attr, long long *v) {
  char buf[64], *end;
  if (!read_attr(supply, attr, buf, sizeof(buf)))
    return false;
  *v = strtoll(buf, &end, 10);
  return end != buf;
}

/* Copies the name of the system battery into name; false if there is none. */
static bool find_battery(char *name, size_t n) {
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
    long long cap;
    if (!read_num(e->d_name, "capacity", &cap))
      continue;
    snprintf(name, n, "%s", e->d_name);
    found = true;
  }
  closedir(d);
  return found;
}

bool battery_read(int *percent, bool *charging) {
  char bat[256];
  long long cap;
  if (!find_battery(bat, sizeof(bat)) || !read_num(bat, "capacity", &cap))
    return false;
  char status[32] = "";
  read_attr(bat, "status", status, sizeof(status));
  *percent = (int)cap;
  *charging = !strcmp(status, "Charging") || !strcmp(status, "Full") ||
              !strcmp(status, "Not charging");
  return true;
}

bool battery_power(double *watts) {
  char bat[256], status[32] = "";
  if (!find_battery(bat, sizeof(bat)) ||
      !read_attr(bat, "status", status, sizeof(status)) ||
      strcmp(status, "Discharging") != 0)
    return false;
  /* power_now is in µW; batteries without it report current_now (µA) and
   * voltage_now (µV). Some drivers sign the rate. */
  long long p, i, v;
  double w;
  if (read_num(bat, "power_now", &p) && p != 0)
    w = (double)llabs(p) / 1e6;
  else if (read_num(bat, "current_now", &i) &&
           read_num(bat, "voltage_now", &v))
    w = (double)llabs(i) / 1e6 * (double)llabs(v) / 1e6;
  else
    return false;
  if (w <= 0)
    return false;
  *watts = w;
  return true;
}
