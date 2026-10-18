#include "handover.h"

#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "util.h"

static struct {
  pid_t pid;    /* the old daemon, 0: none (or told to go) */
  char *answer; /* its lines, after the first */
  int holds;
  bool settled;
  struct loop_timer *timer;
} h;

void handover_parse(const char *answer) {
  free(h.answer);
  h.answer = NULL;
  h.pid = 0;
  if (!answer || strncmp(answer, "handover ", 9) != 0)
    return;
  long pid = strtol(answer + 9, NULL, 10);
  if (pid <= 0)
    return;
  h.pid = (pid_t)pid;
  const char *rest = strchr(answer, '\n');
  h.answer = xstrdup(rest ? rest + 1 : "");
}

const char *handover_get(const char *module) {
  size_t n = strlen(module);
  for (const char *l = h.answer; l && *l;) {
    if (strncmp(l, module, n) == 0 && l[n] == ' ')
      return l + n + 1;
    l = strchr(l, '\n');
    if (l)
      ++l;
  }
  return NULL;
}

static void let_go(void) {
  if (h.pid > 0)
    kill(h.pid, SIGTERM);
  h.pid = 0;
  if (h.timer)
    loop_timer_disarm(h.timer);
}

static void on_timer(struct loop_timer *t, void *data) { let_go(); }

void handover_hold(void) {
  if (h.pid > 0)
    ++h.holds;
}

void handover_release(void) {
  if (h.holds > 0 && --h.holds == 0 && h.settled)
    let_go();
}

void handover_settle(struct loop *loop) {
  h.settled = true;
  if (h.pid <= 0 || h.holds == 0) {
    let_go();
    return;
  }
  h.timer = loop_add_timer(loop, CLOCK_MONOTONIC, on_timer, NULL);
  loop_timer_arm(h.timer, HANDOVER_WAIT_MS, 0);
}

void handover_finish(void) {
  let_go();
  if (h.timer)
    loop_remove_timer(h.timer);
  free(h.answer);
  h = (typeof(h)){0};
}
