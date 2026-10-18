/* wallpaper: the battery bar + Nix snowflake wallpaper, rendered live,
 * with an audio visualiser ringing the snowflake while sound plays.
 *
 * It animates only while the focused workspace is empty (workspace.empty /
 * workspace.occupied on the bus) and the user is not idle
 * (ext-idle-notify-v1). Idle on an empty workspace the screensaver
 * (idle_saver) fades the rest out and the snowflake wanders round the
 * screen, so nothing stays put to burn in; without it, it dozes: only the
 * battery bar pulses (idle_bar_pulse). Otherwise no frame callbacks are
 * requested and no timer is armed: the last frame stays on screen and the
 * module costs nothing; that includes the PipeWire capture, which is torn
 * down (it is while dozing too). Moving
 * the pointer over the wallpaper spins the logo faster.
 *
 * On an empty workspace the popup hands its timeline over (peek.h) and the
 * wallpaper draws its contents instead of the panel (render_peek.c); the
 * screensaver gives way while they show. */
#include "wallpaper.h"

#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "anim.h"
#include "backdrop.h"
#include "battery.h"
#include "bus.h"
#include "cursor-shape-v1-client-protocol.h"
#include "ext-idle-notify-v1-client-protocol.h"
#include "handover.h"
#include "ipc.h"
#include "loop.h"
#include "module.h"
#include "peek.h"
#include "util.h"
#include "wm.h"

static struct {
  struct swell *sw;
  struct wallpaper_config cfg;
  bool ws_empty, idle, animating;
  bool dozing; /* idle on an empty workspace: only the bar pulses */
  bool saving; /* idle on an empty workspace: the snowflake wanders */
  double saver_mix; /* 0 (centred, all shown) .. 1 (wandering) */
  double saver_t;   /* s along the wander */
  double drift_t;   /* s along the map's drift */

  /* battery: percent is the target, -1 when no battery was found */
  int battery;
  int override; /* -1: auto */
  struct loop_timer *battery_timer;

  /* idle */
  struct ext_idle_notification_v1 *idle_note;
  struct loop_timer *idle_timer; /* fallback without ext-idle-notify */

  /* animation */
  int64_t last; /* ms of the last advance */
  double angle, boost, phase;
  double tween_from, tween_to, tween_elapsed; /* percent, percent, ms */
  bool have_pointer;
  double px, py;
  int64_t last_motion; /* ms of the last boosting pointer motion */
  struct wp_viz viz;
  struct wp_viz_in viz_in; /* the last audio block's band levels */
  double viz_time; /* s, drives the bars' wobble */
  struct wp_react react;
  double spin_glow; /* spin_glow(), smoothed so beats swell, not flash */
  double playing;   /* 0..1: music is playing, faded in and out */

  /* what's playing: the details shown (kept through a fade-out) */
  char title[256], artist[256];
  double media_alpha; /* the details' opacity, following the player */
  double progress;    /* the track's, 0..1, -1: none */
  bool timed;         /* the player gives the track's length */

  /* the launch or power-off animation: a power-off ends in black, which
   * stays outro_hold_ms (no frames), then fades back over SHOW_RETURN_MS */
  enum wp_show show;
  int64_t show_start; /* ms */
  bool show_black;    /* a power-off's black frame has been drawn */
  struct loop_timer *hold_timer;

  /* the popup's timeline while its contents are drawn here, else NULL */
  const struct peek *peek;

  /* the topo map: shown on an empty workspace, faded out under windows */
  double map_alpha;
  int64_t map_last; /* ms of its last step */
  /* rings spreading from bass kicks (topo_waves) */
  struct {
    double age, size; /* s; react_pulse_min..1 */
  } rings[WP_RINGS];
  int nrings;
  unsigned kicks_seen;

  /* a restart (handover.h). Replaced: our lines fade out from leave_at.
   * Replacing: nothing is drawn before join_at (the old lines are gone),
   * then ours fade in from the first frame (join_at then). Through it the
   * snowflake follows one curve both daemons compute (handover_angle): from
   * join_angle at join_from, turning join_spin rad/s, eased to join_rest; the
   * old daemon goes once every output shows us (joining). What's
   * playing shows at once: the first frame waits (until join_media_until at
   * most) for the first poll and its cover, and the details start as bright as
   * the old ones. */
  int64_t leave_at, join_at, join_from; /* ms, 0: none */
  double join_angle, join_spin, join_rest;
  bool join_turn; /* the angle is still to be taken over */
  bool joining;
  bool join_media;
  int64_t join_media_until;

  /* measured frame rate */
  int64_t fps_start;
  int fps_frames;
  double fps;
} w;

const struct wallpaper_config *wp_config(void) { return &w.cfg; }
bool wp_animating(void) { return w.animating; }
bool wp_dozing(void) { return w.dozing; }
bool wp_saving(void) { return w.saving; }

#define SHOW_RETURN_MS 800

static bool outro_show(void) {
  return w.show == WP_SHOW_SCATTER || w.show == WP_SHOW_CRT;
}

/* After a power-off: black, waiting for hold_timer. */
static bool show_holding(void) {
  return outro_show() && w.show_black &&
         now_ms() - w.show_start <
             wp_show_ms(&w.cfg, w.show) + (int64_t)w.cfg.outro_hold_ms;
}

bool wp_showing(void) { return w.show != WP_SHOW_NONE; }

/* The screensaver gives way to the popup's contents. */
static bool saver_on(void) { return w.saving && !w.peek; }

static double map_target(void) { return w.ws_empty && !saver_on() ? 1 : 0; }

/* The map is fading out or in (only the topo style has one). */
static bool map_fading(void) {
  return wp_topo(&w.cfg) && fabs(w.map_alpha - map_target()) > 1e-6;
}

/* The lines fading out (replaced) or in (replacing). */
static bool handing(void) {
  int64_t at = w.leave_at ? w.leave_at : w.join_at;
  return at && now_ms() < at + w.cfg.handover_ms;
}

bool wp_ticking(void) {
  if (show_holding())
    return false;
  return w.animating || w.dozing || w.saving || w.saver_mix > 0 ||
         wp_showing() || map_fading() || w.peek || handing();
}

static bool viz_showing(void) {
  int n = w.cfg.viz_bars;
  for (int s = 0; s < 2; ++s)
    for (int i = 0; i < n; ++i)
      if (w.viz.bars[s][i] >= 1e-3)
        return true;
  return false;
}

/* The logo still shows the music (it lingers a little after the bars). */
static bool reacting(void) {
  if (w.react.loud > 1e-3 || w.react.pulse > 0 || w.react.kick > 0 ||
      w.react.shake > 0 ||
      w.spin_glow > 1e-3 || w.playing > 1e-3 || w.nrings > 0)
    return true;
  for (int k = 0; k < 6; ++k)
    if (w.react.slices[k] > 1e-3)
      return true;
  return false;
}

/* The resting spin is slow enough for a low rate; a pointer-boosted one
 * and moving bars need more frames to look smooth. */
int wp_fps_cap(void) {
  if (wp_showing() || map_fading() || handing())
    return 0;
  /* Smooth while it pops, expands, the hold runs or the lines spring. */
  if (w.peek && wp_peek_moving(w.peek, now_ms()))
    return 0;
  if (w.saving)
    return w.cfg.saver_fps;
  if (w.boost > 0.05)
    return w.cfg.fps_spin;
  return viz_showing() || reacting() ? w.cfg.fps_audio : w.cfg.fps;
}

/* ---- animation ---- */

static double target_percent(void) {
  if (w.override >= 0)
    return w.override;
  return w.battery >= 0 ? w.battery : 100;
}

static double shown_percent(void) {
  double t = w.cfg.tween_ms > 0 ? w.tween_elapsed / w.cfg.tween_ms : 1;
  return w.tween_from + (w.tween_to - w.tween_from) * ease_out_cubic(t);
}

static void set_target(double pct) {
  if (pct == w.tween_to)
    return;
  if (wp_ticking()) {
    w.tween_from = shown_percent();
    w.tween_elapsed = 0;
  } else {
    w.tween_from = pct; /* nobody sees a tween while frozen */
  }
  w.tween_to = pct;
  if (!wp_ticking())
    wp_surfaces_draw(false);
}

/* How fast the snowflake turns now, rad/s. */
static double spin_omega(void) {
  double music = w.cfg.react_spin
                     ? w.cfg.react_spin_gain * w.react.energy + w.react.kick
                     : 0;
  double omega = w.cfg.spin_speed * (1 + w.boost + music);
  return copysign(fmin(fabs(omega), w.cfg.spin_visible_max), omega) * M_PI /
         180;
}

/* A restart's spin eases from join_spin to join_rest with this time
 * constant, s. */
#define HANDOVER_SPIN_TAU 0.3

/* Where the snowflake is at now on a restart's curve: the old daemon turns
 * it so after answering, the new one starts from it. */
static double handover_angle(int64_t now) {
  double t = fmax((now - w.join_from) / 1000.0, 0);
  double ease = HANDOVER_SPIN_TAU * (1 - exp(-t / HANDOVER_SPIN_TAU));
  return fmod(w.join_angle + w.join_rest * t +
                  (w.join_spin - w.join_rest) * ease,
              2 * M_PI);
}

/* Returns the time advanced, in seconds. bar_only: the logo stays put. */
static double advance(int64_t now, bool bar_only) {
  /* Clamped, so a stall (or a freeze) doesn't make anything jump. */
  double dt = (now - w.last) / 1000.0;
  w.last = now;
  if (dt <= 0)
    return 0;
  if (dt > 0.1)
    dt = 0.1;
  w.tween_elapsed += dt * 1000;
  w.phase = fmod(w.phase + dt / wp_pulse_period(&w.cfg, shown_percent()), 1);
  if (bar_only)
    return dt;
  if (w.leave_at)
    w.angle = handover_angle(now);
  else
    w.angle = fmod(w.angle + spin_omega() * dt, 2 * M_PI);
  if (now - w.last_motion >= w.cfg.spin_hold_ms)
    w.boost *= exp(-dt / w.cfg.spin_decay_s);
  if (w.boost < 1e-3)
    w.boost = 0;
  return dt;
}

static double spin_glow(void);

/* Age the rings by dt and start one on a new kick. */
static void advance_rings(double dt) {
  int n = 0;
  for (int i = 0; i < w.nrings; ++i) {
    w.rings[i].age += dt;
    if (w.rings[i].age * 1000 < w.cfg.topo_ring_ms)
      w.rings[n++] = w.rings[i];
  }
  w.nrings = n;
  if (w.react.kicks == w.kicks_seen)
    return;
  w.kicks_seen = w.react.kicks;
  if (!wp_topo(&w.cfg) || !w.cfg.topo_waves)
    return;
  if (w.nrings == WP_RINGS) /* the oldest makes way */
    memmove(w.rings, w.rings + 1, sizeof(w.rings[0]) * --w.nrings);
  w.rings[w.nrings].age = 0;
  w.rings[w.nrings++].size = w.react.kick_size;
}

/* Fade the map towards the workspace's state. */
static void advance_map(int64_t now) {
  double dt = fmin(fmax((now - w.map_last) / 1000.0, 0), 0.1);
  w.map_last = now;
  double target = map_target();
  if (w.cfg.topo_fade_ms <= 0 || !wp_topo(&w.cfg)) {
    w.map_alpha = target;
    return;
  }
  double step = dt * 1000 / w.cfg.topo_fade_ms;
  w.map_alpha = target > w.map_alpha ? fmin(w.map_alpha + step, target)
                                     : fmax(w.map_alpha - step, target);
}

/* Ease the snowflake off the centre (saving) or back, and wander on. */
static void advance_saver(double dt) {
  double target = saver_on() ? 1 : 0;
  double step = w.cfg.saver_fade_ms > 0 ? dt * 1000 / w.cfg.saver_fade_ms : 1;
  w.saver_mix = target > w.saver_mix ? fmin(w.saver_mix + step, target)
                                     : fmax(w.saver_mix - step, target);
  w.saver_t = w.saver_mix > 0 ? fmod(w.saver_t + dt, 86400) : 0;
}

/* The map's drift moves on while animating only; a window stops it. The
 * x and y rates are 3:4, so wrapping at four x periods doesn't jump. */
static void advance_drift(double dt) {
  w.drift_t = fmod(w.drift_t + dt, 4 * w.cfg.topo_drift_s);
}

static double smooth(double t) { return t * t * (3 - 2 * t); }

/* Black over the map for a restart: 0 .. 1 as the old lines go, 1 .. 0 as
 * the new ones come. */
static double handover_fade(int64_t now) {
  double ms = w.cfg.handover_ms;
  if (w.leave_at)
    return ms > 0 ? smooth(fmin(fmax((now - w.leave_at) / ms, 0), 1)) : 1;
  if (w.join_at)
    return ms > 0 ? 1 - smooth(fmin(fmax((now - w.join_at) / ms, 0), 1)) : 0;
  return 0;
}

/* A triangle wave through 0 at p = 0, rising: the bounce off two edges. */
static double bounce(double p) { return 2 / M_PI * asin(sin(2 * M_PI * p)); }

/* Where the wander is: -1..1 of the way to each edge. The rates follow the
 * reference panel (16:10), so the snowflake moves about saver_speed. */
static void wander(double *x, double *y) {
  *x = *y = 0;
  if (w.saver_mix <= 0 || w.cfg.saver_speed <= 0)
    return;
  double r = wp_logo_reach(&w.cfg, (int)WP_REF_WIDTH);
  double tx = fmax(WP_REF_WIDTH / 2 - r, 1);
  double ty = fmax(WP_REF_WIDTH * 5 / 16 - r, 1);
  /* One full bounce is four runs from the middle to an edge. */
  double m = smooth(w.saver_mix);
  *x = m * bounce(w.saver_t * w.cfg.saver_speed / (4 * tx));
  *y = m * bounce(w.saver_t * w.cfg.saver_speed / (4 * ty));
}

static void fetch_audio(float *left, float *right, int *rate, void *data) {
  wp_audio_samples(left, right, rate);
}

/* Move the bars on by dt towards the latest audio (silence without any). */
static void advance_viz(double dt) {
  if (dt <= 0)
    return;
  uint64_t seq = w.cfg.viz ? wp_audio_seq() : 0;
  wp_viz_step(&w.viz, &w.cfg,
              wp_viz_levels(&w.viz_in, seq, fetch_audio, NULL, &w.cfg), dt);
  wp_react_step(&w.react, &w.viz, &w.cfg, dt);
  advance_rings(dt);
  double target = spin_glow();
  double tau = target > w.spin_glow ? 0.3 : 0.8;
  w.spin_glow = target + (w.spin_glow - target) * exp(-dt / tau);
  target = viz_showing() ? 1 : 0;
  tau = target > w.playing ? 0.4 : 1.5;
  w.playing = target + (w.playing - target) * exp(-dt / tau);
  w.viz_time = fmod(w.viz_time + dt, 3600);
}

/* Move the details and the hexagon on by dt towards the player. */
static void advance_media(int64_t now, double dt) {
  const struct wp_media *m = wp_media_get();
  bool has = w.cfg.media && m->has;
  if (has) {
    snprintf(w.title, sizeof w.title, "%s", m->title);
    snprintf(w.artist, sizeof w.artist, "%s", m->artist);
  }
  double target = has ? (m->playing ? 1 : w.cfg.media_paused_alpha) : 0;
  double step = w.cfg.media_fade_ms > 0 ? dt * 1000 / w.cfg.media_fade_ms : 1;
  w.media_alpha = target > w.media_alpha
                      ? fmin(w.media_alpha + step, target)
                      : fmax(w.media_alpha - step, target);
  w.progress = has && w.cfg.media_progress ? wp_media_progress(m, now) : -1;
  w.timed = has && m->len_us > 0;
}

/* The logo brightens with the pointer boost, fully at spin_max. */
static double glow(void) {
  return w.cfg.spin_max > 0 ? fmin(w.boost / w.cfg.spin_max, 1) : 0;
}

/* How fast the music spins the logo, 0 (resting) .. 1 (spin_visible_max). */
static double spin_glow(void) {
  if (!w.cfg.react_spin)
    return 0;
  double rest = fabs(w.cfg.spin_speed), cap = w.cfg.spin_visible_max;
  double music = w.cfg.react_spin_gain * w.react.energy + w.react.kick;
  double v = fmin(rest * (1 + music), cap);
  return cap > rest ? fmin(fmax((v - rest) / (cap - rest), 0), 1) : 0;
}

/* Speed past spin_visible_max, as a fraction of the most there can be. */
static double over(void) {
  double cap = w.cfg.spin_visible_max;
  double v = fabs(w.cfg.spin_speed) * (1 + w.boost);
  double vmax = fabs(w.cfg.spin_speed) * (1 + w.cfg.spin_max);
  return vmax > cap ? fmin(fmax((v - cap) / (vmax - cap), 0), 1) : 0;
}

/* Turn f into the show's frame at now, and end the show when it's over. */
static void show_frame(int64_t now, struct wp_frame *f) {
  if (!wp_showing())
    return;
  double t = now - w.show_start;
  int len = wp_show_ms(&w.cfg, w.show);
  if (w.show == WP_SHOW_INTRO) {
    if (t >= len)
      w.show = WP_SHOW_NONE;
    else
      wp_show_apply(&w.cfg, w.show, t, f);
    return;
  }
  double hold_end = len + (double)w.cfg.outro_hold_ms;
  if (t < hold_end) {
    wp_show_apply(&w.cfg, w.show, t, f);
    if (t >= len)
      w.show_black = true;
    return;
  }
  double u = (t - hold_end) / SHOW_RETURN_MS;
  if (u >= 1) {
    w.show = WP_SHOW_NONE;
    return;
  }
  f->fade = 1 - u;
}

/* After the expand the gathered lines spring back out: a damped wave of
 * this period and decay, still after GATHER_SPRING_MS. */
#define GATHER_PERIOD_S 0.5
#define GATHER_DECAY_S 0.18
#define GATHER_SPRING_MS 1500

void wp_peek_frame(const struct peek *pk, bool ws_empty, int64_t now,
                   struct wp_frame *f) {
  f->peek = NULL;
  f->peek_pop = f->peek_expand = f->peek_hold = f->peek_gather = 0;
  if (!pk || !ws_empty)
    return;
  struct peek_values v;
  peek_values(pk, now, &v);
  f->peek = pk->info;
  f->peek_low_pct = pk->battery_low_pct;
  f->peek_pop = v.pop;
  f->peek_expand = v.expand;
  f->peek_hold = v.hold;
  /* Drawn in through the hold (going with the pop if let go early). */
  double g = smooth(fmin(fmax(v.hold, 0), 1));
  if (pk->expand_at) {
    double t = (now - pk->expand_at) / 1000.0;
    g = now - pk->expand_at < GATHER_SPRING_MS
            ? cos(2 * M_PI * t / GATHER_PERIOD_S) * exp(-t / GATHER_DECAY_S)
            : 0;
  }
  f->peek_gather = g * v.pop;
}

bool wp_peek_moving(const struct peek *pk, int64_t now) {
  struct peek_values v;
  peek_values(pk, now, &v);
  return v.more ||
         (pk->expand_at && now - pk->expand_at < GATHER_SPRING_MS && v.pop > 0);
}

void wp_frame_at(int64_t now, struct wp_frame *f) {
  if (w.join_turn) {
    w.join_turn = false;
    w.join_at = now; /* our lines fade in from here */
    w.angle = handover_angle(now);
    w.last = now;
  }
  if (w.animating) {
    double dt = advance(now, false);
    advance_viz(dt);
    advance_media(now, dt);
    advance_saver(dt);
    advance_drift(dt);
  } else if (w.saving) {
    advance_saver(advance(now, false));
  } else if (w.dozing)
    advance(now, true);
  f->percent = shown_percent();
  f->level = wp_bar_level(&w.cfg, w.phase);
  f->angle = w.angle;
  f->glow = glow();
  if (w.cfg.react_glow) {
    /* A base brightness while music plays, the reactions on top of it. */
    double base = w.cfg.react_glow_base * w.playing;
    double react = fmax(fmin(w.cfg.react_glow_amount * w.react.loud, 1),
                        w.cfg.react_glow_spin * w.spin_glow);
    f->glow = fmax(f->glow, base + (1 - base) * react);
  }
  f->pulse = w.cfg.react_pulse ? w.react.pulse : 0;
  for (int k = 0; k < 6; ++k)
    f->slide[k] =
        w.cfg.react_slide ? w.cfg.react_slide_dist * w.react.slices[k] : 0;
  /* Two sines at unrelated rates, so the shake doesn't run on a line. */
  double shake = w.cfg.react_shake ? w.cfg.react_shake_px * w.react.shake : 0;
  f->shake_x = shake * sin(2 * M_PI * 11 * w.react.shake_t);
  f->shake_y = shake * sin(2 * M_PI * 14 * w.react.shake_t + 1.3);
  wander(&f->wander_x, &f->wander_y);
  /* The screensaver fades the title and the bar out. */
  double saver = smooth(w.saver_mix);
  f->media_alpha = w.media_alpha * (1 - saver);
  /* Idle, the hexagon (cover and progress) goes; the title stays. Off the
   * centre it goes too, and without a track length it never shows. */
  bool hex = w.animating && w.saver_mix <= 0 && w.timed;
  f->progress = hex ? w.progress : -1;
  f->art = hex && w.cfg.media_art ? wp_media_art(&f->art_id) : NULL;
  f->title = w.title;
  f->artist = w.artist;
  f->over = over();
  f->dir = w.cfg.spin_speed < 0 ? -1 : 1;
  /* Under the topo style the lines are the visualiser. */
  f->nbars = viz_showing() && !wp_topo(&w.cfg) ? w.cfg.viz_bars : 0;
  for (int s = 0; s < 2; ++s)
    for (int i = 0; i < f->nbars; ++i)
      f->bars[s][i] = (float)fmin(
          w.viz.bars[s][i] * wp_viz_wobble(&w.cfg, s, i, w.viz_time), 1);
  for (int k = 0; k < 6; ++k)
    f->lambda_fade[k] = 0;
  f->logo_shrink = f->flash = f->fade = f->bg_fade = 0;
  f->bar_fade = saver;
  f->crt_w = f->crt_h = f->crt_alpha = 0;
  advance_map(now);
  f->map_fade = 1 - w.map_alpha;
  f->map_under = false;
  bool waves = wp_topo(&w.cfg) && w.cfg.topo_waves;
  f->sway = waves ? fmin(w.react.loud * w.playing, 1) : 0;
  f->time = w.viz_time;
  double drift = wp_topo(&w.cfg) ? w.cfg.topo_drift_px : 0;
  f->drift_x = drift * sin(2 * M_PI * w.drift_t / w.cfg.topo_drift_s);
  f->drift_y = drift * sin(2 * M_PI * w.drift_t * 0.75 / w.cfg.topo_drift_s);
  f->spec_on = waves && viz_showing() &&
               (w.cfg.topo_spectrum_px > 0 || w.cfg.topo_ripple_px > 0 ||
                w.cfg.topo_light > 0 || w.cfg.topo_light_width > 0);
  for (int s = 0; s < 2; ++s)
    if (f->spec_on)
      wp_viz_spectrum(w.viz.bars[s], w.cfg.viz_bars, f->spec[s]);
    else
      memset(f->spec[s], 0, sizeof(f->spec[s]));
  f->nrings = waves ? w.nrings : 0;
  for (int i = 0; i < f->nrings; ++i) {
    double life = 1 - w.rings[i].age * 1000 / w.cfg.topo_ring_ms;
    f->rings[i].radius = w.rings[i].age * w.cfg.topo_ring_speed;
    f->rings[i].size = w.rings[i].size * life * life;
  }
  f->bg_fade = handover_fade(now);
  wp_peek_frame(w.peek, w.ws_empty, now, f);
  f->peek_area = false;
  show_frame(now, f);
}

void wp_count_frame(int64_t now) {
  w.fps_frames++;
  if (now - w.fps_start >= 1000) {
    w.fps = w.fps_frames * 1000.0 / (now - w.fps_start);
    w.fps_start = now;
    w.fps_frames = 0;
  }
}

/* ---- battery ---- */

static void read_battery(void) {
  int pct;
  bool charging;
  w.battery = battery_read(&pct, &charging) ? pct : -1;
  if (w.battery > 100)
    w.battery = 100;
  set_target(target_percent());
}

static void on_battery_timer(struct loop_timer *t, void *data) {
  read_battery();
}

/* ---- the launch and power-off animations ---- */

static void show_start(enum wp_show s) {
  w.show = s;
  w.show_start = now_ms();
  w.show_black = false;
  loop_timer_disarm(w.hold_timer);
  if (outro_show())
    loop_timer_arm(w.hold_timer,
                   wp_show_ms(&w.cfg, s) + (int64_t)w.cfg.outro_hold_ms, 0);
  bus_emit(s == WP_SHOW_INTRO ? "wallpaper.intro" : "wallpaper.outro",
           s == WP_SHOW_INTRO   ? NULL
           : s == WP_SHOW_CRT ? "SWELL_OUTRO=crt"
                                : "SWELL_OUTRO=scatter",
           NULL);
  wp_surfaces_draw(false); /* and every draw asks for the next frame */
}

static void show_stop(void) {
  if (!wp_showing())
    return;
  w.show = WP_SHOW_NONE;
  loop_timer_disarm(w.hold_timer);
  wp_surfaces_draw(false); /* the normal frame, whole */
}

/* The black is over: fade the wallpaper back in. */
static void on_hold_timer(struct loop_timer *t, void *data) {
  if (outro_show())
    wp_surfaces_draw(false);
}

/* The intro plays once per login session: the marker sits next to the
 * control socket in the runtime dir, which goes with the session. */
static bool intro_first(void) {
  char path[4096];
  snprintf(path, sizeof path, "%s.intro", ipc_socket_path());
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0)
    return false;
  close(fd);
  return true;
}

/* ---- the animate/freeze state machine ---- */

static void arm_idle_fallback(void) {
  if (!w.idle_note && w.cfg.idle_timeout_s > 0 && w.animating)
    loop_timer_arm(w.idle_timer, w.cfg.idle_timeout_s * 1000LL, 0);
}

static void update_animating(void) {
  bool want = w.ws_empty && !w.idle;
  bool save = w.ws_empty && w.idle && w.cfg.idle_saver;
  bool doze = w.ws_empty && w.idle && !save && w.cfg.battery_bar &&
              w.cfg.idle_bar_pulse;
  if (want == w.animating && doze == w.dozing && save == w.saving)
    return;
  bool was_animating = w.animating, was_ticking = wp_ticking();
  bool was_saving = w.saving;
  w.animating = want;
  w.dozing = doze;
  w.saving = save;
  if (save && !was_saving)
    bus_emit("wallpaper.saver", NULL);
  /* A window over the wandering snowflake: the plain wallpaper, centred. */
  bool recentre = !w.ws_empty && w.saver_mix > 0;
  if (recentre)
    w.saver_mix = w.saver_t = 0;
  /* The bar (fill and pulse) moves in both states, so the battery is read
   * in both. */
  if (wp_ticking() && !was_ticking) {
    w.last = now_ms();
    w.fps_start = w.last;
    w.fps_frames = 0;
    int64_t ms = w.cfg.battery_interval_s * 1000LL;
    loop_timer_arm(w.battery_timer, ms, ms);
    read_battery();
  } else if (!wp_ticking()) {
    loop_timer_disarm(w.battery_timer);
  }
  /* A window open freezes the plain wallpaper, as before the details
   * existed: no title and no hexagon. Idle on an empty workspace keeps
   * the title only. */
  /* A show is only for the wallpaper; once it's covered, it's over. */
  if (!w.ws_empty)
    show_stop();
  bool cleared = !w.ws_empty && w.media_alpha > 0;
  if (cleared)
    w.media_alpha = 0;
  if (want) {
    arm_idle_fallback();
    if (w.cfg.viz)
      wp_audio_start(w.sw->loop, &w.cfg);
    if (w.cfg.media)
      wp_media_start(w.sw->loop, &w.cfg);
    wp_surfaces_draw(false); /* and every draw asks for the next frame */
    bus_emit("wallpaper.animate", NULL);
    return;
  }
  if (!was_animating) {
    if (doze || save || cleared || recentre)
      wp_surfaces_draw(false); /* start the bar's (or saver's) frames */
    return;
  }
  loop_timer_disarm(w.idle_timer);
  wp_audio_stop();
  wp_media_stop(); /* idle, the frozen frame keeps the details it shows */
  bool glowing = w.boost > 0 || viz_showing() || reacting();
  w.boost = 0;
  memset(&w.viz, 0, sizeof(w.viz));
  memset(&w.react, 0, sizeof(w.react));
  w.kicks_seen = 0;
  w.nrings = 0;
  w.spin_glow = 0;
  w.playing = 0;
  if (!doze && !save)
    w.fps = 0;
  /* A window opened (or the user idled) mid-spin or mid-song: don't
   * leave the brightened logo, the bars or the hexagon as the frozen
   * frame. Dozing draws on for the bar. */
  if (glowing || doze || save || cleared || w.media_alpha > 0)
    wp_surfaces_draw(false);
  bus_emit("wallpaper.freeze", NULL);
}

static void on_bus(const struct bus_event *ev, void *data) {
  if (strcmp(ev->name, "workspace.empty") == 0)
    w.ws_empty = true;
  else if (strcmp(ev->name, "workspace.occupied") == 0)
    w.ws_empty = false;
  else
    return;
  /* The map fades with its own frames, even into a frozen wallpaper. */
  bool was_ticking = wp_ticking();
  if (!was_ticking)
    w.map_last = now_ms();
  update_animating();
  if (map_fading() && !was_ticking && !wp_animating() && !wp_dozing())
    wp_surfaces_draw(false); /* and every draw asks for the next frame */
}

/* ---- the popup's contents ---- */

/* The popup asks before it shows: only over the plain empty workspace,
 * not in a show. */
static bool peek_want(void) { return w.ws_empty && !wp_showing(); }

static void peek_update(const struct peek *pk) {
  bool was_ticking = wp_ticking();
  w.peek = pk;
  if (!was_ticking) {
    w.last = w.map_last = now_ms();
    w.fps_start = w.last;
    w.fps_frames = 0;
  }
  /* A tick or new info shows in the next frame anyway; a still wallpaper
   * needs this one (and every draw asks for the next while ticking). */
  if (!was_ticking || !pk)
    wp_surfaces_draw(false);
}

static const struct peek_sink peek_sink = {
    .want = peek_want,
    .update = peek_update,
};

/* ---- the map under the overview ---- */

/* The topo map without its motion, at half the buffer's resolution (the
 * overview dims it anyway; a quarter of the first draw's cost), kept in
 * topo.c's cache for the next open. */
static bool backdrop_paint(cairo_t *cr, int lw, int lh, double scale) {
  if (!wp_topo(&w.cfg) || lw <= 0 || lh <= 0)
    return false;
  int pw = (int)lround(lw * scale / 2), ph = (int)lround(lh * scale / 2);
  if (pw < 1 || ph < 1)
    return false;
  cairo_save(cr);
  cairo_scale(cr, (double)lw / pw, (double)lh / ph);
  cairo_set_source_surface(cr, wp_topo_background(&w.cfg, pw, ph), 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
  cairo_paint(cr);
  cairo_restore(cr);
  return true;
}

static const struct backdrop_source backdrop_source = {
    .paint = backdrop_paint,
};

/* ---- idle ---- */

static void idled(void *data, struct ext_idle_notification_v1 *n) {
  w.idle = true;
  update_animating();
}

static void resumed(void *data, struct ext_idle_notification_v1 *n) {
  w.idle = false;
  update_animating();
}

static const struct ext_idle_notification_v1_listener idle_listener = {
    .idled = idled,
    .resumed = resumed,
};

static void on_idle_timer(struct loop_timer *t, void *data) {
  w.idle = true;
  update_animating();
}

static void setup_idle(void) {
  if (w.idle_note)
    ext_idle_notification_v1_destroy(w.idle_note);
  w.idle_note = NULL;
  loop_timer_disarm(w.idle_timer);
  w.idle = false;
  struct swell_wl *wl = w.sw->wl;
  if (w.cfg.idle_timeout_s > 0 && wl->idle_notifier && wl->seat) {
    w.idle_note = ext_idle_notifier_v1_get_idle_notification(
        wl->idle_notifier, w.cfg.idle_timeout_s * 1000u, wl->seat);
    ext_idle_notification_v1_add_listener(w.idle_note, &idle_listener, NULL);
  }
  arm_idle_fallback();
}

/* ---- pointer ---- */

static void pointer_enter(void *data, struct wl_surface *s, double x,
                          double y) {
  wl_set_cursor(w.sw->wl, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
  w.have_pointer = true;
  w.px = x;
  w.py = y;
}

static void pointer_leave(void *data, struct wl_surface *s) {
  w.have_pointer = false;
}

static void pointer_motion(void *data, double x, double y) {
  double d = w.have_pointer ? hypot(x - w.px, y - w.py) : 0;
  w.have_pointer = true;
  w.px = x;
  w.py = y;
  if (!w.idle_note && w.idle) {
    /* Without ext-idle-notify only motion over the wallpaper wakes us. */
    w.idle = false;
    update_animating();
    return;
  }
  if (!w.animating)
    return;
  arm_idle_fallback();
  w.boost = fmin(w.boost + w.cfg.spin_gain * d, w.cfg.spin_max);
  if (d > 0)
    w.last_motion = now_ms();
}

const struct swell_input wp_input = {
    .pointer_enter = pointer_enter,
    .pointer_leave = pointer_leave,
    .pointer_motion = pointer_motion,
};

/* ---- module ---- */

/* An unknown style draws as topo. */
static void check_style(void) {
  if (strcmp(w.cfg.style, "topo") && strcmp(w.cfg.style, "classic"))
    warn("wallpaper: unknown style '%s' (topo or classic); using topo",
         w.cfg.style);
}

/* topo_new_map: one map per daemon, so reloads keep it. */
static void pick_map(void) {
  static int seed;
  static double angle;
  if (!w.cfg.topo_new_map)
    return;
  if (!seed) {
    seed = 1 + (int)(drand48() * 1000000);
    angle = floor(drand48() * 360);
  }
  w.cfg.topo_seed = seed;
  w.cfg.topo_angle = angle;
}

/* ---- restarts ---- */

/* The old daemon stays this long past its lines going, at most, for us. */
#define HANDOVER_STAY_MS 6000
/* Replacing it, the first frame waits at most this long for what's
 * playing. */
#define JOIN_MEDIA_MS 1000

int wp_handover(struct reply *r) {
  int64_t now = now_ms();
  bool lines = wp_topo(&w.cfg) && w.map_alpha > 0 && w.cfg.handover_ms > 0;
  int64_t clear = now + (lines ? w.cfg.handover_ms : 0);
  /* From here both turn the snowflake on one curve (handover_angle);
   * frozen, it stays put. */
  bool turning = w.animating || w.saving;
  w.join_from = now;
  w.join_angle = w.angle;
  w.join_spin = turning ? spin_omega() : 0;
  w.join_rest = turning ? w.cfg.spin_speed * M_PI / 180 : 0;
  reply_printf(r,
               "wallpaper now=%lld clear=%lld angle=%.9f spin=%.9f rest=%.9f "
               "media=%.4f\n",
               (long long)now, (long long)clear, w.join_angle, w.join_spin,
               w.join_rest, w.media_alpha);
  bool was_ticking = wp_ticking();
  w.leave_at = now; /* (a frozen wallpaper's lines are already gone) */
  if (lines && !was_ticking)
    wp_surfaces_draw(false); /* and every draw asks for the next frame */
  return (int)(clear - now) + HANDOVER_STAY_MS;
}

/* Replacing a daemon whose wallpaper said where it was. */
static void join(void) {
  const char *s = handover_get("wallpaper");
  long long now, clear;
  double media;
  if (!s ||
      sscanf(s, "now=%lld clear=%lld angle=%lf spin=%lf rest=%lf media=%lf",
             &now, &clear, &w.join_angle, &w.join_spin, &w.join_rest,
             &media) != 6)
    return;
  w.media_alpha = fmin(fmax(media, 0), 1);
  w.join_from = now;
  w.join_at = clear > 0 ? clear : 1;
  w.join_turn = w.joining = true;
  handover_hold();
}

/* The first poll is in, and its cover if it has one. */
static bool media_in(void) {
  const struct wp_media *m = wp_media_get();
  unsigned id;
  return !m->has || !w.cfg.media_art || !m->art_url[0] || wp_media_art(&id);
}

void wp_media_in(void) {
  if (!w.join_media || !media_in())
    return;
  w.join_media = false;
  wp_surfaces_draw(false); /* the held first frame, now or re-timed */
}

int64_t wp_join_wait(void) {
  int64_t until = w.join_at;
  if (w.join_media && w.join_media_until > until)
    until = w.join_media_until;
  int64_t left = until - now_ms();
  return left > 0 ? left : 0;
}

bool wp_joining(void) { return w.joining; }

void wp_joined(void) {
  if (!w.joining)
    return;
  w.joining = false;
  handover_release();
}

bool wp_init(struct swell *sw, const void *cfg) {
  if (!sw->wl->layer_shell) {
    warn("wallpaper: the compositor lacks wlr-layer-shell");
    return false;
  }
  memset(&w, 0, sizeof(w));
  w.sw = sw;
  w.cfg = *(const struct wallpaper_config *)cfg;
  w.override = -1;
  check_style();
  wp_audio_init();
  w.battery_timer =
      loop_add_timer(sw->loop, CLOCK_MONOTONIC, on_battery_timer, NULL);
  w.idle_timer = loop_add_timer(sw->loop, CLOCK_MONOTONIC, on_idle_timer, NULL);
  w.hold_timer = loop_add_timer(sw->loop, CLOCK_MONOTONIC, on_hold_timer, NULL);
  srand48(now_ms() ^ getpid());
  pick_map();
  join();
  int pct;
  bool charging;
  w.battery = battery_read(&pct, &charging) ? (pct > 100 ? 100 : pct) : -1;
  w.tween_from = w.tween_to = target_percent();
  setup_idle();
  wp_surfaces_init(sw->wl); /* a frozen first frame on every output */

  bus_subscribe(on_bus, NULL);
  peek_set_sink(&peek_sink);
  backdrop_set_source(&backdrop_source);
  int empty = wm_focused_empty();
  if (empty < 0 && sw->wm != WM_NONE) {
    wm_emit_focused_state(); /* the answer arrives through on_bus */
    empty = wm_focused_empty();
  }
  w.ws_empty = empty != 0; /* without a window manager: always animate */
  w.map_alpha = map_target();
  w.map_last = now_ms();
  if (w.join_at && w.ws_empty && !w.idle && w.cfg.media) {
    w.join_media = true;
    w.join_media_until = now_ms() + JOIN_MEDIA_MS;
  }
  update_animating();
  if (w.cfg.intro && w.animating && !w.join_at && intro_first())
    show_start(WP_SHOW_INTRO);
  return true;
}

void wp_reconfigure(const void *cfg) {
  struct wallpaper_config old = w.cfg;
  w.cfg = *(const struct wallpaper_config *)cfg;
  wp_viz_forget(&w.viz_in); /* bands may have changed */
  pick_map();
  if (strcmp(old.style, w.cfg.style))
    check_style();
  if (old.idle_timeout_s != w.cfg.idle_timeout_s) {
    setup_idle();
    update_animating();
  } else if (old.idle_bar_pulse != w.cfg.idle_bar_pulse ||
             old.battery_bar != w.cfg.battery_bar ||
             old.idle_saver != w.cfg.idle_saver) {
    update_animating();
  }
  if (old.battery_interval_s != w.cfg.battery_interval_s && w.animating) {
    int64_t ms = w.cfg.battery_interval_s * 1000LL;
    loop_timer_arm(w.battery_timer, ms, ms);
  }
  if (old.viz != w.cfg.viz || strcmp(old.viz_target, w.cfg.viz_target)) {
    wp_audio_stop();
    if (w.animating && w.cfg.viz)
      wp_audio_start(w.sw->loop, &w.cfg);
  }
  if (old.media != w.cfg.media || old.media_art != w.cfg.media_art ||
      old.media_interval_ms != w.cfg.media_interval_ms ||
      strcmp(old.media_player, w.cfg.media_player)) {
    wp_media_stop();
    wp_media_clear();
    if (w.animating && w.cfg.media)
      wp_media_start(w.sw->loop, &w.cfg);
  }
  if (strcmp(old.namespace, w.cfg.namespace) != 0)
    wp_surfaces_recreate();
  else
    wp_surfaces_draw(true);
}

void wp_finish(void) {
  wp_joined();
  bus_unsubscribe(on_bus, NULL);
  peek_set_sink(NULL);
  backdrop_set_source(NULL);
  w.peek = NULL;
  wp_surfaces_finish();
  wp_gl_finish();
  wp_render_finish();
  wp_audio_finish();
  wp_media_stop();
  wp_media_clear();
  memset(&w.viz, 0, sizeof(w.viz));
  if (w.idle_note)
    ext_idle_notification_v1_destroy(w.idle_note);
  w.idle_note = NULL;
  loop_remove_timer(w.battery_timer);
  loop_remove_timer(w.idle_timer);
  loop_remove_timer(w.hold_timer);
  w.battery_timer = w.idle_timer = w.hold_timer = NULL;
  w.show = WP_SHOW_NONE;
  w.animating = false;
  w.dozing = false;
  w.saving = false;
  w.saver_mix = w.saver_t = 0;
}

int wp_command(int argc, char **argv, struct reply *r) {
  if (argc == 0 || strcmp(argv[0], "status") == 0) {
    reply_printf(r, "state: %s\n",
                 w.animating ? (w.saver_mix > 0 ? "animating (back from the saver)"
                                                : "animating")
                 : w.saving  ? "saver (the snowflake wanders)"
                 : w.dozing  ? "dozing (only the bar pulses)"
                             : "frozen");
    reply_printf(r, "workspace: %s\n", w.ws_empty ? "empty" : "occupied");
    reply_printf(r, "idle: %s%s\n", w.idle ? "yes" : "no",
                 w.idle_note || w.cfg.idle_timeout_s == 0
                     ? ""
                     : " (no ext-idle-notify: pointer only)");
    if (w.battery >= 0)
      reply_printf(r, "battery: %d%%\n", w.battery);
    else
      reply_printf(r, "battery: none\n");
    reply_printf(r, "percent: %.0f%s (shown %.1f)\n", target_percent(),
                 w.override >= 0 ? " (override)" : "", shown_percent());
    reply_printf(r, "fps: %.1f\n", wp_ticking() ? w.fps : 0.0);
    reply_printf(r, "spin: x%.2f\n", 1 + w.boost);
    if (wp_topo(&w.cfg))
      reply_printf(r, "topo seed: %d, angle %.0f%s\n", w.cfg.topo_seed,
                   w.cfg.topo_angle,
                   w.cfg.topo_new_map ? " (new each start)" : "");
    reply_printf(r, "audio: %s\n", w.cfg.viz ? wp_audio_state() : "disabled");
    if (wp_topo(&w.cfg))
      reply_printf(r, "logo render: %s\n", wp_surfaces_logo_renderer());
    if (w.cfg.viz && w.cfg.viz_edges && w.cfg.viz_edge_smooth)
      reply_printf(r, "viz render: %s\n", wp_surfaces_viz_renderer());
    if (w.cfg.viz) {
      float top[2] = {0, 0};
      for (int s = 0; s < 2; ++s)
        for (int i = 0; i < w.cfg.viz_bars; ++i)
          top[s] = fmaxf(top[s], w.viz.bars[s][i]);
      reply_printf(r, "viz: left %.2f, right %.2f (peak %.0f dB)\n", top[0],
                   top[1], w.viz.peak);
      reply_printf(r,
                   "react: loud %.2f, pulse %.2f, shake %.2f, spin x%.2f "
                   "(%s%s%s%s%s)\n",
                   w.react.loud, w.react.pulse, w.react.shake,
                   1 + (w.cfg.react_spin ? w.cfg.react_spin_gain * w.react.energy +
                                               w.react.kick
                                         : 0),
                   w.cfg.react_slide ? " slide" : "",
                   w.cfg.react_pulse ? " pulse" : "",
                   w.cfg.react_glow ? " glow" : "",
                   w.cfg.react_spin ? " spin" : "",
                   w.cfg.react_shake ? " shake" : "");
    }
    if (w.cfg.media) {
      const struct wp_media *m = wp_media_get();
      if (!m->has) {
        reply_printf(r, "media: none\n");
      } else {
        double p = wp_media_progress(m, now_ms());
        char prog[32];
        if (p >= 0)
          snprintf(prog, sizeof prog, "%.0f%%", p * 100);
        else
          snprintf(prog, sizeof prog, "no length, no hexagon");
        reply_printf(r, "media: %s, \"%s\"%s%s, %s\n",
                     m->playing ? "playing" : "paused", m->title,
                     m->artist[0] ? " — " : "", m->artist, prog);
        unsigned id;
        reply_printf(r, "cover: %s\n",
                     wp_media_art(&id) ? "yes"
                     : m->art_url[0] && w.cfg.media_art ? "none (unreadable)"
                                                         : "none");
      }
    }
    if (wp_showing())
      reply_printf(r, "show: %s%s\n", wp_show_name(w.show),
                   show_holding() ? " (black)" : "");
    reply_printf(r, "outputs: %d\n", wp_surfaces_count());
    return 0;
  }
  /* Play the launch animation, or the power-off one; the reply is the ms
   * until it ends (for a power-off, in black), 0 if it doesn't play. */
  if ((strcmp(argv[0], "intro") == 0 || strcmp(argv[0], "outro") == 0) &&
      argc == 1) {
    bool intro = argv[0][0] == 'i';
    int64_t left = 0;
    if (outro_show() && !intro) {
      /* Already powering off: the same one, not a restart. */
      left = w.show_start + wp_show_ms(&w.cfg, w.show) - now_ms();
    } else if (w.ws_empty && (intro || w.cfg.outro)) {
      enum wp_show s = intro ? WP_SHOW_INTRO
                       : drand48() < w.cfg.outro_crt_chance ? WP_SHOW_CRT
                                                            : WP_SHOW_SCATTER;
      show_start(s);
      left = wp_show_ms(&w.cfg, s);
    }
    reply_printf(r, "%lld\n", (long long)(left > 0 ? left : 0));
    return 0;
  }
  if (strcmp(argv[0], "percent") == 0 && argc == 2) {
    if (strcmp(argv[1], "auto") == 0) {
      w.override = -1;
    } else {
      char *end;
      long v = strtol(argv[1], &end, 10);
      if (end == argv[1] || *end || v < 0 || v > 100) {
        reply_printf(r, "wallpaper: percent takes 0..100 or auto\n");
        return 1;
      }
      w.override = (int)v;
    }
    set_target(target_percent());
    return 0;
  }
  reply_printf(r, "wallpaper: unknown command '%s'\n", argv[0]);
  return 1;
}
