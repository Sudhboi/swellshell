/* A config file changes the OSD's rendering without a rebuild: load a TOML
 * file with cfg_load/cfg_apply through the module's field table and compare
 * against the defaults. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../png.h"
#include "module.h"
#include "osd/render.h"

extern const struct swell_module osd_module;

static cairo_t *draw(const struct osd_config *c) {
  struct osd_render_state rs = {.kind = OSD_VOLUME, .level = 0.6, .pct = 60,
                                .alpha = 1, .slide = 1};
  cairo_t *cr = canvas(c->width, osd_surface_height(c));
  osd_render_surface(cr, c, &rs);
  return cr;
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : ".";
  struct osd_config def = *(const struct osd_config *)osd_module.defaults;
  struct osd_config cfg = def;

  struct cfg_doc doc;
  load_toml(&doc, "[osd]\n"
                  "width = 300\n"
                  "top_gap = 40\n"
                  "background = \"#202060\"\n"
                  "ring_light = \"#ff8800\"\n"
                  "font_size = 30\n");
  assert(doc.loaded);
  cfg_apply(cfg_section(&doc, "osd"), "osd", osd_module.fields,
            osd_module.nfields, &cfg, NULL);
  cfg_free(&doc);
  assert(cfg.width == 300 && cfg.top_gap == 40 && cfg.font_size == 30);
  assert(cfg.background == 0x202060ffu && cfg.ring_light == 0xff8800ffu);
  assert(cfg.height == def.height && cfg.border == def.border);
  assert(osd_surface_height(&cfg) == def.height + 40);

  cairo_t *a = draw(&def), *b = draw(&cfg);
  save_png(a, dir, "osd_config_default");
  save_png(b, dir, "osd_config_custom");
  assert(image_width(a) == def.width * SCALE);
  assert(image_width(b) == 300 * SCALE);
  assert(pixel(a, 4, def.top_gap + 4) == def.background);
  assert(pixel(b, 4, 40 + 4) == 0x202060ffu);
  /* The gap grew: where the default panel starts, the custom one is empty. */
  assert(pixel(b, 4, def.top_gap + 4) == 0);
  cairo_destroy(a);
  cairo_destroy(b);

  printf("osd config render: ok\n");
  return 0;
}
