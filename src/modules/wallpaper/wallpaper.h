/* wallpaper: a live black wallpaper with a pulsing battery bar at the top
 * and a slowly turning Nix snowflake in the middle (formerly pre-rendered
 * GIFs played by awww). It only animates while the focused workspace is
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

struct wallpaper_config {
  char namespace[64]; /* layer-shell namespace */
  int fps;            /* cap at the resting spin; 0: the display rate */
  int fps_spin;       /* cap while the pointer speeds the logo up */
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
};

/* One frame's worth of animation state. */
struct wp_frame {
  double percent; /* bar fill, 0..100 */
  double level;   /* bar brightness, bar_min..1 */
  double angle;   /* logo rotation, radians clockwise */
  double glow;    /* logo brightness, 0 (logo) .. 1 (logo_glow) */
  double over;    /* speed past spin_visible_max, 0..1 of the most */
  int dir;        /* spin direction, 1 clockwise, -1 counter-clockwise */
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
 * buffer. */
void wp_regions(const struct wallpaper_config *c, int pw, int ph,
                struct wp_rect out[2]);
/* Draw a frame into a pw x ph buffer. full repaints everything, otherwise
 * only the wp_regions() areas are touched. */
void wp_render(cairo_t *cr, const struct wallpaper_config *c, int pw, int ph,
               const struct wp_frame *f, bool full);
/* Free wp_render()'s cache. */
void wp_render_finish(void);

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
