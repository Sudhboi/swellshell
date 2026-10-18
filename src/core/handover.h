/* Replacing a running daemon (`swell daemon` again, e.g. on a sway reload)
 * without a gap on screen. The new daemon asks the old one to `quit
 * handover`; the old one answers "handover PID" and the lines its modules'
 * handover hooks write (module.h), lets go of the socket, and stops every
 * module but those still showing something, which stay until the new
 * daemon has drawn over them (it sends SIGTERM) or their own timeout. */
#ifndef SWELL_HANDOVER_H
#define SWELL_HANDOVER_H

#include "loop.h"

/* At most this long after the new daemon's modules start, the old one is
 * told to go even if a module never released it. */
#define HANDOVER_WAIT_MS 3000

/* The new daemon: the old one's answer to `quit handover` (NULL: none). */
void handover_parse(const char *answer);
/* The rest of the old daemon's line that starts with "module ", or NULL. */
const char *handover_get(const char *module);
/* A module will draw over what the old daemon shows; release once it has
 * (it is then on screen). Both are no-ops without a handover. */
void handover_hold(void);
void handover_release(void);
/* After the modules started: the old daemon goes now unless held, else on
 * the last release or HANDOVER_WAIT_MS. */
void handover_settle(struct loop *loop);
/* The daemon stops: the old one goes now, if it hasn't. */
void handover_finish(void);

#endif
