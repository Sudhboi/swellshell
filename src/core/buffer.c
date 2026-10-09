#include "buffer.h"

#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include "util.h"

static void handle_release(void *data, struct wl_buffer *wl_buffer) {
  struct buffer *buf = data;
  buf->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = handle_release,
};

struct buffer *buffer_create(struct wl_shm *shm, int width, int height,
                             uint32_t format) {
  bool alpha =
      format == WL_SHM_FORMAT_ARGB8888 || format == WL_SHM_FORMAT_ABGR8888;
  cairo_format_t cformat = alpha ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24;
  int stride = cairo_format_stride_for_width(cformat, width);
  size_t size = (size_t)stride * height;

  int fd = memfd_create("swell-shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd < 0) {
    warn("memfd_create failed");
    return NULL;
  }
  if (ftruncate(fd, size) < 0) {
    warn("ftruncate failed");
    close(fd);
    return NULL;
  }
  void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (data == MAP_FAILED) {
    warn("mmap failed");
    close(fd);
    return NULL;
  }

  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
  struct buffer *buf = xcalloc(1, sizeof(*buf));
  buf->wl_buffer =
      wl_shm_pool_create_buffer(pool, 0, width, height, stride, format);
  wl_shm_pool_destroy(pool);
  close(fd);

  wl_buffer_add_listener(buf->wl_buffer, &buffer_listener, buf);
  buf->data = data;
  buf->size = size;
  buf->width = width;
  buf->height = height;
  buf->stride = stride;
  buf->format = format;
  buf->cairo =
      cairo_image_surface_create_for_data(data, cformat, width, height, stride);
  return buf;
}

void buffer_destroy(struct buffer *buf) {
  if (!buf)
    return;
  cairo_surface_destroy(buf->cairo);
  wl_buffer_destroy(buf->wl_buffer);
  munmap(buf->data, buf->size);
  free(buf);
}
