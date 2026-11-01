/* The plymouth theme `swell wallpaper plymouth` writes: its files and the
 * snowflake frames (nothing else: no map). */
#include <assert.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "module.h"
#include "wallpaper/render.h"
#include "wallpaper/wallpaper.h"

extern const struct swell_module wallpaper_module;

#define W 720
#define H 450

static char dir[] = "/tmp/swell-plymouth-XXXXXX";

static cairo_surface_t *load(const char *name) {
  char path[256];
  snprintf(path, sizeof path, "%s/%s", dir, name);
  cairo_surface_t *s = cairo_image_surface_create_from_png(path);
  assert(cairo_surface_status(s) == CAIRO_STATUS_SUCCESS);
  return s;
}

static uint32_t at(cairo_surface_t *s, int x, int y) {
  const unsigned char *d = cairo_image_surface_get_data(s);
  return ((const uint32_t *)(d + y * cairo_image_surface_get_stride(s)))[x];
}

static char *slurp(const char *name) {
  char path[256];
  snprintf(path, sizeof path, "%s/%s", dir, name);
  FILE *f = fopen(path, "r");
  assert(f);
  static char buf[16384];
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  buf[n] = 0;
  fclose(f);
  return buf;
}

int main(void) {
  const struct wallpaper_config *c = wallpaper_module.defaults;
  assert(mkdtemp(dir));
  assert(wp_plymouth_theme(c, dir, "/x/swell-topo", W) == 0);

  /* Snowflake frames: square, the same size, clear in the corners, drawn
   * near the centre (its middle is a hole), within the box at any turn. */
  double k = W / WP_REF_WIDTH, r = c->logo_radius * k;
  int side = 0;
  for (int j = 0; j < WP_PLY_LOGO; ++j) {
    char name[32];
    snprintf(name, sizeof name, "logo-%d.png", j);
    cairo_surface_t *s = load(name);
    int w = cairo_image_surface_get_width(s);
    assert(w == cairo_image_surface_get_height(s));
    assert(j == 0 || w == side);
    side = w;
    assert(w > 2 * r && w < 4 * r);
    assert(at(s, 0, 0) >> 24 == 0 && at(s, w - 1, w - 1) >> 24 == 0);
    int drawn = 0, far = 0;
    for (int y = 0; y < w; ++y)
      for (int x = 0; x < w; ++x)
        if (at(s, x, y) >> 24) {
          ++drawn;
          far += hypot(x + 0.5 - w / 2.0, y + 0.5 - w / 2.0) > w / 2.0 - 2;
        }
    assert(drawn > r * r && far == 0);
    cairo_surface_destroy(s);
  }

  /* Only the frames, the script and the theme file. */
  DIR *dp = opendir(dir);
  assert(dp);
  int files = 0;
  for (struct dirent *e; (e = readdir(dp));)
    if (e->d_name[0] != '.') {
      ++files;
      assert(strncmp(e->d_name, "logo-", 5) == 0 ||
             strncmp(e->d_name, "swell-topo.", 11) == 0);
    }
  closedir(dp);
  assert(files == WP_PLY_LOGO + 2);

  /* The script loads the frames and no other image; the theme file points
   * at the install. */
  const char *sc = slurp("swell-topo.script");
  assert(strstr(sc, "Image(\"logo-\" + i + \".png\")"));
  const char *im = strstr(sc, " Image(\"");
  assert(im && !strstr(im + 1, " Image(\""));
  char want[64];
  snprintf(want, sizeof want, "nlogo = %d; step = %g;", WP_PLY_LOGO,
           WP_PLY_LOGO_STEP);
  assert(strstr(sc, want));
  assert(WP_PLY_LOGO * WP_PLY_LOGO_STEP == 60);
  const char *th = slurp("swell-topo.plymouth");
  assert(strstr(th, "ModuleName=script\n"));
  assert(strstr(th, "ImageDir=/x/swell-topo\n"));
  assert(strstr(th, "ScriptFile=/x/swell-topo/swell-topo.script\n"));

  /* Other wallpaper commands go on to the daemon. */
  char *intro[] = {"intro"}, *bare[] = {"plymouth"};
  assert(wp_run_local(NULL, c, true, 1, intro) == -1);
  assert(wp_run_local(NULL, c, true, 0, NULL) == -1);
  assert(wp_run_local(NULL, c, true, 1, bare) == 2);

  char cmd[300];
  snprintf(cmd, sizeof cmd, "rm -r %s", dir);
  assert(system(cmd) == 0);
  wp_render_logo_finish();
  printf("plymouth theme ok\n");
  return 0;
}
