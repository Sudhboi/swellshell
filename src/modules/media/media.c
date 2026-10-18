/* media: what's playing (cover, title, artist, album, progress) in a panel
 * that drops down from the top while the popup shows its details. It
 * follows the popup's bus events: playerctl is polled from popup.show, the
 * panel drops down on popup.expand if a player has a track, and goes back
 * up on popup.hide (not while the popup is in the wallpaper, which shows
 * the track itself). `swell media show` shows it by hand too, and
 * `swell media play-pause` pauses or resumes the track, showing the panel
 * for flash_ms with the new state. */
#include "media.h"

#include <stdio.h>
#include <string.h>

#include "anim.h"
#include "bus.h"
#include "loop.h"
#include "module.h"
#include "player.h"
#include "render.h"
#include "surface.h"
#include "util.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

static const struct media_config defaults = {
    .namespace = "media",
    .width = 560,
    .height = 160,
    .top_gap = 12,
    .corner = 0,
    .pad = 24,
    .art = true,
    .art_size = 112,
    .font = "IosevkaTerm Nerd Font",
    .font_player = 14,
    .font_title = 22,
    .font_row = 16,
    .font_time = 13,
    .background = 0x000000f0u,
    .border = 0x6a6a6affu,
    .track = 0x333333ffu,
    .bar_dark = 0xa0a0a0ffu,
    .bar_light = 0xd6d6d6ffu,
    .text = 0xf2f2f2ffu,
    .dim = 0x949494ffu,
    .muted = 0x5a5a5affu,
    .gap_title = 12,
    .gap_artist = 10,
    .gap_time = 8,
    .bar_h = 4,
    .player = "",
    .interval_ms = 1000,
    .command_timeout_ms = 3000,
    .slide_ms = 220,
    .close_ms = 220,
    .fade_ms = 200,
    .chase_ms = 1600,
    .status_ms = 250,
    .flash_ms = 1500,
};

#define S struct media_config
static const struct cfg_field fields[] = {
    CFG_STRING(S, namespace, "layer-shell namespace of the surface"),
    CFG_INT(S, width, 1, 4096, "panel width, logical px"),
    CFG_INT(S, height, 1, 4096, "panel height"),
    CFG_INT(S, top_gap, 0, 1024,
            "gap between the top edge (or bar) and the panel"),
    CFG_INT(S, corner, 0, 512, "panel corner radius (0 = square)"),
    CFG_INT(S, pad, 0, 1024, "inner padding of the panel"),
    CFG_BOOL(S, art, "show the track's cover"),
    CFG_INT(S, art_size, 1, 4096, "side of the cover's square"),
    CFG_STRING(S, font, "font family"),
    CFG_INT(S, font_player, 1, 1024, "player name size"),
    CFG_INT(S, font_title, 1, 1024, "title size"),
    CFG_INT(S, font_row, 1, 1024, "artist/album and status icon size"),
    CFG_INT(S, font_time, 1, 1024, "elapsed and total time size"),
    CFG_COLOR(S, background, "panel background"),
    CFG_COLOR(S, border, "1px panel border"),
    CFG_COLOR(S, track, "progress bar track, and the square without a cover"),
    CFG_COLOR(S, bar_dark, "progress bar gradient, left end"),
    CFG_COLOR(S, bar_light, "progress bar gradient, right end"),
    CFG_COLOR(S, text, "title and the playing icon"),
    CFG_COLOR(S, dim, "player, artist/album and times"),
    CFG_COLOR(S, muted, "paused: the icon, bar and times"),
    CFG_DOUBLE(S, gap_title, 0, 1024, "player baseline to title cap top"),
    CFG_DOUBLE(S, gap_artist, 0, 1024, "title baseline to artist cap top"),
    CFG_DOUBLE(S, gap_time, 0, 1024, "bar bottom to time cap top"),
    CFG_DOUBLE(S, bar_h, 0, 1024, "progress bar height"),
    CFG_STRING(S, player, "playerctl -p (\"\": playerctl's pick)"),
    CFG_INT(S, interval_ms, 100, 600000,
            "while the popup shows, a playerctl follower that died is "
            "restarted within this"),
    CFG_INT(S, command_timeout_ms, 100, 600000,
            "kill playerctl if it is still running after this"),
    CFG_INT(S, slide_ms, 0, 10000, "drop down into place"),
    CFG_INT(S, close_ms, 0, 10000, "go back up"),
    CFG_INT(S, fade_ms, 0, 10000, "crossfade on a track change"),
    CFG_INT(S, chase_ms, 0, 600000,
            "no track length: the bar's segment crosses it in this long"),
    CFG_INT(S, status_ms, 0, 10000,
            "play/pause: the icon morphs and the colours fade this long"),
    CFG_INT(S, flash_ms, 0, 600000,
            "after play-pause, the panel shows this long"),
};
#undef S

static struct {
  struct swell *sw;
  struct media_config cfg;
  struct layer_surface *surf;
  bool popup_shown, popup_expanded; /* from the popup's bus events */
  bool manual;                      /* `swell media show` */
  bool flash;                       /* `swell media play-pause` */
  struct loop_timer *flash_end;
  int64_t at;                       /* ms: start of the slide */
  double from, to;                  /* 0 -> 1 down, x -> 0 up */
  bool closing; /* the surface is destroyed once the slide up ends */
  /* The track drawn: a copy, so it stays while the panel goes up after
   * the player is gone. */
  bool present; /* a player has a track */
  struct media_track cur, prev;
  cairo_surface_t *cur_art, *prev_art;
  unsigned cur_art_id, prev_art_id;
  bool has_prev; /* prev fades out from fade_at */
  int64_t fade_at;
  /* cur's play/pause look: 0 playing .. 1 paused, tweened */
  int64_t status_at;
  double status_from, status_to;
} m;

static int slide_ms(void) {
  return m.closing ? m.cfg.close_ms : m.cfg.slide_ms;
}

static double shown_value(int64_t now, bool *more) {
  double t = ease_out_cubic(anim_progress(m.at, slide_ms(), now, more));
  return m.from + (m.to - m.from) * t;
}

static double status_value(int64_t now, bool *more) {
  double t = anim_progress(m.status_at, m.cfg.status_ms, now, more);
  return m.status_from + (m.status_to - m.status_from) * t;
}

/* Follow cur's status: tween while shown, else snap. */
static void sync_status(bool tween) {
  double to = m.cur.playing ? 0 : 1;
  if (to == m.status_to)
    return;
  bool more = false;
  int64_t now = now_ms();
  m.status_from = tween ? status_value(now, &more) : to;
  m.status_to = to;
  m.status_at = now;
}

static void drop_prev(void) {
  if (m.prev_art)
    cairo_surface_destroy(m.prev_art);
  m.prev_art = NULL;
  m.has_prev = false;
}

static bool draw(cairo_t *cr, void *data) {
  int64_t now = now_ms();
  bool more = false;
  double shown = shown_value(now, &more);
  double fade = 1;
  if (m.has_prev) {
    bool fading = false;
    fade = anim_progress(m.fade_at, m.cfg.fade_ms, now, &fading);
    if (fading)
      more = true;
    else
      drop_prev();
  }
  double paused = status_value(now, &more);
  struct media_frame f = {
      .shown = shown,
      .cur = {&m.cur, m.cur_art, m.cur_art_id, paused},
      .prev = {m.has_prev ? &m.prev : NULL, m.prev_art, m.prev_art_id,
               m.prev.playing ? 0 : 1},
      .fade = fade,
      .now = now,
  };
  media_render(cr, &m.cfg, &f);
  return more || (m.present && media_render_moving(&m.cur));
}

static void show_surface(void) {
  struct layer_opts opts = {
      .ns = m.cfg.namespace,
      .width = m.cfg.width,
      .height = media_surface_height(&m.cfg),
      .layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
      /* Top, centred horizontally; zone 0 keeps it below an exclusive bar. */
      .anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP,
      .exclusive_zone = 0,
      .click_through = true,
      .keyboard = false,
  };
  layer_surface_show(m.surf, &opts);
}

static bool visible(void) { return layer_surface_shown(m.surf) && !m.closing; }

/* Start a slide from wherever the current one is. */
static void retarget(double to, bool closing) {
  bool more = false;
  int64_t now = now_ms();
  m.from = shown_value(now, &more);
  m.to = to;
  m.at = now;
  m.closing = closing;
}

static void emit(const char *name) {
  char title[300], artist[300], player[100];
  snprintf(title, sizeof title, "SWELL_MEDIA_TITLE=%s", m.cur.title);
  snprintf(artist, sizeof artist, "SWELL_MEDIA_ARTIST=%s", m.cur.artist);
  snprintf(player, sizeof player, "SWELL_MEDIA_PLAYER=%s", m.cur.player);
  bus_emit(name, title, artist, player, NULL);
}

/* Poll while someone may want the panel; show it while it's wanted and
 * there's a track. */
static void update(void) {
  bool poll = m.popup_shown || m.manual || m.flash;
  if (poll && !player_running())
    player_start(&m.cfg);
  else if (!poll && player_running())
    player_stop();

  bool want = (m.popup_expanded || m.manual || m.flash) && m.present;
  if (want == visible())
    return;
  if (want) {
    if (!layer_surface_shown(m.surf)) {
      m.at = now_ms();
      m.from = m.to = 0;
      drop_prev();
      show_surface();
    }
    retarget(1, false);
  } else {
    retarget(0, true);
  }
  layer_surface_redraw(m.surf);
  emit(want ? "media.show" : "media.hide");
}

static void on_player(void *data) {
  const struct media_track *t = player_track();
  unsigned id;
  cairo_surface_t *art = player_art(&id);
  m.present = t->has;
  if (t->has) {
    bool same = m.cur.has && media_same_track(&m.cur, t);
    if (visible() && m.cur.has && !same && m.cfg.fade_ms > 0) {
      /* Crossfade from the track shown. */
      drop_prev();
      m.prev = m.cur;
      m.prev_art = m.cur_art;
      m.prev_art_id = m.cur_art_id;
      m.cur_art = NULL;
      m.cur_art_id = 0;
      m.has_prev = true;
      m.fade_at = now_ms();
    }
    m.cur = *t;
    /* A new track (crossfading in) or a hidden panel takes the state as
     * it is; only a shown track's play/pause morphs. */
    sync_status(same && visible());
    if (id != m.cur_art_id || (art != NULL) != (m.cur_art != NULL)) {
      if (m.cur_art)
        cairo_surface_destroy(m.cur_art);
      m.cur_art = art ? cairo_surface_reference(art) : NULL;
      m.cur_art_id = id;
    }
  }
  update();
  if (layer_surface_shown(m.surf))
    layer_surface_redraw(m.surf);
}

/* The popup's contents are in the wallpaper (which shows the track). */
static bool in_wallpaper(const struct bus_event *ev) {
  for (char *const *e = ev->env; e && *e; ++e)
    if (!strcmp(*e, "SWELL_POPUP_IN=wallpaper"))
      return true;
  return false;
}

static void on_bus(const struct bus_event *ev, void *data) {
  if (!strncmp(ev->name, "popup.", 6) && strcmp(ev->name, "popup.hide") &&
      in_wallpaper(ev))
    return;
  if (!strcmp(ev->name, "popup.show")) {
    m.popup_shown = true;
    m.popup_expanded = false;
  } else if (!strcmp(ev->name, "popup.expand")) {
    m.popup_expanded = true;
  } else if (!strcmp(ev->name, "popup.hide")) {
    m.popup_shown = m.popup_expanded = false;
  } else {
    return;
  }
  update();
}

static void on_flash_end(struct loop_timer *t, void *data) {
  m.flash = false;
  update();
}

/* Destroy the surface once the slide up has run. The timeout ends it even
 * if no frame callbacks arrive (e.g. the output is off). */
static int prepare(void *data) {
  if (!m.closing)
    return -1;
  int64_t left = m.at + m.cfg.close_ms - now_ms();
  if (left > 0)
    return (int)left;
  layer_surface_hide(m.surf);
  m.closing = false;
  drop_prev();
  return -1;
}

static bool init(struct swell *sw, const void *cfg) {
  if (!sw->wl->layer_shell) {
    warn("media: the compositor lacks wlr-layer-shell");
    return false;
  }
  memset(&m, 0, sizeof(m));
  m.sw = sw;
  m.cfg = *(const struct media_config *)cfg;
  m.surf = layer_surface_create(sw->wl, draw, NULL);
  m.flash_end = loop_add_timer(sw->loop, CLOCK_MONOTONIC, on_flash_end, NULL);
  loop_add_prepare(sw->loop, 50, prepare, NULL);
  player_init(sw->loop, on_player, NULL);
  bus_subscribe(on_bus, NULL);
  return true;
}

static void reconfigure(const void *cfg) {
  m.cfg = *(const struct media_config *)cfg;
  /* A new player, interval or cover setting: start polling afresh. */
  if (player_running()) {
    player_stop();
    player_start(&m.cfg);
  }
  if (!layer_surface_shown(m.surf))
    return;
  /* New sizes need a new surface: re-show it, or finish a close now. */
  layer_surface_hide(m.surf);
  if (m.closing) {
    m.closing = false;
    return;
  }
  show_surface();
  layer_surface_redraw(m.surf);
}

static void finish(void) {
  bus_unsubscribe(on_bus, NULL);
  player_finish();
  loop_remove_prepare(m.sw->loop, prepare, NULL);
  loop_remove_timer(m.flash_end);
  m.flash_end = NULL;
  layer_surface_destroy(m.surf);
  m.surf = NULL;
  drop_prev();
  if (m.cur_art)
    cairo_surface_destroy(m.cur_art);
  m.cur_art = NULL;
}

static int command(int argc, char **argv, struct reply *r) {
  const char *word = argc > 0 ? argv[0] : "";
  if (!strcmp(word, "show"))
    m.manual = true;
  else if (!strcmp(word, "hide"))
    m.manual = false;
  else if (!strcmp(word, "toggle"))
    m.manual = !m.manual;
  else if (!strcmp(word, "play-pause")) {
    /* The panel can drop down with the state from before; the poll
     * after the command flips its icon. */
    player_command(&m.cfg, "play-pause");
    if (m.cfg.flash_ms > 0) {
      m.flash = true;
      loop_timer_arm(m.flash_end, m.cfg.flash_ms, 0);
    }
  }
  else if (!strcmp(word, "status")) {
    const struct media_track *t = &m.cur;
    reply_printf(r, "panel: %s%s\n", visible() ? "shown" : "hidden",
                 m.manual ? " (by hand)" : "");
    if (!m.present) {
      reply_printf(r, "track: none%s\n",
                   player_running() ? "" : " (not polling)");
      return 0;
    }
    char pos[32], len[32];
    media_format_time(pos, sizeof pos, media_position(t, now_ms()));
    media_format_time(len, sizeof len, t->len_us);
    reply_printf(r, "track: %s, \"%s\"%s%s%s%s (%s)%s\n",
                 t->playing ? "playing" : "paused", t->title,
                 t->artist[0] ? " — " : "", t->artist,
                 t->album[0] ? " — " : "", t->album, t->player,
                 player_running() ? "" : " (last seen)");
    reply_printf(r, "position: %s / %s\n", pos, t->len_us > 0 ? len : "?");
    reply_printf(r, "cover: %s\n", m.cur_art ? "yes" : "none");
    return 0;
  } else {
    reply_printf(r, "media: unknown command '%s'; expected show, hide, "
                    "toggle, play-pause or status\n",
                 word);
    return 1;
  }
  update();
  return 0;
}

const struct swell_module media_module = {
    .name = "media",
    .usage = "  show     keep the panel shown (while a player has a track)\n"
             "  hide     stop showing it by hand (the popup still shows it)\n"
             "  toggle   show or hide by hand\n"
             "  play-pause  pause or resume the track; the panel shows the\n"
             "           new state for flash_ms\n"
             "  status   the track and the panel's state\n",
    .needs_wayland = true,
    .needs_sway = false,
    .config_size = sizeof(struct media_config),
    .defaults = &defaults,
    .fields = fields,
    .nfields = ARRAY_LEN(fields),
    .init = init,
    .reconfigure = reconfigure,
    .finish = finish,
    .command = command,
};
