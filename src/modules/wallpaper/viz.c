/* The visualiser's analysis: a windowed FFT of the latest audio, grouped into
 * log-spaced bands, then cava-style bars (auto sensitivity, instant rise,
 * gravity fall). */
#include <math.h>
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
                 const float *db, double dt) {
  int n = c->viz_bars < WP_VIZ_MAX ? c->viz_bars : WP_VIZ_MAX;
  float target[WP_VIZ_MAX] = {0};
  if (v->peak == 0)
    v->peak = VIZ_PEAK_FLOOR_DB;
  if (db) {
    float loudest = -1e9f;
    for (int i = 0; i < n; ++i)
      loudest = fmaxf(loudest, db[i]);
    v->peak = (float)fmax(fmax(v->peak - VIZ_PEAK_DECAY * dt, loudest),
                          VIZ_PEAK_FLOOR_DB);
    float raw[WP_VIZ_MAX];
    for (int i = 0; i < n; ++i)
      raw[i] = (float)fmin(
          fmax((db[i] - (v->peak - VIZ_RANGE_DB)) / VIZ_RANGE_DB, 0), 1);
    /* A light blur, so neighbours don't flicker apart. */
    for (int i = 0; i < n; ++i) {
      float l = raw[i > 0 ? i - 1 : i], r = raw[i < n - 1 ? i + 1 : i];
      target[i] = 0.25f * l + 0.5f * raw[i] + 0.25f * r;
    }
  }
  for (int i = 0; i < n; ++i) {
    if (target[i] >= v->bars[i]) {
      v->bars[i] = target[i];
      v->vel[i] = 0;
    } else {
      v->vel[i] += (float)(c->viz_fall * dt);
      v->bars[i] = fmaxf(v->bars[i] - v->vel[i] * (float)dt, target[i]);
    }
  }
}
