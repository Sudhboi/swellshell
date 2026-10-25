/* gestures: touchpad swipes for compositors that can't bind them (labwc).
 *
 * Reads only the touchpads (libinput's path backend over the udev
 * enumeration, with hotplug) and runs a command per swipe direction once
 * the swipe ends. The compositor still gets the same events; without a
 * binding of its own it passes them to the focused window.
 *
 * Opening a touchpad needs access to its /dev/input node. The udev rule in
 * config/70-swell-touchpad.rules gives the logged-in user just that (not
 * the keyboard). sway and niri bind swipes themselves, so there the module
 * stays idle unless `always` is set. */
#include <errno.h>
#include <fcntl.h>
#include <libinput.h>
#include <libudev.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bus.h"
#include "config.h"
#include "ipc.h"
#include "module.h"
#include "swipe.h"
#include "util.h"

#define MAX_WORDS 16

struct gestures_config {
  int fingers;
  double threshold;
  bool always;
  char swipe_left[256], swipe_right[256], swipe_up[256], swipe_down[256];
};

static const struct gestures_config defaults = {
    .fingers = 3,
    .threshold = 300,
    .always = false,
    .swipe_left = "ws next",
    .swipe_right = "ws prev",
    .swipe_up = "overview open",
    .swipe_down = "overview close",
};

#define S struct gestures_config
static const struct cfg_field fields[] = {
    CFG_INT(S, fingers, 0, 5, "fingers a swipe takes (0: any number)"),
    CFG_DOUBLE(S, threshold, 1, 100000,
               "how far a swipe must move, in 1/1000 inch on the touchpad"),
    CFG_BOOL(S, always,
             "also run under sway and niri (they bind swipes themselves)"),
    CFG_STRING(S, swipe_left,
               "a swell command (\"ws next\"), \"exec CMD\" for a shell "
               "command, or empty"),
    CFG_STRING(S, swipe_right, "as swipe_left"),
    CFG_STRING(S, swipe_up, "as swipe_left"),
    CFG_STRING(S, swipe_down, "as swipe_left"),
};
#undef S

struct device {
  char *node;
  struct libinput_device *dev;
};

static struct {
  struct swell *sw;
  struct gestures_config cfg;
  bool active; /* reading touchpads (not idle under sway/niri) */
  struct udev *udev;
  struct udev_monitor *monitor;
  struct libinput *li;
  struct loop_watch *li_watch, *udev_watch;
  struct device *devs;
  int ndevs;
  int denied; /* touchpads we may not open */

  /* the swipe in progress */
  bool swiping;
  int fingers;
  double dx, dy;
  enum swipe_dir last;
  int last_fingers;
} g;

/* ---- running a swipe's command ---- */

static void run(const char *cmd) {
  while (*cmd == ' ')
    ++cmd;
  if (!*cmd)
    return;
  if (strncmp(cmd, "exec ", 5) == 0) {
    spawn_shell_detached(cmd + 5, NULL);
    return;
  }
  char *copy = xstrdup(cmd);
  char *argv[MAX_WORDS];
  int argc = 0;
  for (char *w = strtok(copy, " \t"); w && argc < MAX_WORDS;
       w = strtok(NULL, " \t"))
    argv[argc++] = w;
  struct reply r = {0};
  if (argc && g.sw->command(argc, argv, &r) != 0)
    warn("gestures: '%s': %s", cmd, r.buf ? r.buf : "failed");
  reply_free(&r);
  free(copy);
}

static void swiped(enum swipe_dir d) {
  g.last = d;
  g.last_fingers = g.fingers;
  char *e1 = xasprintf("SWELL_SWIPE=%s", swipe_name(d));
  char *e2 = xasprintf("SWELL_FINGERS=%d", g.fingers);
  bus_emit("gesture.swipe", e1, e2, NULL);
  free(e1), free(e2);
  const char *cmd = d == SWIPE_LEFT    ? g.cfg.swipe_left
                    : d == SWIPE_RIGHT ? g.cfg.swipe_right
                    : d == SWIPE_UP    ? g.cfg.swipe_up
                                       : g.cfg.swipe_down;
  run(cmd);
}

/* ---- libinput ---- */

static void on_event(struct libinput_event *ev) {
  enum libinput_event_type type = libinput_event_get_type(ev);
  if (type != LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN &&
      type != LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE &&
      type != LIBINPUT_EVENT_GESTURE_SWIPE_END)
    return;
  struct libinput_event_gesture *ge = libinput_event_get_gesture_event(ev);
  switch (type) {
  case LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN:
    g.swiping = true;
    g.fingers = libinput_event_gesture_get_finger_count(ge);
    g.dx = g.dy = 0;
    break;
  case LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE:
    g.dx += libinput_event_gesture_get_dx_unaccelerated(ge);
    g.dy += libinput_event_gesture_get_dy_unaccelerated(ge);
    break;
  default: {
    bool done = g.swiping && !libinput_event_gesture_get_cancelled(ge);
    g.swiping = false;
    enum swipe_dir d = done ? swipe_classify(g.dx, g.dy, g.fingers,
                                             g.cfg.fingers, g.cfg.threshold)
                            : SWIPE_NONE;
    if (d != SWIPE_NONE)
      swiped(d);
  }
  }
}

static void on_libinput(int fd, short revents, void *data) {
  if (libinput_dispatch(g.li) < 0)
    return;
  struct libinput_event *ev;
  while ((ev = libinput_get_event(g.li))) {
    on_event(ev);
    libinput_event_destroy(ev);
  }
}

static int open_restricted(const char *path, int flags, void *data) {
  int fd = open(path, flags | O_CLOEXEC);
  return fd < 0 ? -errno : fd;
}

static void close_restricted(int fd, void *data) { close(fd); }

static const struct libinput_interface li_iface = {
    .open_restricted = open_restricted,
    .close_restricted = close_restricted,
};

/* ---- touchpads ---- */

static bool is_touchpad(struct udev_device *d) {
  const char *name = udev_device_get_sysname(d);
  const char *tp = udev_device_get_property_value(d, "ID_INPUT_TOUCHPAD");
  return name && strncmp(name, "event", 5) == 0 && tp && strcmp(tp, "1") == 0;
}

static void add_touchpad(const char *node) {
  for (int i = 0; i < g.ndevs; ++i)
    if (strcmp(g.devs[i].node, node) == 0)
      return;
  if (access(node, R_OK) != 0) {
    ++g.denied;
    warn("gestures: cannot open %s (%s); install "
         "config/70-swell-touchpad.rules (see the README)",
         node, strerror(errno));
    return;
  }
  struct libinput_device *dev = libinput_path_add_device(g.li, node);
  if (!dev) {
    warn("gestures: libinput cannot use %s", node);
    return;
  }
  g.devs = xrealloc(g.devs, (g.ndevs + 1) * sizeof(*g.devs));
  g.devs[g.ndevs++] =
      (struct device){xstrdup(node), libinput_device_ref(dev)};
  on_libinput(-1, 0, NULL); /* its "device added" */
}

static void remove_touchpad(const char *node) {
  for (int i = 0; i < g.ndevs; ++i) {
    if (strcmp(g.devs[i].node, node) != 0)
      continue;
    libinput_path_remove_device(g.devs[i].dev);
    libinput_device_unref(g.devs[i].dev);
    free(g.devs[i].node);
    g.devs[i] = g.devs[--g.ndevs];
    return;
  }
}

static void on_udev(int fd, short revents, void *data) {
  struct udev_device *d;
  while ((d = udev_monitor_receive_device(g.monitor))) {
    const char *action = udev_device_get_action(d);
    const char *node = udev_device_get_devnode(d);
    if (action && node) {
      if (strcmp(action, "remove") == 0)
        remove_touchpad(node);
      else if (strcmp(action, "add") == 0 && is_touchpad(d))
        add_touchpad(node);
    }
    udev_device_unref(d);
  }
}

static void scan(void) {
  struct udev_enumerate *e = udev_enumerate_new(g.udev);
  udev_enumerate_add_match_subsystem(e, "input");
  udev_enumerate_add_match_property(e, "ID_INPUT_TOUCHPAD", "1");
  udev_enumerate_scan_devices(e);
  struct udev_list_entry *it;
  udev_list_entry_foreach(it, udev_enumerate_get_list_entry(e)) {
    struct udev_device *d =
        udev_device_new_from_syspath(g.udev, udev_list_entry_get_name(it));
    if (!d)
      continue;
    const char *node = udev_device_get_devnode(d);
    if (node && is_touchpad(d))
      add_touchpad(node);
    udev_device_unref(d);
  }
  udev_enumerate_unref(e);
}

static bool start(void) {
  g.udev = udev_new();
  g.li = g.udev ? libinput_path_create_context(&li_iface, NULL) : NULL;
  if (!g.li) {
    warn("gestures: cannot set up udev/libinput");
    return false;
  }
  g.monitor = udev_monitor_new_from_netlink(g.udev, "udev");
  if (g.monitor) {
    udev_monitor_filter_add_match_subsystem_devtype(g.monitor, "input", NULL);
    udev_monitor_enable_receiving(g.monitor);
    g.udev_watch = loop_add_fd(g.sw->loop, udev_monitor_get_fd(g.monitor),
                               POLLIN, on_udev, NULL);
  }
  g.li_watch =
      loop_add_fd(g.sw->loop, libinput_get_fd(g.li), POLLIN, on_libinput, NULL);
  scan();
  if (!g.ndevs && !g.denied)
    warn("gestures: no touchpad found (one plugged in later is picked up)");
  g.active = true;
  return true;
}

static void stop(void) {
  for (int i = 0; i < g.ndevs; ++i) {
    libinput_path_remove_device(g.devs[i].dev);
    libinput_device_unref(g.devs[i].dev);
    free(g.devs[i].node);
  }
  free(g.devs);
  g.devs = NULL;
  g.ndevs = g.denied = 0;
  loop_remove_fd(g.li_watch);
  loop_remove_fd(g.udev_watch);
  g.li_watch = g.udev_watch = NULL;
  if (g.li)
    libinput_unref(g.li);
  if (g.monitor)
    udev_monitor_unref(g.monitor);
  if (g.udev)
    udev_unref(g.udev);
  g.li = NULL;
  g.monitor = NULL;
  g.udev = NULL;
  g.active = g.swiping = false;
}

/* ---- module ---- */

static bool wants_devices(void) {
  return g.cfg.always || (g.sw->wm != WM_SWAY && g.sw->wm != WM_NIRI);
}

static bool init(struct swell *sw, const void *cfg) {
  g.sw = sw;
  g.cfg = *(const struct gestures_config *)cfg;
  g.last = SWIPE_NONE;
  return !wants_devices() || start();
}

static void reconfigure(const void *cfg) {
  g.cfg = *(const struct gestures_config *)cfg;
  if (wants_devices() && !g.active)
    start();
  else if (!wants_devices() && g.active)
    stop();
}

static void finish(void) { stop(); }

static int command(int argc, char **argv, struct reply *r) {
  if (argc != 1 || strcmp(argv[0], "status") != 0) {
    reply_printf(r, "usage: swell gestures status\n");
    return 1;
  }
  if (!g.active) {
    reply_printf(r, "idle: %s binds swipes itself (set always = true to "
                    "run anyway)\n",
                 wm_name(g.sw->wm));
    return 0;
  }
  reply_printf(r, "touchpads: %d\n", g.ndevs);
  for (int i = 0; i < g.ndevs; ++i)
    reply_printf(r, "  %s %s\n", g.devs[i].node,
                 libinput_device_get_name(g.devs[i].dev));
  if (g.denied)
    reply_printf(r, "not allowed to open: %d (install "
                    "config/70-swell-touchpad.rules)\n",
                 g.denied);
  if (g.last != SWIPE_NONE)
    reply_printf(r, "last swipe: %s, %d fingers\n", swipe_name(g.last),
                 g.last_fingers);
  return 0;
}

const struct swell_module gestures_module = {
    .name = "gestures",
    .usage = "  status    the touchpads read and the last swipe\n",
    .config_size = sizeof(struct gestures_config),
    .defaults = &defaults,
    .fields = fields,
    .nfields = ARRAY_LEN(fields),
    .init = init,
    .reconfigure = reconfigure,
    .finish = finish,
    .command = command,
};
