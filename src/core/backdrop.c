/* The wallpaper look handed to overlays that hide the windows. */
#include "backdrop.h"

#include <stddef.h>

static const struct backdrop_source *source;

void backdrop_set_source(const struct backdrop_source *s) { source = s; }
const struct backdrop_source *backdrop_get_source(void) { return source; }
