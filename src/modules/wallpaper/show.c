/* The launch and power-off animations, as pure functions of time that turn
 * a normal frame into the show's. */
#include <math.h>

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

const char *wp_show_name(enum wp_show s) {
  switch (s) {
  case WP_SHOW_INTRO:
    return "intro";
  case WP_SHOW_SCATTER:
    return "scatter";
  case WP_SHOW_CRT:
    return "crt";
  default:
    return "none";
  }
}

int wp_show_ms(const struct wallpaper_config *c, enum wp_show s) {
  switch (s) {
  case WP_SHOW_INTRO:
    return (int)intro_lock_ms(c) + INTRO_SETTLE_MS;
  case WP_SHOW_SCATTER:
    return c->outro_ms > 0 ? c->outro_ms : 1;
  case WP_SHOW_CRT:
    return c->outro_crt_ms > 0 ? c->outro_crt_ms : 1;
  default:
    return 0;
  }
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

void wp_show_apply(const struct wallpaper_config *c, enum wp_show s,
                   double t, struct wp_frame *f) {
  int len = wp_show_ms(c, s);
  switch (s) {
  case WP_SHOW_INTRO:
    if (t < len)
      intro(c, t, f);
    return;
  case WP_SHOW_SCATTER:
  case WP_SHOW_CRT:
    if (t >= len) {
      f->nbars = 0;
      f->fade = 1;
    } else if (s == WP_SHOW_SCATTER) {
      scatter(c, t, f);
    } else {
      crt(c, t, f);
    }
    return;
  default:
    return;
  }
}
