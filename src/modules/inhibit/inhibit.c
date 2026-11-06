/* inhibit: keeps the idle screensaver away while something asks it to.
 * Apps ask through org.freedesktop.ScreenSaver on the session bus
 * (Inhibit/UnInhibit: Firefox and its forks, Chromium, mpv, VLC, ... while
 * a video plays); `swell inhibit on` asks by hand. Wayland idle inhibitors
 * (zwp_idle_inhibit) need nothing here: the compositor already holds back
 * the ext-idle-notify idle the wallpaper listens to. Emits idle.inhibit
 * (SWELL_INHIBITED=1|0) when the sum changes; the wallpaper keeps its saver
 * off while it is 1 and restarts its idle timeout when it drops. */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <time.h>

#include "bus.h"
#include "handover.h"
#include "list.h"
#include "loop.h"
#include "module.h"
#include "util.h"

#define SERVICE "org.freedesktop.ScreenSaver"
#define IFACE "org.freedesktop.ScreenSaver"

struct inhibit_config {
  bool dbus;
};

static const struct inhibit_config defaults = {.dbus = true};

#define S struct inhibit_config
static const struct cfg_field fields[] = {
    CFG_BOOL(S, dbus,
             "serve org.freedesktop.ScreenSaver on the session bus, so "
             "browsers and video players playing a video keep the "
             "screensaver away"),
};
#undef S

static struct {
  struct swell *sw;
  struct inhibit_config cfg;
  sd_bus *bus;
  struct loop_watch *watch;
  sd_bus_slot *objs[2];
  bool owned;   /* the name is ours (not queued behind another owner) */
  bool manual;  /* swell inhibit on */
  bool emitted; /* the state last emitted */
  struct inhibit_list list;
} m;

static bool inhibited(void) { return m.manual || m.list.n > 0; }

static void emit(void) {
  bool on = inhibited();
  if (on == m.emitted)
    return;
  m.emitted = on;
  bus_emit("idle.inhibit", on ? "SWELL_INHIBITED=1" : "SWELL_INHIBITED=0",
           NULL);
}

static void drop(uint32_t cookie) {
  struct inhibitor in;
  if (!inhibit_list_remove(&m.list, cookie, &in))
    return;
  sd_bus_track_unref(in.track);
  emit();
}

/* The sender left the bus without UnInhibit (it quit or crashed). */
static int on_gone(sd_bus_track *t, void *data) {
  drop((uint32_t)(uintptr_t)data);
  return 0;
}

static void track(struct inhibitor *in) {
  sd_bus_track *t = NULL;
  if (sd_bus_track_new(m.bus, &t, on_gone, (void *)(uintptr_t)in->cookie) <
          0 ||
      sd_bus_track_add_name(t, in->sender) < 0) {
    sd_bus_track_unref(t);
    t = NULL;
  }
  in->track = t;
}

static int m_inhibit(sd_bus_message *msg, void *data, sd_bus_error *err) {
  const char *app, *reason;
  int r = sd_bus_message_read(msg, "ss", &app, &reason);
  if (r < 0)
    return r;
  struct inhibitor *in =
      inhibit_list_add(&m.list, sd_bus_message_get_sender(msg), app, reason);
  uint32_t cookie = in->cookie;
  track(in);
  emit();
  return sd_bus_reply_method_return(msg, "u", cookie);
}

static int m_uninhibit(sd_bus_message *msg, void *data, sd_bus_error *err) {
  uint32_t cookie;
  int r = sd_bus_message_read(msg, "u", &cookie);
  if (r < 0)
    return r;
  drop(cookie);
  return sd_bus_reply_method_return(msg, "");
}

static const sd_bus_vtable vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Inhibit", "ss", "u", m_inhibit, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("UnInhibit", "u", "", m_uninhibit,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

static int on_name(sd_bus_message *msg, void *data, sd_bus_error *err) {
  const char *name;
  if (sd_bus_message_read(msg, "s", &name) >= 0 && strcmp(name, SERVICE) == 0)
    m.owned = data != NULL;
  return 0;
}

static int on_request(sd_bus_message *msg, void *data, sd_bus_error *err) {
  const sd_bus_error *e = sd_bus_message_get_error(msg);
  if (e)
    warn("inhibit: can't own %s: %s", SERVICE, e->message);
  return 0;
}

/* ---- the bus in the loop ---- */

static void close_bus(void);

static void process(void) {
  int r;
  while (m.bus && (r = sd_bus_process(m.bus, NULL)) > 0)
    ;
  if (m.bus && r < 0) {
    warn("inhibit: session bus: %s", strerror(-r));
    close_bus();
  }
}

static void on_fd(int fd, short revents, void *data) { process(); }

static int prepare(void *data) {
  if (!m.bus)
    return -1;
  process(); /* replies queued, messages read by other calls */
  if (!m.bus)
    return -1;
  int ev = sd_bus_get_events(m.bus);
  loop_watch_set_events(m.watch, ev > 0 ? (short)ev : POLLIN);
  uint64_t until;
  if (sd_bus_get_timeout(m.bus, &until) < 0 || until == UINT64_MAX)
    return -1;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  uint64_t now = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000;
  return until <= now ? 0 : (int)((until - now + 999) / 1000);
}

static void close_bus(void) {
  for (size_t i = 0; i < m.list.n; ++i) {
    sd_bus_track_unref(m.list.v[i].track);
    m.list.v[i].track = NULL;
  }
  /* Its inhibitors went with the connection. */
  m.list.n = 0;
  for (size_t i = 0; i < ARRAY_LEN(m.objs); ++i)
    m.objs[i] = sd_bus_slot_unref(m.objs[i]);
  if (m.watch)
    loop_remove_fd(m.watch);
  m.watch = NULL;
  if (m.bus)
    loop_remove_prepare(m.sw->loop, prepare, NULL);
  m.bus = sd_bus_flush_close_unref(m.bus);
  m.owned = false;
  emit();
}

static void open_bus(void) {
  if (m.bus || !m.cfg.dbus)
    return;
  int r = sd_bus_open_user(&m.bus);
  if (r < 0) {
    warn("inhibit: no session bus: %s", strerror(-r));
    m.bus = NULL;
    return;
  }
  /* Firefox asks /ScreenSaver, Chromium /org/freedesktop/ScreenSaver. */
  sd_bus_add_object_vtable(m.bus, &m.objs[0], "/org/freedesktop/ScreenSaver",
                           IFACE, vtable, NULL);
  sd_bus_add_object_vtable(m.bus, &m.objs[1], "/ScreenSaver", IFACE, vtable,
                           NULL);
  sd_bus_match_signal(m.bus, NULL, "org.freedesktop.DBus",
                      "/org/freedesktop/DBus", "org.freedesktop.DBus",
                      "NameAcquired", on_name, (void *)1);
  sd_bus_match_signal(m.bus, NULL, "org.freedesktop.DBus",
                      "/org/freedesktop/DBus", "org.freedesktop.DBus",
                      "NameLost", on_name, NULL);
  /* Replacing the daemon before us; queued behind anyone else. */
  sd_bus_request_name_async(m.bus, NULL, SERVICE,
                            SD_BUS_NAME_ALLOW_REPLACEMENT |
                                SD_BUS_NAME_REPLACE_EXISTING |
                                SD_BUS_NAME_QUEUE,
                            on_request, NULL);
  m.watch =
      loop_add_fd(m.sw->loop, sd_bus_get_fd(m.bus), POLLIN, on_fd, NULL);
  loop_add_prepare(m.sw->loop, 50, prepare, NULL);
}

/* ---- lifecycle ---- */

/* Taking over from the daemon before us: its inhibitors and `on`. */
static void join(void) {
  const char *s = handover_get("inhibit");
  int manual = 0, off = 0;
  if (!s || sscanf(s, "manual=%d %n", &manual, &off) < 1)
    return;
  m.manual = manual != 0;
  size_t had = m.list.n;
  inhibit_list_decode(&m.list, s + off);
  if (!m.bus) {
    m.list.n = had; /* nobody to watch them for */
    return;
  }
  for (size_t i = had; i < m.list.n; ++i)
    track(&m.list.v[i]);
}

static bool init(struct swell *sw, const void *cfg) {
  m.sw = sw;
  m.cfg = *(const struct inhibit_config *)cfg;
  m.emitted = false;
  open_bus();
  join();
  emit();
  return true;
}

static void reconfigure(const void *cfg) {
  m.cfg = *(const struct inhibit_config *)cfg;
  if (m.cfg.dbus)
    open_bus();
  else if (m.bus)
    close_bus();
}

static void finish(void) {
  m.manual = false;
  if (m.bus)
    close_bus();
  emit();
  inhibit_list_free(&m.list);
}

static int handover(struct reply *r) {
  char *s = inhibit_list_encode(&m.list);
  reply_printf(r, "inhibit manual=%d %s\n", m.manual, s);
  free(s);
  return 0;
}

static int usage(struct reply *r) {
  reply_printf(r, "usage: swell inhibit on|off|toggle|status\n");
  return 1;
}

static int command(int argc, char **argv, struct reply *r) {
  const char *cmd = argc ? argv[0] : "status";
  if (argc > 1)
    return usage(r);
  if (strcmp(cmd, "on") == 0)
    m.manual = true;
  else if (strcmp(cmd, "off") == 0)
    m.manual = false;
  else if (strcmp(cmd, "toggle") == 0)
    m.manual = !m.manual;
  else if (strcmp(cmd, "status") != 0)
    return usage(r);
  emit();
  reply_printf(r, "%s\n", inhibited() ? "inhibited" : "not inhibited");
  if (strcmp(cmd, "status") != 0)
    return 0;
  reply_printf(r, "manual: %s\n", m.manual ? "on" : "off");
  reply_printf(r, "dbus: %s\n",
               !m.cfg.dbus ? "off"
               : !m.bus    ? "no session bus"
               : m.owned   ? "serving " SERVICE
                           : "waiting for " SERVICE " (another owner)");
  for (size_t i = 0; i < m.list.n; ++i) {
    const struct inhibitor *in = &m.list.v[i];
    reply_printf(r, "  %u %s (%s): %s\n", in->cookie, in->app, in->sender,
                 in->reason);
  }
  return 0;
}

const struct swell_module inhibit_module = {
    .name = "inhibit",
    .usage = "  on|off|toggle  keep the screensaver away by hand, or stop\n"
             "  status    inhibited or not, and who asks\n",
    .config_size = sizeof(struct inhibit_config),
    .defaults = &defaults,
    .fields = fields,
    .nfields = ARRAY_LEN(fields),
    .init = init,
    .reconfigure = reconfigure,
    .finish = finish,
    .command = command,
    .handover = handover,
};
