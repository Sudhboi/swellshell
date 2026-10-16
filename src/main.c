/* swell: a small sway/niri shell. `swell daemon` runs every module in one event
 * loop; every other invocation is a client that sends its arguments to the
 * daemon over $XDG_RUNTIME_DIR/swell.sock. */
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "bus.h"
#include "config.h"
#include "ipc.h"
#include "loop.h"
#include "module.h"
#include "wm.h"
#include "util.h"
#include "wl.h"

#ifndef SWELL_VERSION
#define SWELL_VERSION "dev"
#endif

struct module_state {
  void *cfg;
  bool enabled; /* per the config */
  bool running; /* init succeeded */
};

static struct {
  struct swell sw;
  struct cfg_doc doc;
  struct module_state *mods;
  time_t config_mtime;
} d;

/* ---- config ---- */

static void *build_config(const struct swell_module *m,
                          const struct cfg_doc *doc, bool *enabled) {
  void *cfg = xcalloc(1, m->config_size ? m->config_size : 1);
  if (m->config_size)
    memcpy(cfg, m->defaults, m->config_size);
  toml_datum_t sec = cfg_section(doc, m->name);

  const char *extra[32];
  int n = 0;
  extra[n++] = "enable";
  for (int i = 0; m->extra_keys && m->extra_keys[i] && n < 31; ++i)
    extra[n++] = m->extra_keys[i];
  if (n == 31 && m->extra_keys[n - 1])
    warn("%s: more than 30 extra config keys; raise the limit in "
         "build_config",
         m->name);
  extra[n] = NULL;
  cfg_apply(sec, m->name, m->fields, m->nfields, cfg, extra);

  *enabled = true;
  if (sec.type == TOML_TABLE) {
    toml_datum_t en = toml_get(sec, "enable");
    if (en.type == TOML_BOOLEAN)
      *enabled = en.u.boolean;
    else if (en.type != TOML_UNKNOWN)
      cfg_error(en, m->name, "enable", "expected true or false");
  }
  if (m->configure)
    m->configure(cfg, sec, m->name);
  return cfg;
}

static void free_config(const struct swell_module *m, void *cfg) {
  if (!cfg)
    return;
  if (m->free_config)
    m->free_config(cfg);
  free(cfg);
}

static void warn_unknown_sections(const struct cfg_doc *doc) {
  toml_datum_t top = doc->toml.toptab;
  for (int i = 0; top.type == TOML_TABLE && i < top.u.tab.size; ++i) {
    const char *key = top.u.tab.key[i];
    bool known = false;
    for (size_t j = 0; j < swell_nmodules; ++j)
      if (strcmp(swell_modules[j]->name, key) == 0)
        known = true;
    if (!known)
      cfg_error(top.u.tab.value[i], NULL, key, "unknown section (ignored)");
  }
}

static time_t file_mtime(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 ? st.st_mtime : 0;
}

/* ---- module lifecycle ---- */

static bool can_run(const struct swell_module *m) {
  if (m->needs_wayland && !d.sw.wl)
    return false;
  if (m->needs_wm && d.sw.wm == WM_NONE)
    return false;
  if (m->needs_sway && d.sw.wm != WM_SWAY)
    return false;
  return true;
}

static void start_module(size_t i) {
  const struct swell_module *m = swell_modules[i];
  struct module_state *ms = &d.mods[i];
  if (!ms->enabled || ms->running)
    return;
  if (!can_run(m)) {
    warn("%s: disabled (needs %s)", m->name,
         m->needs_wayland && !d.sw.wl ? "Wayland"
         : m->needs_sway              ? "sway"
                                      : "sway or niri");
    return;
  }
  ms->running = !m->init || m->init(&d.sw, ms->cfg);
  if (!ms->running)
    warn("%s: failed to start", m->name);
}

static void stop_module(size_t i) {
  const struct swell_module *m = swell_modules[i];
  if (!d.mods[i].running)
    return;
  if (m->finish)
    m->finish();
  d.mods[i].running = false;
}

static int reload_config(struct reply *r) {
  struct cfg_doc doc;
  cfg_load(&doc, d.doc.path);
  if (doc.broken) {
    /* Keep running with the current settings rather than silently
     * dropping back to the defaults. */
    reply_printf(r, "%s\nkept the current configuration\n", doc.error);
    cfg_free(&doc);
    return 1;
  }
  warn_unknown_sections(&doc);
  for (size_t i = 0; i < swell_nmodules; ++i) {
    const struct swell_module *m = swell_modules[i];
    struct module_state *ms = &d.mods[i];
    bool enabled;
    void *cfg = build_config(m, &doc, &enabled);
    void *old = ms->cfg;
    if (!enabled) {
      stop_module(i);
      ms->cfg = cfg;
    } else if (ms->running) {
      ms->cfg = cfg;
      if (m->reconfigure)
        m->reconfigure(cfg);
    } else {
      ms->cfg = cfg;
    }
    ms->enabled = enabled;
    free_config(m, old);
    start_module(i);
  }
  cfg_free(&d.doc);
  d.doc = doc;
  d.config_mtime = file_mtime(d.doc.path);
  reply_printf(r, "reloaded %s%s\n", d.doc.path,
               d.doc.loaded ? "" : " (not found; using the defaults)");
  return 0;
}

/* ---- commands ---- */

static void print_help(FILE *f, struct reply *r) {
  char *buf = NULL;
  size_t len = 0;
  FILE *mem = r ? open_memstream(&buf, &len) : f;
  fprintf(mem,
          "usage: swell <command> [args...]\n"
          "\n"
          "  daemon [-c FILE] [--no-replace]\n"
          "           run the shell; replaces a running daemon unless "
          "--no-replace\n"
          "  reload   re-read the config file\n"
          "  status   show the daemon's state\n"
          "  quit     stop the daemon\n"
          "  config [--defaults] [-c FILE]\n"
          "           print the effective (or default) configuration as TOML\n"
          "  help     this text\n");
  for (size_t i = 0; i < swell_nmodules; ++i) {
    const struct swell_module *m = swell_modules[i];
    fprintf(mem, "\n%s", m->name);
    for (int j = 0; m->aliases && m->aliases[j]; ++j)
      fprintf(mem, " | %s", m->aliases[j]);
    fprintf(mem, ":\n%s", m->usage ? m->usage : "");
  }
  fprintf(mem, "\nConfig: %s\nSocket: %s\n", cfg_default_path(),
          ipc_socket_path());
  if (r) {
    fclose(mem);
    reply_printf(r, "%s", buf);
    free(buf);
  }
}

static int handle(int argc, char **argv, struct reply *r, void *data) {
  const char *cmd = argv[0];
  if (strcmp(cmd, "quit") == 0) {
    loop_quit(d.sw.loop);
    return 0;
  }
  if (strcmp(cmd, "reload") == 0)
    return reload_config(r);
  if (strcmp(cmd, "status") == 0) {
    char when[64] = "-";
    if (d.config_mtime)
      strftime(when, sizeof(when), "%F %T", localtime(&d.config_mtime));
    reply_printf(r, "pid %d\nconfig %s (%s, mtime %s)\nwayland %s\nwm %s\n",
                 (int)getpid(), d.doc.path,
                 d.doc.loaded ? "loaded" : "defaults", when,
                 d.sw.wl ? "connected" : "no", wm_name(d.sw.wm));
    for (size_t i = 0; i < swell_nmodules; ++i)
      reply_printf(r, "module %s %s\n", swell_modules[i]->name,
                   d.mods[i].running   ? "running"
                   : d.mods[i].enabled ? "unavailable"
                                       : "disabled");
    return 0;
  }
  if (strcmp(cmd, "help") == 0) {
    print_help(NULL, r);
    return 0;
  }
  const struct swell_module *m = module_find(cmd);
  if (!m) {
    reply_printf(r, "unknown command '%s' (try 'swell help')\n", cmd);
    return 1;
  }
  size_t i = 0;
  while (swell_modules[i] != m)
    ++i;
  if (!d.mods[i].running) {
    reply_printf(r, "module %s is not running\n", m->name);
    return 1;
  }
  if (!m->command) {
    reply_printf(r, "module %s takes no commands\n", m->name);
    return 1;
  }
  return m->command(argc - 1, argv + 1, r);
}

/* ---- daemon ---- */

static int run_daemon(int argc, char **argv) {
  const char *config_path = NULL;
  bool replace = true;
  static const struct option opts[] = {
      {"config", required_argument, NULL, 'c'},
      {"no-replace", no_argument, NULL, 'n'},
      {"help", no_argument, NULL, 'h'},
      {0},
  };
  int c;
  optind = 1;
  while ((c = getopt_long(argc, argv, "c:h", opts, NULL)) != -1) {
    switch (c) {
    case 'c':
      config_path = optarg;
      break;
    case 'n':
      replace = false;
      break;
    default:
      fprintf(stderr, "usage: swell daemon [-c FILE] [--no-replace]\n");
      return c == 'h' ? 0 : 2;
    }
  }

  d.sw.loop = loop_create();
  d.sw.daemon = true;
  if (!ipc_server_start(d.sw.loop, ipc_socket_path(), replace, handle, NULL))
    return 1;

  cfg_load(&d.doc, config_path);
  warn_unknown_sections(&d.doc);
  d.config_mtime = file_mtime(d.doc.path);

  d.sw.wl = wl_create(d.sw.loop);
  d.sw.wm = wm_init(d.sw.loop);

  d.mods = xcalloc(swell_nmodules, sizeof(*d.mods));
  for (size_t i = 0; i < swell_nmodules; ++i)
    d.mods[i].cfg = build_config(swell_modules[i], &d.doc, &d.mods[i].enabled);
  for (size_t i = 0; i < swell_nmodules; ++i)
    start_module(i);

  loop_run(d.sw.loop);

  ipc_server_stop();
  for (size_t i = swell_nmodules; i-- > 0;) {
    stop_module(i);
    free_config(swell_modules[i], d.mods[i].cfg);
  }
  free(d.mods);
  wm_finish();
  wl_destroy(d.sw.wl);
  cfg_free(&d.doc);
  loop_destroy(d.sw.loop);
  return 0;
}

/* ---- client ---- */

static int print_config(int argc, char **argv) {
  const char *path = NULL;
  bool defaults = false;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--defaults") == 0)
      defaults = true;
    else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
      path = argv[++i];
    else {
      fprintf(stderr, "usage: swell config [--defaults] [-c FILE]\n");
      return 2;
    }
  }
  struct cfg_doc doc;
  if (defaults)
    cfg_load(&doc, "/dev/null");
  else
    cfg_load(&doc, path);
  warn_unknown_sections(&doc);
  printf("# swell configuration (%s)\n# Copy to ~/.config/swell/config.toml "
         "and edit; every key is optional.\n",
         defaults ? "defaults" : doc.path);
  for (size_t i = 0; i < swell_nmodules; ++i) {
    const struct swell_module *m = swell_modules[i];
    bool enabled;
    void *cfg = build_config(m, &doc, &enabled);
    printf("\n[%s]\n# false turns the module off\nenable = %s\n", m->name,
           enabled ? "true" : "false");
    cfg_dump(stdout, m->fields, m->nfields, cfg);
    if (m->dump_extra)
      m->dump_extra(stdout, cfg);
    free_config(m, cfg);
  }
  cfg_free(&doc);
  return 0;
}

static int send_or_local(int argc, char **argv) {
  const struct swell_module *m = module_find(argv[0]);
  const char *sock = ipc_socket_path();
  struct swell local = {0};
  void *cfg = NULL;
  struct cfg_doc doc = {0};
  int ret = -1;

  if (m && m->run_local) {
    cfg_load(&doc, NULL);
    bool enabled;
    cfg = build_config(m, &doc, &enabled);
    local.loop = loop_create();
    /* A client keeps the default signal behaviour (loop_create blocked
     * SIGINT/SIGTERM for the daemon's signalfd), so Ctrl-C works. */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_UNBLOCK, &mask, NULL);
    ret = m->run_local(&local, cfg, true, argc - 1, argv + 1);
  }
  if (ret < 0) {
    struct reply r = {0};
    bool ok = false;
    if (ipc_client_send(sock, argc, argv, &r, &ok) == 0) {
      fputs(r.buf ? r.buf : "", ok ? stdout : stderr);
      ret = ok ? 0 : 1;
    } else if (m && m->run_local) {
      ret = m->run_local(&local, cfg, false, argc - 1, argv + 1);
      if (ret < 0) {
        fprintf(stderr, "swell: no daemon is listening on %s\n", sock);
        ret = 1;
      }
    } else {
      fprintf(stderr, "swell: no daemon is listening on %s (start it with "
                      "'swell daemon')\n",
              sock);
      ret = 1;
    }
    reply_free(&r);
  }
  if (cfg)
    free_config(m, cfg);
  if (local.loop) {
    wm_finish();
    wl_destroy(local.wl);
    loop_destroy(local.loop);
    cfg_free(&doc);
  }
  return ret;
}

int main(int argc, char **argv) {
  setvbuf(stderr, NULL, _IOLBF, 0);
  if (argc < 2 || strcmp(argv[1], "help") == 0 ||
      strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
    print_help(argc < 2 ? stderr : stdout, NULL);
    return argc < 2 ? 2 : 0;
  }
  if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "version") == 0) {
    printf("swell %s\n", SWELL_VERSION);
    return 0;
  }
  if (strcmp(argv[1], "daemon") == 0)
    return run_daemon(argc - 1, argv + 1);
  if (strcmp(argv[1], "config") == 0)
    return print_config(argc - 1, argv + 1);
  return send_or_local(argc - 1, argv + 1);
}
