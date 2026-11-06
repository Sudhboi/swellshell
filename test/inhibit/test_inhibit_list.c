/* The screensaver inhibitors: cookies, removal, the handover line. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inhibit/list.h"

int main(void) {
  struct inhibit_list l = {0};
  struct inhibitor *a = inhibit_list_add(&l, ":1.5", "Zen", "video-playing");
  uint32_t ca = a->cookie;
  struct inhibitor *b = inhibit_list_add(&l, ":1.9", "", "two words");
  uint32_t cb = b->cookie;
  assert(ca != 0 && cb != 0 && ca != cb);
  assert(l.n == 2);
  /* Blanks and empty words can't split the handover line. */
  b = inhibit_list_find(&l, cb);
  assert(strcmp(b->app, "?") == 0 && strcmp(b->reason, "two_words") == 0);

  /* Unknown cookie: nothing. */
  assert(!inhibit_list_remove(&l, 12345, NULL));
  struct inhibitor out;
  assert(inhibit_list_remove(&l, ca, &out) && out.cookie == ca);
  assert(strcmp(out.sender, ":1.5") == 0);
  assert(l.n == 1 && !inhibit_list_find(&l, ca));

  /* Cookies never repeat a live one, wrap past 0. */
  l.next = 0xffffffffu;
  uint32_t c1 = inhibit_list_add(&l, ":1.1", "x", "y")->cookie;
  uint32_t c2 = inhibit_list_add(&l, ":1.1", "x", "y")->cookie;
  assert(c1 == 0xffffffffu && c2 == 1);
  l.next = cb;
  uint32_t c3 = inhibit_list_add(&l, ":1.1", "x", "y")->cookie;
  assert(c3 != cb && c3 != 0);

  /* Handover: the new daemon gets the same cookies and senders. */
  char *s = inhibit_list_encode(&l);
  struct inhibit_list n = {0};
  assert(inhibit_list_decode(&n, s) == l.n);
  assert(n.next == l.next);
  for (size_t i = 0; i < l.n; ++i) {
    struct inhibitor *x = inhibit_list_find(&n, l.v[i].cookie);
    assert(x && strcmp(x->sender, l.v[i].sender) == 0);
    assert(strcmp(x->app, l.v[i].app) == 0);
    assert(strcmp(x->reason, l.v[i].reason) == 0);
  }
  /* New cookies after it don't collide. */
  uint32_t c4 = inhibit_list_add(&n, ":1.2", "a", "b")->cookie;
  for (size_t i = 0; i < l.n; ++i)
    assert(c4 != l.v[i].cookie);
  free(s);
  /* Empty list and garbage. */
  struct inhibit_list e = {0};
  s = inhibit_list_encode(&e);
  assert(inhibit_list_decode(&n, s) == 0);
  free(s);
  assert(inhibit_list_decode(&n, "junk") == 0);
  assert(inhibit_list_decode(&n, NULL) == 0);
  inhibit_list_free(&n);
  inhibit_list_free(&l);
  printf("inhibit list: ok\n");
  return 0;
}
