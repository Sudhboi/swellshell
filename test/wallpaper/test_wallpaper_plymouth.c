/* The plymouth theme `swell wallpaper plymouth` writes: its files, the
 * still snowflake and its lambdas' glows (nothing else: no map). */
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

  /* The snowflake: square, clear in the corners, drawn well inside the
   * box (its middle is a hole), centred by the script. */
  double k = W / WP_REF_WIDTH, r = c->logo_radius * k;
  cairo_surface_t *logo = load("logo.png");
  int w = cairo_image_surface_get_width(logo);
  assert(w == cairo_image_surface_get_height(logo));
  assert(w > 2 * r && w < 4 * r);
  assert(at(logo, 0, 0) >> 24 == 0 && at(logo, w - 1, w - 1) >> 24 == 0);
  int drawn = 0, far = 0;
  for (int y = 0; y < w; ++y)
    for (int x = 0; x < w; ++x)
      if (at(logo, x, y) >> 24) {
        ++drawn;
        far += hypot(x + 0.5 - w / 2.0, y + 0.5 - w / 2.0) > w / 2.0 - 2;
      }
  assert(drawn > r * r && far == 0);
  cairo_surface_destroy(logo);

  const char *sc = slurp("swell-topo.script");
  char want[160];
  snprintf(want, sizeof want, "logo = place(\"logo.png\", %d, %d, 1);",
           -w / 2, -w / 2);
  assert(strstr(sc, want));
  snprintf(want, sizeof want, "nglow = 6; lap = %g; peak = %g;", WP_PLY_LAP,
           WP_PLY_PEAK);
  assert(strstr(sc, want));
  /* (still: nothing turned) */
  assert(!strstr(sc, "Rotate"));
  /* Nothing shows but through refresh, which a shutdown or reboot never
   * gets: black after swell's outro. */
  assert(strstr(sc, "if (mode != \"shutdown\" && mode != \"reboot\")\n"
                    "  Plymouth.SetRefreshFunction(refresh);\n"));
  const char *rf = strstr(sc, "SetRefreshFunction");
  assert(rf && !strstr(rf + 1, "SetRefreshFunction"));

  /* The glows, one a lambda, under the glass, clockwise (y down) from the
   * top a sixth of a turn apart, round the centre. */
  double prev = -1;
  for (int i = 0; i < 6; ++i) {
    char name[32];
    snprintf(name, sizeof name, "glow-%d.png", i);
    cairo_surface_t *g = load(name);
    snprintf(want, sizeof want, "glow[%d] = place(\"glow-%d.png\", ", i, i);
    const char *line = strstr(sc, want);
    assert(line);
    int gx, gy;
    assert(sscanf(line + strlen(want), "%d, %d, 0);", &gx, &gy) == 2);
    double sx = 0, sy = 0, sum = 0;
    int gw = cairo_image_surface_get_width(g),
        gh = cairo_image_surface_get_height(g);
    for (int y = 0; y < gh; ++y)
      for (int x = 0; x < gw; ++x) {
        double a = at(g, x, y) >> 24;
        sx += a * (gx + x + 0.5);
        sy += a * (gy + y + 0.5);
        sum += a;
      }
    assert(sum > 0);
    sx /= sum;
    sy /= sum;
    assert(hypot(sx, sy) > 0.3 * r && hypot(sx, sy) < 1.2 * r);
    double ang = fmod(atan2(sx, -sy) * 180 / M_PI + 360, 360);
    if (i > 0)
      assert(fabs(ang - prev - 60) < 10);
    prev = ang;
    cairo_surface_destroy(g);
  }

  /* Only those, the script and the theme file. */
  DIR *dp = opendir(dir);
  assert(dp);
  int files = 0;
  for (struct dirent *e; (e = readdir(dp));)
    if (e->d_name[0] != '.') {
      ++files;
      assert(strcmp(e->d_name, "logo.png") == 0 ||
             strncmp(e->d_name, "glow-", 5) == 0 ||
             strncmp(e->d_name, "swell-topo.", 11) == 0);
    }
  closedir(dp);
  assert(files == 1 + 6 + 2);

  /* The theme file points at the install. */
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
