/* The popup's open/expand/close timeline (peek.c), shared by its panel and
 * the wallpaper, and the hand-off to the wallpaper (the sink). */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "peek.h"

static bool near(double a, double b) { return fabs(a - b) < 1e-9; }

static struct peek fresh(void) {
  struct peek p;
  memset(&p, 0, sizeof p);
  p.pop_ms = 200;
  p.close_ms = 100;
  p.expand_ms = 300;
  p.expand_delay_ms = 2000;
  p.hold_line = true;
  return p;
}

static void test_open_expand_close(void) {
  struct peek p = fresh();
  struct peek_values v;
  /* Opening from closed: arm the expand delay, start the hold line. */
  assert(peek_open(&p, true, 1000));
  peek_values(&p, 1000, &v);
  assert(near(v.pop, 0) && near(v.expand, 0) && near(v.hold, 0) && v.more);
  peek_values(&p, 1100, &v);
  assert(v.pop > 0.5 && v.pop < 1); /* eased out: past half at half time */
  assert(near(v.hold, 0.05));       /* linear, at the pace of the wait */
  peek_values(&p, 1200, &v);
  assert(near(v.pop, 1) && v.more); /* the hold line still runs */
  /* The delay ran out: the details reveal, the line is full. */
  peek_expand(&p, 3000);
  peek_values(&p, 3000, &v);
  assert(near(v.expand, 0) && near(v.hold, 1) && v.more);
  peek_values(&p, 3300, &v);
  assert(near(v.pop, 1) && near(v.expand, 1) && near(v.hold, 1) && !v.more);
  assert(peek_close_left(&p, 3300) < 0);
  /* Closing slides down over close_ms; the expand stays. */
  peek_close(&p, 4000);
  assert(p.closing);
  peek_values(&p, 4050, &v);
  assert(v.pop > 0 && v.pop < 0.5 && near(v.expand, 1));
  assert(peek_close_left(&p, 4050) == 50);
  assert(peek_close_left(&p, 4100) == 0);
  peek_values(&p, 4100, &v);
  assert(near(v.pop, 0) && !v.more);
}

static void test_reopen_mid_close(void) {
  struct peek p = fresh();
  struct peek_values v;
  assert(peek_open(&p, true, 0));
  peek_expand(&p, 2000);
  peek_close(&p, 3000);
  peek_values(&p, 3050, &v);
  double at = v.pop;
  /* Back up from where it was, details kept, no new delay. */
  assert(!peek_open(&p, false, 3050));
  assert(!p.closing);
  peek_values(&p, 3050, &v);
  assert(near(v.pop, at) && near(v.expand, 1));
  peek_values(&p, 3250, &v);
  assert(near(v.pop, 1));
}

static void test_hold_stops_on_close(void) {
  struct peek p = fresh();
  struct peek_values v;
  peek_open(&p, true, 1000); /* (0 means "not running": never a real time) */
  peek_close(&p, 1500);      /* released before the expand */
  peek_values(&p, 1900, &v);
  assert(near(v.hold, 0.25) && near(v.expand, 0));
  /* No hold line configured: always 0. */
  p = fresh();
  p.hold_line = false;
  peek_open(&p, true, 1000);
  peek_values(&p, 1500, &v);
  assert(near(v.hold, 0));
  /* No delay: no line, expand right away (the caller arms 1 ms). */
  p = fresh();
  p.expand_delay_ms = 0;
  assert(peek_open(&p, true, 1000));
  peek_values(&p, 1000, &v);
  assert(near(v.hold, 0));
}

static int wants, updates;
static const struct peek *last;
static bool want_yes(void) {
  wants++;
  return true;
}
static void update(const struct peek *p) {
  updates++;
  last = p;
}

static void test_sink(void) {
  assert(!peek_get_sink());
  static const struct peek_sink sink = {.want = want_yes, .update = update};
  peek_set_sink(&sink);
  assert(peek_get_sink() == &sink);
  assert(peek_get_sink()->want() && wants == 1);
  struct peek p = fresh();
  peek_get_sink()->update(&p);
  assert(updates == 1 && last == &p);
  peek_set_sink(NULL);
  assert(!peek_get_sink());
}

int main(void) {
  test_open_expand_close();
  test_reopen_mid_close();
  test_hold_stops_on_close();
  test_sink();
  printf("core peek: ok\n");
  return 0;
}
