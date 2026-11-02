/* When the low battery warning fires. */
#include <assert.h>
#include <stdio.h>

#include "lowbat/state.h"

#define LOW 15
#define CRIT 5

static enum lowbat_event step(struct lowbat_state *s, int pct, bool ac) {
  return lowbat_step(s, pct, ac, LOW, CRIT);
}

int main(void) {
  struct lowbat_state s = {0};
  /* Discharging down through both levels: each fires once. */
  assert(step(&s, 20, false) == LOWBAT_NONE);
  assert(step(&s, 16, false) == LOWBAT_NONE);
  assert(step(&s, 15, false) == LOWBAT_LOW);
  assert(step(&s, 15, false) == LOWBAT_NONE);
  assert(step(&s, 14, false) == LOWBAT_NONE);
  assert(step(&s, 6, false) == LOWBAT_NONE);
  assert(step(&s, 5, false) == LOWBAT_CRITICAL);
  assert(step(&s, 4, false) == LOWBAT_NONE);
  assert(step(&s, 1, false) == LOWBAT_NONE);
  /* Plugged in: clears, and only once. */
  assert(step(&s, 1, true) == LOWBAT_CLEAR);
  assert(step(&s, 2, true) == LOWBAT_NONE);
  /* Unplugged below low: warns again. */
  assert(step(&s, 14, false) == LOWBAT_LOW);
  assert(step(&s, 14, true) == LOWBAT_CLEAR);
  /* Charged up and unplugged above low: nothing until it falls. */
  assert(step(&s, 80, false) == LOWBAT_NONE);
  assert(step(&s, 15, false) == LOWBAT_LOW);

  /* Falling straight past both (suspend, a start below): critical only. */
  s = (struct lowbat_state){0};
  assert(step(&s, 16, false) == LOWBAT_NONE);
  assert(step(&s, 4, false) == LOWBAT_CRITICAL);
  assert(step(&s, 3, false) == LOWBAT_NONE);
  s = (struct lowbat_state){0};
  assert(step(&s, 3, false) == LOWBAT_CRITICAL);

  /* On AC with nothing shown: nothing. No battery: nothing. */
  s = (struct lowbat_state){0};
  assert(step(&s, 3, true) == LOWBAT_NONE);
  assert(step(&s, -1, false) == LOWBAT_NONE);
  assert(!s.low && !s.critical);

  printf("lowbat state ok\n");
  return 0;
}
