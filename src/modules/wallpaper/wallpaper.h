/* wallpaper: a live black wallpaper with a pulsing battery bar at the top
 * and a slowly turning Nix snowflake in the middle (formerly pre-rendered
 * GIFs played by awww), ringed by an audio visualiser while sound plays. It only animates while the focused workspace is
 * empty and the user is not idle; idle on an empty workspace only the bar
 * pulses, and otherwise it keeps its last frame and does no work at all. Shared declarations of the module's files. */
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
  bool battery_bar;    /* the battery bar at the top */
  bool idle_bar_pulse; /* idle on an empty workspace: the bar still pulses */
  int battery_interval_s;
  /* colours */
  uint32_t background, bar, logo, logo_alt;
  uint32_t logo_glow, logo_glow_alt; /* the logo at the fastest spin */
  /* the look: "topo" (contour lines over a dark gradient, a glass
   * snowflake) or "classic" (black, the solid snowflake above) */
  char style[16];
  uint32_t topo_bg_left, topo_bg_right; /* the background, left to right */
  uint32_t topo_line_left, topo_line_mid, topo_line_right; /* the lines */
  int topo_seed;          /* which map */
  int topo_levels;        /* contour lines from the lowest to the highest */
  int topo_index_every;   /* every this many'th line is wider, 0 = none */
  double topo_scale;      /* the hills' size, reference px */
  double topo_warp;       /* how much the hills meander, 0..2 */
  double topo_line_width; /* reference px */
  double topo_index_width; /* x topo_line_width */
  double topo_line_alpha;
  uint32_t topo_logo_fill, topo_logo_glow; /* the glass, at rest and lit */
  /* the outline, from the top left to the bottom right */
  uint32_t topo_edge_a, topo_edge_mid, topo_edge_b;
  double topo_edge_width;   /* reference px */
  double topo_shadow_px;    /* the shadow's offset down and right, ref px */
  double topo_shadow_alpha; /* 0..1 */
  int topo_fade_ms; /* the map fading out and in as windows come and go */
  /* the lines move with the music (GPU only): bass kicks send rings out
   * from the logo, loudness sways them */
  bool topo_waves;
  double topo_ring_px;     /* how far a ring moves the lines, ref px */
  double topo_ring_width;  /* ref px */
  double topo_ring_speed;  /* ref px/s */
  int topo_ring_ms;        /* each ring's life */
  double topo_sway_px;     /* at full loudness, ref px */
  uint32_t topo_text, topo_text_dim; /* title, artist over the map */
  double topo_text_backdrop; /* the dark glow behind them, 0..1 */
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
  /* along the screen's left and right edges, growing inwards (otherwise
   * the ring around the logo) */
  bool viz_edges;
  double viz_edge_length; /* reference px at full level */
  double viz_edge_gap;    /* from the screen's side, reference px */
  double viz_edge_margin; /* kept free at the top and bottom, reference px */
  bool viz_edge_smooth;   /* one smooth filled curve per side, not bars */
  double viz_edge_blur;   /* the curve's spread over neighbours, in bars */
  double viz_edge_fade;   /* the fill's opacity at full length, 0..1 */
  uint32_t viz_edge_rim;  /* the line along the curve */
  double viz_edge_rim_width; /* reference px, 0 = none */
  bool viz_gpu; /* draw the smooth edges with the GPU (if EGL works) */
  double viz_radius;    /* the bars' centre line, x logo_radius */
  double viz_length;    /* reference px at full level, both ends together */
  double viz_inward;    /* the share of a bar that grows inwards, 0..1 */
  double viz_width;     /* of each bar's slot, 0..1 */
  uint32_t viz_color;
  double viz_alpha;
  double viz_low_hz, viz_high_hz;
  double viz_fall;   /* gravity, levels per s^2 */
  double viz_rise_ms; /* time constant of a rise, 0 = instant */
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
  /* what's playing (playerctl): the title and artist under the logo, the
   * track's progress lighting its lambdas */
  bool media;
  char media_player[128]; /* playerctl -p, "": playerctl's pick */
  int media_interval_ms;  /* playerctl poll while animating */
  bool media_progress;    /* a line round the hexagon shows the progress */
  int media_chase_ms;     /* no track length: a line runs round, a sixth
                           * of the way in this long */
  bool media_art;         /* the cover, in grey, fills the hexagon */
  double media_art_alpha; /* the cover's opacity, 0..1 */
  uint32_t media_line, media_track; /* the progress line, the rest */
  double media_line_width;          /* reference px */
  double media_hex_size;  /* of the snowflake's hole, 0..1 */
  bool media_title;       /* the title and artist under the logo */
  char media_font[128];
  double media_title_size, media_artist_size; /* reference px */
  uint32_t media_text, media_dim;             /* title, artist */
  double media_gap;       /* below the biggest pulsed logo, reference px */
  double media_max_width; /* reference px, ellipsised past it */
  int media_fade_ms;
  double media_paused_alpha;
  /* the launch animation: the lambdas fly in and lock together, once per
   * login session, when the daemon starts on an empty workspace */
  bool intro;
  int intro_ms;         /* each lambda's flight */
  int intro_stagger_ms; /* between one lambda's start and the next's */
  double intro_dist;    /* where they start, x logo_radius out */
  double intro_turn;    /* the logo unwinds from this many extra deg */
  /* the power-off animation (`swell power off`): the lambdas scatter, or
   * now and then the logo collapses like an old CRT */
  bool outro;
  int outro_ms;            /* the scatter */
  double outro_crt_chance; /* 0..1, of the CRT collapse instead */
  int outro_crt_ms;
  int outro_hold_ms; /* black this long, then the wallpaper comes back */
  uint32_t crt_color;
};

#define WP_RINGS 8
#define WP_RINGS_STR "8"

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
  double media_alpha; /* the details (title, artist, hexagon), 0: none */
  const char *title, *artist;
  double progress; /* 0..1 of the way round from the top, -1: none */
  double chase;    /* -1, or where the running line is, 0..6 */
  cairo_surface_t *art; /* the cover in grey (RGB24, square), or NULL */
  unsigned art_id;      /* changes with the cover */
  /* the launch and power-off animations (all 0 otherwise) */
  double lambda_fade[6]; /* lambda k's opacity taken away, 0..1 */
  double logo_shrink;    /* the logo's size taken away, 0..1 */
  double bar_fade;       /* the bar's opacity taken away, 0..1 */
  double flash;          /* a bloom without speed lines, 0..1 */
  double crt_w, crt_h, crt_alpha; /* the CRT's line or dot, reference px */
  double fade;           /* black over everything, 0..1 */
  double bg_fade;        /* black over the topo map only, 0..1 */
  double map_fade;       /* the topo map taken away (windows), 0..1 */
  bool map_under; /* the GPU draws the map under the buffer: leave it clear */
  double sway;    /* the map's sway, 0..1 (loudness) */
  double time;    /* s, drives the sway */
  int nrings;
  struct wp_ring {
    double radius; /* ref px from the logo's centre */
    double size;   /* 0..1, already faded with age */
  } rings[WP_RINGS];
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
/* The buffer areas that change between frames of a pw x ph buffer: the bar,
 * the logo, the left and right edge strips (empty unless ring and
 * viz_edges) and the title and artist (empty unless text). ring: they also
 * cover the visualiser, the ring in the logo area or the edge strips. */
#define WP_REGIONS 5
void wp_regions(const struct wallpaper_config *c, int pw, int ph, bool ring,
                bool text, struct wp_rect out[WP_REGIONS]);
/* Draw a frame into a pw x ph buffer. full repaints everything, otherwise
 * only the wp_regions() areas (with ring and text) are touched. */
void wp_render(cairo_t *cr, const struct wallpaper_config *c, int pw, int ph,
               const struct wp_frame *f, bool full, bool ring, bool text);
/* The smooth edge curve of side s (0 left, 1 right), as drawn by
 * wp_render(): m points from the bottom up at levels pl (px in from the
 * screen's side) and heights py, joined by Beziers whose control points
 * (levels c1, c2 at heights cy1, cy2) lead from point i to i + 1. The strip
 * spans x0..x1 (len wide), y0..y1 past the screen by the shake distance. */
struct wp_edge_curve {
  double x0, y0, x1, y1, len;
  int m;
  double py[WP_VIZ_MAX + 2], pl[WP_VIZ_MAX + 2];
  double c1[WP_VIZ_MAX + 1], cy1[WP_VIZ_MAX + 1];
  double c2[WP_VIZ_MAX + 1], cy2[WP_VIZ_MAX + 1];
};
void wp_edge_curve(const struct wallpaper_config *c, int pw, int ph,
                   const struct wp_frame *f, int s, struct wp_edge_curve *e);
/* topo.c: the topo style's background */
/* The style isn't "classic". */
bool wp_topo(const struct wallpaper_config *c);
/* The contour map for a pw x ph buffer (RGB24), cached per size and config;
 * owned by topo.c. */
cairo_surface_t *wp_topo_background(const struct wallpaper_config *c, int pw,
                                    int ph);
/* The landscape the map's lines are drawn from: nx x ny heights (0..1,
 * row by row) cell buffer px apart from (origin, origin), past the
 * buffer's edges all round; id changes with it. */
struct wp_topo_grid {
  int nx, ny;
  double cell, origin;
  float *h;
  unsigned id;
};
const struct wp_topo_grid *wp_topo_heights(const struct wallpaper_config *c,
                                           int pw, int ph);
void wp_topo_finish(void);
/* How bright the map shows in frame f: windows and shows dim it, 0..1. */
double wp_map_brightness(const struct wp_frame *f);
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
  unsigned kicks;   /* counts the kicks */
  double kick_size; /* the last one's, react_pulse_min..1 */
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

/* media.c: what's playing, polled with playerctl only while animating */
struct wp_media {
  bool has;     /* a player has a track */
  bool playing; /* else paused */
  int64_t len_us, pos_us; /* length 0: unknown */
  int64_t at_ms;          /* when pos_us was read (monotonic) */
  char artist[256], title[256];
  char art_url[1024];
};
/* Parse a playerctl line "status\tlength\tposition\tart url\tartist\t
 * title" read at now. Returns m->has (false, and m zeroed, without a
 * track). */
bool wp_media_parse(const char *line, int64_t now, struct wp_media *m);
/* The track's progress at now, 0..1, or -1 if its length is unknown. */
double wp_media_progress(const struct wp_media *m, int64_t now);
void wp_media_start(struct loop *loop, const struct wallpaper_config *c);
void wp_media_stop(void);
/* Forget the track (media turned off). */
void wp_media_clear(void);
const struct wp_media *wp_media_get(void);
/* The current track's cover in grey (RGB24, a centred square), or NULL;
 * id changes whenever it does. */
cairo_surface_t *wp_media_art(unsigned *id);

/* gl.c: the smooth edge visualiser on the GPU */
struct wl_display;
struct wl_surface;
/* Set up EGL + GLES 3 once (display NULL: surfaceless, for tests). Later
 * calls give the first one's result. */
bool wp_gl_init(struct wl_display *display);
void wp_gl_finish(void);
/* Make the context current without a surface (tests). */
bool wp_gl_current(void);
struct wp_gl_win;
struct wp_gl_win *wp_gl_win_create(struct wl_surface *surface, int bw, int bh);
void wp_gl_win_resize(struct wp_gl_win *w, int bw, int bh);
void wp_gl_win_destroy(struct wp_gl_win *w);
/* Draw side s of the smooth edge visualiser of a pw x ph cairo buffer into
 * the bound bw x bh framebuffer, premultiplied, transparent around it. Its
 * top-left pixel's corner sits at (ox, oy) of the cairo buffer, and each
 * pixel spans sx x sy of it. */
void wp_gl_edge_draw(const struct wallpaper_config *c, int pw, int ph,
                     const struct wp_frame *f, int s, double ox, double oy,
                     double sx, double sy, int bw, int bh);
/* The same into w, then swap (which commits w's surface). */
bool wp_gl_edge(struct wp_gl_win *w, const struct wallpaper_config *c, int pw,
                int ph, const struct wp_frame *f, int s, double ox, double oy,
                double sx, double sy, int bw, int bh);

/* Draw the topo map of a pw x ph cairo buffer into the bound bw x bh
 * framebuffer, opaque, placed as wp_gl_edge_draw()'s strips. */
void wp_gl_map_draw(const struct wallpaper_config *c, int pw, int ph,
                    const struct wp_frame *f, double ox, double oy, double sx,
                    double sy, int bw, int bh);
/* The same into w, then swap. */
bool wp_gl_map(struct wp_gl_win *w, const struct wallpaper_config *c, int pw,
               int ph, const struct wp_frame *f, double ox, double oy,
               double sx, double sy, int bw, int bh);

/* surface.c: one background layer surface per output */
void wp_surfaces_init(struct swell_wl *wl);
void wp_surfaces_finish(void);
/* Destroy and recreate every surface (namespace change). */
void wp_surfaces_recreate(void);
/* "gpu", "cpu" or "cpu (no GPU)": how the smooth edges are drawn. */
const char *wp_surfaces_viz_renderer(void);
/* Draw every output now. full: the geometry or colours changed. If the
 * module is animating, each draw also asks for the next frame. */
void wp_surfaces_draw(bool full);
int wp_surfaces_count(void);

/* show.c: the launch and power-off timelines */
enum wp_show {
  WP_SHOW_NONE,
  WP_SHOW_INTRO,   /* the lambdas assemble */
  WP_SHOW_SCATTER, /* the lambdas scatter */
  WP_SHOW_CRT,     /* spin up and collapse like an old CRT */
};
const char *wp_show_name(enum wp_show s);
/* The show's length in ms. The power-off ones end in black. */
int wp_show_ms(const struct wallpaper_config *c, enum wp_show s);
/* Change f, a normal frame, into the show's frame t ms in. Past its length
 * an intro is the normal frame and a power-off black. */
void wp_show_apply(const struct wallpaper_config *c, enum wp_show s,
                   double t, struct wp_frame *f);

/* wallpaper.c, for surface.c */
extern const struct swell_input wp_input;
const struct wallpaper_config *wp_config(void);
bool wp_animating(void);
/* Idle on an empty workspace: the bar pulses, the rest is frozen. */
bool wp_dozing(void);
/* Animating or dozing or playing a show: frames are wanted. */
bool wp_ticking(void);
/* A show (or its black after power-off) is on screen: frames are drawn
 * whole. */
bool wp_showing(void);
/* The frame rate cap right now (0: none). */
int wp_fps_cap(void);
/* Advance the animation to now (monotonic ms) and give the frame. */
void wp_frame_at(int64_t now, struct wp_frame *f);
/* A frame was presented (for the fps count in `status`). */
void wp_count_frame(int64_t now);

#endif
