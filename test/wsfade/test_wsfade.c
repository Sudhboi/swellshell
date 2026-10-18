/* wsfade's motion (curve.h). */
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "curve.h"

static bool near(double a, double b) { return fabs(a - b) < 1e-9; }

static void test_start(void) {
  struct wsfade_curve c;
  wsfade_start(&c, 1, 60, 160, 1000);
  assert(near(tween_value(&c.alpha, 1000), 1));
  assert(near(tween_value(&c.x, 1000), 0));
  /* eased out: more than half done at half time */
  assert(tween_value(&c.alpha, 1080) < 0.5);
  assert(tween_value(&c.x, 1080) < -30);
  assert(!wsfade_done(&c, 1159));
  assert(wsfade_done(&c, 1160));
  assert(near(tween_value(&c.alpha, 1160), 0));
  assert(near(tween_value(&c.x, 1160), -60));
  /* previous: drifts the other way */
  wsfade_start(&c, -1, 60, 160, 1000);
  assert(near(tween_value(&c.x, 2000), 60));
}

static void test_retarget(void) {
  struct wsfade_curve c;
  wsfade_start(&c, 1, 60, 160, 1000);
  double a = tween_value(&c.alpha, 1040), x = tween_value(&c.x, 1040);
  /* no jump at the retarget */
  wsfade_retarget(&c, 1, 60, 160, 1040);
  assert(near(tween_value(&c.alpha, 1040), a));
  assert(near(tween_value(&c.x, 1040), x));
  /* the rest takes the share of the time the opacity left is worth */
  assert(near(c.alpha.dur, 160 * a));
  assert(near(tween_value(&c.x, 5000), x - 60 * a));
  assert(wsfade_done(&c, 1040 + 160 * a));
  /* the other way: back towards where it started */
  wsfade_start(&c, 1, 60, 160, 1000);
  x = tween_value(&c.x, 1040);
  wsfade_retarget(&c, -1, 60, 160, 1040);
  assert(tween_value(&c.x, 5000) > x);
  /* retargeted when already done: done at once */
  wsfade_start(&c, 1, 60, 160, 1000);
  wsfade_retarget(&c, 1, 60, 160, 2000);
  assert(wsfade_done(&c, 2000));
}

int main(void) {
  test_start();
  test_retarget();
  puts("ok");
  return 0;
}
