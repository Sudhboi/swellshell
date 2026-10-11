/* power: `swell power off` plays the wallpaper's power-off animation and
 * returns once the screen is black. It never powers anything off itself:
 * bind `swell power off && systemctl poweroff`. It always succeeds, so the
 * && never holds a shutdown back: with no daemon, the wallpaper off or a
 * window open there is no animation and it returns at once. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ipc.h"
#include "module.h"

/* The longest it waits, whatever the daemon says. */
#define POWER_MAX_WAIT_MS 10000
/* Past the animation's end, so its last (black) frame is on screen. */
#define POWER_MARGIN_MS 100

static void sleep_ms(long ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
    ;
}

static int run_local(struct swell *sw, const void *cfg, bool have_daemon,
                     int argc, char **argv) {
  if (argc != 1 || strcmp(argv[0], "off") != 0) {
    fprintf(stderr, "usage: swell power off\n");
    return 2;
  }
  if (!have_daemon)
    return 0;
  char *args[] = {"wallpaper", "outro"};
  struct reply r = {0};
  bool ok = false;
  long ms = 0;
  if (ipc_client_send(ipc_socket_path(), 2, args, &r, &ok) == 0 && ok &&
      r.buf)
    ms = strtol(r.buf, NULL, 10);
  reply_free(&r);
  if (ms > 0)
    sleep_ms((ms > POWER_MAX_WAIT_MS ? POWER_MAX_WAIT_MS : ms) +
             POWER_MARGIN_MS);
  return 0;
}

const struct swell_module power_module = {
    .name = "power",
    .usage = "  off       play the power-off animation and return once the "
             "screen is\n"
             "            black (it doesn't power off: bind `swell power off "
             "&& systemctl poweroff`)\n",
    .run_local = run_local,
};
