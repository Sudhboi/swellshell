/* The inhibitors the screensaver service holds: one per Inhibit call,
 * named by its cookie, gone on UnInhibit or when its sender leaves the
 * bus. Pure, so it can be tested; inhibit.c does the D-Bus side. */
#ifndef SWELL_INHIBIT_LIST_H
#define SWELL_INHIBIT_LIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct inhibitor {
  uint32_t cookie;
  char sender[64]; /* unique bus name, ":1.42" */
  char app[64];
  char reason[96];
  void *track; /* inhibit.c's sd_bus_track */
};

struct inhibit_list {
  struct inhibitor *v;
  size_t n, cap;
  uint32_t next; /* the next cookie handed out (never 0) */
};

/* Adds one and returns it (its cookie unused by the others). */
struct inhibitor *inhibit_list_add(struct inhibit_list *l, const char *sender,
                                   const char *app, const char *reason);
struct inhibitor *inhibit_list_find(struct inhibit_list *l, uint32_t cookie);
/* Removes the one with cookie into *out (may be NULL); false if none. */
bool inhibit_list_remove(struct inhibit_list *l, uint32_t cookie,
                         struct inhibitor *out);
void inhibit_list_free(struct inhibit_list *l);

/* For a handover: "next=N" then "COOKIE SENDER APP REASON" per inhibitor,
 * space separated (app and reason with blanks as '_'). Free the result. */
char *inhibit_list_encode(const struct inhibit_list *l);
/* Adds what encode wrote (tracks NULL); returns how many. */
size_t inhibit_list_decode(struct inhibit_list *l, const char *s);

#endif
