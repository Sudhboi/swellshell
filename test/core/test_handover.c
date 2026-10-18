/* Taking over from a replaced daemon (handover.c): its answer, and when it
 * is told to go. A child process stands in for the old daemon. */
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "handover.h"
#include "loop.h"

static pid_t old_daemon(void) {
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    pause();
    _exit(0);
  }
  return pid;
}

/* The child got SIGTERM (waiting up to 1 s), or is still there. */
static bool gone(pid_t pid) {
  for (int i = 0; i < 100; ++i) {
    int status;
    if (waitpid(pid, &status, WNOHANG) == pid)
      return WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM;
    usleep(10000);
  }
  return false;
}

static void answer(pid_t pid, const char *lines) {
  char buf[512];
  snprintf(buf, sizeof buf, "handover %d\n%s", (int)pid, lines);
  handover_parse(buf);
}

static void test_parse(void) {
  answer(4242, "wallpaper now=1 clear=2 angle=0.5 spin=0.2\nosd x\n");
  assert(strcmp(handover_get("wallpaper"), "now=1 clear=2 angle=0.5 spin=0.2\n"
                                           "osd x\n") == 0);
  assert(strcmp(handover_get("osd"), "x\n") == 0);
  assert(!handover_get("wall"));
  assert(!handover_get("media"));
  /* An older daemon answers nothing; a junk answer is no handover. */
  handover_parse(NULL);
  assert(!handover_get("wallpaper"));
  handover_parse("no\nwallpaper now=1\n");
  assert(!handover_get("wallpaper"));
  handover_finish();
}

static void test_unheld_goes_at_once(struct loop *loop) {
  pid_t pid = old_daemon();
  answer(pid, "");
  handover_settle(loop);
  assert(gone(pid));
  handover_finish();
}

static void test_goes_on_last_release(struct loop *loop) {
  pid_t pid = old_daemon();
  answer(pid, "wallpaper x\n");
  handover_hold();
  handover_hold();
  handover_release(); /* before settling: it can't go yet */
  handover_settle(loop);
  usleep(50000);
  assert(waitpid(pid, NULL, WNOHANG) == 0);
  handover_release();
  assert(gone(pid));
  handover_release(); /* one too many is harmless */
  handover_finish();
}

static void test_goes_when_the_daemon_stops(struct loop *loop) {
  pid_t pid = old_daemon();
  answer(pid, "");
  handover_hold();
  handover_settle(loop);
  handover_finish();
  assert(gone(pid));
}

static void on_quit(struct loop_timer *t, void *data) { loop_quit(data); }

static void test_goes_after_the_wait(struct loop *loop) {
  pid_t pid = old_daemon();
  answer(pid, "");
  handover_hold();
  handover_settle(loop);
  struct loop_timer *t = loop_add_timer(loop, CLOCK_MONOTONIC, on_quit, loop);
  loop_timer_arm(t, HANDOVER_WAIT_MS + 200, 0);
  loop_run(loop);
  loop_remove_timer(t);
  assert(gone(pid));
  handover_finish();
}

int main(void) {
  struct loop *loop = loop_create();
  test_parse();
  test_unheld_goes_at_once(loop);
  test_goes_on_last_release(loop);
  test_goes_when_the_daemon_stops(loop);
  test_goes_after_the_wait(loop);
  /* Without a handover nothing is held or signalled. */
  handover_parse(NULL);
  handover_hold();
  handover_settle(loop);
  handover_release();
  handover_finish();
  loop_destroy(loop);
  printf("handover: ok\n");
  return 0;
}
