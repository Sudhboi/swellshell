/* What's playing, for the wallpaper: polled with playerctl (core mpris.c)
 * every media_interval_ms while the module animates, the track's cover
 * turned grey. Frozen or dozing, nothing runs. */
#include <stdlib.h>
#include <string.h>

#include "mpris.h"
#include "util.h"
#include "wallpaper.h"

/* A playerctl still running after this long is killed. */
#define KILL_MS 3000

static struct {
  struct mpris poll;
  struct wp_media m;
  bool want_art;
} md;

bool wp_media_parse(const char *line, int64_t now, struct wp_media *m) {
  char buf[4096];
  copy_str(buf, sizeof buf, line);
  buf[strcspn(buf, "\n")] = '\0';
  /* status, length, position, art url, artist, title: the title takes the
   * rest, so a tab in it survives */
  char *f[6];
  f[0] = buf;
  for (int i = 1; i < 6; ++i) {
    f[i] = strchr(f[i - 1], '\t');
    if (!f[i]) {
      memset(m, 0, sizeof *m);
      return false;
    }
    *f[i]++ = '\0';
  }
  if (!f[5][0] || (strcmp(f[0], "Playing") && strcmp(f[0], "Paused"))) {
    memset(m, 0, sizeof *m);
    return false;
  }
  m->has = true;
  m->playing = !strcmp(f[0], "Playing");
  m->len_us = strtoll(f[1], NULL, 10);
  if (m->len_us < 0)
    m->len_us = 0;
  m->pos_us = strtoll(f[2], NULL, 10);
  if (m->pos_us < 0)
    m->pos_us = 0;
  m->at_ms = now;
  copy_str(m->art_url, sizeof m->art_url, f[3]);
  copy_str(m->artist, sizeof m->artist, f[4]);
  copy_str(m->title, sizeof m->title, f[5]);
  return true;
}

double wp_media_progress(const struct wp_media *m, int64_t now) {
  if (!m->has || m->len_us <= 0)
    return -1;
  double pos = m->pos_us + (m->playing ? (now - m->at_ms) * 1000.0 : 0);
  double p = pos / m->len_us;
  return p < 0 ? 0 : p > 1 ? 1 : p;
}

const struct wp_media *wp_media_get(void) { return &md.m; }

cairo_surface_t *wp_media_art(unsigned *id) { return mpris_art(&md.poll, id); }

static const char *parsed(const char *out, void *data) {
  wp_media_parse(out, now_ms(), &md.m);
  return md.want_art && md.m.has ? md.m.art_url : "";
}

static const struct mpris_ops ops = {
    .format = "{{status}}\t{{mpris:length}}\t{{position}}\t{{mpris:artUrl}}"
              "\t{{artist}}\t{{title}}",
    .grey = true,
    .art_tag = "swell-art",
    .parsed = parsed,
};

void wp_media_start(struct loop *loop, const struct wallpaper_config *c) {
  if (!md.poll.ops)
    mpris_init(&md.poll, loop, &ops, NULL);
  if (mpris_running(&md.poll))
    return;
  md.want_art = c->media_art;
  mpris_configure(&md.poll, c->media_player, KILL_MS);
  mpris_start(&md.poll, c->media_interval_ms);
}

void wp_media_stop(void) {
  if (md.poll.ops)
    mpris_stop(&md.poll);
}

void wp_media_clear(void) {
  memset(&md.m, 0, sizeof md.m);
  mpris_clear_art(&md.poll); /* safe before mpris_init */
}
