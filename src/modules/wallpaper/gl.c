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
      m_spec_reach, m_ripple_px, m_ripple_span, m_light, m_light_w, m_gather,
      m_gather_reach, m_hold_front, m_hold_lit, m_hold_band, m_drift, m_drift_l,
      m_s_in, m_s_out, m_s_lit, m_s_lit_all, m_s_levels, m_pokes, m_npokes,
      m_poke_r, m_poke_light;
  /* peek_layout "terrain": the heights again, filtered (mirrored past the
   * grid), and the clock raised out of them */
  GLuint hf_tex, clk_tex;
  unsigned hf_grid, clk_id;
  GLint m_terrain, m_hf, m_clk, m_lsdf, m_cam_o, m_cam_r, m_cam_d, m_cam_f,
      m_cam_c, m_cam_F, m_t_t, m_t_tf, m_t_unit, m_t_zmax, m_t_fog,
      m_t_shade, m_t_light, m_lmax, m_t_sum, m_t_sum_r, m_clk_o, m_clk_r,
      m_clk_u, m_clk_n, m_clk_wh, m_clk_base, m_clk_st, m_clk_a, m_clk_depth,
      m_glass, m_cedge0, m_cedge1, m_cedge2, m_mesa, m_mesa_r, m_mesa_col, m_l_pos, m_l_rot,
      m_l_inv_s, m_l_scale, m_l_on, m_lsdf_org, m_lsdf_size;
  /* the topo snowflake */
  GLuint logo_prog, logo_tex[3]; /* distance field, shadow, glow */
  unsigned logo_ids[3];          /* what's in them */
  GLint l_sdf, l_shadow, l_neon, l_origin, l_step, l_bh, l_centre, l_rot,
      l_inv_s, l_scale, l_sdf_org, l_sdf_size, l_sh_org, l_sh_size, l_ne_org,
      l_ne_size, l_sh_off, l_sh_alpha, l_neon_a, l_fill, l_edge0, l_edge1,
      l_edge2, l_edge_alpha, l_hw, l_axis0, l_axis, l_tilted, l_tm, l_tn,
      l_focal, l_depth, l_layers, l_layer_a, l_c_o, l_c_r, l_c_d, l_c_f,
      l_c_c, l_c_F;
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
 * sway moves them gently, the pointer's wake (pokes: where, how open)
 * parts them, pushing them aside within about poke_r and pressing them
 * together (brighter by poke_light) round it, and at rest they drift
 * slowly (bending a little, by drift_l). The spectrum (bass first, left then right) bulges
 * them out round the logo by angle (bass at the bottom, each channel on
 * its side), ripples them out over the map by distance, and lights each
 * line by its place along the gradient. A held popup key draws them in
 * towards the logo by gather px (out when < 0), less further out, and
 * lights them (as the spectrum does, by hold_lit) outside a front that
 * closes in from the corners to the logo as the hold runs. The topo shows
 * keep lines only inside s_in.x (soft over s_in.y) and outside s_out.x,
 * light a band (s_lit: radius, width, strength) and every line by
 * s_lit_all, and keep only the levels below s_levels.x (lighting the top
 * one by s_levels.y). q: the pixel in the cairo buffer's coordinates. */
static const char *map_fs_src =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform highp sampler2D heights;\n"
    "uniform ivec2 gsize;\n"
    "uniform float cell, bh, levels, lw, iw, bright, ring_px, ring_w,\n"
    "    sway, time, k, logo_r, spec_px, spec_reach, ripple_px, ripple_span,\n"
    "    light, light_w, gather, gather_reach, hold_front, hold_lit,\n"
    "    hold_band, drift_l, s_lit_all, poke_r, poke_light;\n"
    "uniform vec2 s_in, s_out, s_levels;\n"
    "uniform vec3 s_lit;\n"
    "uniform vec3 pokes[" WP_WAKE_STR "];\n"
    "uniform int every, nrings, npokes;\n"
    "uniform bool spec_on;\n"
    "uniform float spec[2 * " WP_SPEC_STR "];\n"
    "uniform vec2 origin, stp, centre, shake, drift;\n"
    "uniform vec2 axis0, axis;\n"
    "uniform float gorigin;\n"
    "uniform vec4 line0, line1, line2;\n"
    "uniform vec3 bg0, bg1;\n"
    "uniform vec4 rings[" WP_RINGS_STR "];\n"
    "uniform int terrain;\n"
    "uniform highp sampler2D hf, clk, lsdf;\n"
    "uniform vec3 cam_o, cam_r, cam_d, cam_f, t_light, t_sum, l_pos;\n"
    "uniform vec2 cam_c, clk_wh, l_rot, lsdf_org,\n"
    "    lsdf_size;\n"
    "uniform float cam_F, t_t, t_tf, t_unit, t_zmax, t_fog, t_shade, lmax,\n"
    "    t_sum_r, mesa_r, l_inv_s, l_scale, l_on, clk_base, clk_st, clk_a,\n"
    "    clk_depth;\n"
    "uniform vec3 clk_o, clk_r, clk_u, clk_n;\n"
    "uniform vec4 glass, cedge0, cedge1, cedge2;\n"
    "uniform vec4 mesa[3], mesa_col[3];\n"
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
    /* The terrain: the map's heights (drifting as the flat map does,
     * mirrored past the grid), the summit under the snowflake and the
     * mesas, each a pad at its floor and a column to its top. Units: the
     * map's 0..1. */
    "float ground(vec2 p) {\n"
    "  if (drift != vec2(0.0))\n"
    "    p -= drift * (1.0 + 0.3 * sin(p.yx / drift_l));\n"
    "  return texture(hf, ((p - gorigin) / cell + 0.5) / vec2(gsize)).r;\n"
    "}\n"
    "float nm;\n" /* (th's) how far p is outside the nearest mesa */
    "float th(vec2 p, out float cm) {\n"
    "  nm = 1e9;\n"
    "  float h = ground(p);\n"
    "  vec2 s = p - t_sum.xy;\n"
    "  h += t_sum.z * exp(-dot(s, s) / (t_sum_r * t_sum_r));\n"
    "  cm = 0.0;\n"
    "  for (int i = 0; i < 3; ++i) {\n"
    "    float r = length(p - mesa[i].xy);\n"
    "    nm = min(nm, r - mesa_r);\n"
    "    if (r >= 1.35 * mesa_r)\n"
    "      continue;\n"
    "    h = mix(h, mesa[i].z,\n"
    "            (1.0 - smoothstep(mesa_r, 1.35 * mesa_r, r)) * t_tf);\n"
    "    h = mix(h, mesa[i].w,\n"
    "            (1.0 - smoothstep(0.72 * mesa_r, mesa_r, r)) * t_tf);\n"
    "  }\n"
    "  return h;\n"
    "}\n"
    /* The clock's sign: where the ray o + d·t meets it (moved off px
     * back), as the mask's uv (its foot at row clk_base, stretched up). */
    "vec2 sign_uv(vec3 o, vec3 d, float off, out float t) {\n"
    "  vec3 c = clk_o - clk_n * off;\n"
    "  float dn = dot(d, clk_n);\n"
    "  t = abs(dn) > 1e-6 ? dot(c - o, clk_n) / dn : -1.0;\n"
    "  vec3 X = o + d * t - c;\n"
    "  return vec2(0.5 + dot(X, clk_r) / clk_wh.x,\n"
    "              (clk_base - dot(X, clk_u) / clk_st) / clk_wh.y);\n"
    "}\n"
    "float sign_at(vec2 uv) {\n"
    "  return uv == clamp(uv, 0.0, 1.0) ? texture(clk, uv).r : 0.0;\n"
    "}\n"
    "vec4 sign_edge(float x) {\n"
    "  return x < 0.5 ? mix(cedge0, cedge1, x * 2.0)\n"
    "                 : mix(cedge1, cedge2, x * 2.0 - 1.0);\n"
    "}\n"
    /* Tipped (peek_layout "terrain"): the eye's ray through q marched to
     * the ground (steps as long as it is above it over the steepest it
     * likely is), then halved in on it, and the lines drawn there. The
     * flat map's pushes (pe - q) fade as the camera goes. */
    "vec4 tshade(vec2 q, vec2 pe, float x, float lit, float pclear, float sl) {\n"
    "  float tfog, wsc, tsh, shd = 0.0, v;\n"
    "  vec3 fill = vec3(0.0), tcol = vec3(0.0);\n"
    "  float tamt = 0.0, xl;\n"

    "  vec3 dir = normalize(cam_r * (q.x - cam_c.x) + cam_d * (q.y - cam_c.y)\n"
    "                       + cam_f * cam_F);\n"
    "  float far = cam_F + 3.5 * t_fog;\n"
    "  bool sky = dir.z > -1e-4;\n"
    "  dir.z = min(dir.z, -1e-4);\n"
    "  float a0 = min(max((cam_o.z - t_zmax) / -dir.z, 0.0), far);\n"
    "  float b0 = min(cam_o.z / -dir.z, far);\n"
    "  float slope = 0.9 / (-dir.z + 1.2 * length(dir.xy));\n"
    "  float st = a0, prev = a0, cm;\n"
    "  for (int i = 0; i < 160; ++i) {\n"
    "    vec3 P = cam_o + dir * st;\n"
    "    float dz = P.z - t_unit * th(P.xy, cm);\n"
    "    if (dz <= 0.0 || st >= b0)\n"
    "      break;\n"
    "    prev = st;\n"
    "    float step = max(dz * slope, 2.5 * k);\n"
    /* the mesas' walls are steeper than slope allows: near them, short
     * enough steps not to pass through */
    "    if (t_tf > 0.0)\n"
    "      step = min(step, max(nm, 0.3 * mesa_r));\n"
    "    st = min(st + step, b0);\n"
    "  }\n"
    "  float lo = prev, hi = st;\n"
    "  float dlo = -1.0, dhi = 1.0;\n"
    "  for (int i = 0; i < 8; ++i) {\n"
    "    float m = 0.5 * (lo + hi);\n"
    "    vec3 P = cam_o + dir * m;\n"
    "    float dz = P.z - t_unit * th(P.xy, cm);\n"
    "    if (dz <= 0.0) {\n"
    "      hi = m;\n"
    "      dhi = dz;\n"
    "    } else {\n"
    "      lo = m;\n"
    "      dlo = dz;\n"
    "    }\n"
    "  }\n"
    /* the last bit by where the line between the two crosses */
    "  if (dlo > 0.0 && dhi < 0.0)\n"
    "    hi = mix(lo, hi, dlo / (dlo - dhi));\n"
    "  vec3 hp = cam_o + dir * hi;\n"
    "  vec2 P2 = hp.xy + (pe - q) * (1.0 - t_t);\n"
    "  float hh = th(P2, cm);\n"
    "  v = hh * levels - 0.5;\n"
    "  xl = clamp(dot(P2 - axis0, axis), 0.0, 1.0);\n"
    "  tfog = exp(-max(hi - cam_F, 0.0) / t_fog);\n"
    "  if (sky || hi >= far)\n"
    "    tfog = 0.0;\n"
    "  wsc = mix(1.0, clamp(cam_F / hi, 0.6, 1.5), t_t);\n"
    "  vec3 n = cross(dFdx(hp), dFdy(hp));\n"
    "  n = length(n) > 0.0 ? normalize(n) : vec3(0.0, 0.0, 1.0);\n"
    "  n *= sign(n.z + 1e-6);\n"
    "  float lam = max(dot(n, t_light), 0.0);\n"
    "  tsh = mix(1.0, 0.4 + 0.75 * lam, t_t);\n"
    /* the snowflake's shadow, its outline's light round it */
    "  if (l_on > 0.0) {\n"
    "    float s = (l_pos.z - hp.z) / t_light.z;\n"
    "    if (s > 0.0) {\n"
    "      vec2 X = hp.xy + t_light.xy * s - l_pos.xy;\n"
    "      vec2 b = vec2(l_rot.x * X.x + l_rot.y * X.y,\n"
    "                    l_rot.x * X.y - l_rot.y * X.x) * l_inv_s;\n"
    "      vec2 uv = (b - lsdf_org) / lsdf_size;\n"
    "      float d = uv == clamp(uv, 0.0, 1.0)\n"
    "                ? texture(lsdf, uv).r * l_scale : 1e4;\n"
    "      float pen = 2.0 * k + 0.012 * s;\n"
    "      shd = l_on * (1.0 - smoothstep(-pen, pen, d));\n"
    "      float u = (d - pen) / (pen + 12.0 * k);\n"
    "      float rim = l_on * exp(-u * u);\n"
    "      lit = max(lit, 0.9 * rim);\n"
    "    }\n"
    "  }\n"
    /* the clock's sign's, falling in front of it */
    "  if (clk_a > 0.0) {\n"
    "    float ts;\n"
    "    float m = sign_at(sign_uv(hp, t_light, 0.0, ts));\n"
    "    if (ts > 0.0)\n"
    "      shd = max(shd, 0.8 * clk_a * smoothstep(0.3, 0.7, m));\n"
    "  }\n"
    /* the mesas: their sides' lines in their colour, lit, their tops
     * filled with it */
    "  for (int i = 0; i < 3; ++i) {\n"
    "    float r = length(P2 - mesa[i].xy);\n"
    "    float side = (1.0 - smoothstep(0.98 * mesa_r, 1.08 * mesa_r, r)) * t_tf;\n"
    "    float top = (1.0 - smoothstep(0.64 * mesa_r, 0.7 * mesa_r, r)) * t_tf;\n"
    "    if (side > tamt) {\n"
    "      tcol = mesa_col[i].rgb;\n"
    "      tamt = side * mesa_col[i].a;\n"
    "    }\n"
    "    lit = max(lit, 0.7 * side * (1.0 - top));\n"
    "    fill += mesa_col[i].rgb * mesa_col[i].a * 0.2 * top;\n"
    "  }\n"
    "  fill += t_shade * lam * t_t * mix(line0.rgb, line2.rgb, xl);\n"

    /* past the grid the heights only mirror: the lines fade out there */
    "  vec2 gx = max(max(vec2(gorigin) - P2, P2 - (vec2(gorigin) + vec2(gsize - 1) * cell)), 0.0);\n"
    "  tfog *= 1.0 - smoothstep(0.0, 360.0 * k, max(gx.x, gx.y));\n"
    "  fill *= (1.0 - shd) * tfog;\n"
    "  float fw = max(length(vec2(dFdx(v), dFdy(v))), 1e-5);\n"
    "  float li = floor(v + 0.5);\n"
    "  float dist = abs(v - li) / fw;\n"
    "  bool index = every > 0 && int(li) % every == every / 2;\n"
    "  float dl = s_levels.x - li;\n"
    "  if (s_levels.y > 0.0 && dl > 0.0)\n"
    "    lit = max(lit, s_levels.y * exp(-(dl - 0.6) * (dl - 0.6) * 2.5));\n"
    "  float w = lw * (index ? iw : 1.0) * (1.0 + light_w * lit) * wsc;\n"
    "  float cov = li < 0.0 || li >= lmax ? 0.0\n"
    "            : clamp(w * 0.5 - dist + 0.5, 0.0, 1.0);\n"
    "  cov *= clamp(dl, 0.0, 1.0) * (1.0 - pclear) *\n"
    "         (1.0 - smoothstep(s_in.x - s_in.y, s_in.x, sl)) *\n"
    "         smoothstep(s_out.x - s_out.y, s_out.x, sl);\n"
    "  cov *= tfog * (1.0 - 0.65 * shd);\n"
    "  vec4 lc = xl < 0.5 ? mix(line0, line1, xl * 2.0)\n"
    "                     : mix(line1, line2, xl * 2.0 - 1.0);\n"
    "  lc.rgb = mix(lc.rgb, tcol, tamt);\n"
    "  float a = min(lc.a * (1.0 + light * lit), 1.0) * tsh;\n"
    "  vec3 c = mix(mix(bg0, bg1, x), lc.rgb, cov * a) + fill;\n"
    /* The clock's sign, where it's nearer than the ground: its thickness
     * as faint outlines behind, then its glass, the contours' levels
     * running across it, and its outline in the snowflake's colours. */
    "  if (clk_a > 0.0) {\n"
    "    for (int i = 3; i >= 1; --i) {\n"
    "      float tl;\n"
    "      vec2 ul = sign_uv(cam_o, dir, clk_depth * float(i) / 3.0, tl);\n"
    "      float m = sign_at(ul);\n"
    "      float e = (1.0 - smoothstep(0.02, 0.09, abs(m - 0.5))) *\n"
    "                step(0.0, tl) * step(tl, hi);\n"
    "      vec4 ec = sign_edge(ul.x);\n"
    "      c = mix(c, ec.rgb, e * ec.a * 0.4 * (1.0 - 0.22 * float(i)) * clk_a);\n"
    "    }\n"
    "    float tp;\n"
    "    vec2 uv = sign_uv(cam_o, dir, 0.0, tp);\n"
    "    float m = sign_at(uv);\n"
    "    float fm = max(fwidth(m), 1e-4);\n"
    "    float vis = clk_a * step(0.0, tp) * step(tp, hi);\n"
    "    float inside = clamp((m - 0.5) / fm + 0.5, 0.0, 1.0) * vis;\n"
    "    float edge = clamp(1.6 * k * wsc - abs(m - 0.5) / fm + 0.5, 0.0, 1.0) * vis;\n"
    "    vec4 ec = sign_edge(uv.x);\n"
    "    float sv = (cam_o.z + dir.z * tp) / max(t_unit, 1e-3) * levels - 0.5;\n"
    "    float sd = abs(sv - floor(sv + 0.5)) / max(fwidth(sv), 1e-5);\n"
    "    float stripe = clamp(lw * wsc * 0.5 - sd + 0.5, 0.0, 1.0);\n"
    "    c = mix(c, glass.rgb, inside * glass.a);\n"
    "    c = mix(c, ec.rgb, inside * stripe * 0.75 * ec.a);\n"
    "    c = mix(c, ec.rgb, edge * ec.a);\n"
    "  }\n"
    "  return vec4(c * bright, 1.0);\n"
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
    /* The wake: each point, oldest first, pulls where the lines are
     * looked up towards itself (open·e^-x², x = distance / poke_r), which
     * spreads them out round it and presses them together further out.
     * The newest is the pointer; the others pull more the further it has
     * gone from them, so each comes in smoothly. Where a point is still
     * open the line that would run through it is cut, leaving a clean
     * hole the others bend round. */
    "  float pclear = 0.0, plit = 0.0;\n"
    "  if (npokes > 0) {\n"
    "    vec2 head = pokes[npokes - 1].xy;\n"
    "    for (int i = 0; i < npokes; ++i) {\n"
    "      vec2 c = pokes[i].xy;\n"
    "      float o = pokes[i].z;\n"
    "      if (i < npokes - 1)\n"
    "        o *= smoothstep(0.0, poke_r * 0.5, length(c - head));\n"
    "      float x = length(p - c) / poke_r;\n"
    "      p += o * exp(-x * x) * (c - p);\n"
    "      pclear = max(pclear, smoothstep(0.6, 0.9, o) *\n"
    "                               (1.0 - smoothstep(0.32, 0.42, x)));\n"
    "      plit = max(plit, o * exp(-4.0 * (x - 1.1) * (x - 1.1)));\n"
    "    }\n"
    "  }\n"
    "  if (gather != 0.0) {\n"
    "    vec2 d = q - centre;\n"
    "    float l = max(length(d), 1e-3);\n"
    "    float out_ = max(l - logo_r, 0.0);\n"
    "    p += d / l * gather * exp(-out_ / gather_reach) *\n"
    "         smoothstep(0.0, logo_r * 0.6, l);\n"
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
    "  if (hold_lit > 0.0)\n"
    "    lit = max(lit, hold_lit * smoothstep(hold_front - hold_band,\n"
    "                                         hold_front, length(q - "
    "centre)));\n"
    "  float sl = length(q - centre);\n"
    "  if (s_lit.z > 0.0) {\n"
    "    float u = (sl - s_lit.x) / s_lit.y;\n"
    "    lit = max(lit, s_lit.z * exp(-u * u));\n"
    "  }\n"
    "  lit = max(lit, max(s_lit_all, poke_light * plit));\n"
    "  if (sway > 0.0) {\n"
    "    float wl = 420.0 * k;\n"
    "    p += sway * vec2(sin(q.y / wl + time * 1.7),\n"
    "                     sin(q.x / wl * 0.8 + time * 2.3 + 1.0));\n"
    "  }\n"
    "  if (terrain == 1) {\n"
    "    color = tshade(q, p, x, lit, pclear, sl);\n"
    "    return;\n"
    "  }\n"
    "  if (drift != vec2(0.0))\n"
    "    p -= drift * (1.0 + 0.3 * sin(q.yx / drift_l));\n"
    "  float v = height(p) * levels - 0.5;\n"
    "  float fw = max(length(vec2(dFdx(v), dFdy(v))), 1e-5);\n"
    "  float li = floor(v + 0.5);\n"
    "  float dist = abs(v - li) / fw;\n"
    "  bool index = every > 0 && int(li) % every == every / 2;\n"
    "  float dl = s_levels.x - li;\n"
    "  if (s_levels.y > 0.0 && dl > 0.0)\n"
    "    lit = max(lit, s_levels.y * exp(-(dl - 0.6) * (dl - 0.6) * 2.5));\n"
    "  float w = lw * (index ? iw : 1.0) * (1.0 + light_w * lit);\n"
    "  float cov = li < 0.0 || li >= levels ? 0.0\n"
    "            : clamp(w * 0.5 - dist + 0.5, 0.0, 1.0);\n"
    "  cov *= clamp(dl, 0.0, 1.0) * (1.0 - pclear) *\n"
    "         (1.0 - smoothstep(s_in.x - s_in.y, s_in.x, sl)) *\n"
    "         smoothstep(s_out.x - s_out.y, s_out.x, sl);\n"
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
 * shrunk by the pulse. The outline's gradient stays put on the screen.
 * Tipped (tilted), each pixel is first taken onto the tipped plane, seen
 * in perspective from focal px in front of it (tm turns the screen's
 * space to the plane's, tn is its normal), and up to layers outlines on
 * planes behind it, depth px at the back, show faintly through it. */
static const char *logo_fs_src =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform highp sampler2D sdf, shadow, neon;\n"
    "uniform vec2 origin, stp, centre, rot, sdf_org, sdf_size, sh_org,\n"
    "    sh_size, ne_org, ne_size, sh_off, axis0, axis;\n"
    "uniform float bh, inv_s, scale, sh_alpha, neon_a, edge_alpha, hw;\n"
    "uniform vec4 fill, edge0, edge1, edge2;\n"
    "uniform int tilted, layers;\n"
    "uniform mat3 tm;\n"
    "uniform vec3 tn;\n"
    "uniform float focal, depth, layer_a;\n"
    "uniform vec3 c_o, c_r, c_d, c_f;\n"
    "uniform vec2 c_c;\n"
    "uniform float c_F;\n"
    "out vec4 color;\n"
    "vec2 back(vec2 d) {\n"
    "  return vec2(rot.x * d.x + rot.y * d.y, rot.x * d.y - rot.y * d.x) *\n"
    "         inv_s;\n"
    "}\n"
    "vec2 onto(vec2 d, float off) {\n"
    "  vec3 r = vec3(d, -focal);\n"
    "  float t = (off - focal * tn.z) / dot(r, tn);\n"
    "  return (tm * (vec3(0.0, 0.0, focal) + r * t)).xy;\n"
    "}\n"
    /* Over the terrain (tilted 2): the eye's ray through q onto the plane
     * off px over the snowflake's, which is level, the eye at c_o from its
     * centre. */
    "vec2 seen(vec2 q, float off) {\n"
    "  vec3 r = c_r * (q.x - c_c.x) + c_d * (q.y - c_c.y) + c_f * c_F;\n"
    "  return c_o.xy + r.xy * ((off - c_o.z) / r.z);\n"
    "}\n"
    "vec2 layer(vec2 q, vec2 d0, float off) {\n"
    "  return tilted == 2 ? seen(q, off) : onto(d0, off);\n"
    "}\n"
    "void main() {\n"
    "  vec2 q = origin + vec2(gl_FragCoord.x, bh - gl_FragCoord.y) * stp;\n"
    "  vec2 d0 = q - centre;\n"
    "  vec2 ds = d0 - sh_off, dp = d0;\n"
    "  if (tilted == 1) {\n"
    "    ds = onto(ds, 0.0);\n"
    "    dp = onto(d0, 0.0);\n"
    "  } else if (tilted == 2) {\n"
    "    dp = seen(q, 0.0);\n"
    "    ds = dp - sh_off;\n"
    "  }\n"
    "  vec4 o = vec4(0.0);\n"
    "  if (sh_alpha > 0.0)\n"
    "    o.a = sh_alpha *\n"
    "          texture(shadow, (back(ds) - sh_org) / sh_size).r;\n"
    "  float x = clamp(dot(q - axis0, axis), 0.0, 1.0);\n"
    "  vec4 ec = x < 0.5 ? mix(edge0, edge1, x * 2.0)\n"
    "                    : mix(edge1, edge2, x * 2.0 - 1.0);\n"
    "  vec2 p = back(dp);\n"
    "  if (neon_a > 0.0)\n"
    "    o.rgb += ec.rgb * neon_a *\n"
    "             texture(neon, (p - ne_org) / ne_size).r;\n"
    "  float d = texture(sdf, (p - sdf_org) / sdf_size).r * scale;\n"
    "  float fa = fill.a * clamp(0.5 - d, 0.0, 1.0);\n"
    "  o = vec4(fill.rgb * fa, fa) + o * (1.0 - fa);\n"
    "  if (tilted >= 1 && layer_a > 0.0 && hw > 0.0) {\n"
    "    float seen = 1.0 - 0.6 * fa / max(fill.a, 0.001);\n"
    "    for (int i = 8; i >= 1; --i) {\n"
    "      if (i > layers)\n"
    "        continue;\n"
    "      float f = float(i) / float(layers);\n"
    "      vec2 pl = back(layer(q, d0, -depth * f));\n"
    "      float dl = texture(sdf, (pl - sdf_org) / sdf_size).r * scale;\n"
    "      float la = layer_a * ec.a * edge_alpha * 0.75 * (1.0 - 0.7 * f) *\n"
    "                 clamp(0.8 * hw - abs(dl) + 0.5, 0.0, 1.0) * seen;\n"
    "      o = vec4(ec.rgb * la, la) + o * (1.0 - la);\n"
    "    }\n"
    "  }\n"
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
  M(ripple_span), M(light), M(light_w), M(gather), M(gather_reach);
  M(hold_front), M(hold_lit), M(hold_band), M(drift), M(drift_l);
  M(s_in), M(s_out), M(s_lit), M(s_lit_all), M(s_levels), M(pokes);
  M(npokes), M(poke_r), M(poke_light), M(terrain), M(hf), M(clk), M(lsdf);
  M(cam_o), M(cam_r), M(cam_d), M(cam_f), M(cam_c), M(cam_F), M(t_t);
  M(t_tf), M(t_unit), M(t_zmax), M(t_fog), M(t_shade), M(t_light), M(lmax);
  M(t_sum), M(t_sum_r), M(clk_o), M(clk_r), M(clk_u), M(clk_n), M(clk_wh);
  M(clk_base), M(clk_st), M(clk_a), M(clk_depth), M(glass), M(cedge0);
  M(cedge1), M(cedge2);
  M(mesa), M(mesa_r), M(mesa_col), M(l_pos), M(l_rot), M(l_inv_s);
  M(l_scale), M(l_on), M(lsdf_org), M(lsdf_size);
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
  L(edge2), L(edge_alpha), L(hw), L(axis0), L(axis), L(tilted), L(tm);
  L(tn), L(focal), L(depth), L(layers), L(layer_a), L(c_o), L(c_r), L(c_d);
  L(c_f), L(c_c), L(c_F);
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
  glGenTextures(1, &g.hf_tex);
  glBindTexture(GL_TEXTURE_2D, g.hf_tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT);
  glGenTextures(1, &g.clk_tex);
  glBindTexture(GL_TEXTURE_2D, g.clk_tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
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
      if (g.hf_tex)
        glDeleteTextures(1, &g.hf_tex);
      if (g.clk_tex)
        glDeleteTextures(1, &g.clk_tex);
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

/* The snowflake's distance field into logo_tex[0] (bound to the active
 * unit) if it isn't there. */
static void bind_sdf(const struct wp_logo_sdf *sdf) {
  glBindTexture(GL_TEXTURE_2D, g.logo_tex[0]);
  if (g.logo_ids[0] != sdf->id) {
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, sdf->w, sdf->h, 0, GL_RED,
                 GL_FLOAT, sdf->d);
    g.logo_ids[0] = sdf->id;
  }
}

static void vec3u(GLint u, const double v[3]) {
  glUniform3f(u, v[0], v[1], v[2]);
}

/* peek_layout "terrain"'s uniforms (terrain.c has the landscape): off at
 * t 0, the flat map's code exactly. */
static void map_terrain(const struct wallpaper_config *c, int pw, int ph,
                        const struct wp_frame *f,
                        const struct wp_topo_grid *grid, double k) {
  glUniform1i(g.m_hf, 3);
  glUniform1i(g.m_clk, 4);
  glUniform1i(g.m_lsdf, 5);
  struct wp_terrain T;
  wp_terrain_at(c, pw, ph, f, &T);
  bool on = T.t > 0;
  glUniform1i(g.m_terrain, on);
  bool lines = c->topo_line_width > 0 && c->topo_line_alpha > 0;
  glUniform1f(g.m_lmax, lines ? c->topo_levels + (on ? 60 : 0) : 0);
  if (!on)
    return;
  glActiveTexture(GL_TEXTURE3);
  glBindTexture(GL_TEXTURE_2D, g.hf_tex);
  if (g.hf_grid != grid->id) {
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, grid->nx, grid->ny, 0, GL_RED,
                 GL_FLOAT, grid->h);
    g.hf_grid = grid->id;
  }
  vec3u(g.m_cam_o, T.cam.o);
  vec3u(g.m_cam_r, T.cam.r);
  vec3u(g.m_cam_d, T.cam.d);
  vec3u(g.m_cam_f, T.cam.fw);
  glUniform2f(g.m_cam_c, T.cam.cx, T.cam.cy);
  glUniform1f(g.m_cam_F, T.cam.focal);
  glUniform1f(g.m_t_t, T.t);
  glUniform1f(g.m_t_tf, T.tf);
  glUniform1f(g.m_t_unit, T.unit);
  glUniform1f(g.m_t_zmax, T.zmax);
  glUniform1f(g.m_t_fog, fmax(c->peek_terrain_fog * ph, 1));
  glUniform1f(g.m_t_shade, c->peek_terrain_shade);
  vec3u(g.m_t_light, T.light);
  glUniform3f(g.m_t_sum, T.lx, T.ly, T.sum_h);
  glUniform1f(g.m_t_sum_r, T.sum_r);
  /* HH:MM's sign, a texel per px across, in the snowflake's glass */
  struct wp_clock_mask cm;
  bool clock = f->peek && T.tf > 0 &&
               wp_peek_clock_mask(c, f->peek, T.clk_size, &cm);
  glUniform1f(g.m_clk_a, clock ? T.tf : 0);
  if (clock) {
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, g.clk_tex);
    if (g.clk_id != cm.id) {
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      glPixelStorei(GL_UNPACK_ROW_LENGTH, cm.stride);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, cm.w, cm.h, 0, GL_RED,
                   GL_UNSIGNED_BYTE, cm.data);
      glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      g.clk_id = cm.id;
    }
    vec3u(g.m_clk_o, T.clk_o);
    vec3u(g.m_clk_r, T.clk_r);
    vec3u(g.m_clk_u, T.clk_u);
    vec3u(g.m_clk_n, T.clk_n);
    glUniform2f(g.m_clk_wh, cm.w, cm.h);
    glUniform1f(g.m_clk_base, cm.base);
    glUniform1f(g.m_clk_st, T.clk_stretch);
    glUniform1f(g.m_clk_depth, T.clk_depth);
    struct wp_glass look;
    wp_glass_look(c, f, k, &look);
    set_color(g.m_glass, look.fill, 1);
    set_color(g.m_cedge0, look.edge[0], 1);
    set_color(g.m_cedge1, look.edge[1], 1);
    set_color(g.m_cedge2, look.edge[2], 1);
  }
  /* the mesas: CPU, memory, battery (warning when low) */
  float mesa[3][4], col[3][4];
  const struct info *in = f->peek;
  bool low = in && in->has_battery && !in->charging &&
             in->battery <= f->peek_low_pct;
  uint32_t cols[3] = {c->topo_edge_a, c->topo_edge_mid,
                      low ? c->peek_warn : c->topo_edge_b};
  for (int i = 0; i < 3; ++i) {
    mesa[i][0] = (float)T.mesa_x[i];
    mesa[i][1] = (float)T.mesa_y[i];
    mesa[i][2] = (float)T.mesa_floor[i];
    mesa[i][3] = (float)T.mesa_top[i];
    col[i][0] = (float)col_r(cols[i]);
    col[i][1] = (float)col_g(cols[i]);
    col[i][2] = (float)col_b(cols[i]);
    col[i][3] = 1;
  }
  glUniform4fv(g.m_mesa, 3, mesa[0]);
  glUniform4fv(g.m_mesa_col, 3, col[0]);
  glUniform1f(g.m_mesa_r, T.mesa_r);
  /* the snowflake's shadow, cast along the light */
  double r0 = c->logo_radius * k;
  double s = 1 + c->react_pulse_scale * f->pulse;
  bool shadow = r0 > 0 && wp_logo_whole(f) && f->fade < 1;
  glUniform1f(g.m_l_on, shadow ? T.tf : 0);
  if (shadow) {
    const struct wp_logo_sdf *sdf = wp_logo_sdf(c, pw, 1);
    glActiveTexture(GL_TEXTURE5);
    bind_sdf(sdf);
    glUniform2f(g.m_lsdf_org, sdf->x, sdf->y);
    glUniform2f(g.m_lsdf_size, (double)sdf->w / sdf->res,
                (double)sdf->h / sdf->res);
    glUniform3f(g.m_l_pos, T.lx, T.ly, T.lz);
    glUniform2f(g.m_l_rot, cos(f->angle), sin(f->angle));
    glUniform1f(g.m_l_inv_s, 1 / s);
    glUniform1f(g.m_l_scale, s);
  }
  glActiveTexture(GL_TEXTURE0);
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
  glUniform1f(g.m_bright, wp_map_brightness(c, f));
  float rings[WP_RINGS][4] = {{0}};
  int n = f->nrings < WP_RINGS ? f->nrings : WP_RINGS;
  for (int i = 0; i < n; ++i) {
    rings[i][0] = (float)(f->rings[i].radius * k);
    rings[i][1] = (float)f->rings[i].size;
  }
  glUniform4fv(g.m_rings, WP_RINGS, rings[0]);
  glUniform1i(g.m_nrings, n);
  /* Round the snowflake, moved aside (not wandering: no rings then). */
  double side = wp_peek_side(c, f), ls = 1 + (c->peek_side_scale - 1) * side;
  glUniform2f(g.m_centre, pw / 2.0 + (c->peek_side_x - 0.5) * pw * side,
              ph / 2.0 + (c->peek_side_y - 0.5) * ph * side);
  /* A topo show's own rings, pull and lines. */
  const struct wp_show_map *sm = &f->smap;
  bool sring = sm->on && sm->ring_px > 0;
  glUniform1f(g.m_ring_px, (sring ? sm->ring_px : c->topo_ring_px) * k);
  glUniform1f(g.m_ring_w, (sring ? sm->ring_w : c->topo_ring_width) * k);
  if (sm->on) {
    glUniform2f(g.m_s_in, sm->in_r * k, fmax(sm->in_band * k, 1));
    glUniform2f(g.m_s_out, sm->out_r * k, fmax(sm->out_band * k, 1));
    glUniform3f(g.m_s_lit, sm->lit_r * k, fmax(sm->lit_w * k, 1), sm->lit);
    glUniform1f(g.m_s_lit_all, sm->lit_all);
    glUniform2f(g.m_s_levels, sm->levels, sm->level_lit);
  } else {
    glUniform2f(g.m_s_in, 1e6, 1);
    glUniform2f(g.m_s_out, -1e6, 1);
    glUniform3f(g.m_s_lit, 0, 1, 0);
    glUniform1f(g.m_s_lit_all, 0);
    glUniform2f(g.m_s_levels, 1e4, 0);
  }
  glUniform1f(g.m_sway, f->sway * c->topo_sway_px * k);
  float pokes[WP_WAKE][3] = {{0}};
  int np = f->npokes < WP_WAKE ? f->npokes : WP_WAKE;
  for (int i = 0; i < np; ++i) {
    pokes[i][0] = (float)f->pokes[i].x;
    pokes[i][1] = (float)f->pokes[i].y;
    pokes[i][2] = (float)f->pokes[i].open;
  }
  glUniform3fv(g.m_pokes, WP_WAKE, pokes[0]);
  glUniform1i(g.m_npokes, np);
  glUniform1f(g.m_poke_r, fmax(c->topo_poke_radius * k, 1));
  glUniform1f(g.m_poke_light, c->topo_poke_light);
  glUniform1f(g.m_time, f->time);
  glUniform2f(g.m_drift, f->drift_x * k, f->drift_y * k);
  glUniform1f(g.m_drift_l, 900 * k);
  glUniform1i(g.m_spec_on, f->spec_on);
  glUniform1fv(g.m_spec, 2 * WP_SPEC, f->spec[0]);
  glUniform1f(g.m_logo_r, c->logo_radius * k * ls);
  glUniform1f(g.m_spec_px, c->topo_spectrum_px * k);
  glUniform1f(g.m_spec_reach, c->topo_spectrum_reach * k);
  glUniform1f(g.m_ripple_px, c->topo_ripple_px * k);
  glUniform1f(g.m_ripple_span, c->topo_ripple_span * k);
  glUniform1f(g.m_light, c->topo_light);
  glUniform1f(g.m_light_w, c->topo_light_width);
  bool sgather = sm->on && sm->gather != 0;
  /* Moving the snowflake aside shows the hold instead of the lines. */
  bool sided = strcmp(c->peek_layout, "flanks") != 0;
  double pg = sided ? 0 : f->peek_gather;
  glUniform1f(g.m_gather, (sgather ? sm->gather : pg * c->topo_gather_px) * k);
  glUniform1f(g.m_gather_reach,
              fmax((sgather ? sm->gather_reach : c->topo_gather_reach) * k, 1));
  /* The front starts past the corners and reaches the logo as the hold
   * ends; the lit lines go with the expand. */
  double band = 120 * k, logo = c->logo_radius * k;
  double rmax = hypot(pw, ph) / 2 + band;
  bool held = f->peek && f->peek_hold > 0 && !sided;
  glUniform1f(g.m_hold_front, logo + (rmax - logo) * (1 - f->peek_hold));
  glUniform1f(g.m_hold_band, band);
  glUniform1f(g.m_hold_lit,
              held ? c->topo_hold_light * f->peek_pop * (1 - f->peek_expand)
                   : 0);
  map_terrain(c, pw, ph, f, grid, k);
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
  double k = pw / WP_REF_WIDTH, r0 = c->logo_radius * k, cx, cy, ls;
  wp_logo_place(c, pw, ph, f, &cx, &cy, &ls);
  double s = ls * (1 + c->react_pulse_scale * f->pulse);
  if (r0 <= 0 || s <= 0 || !wp_logo_whole(f) || f->fade >= 1)
    return;
  /* Grown aside for the popup, a field at rest's px would lose the gaps
   * between the lambdas' outlines: as many texels per px as it grows. */
  int res = ls > 1.01 ? (int)fmin(fmax(ceil(c->peek_side_scale), 1), 4) : 1;
  const struct wp_logo_sdf *sdf = wp_logo_sdf(c, pw, res);
  glUseProgram(g.logo_prog);
  glActiveTexture(GL_TEXTURE0);
  bind_sdf(sdf);
  glUniform1i(g.l_sdf, 0);
  glUniform1i(g.l_shadow, 1);
  glUniform1i(g.l_neon, 2);
  glUniform2f(g.l_sdf_org, sdf->x, sdf->y);
  glUniform2f(g.l_sdf_size, (double)sdf->w / sdf->res,
              (double)sdf->h / sdf->res);
  glUniform2f(g.l_origin, ox, oy);
  glUniform2f(g.l_step, sx, sy);
  glUniform1f(g.l_bh, bh);
  /* As cairo: shaken by whole pixels. */
  cx += round(f->shake_x * k);
  cy += round(f->shake_y * k);
  glUniform2f(g.l_centre, cx, cy);
  glUniform2f(g.l_rot, cos(f->angle), sin(f->angle));
  glUniform1f(g.l_inv_s, 1 / s);
  glUniform1f(g.l_scale, s);
  double sd = c->topo_shadow_px * k * s;
  bool shadow = logo_mask(c, pw, 0, g.l_sh_org, g.l_sh_size);
  /* Grown aside for the popup, its texels would show: smooth them. */
  GLint filter = ls > 1.01 ? GL_LINEAR : GL_NEAREST;
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
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
  /* Tipped towards the pointer: its face turned towards screen direction
   * (ux, uy) by th, about the axis (-uy, ux, 0) in the screen's plane. */
  double th = hypot(f->tilt_x, f->tilt_y);
  bool tilted = th > 1e-6 && c->logo_tilt > 0;
  glUniform1i(g.l_tilted, tilted);
  if (tilted) {
    double ux = f->tilt_x / th, uy = f->tilt_y / th, kx = -uy, ky = ux;
    double co = cos(th), si = sin(th);
    /* Row by row; read column by column, it's the inverse. */
    float m[9] = {co + (1 - co) * kx * kx, (1 - co) * kx * ky, si * ky,
                  (1 - co) * kx * ky,      co + (1 - co) * ky * ky, -si * kx,
                  -si * ky,                si * kx,                 co};
    glUniformMatrix3fv(g.l_tm, 1, GL_FALSE, m);
    glUniform3f(g.l_tn, si * ux, si * uy, co);
    glUniform1f(g.l_focal, WP_TILT_FOCAL * r0 * ls);
    glUniform1f(g.l_depth, c->logo_tilt_depth * r0 * s);
    glUniform1i(g.l_layers, c->logo_tilt_layers);
    /* The layers come in over the first half of the way to logo_tilt. */
    glUniform1f(g.l_layer_a, fmin(th / (0.5 * c->logo_tilt * M_PI / 180), 1));
  }
  /* Hovering over the terrain: level, seen by its eye; its shadow falls
   * on the ground (the map's), the layers show its thickness. */
  if (wp_peek_terrain(c, f) > 0) {
    struct wp_terrain T;
    wp_terrain_at(c, pw, ph, f, &T);
    glUniform1i(g.l_tilted, 2);
    glUniform3f(g.l_c_o, T.cam.o[0] - T.lx, T.cam.o[1] - T.ly,
                T.cam.o[2] - T.lz);
    vec3u(g.l_c_r, T.cam.r);
    vec3u(g.l_c_d, T.cam.d);
    vec3u(g.l_c_f, T.cam.fw);
    glUniform2f(g.l_c_c, T.cam.cx + round(f->shake_x * k),
                T.cam.cy + round(f->shake_y * k));
    glUniform1f(g.l_c_F, T.cam.focal);
    glUniform1f(g.l_depth, c->logo_tilt_depth * r0 * s);
    glUniform1i(g.l_layers, c->logo_tilt_layers);
    glUniform1f(g.l_layer_a, T.tf);
    glUniform1f(g.l_sh_alpha, shadow ? c->topo_shadow_alpha * (1 - T.tf) : 0);
  }
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
