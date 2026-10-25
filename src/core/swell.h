/* What the core hands every module. */
#ifndef SWELL_SWELL_H
#define SWELL_SWELL_H

#include <stdbool.h>

#include "loop.h"
#include "wl.h"
#include "wm.h"

struct reply;

struct swell {
  struct loop *loop;
  struct swell_wl *wl; /* NULL without a Wayland connection */
  enum wm_kind wm;     /* the connected window manager (see wm.h) */
  bool daemon;         /* false for one-shot local commands */
  /* Run a command as if it came over the socket (`swell ARGV...`); daemon
   * only. Returns its exit status. */
  int (*command)(int argc, char **argv, struct reply *r);
};

#endif
