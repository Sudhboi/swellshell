/* The playerctl poller against a fake playerctl on PATH: one poll for the
 * state now, then one --follow child whose lines come in as they're
 * printed, respawned if it exits, killed on stop. */
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "loop.h"
#include "mpris.h"

static char dir[] = "/tmp/swell-test-mpris-XXXXXX";
static char outs[32][64];
static int nouts, nchanged;

static const char *parsed(const char *out, void *data) {
  assert(nouts < 32);
  snprintf(outs[nouts++], sizeof outs[0], "%s", out);
  return "";
}

static void changed(void *data) { nchanged++; }

static const struct mpris_ops ops = {
    .format = "{{status}}",
    .art_tag = "swell-test-art",
    .parsed = parsed,
    .changed = changed,
};

static void on_quit(struct loop_timer *t, void *data) { loop_quit(data); }

/* A loop runs once (loop_quit is final), so each case has its own. */
static void run_ms(struct loop *loop, int ms) {
  struct loop_timer *t = loop_add_timer(loop, CLOCK_MONOTONIC, on_quit, loop);
  loop_timer_arm(t, ms, 0);
  loop_run(loop);
  loop_remove_timer(t);
}

static struct mpris m;

static void on_repoll(struct loop_timer *t, void *data) { mpris_repoll(&m); }

/* Killed: a zombie (nothing reaps it any more) or gone. */
static void assert_killed(pid_t pid) {
  usleep(100000);
  char path[64], stat[256] = "";
  snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
  FILE *f = fopen(path, "r");
  if (f) {
    assert(fgets(stat, sizeof stat, f));
    fclose(f);
    assert(strstr(stat, ") Z "));
    waitpid(pid, NULL, 0);
  }
}

static int calls(const char *what) {
  char path[256], line[256];
  snprintf(path, sizeof path, "%s/calls", dir);
  FILE *f = fopen(path, "r");
  int n = 0;
  if (!f)
    return 0;
  while (fgets(line, sizeof line, f))
    n += strstr(line, what) != NULL;
  fclose(f);
  return n;
}

/* The follower's lines, in order, from outs[from]. */
static void assert_follow(int from) {
  assert(nouts - from >= 3);
  assert(!strcmp(outs[from], "follow 1\n"));
  assert(!strcmp(outs[from + 1], "follow 2\n"));
  assert(!strcmp(outs[from + 2], "\n")); /* the player went */
}

int main(void) {
  assert(mkdtemp(dir));
  char path[256];
  snprintf(path, sizeof path, "%s/playerctl", dir);
  FILE *f = fopen(path, "w");
  assert(f);
  fprintf(f,
          "#!/bin/sh\n"
          "echo \"$*\" >>'%s/calls'\n"
          "case \" $* \" in\n"
          "*' --follow '*)\n"
          "  echo 'follow 1'; sleep 0.1; echo 'follow 2'; sleep 0.1; echo\n"
          "  [ -e '%s/exit' ] && exit 0\n"
          "  exec sleep 100 ;;\n"
          "*) sleep 0.3; echo oneshot ;;\n"
          "esac\n",
          dir, dir);
  fclose(f);
  chmod(path, 0755);
  char env[4096];
  snprintf(env, sizeof env, "%s:%s", dir, getenv("PATH"));
  setenv("PATH", env, 1);

  /* Many intervals pass: still one poll and one follower. The poll's
   * answer, later than the follower's first line, is older: dropped. */
  struct loop *loop = loop_create();
  mpris_init(&m, loop, &ops, NULL);
  mpris_configure(&m, "", 1000);
  mpris_start(&m, 100);
  run_ms(loop, 1000);
  assert(calls("--follow") == 1);
  assert(calls("metadata") == 2);
  assert_follow(0);
  assert(nouts == 3 && nchanged == 3);
  /* Stop kills the follower. */
  pid_t pid = m.fpid;
  assert(pid > 0);
  mpris_stop(&m);
  assert(!mpris_running(&m) && m.fpid == 0 && m.ffd < 0);
  assert_killed(pid);
  loop_destroy(loop);

  /* A follower that exits comes back on the next interval. */
  snprintf(path, sizeof path, "%s/exit", dir);
  fclose(fopen(path, "w"));
  nouts = nchanged = 0;
  loop = loop_create();
  mpris_init(&m, loop, &ops, NULL);
  mpris_start(&m, 500);
  run_ms(loop, 1300);
  assert(calls("--follow") == 1 + 3);
  assert(nouts == 9);
  assert_follow(0);
  assert_follow(3);
  assert_follow(6);
  mpris_stop(&m);
  loop_destroy(loop);

  /* A repoll restarts both: the state now, even without any change. */
  unlink(path);
  nouts = nchanged = 0;
  loop = loop_create();
  mpris_init(&m, loop, &ops, NULL);
  mpris_start(&m, 100);
  struct loop_timer *t = loop_add_timer(loop, CLOCK_MONOTONIC, on_repoll, NULL);
  loop_timer_arm(t, 50, 0);
  run_ms(loop, 600);
  assert(calls("--follow") == 4 + 2);
  /* The first follower's first line (maybe), then all of the second's. */
  assert(nouts == 3 || nouts == 4);
  assert_follow(nouts - 3);
  pid = m.fpid;
  mpris_stop(&m);
  assert_killed(pid);
  loop_remove_timer(t);
  loop_destroy(loop);

  /* The daemon killed outright: its follower doesn't outlive it. */
  int p[2];
  assert(pipe(p) == 0);
  pid_t daemon = fork();
  if (daemon == 0) {
    loop = loop_create();
    mpris_init(&m, loop, &ops, NULL);
    mpris_start(&m, 100);
    assert(write(p[1], &m.fpid, sizeof m.fpid) == sizeof m.fpid);
    run_ms(loop, 10000);
    _exit(0);
  }
  assert(read(p[0], &pid, sizeof pid) == sizeof pid && pid > 0);
  usleep(600000); /* done printing: no SIGPIPE will end it */
  kill(daemon, SIGKILL);
  waitpid(daemon, NULL, 0);
  int i = 0;
  while (kill(pid, 0) == 0 && i++ < 50) {
    char path[64], stat[256] = "";
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    FILE *sf = fopen(path, "r");
    if (sf && fgets(stat, sizeof stat, sf) && strstr(stat, ") Z ")) {
      fclose(sf);
      break; /* dead, waiting for its new parent to reap it */
    }
    if (sf)
      fclose(sf);
    usleep(20000);
  }
  assert(i <= 50);
  char cmd[512];
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
  assert(system(cmd) == 0);
  return 0;
}
