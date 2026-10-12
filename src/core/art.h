/* Track covers: an image file (PNG or JPEG) cut to its centred square, and
 * file:// URL decoding (wallpaper, media). */
#ifndef SWELL_ART_H
#define SWELL_ART_H

#include <cairo.h>
#include <stdbool.h>
#include <stddef.h>

/* Load an image file as an RGB24 square of at most 1024 px, see-through
 * parts over black, turned into its luminance if grey. NULL if it can't be
 * read. */
cairo_surface_t *art_load(const char *path, bool grey);

/* A file:// URL's path, %-escapes decoded, into out (false: not one). */
bool art_file_path(const char *url, char *out, size_t n);

#endif
