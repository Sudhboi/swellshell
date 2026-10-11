/* wallpaper: the battery bar + Nix snowflake wallpaper, rendered live,
 * with an audio visualiser ringing the snowflake while sound plays.
 *
 * It animates only while the focused workspace is empty (workspace.empty /
 * workspace.occupied on the bus) and the user is not idle
 * (ext-idle-notify-v1). Otherwise no frame callbacks are requested and no
 * timer is armed: the last frame stays on screen and the module costs
 * nothing; that includes the PipeWire capture, which is torn down. Moving
 * the pointer over the wallpaper spins the logo faster. */
#include "wallpaper.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "anim.h"
#include "battery.h"
#include "bus.h"
#include "cursor-shape-v1-client-protocol.h"
#include "ext-idle-notify-v1-client-protocol.h"
#include "loop.h"
#include "module.h"
#include "sway.h"
#include "util.h"

static const struct wallpaper_config defaults = {
    .namespace = "swell-wallpaper",
    .fps = 30,
    .fps_spin = 0,
    .fps_audio = 60,
    .idle_timeout_s = 300,
    .battery_interval_s = 30,
    .background = 0x000000ffu,
    .bar = 0xffffffffu,
    .logo = 0x181818ffu,
    .logo_alt = 0x111111ffu,
    .logo_glow = 0x5a5a5affu,
    .logo_glow_alt = 0x444444ffu,
    .bar_top = 10,
    .bar_width = 600,
    .bar_height = 10,
    .bar_border = 2,
    .bar_gap = 2,
    .bar_min = 25 / 255.0,
    .pulse_period_s = 4.0,
    .pulse_period_min_s = 0.5,
    .pulse_sigma_s = 0.4,
    .pulse_hold_s = 1.0,
    .tween_ms = 600,
    .logo_radius = 225,
    .spin_speed = 15,
    .spin_gain = 0.008,
    .spin_max = 100,
    .spin_decay_s = 1.2,
    .spin_hold_ms = 200,
    .spin_visible_max = 200,
    .bloom = 0x808080ffu,
    .bloom_radius = 1.6,
    .bloom_alpha = 0.25,
    .streak = 0xa0a0a0ffu,
    .streak_count = 9,
    .streak_length = 70,
    .streak_width = 5,
    .streak_alpha = 0.7,
    .viz = true,
    .viz_target = "",
    .viz_bars = 48,
    .viz_radius = 1.6,
    .viz_length = 260,
    .viz_inward = 0.4,
    .viz_width = 0.55,
    .viz_color = 0x5a5a5affu,
    .viz_alpha = 1.0,
    .viz_low_hz = 50,
    .viz_high_hz = 10000,
    .viz_fall = 4,
    .viz_jitter = 1,
    .react_slide = false,
    .react_slide_dist = 0.1,
    .react_pulse = true,
    .react_pulse_scale = 0.1,
    .react_pulse_ms = 150,
    .react_glow = true,
    .react_glow_amount = 0.6,
    .react_spin = true,
    .react_spin_gain = 6,
    .react_spin_kick = 4,
    .react_spin_kick_ms = 400,
};

#define S struct wallpaper_config
static const struct cfg_field fields[] = {
    CFG_STRING(S, namespace, "layer-shell namespace of the surfaces"),
    CFG_INT(S, fps, 0, 1000,
            "frame rate cap at the resting spin (0 = the display rate)"),
    CFG_INT(S, fps_spin, 0, 1000,
            "frame rate cap while the pointer spins the logo faster (0 = "
            "the display rate)"),
    CFG_INT(S, fps_audio, 0, 1000,
            "frame rate cap while the visualiser shows (0 = the display "
            "rate)"),
    CFG_INT(S, idle_timeout_s, 0, 86400,
            "freeze after this long without input (0 = never)"),
    CFG_INT(S, battery_interval_s, 1, 3600,
            "battery re-read period while animating"),
    CFG_COLOR(S, background, "background"),
    CFG_COLOR(S, bar, "battery bar at full pulse brightness"),
    CFG_COLOR(S, logo, "snowflake"),
    CFG_COLOR(S, logo_alt, "snowflake, every other lambda"),
    CFG_COLOR(S, logo_glow,
              "snowflake at the fastest spin (it brightens with the speed)"),
    CFG_COLOR(S, logo_glow_alt,
              "snowflake at the fastest spin, every other lambda"),
    CFG_DOUBLE(S, bar_top, 0, 10000,
               "bar top edge (px on a 2880 px wide output; all sizes scale "
               "with the output width)"),
    CFG_DOUBLE(S, bar_width, 1, 10000, "bar length, px"),
    CFG_DOUBLE(S, bar_height, 1, 1000, "bar thickness, px"),
    CFG_DOUBLE(S, bar_border, 0, 100, "outline width, px"),
    CFG_DOUBLE(S, bar_gap, 0, 100, "gap between the outline and the fill, px"),
    CFG_DOUBLE(S, bar_min, 0, 1, "bar brightness between pulses (0..1)"),
    CFG_DOUBLE(S, pulse_period_s, 0.01, 3600,
               "time between pulses at 50% (scales with the percent)"),
    CFG_DOUBLE(S, pulse_period_min_s, 0.01, 3600,
               "shortest time between pulses (near 0%)"),
    CFG_DOUBLE(S, pulse_sigma_s, 0.001, 3600,
               "pulse fade width at 50% (Gaussian sigma)"),
    CFG_DOUBLE(S, pulse_hold_s, 0, 3600,
               "time at full brightness per pulse at 50%"),
    CFG_INT(S, tween_ms, 0, 60000, "fill change animation"),
    CFG_DOUBLE(S, logo_radius, 0, 10000, "snowflake radius, px"),
    CFG_DOUBLE(S, spin_speed, -3600, 3600,
               "snowflake rotation, degrees/s clockwise"),
    CFG_DOUBLE(S, spin_gain, 0, 100,
               "extra speed (x spin_speed) per px of pointer travel"),
    CFG_DOUBLE(S, spin_max, 0, 1000, "most extra speed (x spin_speed)"),
    CFG_DOUBLE(S, spin_decay_s, 0.001, 3600,
               "extra speed decay time constant"),
    CFG_INT(S, spin_hold_ms, 0, 60000,
            "the extra speed only decays once the pointer has been still "
            "this long, so it keeps building while the pointer moves"),
    CFG_DOUBLE(S, spin_visible_max, 0, 3600,
               "fastest the snowflake visibly turns, degrees/s; more speed "
               "shows as a bloom and speed lines"),
    CFG_COLOR(S, bloom, "glow behind the snowflake past spin_visible_max"),
    CFG_DOUBLE(S, bloom_radius, 1, 3,
               "bloom radius at the fastest spin (x logo_radius)"),
    CFG_DOUBLE(S, bloom_alpha, 0, 1, "bloom opacity at the fastest spin"),
    CFG_COLOR(S, streak, "speed lines around the snowflake"),
    CFG_INT(S, streak_count, 0, 64, "speed lines (0 = none)"),
    CFG_DOUBLE(S, streak_length, 0, 360,
               "speed line length at the fastest spin, degrees"),
    CFG_DOUBLE(S, streak_width, 0, 100, "speed line thickness, px"),
    CFG_DOUBLE(S, streak_alpha, 0, 1,
               "speed line opacity at the fastest spin"),
    CFG_BOOL(S, viz,
             "audio visualiser around the snowflake (PipeWire; only while "
             "the wallpaper animates and something plays)"),
    CFG_STRING(S, viz_target,
               "PipeWire sink to visualise (node name or serial; \"\" = the "
               "default sink)"),
    CFG_INT(S, viz_bars, 1, WP_VIZ_MAX,
            "visualiser bars per side (left channel on the left, right on "
            "the right)"),
    CFG_DOUBLE(S, viz_radius, 0, 10,
               "the line the bars grow from, both ways (x logo_radius)"),
    CFG_DOUBLE(S, viz_length, 0, 10000,
               "bar length at full level, both ends together, px"),
    CFG_DOUBLE(S, viz_inward, 0, 1,
               "share of a bar's length that grows inwards (0 = all "
               "outwards from viz_radius)"),
    CFG_DOUBLE(S, viz_width, 0, 1, "bar thickness, as a fraction of its slot"),
    CFG_COLOR(S, viz_color, "visualiser bars"),
    CFG_DOUBLE(S, viz_alpha, 0, 1, "visualiser opacity"),
    CFG_DOUBLE(S, viz_low_hz, 1, 100000, "lowest frequency (the bottom bars)"),
    CFG_DOUBLE(S, viz_high_hz, 1, 100000, "highest frequency (the top bars)"),
    CFG_DOUBLE(S, viz_fall, 0.01, 1000,
               "how fast the bars fall (gravity, full heights per s^2)"),
    CFG_DOUBLE(S, viz_jitter, 0, 1,
               "irregularity: each bar's angle, width, length and gain vary "
               "a little (0 = a perfect ring)"),
    CFG_BOOL(S, react_slide,
             "the snowflake's lambdas slide outwards with the music, each by "
             "its own slice of the spectrum (needs viz)"),
    CFG_DOUBLE(S, react_slide_dist, 0, 1,
               "lambda slide at full level (x logo_radius)"),
    CFG_BOOL(S, react_pulse, "the snowflake grows on bass hits (needs viz)"),
    CFG_DOUBLE(S, react_pulse_scale, 0, 1,
               "growth per bass hit (x logo_radius)"),
    CFG_INT(S, react_pulse_ms, 1, 10000, "how fast a bass hit eases back"),
    CFG_BOOL(S, react_glow,
             "the snowflake brightens with loudness (needs viz)"),
    CFG_DOUBLE(S, react_glow_amount, 0, 1,
               "brightness at full loudness, of the way to logo_glow"),
    CFG_BOOL(S, react_spin,
             "loudness and beats speed the snowflake up (needs viz)"),
    CFG_DOUBLE(S, react_spin_gain, 0, 100,
               "extra speed at full loudness (x spin_speed)"),
    CFG_DOUBLE(S, react_spin_kick, 0, 100,
               "extra speed per beat (x spin_speed)"),
    CFG_INT(S, react_spin_kick_ms, 1, 10000,
            "how fast a beat's extra speed eases off"),
};
#undef S

static struct {
  struct swell *sw;
  struct wallpaper_config cfg;
  bool ws_empty, idle, animating;

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
  double viz_time; /* s, drives the bars' wobble */
  struct wp_react react;

  /* measured frame rate */
  int64_t fps_start;
  int fps_frames;
  double fps;
} w;

const struct wallpaper_config *wp_config(void) { return &w.cfg; }
bool wp_animating(void) { return w.animating; }

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
  if (w.react.loud > 1e-3 || w.react.pulse > 0 || w.react.kick > 0)
    return true;
  for (int k = 0; k < 6; ++k)
    if (w.react.slices[k] > 1e-3)
      return true;
  return false;
}

/* The resting spin is slow enough for a low rate; a pointer-boosted one
 * and moving bars need more frames to look smooth. */
int wp_fps_cap(void) {
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
  if (w.animating) {
    w.tween_from = shown_percent();
    w.tween_elapsed = 0;
  } else {
    w.tween_from = pct; /* nobody sees a tween while frozen */
  }
  w.tween_to = pct;
  if (!w.animating)
    wp_surfaces_draw(false);
}

/* Returns the time advanced, in seconds. */
static double advance(int64_t now) {
  /* Clamped, so a stall (or a freeze) doesn't make anything jump. */
  double dt = (now - w.last) / 1000.0;
  w.last = now;
  if (dt <= 0)
    return 0;
  if (dt > 0.1)
    dt = 0.1;
  double music =
      w.cfg.react_spin ? w.cfg.react_spin_gain * w.react.energy + w.react.kick
                       : 0;
  double omega = w.cfg.spin_speed * (1 + w.boost + music);
  omega = copysign(fmin(fabs(omega), w.cfg.spin_visible_max), omega) * M_PI /
          180;
  w.angle = fmod(w.angle + omega * dt, 2 * M_PI);
  if (now - w.last_motion >= w.cfg.spin_hold_ms)
    w.boost *= exp(-dt / w.cfg.spin_decay_s);
  if (w.boost < 1e-3)
    w.boost = 0;
  w.tween_elapsed += dt * 1000;
  w.phase = fmod(w.phase + dt / wp_pulse_period(&w.cfg, shown_percent()), 1);
  return dt;
}

/* Move the bars on by dt towards the latest audio (silence without any). */
static void advance_viz(double dt) {
  if (dt <= 0)
    return;
  static float samples[2][WP_VIZ_FFT];
  float db[2][WP_VIZ_MAX];
  int rate;
  bool fresh = w.cfg.viz && wp_audio_samples(samples[0], samples[1], &rate);
  if (fresh)
    for (int s = 0; s < 2; ++s)
      wp_viz_analyse(samples[s], rate, &w.cfg, db[s]);
  wp_viz_step(&w.viz, &w.cfg, fresh ? (const float (*)[WP_VIZ_MAX])db : NULL,
              dt);
  wp_react_step(&w.react, &w.viz, &w.cfg, dt);
  w.viz_time = fmod(w.viz_time + dt, 3600);
}

/* The logo brightens with the pointer boost, fully at spin_max. */
static double glow(void) {
  return w.cfg.spin_max > 0 ? fmin(w.boost / w.cfg.spin_max, 1) : 0;
}

/* Speed past spin_visible_max, as a fraction of the most there can be. */
static double over(void) {
  double cap = w.cfg.spin_visible_max;
  double v = fabs(w.cfg.spin_speed) * (1 + w.boost);
  double vmax = fabs(w.cfg.spin_speed) * (1 + w.cfg.spin_max);
  return vmax > cap ? fmin(fmax((v - cap) / (vmax - cap), 0), 1) : 0;
}

void wp_frame_at(int64_t now, struct wp_frame *f) {
  if (w.animating)
    advance_viz(advance(now));
  f->percent = shown_percent();
  f->level = wp_bar_level(&w.cfg, w.phase);
  f->angle = w.angle;
  f->glow = glow();
  if (w.cfg.react_glow)
    f->glow = fmax(f->glow, fmin(w.cfg.react_glow_amount * w.react.loud, 1));
  f->pulse = w.cfg.react_pulse ? w.react.pulse : 0;
  for (int k = 0; k < 6; ++k)
    f->slide[k] =
        w.cfg.react_slide ? w.cfg.react_slide_dist * w.react.slices[k] : 0;
  f->over = over();
  f->dir = w.cfg.spin_speed < 0 ? -1 : 1;
  f->nbars = viz_showing() ? w.cfg.viz_bars : 0;
  for (int s = 0; s < 2; ++s)
    for (int i = 0; i < f->nbars; ++i)
      f->bars[s][i] = (float)fmin(
          w.viz.bars[s][i] * wp_viz_wobble(&w.cfg, s, i, w.viz_time), 1);
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

/* ---- the animate/freeze state machine ---- */

static void arm_idle_fallback(void) {
  if (!w.idle_note && w.cfg.idle_timeout_s > 0 && w.animating)
    loop_timer_arm(w.idle_timer, w.cfg.idle_timeout_s * 1000LL, 0);
}

static void update_animating(void) {
  bool want = w.ws_empty && !w.idle;
  if (want == w.animating)
    return;
  w.animating = want;
  if (want) {
    w.last = now_ms();
    w.fps_start = w.last;
    w.fps_frames = 0;
    int64_t ms = w.cfg.battery_interval_s * 1000LL;
    loop_timer_arm(w.battery_timer, ms, ms);
    arm_idle_fallback();
    read_battery();
    if (w.cfg.viz)
      wp_audio_start(w.sw->loop, &w.cfg);
    wp_surfaces_draw(false); /* and every draw asks for the next frame */
    bus_emit("wallpaper.animate", NULL);
  } else {
    loop_timer_disarm(w.battery_timer);
    loop_timer_disarm(w.idle_timer);
    wp_audio_stop();
    bool glowing = w.boost > 0 || viz_showing() || reacting();
    w.boost = 0;
    memset(&w.viz, 0, sizeof(w.viz));
    memset(&w.react, 0, sizeof(w.react));
    w.fps = 0;
    /* A window opened (or the user idled) mid-spin or mid-song: don't
     * leave the brightened logo or the bars as the frozen frame. */
    if (glowing)
      wp_surfaces_draw(false);
    bus_emit("wallpaper.freeze", NULL);
  }
}

static void on_bus(const struct bus_event *ev, void *data) {
  if (strcmp(ev->name, "workspace.empty") == 0)
    w.ws_empty = true;
  else if (strcmp(ev->name, "workspace.occupied") == 0)
    w.ws_empty = false;
  else
    return;
  update_animating();
}

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

static bool init(struct swell *sw, const void *cfg) {
  if (!sw->wl->layer_shell) {
    warn("wallpaper: the compositor lacks wlr-layer-shell");
    return false;
  }
  memset(&w, 0, sizeof(w));
  w.sw = sw;
  w.cfg = *(const struct wallpaper_config *)cfg;
  w.override = -1;
  wp_audio_init();
  w.battery_timer =
      loop_add_timer(sw->loop, CLOCK_MONOTONIC, on_battery_timer, NULL);
  w.idle_timer = loop_add_timer(sw->loop, CLOCK_MONOTONIC, on_idle_timer, NULL);
  int pct;
  bool charging;
  w.battery = battery_read(&pct, &charging) ? (pct > 100 ? 100 : pct) : -1;
  w.tween_from = w.tween_to = target_percent();
  setup_idle();
  wp_surfaces_init(sw->wl); /* a frozen first frame on every output */

  bus_subscribe(on_bus, NULL);
  int empty = sway_focused_empty();
  if (empty < 0 && sw->sway) {
    sway_emit_focused_state(); /* the answer arrives through on_bus */
    empty = sway_focused_empty();
  }
  w.ws_empty = empty != 0; /* without sway: always animate */
  update_animating();
  return true;
}

static void reconfigure(const void *cfg) {
  struct wallpaper_config old = w.cfg;
  w.cfg = *(const struct wallpaper_config *)cfg;
  if (old.idle_timeout_s != w.cfg.idle_timeout_s) {
    setup_idle();
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
  if (strcmp(old.namespace, w.cfg.namespace) != 0)
    wp_surfaces_recreate();
  else
    wp_surfaces_draw(true);
}

static void finish(void) {
  bus_unsubscribe(on_bus, NULL);
  wp_surfaces_finish();
  wp_render_finish();
  wp_audio_finish();
  memset(&w.viz, 0, sizeof(w.viz));
  if (w.idle_note)
    ext_idle_notification_v1_destroy(w.idle_note);
  w.idle_note = NULL;
  loop_remove_timer(w.battery_timer);
  loop_remove_timer(w.idle_timer);
  w.battery_timer = w.idle_timer = NULL;
  w.animating = false;
}

static int command(int argc, char **argv, struct reply *r) {
  if (argc == 0 || strcmp(argv[0], "status") == 0) {
    reply_printf(r, "state: %s\n", w.animating ? "animating" : "frozen");
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
    reply_printf(r, "fps: %.1f\n", w.animating ? w.fps : 0.0);
    reply_printf(r, "spin: x%.2f\n", 1 + w.boost);
    reply_printf(r, "audio: %s\n", w.cfg.viz ? wp_audio_state() : "disabled");
    if (w.cfg.viz) {
      float top[2] = {0, 0};
      for (int s = 0; s < 2; ++s)
        for (int i = 0; i < w.cfg.viz_bars; ++i)
          top[s] = fmaxf(top[s], w.viz.bars[s][i]);
      reply_printf(r, "viz: left %.2f, right %.2f (peak %.0f dB)\n", top[0],
                   top[1], w.viz.peak);
      reply_printf(r,
                   "react: loud %.2f, pulse %.2f, spin x%.2f (%s%s%s%s)\n",
                   w.react.loud, w.react.pulse,
                   1 + (w.cfg.react_spin ? w.cfg.react_spin_gain * w.react.energy +
                                               w.react.kick
                                         : 0),
                   w.cfg.react_slide ? " slide" : "",
                   w.cfg.react_pulse ? " pulse" : "",
                   w.cfg.react_glow ? " glow" : "",
                   w.cfg.react_spin ? " spin" : "");
    }
    reply_printf(r, "outputs: %d\n", wp_surfaces_count());
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

const struct swell_module wallpaper_module = {
    .name = "wallpaper",
    .aliases = (const char *const[]){"wp", NULL},
    .usage = "  status                 animation state, battery, fps\n"
             "  percent N|auto         show N% instead of the battery\n",
    .needs_wayland = true,
    .config_size = sizeof(struct wallpaper_config),
    .defaults = &defaults,
    .fields = fields,
    .nfields = ARRAY_LEN(fields),
    .init = init,
    .reconfigure = reconfigure,
    .finish = finish,
    .command = command,
};
