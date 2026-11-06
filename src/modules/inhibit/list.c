#include "list.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

static uint32_t next_cookie(struct inhibit_list *l) {
  for (;;) {
    uint32_t c = l->next++;
    if (l->next == 0)
      l->next = 1;
    if (c != 0 && !inhibit_list_find(l, c))
      return c;
  }
}

/* Blanks would split the handover line; an empty word would too. */
static void copy_word(char *dst, size_t n, const char *src) {
  copy_str(dst, n, src && *src ? src : "?");
  for (char *p = dst; *p; ++p)
    if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
      *p = '_';
}

struct inhibitor *inhibit_list_add(struct inhibit_list *l, const char *sender,
                                   const char *app, const char *reason) {
  if (l->n == l->cap) {
    l->cap = l->cap ? 2 * l->cap : 8;
    l->v = xrealloc(l->v, l->cap * sizeof *l->v);
  }
  if (l->next == 0)
    l->next = 1;
  struct inhibitor *in = &l->v[l->n];
  *in = (struct inhibitor){.cookie = next_cookie(l)};
  ++l->n;
  copy_word(in->sender, sizeof in->sender, sender);
  copy_word(in->app, sizeof in->app, app);
  copy_word(in->reason, sizeof in->reason, reason);
  return in;
}

struct inhibitor *inhibit_list_find(struct inhibit_list *l, uint32_t cookie) {
  for (size_t i = 0; i < l->n; ++i)
    if (l->v[i].cookie == cookie)
      return &l->v[i];
  return NULL;
}

bool inhibit_list_remove(struct inhibit_list *l, uint32_t cookie,
                         struct inhibitor *out) {
  struct inhibitor *in = inhibit_list_find(l, cookie);
  if (!in)
    return false;
  if (out)
    *out = *in;
  size_t i = (size_t)(in - l->v);
  memmove(&l->v[i], &l->v[i + 1], (l->n - i - 1) * sizeof *l->v);
  --l->n;
  return true;
}

void inhibit_list_free(struct inhibit_list *l) {
  free(l->v);
  *l = (struct inhibit_list){0};
}

char *inhibit_list_encode(const struct inhibit_list *l) {
  char *s = xasprintf("next=%u", l->next);
  for (size_t i = 0; i < l->n; ++i) {
    const struct inhibitor *in = &l->v[i];
    char *t = xasprintf("%s %u %s %s %s", s, in->cookie, in->sender, in->app,
                        in->reason);
    free(s);
    s = t;
  }
  return s;
}

size_t inhibit_list_decode(struct inhibit_list *l, const char *s) {
  unsigned next;
  int off = 0;
  if (!s || sscanf(s, "next=%u%n", &next, &off) != 1)
    return 0;
  s += off;
  size_t added = 0;
  unsigned cookie;
  char sender[64], app[64], reason[96];
  while (sscanf(s, " %u %63s %63s %95s%n", &cookie, sender, app, reason,
                &off) == 4) {
    s += off;
    if (cookie == 0 || inhibit_list_find(l, cookie))
      continue;
    uint32_t keep = l->next;
    l->next = cookie;
    inhibit_list_add(l, sender, app, reason);
    l->next = keep;
    ++added;
  }
  l->next = next ? next : 1;
  return added;
}
