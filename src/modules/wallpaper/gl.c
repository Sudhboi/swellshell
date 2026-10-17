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
  /* the topo map */
  GLuint map_prog, map_tex;
  unsigned map_grid; /* the grid in map_tex */
  GLint m_heights, m_gsize, m_cell, m_origin, m_step, m_bh, m_levels, m_every,
      m_lw, m_iw, m_line0, m_line1, m_line2, m_bg0, m_bg1, m_bright, m_axis0,
      m_axis, m_rings, m_nrings, m_centre, m_ring_px, m_ring_w, m_sway, m_time,
      m_k, m_gorigin, m_shake, m_spec, m_spec_on, m_logo_r, m_spec_px,
      m_spec_reach, m_ripple_px, m_ripple_span, m_light, m_light_w;
  /* the topo snowflake */
  GLuint logo_prog, logo_tex[3]; /* distance field, shadow, glow */
  unsigned logo_ids[3];          /* what's in them */
  GLint l_sdf, l_shadow, l_neon, l_origin, l_step, l_bh, l_centre, l_rot,
      l_inv_s, l_scale, l_sdf_org, l_sdf_size, l_sh_org, l_sh_size, l_ne_org,
      l_ne_size, l_sh_off, l_sh_alpha, l_neon_a, l_fill, l_edge0, l_edge1,
      l_edge2, l_edge_alpha, l_hw, l_axis0, l_axis;
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

/* The topo map: the contour lines of the height grid, antialiased by the
 * heights' screen derivative, over the background, dimmed by bright. Rings
 * (centre-relative radius, size) push the lines outwards as they pass; the
 * sway moves them gently. The spectrum (bass first, left then right) bulges
 * them out round the logo by angle (bass at the bottom, each channel on
 * its side), ripples them out over the map by distance, and lights each
 * line by its place along the gradient. q: the pixel in the cairo buffer's
 * coordinates. */
static const char *map_fs_src =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform highp sampler2D heights;\n"
    "uniform ivec2 gsize;\n"
    "uniform float cell, bh, levels, lw, iw, bright, ring_px, ring_w,\n"
    "    sway, time, k, logo_r, spec_px, spec_reach, ripple_px, ripple_span,\n"
    "    light, light_w;\n"
    "uniform int every, nrings;\n"
    "uniform bool spec_on;\n"
    "uniform float spec[2 * " WP_SPEC_STR "];\n"
    "uniform vec2 origin, stp, centre, shake;\n"
    "uniform vec2 axis0, axis;\n"
    "uniform float gorigin;\n"
    "uniform vec4 line0, line1, line2;\n"
    "uniform vec3 bg0, bg1;\n"
    "uniform vec4 rings[" WP_RINGS_STR "];\n"
    "out vec4 color;\n"
    "float at(ivec2 i) {\n"
    "  return texelFetch(heights, clamp(i, ivec2(0), gsize - 1), 0).r;\n"
    "}\n"
    "float height(vec2 p) {\n"
    "  vec2 g = (p - gorigin) / cell;\n"
    "  vec2 f = floor(g);\n"
    "  ivec2 i = ivec2(f);\n"
    "  vec2 t = g - f;\n"
    "  return mix(mix(at(i), at(i + ivec2(1, 0)), t.x),\n"
    "             mix(at(i + ivec2(0, 1)), at(i + ivec2(1, 1)), t.x), t.y);\n"
    "}\n"
    "float band(int ch, float t) {\n"
    "  float x = clamp(t, 0.0, 1.0) * float(" WP_SPEC_STR " - 1);\n"
    "  int i = min(int(x), " WP_SPEC_STR " - 2);\n"
    "  float f = x - float(i);\n"
    "  f = f * f * (3.0 - 2.0 * f);\n"
    "  int o = ch * " WP_SPEC_STR ";\n"
    "  return mix(spec[o + i], spec[o + i + 1], f);\n"
    "}\n"
    "float bands(float t, float side) {\n"
    "  return mix(band(0, t), band(1, t), side);\n"
    "}\n"
    "void main() {\n"
    "  vec2 q = origin + vec2(gl_FragCoord.x, bh - gl_FragCoord.y) * stp -\n"
    "           shake;\n"
    "  vec2 p = q;\n"
    "  for (int r = 0; r < nrings; ++r) {\n"
    "    vec2 d = q - centre;\n"
    "    float l = max(length(d), 1e-3);\n"
    "    float u = (l - rings[r].x) / ring_w;\n"
    "    float fall = exp(-u * u) * (1.0 - smoothstep(1.5, 2.1, abs(u)));\n"
    "    p -= d / l * rings[r].y * ring_px * fall;\n"
    "  }\n"
    "  float x = clamp(dot(q - axis0, axis), 0.0, 1.0);\n"
    "  float lit = 0.0;\n"
    "  if (spec_on) {\n"
    "    vec2 d = q - centre;\n"
    "    float l = max(length(d), 1e-3);\n"
    "    float out_ = max(l - logo_r, 0.0);\n"
    "    float side = smoothstep(-40.0 * k, 40.0 * k, d.x);\n"
    "    float up = atan(abs(d.x), d.y) / 3.14159265;\n"
    "    float push = bands(up, side) * spec_px * exp(-out_ / spec_reach);\n"
    "    float t = out_ / ripple_span;\n"
    "    push += bands(t, side) * ripple_px * (1.0 - smoothstep(0.85, 1.0, "
    "t))\n"
    "            * smoothstep(0.0, 0.08, t);\n"
    "    p -= d / l * push * smoothstep(0.0, logo_r * 0.6, l);\n"
    "    lit = (band(0, x) + band(1, x)) * 0.5;\n"
    "  }\n"
    "  if (sway > 0.0) {\n"
    "    float wl = 420.0 * k;\n"
    "    p += sway * vec2(sin(q.y / wl + time * 1.7),\n"
    "                     sin(q.x / wl * 0.8 + time * 2.3 + 1.0));\n"
    "  }\n"
    "  float v = height(p) * levels - 0.5;\n"
    "  float fw = max(length(vec2(dFdx(v), dFdy(v))), 1e-5);\n"
    "  float li = floor(v + 0.5);\n"
    "  float dist = abs(v - li) / fw;\n"
    "  bool index = every > 0 && int(li) % every == every / 2;\n"
    "  float w = lw * (index ? iw : 1.0) * (1.0 + light_w * lit);\n"
    "  float cov = li < 0.0 || li >= levels ? 0.0\n"
    "            : clamp(w * 0.5 - dist + 0.5, 0.0, 1.0);\n"
    "  vec4 lc = x < 0.5 ? mix(line0, line1, x * 2.0)\n"
    "                    : mix(line1, line2, x * 2.0 - 1.0);\n"
    "  float a = min(lc.a * (1.0 + light * lit), 1.0);\n"
    "  vec3 c = mix(mix(bg0, bg1, x), lc.rgb, cov * a);\n"
    "  color = vec4(c * bright, 1.0);\n"
    "}\n";

/* The glass snowflake (gl.c's twin of render.c's draw_glass_logo() for
 * whole lambdas), premultiplied over transparent: the shadow's mask down
 * and right, the glow's mask added as light (alpha 0: the compositor's
 * OVER adds it, like cairo's ADD), the glass filled where the distance
 * field is negative and the outline stroked where it is near 0, both in
 * screen px (the field times the pulse). The masks and the field are in
 * the resting snowflake's px; p is the pixel there, turned back and
 * shrunk by the pulse. The outline's gradient stays put on the screen. */
static const char *logo_fs_src =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform highp sampler2D sdf, shadow, neon;\n"
    "uniform vec2 origin, stp, centre, rot, sdf_org, sdf_size, sh_org,\n"
    "    sh_size, ne_org, ne_size, sh_off, axis0, axis;\n"
    "uniform float bh, inv_s, scale, sh_alpha, neon_a, edge_alpha, hw;\n"
    "uniform vec4 fill, edge0, edge1, edge2;\n"
    "out vec4 color;\n"
    "vec2 back(vec2 d) {\n"
    "  return vec2(rot.x * d.x + rot.y * d.y, rot.x * d.y - rot.y * d.x) *\n"
    "         inv_s;\n"
    "}\n"
    "void main() {\n"
    "  vec2 q = origin + vec2(gl_FragCoord.x, bh - gl_FragCoord.y) * stp;\n"
    "  vec2 d0 = q - centre;\n"
    "  vec4 o = vec4(0.0);\n"
    "  if (sh_alpha > 0.0)\n"
    "    o.a = sh_alpha *\n"
    "          texture(shadow, (back(d0 - sh_off) - sh_org) / sh_size).r;\n"
    "  float x = clamp(dot(q - axis0, axis), 0.0, 1.0);\n"
    "  vec4 ec = x < 0.5 ? mix(edge0, edge1, x * 2.0)\n"
    "                    : mix(edge1, edge2, x * 2.0 - 1.0);\n"
    "  vec2 p = back(d0);\n"
    "  if (neon_a > 0.0)\n"
    "    o.rgb += ec.rgb * neon_a *\n"
    "             texture(neon, (p - ne_org) / ne_size).r;\n"
    "  float d = texture(sdf, (p - sdf_org) / sdf_size).r * scale;\n"
    "  float fa = fill.a * clamp(0.5 - d, 0.0, 1.0);\n"
    "  o = vec4(fill.rgb * fa, fa) + o * (1.0 - fa);\n"
    "  if (hw > 0.0) {\n"
    "    float ea = ec.a * edge_alpha * clamp(hw - abs(d) + 0.5, 0.0, 1.0);\n"
    "    o = vec4(ec.rgb * ea, ea) + o * (1.0 - ea);\n"
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

  GLuint mvs = compile(GL_VERTEX_SHADER, vs_src);
  GLuint mfs = compile(GL_FRAGMENT_SHADER, map_fs_src);
  if (!mvs || !mfs)
    return false;
  g.map_prog = glCreateProgram();
  glAttachShader(g.map_prog, mvs);
  glAttachShader(g.map_prog, mfs);
  glLinkProgram(g.map_prog);
  glDeleteShader(mvs);
  glDeleteShader(mfs);
  glGetProgramiv(g.map_prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    warn("wallpaper: map shader link failed");
    return false;
  }
#define M(n) g.m_##n = glGetUniformLocation(g.map_prog, #n)
  M(heights), M(gsize), M(cell), M(origin), M(bh), M(levels);
  M(every), M(lw), M(iw), M(line0), M(line1), M(line2), M(bg0), M(bg1);
  M(bright), M(rings), M(nrings), M(centre), M(ring_px), M(ring_w);
  M(sway), M(time), M(k), M(gorigin), M(shake), M(axis0), M(axis);
  M(spec), M(spec_on), M(logo_r), M(spec_px), M(spec_reach), M(ripple_px);
  M(ripple_span), M(light), M(light_w);
#undef M
  g.m_step = glGetUniformLocation(g.map_prog, "stp");

  GLuint lvs = compile(GL_VERTEX_SHADER, vs_src);
  GLuint lfs = compile(GL_FRAGMENT_SHADER, logo_fs_src);
  if (!lvs || !lfs)
    return false;
  g.logo_prog = glCreateProgram();
  glAttachShader(g.logo_prog, lvs);
  glAttachShader(g.logo_prog, lfs);
  glLinkProgram(g.logo_prog);
  glDeleteShader(lvs);
  glDeleteShader(lfs);
  glGetProgramiv(g.logo_prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    warn("wallpaper: logo shader link failed");
    return false;
  }
#define L(n) g.l_##n = glGetUniformLocation(g.logo_prog, #n)
  L(sdf), L(shadow), L(neon), L(origin), L(bh), L(centre), L(rot), L(inv_s);
  L(scale), L(sdf_org), L(sdf_size), L(sh_org), L(sh_size), L(ne_org);
  L(ne_size), L(sh_off), L(sh_alpha), L(neon_a), L(fill), L(edge0), L(edge1);
  L(edge2), L(edge_alpha), L(hw), L(axis0), L(axis);
#undef L
  g.l_step = glGetUniformLocation(g.logo_prog, "stp");
  glGenTextures(3, g.logo_tex);
  for (int i = 0; i < 3; ++i) {
    /* The shadow as cairo paints it: the nearest texel. */
    GLint filter = i == 1 ? GL_NEAREST : GL_LINEAR;
    glBindTexture(GL_TEXTURE_2D, g.logo_tex[i]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  glGenTextures(1, &g.map_tex);
  glBindTexture(GL_TEXTURE_2D, g.map_tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
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
      if (g.map_tex)
        glDeleteTextures(1, &g.map_tex);
      if (g.map_prog)
        glDeleteProgram(g.map_prog);
      if (g.logo_tex[0])
        glDeleteTextures(3, g.logo_tex);
      if (g.logo_prog)
        glDeleteProgram(g.logo_prog);
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

static void set_color(GLint u, uint32_t c, double a) {
  glUniform4f(u, col_r(c), col_g(c), col_b(c), col_a(c) * a);
}

void wp_gl_map_draw(const struct wallpaper_config *c, int pw, int ph,
                    const struct wp_frame *f, double ox, double oy, double sx,
                    double sy, int bw, int bh) {
  glViewport(0, 0, bw, bh);
  glDisable(GL_BLEND);
  const struct wp_topo_grid *grid = wp_topo_heights(c, pw, ph);
  glUseProgram(g.map_prog);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g.map_tex);
  if (g.map_grid != grid->id) {
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, grid->nx, grid->ny, 0, GL_RED,
                 GL_FLOAT, grid->h);
    g.map_grid = grid->id;
  }
  double k = pw / WP_REF_WIDTH;
  glUniform1i(g.m_heights, 0);
  glUniform2i(g.m_gsize, grid->nx, grid->ny);
  glUniform1f(g.m_cell, grid->cell);
  glUniform1f(g.m_gorigin, grid->origin);
  /* As cairo: shaken by whole pixels. */
  glUniform2f(g.m_shake, round(f->shake_x * k), round(f->shake_y * k));
  glUniform2f(g.m_origin, ox, oy);
  glUniform2f(g.m_step, sx, sy);
  glUniform1f(g.m_bh, bh);
  glUniform1f(g.m_k, k);
  bool lines = c->topo_line_width > 0 && c->topo_line_alpha > 0;
  glUniform1f(g.m_levels, lines ? c->topo_levels : 0);
  glUniform1i(g.m_every, c->topo_index_every);
  glUniform1f(g.m_lw, c->topo_line_width * k);
  glUniform1f(g.m_iw, c->topo_index_width);
  set_color(g.m_line0, c->topo_line_left, c->topo_line_alpha);
  set_color(g.m_line1, c->topo_line_mid, c->topo_line_alpha);
  set_color(g.m_line2, c->topo_line_right, c->topo_line_alpha);
  glUniform3f(g.m_bg0, col_r(c->topo_bg_left), col_g(c->topo_bg_left),
              col_b(c->topo_bg_left));
  glUniform3f(g.m_bg1, col_r(c->topo_bg_right), col_g(c->topo_bg_right),
              col_b(c->topo_bg_right));
  /* The gradient's position along its axis: dot(q - axis0, axis). */
  double x0, y0, x1, y1;
  wp_topo_axis(c, pw, ph, &x0, &y0, &x1, &y1);
  double len2 = (x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0);
  glUniform2f(g.m_axis0, x0, y0);
  glUniform2f(g.m_axis, (x1 - x0) / len2, (y1 - y0) / len2);
  glUniform1f(g.m_bright, wp_map_brightness(f));
  float rings[WP_RINGS][4] = {{0}};
  int n = f->nrings < WP_RINGS ? f->nrings : WP_RINGS;
  for (int i = 0; i < n; ++i) {
    rings[i][0] = (float)(f->rings[i].radius * k);
    rings[i][1] = (float)f->rings[i].size;
  }
  glUniform4fv(g.m_rings, WP_RINGS, rings[0]);
  glUniform1i(g.m_nrings, n);
  glUniform2f(g.m_centre, pw / 2.0, ph / 2.0);
  glUniform1f(g.m_ring_px, c->topo_ring_px * k);
  glUniform1f(g.m_ring_w, c->topo_ring_width * k);
  glUniform1f(g.m_sway, f->sway * c->topo_sway_px * k);
  glUniform1f(g.m_time, f->time);
  glUniform1i(g.m_spec_on, f->spec_on);
  glUniform1fv(g.m_spec, 2 * WP_SPEC, f->spec[0]);
  glUniform1f(g.m_logo_r, c->logo_radius * k);
  glUniform1f(g.m_spec_px, c->topo_spectrum_px * k);
  glUniform1f(g.m_spec_reach, c->topo_spectrum_reach * k);
  glUniform1f(g.m_ripple_px, c->topo_ripple_px * k);
  glUniform1f(g.m_ripple_span, c->topo_ripple_span * k);
  glUniform1f(g.m_light, c->topo_light);
  glUniform1f(g.m_light_w, c->topo_light_width);
  glDrawArrays(GL_TRIANGLES, 0, 3);
}

bool wp_gl_map(struct wp_gl_win *w, const struct wallpaper_config *c, int pw,
               int ph, const struct wp_frame *f, double ox, double oy,
               double sx, double sy, int bw, int bh) {
  if (!eglMakeCurrent(g.dpy, w->surf, w->surf, g.ctx))
    return false;
  wp_gl_map_draw(c, pw, ph, f, ox, oy, sx, sy, bw, bh);
  return eglSwapBuffers(g.dpy, w->surf);
}

/* Upload mask which (0 shadow, 1 glow) into logo_tex[1 + which] if it
 * changed and set its uniforms; false if that part is off. */
static bool logo_mask(const struct wallpaper_config *c, int pw, int which,
                      GLint u_org, GLint u_size) {
  struct wp_logo_mask m;
  if (!wp_logo_mask(c, pw, which, &m))
    return false;
  glActiveTexture(GL_TEXTURE1 + which);
  glBindTexture(GL_TEXTURE_2D, g.logo_tex[1 + which]);
  if (g.logo_ids[1 + which] != m.id) {
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, m.stride);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, m.w, m.h, 0, GL_RED,
                 GL_UNSIGNED_BYTE, m.data);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    g.logo_ids[1 + which] = m.id;
  }
  glUniform2f(u_org, m.x, m.y);
  glUniform2f(u_size, m.w, m.h);
  return true;
}

void wp_gl_logo_draw(const struct wallpaper_config *c, int pw, int ph,
                     const struct wp_frame *f, double ox, double oy,
                     double sx, double sy, int bw, int bh) {
  glViewport(0, 0, bw, bh);
  glDisable(GL_BLEND);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);
  double k = pw / WP_REF_WIDTH, r0 = c->logo_radius * k;
  double s = 1 + c->react_pulse_scale * f->pulse;
  if (r0 <= 0 || s <= 0 || !wp_logo_whole(f) || f->fade >= 1)
    return;
  const struct wp_logo_sdf *sdf = wp_logo_sdf(c, pw);
  glUseProgram(g.logo_prog);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g.logo_tex[0]);
  if (g.logo_ids[0] != sdf->id) {
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, sdf->w, sdf->h, 0, GL_RED,
                 GL_FLOAT, sdf->d);
    g.logo_ids[0] = sdf->id;
  }
  glUniform1i(g.l_sdf, 0);
  glUniform1i(g.l_shadow, 1);
  glUniform1i(g.l_neon, 2);
  glUniform2f(g.l_sdf_org, sdf->x, sdf->y);
  glUniform2f(g.l_sdf_size, sdf->w, sdf->h);
  glUniform2f(g.l_origin, ox, oy);
  glUniform2f(g.l_step, sx, sy);
  glUniform1f(g.l_bh, bh);
  /* As cairo: shaken by whole pixels. */
  double dx, dy;
  wp_logo_offset(c, pw, ph, f, &dx, &dy);
  double cx = pw / 2.0 + dx + round(f->shake_x * k);
  double cy = ph / 2.0 + dy + round(f->shake_y * k);
  glUniform2f(g.l_centre, cx, cy);
  glUniform2f(g.l_rot, cos(f->angle), sin(f->angle));
  glUniform1f(g.l_inv_s, 1 / s);
  glUniform1f(g.l_scale, s);
  double sd = c->topo_shadow_px * k * s;
  bool shadow = logo_mask(c, pw, 0, g.l_sh_org, g.l_sh_size);
  glUniform2f(g.l_sh_off, 0.7 * sd, 0.7 * sd);
  glUniform1f(g.l_sh_alpha, shadow ? c->topo_shadow_alpha : 0);
  struct wp_glass look;
  wp_glass_look(c, f, k, &look);
  bool neon = look.neon > 0 && logo_mask(c, pw, 1, g.l_ne_org, g.l_ne_size);
  glUniform1f(g.l_neon_a, neon ? look.neon : 0);
  set_color(g.l_fill, look.fill, 1);
  set_color(g.l_edge0, look.edge[0], 1);
  set_color(g.l_edge1, look.edge[1], 1);
  set_color(g.l_edge2, look.edge[2], 1);
  glUniform1f(g.l_edge_alpha, look.edge_alpha);
  glUniform1f(g.l_hw, look.edge_width * s / 2);
  /* The outline's gradient, top left to bottom right over the lambdas'
   * extent: dot(q - axis0, axis) from 0 to 1. */
  double e = 0.6 * r0 * s;
  glUniform2f(g.l_axis0, cx - e, cy - e);
  glUniform2f(g.l_axis, 1 / (4 * e), 1 / (4 * e));
  glDrawArrays(GL_TRIANGLES, 0, 3);
}

bool wp_gl_logo(struct wp_gl_win *w, const struct wallpaper_config *c, int pw,
                int ph, const struct wp_frame *f, double ox, double oy,
                double sx, double sy, int bw, int bh) {
  if (!eglMakeCurrent(g.dpy, w->surf, w->surf, g.ctx))
    return false;
  wp_gl_logo_draw(c, pw, ph, f, ox, oy, sx, sy, bw, bh);
  return eglSwapBuffers(g.dpy, w->surf);
}
