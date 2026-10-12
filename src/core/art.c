/* Track covers: PNG through cairo, JPEG through libjpeg. */
#include "art.h"

#include <ctype.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>

/* Covers are cut to a square of at most this many px. */
#define ART_MAX 1024

/* libjpeg reports errors by calling error_exit, which must not return. */
struct jpeg_err {
  struct jpeg_error_mgr mgr;
  jmp_buf jump;
};

static void jpeg_fail(j_common_ptr cinfo) {
  longjmp(((struct jpeg_err *)cinfo->err)->jump, 1);
}

/* A JPEG file as an RGB24 surface (NULL if it can't be read). */
static cairo_surface_t *load_jpeg(FILE *f) {
  struct jpeg_decompress_struct cinfo;
  struct jpeg_err err;
  cairo_surface_t *volatile out = NULL;
  JSAMPLE *volatile row = NULL;
  cinfo.err = jpeg_std_error(&err.mgr);
  err.mgr.error_exit = jpeg_fail;
  if (setjmp(err.jump)) {
    jpeg_destroy_decompress(&cinfo);
    free(row);
    if (out)
      cairo_surface_destroy(out);
    return NULL;
  }
  jpeg_create_decompress(&cinfo);
  jpeg_stdio_src(&cinfo, f);
  jpeg_read_header(&cinfo, TRUE);
  cinfo.out_color_space = JCS_RGB;
  jpeg_start_decompress(&cinfo);
  int w = (int)cinfo.output_width, h = (int)cinfo.output_height;
  out = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
  row = malloc((size_t)w * 3);
  if (cairo_surface_status(out) != CAIRO_STATUS_SUCCESS || !row)
    longjmp(err.jump, 1);
  unsigned char *dst = cairo_image_surface_get_data(out);
  int stride = cairo_image_surface_get_stride(out);
  while (cinfo.output_scanline < cinfo.output_height) {
    int y = (int)cinfo.output_scanline;
    JSAMPROW rows[1] = {row};
    jpeg_read_scanlines(&cinfo, rows, 1);
    uint32_t *d = (uint32_t *)(dst + y * stride);
    for (int x = 0; x < w; ++x)
      d[x] = (uint32_t)row[3 * x] << 16 | (uint32_t)row[3 * x + 1] << 8 |
             row[3 * x + 2];
  }
  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  free(row);
  cairo_surface_mark_dirty(out);
  return out;
}

cairo_surface_t *art_load(const char *path, bool grey) {
  /* PNG (cairo) or JPEG (libjpeg), told apart by their first bytes. */
  FILE *f = fopen(path, "rbe");
  if (!f)
    return NULL;
  unsigned char magic[8] = {0};
  size_t n = fread(magic, 1, sizeof magic, f);
  rewind(f);
  cairo_surface_t *img = NULL;
  if (n == 8 && !memcmp(magic, "\x89PNG\r\n\x1a\n", 8)) {
    img = cairo_image_surface_create_from_png(path);
  } else if (n >= 3 && magic[0] == 0xff && magic[1] == 0xd8 &&
             magic[2] == 0xff) {
    img = load_jpeg(f);
  }
  fclose(f);
  if (!img)
    return NULL;
  if (cairo_surface_status(img) != CAIRO_STATUS_SUCCESS ||
      cairo_image_surface_get_width(img) <= 0 ||
      cairo_image_surface_get_height(img) <= 0) {
    cairo_surface_destroy(img);
    return NULL;
  }
  /* The centred square, smaller if it's huge. */
  int w = cairo_image_surface_get_width(img);
  int h = cairo_image_surface_get_height(img);
  int side = w < h ? w : h, outside = side > ART_MAX ? ART_MAX : side;
  cairo_surface_t *out =
      cairo_image_surface_create(CAIRO_FORMAT_RGB24, outside, outside);
  if (cairo_surface_status(out) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(img);
    cairo_surface_destroy(out);
    return NULL;
  }
  cairo_t *cr = cairo_create(out);
  cairo_set_source_rgb(cr, 0, 0, 0); /* see-through parts over black */
  cairo_paint(cr);
  double s = (double)outside / side;
  cairo_scale(cr, s, s);
  cairo_set_source_surface(cr, img, -(w - side) / 2, -(h - side) / 2);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
  cairo_paint(cr);
  cairo_destroy(cr);
  cairo_surface_destroy(img);
  if (!grey)
    return out;
  /* Then grey: its luminance. */
  cairo_surface_flush(out);
  unsigned char *data = cairo_image_surface_get_data(out);
  int stride = cairo_image_surface_get_stride(out);
  for (int y = 0; y < outside; ++y) {
    uint32_t *p = (uint32_t *)(data + y * stride);
    for (int x = 0; x < outside; ++x) {
      uint32_t c = p[x];
      double l = 0.2126 * (c >> 16 & 0xff) + 0.7152 * (c >> 8 & 0xff) +
                 0.0722 * (c & 0xff);
      uint32_t v = (uint32_t)(l + 0.5);
      p[x] = v << 16 | v << 8 | v;
    }
  }
  cairo_surface_mark_dirty(out);
  return out;
}

bool art_file_path(const char *url, char *out, size_t n) {
  if (strncmp(url, "file://", 7))
    return false;
  url += 7;
  if (!strncmp(url, "localhost/", 10))
    url += 9;
  if (*url != '/')
    return false;
  size_t o = 0;
  for (; *url && o + 1 < n; ++url) {
    if (url[0] == '%' && isxdigit((unsigned char)url[1]) &&
        isxdigit((unsigned char)url[2])) {
      char hex[3] = {url[1], url[2], 0};
      out[o++] = (char)strtol(hex, NULL, 16);
      url += 2;
    } else {
      out[o++] = *url;
    }
  }
  out[o] = '\0';
  return true;
}
