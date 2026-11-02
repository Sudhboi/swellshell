#include <string.h>

#include "module.h"
#include "util.h"

extern const struct swell_module osd_module;
extern const struct swell_module wsosd_module;
extern const struct swell_module popup_module;
extern const struct swell_module media_module;
extern const struct swell_module overview_module;
extern const struct swell_module workspaces_module;
extern const struct swell_module runner_module;
extern const struct swell_module wallpaper_module;
extern const struct swell_module power_module;
extern const struct swell_module wsfade_module;
extern const struct swell_module gestures_module;
extern const struct swell_module lowbat_module;

/* Initialisation order: the runner first so it sees the others' events.
 * Every module here is also listed in meson.build and test/meson.build. */
const struct swell_module *const swell_modules[] = {
    &runner_module, &workspaces_module, &osd_module,      &wsosd_module,
    &popup_module,  &media_module,      &overview_module, &wallpaper_module,
    &power_module,  &wsfade_module,     &gestures_module, &lowbat_module,
};
const size_t swell_nmodules = ARRAY_LEN(swell_modules);

const struct swell_module *module_find(const char *word) {
  for (size_t i = 0; i < swell_nmodules; ++i) {
    const struct swell_module *m = swell_modules[i];
    if (strcmp(m->name, word) == 0)
      return m;
    for (int j = 0; m->aliases && m->aliases[j]; ++j)
      if (strcmp(m->aliases[j], word) == 0)
        return m;
  }
  return NULL;
}
