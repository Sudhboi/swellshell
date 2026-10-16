/* runner: run shell commands when events happen (formerly awww-kill, which
 * paused the awww wallpaper daemon while the focused workspace had
 * windows). There are no hooks unless the config defines some.
 *
 * Events come from the bus: the window manager's (workspace.*, window.*,
 * output.change on sway,
 * and the workspace.empty/occupied transitions) and every module's
 * (osd.show, popup.hide, overview.open, ...). Matching hooks are queued and
 * run one command at a time, so a "pause" always finishes before a later
 * "unpause" starts. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>

#include "bus.h"
#include "hooks.h"
#include "module.h"
#include "wm.h"
#include "util.h"

struct runner_config {
  int timeout_ms;
  bool log;
  struct hook_list hooks;
};

static const struct runner_config defaults = {
    .timeout_ms = 10000,
    .log = false,
};

static const struct cfg_field fields[] = {
    CFG_INT(struct runner_config, timeout_ms, 0, 3600000,
            "move on to the next queued command if one runs longer than this "
            "(it keeps running); 0 waits forever"),
    CFG_BOOL(struct runner_config, log,
             "print every command the runner starts to stderr"),
};

static const char *const extra_keys[] = {"hook", NULL};

static void configure(void *cfgp, toml_datum_t section, const char *name) {
  struct runner_config *cfg = cfgp;
  cfg->hooks = (struct hook_list){0};
  hooks_parse(&cfg->hooks, section, name);
}

static void free_config(void *cfgp) {
  hooks_free(&((struct runner_config *)cfgp)->hooks);
}

static void dump_extra(FILE *f, const void *cfgp) {
  hooks_dump(f, &((const struct runner_config *)cfgp)->hooks);
}

/* ---- the queue ---- */

struct job {
  char *cmd;
  char **env; /* NULL-terminated, owned */
  bool detach;
};

static struct {
  struct swell *sw;
  struct runner_config cfg; /* hooks deep-copied */
  struct job *queue;
  int nqueue;
  pid_t running; /* the command being waited for, 0 if none */
  struct loop_timer *timeout;
} rn;

static void job_free(struct job *j) {
  free(j->cmd);
  for (int i = 0; j->env && j->env[i]; ++i)
    free(j->env[i]);
  free(j->env);
}

static void run_next(void);

static void on_exit_child(pid_t pid, int status, void *data) {
  if (rn.cfg.log || (status != -1 && WIFEXITED(status) &&
                     WEXITSTATUS(status) == 127))
    warn("runner: pid %d exited with status %d", (int)pid,
         WIFEXITED(status) ? WEXITSTATUS(status) : -1);
  if (pid != rn.running)
    return; /* it had timed out; the queue already moved on */
  rn.running = 0;
  loop_timer_disarm(rn.timeout);
  run_next();
}

static void on_timeout(struct loop_timer *t, void *data) {
  warn("runner: pid %d still running after %d ms; not waiting any longer",
       (int)rn.running, rn.cfg.timeout_ms);
  rn.running = 0;
  run_next();
}

static void run_next(void) {
  while (!rn.running && rn.nqueue > 0) {
    struct job j = rn.queue[0];
    memmove(rn.queue, rn.queue + 1, --rn.nqueue * sizeof(*rn.queue));
    if (rn.cfg.log)
      warn("runner: %s%s", j.detach ? "(detached) " : "", j.cmd);
    if (j.detach) {
      spawn_shell_detached(j.cmd, j.env);
    } else {
      pid_t pid = spawn_shell(j.cmd, j.env);
      if (pid > 0) {
        if (loop_watch_child(rn.sw->loop, pid, on_exit_child, NULL)) {
          rn.running = pid;
          if (rn.cfg.timeout_ms > 0)
            loop_timer_arm(rn.timeout, rn.cfg.timeout_ms, 0);
        } else {
          waitpid(pid, NULL, 0);
        }
      }
    }
    job_free(&j);
  }
}

static void enqueue(const struct hook *h, const struct bus_event *ev) {
  int nenv = 0;
  while (ev->env[nenv])
    ++nenv;
  for (int c = 0; c < h->nrun; ++c) {
    struct job j = {.cmd = xstrdup(h->run[c]), .detach = h->detach};
    j.env = xcalloc(nenv + 2, sizeof(char *));
    j.env[0] = xasprintf("SWELL_EVENT=%s", ev->name);
    for (int i = 0; i < nenv; ++i)
      j.env[i + 1] = xstrdup(ev->env[i]);
    rn.queue = xrealloc(rn.queue, (rn.nqueue + 1) * sizeof(*rn.queue));
    rn.queue[rn.nqueue++] = j;
  }
}

static void on_event(const struct bus_event *ev, void *data) {
  bool any = false;
  for (int i = 0; i < rn.cfg.hooks.n; ++i) {
    if (hook_matches(&rn.cfg.hooks.hooks[i], ev->name)) {
      enqueue(&rn.cfg.hooks.hooks[i], ev);
      any = true;
    }
  }
  if (any)
    run_next();
}

/* ---- module ---- */

static void copy_config(const struct runner_config *src) {
  hooks_free(&rn.cfg.hooks);
  rn.cfg = *src;
  rn.cfg.hooks = (struct hook_list){0};
  /* deep copy: the core frees src after reconfigure */
  for (int i = 0; i < src->hooks.n; ++i) {
    const struct hook *h = &src->hooks.hooks[i];
    struct hook c = {.non = h->non, .nrun = h->nrun, .detach = h->detach};
    c.on = xcalloc(h->non, sizeof(char *));
    c.run = xcalloc(h->nrun, sizeof(char *));
    for (int k = 0; k < h->non; ++k)
      c.on[k] = xstrdup(h->on[k]);
    for (int k = 0; k < h->nrun; ++k)
      c.run[k] = xstrdup(h->run[k]);
    rn.cfg.hooks.hooks =
        xrealloc(rn.cfg.hooks.hooks, (i + 1) * sizeof(*rn.cfg.hooks.hooks));
    rn.cfg.hooks.hooks[i] = c;
    rn.cfg.hooks.n = i + 1;
  }
}

static bool init(struct swell *sw, const void *cfg) {
  rn.sw = sw;
  copy_config(cfg);
  rn.timeout = loop_add_timer(sw->loop, CLOCK_MONOTONIC, on_timeout, NULL);
  bus_subscribe(on_event, NULL);
  /* Hooks see the current workspace.empty/occupied state when the runner
   * starts, including when `swell reload` turns it on. */
  wm_emit_focused_state();
  return true;
}

static void reconfigure(const void *cfg) { copy_config(cfg); }

static void finish(void) {
  bus_unsubscribe(on_event, NULL);
  loop_remove_timer(rn.timeout);
  rn.timeout = NULL;
  for (int i = 0; i < rn.nqueue; ++i)
    job_free(&rn.queue[i]);
  free(rn.queue);
  rn.queue = NULL;
  rn.nqueue = 0;
  rn.running = 0;
  hooks_free(&rn.cfg.hooks);
}

static int command(int argc, char **argv, struct reply *r) {
  if (argc >= 2 && strcmp(argv[0], "fire") == 0) {
    char *env[32];
    int n = 0;
    if (argc - 2 > 31) {
      reply_printf(r, "too many KEY=VALUE pairs (at most 31)\n");
      return 1;
    }
    for (int i = 2; i < argc; ++i) {
      if (!strchr(argv[i], '=')) {
        reply_printf(r, "expected KEY=VALUE, got '%s'\n", argv[i]);
        return 1;
      }
      env[n++] = argv[i];
    }
    env[n] = NULL;
    bus_emitv(argv[1], env);
    return 0;
  }
  if (argc == 1 && strcmp(argv[0], "list") == 0) {
    for (int i = 0; i < rn.cfg.hooks.n; ++i) {
      const struct hook *h = &rn.cfg.hooks.hooks[i];
      for (int k = 0; k < h->non; ++k)
        reply_printf(r, "%s%s", k ? "," : "", h->on[k]);
      reply_printf(r, "%s:", h->detach ? " (detached)" : "");
      for (int k = 0; k < h->nrun; ++k)
        reply_printf(r, " [%s]", h->run[k]);
      reply_printf(r, "\n");
    }
    return 0;
  }
  reply_printf(r, "usage: swell runner fire EVENT [KEY=VALUE...] | list\n");
  return 1;
}

const struct swell_module runner_module = {
    .name = "runner",
    .usage = "  fire EVENT [KEY=VALUE...]   emit an event (runs matching hooks)\n"
             "  list                        show the configured hooks\n",
    .config_size = sizeof(struct runner_config),
    .defaults = &defaults,
    .fields = fields,
    .nfields = ARRAY_LEN(fields),
    .extra_keys = extra_keys,
    .configure = configure,
    .free_config = free_config,
    .dump_extra = dump_extra,
    .init = init,
    .reconfigure = reconfigure,
    .finish = finish,
    .command = command,
};
