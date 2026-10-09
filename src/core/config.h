/* Runtime configuration: ~/.config/swell/config.toml.
 *
 * Each module describes its settings as a table of cfg_field entries that
 * point into its config struct. The TOML key is the field name, and the
 * module's [section] holds them. To add a setting: add a struct field, give
 * it a default in the module's defaults initialiser, and add one CFG_* line.
 * Invalid values are reported with file:line and keep the default. */
#ifndef SWELL_CONFIG_H
#define SWELL_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "tomlc17.h"

enum cfg_type {
  CFG_T_INT,    /* int, range-checked */
  CFG_T_DOUBLE, /* double, range-checked */
  CFG_T_BOOL,   /* bool */
  CFG_T_STRING, /* char[size] */
  CFG_T_COLOR,  /* uint32_t 0xRRGGBBAA, written "#rrggbb" or "#rrggbbaa" */
};

struct cfg_field {
  const char *key;
  enum cfg_type type;
  size_t offset;
  size_t size; /* CFG_T_STRING buffer size */
  double min, max;
  const char *doc; /* one-line description for `swell config` */
};

#define CFG_INT(type, field, lo, hi, doc)                                      \
  {#field, CFG_T_INT, offsetof(type, field), 0, (lo), (hi), (doc)}
#define CFG_DOUBLE(type, field, lo, hi, doc)                                   \
  {#field, CFG_T_DOUBLE, offsetof(type, field), 0, (lo), (hi), (doc)}
#define CFG_BOOL(type, field, doc)                                             \
  {#field, CFG_T_BOOL, offsetof(type, field), 0, 0, 0, (doc)}
#define CFG_STRING(type, field, doc)                                           \
  {#field, CFG_T_STRING, offsetof(type, field),                               \
   sizeof(((type *)0)->field), 0, 0, (doc)}
#define CFG_COLOR(type, field, doc)                                            \
  {#field, CFG_T_COLOR, offsetof(type, field), 0, 0, 0, (doc)}

/* A parsed config file. A missing file is an empty document. */
struct cfg_doc {
  char *path;
  toml_result_t toml;
  bool loaded; /* the file existed and parsed */
  bool broken; /* the file exists but could not be read or parsed */
  char error[256];
};

/* Default path: $SWELL_CONFIG, else $XDG_CONFIG_HOME/swell/config.toml, else
 * ~/.config/swell/config.toml. Returns a static buffer. */
const char *cfg_default_path(void);

/* Parse path (NULL: cfg_default_path()). Errors are reported and leave an
 * empty document, so the defaults apply. */
void cfg_load(struct cfg_doc *doc, const char *path);
void cfg_free(struct cfg_doc *doc);

/* The [name] table, or a TOML_UNKNOWN datum if absent. */
toml_datum_t cfg_section(const struct cfg_doc *doc, const char *name);

/* Apply the keys of section to the struct at dst. Keys listed in extra_keys
 * (NULL-terminated, may be NULL) are left for the module; any other unknown
 * key is warned about. */
void cfg_apply(toml_datum_t section, const char *section_name,
               const struct cfg_field *fields, size_t nfields, void *dst,
               const char *const *extra_keys);

/* Write s as a TOML basic string (quoted and escaped). */
void cfg_dump_string(FILE *f, const char *s);

/* Write fields of src as TOML lines (with doc comments) to f. */
void cfg_dump(FILE *f, const struct cfg_field *fields, size_t nfields,
              const void *src);

/* Report a problem with datum d (uses its line number). */
void cfg_error(toml_datum_t d, const char *section, const char *key,
               const char *fmt, ...) __attribute__((format(printf, 4, 5)));

/* Parse "#rrggbb" / "#rrggbbaa" (the # is optional). */
bool cfg_parse_color(const char *s, uint32_t *out);

/* Colour helpers for cairo: components of 0xRRGGBBAA in 0..1. */
static inline double col_r(uint32_t c) { return ((c >> 24) & 0xff) / 255.0; }
static inline double col_g(uint32_t c) { return ((c >> 16) & 0xff) / 255.0; }
static inline double col_b(uint32_t c) { return ((c >> 8) & 0xff) / 255.0; }
static inline double col_a(uint32_t c) { return (c & 0xff) / 255.0; }

#endif
