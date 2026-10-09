/* What the core hands every module. */
#ifndef SWELL_SWELL_H
#define SWELL_SWELL_H

#include <stdbool.h>

#include "loop.h"
#include "wl.h"

struct swell {
  struct loop *loop;
  struct swell_wl *wl; /* NULL without a Wayland connection */
  bool sway;           /* sway IPC is connected (see sway.h) */
  bool daemon;         /* false for one-shot local commands */
};

#endif
