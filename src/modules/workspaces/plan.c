#include "plan.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ws_plan_renumber(const char *const *names, int n, int *want) {
  int count = 0;
  for (int i = 0; i < n; ++i) {
    char target[16];
    snprintf(target, sizeof(target), "%d", i + 1);
    want[i] = strcmp(names[i], target) == 0 ? 0 : i + 1;
    count += want[i] != 0;
  }
  return count;
}

struct buf {
  char *s;
  size_t len, cap;
};

static void put(struct buf *b, const char *s, size_t n) {
  if (b->len + n + 1 > b->cap) {
    b->cap = (b->len + n + 1) * 2;
    b->s = realloc(b->s, b->cap);
    if (!b->s)
      abort();
  }
  memcpy(b->s + b->len, s, n);
  b->len += n;
  b->s[b->len] = '\0';
}

static void put_str(struct buf *b, const char *s) { put(b, s, strlen(s)); }

/* A double-quoted sway argument. */
static void put_quoted(struct buf *b, const char *s) {
  put(b, "\"", 1);
  for (; *s; ++s) {
    if (*s == '"' || *s == '\\')
      put(b, "\\", 1);
    put(b, s, 1);
  }
  put(b, "\"", 1);
}

static void put_rename(struct buf *b, const char *from, const char *to) {
  if (b->len)
    put_str(b, "; ");
  put_str(b, "rename workspace ");
  put_quoted(b, from);
  put_str(b, " to ");
  put_quoted(b, to);
}

char *ws_plan_commands(const char *const *names, int n) {
  int *want = calloc(n ? n : 1, sizeof(*want));
  if (!want)
    abort();
  if (ws_plan_renumber(names, n, want) == 0) {
    free(want);
    return NULL;
  }
  /* Renaming in order works unless a target is still held at that moment
   * by a workspace that hasn't been renamed yet; simulate it. */
  bool clash = false;
  char(*cur)[16] = calloc(n, sizeof(*cur));
  const char **now = calloc(n, sizeof(*now));
  if (!cur || !now)
    abort();
  for (int i = 0; i < n; ++i)
    now[i] = names[i];
  for (int i = 0; i < n && !clash; ++i) {
    if (!want[i])
      continue;
    snprintf(cur[i], sizeof(cur[i]), "%d", want[i]);
    for (int j = 0; j < n; ++j)
      if (j != i && strcmp(now[j], cur[i]) == 0)
        clash = true;
    now[i] = cur[i];
  }
  free(cur);
  free(now);

  struct buf b = {0};
  char target[16], tmp[32];
  if (clash) {
    for (int i = 0; i < n; ++i) {
      if (!want[i])
        continue;
      snprintf(tmp, sizeof(tmp), "swell-renumber-%d", i);
      put_rename(&b, names[i], tmp);
    }
  }
  for (int i = 0; i < n; ++i) {
    if (!want[i])
      continue;
    snprintf(tmp, sizeof(tmp), "swell-renumber-%d", i);
    snprintf(target, sizeof(target), "%d", want[i]);
    put_rename(&b, clash ? tmp : names[i], target);
  }
  free(want);
  return b.s;
}

int ws_parse_number(const char *s) {
  size_t len = strlen(s);
  if (len == 0 || len > 9 || strspn(s, "0123456789") != len)
    return 0;
  return atoi(s);
}
