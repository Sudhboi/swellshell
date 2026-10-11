/* The visualiser's analysis: tones land in the right bands, silence and
 * gravity bring the bars down. */
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "module.h"
#include "wallpaper/wallpaper.h"

extern const struct swell_module wallpaper_module;

#define RATE 48000

static void tone(float *s, double hz, double amp) {
  for (int i = 0; i < WP_VIZ_FFT; ++i)
    s[i] = (float)(amp * sin(2 * M_PI * hz * i / RATE));
}

/* The band whose level is highest. */
static int loudest(const float *db, int n) {
  int best = 0;
  for (int i = 1; i < n; ++i)
    if (db[i] > db[best])
      best = i;
  return best;
}

/* The band holding hz, from the config's log spacing. */
static int band_of(const struct wallpaper_config *c, double hz) {
  return (int)(log(hz / c->viz_low_hz) / log(c->viz_high_hz / c->viz_low_hz) *
               c->viz_bars);
}

int main(void) {
  const struct wallpaper_config *c = wallpaper_module.defaults;
  int n = c->viz_bars;
  static float s[WP_VIZ_FFT];
  float db[WP_VIZ_MAX];

  double tones[] = {100, 440, 4000};
  for (size_t t = 0; t < sizeof(tones) / sizeof(tones[0]); ++t) {
    tone(s, tones[t], 0.5);
    wp_viz_analyse(s, RATE, c, db);
    int want = band_of(c, tones[t]);
    int got = loudest(db, n);
    assert(abs(got - want) <= 1);
  }
  /* Full scale is about 0 dB. */
  tone(s, 1000, 1.0);
  wp_viz_analyse(s, RATE, c, db);
  assert(fabs(db[loudest(db, n)]) < 3);

  /* Bars: a tone shows at once, at full height after auto-sensitivity. */
  struct wp_viz v;
  memset(&v, 0, sizeof(v));
  tone(s, 440, 0.5);
  wp_viz_analyse(s, RATE, c, db);
  wp_viz_step(&v, c, db, 1 / 60.0);
  int b = band_of(c, 440);
  assert(v.bars[b] > 0.6f);
  assert(v.bars[0] < 0.2f); /* the far bass stays low */

  /* Silence: gravity brings everything to 0 within sqrt(2 / fall) s. */
  double fall_s = sqrt(2 / c->viz_fall);
  int steps = (int)ceil(fall_s * 60) + 1;
  for (int i = 0; i < steps / 2; ++i)
    wp_viz_step(&v, c, NULL, 1 / 60.0);
  assert(v.bars[b] > 0); /* still falling halfway */
  for (int i = 0; i < steps; ++i)
    wp_viz_step(&v, c, NULL, 1 / 60.0);
  for (int i = 0; i < n; ++i)
    assert(v.bars[i] == 0);

  /* Digital silence never shows, however long the sensitivity adapts. */
  memset(s, 0, sizeof(s));
  memset(&v, 0, sizeof(v));
  for (int i = 0; i < 600; ++i) {
    wp_viz_analyse(s, RATE, c, db);
    wp_viz_step(&v, c, db, 1 / 60.0);
  }
  for (int i = 0; i < n; ++i)
    assert(v.bars[i] == 0);
  return 0;
}
