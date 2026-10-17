/* Nerd Font icons (UTF-8) for the system information, shared by the popup
 * and the wallpaper's copy of its contents. */
#ifndef SWELL_ICONS_H
#define SWELL_ICONS_H

#include "info.h"

static const char *const ICON_BAT[] = {
    /* 0, 10, 20 .. 100 % */
    "\U000F008E", "\U000F007A", "\U000F007B", "\U000F007C",
    "\U000F007D", "\U000F007E", "\U000F007F", "\U000F0080",
    "\U000F0081", "\U000F0082", "\U000F0079",
};
static const char *const ICON_BAT_CHG[] = {
    "\U000F089F", "\U000F089C", "\U000F0086", "\U000F0087",
    "\U000F0088", "\U000F089D", "\U000F0089", "\U000F089E",
    "\U000F008A", "\U000F008B", "\U000F0085",
};
static const char ICON_BAT_NONE[] = "\U000F0091";
static const char ICON_VOL_HIGH[] = "\U000F057E";
static const char ICON_VOL_MED[] = "\U000F0580";
static const char ICON_VOL_LOW[] = "\U000F057F";
static const char ICON_MUTED[] = "\U000F0581";
static const char ICON_BRIGHT[] = "\U000F00E0";
static const char *const ICON_WIFI[] = {
    /* weakest .. strongest */
    "\U000F091F",
    "\U000F0922",
    "\U000F0925",
    "\U000F0928",
};
static const char ICON_WIRED[] = "\U000F0200";
static const char ICON_OFFLINE[] = "\U000F0319";
static const char ICON_NET_UNK[] = "\U000F06F5";

static inline const char *icon_battery(const struct info *in) {
  if (!in->has_battery)
    return ICON_BAT_NONE;
  int b = in->battery < 0 ? 0 : in->battery > 100 ? 100 : in->battery;
  int i = (b + 5) / 10;
  return in->charging ? ICON_BAT_CHG[i] : ICON_BAT[i];
}

static inline const char *icon_volume(const struct info *in) {
  if (in->muted)
    return ICON_MUTED;
  if (in->volume >= 66)
    return ICON_VOL_HIGH;
  if (in->volume >= 33)
    return ICON_VOL_MED;
  return ICON_VOL_LOW;
}

/* By wifi signal percent (-1: unknown, drawn full). */
static inline const char *icon_wifi(int s) {
  return ICON_WIFI[s < 0 ? 3 : s >= 75 ? 3 : s >= 50 ? 2 : s >= 25 ? 1 : 0];
}

#endif
