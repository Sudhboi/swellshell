#include "swipe.h"

#include <math.h>

enum swipe_dir swipe_classify(double dx, double dy, int fingers,
                              int want_fingers, double threshold) {
  if (want_fingers && fingers != want_fingers)
    return SWIPE_NONE;
  double ax = fabs(dx), ay = fabs(dy);
  if (fmax(ax, ay) < threshold || ax == ay)
    return SWIPE_NONE;
  if (ax > ay)
    return dx < 0 ? SWIPE_LEFT : SWIPE_RIGHT;
  return dy < 0 ? SWIPE_UP : SWIPE_DOWN;
}

const char *swipe_name(enum swipe_dir d) {
  switch (d) {
  case SWIPE_LEFT:
    return "left";
  case SWIPE_RIGHT:
    return "right";
  case SWIPE_UP:
    return "up";
  case SWIPE_DOWN:
    return "down";
  default:
    return "none";
  }
}
