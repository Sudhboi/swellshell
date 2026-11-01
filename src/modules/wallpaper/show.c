/* The launch and power-off animations, as pure functions of time that turn
 * a normal frame into the show's. */
#include <math.h>
#include <string.h>

#include "anim.h"
#include "wallpaper.h"

static double clamp01(double x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

static double ease_in_cubic(double t) {
  t = clamp01(t);
  return t * t * t;
}

static double smoothstep(double t) {
  t = clamp01(t);
  return t * t * (3 - 2 * t);
}

/* The intro: when the last lambda locks in, then the flash and the bar. */
static double intro_lock_ms(const struct wallpaper_config *c) {
  return fmax(c->intro_ms, 0) + 5.0 * fmax(c->intro_stagger_ms, 0);
}
#define INTRO_SETTLE_MS 800

/* The CRT dot and line, reference px. */
#define CRT_DOT 18.0
#define CRT_LINE 5.0
#define CRT_END_DOT 8.0

static const char *const names[WP_SHOW_COUNT] = {
    [WP_SHOW_NONE] = "none",       [WP_SHOW_INTRO] = "intro",
    [WP_SHOW_SCATTER] = "scatter", [WP_SHOW_CRT] = "crt",
    [WP_SHOW_SHOCK] = "shock",     [WP_SHOW_RISE] = "rise",
    [WP_SHOW_DRAIN] = "drain",     [WP_SHOW_SINK] = "sink",
    [WP_SHOW_BLAST] = "blast",
};

/* topo's, each as likely as the others */
static const enum wp_show topo_intros[] = {WP_SHOW_SHOCK, WP_SHOW_RISE};
static const enum wp_show topo_outros[] = {WP_SHOW_DRAIN, WP_SHOW_SINK,
                                           WP_SHOW_BLAST};
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

const char *wp_show_name(enum wp_show s) {
  return s > WP_SHOW_NONE && s < WP_SHOW_COUNT ? names[s] : "none";
}

enum wp_show wp_show_named(const char *name) {
  for (int s = WP_SHOW_NONE + 1; s < WP_SHOW_COUNT; ++s)
    if (strcmp(names[s], name) == 0)
      return s;
  return WP_SHOW_NONE;
}

bool wp_show_is_intro(enum wp_show s) {
  return s == WP_SHOW_INTRO || s == WP_SHOW_SHOCK || s == WP_SHOW_RISE;
}

enum wp_show wp_show_pick(const struct wallpaper_config *c, bool intro,
                          double r) {
  if (wp_topo(c)) {
    const enum wp_show *l = intro ? topo_intros : topo_outros;
    int n = intro ? COUNT(topo_intros) : COUNT(topo_outros);
    int i = (int)(clamp01(r) * n);
    return l[i < n ? i : n - 1];
  }
  if (intro)
    return WP_SHOW_INTRO;
  return r < c->outro_crt_chance ? WP_SHOW_CRT : WP_SHOW_SCATTER;
}

int wp_show_ms(const struct wallpaper_config *c, enum wp_show s) {
  if (s == WP_SHOW_NONE || s >= WP_SHOW_COUNT)
    return 0;
  if (wp_show_is_intro(s))
    return (int)intro_lock_ms(c) + INTRO_SETTLE_MS;
  if (s == WP_SHOW_CRT)
    return c->outro_crt_ms > 0 ? c->outro_crt_ms : 1;
  return c->outro_ms > 0 ? c->outro_ms : 1;
}

/* The lambdas fly in from intro_dist out, one after another, fading in,
 * while the logo unwinds from intro_turn; once all are in, a flash and a
 * pulse, and the bar fades in and fills. */
static void intro(const struct wallpaper_config *c, double t,
                  struct wp_frame *f) {
  double lock = intro_lock_ms(c);
  for (int k = 0; k < 6; ++k) {
    double u = c->intro_ms > 0
                   ? clamp01((t - k * c->intro_stagger_ms) / c->intro_ms)
                   : 1;
    f->slide[k] += c->intro_dist * (1 - ease_out_cubic(u));
    f->lambda_fade[k] = 1 - clamp01(u / 0.3);
  }
  f->angle -= f->dir * c->intro_turn * M_PI / 180 *
              (1 - ease_out_cubic(lock > 0 ? t / lock : 1));
  double after = t - lock;
  double flash = 1 - clamp01(after / 600);
  double pulse = 1 - clamp01(after / 400);
  if (after >= 0) {
    f->flash = fmax(f->flash, 0.5 * flash * flash);
    f->glow = fmax(f->glow, flash * flash);
    f->pulse = fmax(f->pulse, pulse * pulse);
  }
  f->bg_fade = fmax(f->bg_fade, 1 - smoothstep(lock > 0 ? t / lock : 1));
  f->bar_fade = 1 - clamp01(after / 300);
  f->percent *= ease_out_cubic(after / 700);
  f->media_alpha *= clamp01(after / 300);
}

/* The logo spins up while its lambdas slide away one after another and
 * fade, and the bar drains; then black. */
static void scatter(const struct wallpaper_config *c, double t,
                    struct wp_frame *f) {
  double p = clamp01(t / wp_show_ms(c, WP_SHOW_SCATTER));
  f->nbars = 0;
  f->media_alpha *= 1 - clamp01(t / 300);
  f->angle += f->dir * 1.5 * M_PI * p * p;
  f->glow = fmax(f->glow, clamp01(p * 2.5));
  for (int k = 0; k < 6; ++k) {
    double u = clamp01((p - 0.1 - 0.08 * k) / 0.45);
    f->slide[k] += c->intro_dist * ease_in_cubic(u);
    f->lambda_fade[k] = fmax(f->lambda_fade[k], smoothstep((u - 0.2) / 0.8));
  }
  f->percent *= 1 - smoothstep(p / 0.75);
  f->bar_fade = fmax(f->bar_fade, clamp01((p - 0.55) / 0.35));
  f->bg_fade = fmax(f->bg_fade, smoothstep((p - 0.3) / 0.6));
  f->fade = fmax(f->fade, clamp01((p - 0.9) / 0.1));
}

/* The logo spins into overspeed and shrinks to a bright dot, which
 * stretches into a line across the screen, thins and narrows back to a
 * dot and goes out. */
static void crt(const struct wallpaper_config *c, double t,
                struct wp_frame *f) {
  double len = wp_show_ms(c, WP_SHOW_CRT);
  double p = clamp01(t / len);
  f->nbars = 0;
  f->media_alpha *= 1 - clamp01(t / 300);
  f->bar_fade = fmax(f->bar_fade, clamp01(p / 0.3));
  /* Speed up to the visible cap over the first 45 %, then hold it. */
  double vmax = fabs(c->spin_visible_max) * M_PI / 180;
  double ramp = 0.45 * len / 1000, up = clamp01(p / 0.45);
  double turn = vmax * ramp * up * up / 2 +
                vmax * fmax(t / 1000 - ramp, 0);
  f->angle += f->dir * turn;
  double shrink = ease_in_cubic((p - 0.35) / 0.25);
  f->logo_shrink = fmax(f->logo_shrink, shrink);
  f->bg_fade = fmax(f->bg_fade, shrink);
  f->over = fmax(f->over, up * up * (1 - shrink));
  f->glow = fmax(f->glow, up);
  /* The line stretches out, then collapses to a dot that fades. */
  double l1 = ease_out_cubic((p - 0.58) / 0.17);
  double l2 = ease_in_cubic((p - 0.8) / 0.14);
  double l3 = clamp01((p - 0.94) / 0.06);
  double w = CRT_DOT + (WP_REF_WIDTH - CRT_DOT) * l1;
  double h = CRT_DOT + (CRT_LINE - CRT_DOT) * l1;
  w += (CRT_END_DOT - w) * l2;
  h += (CRT_END_DOT - h) * l2;
  f->crt_w = w;
  f->crt_h = h;
  f->crt_alpha = smoothstep((shrink - 0.6) / 0.4) * (1 - l3);
}

/* ---- topo's: the contour lines carry the show ---- */

/* Reference px from the centre to past the corners (4:3 at the most). */
#define REACH 1900.0
/* topo's intros are written for this length and stretched to the intro's. */
#define TOPO_INTRO_MS 2350.0

/* The map's show fields on, at rest; the music's rings and spectrum give
 * way to the show's. */
static struct wp_show_map *map_on(struct wp_frame *f) {
  f->smap = (struct wp_show_map){
      .on = true,
      .in_r = 1e6,
      .in_band = 1,
      .out_r = -1e6,
      .out_band = 1,
      .lit_w = 1,
      .levels = 1e4,
      .gather_reach = 1,
      .ring_w = 1,
  };
  f->nrings = 0;
  f->spec_on = false;
  f->nbars = 0;
  return &f->smap;
}

/* The bar, the percent and the details come in from ms after t. */
static void details_in(double t, double after, struct wp_frame *f) {
  f->bar_fade = 1 - clamp01((t - after) / 300);
  f->percent *= ease_out_cubic((t - after) / 700);
  f->media_alpha *= clamp01((t - after) / 300);
}

/* The CRT's pill as a glowing dot of radius r, reference px. */
static void dot(struct wp_frame *f, double r, double a) {
  f->crt_w = f->crt_h = 2 * r;
  f->crt_alpha = a;
}

/* The snowflake (there from the start, as the boot splash left it)
 * charges and fires one wave: the lines appear behind its lit front,
 * pushed outwards as it passes. */
static void shock(const struct wallpaper_config *c, double t,
                  struct wp_frame *f) {
  struct wp_show_map *m = map_on(f);
  double r0 = c->logo_radius;
  double neon =
      t < 650 ? smoothstep(t / 650) : 1 - clamp01((t - 650) / 1100);
  double flash = t < 700 ? 0 : 0.6 * pow(1 - clamp01((t - 700) / 600), 2);
  f->flash = fmax(f->flash, fmax(neon, flash));
  f->glow = fmax(f->glow, flash);
  double w = clamp01((t - 650) / 1200);
  double r = t < 650 ? 0 : 0.8 * r0 + REACH * ease_out_cubic(w);
  m->in_r = r;
  m->in_band = 220;
  if (w > 0 && w < 1) {
    f->nrings = 1;
    f->rings[0].radius = r - 60;
    f->rings[0].size = 1.2 * (1 - w);
    m->ring_px = 70;
    m->ring_w = 200;
  }
  m->lit_r = r - 80;
  m->lit_w = 150;
  m->lit = 1.8 * (1 - 0.6 * w) * (1 - clamp01((t - 1850) / 450));
  details_in(t, 1700, f);
}

/* The contours draw in a level at a time, lowest first, the newest lit,
 * round the snowflake (there from the start), which flashes and sends out
 * a soft ring. */
static void rise(const struct wallpaper_config *c, double t,
                 struct wp_frame *f) {
  struct wp_show_map *m = map_on(f);
  double n = fmax(c->topo_levels, 1);
  /* Eased in: the high levels have fewer lines, so they come faster. */
  m->levels = 0.2 + (n + 0.8) * pow(clamp01((t - 80) / 1450), 1.6);
  m->level_lit = 2 * (1 - clamp01((t - 1500) / 500));
  m->cpu_dim = 1 - clamp01((m->levels - 0.2) / n); /* black at the start */
  /* (rising to the flash, not jumping) */
  double lock = t > 1800 ? 1 - clamp01((t - 1800) / 700)
                         : smoothstep((t - 1500) / 300);
  f->flash = fmax(f->flash, lock);
  f->glow = fmax(f->glow, 0.5 * lock * lock);
  double a = (t - 1800) / 1000;
  if (a > 0 && a < 0.55) {
    f->nrings = 1;
    f->rings[0].radius = c->logo_radius + a * 1600;
    f->rings[0].size = 0.9 * pow(1 - a / 0.55, 2);
    m->ring_px = 40;
    m->ring_w = 160;
  }
  details_in(t, 1800, f);
}

/* A power-off's common part at p (0..1 of its length): the details go,
 * the bar drains and fades, and it's black by the end whatever the rest
 * does. */
static void details_out(double p, struct wp_frame *f, double len) {
  f->media_alpha *= 1 - clamp01(p * len / 300);
  f->bar_fade = fmax(f->bar_fade, clamp01(p / 0.3));
  f->percent *= 1 - smoothstep(p / 0.75);
  f->fade = fmax(f->fade, clamp01((p - 0.92) / 0.08));
}

/* The lines are pulled into the snowflake, lit as they go, while the dark
 * closes in from the corners; it spins up, swallows them, flashes and
 * shrinks to a dot that goes out. */
static void drain(const struct wallpaper_config *c, double p,
                  struct wp_frame *f) {
  struct wp_show_map *m = map_on(f);
  double r0 = c->logo_radius;
  m->gather = 1800 * ease_in_cubic(p / 0.8);
  m->gather_reach = 700 + 2200 * clamp01(p / 0.8);
  double r = REACH + 250 - (REACH + 250 - 0.6 * r0) * smoothstep(p / 0.82);
  m->in_r = r;
  m->in_band = 280;
  m->lit_r = r - 120;
  m->lit_w = 160;
  m->lit = 1.6 * smoothstep(p / 0.15);
  m->lit_all = 0.5 * smoothstep(p / 0.6);
  f->bg_fade = fmax(f->bg_fade, smoothstep((p - 0.85) / 0.15));
  f->angle += f->dir * 1.6 * M_PI * p * p;
  double flash = 0.8 * exp(-pow((p - 0.8) / 0.06, 2));
  f->flash = fmax(f->flash, fmax(clamp01(p * 1.6), flash));
  f->glow = fmax(f->glow, flash);
  f->logo_shrink = fmax(f->logo_shrink, ease_in_cubic((p - 0.78) / 0.15));
  dot(f, 10, clamp01((p - 0.85) / 0.06) * (1 - clamp01((p - 0.93) / 0.07)));
}

/* The contours go from the highest level down, the top one lit as it
 * goes; then the lambdas scatter on a last wave that pushes the low lines
 * left off the screen. */
static void sink(const struct wallpaper_config *c, double p,
                 struct wp_frame *f) {
  struct wp_show_map *m = map_on(f);
  double n = fmax(c->topo_levels, 1);
  m->levels = n + 0.5 - (n - 2) * smoothstep(p / 0.55);
  m->level_lit = 1.8;
  m->cpu_dim = 1 - clamp01(m->levels / n);
  double w = clamp01((p - 0.5) / 0.4);
  if (p > 0.5) {
    double r = c->logo_radius + (REACH + 300) * ease_out_cubic(w);
    f->nrings = 1;
    f->rings[0].radius = r;
    f->rings[0].size = 1.2 * (1 - w);
    m->ring_px = 90;
    m->ring_w = 220;
    m->out_r = r - 100;
    m->out_band = 200;
    m->lit_r = r;
    m->lit_w = 150;
    m->lit = 1.5 * (1 - w);
  }
  for (int k = 0; k < 6; ++k) {
    double u = clamp01((p - 0.42 - 0.04 * k) / 0.35);
    f->slide[k] += c->intro_dist * ease_in_cubic(u);
    f->lambda_fade[k] = fmax(f->lambda_fade[k], smoothstep((u - 0.2) / 0.8));
  }
  f->angle += f->dir * 1.2 * M_PI * p * p;
  f->flash = fmax(f->flash, clamp01((p - 0.3) * 3));
}

/* The snowflake charges, drawing the lines in a little, then fires a
 * strong ring that blows them off the screen behind it; it fades to a
 * dot. */
static void blast(const struct wallpaper_config *c, double p,
                  struct wp_frame *f) {
  struct wp_show_map *m = map_on(f);
  double r0 = c->logo_radius, charge = clamp01(p / 0.28);
  f->flash = fmax(f->flash, charge);
  f->angle += f->dir * 2 * M_PI * p * p;
  m->gather_reach = 700;
  if (p < 0.28) {
    m->gather = 80 * smoothstep(charge);
    f->logo_shrink = fmax(f->logo_shrink, 0.12 * smoothstep(charge));
    return;
  }
  m->gather = 80 * (1 - ease_out_cubic((p - 0.28) / 0.1));
  double w = clamp01((p - 0.28) / 0.55);
  double r = 0.7 * r0 + (REACH + 450) * ease_out_cubic(w);
  f->nrings = 1;
  f->rings[0].radius = r;
  f->rings[0].size = 1.3;
  m->ring_px = 140;
  m->ring_w = 240;
  m->out_r = r - 160;
  m->out_band = 220;
  m->lit_r = r;
  m->lit_w = 140;
  m->lit = 2.2 * (1 - 0.6 * w);
  double flash = 0.9 * pow(1 - clamp01((p - 0.28) / 0.15), 2);
  f->glow = fmax(f->glow, flash);
  f->logo_shrink =
      fmax(f->logo_shrink, 1 - 0.88 * (1 - ease_in_cubic((p - 0.3) / 0.3)));
  dot(f, 9, clamp01((p - 0.5) / 0.1) * (1 - clamp01((p - 0.85) / 0.12)));
}

void wp_show_apply(const struct wallpaper_config *c, enum wp_show s,
                   double t, struct wp_frame *f) {
  int len = wp_show_ms(c, s);
  if (s == WP_SHOW_NONE || s >= WP_SHOW_COUNT)
    return;
  if (wp_show_is_intro(s)) {
    if (t >= len)
      return;
    double u = t * TOPO_INTRO_MS / len;
    switch (s) {
    case WP_SHOW_SHOCK:
      shock(c, u, f);
      return;
    case WP_SHOW_RISE:
      rise(c, u, f);
      return;
    default:
      intro(c, t, f);
      return;
    }
  }
  if (t >= len) {
    f->nbars = 0;
    f->fade = 1;
    return;
  }
  double p = t / len;
  switch (s) {
  case WP_SHOW_CRT:
    crt(c, t, f);
    return;
  case WP_SHOW_DRAIN:
    drain(c, p, f);
    details_out(p, f, len);
    return;
  case WP_SHOW_SINK:
    sink(c, p, f);
    details_out(p, f, len);
    return;
  case WP_SHOW_BLAST:
    blast(c, p, f);
    details_out(p, f, len);
    return;
  default:
    scatter(c, t, f);
    return;
  }
}
