/* A config file changes the popup's rendering without a rebuild: load a
 * TOML file with cfg_load/cfg_apply through the module's field table and
 * compare against the defaults. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../png.h"
#include "module.h"
#include "popup/render.h"

extern const struct swell_module popup_module;

static cairo_t *draw(const struct popup_config *c, const struct info *in) {
  cairo_t *cr = canvas(c->width, popup_surface_height(c));
  popup_render(cr, c, in, 1, 1, 0);
  return cr;
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : ".";
  struct popup_config def = *(const struct popup_config *)popup_module.defaults;
  struct popup_config cfg = def;

  struct cfg_doc doc;
  load_toml(&doc, "[popup]\n"
                  "width = 700\n"
                  "height = 600\n"
                  "background = \"#103010\"\n"
                  "border = \"#ff0000\"\n"
                  "row_step = 60.5\n"
                  "font_clock = 90\n");
  assert(doc.loaded);
  cfg_apply(cfg_section(&doc, "popup"), "popup", popup_module.fields,
            popup_module.nfields, &cfg, NULL);
  cfg_free(&doc);
  assert(cfg.width == 700 && cfg.height == 600 && cfg.font_clock == 90);
  assert(cfg.row_step == 60.5 && cfg.background == 0x103010ffu);
  assert(cfg.pad == def.pad && cfg.gap_rows == def.gap_rows);
  assert(popup_surface_height(&cfg) == 600 + def.bottom_gap);

  struct info in;
  memset(&in, 0, sizeof in);
  in.now = 1791295200;
  in.cpu = -1;
  in.net_signal = -1;

  cairo_t *a = draw(&def, &in), *b = draw(&cfg, &in);
  save_png(a, dir, "popup_config_default");
  save_png(b, dir, "popup_config_custom");
  assert(image_width(a) == def.width * SCALE);
  assert(image_width(b) == 700 * SCALE);
  /* Both panels rest on y = height; colours follow the config. */
  assert(pixel(a, 0.5, def.height - 4) == def.border);
  assert(pixel(b, 0.5, 600 - 4) == 0xff0000ffu);
  assert(pixel(b, 600 / 2.0, 600 - 10) == 0x103010ffu);
  assert(pixel(a, def.width / 2.0, def.height - 10) == def.background);
  /* Wider rows push the panel taller: its top is higher than by default
   * (relative to the bottom it rests on). */
  double top_a = -1, top_b = -1;
  for (double y = 0; y < def.height && top_a < 0; y += 0.5)
    if (pixel(a, 0.5, y) == def.border)
      top_a = def.height - y;
  for (double y = 0; y < 600 && top_b < 0; y += 0.5)
    if (pixel(b, 0.5, y) == 0xff0000ffu)
      top_b = 600 - y;
  printf("panel heights: default %.1f, custom %.1f\n", top_a, top_b);
  assert(top_a > 0 && top_b > top_a);
  cairo_destroy(a);
  cairo_destroy(b);

  printf("popup config render: ok\n");
  return 0;
}
