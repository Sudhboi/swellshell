/* The media module's pure parts: playerctl parsing, the position, time
 * formatting, covers in colour, and offscreen renders (PNGs at scale 2)
 * with checks on the panel's geometry and look.
 * Usage: test_media [outdir]   (default: current directory) */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../png.h"
#include "art.h"
#include "media/player.h"
#include "media/render.h"
#include "module.h"

extern const struct swell_module media_module;

static struct media_config cfg;

static int check(bool ok, const char *what) {
  if (!ok)
    fprintf(stderr, "check failed: %s\n", what);
  return !ok;
}

static int test_parse(void) {
  int rc = 0;
  struct media_track t;
  rc |= check(media_parse("Playing\t245000000\t61500000\tfile:///c.png\t"
                          "spotify\tMezzanine\tMassive Attack\tTeardrop\n",
                          1000, &t),
              "a full line parses");
  rc |= check(t.has && t.playing && t.len_us == 245000000 &&
                  t.pos_us == 61500000 && t.at_ms == 1000,
              "status, length, position");
  rc |= check(!strcmp(t.art_url, "file:///c.png") &&
                  !strcmp(t.player, "spotify") &&
                  !strcmp(t.album, "Mezzanine") &&
                  !strcmp(t.artist, "Massive Attack") &&
                  !strcmp(t.title, "Teardrop"),
              "the text fields");
  /* a browser: no length, an instance suffix, a tab in the title */
  rc |= check(media_parse("Paused\t\t0\t\tfirefox.instance_1_23\t\t\tA\tB",
                          0, &t),
              "a line without a length parses");
  rc |= check(!t.playing && t.len_us == 0 && !strcmp(t.player, "firefox") &&
                  !strcmp(t.title, "A\tB") && !t.album[0] && !t.artist[0],
              "no length, the player's suffix cut, the title keeps its tab");
  rc |= check(!media_parse("Stopped\t1\t0\t\tmpv\t\t\tTitle", 0, &t) && !t.has,
              "stopped is no track");
  rc |= check(!media_parse("Playing\t1\t0\t\tmpv\t\t\t", 0, &t),
              "no title is no track");
  rc |= check(!media_parse("", 0, &t) && !media_parse("No players found", 0, &t),
              "no player");
  rc |= check(!media_parse("Playing\t1\t0\tart\tArtist\tTitle", 0, &t),
              "the wallpaper's shorter format doesn't parse");
  return rc;
}

static int test_position(void) {
  int rc = 0;
  struct media_track t = {.has = true, .playing = true, .len_us = 10000000,
                          .pos_us = 4000000, .at_ms = 100};
  rc |= check(media_position(&t, 100) == 4000000, "position when read");
  rc |= check(media_position(&t, 1600) == 5500000, "extrapolated");
  rc |= check(media_position(&t, 100000) == 10000000, "held at the length");
  t.playing = false;
  rc |= check(media_position(&t, 1600) == 4000000, "paused, it stays");
  t.playing = true;
  t.len_us = 0;
  rc |= check(media_position(&t, 100000) > 10000000, "no length, no limit");
  struct media_track u = t;
  u.pos_us = 1;
  rc |= check(media_same_track(&t, &u), "a new position is the same track");
  snprintf(u.title, sizeof u.title, "other");
  rc |= check(!media_same_track(&t, &u), "a new title is another track");

  char s[32];
  media_format_time(s, sizeof s, 0);
  rc |= check(!strcmp(s, "0:00"), "0:00");
  media_format_time(s, sizeof s, 61999999);
  rc |= check(!strcmp(s, "1:01"), "1:01 (rounded down)");
  media_format_time(s, sizeof s, (3600 + 2 * 60 + 3) * 1000000LL);
  rc |= check(!strcmp(s, "1:02:03"), "h:mm:ss");
  return rc;
}

/* A 300x200 PNG, red on the left half and blue on the right. */
static void write_cover(const char *path) {
  cairo_surface_t *in =
      cairo_image_surface_create(CAIRO_FORMAT_RGB24, 300, 200);
  cairo_t *ic = cairo_create(in);
  cairo_set_source_rgb(ic, 1, 0, 0);
  cairo_paint(ic);
  cairo_rectangle(ic, 150, 0, 150, 200);
  cairo_set_source_rgb(ic, 0, 0, 1);
  cairo_fill(ic);
  cairo_destroy(ic);
  assert(cairo_surface_write_to_png(in, path) == CAIRO_STATUS_SUCCESS);
  cairo_surface_destroy(in);
}

static uint32_t px(cairo_surface_t *s, int x, int y) {
  cairo_surface_flush(s);
  return *(uint32_t *)(cairo_image_surface_get_data(s) +
                       y * cairo_image_surface_get_stride(s) + x * 4) &
         0xffffff;
}

static int test_art(const char *path) {
  int rc = 0;
  cairo_surface_t *c = art_load(path, false), *g = art_load(path, true);
  rc |= check(c && g && cairo_image_surface_get_width(c) == 200,
              "the cover loads, cut to its square");
  rc |= check(px(c, 10, 100) == 0xff0000 && px(c, 190, 100) == 0x0000ff,
              "in colour");
  rc |= check(px(g, 10, 100) == 0x363636, "in grey");
  cairo_surface_destroy(c);
  cairo_surface_destroy(g);
  char p[1024];
  rc |= check(art_file_path("file:///tmp/a%20b.png", p, sizeof p) &&
                  !strcmp(p, "/tmp/a b.png"),
              "file:// URL decoded");
  rc |= check(!art_file_path("https://x/y.png", p, sizeof p),
              "an http URL isn't a file");
  return rc;
}

/* The logical y of the panel's bottom border (scanning up the left edge),
 * or -1 if none is drawn. */
static double panel_bottom(cairo_t *cr) {
  for (int py = media_surface_height(&cfg) * SCALE - 1; py >= 0; --py)
    if (pixel(cr, 0.5, (py + 0.5) / SCALE) == cfg.border)
      return (double)(py + 1) / SCALE;
  return -1;
}

/* A slot at rest: paused from the track's state. */
static struct media_slot slot(const struct media_track *t,
                              cairo_surface_t *art, unsigned id) {
  return (struct media_slot){t, art, id, t->playing ? 0 : 1};
}

static cairo_t *render(const struct media_frame *f) {
  cairo_t *cr = canvas(cfg.width, media_surface_height(&cfg));
  media_render(cr, &cfg, f);
  return cr;
}

static int dump(const char *dir, const char *name, const struct media_frame *f,
                bool rests) {
  cairo_t *cr = render(f);
  save_png(cr, dir, name);
  int bad = 0;
  if (rests) {
    double b = panel_bottom(cr);
    bad = check(b == cfg.top_gap + cfg.height,
                "shown, the panel hangs from y = top_gap");
    if (bad)
      fprintf(stderr, "%s: bottom at %.1f\n", name, b);
  }
  cairo_destroy(cr);
  return bad;
}

/* The status icon's middle column: filled by the triangle while playing,
 * the gap between the bars when paused. */
static bool icon_middle_filled(cairo_t *cr) {
  double s = cfg.font_row * 0.75;
  cairo_select_font_face(cr, cfg.font, CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, cfg.font_player);
  cairo_text_extents_t ext;
  cairo_text_extents(cr, "H0", &ext);
  double base = cfg.top_gap + cfg.pad - ext.y_bearing;
  uint32_t v = pixel(cr, cfg.width - cfg.pad - s / 2, base - s / 2);
  return v != cfg.background;
}

/* Playing to paused: the icon morphs and the bar fades to muted. */
static int check_morph(const char *dir, struct media_frame *f) {
  int rc = 0;
  const char *names[] = {"media_status_0", "media_status_1",
                         "media_status_2", "media_status_3"};
  uint32_t bar[4];
  bool middle[4];
  for (int i = 0; i < 4; ++i) {
    f->cur.paused = i / 3.0;
    cairo_t *cr = render(f);
    save_png(cr, dir, names[i]);
    middle[i] = icon_middle_filled(cr);
    double bx = cfg.pad + cfg.art_size + cfg.pad + 2;
    cairo_select_font_face(cr, cfg.font, CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, cfg.font_time);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, "H0", &ext);
    double bar_y = cfg.top_gap + cfg.height - cfg.pad + ext.y_bearing -
                   cfg.gap_time - cfg.bar_h;
    bar[i] = pixel(cr, bx, bar_y + cfg.bar_h / 2);
    cairo_destroy(cr);
  }
  rc |= check(middle[0] && !middle[3], "a triangle playing, bars paused");
  rc |= check(bar[0] != bar[3] && bar[3] == cfg.muted,
              "the bar is muted once paused");
  rc |= check(bar[1] != bar[0] && bar[1] != bar[3],
              "and in between on the way");
  f->cur.paused = 1;
  return rc;
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : ".";
  int rc = 0;
  cfg = *(const struct media_config *)media_module.defaults;

  rc |= test_parse();
  rc |= test_position();

  char cover[1024];
  snprintf(cover, sizeof cover, "%s/media_cover_in.png", dir);
  write_cover(cover);
  rc |= test_art(cover);
  cairo_surface_t *art = art_load(cover, false);

  struct media_track t;
  media_parse("Playing\t245000000\t61500000\t\tspotify\tMezzanine\t"
              "Massive Attack\tTeardrop",
              0, &t);
  struct media_frame f = {.shown = 1, .cur = slot(&t, art, 1), .fade = 1};

  rc |= dump(dir, "media_cover", &f, true);
  {
    /* The cover in colour, in its square; the bar half the time in. */
    cairo_t *cr = render(&f);
    double ay = cfg.top_gap + (cfg.height - cfg.art_size) / 2.0;
    rc |= check(pixel(cr, cfg.pad + 10, ay + cfg.art_size / 2.0) ==
                    0xff0000ffu,
                "the cover's left half is red");
    rc |= check(pixel(cr, cfg.pad + cfg.art_size - 10,
                      ay + cfg.art_size / 2.0) == 0x0000ffffu,
                "and its right half blue");
    cairo_destroy(cr);
  }

  f.cur.art = NULL;
  rc |= dump(dir, "media_no_cover", &f, true);
  {
    cairo_t *cr = render(&f);
    double ay = cfg.top_gap + (cfg.height - cfg.art_size) / 2.0;
    rc |= check(pixel(cr, cfg.pad + 4, ay + 4) == cfg.track,
                "no cover: the track-coloured square");
    cairo_destroy(cr);
  }

  struct media_track p = t;
  p.playing = false;
  f.cur = slot(&p, art, 1);
  rc |= dump(dir, "media_paused", &f, true);
  rc |= check_morph(dir, &f);

  struct media_track n;
  media_parse("Playing\t\t0\t\tfirefox.instance_1_2\t\tSome channel\t"
              "A long video title that goes on past the panel's edge, "
              "and further still",
              0, &n);
  f.cur = slot(&n, NULL, 2);
  f.now = 400;
  rc |= dump(dir, "media_no_length", &f, true);
  rc |= check(media_render_moving(&n) && !media_render_moving(&t),
              "only a playing track without a length keeps moving");

  struct media_track k;
  media_parse("Playing\t180000000\t5000000\t\tchromium\t回声\t杨胤岳 - Topic\t"
              "回声 Midu Echoing」",
              0, &k);
  f.cur = slot(&k, art, 1);
  rc |= dump(dir, "media_cjk", &f, true);

  struct media_track h = t;
  h.len_us = (2 * 3600 + 5) * 1000000LL;
  h.pos_us = (3600 + 23 * 60) * 1000000LL;
  f.cur = slot(&h, art, 1);
  rc |= dump(dir, "media_hours", &f, true);

  /* a crossfade half-way, from k to t */
  f.cur = slot(&t, NULL, 3);
  f.prev = slot(&k, art, 1);
  f.fade = 0.5;
  rc |= dump(dir, "media_crossfade", &f, true);
  f.prev = (struct media_slot){0};
  f.fade = 1;

  /* the slide */
  f.cur = slot(&t, art, 1);
  f.shown = 0.5;
  {
    cairo_t *cr = render(&f);
    save_png(cr, dir, "media_slide_half");
    double b = panel_bottom(cr);
    rc |= check(b > 0 && b < cfg.top_gap + cfg.height,
                "half shown, the panel is higher");
    cairo_destroy(cr);
  }
  f.shown = 0;
  {
    cairo_t *cr = render(&f);
    rc |= check(panel_bottom(cr) < 0 &&
                    pixel(cr, cfg.width / 2.0, cfg.top_gap + 10) == 0,
                "not shown, nothing is drawn");
    cairo_destroy(cr);
  }

  /* art off: the text starts at the pad */
  f.shown = 1;
  cfg.art = false;
  rc |= dump(dir, "media_art_off", &f, true);
  cfg = *(const struct media_config *)media_module.defaults;

  cairo_surface_destroy(art);
  if (!rc)
    printf("media: ok\n");
  return rc;
}
