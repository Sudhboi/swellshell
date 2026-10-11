/* wallpaper: a live black wallpaper with a pulsing battery bar at the top
 * and a slowly turning Nix snowflake in the middle (formerly pre-rendered
 * GIFs played by awww), ringed by an audio visualiser while sound plays. It only animates while the focused workspace is
 * empty and the user is not idle; otherwise it keeps its last frame and
 * does no work at all. Shared declarations of the module's files. */
#ifndef SWELL_WALLPAPER_H
#define SWELL_WALLPAPER_H

#include <stdbool.h>
#include <stdint.h>

#include <cairo.h>

#include "wl.h"

/* Geometry is given in pixels of a 2880 px wide output (the laptop panel
 * the GIFs were made for) and scaled to each output's buffer width. */
#define WP_REF_WIDTH 2880.0

/* Most visualiser bars per side of the ring. */
#define WP_VIZ_MAX 128

struct wallpaper_config {
  char namespace[64]; /* layer-shell namespace */
  int fps;            /* cap at the resting spin; 0: the display rate */
  int fps_spin;       /* cap while the pointer speeds the logo up */
  int fps_audio;      /* cap while the visualiser shows */
  int idle_timeout_s; /* 0: never freeze for idleness */
  int battery_interval_s;
  /* colours */
  uint32_t background, bar, logo, logo_alt;
  uint32_t logo_glow, logo_glow_alt; /* the logo at the fastest spin */
  /* bar, reference px */
  double bar_top, bar_width, bar_height, bar_border, bar_gap;
  /* bar pulse: the shape at 50 %; the period scales with the percent */
  double bar_min, pulse_period_s, pulse_period_min_s, pulse_sigma_s,
      pulse_hold_s;
  int tween_ms; /* fill change */
  /* logo */
  double logo_radius; /* reference px */
  double spin_speed;  /* deg/s, clockwise */
  double spin_gain, spin_max, spin_decay_s;
  int spin_hold_ms; /* no decay until the pointer is still this long */
  /* past this speed (deg/s) the logo turns no faster; a bloom and speed
   * lines show the rest */
  double spin_visible_max;
  uint32_t bloom, streak;
  double bloom_radius, bloom_alpha; /* x logo_radius, at full speed */
  int streak_count;
  double streak_length, streak_width, streak_alpha; /* deg, px, at full */
  /* visualiser: a ring of bars around the logo, the left channel on the
   * left and the right on the right, bass at the bottom */
  bool viz;
  char viz_target[128]; /* PipeWire sink to listen to, "": the default */
  int viz_bars;         /* per side */
  double viz_radius;    /* the bars' centre line, x logo_radius */
  double viz_length;    /* reference px at full level, both ends together */
  double viz_inward;    /* the share of a bar that grows inwards, 0..1 */
  double viz_width;     /* of each bar's slot, 0..1 */
  uint32_t viz_color;
  double viz_alpha;
  double viz_low_hz, viz_high_hz;
  double viz_fall;   /* gravity, levels per s^2 */
  double viz_jitter; /* 0: a perfect ring .. 1: the most irregular */
  /* the logo reacting to the music (needs viz), each part on its own */
  bool react_slide;         /* lambdas slide out by their slice of bands */
  double react_slide_dist;  /* x logo_radius at full level */
  bool react_pulse;         /* the logo grows on bass hits */
  double react_pulse_scale; /* x logo_radius per hit */
  double react_pulse_min;   /* of it for the softest hit; full at 15 dB */
  int react_pulse_ms;       /* ease-back time constant */
  bool react_glow;          /* brightens with loudness */
  double react_glow_amount; /* of the way to logo_glow at full loudness */
  double react_glow_spin;   /* the same at the fastest music spin */
  double react_glow_base;   /* the least glow while music plays */
  bool react_spin;          /* loudness and beats speed the spin up */
  double react_spin_gain;   /* x spin_speed extra at full loudness */
  double react_spin_kick;   /* x spin_speed extra per beat, decaying */
  int react_spin_kick_ms;   /* the beat's extra speed decay time constant */
  bool react_shake;         /* everything shakes on the biggest bass hits */
  double react_shake_rise;  /* dB a hit must jump to shake */
  double react_shake_px;    /* reference px at the start of a shake */
  int react_shake_ms;       /* the shake's decay time constant */
};

/* One frame's worth of animation state. */
struct wp_frame {
  double percent; /* bar fill, 0..100 */
  double level;   /* bar brightness, bar_min..1 */
  double angle;   /* logo rotation, radians clockwise */
  double glow;    /* logo brightness, 0 (logo) .. 1 (logo_glow) */
  double over;    /* speed past spin_visible_max, 0..1 of the most */
  int dir;        /* spin direction, 1 clockwise, -1 counter-clockwise */
  double pulse;    /* logo growth, 0..1 of react_pulse_scale */
  double slide[6]; /* lambda k moved out by this x logo radius */
  double shake_x, shake_y; /* everything moved by this, reference px */
  int nbars;      /* visualiser bars per side, 0: no ring */
  float bars[2][WP_VIZ_MAX]; /* 0..1, bass first; [0] left, [1] right */
};

struct wp_rect {
  int x, y, w, h;
};

/* render.c */
/* Pulse period in seconds at percent. */
double wp_pulse_period(const struct wallpaper_config *c, double percent);
/* Bar brightness at phase 0..1 of a pulse (peak at 0.5). */
double wp_bar_level(const struct wallpaper_config *c, double phase);
/* The two buffer areas that change between frames (bar, logo) of a pw x ph
 * buffer. ring: the logo area also covers the visualiser ring. */
void wp_regions(const struct wallpaper_config *c, int pw, int ph, bool ring,
                struct wp_rect out[2]);
/* Draw a frame into a pw x ph buffer. full repaints everything, otherwise
 * only the wp_regions() areas (with ring) are touched. */
void wp_render(cairo_t *cr, const struct wallpaper_config *c, int pw, int ph,
               const struct wp_frame *f, bool full, bool ring);
/* Free wp_render()'s cache. */
void wp_render_finish(void);

/* viz.c: the visualiser's analysis, cava-style */
#define WP_VIZ_FFT 2048 /* samples per analysis */
/* Band levels in dB (viz_bars of them, bass first) of the last WP_VIZ_FFT
 * samples (oldest first) at rate Hz. */
void wp_viz_analyse(const float *samples, int rate,
                    const struct wallpaper_config *c, float *db);
struct wp_viz {
  float bars[2][WP_VIZ_MAX];  /* 0..1, [0] left, [1] right */
  float bass_db; /* power under KICK_HZ right now, dB (-200: silence) */
  float vel[2][WP_VIZ_MAX];  /* fall speed, levels/s */
  float peak; /* auto-sensitivity: the loudest recent band of both, dB */
};
/* Advance the shown bars by dt seconds towards the band levels db of the
 * left and right channel (NULL: silence). */
void wp_viz_step(struct wp_viz *v, const struct wallpaper_config *c,
                 const float (*db)[WP_VIZ_MAX], double dt);
/* What the logo reacts to, from the bars. */
struct wp_react {
  double loud;           /* smoothed mean bar level, 0..1 (for the glow) */
  double energy;         /* the same, following quickly (for the spin) */
  double bass;      /* wp_viz.bass_db, as last seen */
  double hist[16];  /* bass over the last KICK_WINDOW_S, newest last */
  double hist_t[16]; /* their ages, s */
  double bass_peak; /* the high since the last kick, dB */
  double pulse; /* on a bass hit, by its size (react_pulse_min..1), easing
                 * back to 0 */
  double kick;           /* extra spin from beats (x spin_speed), decaying */
  double since_kick;     /* s */
  double hit_rise;       /* the last kick's jump over the recent low, dB */
  double shake;          /* 1 on a big bass hit, easing back to 0 */
  double shake_t;        /* s since the shake started */
  bool held; /* a kick fired and the bass hasn't fallen from its peak */
  double slices[6];      /* per lambda, bass (0) to treble (5), 0..1 */
};
/* Follow the bars v by dt seconds. */
void wp_react_step(struct wp_react *r, const struct wp_viz *v,
                   const struct wallpaper_config *c, double dt);

/* A bar's fixed irregularity, -1..1: one stable value per (side, bar, what)
 * so the ring isn't a perfect circle. */
double wp_viz_hash(int side, int bar, int what);
/* A bar's slowly drifting gain at time t (s), around 1, by viz_jitter. */
double wp_viz_wobble(const struct wallpaper_config *c, int side, int bar,
                     double t);

/* audio.c: PipeWire capture of the sink, only while the module animates and
 * something plays */
struct loop;
void wp_audio_init(void);
void wp_audio_finish(void);
void wp_audio_start(struct loop *loop, const struct wallpaper_config *c);
void wp_audio_stop(void);
/* "off", "waiting" (connected, nothing plays) or "capturing". */
const char *wp_audio_state(void);
/* Copy the last WP_VIZ_FFT samples (oldest first) of the left and right
 * channel and give the rate. Returns false if no fresh audio arrived
 * lately. */
bool wp_audio_samples(float *left, float *right, int *rate);

/* surface.c: one background layer surface per output */
void wp_surfaces_init(struct swell_wl *wl);
void wp_surfaces_finish(void);
/* Destroy and recreate every surface (namespace change). */
void wp_surfaces_recreate(void);
/* Draw every output now. full: the geometry or colours changed. If the
 * module is animating, each draw also asks for the next frame. */
void wp_surfaces_draw(bool full);
int wp_surfaces_count(void);

/* wallpaper.c, for surface.c */
extern const struct swell_input wp_input;
const struct wallpaper_config *wp_config(void);
bool wp_animating(void);
/* The frame rate cap right now (0: none). */
int wp_fps_cap(void);
/* Advance the animation to now (monotonic ms) and give the frame. */
void wp_frame_at(int64_t now, struct wp_frame *f);
/* A frame was presented (for the fps count in `status`). */
void wp_count_frame(int64_t now);

#endif
