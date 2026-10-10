/* The system battery, read from /sys/class/power_supply. */
#ifndef SWELL_BATTERY_H
#define SWELL_BATTERY_H

#include <stdbool.h>

/* The first supply of type Battery that powers the system (not a device's,
 * like a mouse). Sets *percent (capacity) and *charging (Charging, or
 * Full/Not charging on AC). False if there is none. */
bool battery_read(int *percent, bool *charging);

/* The battery's power in watts: the system's draw while discharging, the
 * charge rate while charging (*charging set). False otherwise (full or idle on
 * AC, where it reads 0) or if the battery doesn't report it. */
bool battery_power(double *watts, bool *charging);

#endif
