/* The visualiser's analysis: a windowed FFT of the latest audio, grouped into
 * log-spaced bands, then cava-style bars (auto sensitivity, instant rise,
 * gravity fall), plus the irregularity that keeps the ring from looking
 * machine-made. */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "wallpaper.h"

/* Bars show this many dB below the loudest recent band. */
#define VIZ_RANGE_DB 30.0
/* The sensitivity never rises past this: quieter is shown as quiet. */
#define VIZ_PEAK_FLOOR_DB -45.0
/* How fast the loudest band forgets a loud moment, dB/s. */
#define VIZ_PEAK_DECAY 6.0

/* In-place radix-2 FFT of n (a power of two) complex values. */
static void fft(float *re, float *im, int n) {
  for (int i = 1, j = 0; i < n; ++i) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1)
      j ^= bit;
    j ^= bit;
    if (i < j) {
      float t = re[i];
      re[i] = re[j];
      re[j] = t;
      t = im[i];
      im[i] = im[j];
      im[j] = t;
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    double a = -2 * M_PI / len;
    float wr = (float)cos(a), wi = (float)sin(a);
    for (int i = 0; i < n; i += len) {
      float cr = 1, ci = 0;
      for (int k = 0; k < len / 2; ++k) {
        int p = i + k, q = p + len / 2;
        float xr = re[q] * cr - im[q] * ci;
        float xi = re[q] * ci + im[q] * cr;
        re[q] = re[p] - xr;
        im[q] = im[p] - xi;
        re[p] += xr;
        im[p] += xi;
        float t = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = t;
      }
    }
  }
}

/* Power at fractional bin b, interpolated between its neighbours. */
static double power_at(const float *pw, double b) {
  int i = (int)floor(b);
  if (i < 0)
    return pw[0];
  if (i >= WP_VIZ_FFT / 2 - 1)
    return pw[WP_VIZ_FFT / 2 - 1];
  double t = b - i;
  return pw[i] * (1 - t) + pw[i + 1] * t;
}

void wp_viz_analyse(const float *samples, int rate,
                    const struct wallpaper_config *c, float *db) {
  static float re[WP_VIZ_FFT], im[WP_VIZ_FFT], pw[WP_VIZ_FFT / 2];
  static float window[WP_VIZ_FFT];
  if (window[WP_VIZ_FFT / 2] == 0)
    for (int i = 0; i < WP_VIZ_FFT; ++i)
      window[i] = (float)(0.5 - 0.5 * cos(2 * M_PI * i / (WP_VIZ_FFT - 1)));
  for (int i = 0; i < WP_VIZ_FFT; ++i) {
    re[i] = samples[i] * window[i];
    im[i] = 0;
  }
  fft(re, im, WP_VIZ_FFT);
  /* A full-scale sine peaks at N/4 through the Hann window: 0 dB. */
  double norm = 4.0 / WP_VIZ_FFT;
  for (int i = 0; i < WP_VIZ_FFT / 2; ++i)
    pw[i] = (float)((re[i] * re[i] + im[i] * im[i]) * norm * norm);

  int n = c->viz_bars < WP_VIZ_MAX ? c->viz_bars : WP_VIZ_MAX;
  double df = (double)rate / WP_VIZ_FFT;
  double lo = fmax(c->viz_low_hz, df);
  double hi = fmax(fmin(c->viz_high_hz, rate / 2.0), lo * 1.01);
  for (int i = 0; i < n; ++i) {
    /* The band's edges, as fractional bins. Summing a band's power keeps
     * music (louder in the bass, about pink) roughly level across bands. */
    double b0 = lo * pow(hi / lo, (double)i / n) / df;
    double b1 = lo * pow(hi / lo, (double)(i + 1) / n) / df;
    double p = 0;
    if (b1 - b0 < 1) {
      /* Narrower than a bin (the bass): read between bins instead. */
      p = power_at(pw, (b0 + b1) / 2);
    } else {
      for (int k = (int)ceil(b0); k <= (int)floor(b1) && k < WP_VIZ_FFT / 2;
           ++k)
        p += pw[k];
    }
    db[i] = (float)(10 * log10(p + 1e-12));
  }
}

void wp_viz_step(struct wp_viz *v, const struct wallpaper_config *c,
                 const float (*db)[WP_VIZ_MAX], double dt) {
  int n = c->viz_bars < WP_VIZ_MAX ? c->viz_bars : WP_VIZ_MAX;
  float target[2][WP_VIZ_MAX] = {{0}};
  if (v->peak == 0)
    v->peak = VIZ_PEAK_FLOOR_DB;
  if (db) {
    /* One sensitivity for both sides, so the stereo balance shows. */
    float loudest = -1e9f;
    for (int s = 0; s < 2; ++s)
      for (int i = 0; i < n; ++i)
        loudest = fmaxf(loudest, db[s][i]);
    v->peak = (float)fmax(fmax(v->peak - VIZ_PEAK_DECAY * dt, loudest),
                          VIZ_PEAK_FLOOR_DB);
    for (int s = 0; s < 2; ++s) {
      float raw[WP_VIZ_MAX];
      for (int i = 0; i < n; ++i)
        raw[i] = (float)fmin(
            fmax((db[s][i] - (v->peak - VIZ_RANGE_DB)) / VIZ_RANGE_DB, 0), 1);
      /* A light blur, so neighbours don't flicker apart. */
      for (int i = 0; i < n; ++i) {
        float l = raw[i > 0 ? i - 1 : i], r = raw[i < n - 1 ? i + 1 : i];
        target[s][i] = 0.25f * l + 0.5f * raw[i] + 0.25f * r;
      }
    }
  }
  for (int s = 0; s < 2; ++s)
    for (int i = 0; i < n; ++i) {
      float *bar = &v->bars[s][i], *vel = &v->vel[s][i];
      if (target[s][i] >= *bar) {
        *bar = target[s][i];
        *vel = 0;
      } else {
        *vel += (float)(c->viz_fall * dt);
        *bar = fmaxf(*bar - *vel * (float)dt, target[s][i]);
      }
    }
}

/* Move x towards target with time constant up (rising) or down (falling). */
static double follow(double x, double target, double up, double down,
                     double dt) {
  double tau = target > x ? up : down;
  return target + (x - target) * exp(-dt / tau);
}

/* A bass hit: the bass jumped well above its recent average. A held note
 * kicks once; the next kick needs the bass to fall back first. */
#define KICK_LEVEL 0.45
#define KICK_RISE 0.15
#define KICK_GAP_S 0.12

void wp_react_step(struct wp_react *r, const struct wp_viz *v,
                   const struct wallpaper_config *c, double dt) {
  if (dt <= 0)
    return;
  int n = c->viz_bars < WP_VIZ_MAX ? c->viz_bars : WP_VIZ_MAX;
  int nbass = n / 8 > 0 ? n / 8 : 1;
  double all = 0, bass = 0;
  for (int i = 0; i < n; ++i) {
    double l = (v->bars[0][i] + v->bars[1][i]) / 2;
    all += l;
    if (i < nbass)
      bass += l;
  }
  all /= n;
  bass /= nbass;
  r->loud = follow(r->loud, all, 0.08, 0.5, dt);
  r->energy = follow(r->energy, all, 0.03, 0.15, dt);

  for (int g = 0; g < 6; ++g) {
    int i0 = g * n / 6, i1 = (g + 1) * n / 6;
    double sum = 0;
    for (int i = i0; i < i1; ++i)
      sum += (v->bars[0][i] + v->bars[1][i]) / 2;
    double level = i1 > i0 ? sum / (i1 - i0) : 0;
    r->slices[g] = follow(r->slices[g], level, 0.03, 0.2, dt);
  }

  r->since_kick += dt;
  r->pulse *= exp(-dt / fmax(c->react_pulse_ms / 1000.0, 1e-3));
  r->kick *= exp(-dt / fmax(c->react_spin_kick_ms / 1000.0, 1e-3));
  if (r->held && bass < r->bass_avg + KICK_RISE / 2)
    r->held = false;
  if (!r->held && bass > KICK_LEVEL && bass - r->bass_avg > KICK_RISE &&
      r->since_kick >= KICK_GAP_S) {
    r->pulse = 1;
    r->kick += c->react_spin_kick;
    r->since_kick = 0;
    r->held = true;
  }
  r->bass = bass;
  r->bass_avg = follow(r->bass_avg, bass, 0.4, 0.4, dt);
  if (r->pulse < 1e-3)
    r->pulse = 0;
  if (r->kick < 1e-3)
    r->kick = 0;
}

double wp_viz_hash(int side, int bar, int what) {
  uint32_t x = (uint32_t)side * 0x9e3779b9u ^ (uint32_t)bar * 0x85ebca6bu ^
               (uint32_t)what * 0xc2b2ae35u;
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x / 2147483647.5 - 1;
}

/* Two slow sines per bar at their own rates and phases: the bars breathe
 * a little apart from each other instead of moving as one. */
double wp_viz_wobble(const struct wallpaper_config *c, int side, int bar,
                     double t) {
  if (c->viz_jitter <= 0)
    return 1;
  double f1 = 0.35 + 0.25 * wp_viz_hash(side, bar, 10);
  double f2 = 0.9 + 0.5 * wp_viz_hash(side, bar, 11);
  double p1 = M_PI * wp_viz_hash(side, bar, 12);
  double p2 = M_PI * wp_viz_hash(side, bar, 13);
  double wob = 0.6 * sin(2 * M_PI * f1 * t + p1) +
               0.4 * sin(2 * M_PI * f2 * t + p2);
  return 1 + 0.2 * c->viz_jitter * wob;
}
