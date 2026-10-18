/* wl_shm-backed pixel buffers, wrapped as cairo image surfaces. */
#ifndef SWELL_BUFFER_H
#define SWELL_BUFFER_H

#include <cairo.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

struct buffer {
  struct wl_buffer *wl_buffer;
  void *data;
  size_t size;
  int width, height, stride;
  uint32_t format; /* WL_SHM_FORMAT_* */
  bool busy;       /* attached to a surface and not yet released */
  cairo_surface_t *cairo;
};

/* format must be a 32-bit [AX]RGB8888 or [AX]BGR8888 wl_shm format. For the
 * BGR ones the cairo surface sees red and blue swapped. */
struct buffer *buffer_create(struct wl_shm *shm, int width, int height,
                             uint32_t format);
void buffer_destroy(struct buffer *buf);

/* How well buffer_create handles a wl_shm format offered for capture:
 * higher is better, 0 = unusable. */
int buffer_format_rank(uint32_t format);

#endif
