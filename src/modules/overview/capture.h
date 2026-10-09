/* overview: live window thumbnails via ext-image-copy-capture-v1, matched to
 * sway windows through ext-foreign-toplevel-list-v1 identifiers. */
#ifndef SWELL_OVERVIEW_CAPTURE_H
#define SWELL_OVERVIEW_CAPTURE_H

#include <cairo.h>

#include "overview.h"

/* Bind a fresh foreign toplevel list (if capturing is on) with its listener
 * attached at once; the caller then does a roundtrip for the initial
 * toplevels. Warns (once) if capturing is unavailable. */
void ov_capture_init(struct ov *ov);
/* Destroy every capture and toplevel handle; stop and destroy the list. */
void ov_capture_finish(struct ov *ov);

/* Attach a capture to every window in ov->model (reusing existing ones)
 * and drop captures for windows that went away. */
void ov_capture_bind_model(struct ov *ov);

/* A thumbnail of exactly w x h pixels, or NULL if no frame has arrived yet.
 * The surface is owned by the capture. */
cairo_surface_t *ov_capture_thumbnail(struct capture *cap, int w, int h);

/* Size of the latest frame. Returns false if there is none. */
bool ov_capture_size(struct capture *cap, int *w, int *h);

/* Request new frames that are due. */
void ov_capture_tick(struct ov *ov, int64_t now);
/* Milliseconds until ov_capture_tick has work to do, or -1. */
int ov_capture_timeout(struct ov *ov, int64_t now);
/* True once every capture has produced a frame or given up. */
bool ov_capture_settled(struct ov *ov);

#endif
