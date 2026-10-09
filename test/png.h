/* Helpers shared by the offscreen render tests. */
#ifndef SWELL_TEST_PNG_H
#define SWELL_TEST_PNG_H

#include <assert.h>
#include <cairo.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "config.h"

#define SCALE 2

/* A cleared ARGB surface of logical w x h at SCALE, like the layer surface
 * hands the draw callback. */
static inline cairo_t *canvas(int w, int h) {
  cairo_surface_t *s =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w * SCALE, h * SCALE);
  cairo_t *cr = cairo_create(s);
  cairo_surface_destroy(s);
  cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  cairo_scale(cr, SCALE, SCALE);
  return cr;
}

static inline void save_png(cairo_t *cr, const char *dir, const char *name) {
  char path[1024];
  snprintf(path, sizeof path, "%s/%s.png", dir, name);
  cairo_surface_t *s = cairo_get_target(cr);
  cairo_surface_flush(s);
  cairo_status_t st = cairo_surface_write_to_png(s, path);
  if (st != CAIRO_STATUS_SUCCESS) {
    fprintf(stderr, "%s: %s\n", path, cairo_status_to_string(st));
    exit(1);
  }
  printf("wrote %s\n", path);
}

/* Un-premultiplied 0xRRGGBBAA of the device pixel at logical (x, y). */
static inline uint32_t pixel(cairo_t *cr, double x, double y) {
  cairo_surface_t *s = cairo_get_target(cr);
  cairo_surface_flush(s);
  int px = (int)(x * SCALE), py = (int)(y * SCALE);
  assert(px >= 0 && py >= 0 && px < cairo_image_surface_get_width(s) &&
         py < cairo_image_surface_get_height(s));
  uint32_t v = *(uint32_t *)(cairo_image_surface_get_data(s) +
                             py * cairo_image_surface_get_stride(s) + px * 4);
  uint32_t a = v >> 24;
  if (!a)
    return 0;
  uint32_t r = ((v >> 16) & 0xff) * 255 / a, g = ((v >> 8) & 0xff) * 255 / a,
           b = (v & 0xff) * 255 / a;
  return r << 24 | g << 16 | b << 8 | a;
}

static inline int image_width(cairo_t *cr) {
  return cairo_image_surface_get_width(cairo_get_target(cr));
}

/* Write toml to a temp file and load it. */
static inline void load_toml(struct cfg_doc *doc, const char *toml) {
  char path[] = "/tmp/swell-test-XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  FILE *f = fdopen(fd, "w");
  fputs(toml, f);
  fclose(f);
  cfg_load(doc, path);
  remove(path);
}

#endif
