#include "config.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

const char *cfg_default_path(void) {
  static char path[PATH_MAX];
  const char *env = getenv("SWELL_CONFIG");
  if (env && *env)
    return env;
  const char *xdg = getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg)
    snprintf(path, sizeof(path), "%s/swell/config.toml", xdg);
  else
    snprintf(path, sizeof(path), "%s/.config/swell/config.toml",
             getenv("HOME") ? getenv("HOME") : "");
  return path;
}

void cfg_load(struct cfg_doc *doc, const char *path) {
  memset(doc, 0, sizeof(*doc));
  doc->path = xstrdup(path ? path : cfg_default_path());
  FILE *f = fopen(doc->path, "r");
  if (!f) {
    if (errno != ENOENT) {
      doc->broken = true;
      snprintf(doc->error, sizeof(doc->error), "%s: %s", doc->path,
               strerror(errno));
      warn("%s; using defaults", doc->error);
    }
    doc->toml = toml_parse("", 0);
    return;
  }
  doc->toml = toml_parse_file_named(f, doc->path);
  fclose(f);
  if (!doc->toml.ok) {
    doc->broken = true;
    snprintf(doc->error, sizeof(doc->error), "%s: %s", doc->path,
             doc->toml.errmsg);
    warn("%s; using defaults", doc->error);
    toml_free(doc->toml);
    doc->toml = toml_parse("", 0);
    return;
  }
  doc->loaded = true;
}

void cfg_free(struct cfg_doc *doc) {
  toml_free(doc->toml);
  free(doc->path);
  memset(doc, 0, sizeof(*doc));
}

toml_datum_t cfg_section(const struct cfg_doc *doc, const char *name) {
  toml_datum_t d = toml_get(doc->toml.toptab, name);
  if (d.type != TOML_TABLE && d.type != TOML_UNKNOWN) {
    cfg_error(d, NULL, name, "expected a table");
    d.type = TOML_UNKNOWN;
  }
  return d;
}

void cfg_error(toml_datum_t d, const char *section, const char *key,
               const char *fmt, ...) {
  char msg[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  if (d.source && d.lineno)
    warn("%s:%d: %s%s%s: %s", d.source, d.lineno, section ? section : "",
         section ? "." : "", key, msg);
  else
    warn("config: %s%s%s: %s", section ? section : "", section ? "." : "", key,
         msg);
}

bool cfg_parse_color(const char *s, uint32_t *out) {
  if (*s == '#')
    ++s;
  size_t n = strlen(s);
  if (n != 6 && n != 8)
    return false;
  for (size_t i = 0; i < n; ++i)
    if (!strchr("0123456789abcdefABCDEF", s[i]))
      return false;
  unsigned long v = strtoul(s, NULL, 16);
  *out = n == 6 ? (uint32_t)(v << 8 | 0xff) : (uint32_t)v;
  return true;
}

static bool in_list(const char *key, const char *const *list) {
  for (int i = 0; list && list[i]; ++i)
    if (strcmp(key, list[i]) == 0)
      return true;
  return false;
}

static void apply_one(toml_datum_t v, const char *sec,
                      const struct cfg_field *f, void *dst) {
  char *p = (char *)dst + f->offset;
  switch (f->type) {
  case CFG_T_INT: {
    double x;
    if (v.type == TOML_INT64)
      x = (double)v.u.int64;
    else if (v.type == TOML_FP64 && v.u.fp64 == floor(v.u.fp64))
      x = v.u.fp64;
    else {
      cfg_error(v, sec, f->key, "expected an integer");
      return;
    }
    if (x < f->min || x > f->max) {
      cfg_error(v, sec, f->key, "%g is out of range [%g, %g]", x, f->min,
                f->max);
      return;
    }
    *(int *)p = (int)x;
    break;
  }
  case CFG_T_DOUBLE: {
    double x;
    if (v.type == TOML_FP64)
      x = v.u.fp64;
    else if (v.type == TOML_INT64)
      x = (double)v.u.int64;
    else {
      cfg_error(v, sec, f->key, "expected a number");
      return;
    }
    if (isnan(x) || x < f->min || x > f->max) {
      cfg_error(v, sec, f->key, "%g is out of range [%g, %g]", x, f->min,
                f->max);
      return;
    }
    *(double *)p = x;
    break;
  }
  case CFG_T_BOOL:
    if (v.type != TOML_BOOLEAN) {
      cfg_error(v, sec, f->key, "expected true or false");
      return;
    }
    *(bool *)p = v.u.boolean;
    break;
  case CFG_T_STRING:
    if (v.type != TOML_STRING) {
      cfg_error(v, sec, f->key, "expected a string");
      return;
    }
    if ((size_t)v.u.str.len >= f->size) {
      cfg_error(v, sec, f->key, "longer than %zu bytes", f->size - 1);
      return;
    }
    memcpy(p, v.u.str.ptr, v.u.str.len + 1);
    break;
  case CFG_T_COLOR: {
    uint32_t c;
    if (v.type != TOML_STRING || !cfg_parse_color(v.u.s, &c)) {
      cfg_error(v, sec, f->key, "expected a colour like \"#rrggbb\" or "
                                "\"#rrggbbaa\"");
      return;
    }
    *(uint32_t *)p = c;
    break;
  }
  }
}

void cfg_apply(toml_datum_t section, const char *section_name,
               const struct cfg_field *fields, size_t nfields, void *dst,
               const char *const *extra_keys) {
  if (section.type != TOML_TABLE)
    return;
  for (int i = 0; i < section.u.tab.size; ++i) {
    const char *key = section.u.tab.key[i];
    toml_datum_t v = section.u.tab.value[i];
    const struct cfg_field *f = NULL;
    for (size_t j = 0; j < nfields; ++j)
      if (strcmp(fields[j].key, key) == 0)
        f = &fields[j];
    if (f)
      apply_one(v, section_name, f, dst);
    else if (!in_list(key, extra_keys))
      cfg_error(v, section_name, key, "unknown key (ignored)");
  }
}

void cfg_dump_string(FILE *f, const char *s) {
  fputc('"', f);
  for (; *s; ++s) {
    unsigned char c = (unsigned char)*s;
    switch (c) {
    case '"':
      fputs("\\\"", f);
      break;
    case '\\':
      fputs("\\\\", f);
      break;
    case '\n':
      fputs("\\n", f);
      break;
    case '\t':
      fputs("\\t", f);
      break;
    case '\r':
      fputs("\\r", f);
      break;
    case '\b':
      fputs("\\b", f);
      break;
    case '\f':
      fputs("\\f", f);
      break;
    default:
      if (c < 0x20 || c == 0x7f)
        fprintf(f, "\\u%04x", c);
      else
        fputc(c, f);
    }
  }
  fputc('"', f);
}

void cfg_dump(FILE *f, const struct cfg_field *fields, size_t nfields,
              const void *src) {
  for (size_t i = 0; i < nfields; ++i) {
    const struct cfg_field *fl = &fields[i];
    const char *p = (const char *)src + fl->offset;
    if (fl->doc && *fl->doc)
      fprintf(f, "# %s\n", fl->doc);
    fprintf(f, "%s = ", fl->key);
    switch (fl->type) {
    case CFG_T_INT:
      fprintf(f, "%d\n", *(const int *)p);
      break;
    case CFG_T_DOUBLE: {
      double d = *(const double *)p;
      if (d == floor(d) && fabs(d) < 1e15)
        fprintf(f, "%.1f\n", d);
      else
        fprintf(f, "%g\n", d);
      break;
    }
    case CFG_T_BOOL:
      fprintf(f, "%s\n", *(const bool *)p ? "true" : "false");
      break;
    case CFG_T_STRING:
      cfg_dump_string(f, p);
      fputc('\n', f);
      break;
    case CFG_T_COLOR: {
      uint32_t c = *(const uint32_t *)p;
      if ((c & 0xff) == 0xff)
        fprintf(f, "\"#%06x\"\n", c >> 8);
      else
        fprintf(f, "\"#%08x\"\n", c);
      break;
    }
    }
  }
}
