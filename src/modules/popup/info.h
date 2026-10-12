/* System information shown in the popup. */
#ifndef SWELL_POPUP_INFO_H
#define SWELL_POPUP_INFO_H

#include <stdbool.h>
#include <time.h>

#include "loop.h"
#include "sway.h"

enum net_kind { NET_UNKNOWN, NET_OFFLINE, NET_WIFI, NET_WIRED };

#define WS_MAX 32

struct workspace {
  char name[32];
  int num; /* -1 for a named workspace */
  bool focused;
  bool urgent;
};

struct info {
  time_t now;

  bool has_battery;
  int battery;   /* percent */
  bool charging; /* Charging, or Full/Not charging on AC */
  bool has_power;      /* the battery reports a charge/discharge rate */
  bool power_charging; /* power_w is the charge rate, not the system's draw */
  double power_w;      /* watts */

  bool has_volume;
  int volume; /* percent, may exceed 100 */
  bool muted;

  bool has_brightness;
  int brightness; /* percent */

  enum net_kind net;
  char net_name[64]; /* SSID or connection name */
  int net_signal;    /* wifi signal percent, -1 if unknown */

  int cpu; /* percent, -1 until two samples exist */
  double mem_used_gib, mem_total_gib;
  long uptime_s;

  int ws_count; /* 0 until sway answers; sorted by num, then name */
  struct workspace ws[WS_MAX];
};

/* Called after an async command's result changed *in. */
typedef void (*info_changed_fn)(void *data);

/* Async commands run on loop and write their results into *in. */
void info_init(struct loop *loop, struct info *in, info_changed_fn fn,
               void *data);
/* Kill running commands and stop watching their pipes. */
void info_finish(void);

/* Re-read fast sources (time, sysfs, /proc) and start async commands that
 * aren't still running. refresh_ms is the tick period (for the CPU
 * baseline). */
void info_refresh(struct info *in, int refresh_ms, int kill_ms);

/* Copy sway's workspaces into *in: the first WS_MAX, sorted by num with
 * named workspaces (num -1) last by name. Returns true if *in changed. */
bool info_set_workspaces(struct info *in, const struct sway_workspace *ws,
                         int n);

#endif
