/* Scratch input driver for a headless sway: virtual pointer + keyboard.
 * usage: vin CMD...   (VIN_W/VIN_H = layout extent, default 1536x864)
 *   m X Y          move to logical X Y
 *   g X Y N        glide from the last position to X Y in N steps (16 ms)
 *   bd | bu        left button down / up
 *   click | mclick | rclick
 *   k [ctrl+][shift+]KEYSYM   press and release
 *   kd KEYSYM | ku KEYSYM     press / release (for repeat)
 *   s MS           sleep */
#define _GNU_SOURCE
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct wl_display *dpy;
static struct wl_seat *seat;
static struct zwlr_virtual_pointer_manager_v1 *pmgr;
static struct zwp_virtual_keyboard_manager_v1 *kmgr;
static struct zwlr_virtual_pointer_v1 *ptr;
static struct zwp_virtual_keyboard_v1 *kbd;
static struct xkb_keymap *keymap;
static int W = 1536, H = 864;
static double cx, cy;

static uint32_t now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void global(void *d, struct wl_registry *r, uint32_t name,
                   const char *iface, uint32_t v) {
  if (!strcmp(iface, "wl_seat") && !seat)
    seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
  else if (!strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name))
    pmgr = wl_registry_bind(r, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
  else if (!strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name))
    kmgr = wl_registry_bind(r, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
}
static void global_remove(void *d, struct wl_registry *r, uint32_t n) {}
static const struct wl_registry_listener rl = {global, global_remove};

static void rt(void) { wl_display_roundtrip(dpy); }

static void move(double x, double y) {
  cx = x, cy = y;
  zwlr_virtual_pointer_v1_motion_absolute(ptr, now(), (uint32_t)(x * 100),
                                          (uint32_t)(y * 100), W * 100, H * 100);
  zwlr_virtual_pointer_v1_frame(ptr);
  rt();
}

static void button(uint32_t b, int down) {
  zwlr_virtual_pointer_v1_button(ptr, now(), b, down);
  zwlr_virtual_pointer_v1_frame(ptr);
  rt();
}

static xkb_keycode_t find_key(xkb_keysym_t sym) {
  for (xkb_keycode_t k = xkb_keymap_min_keycode(keymap);
       k <= xkb_keymap_max_keycode(keymap); ++k) {
    const xkb_keysym_t *syms;
    int n = xkb_keymap_key_get_syms_by_level(keymap, k, 0, 0, &syms);
    for (int i = 0; i < n; ++i)
      if (syms[i] == sym)
        return k;
  }
  return 0;
}

static uint32_t mods;
static void set_mods(uint32_t m) {
  mods = m;
  zwp_virtual_keyboard_v1_modifiers(kbd, m, 0, 0, 0);
  rt();
}

static void key(const char *spec, int press, int release) {
  uint32_t m = 0;
  const char *p = spec;
  for (;;) {
    if (!strncmp(p, "ctrl+", 5)) {
      m |= 1u << xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_CTRL);
      p += 5;
    } else if (!strncmp(p, "shift+", 6)) {
      m |= 1u << xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_SHIFT);
      p += 6;
    } else
      break;
  }
  xkb_keysym_t sym = xkb_keysym_from_name(p, XKB_KEYSYM_NO_FLAGS);
  xkb_keycode_t kc = sym ? find_key(sym) : 0;
  if (!kc) {
    fprintf(stderr, "vin: unknown key %s\n", p);
    exit(1);
  }
  if (m)
    set_mods(m);
  if (press) {
    zwp_virtual_keyboard_v1_key(kbd, now(), kc - 8, 1);
    rt();
  }
  if (release) {
    zwp_virtual_keyboard_v1_key(kbd, now(), kc - 8, 0);
    rt();
  }
  if (m)
    set_mods(0);
}

int main(int argc, char **argv) {
  if (getenv("VIN_W"))
    W = atoi(getenv("VIN_W"));
  if (getenv("VIN_H"))
    H = atoi(getenv("VIN_H"));
  dpy = wl_display_connect(NULL);
  if (!dpy)
    return 1;
  wl_registry_add_listener(wl_display_get_registry(dpy), &rl, NULL);
  wl_display_roundtrip(dpy);
  if (!seat || !pmgr || !kmgr) {
    fprintf(stderr, "vin: missing globals\n");
    return 1;
  }
  ptr = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(pmgr, seat);
  kbd = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(kmgr, seat);
  struct xkb_context *ctx = xkb_context_new(0);
  struct xkb_rule_names names = {.layout = "us"};
  keymap = xkb_keymap_new_from_names(ctx, &names, 0);
  char *s = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
  size_t len = strlen(s) + 1;
  int fd = memfd_create("keymap", 0);
  write(fd, s, len);
  zwp_virtual_keyboard_v1_keymap(kbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, len);
  close(fd);
  rt();
  /* clients re-create wl_pointer/wl_keyboard when the seat gains them */
  usleep(300000);
  rt();
  if (getenv("VIN_X")) {
    cx = atof(getenv("VIN_X"));
    cy = atof(getenv("VIN_Y"));
  }

  for (int i = 1; i < argc; ++i) {
    const char *c = argv[i];
    if (!strcmp(c, "m")) {
      double x = atof(argv[++i]), y = atof(argv[++i]);
      move(x, y);
    } else if (!strcmp(c, "g")) {
      double x = atof(argv[++i]), y = atof(argv[++i]);
      int n = atoi(argv[++i]);
      double x0 = cx, y0 = cy;
      for (int j = 1; j <= n; ++j) {
        move(x0 + (x - x0) * j / n, y0 + (y - y0) * j / n);
        usleep(16000);
      }
    } else if (!strcmp(c, "bd")) {
      button(BTN_LEFT, 1);
    } else if (!strcmp(c, "bu")) {
      button(BTN_LEFT, 0);
    } else if (!strcmp(c, "click")) {
      button(BTN_LEFT, 1);
      usleep(30000);
      button(BTN_LEFT, 0);
    } else if (!strcmp(c, "mclick")) {
      button(BTN_MIDDLE, 1);
      button(BTN_MIDDLE, 0);
    } else if (!strcmp(c, "rclick")) {
      button(BTN_RIGHT, 1);
      button(BTN_RIGHT, 0);
    } else if (!strcmp(c, "k")) {
      key(argv[++i], 1, 1);
    } else if (!strcmp(c, "kd")) {
      key(argv[++i], 1, 0);
    } else if (!strcmp(c, "ku")) {
      key(argv[++i], 0, 1);
    } else if (!strcmp(c, "s")) {
      usleep(atoi(argv[++i]) * 1000);
    } else {
      fprintf(stderr, "vin: unknown command %s\n", c);
      return 2;
    }
  }
  zwlr_virtual_pointer_v1_destroy(ptr);
  zwp_virtual_keyboard_v1_destroy(kbd);
  rt();
  wl_display_disconnect(dpy);
  return 0;
}
