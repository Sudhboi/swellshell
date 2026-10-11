/* The smooth edge visualiser drawn by the GPU (EGL + GLES 3). The CPU traces
 * wp_edge_curve() once per buffer row into a small texture; a fragment shader
 * fills the fade and the rim per pixel from it. Drawing the same shape with
 * cairo cost ~6 ms a frame at 2880x1800, plus sway's upload of the strips.
 * Each strip is drawn into its own subsurface (surface.c), transparent where
 * the shape isn't, so the cairo buffer underneath never holds the bars. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <wayland-egl.h>

#include "config.h"
#include "util.h"
#include "wallpaper.h"

static struct {
  bool tried, ok;
  EGLDisplay dpy;
  EGLConfig cfg;
  EGLContext ctx;
  GLuint prog, tex;
  GLint u_rows, u_nrows, u_ybase, u_origin, u_step, u_shake, u_bh, u_side_x,
      u_dir, u_len, u_fade, u_rw, u_fill, u_rim;
  float *rows;
  int rows_cap;
} g;

static const char *vs_src = "#version 300 es\n"
                            "void main() {\n"
                            "  vec2 p = vec2(float((gl_VertexID << 1) & 2),\n"
                            "                float(gl_VertexID & 2));\n"
                            "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
                            "}\n";

/* q: the pixel in the cairo buffer's coordinates, without the shake.
 * u: how far in from the screen's side. Row r of the texture holds the
 * curve's level at buffer y ybase + r + 0.5 (-1: no curve there) and the
 * lowest and highest level of rows r - 3 .. r + 4; between row centres the
 * curve is linear, and d is the distance to that polyline. Pixels clear of
 * the curve and its rim by those bounds skip the search for d. */
static const char *fs_src =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform highp sampler2D rows;\n"
    "uniform int nrows;\n"
    "uniform float ybase, bh, side_x, dir, len, fade, rw;\n"
    "uniform vec2 origin, stp, shake;\n"
    "uniform vec4 fill, rim;\n"
    "out vec4 color;\n"
    "vec4 row(int r) {\n"
    "  return r < 0 || r >= nrows ? vec4(-1.0)\n"
    "                             : texelFetch(rows, ivec2(r, 0), 0);\n"
    "}\n"
    "float lev(int r) { return row(r).r; }\n"
    "float seg(vec2 p, vec2 a, vec2 b) {\n"
    "  vec2 ab = b - a;\n"
    "  float t = clamp(dot(p - a, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);\n"
    "  return length(p - a - ab * t);\n"
    "}\n"
    "void main() {\n"
    "  vec2 q = origin + vec2(gl_FragCoord.x, bh - gl_FragCoord.y) * stp - shake;\n"
    "  float u = (q.x - side_x) * dir;\n"
    "  vec2 p = vec2(u, q.y - ybase);\n"
    "  int r0 = int(floor(p.y - 0.5));\n"
    "  vec4 t = row(r0);\n"
    "  float clear = rw * 0.5 + 1.0;\n"
    "  if (t.r < 0.0 || u > t.b + clear) {\n"
    "    color = vec4(0.0);\n"
    "    return;\n"
    "  }\n"
    "  if (t.g >= 0.0 && u < t.g - clear) {\n"
    "    float al = fill.a * mix(1.0, fade, clamp(u / len, 0.0, 1.0)) *\n"
    "               clamp(u + 0.5, 0.0, 1.0);\n"
    "    color = vec4(fill.rgb * al, al);\n"
    "    return;\n"
    "  }\n"
    "  float d = 1e9;\n"
    "  for (int k = -3; k <= 3; ++k) {\n"
    "    float a = lev(r0 + k), b = lev(r0 + k + 1);\n"
    "    if (a >= 0.0 && b >= 0.0)\n"
    "      d = min(d, seg(p, vec2(a, float(r0 + k) + 0.5),\n"
    "                        vec2(b, float(r0 + k) + 1.5)));\n"
    "  }\n"
    "  vec4 o = vec4(0.0);\n"
    "  float a = t.r, b = lev(r0 + 1);\n"
    "  if (a >= 0.0 && b >= 0.0) {\n"
    "    float l = mix(a, b, p.y - 0.5 - float(r0));\n"
    "    float cov = clamp((l >= u ? d : -d) + 0.5, 0.0, 1.0) *\n"
    "                clamp(u + 0.5, 0.0, 1.0);\n"
    "    float al = fill.a * mix(1.0, fade, clamp(u / len, 0.0, 1.0)) * cov;\n"
    "    o = vec4(fill.rgb * al, al);\n"
    "  }\n"
    "  if (rw > 0.0 && d < 1e8) {\n"
    "    float cov = clamp(rw * 0.5 - d + 0.5, 0.0, 1.0) *\n"
    "                clamp(u - rw + 0.5, 0.0, 1.0) *\n"
    "                clamp(len + rw - u + 0.5, 0.0, 1.0);\n"
    "    float al = rim.a * cov;\n"
    "    o = vec4(rim.rgb * al, al) + o * (1.0 - al);\n"
    "  }\n"
    "  color = o;\n"
    "}\n";

static GLuint compile(GLenum type, const char *src) {
  GLuint sh = glCreateShader(type);
  glShaderSource(sh, 1, &src, NULL);
  glCompileShader(sh);
  GLint ok = 0;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[1024];
    glGetShaderInfoLog(sh, sizeof(log), NULL, log);
    warn("wallpaper: shader: %s", log);
    glDeleteShader(sh);
    return 0;
  }
  return sh;
}

static bool build_program(void) {
  GLuint vs = compile(GL_VERTEX_SHADER, vs_src);
  GLuint fs = compile(GL_FRAGMENT_SHADER, fs_src);
  if (!vs || !fs)
    return false;
  g.prog = glCreateProgram();
  glAttachShader(g.prog, vs);
  glAttachShader(g.prog, fs);
  glLinkProgram(g.prog);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = 0;
  glGetProgramiv(g.prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    warn("wallpaper: shader link failed");
    return false;
  }
#define U(n) g.u_##n = glGetUniformLocation(g.prog, #n)
  U(rows), U(nrows), U(ybase), U(bh), U(side_x), U(dir), U(len), U(fade);
  U(rw), U(fill), U(rim), U(shake), U(origin);
#undef U
  g.u_step = glGetUniformLocation(g.prog, "stp");
  glGenTextures(1, &g.tex);
  glBindTexture(GL_TEXTURE_2D, g.tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  return true;
}

bool wp_gl_init(struct wl_display *display) {
  if (g.tried)
    return g.ok;
  g.tried = true;
  g.dpy = eglGetPlatformDisplay(display ? EGL_PLATFORM_WAYLAND_KHR
                                        : EGL_PLATFORM_SURFACELESS_MESA,
                                display ? (void *)display : EGL_DEFAULT_DISPLAY,
                                NULL);
  if (g.dpy == EGL_NO_DISPLAY || !eglInitialize(g.dpy, NULL, NULL)) {
    warn("wallpaper: no EGL display; the visualiser stays on the CPU");
    g.dpy = EGL_NO_DISPLAY;
    return false;
  }
  const EGLint attrs[] = {EGL_SURFACE_TYPE,
                          display ? EGL_WINDOW_BIT : EGL_PBUFFER_BIT,
                          EGL_RENDERABLE_TYPE,
                          EGL_OPENGL_ES3_BIT,
                          EGL_RED_SIZE,
                          8,
                          EGL_GREEN_SIZE,
                          8,
                          EGL_BLUE_SIZE,
                          8,
                          EGL_ALPHA_SIZE,
                          8,
                          EGL_NONE};
  const EGLint ctx_attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
  EGLint n = 0;
  if (!eglBindAPI(EGL_OPENGL_ES_API) ||
      !eglChooseConfig(g.dpy, attrs, &g.cfg, 1, &n) || n < 1 ||
      (g.ctx = eglCreateContext(g.dpy, g.cfg, EGL_NO_CONTEXT, ctx_attrs)) ==
          EGL_NO_CONTEXT ||
      !eglMakeCurrent(g.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g.ctx) ||
      !build_program()) {
    warn("wallpaper: no GLES 3 context; the visualiser stays on the CPU");
    wp_gl_finish();
    g.tried = true;
    return false;
  }
  /* Mesa's software rasterisers run the shader on the CPU, slower than
   * cairo: they're no GPU. Tests (display NULL) may use them. */
  const char *renderer = (const char *)glGetString(GL_RENDERER);
  if (display && renderer &&
      (strstr(renderer, "llvmpipe") || strstr(renderer, "softpipe") ||
       strstr(renderer, "swrast"))) {
    warn("wallpaper: GL renderer is %s; the visualiser stays on the CPU",
         renderer);
    wp_gl_finish();
    g.tried = true;
    return false;
  }
  g.ok = true;
  return true;
}

void wp_gl_finish(void) {
  if (g.dpy != EGL_NO_DISPLAY && g.dpy) {
    if (g.ctx != EGL_NO_CONTEXT && g.ctx) {
      eglMakeCurrent(g.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g.ctx);
      if (g.tex)
        glDeleteTextures(1, &g.tex);
      if (g.prog)
        glDeleteProgram(g.prog);
      eglMakeCurrent(g.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      eglDestroyContext(g.dpy, g.ctx);
    }
    eglTerminate(g.dpy);
  }
  free(g.rows);
  g = (typeof(g)){0};
}

bool wp_gl_current(void) {
  return g.ok && eglMakeCurrent(g.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g.ctx);
}

/* ---- windows ---- */

struct wp_gl_win {
  struct wl_egl_window *native;
  EGLSurface surf;
};

struct wp_gl_win *wp_gl_win_create(struct wl_surface *surface, int bw,
                                   int bh) {
  if (!g.ok)
    return NULL;
  struct wp_gl_win *w = xcalloc(1, sizeof(*w));
  w->native = wl_egl_window_create(surface, bw, bh);
  if (w->native)
    w->surf = eglCreatePlatformWindowSurface(g.dpy, g.cfg, w->native, NULL);
  if (!w->native || w->surf == EGL_NO_SURFACE ||
      !eglMakeCurrent(g.dpy, w->surf, w->surf, g.ctx)) {
    warn("wallpaper: no EGL window surface");
    wp_gl_win_destroy(w);
    return NULL;
  }
  /* The strip is a synchronised subsurface: it shows on the parent's
   * commit, and frame callbacks pace the parent, so never wait here. */
  eglSwapInterval(g.dpy, 0);
  return w;
}

void wp_gl_win_resize(struct wp_gl_win *w, int bw, int bh) {
  wl_egl_window_resize(w->native, bw, bh, 0, 0);
}

void wp_gl_win_destroy(struct wp_gl_win *w) {
  if (!w)
    return;
  if (w->surf != EGL_NO_SURFACE && w->surf) {
    eglMakeCurrent(g.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g.ctx);
    eglDestroySurface(g.dpy, w->surf);
  }
  if (w->native)
    wl_egl_window_destroy(w->native);
  free(w);
}

/* ---- drawing ---- */

/* The curve's level at each row centre ybase + r + 0.5 of n rows, -1 past
 * its ends, at out[4 r]; out[4 r + 1 .. 2] are the lowest and highest of
 * rows r - 3 .. r + 4 (rows past the table count as -1). The Beziers are
 * flattened (y falls monotonically along them). */
static void trace_rows(const struct wp_edge_curve *e, int ybase, int n,
                       float *out) {
  enum { STEPS = 24 };
  int np = (e->m - 1) * STEPS + 1;
  double ly[(WP_VIZ_MAX + 1) * STEPS + 1], ll[(WP_VIZ_MAX + 1) * STEPS + 1];
  /* From the top (the last point) down, so y grows along the list. */
  int k = 0;
  for (int i = e->m - 2; i >= 0; --i)
    for (int j = i == e->m - 2 ? 0 : 1; j <= STEPS; ++j) {
      double t = 1 - (double)j / STEPS, it = 1 - t;
      double b0 = it * it * it, b1 = 3 * it * it * t, b2 = 3 * it * t * t,
             b3 = t * t * t;
      ly[k] = b0 * e->py[i] + b1 * e->cy1[i] + b2 * e->cy2[i] +
              b3 * e->py[i + 1];
      ll[k++] = b0 * e->pl[i] + b1 * e->c1[i] + b2 * e->c2[i] +
                b3 * e->pl[i + 1];
    }
  np = k;
  int p = 0;
  for (int r = 0; r < n; ++r) {
    double y = ybase + r + 0.5;
    if (y < ly[0] || y > ly[np - 1]) {
      out[4 * r] = -1;
      continue;
    }
    while (p + 2 < np && ly[p + 1] < y)
      ++p;
    double dy = ly[p + 1] - ly[p];
    double t = dy > 0 ? (y - ly[p]) / dy : 0;
    out[4 * r] = (float)(ll[p] + (ll[p + 1] - ll[p]) * fmin(fmax(t, 0), 1));
  }
  for (int r = 0; r < n; ++r) {
    float lo = INFINITY, hi = -1;
    for (int j = r - 3; j <= r + 4; ++j) {
      float l = j >= 0 && j < n ? out[4 * j] : -1;
      lo = fminf(lo, l);
      hi = fmaxf(hi, l);
    }
    out[4 * r + 1] = lo;
    out[4 * r + 2] = hi;
    out[4 * r + 3] = 0;
  }
}

void wp_gl_edge_draw(const struct wallpaper_config *c, int pw, int ph,
                     const struct wp_frame *f, int s, double ox, double oy,
                     double sx, double sy, int bw, int bh) {
  glViewport(0, 0, bw, bh);
  glDisable(GL_BLEND);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);
  if (f->nbars <= 0)
    return;
  struct wp_edge_curve e;
  wp_edge_curve(c, pw, ph, f, s, &e);
  int ybase = (int)floor(e.y0) - 4;
  int n = (int)ceil(e.y1) + 4 - ybase;
  if (n > g.rows_cap) {
    free(g.rows);
    g.rows = malloc(4 * n * sizeof(*g.rows));
    g.rows_cap = n;
  }
  trace_rows(&e, ybase, n, g.rows);

  double k = pw / WP_REF_WIDTH;
  uint32_t vc = c->viz_color, rc = c->viz_edge_rim;
  double va = col_a(vc) * c->viz_alpha, ra = col_a(rc) * c->viz_alpha;
  glUseProgram(g.prog);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g.tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, n, 1, 0, GL_RGBA, GL_FLOAT,
               g.rows);
  glUniform1i(g.u_rows, 0);
  glUniform1i(g.u_nrows, n);
  glUniform1f(g.u_ybase, ybase);
  glUniform2f(g.u_origin, ox, oy);
  glUniform2f(g.u_step, sx, sy);
  glUniform1f(g.u_bh, bh);
  /* As cairo: shaken by whole pixels. */
  glUniform2f(g.u_shake, round(f->shake_x * k), round(f->shake_y * k));
  glUniform1f(g.u_side_x, s ? e.x1 : e.x0);
  glUniform1f(g.u_dir, s ? -1 : 1);
  glUniform1f(g.u_len, e.len);
  glUniform1f(g.u_fade, c->viz_edge_fade);
  glUniform1f(g.u_rw, c->viz_edge_rim_width * k);
  glUniform4f(g.u_fill, col_r(vc), col_g(vc), col_b(vc), va);
  glUniform4f(g.u_rim, col_r(rc), col_g(rc), col_b(rc), ra);
  glDrawArrays(GL_TRIANGLES, 0, 3);
}

bool wp_gl_edge(struct wp_gl_win *w, const struct wallpaper_config *c, int pw,
                int ph, const struct wp_frame *f, int s, double ox, double oy,
                double sx, double sy, int bw, int bh) {
  if (!eglMakeCurrent(g.dpy, w->surf, w->surf, g.ctx))
    return false;
  wp_gl_edge_draw(c, pw, ph, f, s, ox, oy, sx, sy, bw, bh);
  return eglSwapBuffers(g.dpy, w->surf);
}
