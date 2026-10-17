/* The popup's timeline: it pops up, the expand delay runs (traced by the
 * hold line), the details reveal, and it closes. The popup owns one and
 * draws its panel from it; on an empty workspace it hands it to the
 * wallpaper (the sink), which draws the same contents instead. */
#ifndef SWELL_PEEK_H
#define SWELL_PEEK_H

#include <stdbool.h>
#include <stdint.h>

#include "info.h"

/* Times are now_ms() milliseconds. */
struct peek {
  const struct info *info; /* what to show */
  int battery_low_pct;     /* discharging at or below: a warning */
  /* from the popup's config */
  int pop_ms, close_ms, expand_ms, expand_delay_ms;
  bool hold_line; /* the expand delay is traced */
  /* the timeline */
  int64_t pop_at;          /* start of the pop tween */
  double pop_from, pop_to; /* 0 -> 1 open, x -> 0 close */
  bool closing;
  int64_t expand_at; /* 0 until the details are revealed */
  int64_t hold_at;   /* expand delay running since, 0 while not */
  double hold;       /* the hold line, kept as it was when it stopped */
};

/* Where the timeline is at now: pop (0 closed .. 1 open), expand (0 .. 1),
 * hold (0 .. 1 of the expand delay, linear); more while any still moves. */
struct peek_values {
  double pop, expand, hold;
  bool more;
};
void peek_values(const struct peek *p, int64_t now, struct peek_values *v);

/* Open at now: from closed if fresh, else from wherever a close got to
 * (keeping details already revealed). True if the expand delay must be
 * armed. */
bool peek_open(struct peek *p, bool fresh, int64_t now);
/* The expand delay ran out. */
void peek_expand(struct peek *p, int64_t now);
/* Start closing; the hold line stops where it got. */
void peek_close(struct peek *p, int64_t now);
/* ms left of a close (0: over), -1 when not closing. */
int64_t peek_close_left(const struct peek *p, int64_t now);

/* Whoever can draw the popup's contents instead of its panel. */
struct peek_sink {
  bool (*want)(void); /* takes a show starting now */
  /* p changed (or the clock ticked): draw it; NULL: no longer shown */
  void (*update)(const struct peek *p);
};
void peek_set_sink(const struct peek_sink *s); /* NULL: none */
const struct peek_sink *peek_get_sink(void);

#endif
