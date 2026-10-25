/* gestures: telling a finished touchpad swipe's direction (pure). */
#ifndef SWELL_GESTURES_SWIPE_H
#define SWELL_GESTURES_SWIPE_H

enum swipe_dir { SWIPE_NONE, SWIPE_LEFT, SWIPE_RIGHT, SWIPE_UP, SWIPE_DOWN };

/* The direction of a swipe that moved dx, dy in all (libinput's
 * unaccelerated units, 1/1000 inch; y grows downwards) with fingers
 * fingers: the axis it moved along most, if that is at least threshold.
 * SWIPE_NONE if it was too short, or want_fingers is not 0 and differs. */
enum swipe_dir swipe_classify(double dx, double dy, int fingers,
                              int want_fingers, double threshold);
/* "left", "right", "up", "down" or "none". */
const char *swipe_name(enum swipe_dir d);

#endif
