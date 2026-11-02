#include "state.h"

enum lowbat_event lowbat_step(struct lowbat_state *s, int pct, bool on_ac,
                              int low_pct, int critical_pct) {
  if (pct < 0)
    return LOWBAT_NONE;
  if (on_ac) {
    bool fired = s->low || s->critical;
    *s = (struct lowbat_state){0};
    return fired ? LOWBAT_CLEAR : LOWBAT_NONE;
  }
  if (pct <= critical_pct && !s->critical) {
    s->low = s->critical = true;
    return LOWBAT_CRITICAL;
  }
  if (pct <= low_pct && !s->low) {
    s->low = true;
    return LOWBAT_LOW;
  }
  return LOWBAT_NONE;
}
