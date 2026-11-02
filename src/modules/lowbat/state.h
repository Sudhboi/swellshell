/* lowbat: when to warn. Pure: fed one battery reading at a time. */
#ifndef SWELL_LOWBAT_STATE_H
#define SWELL_LOWBAT_STATE_H

#include <stdbool.h>

enum lowbat_event {
  LOWBAT_NONE,     /* nothing changes */
  LOWBAT_LOW,      /* crossed low_pct: a short warning */
  LOWBAT_CRITICAL, /* crossed critical_pct: a warning that stays */
  LOWBAT_CLEAR,    /* plugged in after a warning: take it away */
};

/* What has been warned about since the battery last charged. */
struct lowbat_state {
  bool low, critical;
};

/* One reading (pct < 0: no battery). Each level fires once per discharge;
 * falling straight past both fires only the critical one. AC re-arms. */
enum lowbat_event lowbat_step(struct lowbat_state *s, int pct, bool on_ac,
                              int low_pct, int critical_pct);

#endif
