/* One picture of an output (ext-image-copy-capture), into a wl_shm buffer
 * that can be attached to a surface as it is. */
#ifndef SWELL_WSFADE_SNAP_H
#define SWELL_WSFADE_SNAP_H

#include "buffer.h"
#include "wl.h"

struct snap;

/* buf is the picture (owned by the snap), NULL if capturing failed. Called
 * once; the snap may be destroyed from inside. */
typedef void (*snap_fn)(struct snap *s, struct buffer *buf, void *data);

/* NULL if the compositor can't capture outputs. */
struct snap *snap_start(struct swell_wl *wl, struct wl_output *output,
                        snap_fn fn, void *data);
/* Cancel, or free the picture. */
void snap_destroy(struct snap *s);

#endif
