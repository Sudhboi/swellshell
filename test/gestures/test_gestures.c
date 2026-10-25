/* gestures: a finished swipe's direction from how far it moved. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "swipe.h"

int main(void) {
  /* the axis it moved along most */
  assert(swipe_classify(-400, 50, 3, 3, 300) == SWIPE_LEFT);
  assert(swipe_classify(400, -100, 3, 3, 300) == SWIPE_RIGHT);
  assert(swipe_classify(80, -500, 3, 3, 300) == SWIPE_UP);
  assert(swipe_classify(-80, 500, 3, 3, 300) == SWIPE_DOWN);
  /* too short, or exactly diagonal */
  assert(swipe_classify(-299, 0, 3, 3, 300) == SWIPE_NONE);
  assert(swipe_classify(400, 400, 3, 3, 300) == SWIPE_NONE);
  /* the threshold is on the main axis, not the length */
  assert(swipe_classify(250, 200, 3, 3, 300) == SWIPE_NONE);
  /* finger count: must match, unless 0 (any) */
  assert(swipe_classify(-400, 0, 4, 3, 300) == SWIPE_NONE);
  assert(swipe_classify(-400, 0, 4, 0, 300) == SWIPE_LEFT);
  assert(strcmp(swipe_name(SWIPE_UP), "up") == 0);
  assert(strcmp(swipe_name(SWIPE_NONE), "none") == 0);
  printf("gestures tests passed\n");
  return 0;
}
