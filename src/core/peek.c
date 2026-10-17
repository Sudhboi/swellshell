/* The popup's timeline and its hand-off to the wallpaper. */
#include "peek.h"

#include <stddef.h>

#include "anim.h"

static const struct peek_sink *sink;

void peek_set_sink(const struct peek_sink *s) { sink = s; }
const struct peek_sink *peek_get_sink(void) { return sink; }

static double tween(int64_t t0, int ms, int64_t now, bool *more) {
  return ease_out_cubic(anim_progress(t0, ms, now, more));
}

static double pop_value(const struct peek *p, int64_t now, bool *more) {
  double t = tween(p->pop_at, p->closing ? p->close_ms : p->pop_ms, now, more);
  return p->pop_from + (p->pop_to - p->pop_from) * t;
}

/* How far the expand delay has run (linear, so the hold line runs at the
 * pace of the wait). */
static double hold_value(const struct peek *p, int64_t now, bool *more) {
  if (!p->hold_at)
    return p->hold;
  return anim_progress(p->hold_at, p->expand_delay_ms, now, more);
}

void peek_values(const struct peek *p, int64_t now, struct peek_values *v) {
  v->more = false;
  v->pop = pop_value(p, now, &v->more);
  v->expand =
      p->expand_at ? tween(p->expand_at, p->expand_ms, now, &v->more) : 0;
  v->hold = p->hold_line ? hold_value(p, now, &v->more) : 0;
}

/* Start a pop tween from wherever the current one is. */
static void retarget_pop(struct peek *p, double to, bool closing, int64_t now) {
  p->pop_from = pop_value(p, now, NULL);
  p->pop_to = to;
  p->pop_at = now;
  p->closing = closing;
}

bool peek_open(struct peek *p, bool fresh, int64_t now) {
  if (fresh) {
    p->pop_at = now;
    p->pop_from = p->pop_to = 0;
    p->closing = false;
    p->expand_at = 0;
  }
  retarget_pop(p, 1, false, now);
  if (p->expand_at)
    return false;
  p->hold_at = p->expand_delay_ms > 0 ? now : 0;
  p->hold = 0;
  return true;
}

void peek_expand(struct peek *p, int64_t now) {
  p->expand_at = now;
  /* Full, then faded out by the expand. */
  p->hold_at = 0;
  p->hold = 1;
}

void peek_close(struct peek *p, int64_t now) {
  retarget_pop(p, 0, true, now);
  /* The line slides away as far as it got. */
  p->hold = hold_value(p, now, NULL);
  p->hold_at = 0;
}

int64_t peek_close_left(const struct peek *p, int64_t now) {
  if (!p->closing)
    return -1;
  int64_t left = p->pop_at + p->close_ms - now;
  return left > 0 ? left : 0;
}
