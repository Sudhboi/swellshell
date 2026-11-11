/* One background layer surface per output, drawn natively at the
 * fractional scale (wp_fractional_scale_v1 + viewporter), double-buffered,
 * repainting and damaging only the bar and the logo after the first frame.
 * Frame callbacks pace the animation and are only requested while the
 * module animates or dozes, so a frozen wallpaper costs nothing. Dozing
 * repaints only the bar, and only when its brightness or fill changed. While the smooth
 * edge visualiser shows, the GPU draws it (gl.c) into two transparent
 * subsurfaces over the strips; they exist only while bars show. Under the
 * topo style the GPU also draws the map on a subsurface below and the
 * snowflake on one above, so a frame often leaves the buffer alone.
 *
 * Idle over windows (saver_windows), each output also gets a cover: the
 * same pipeline on the overlay layer, drawing the saver frame over the
 * windows, faded by wp_alpha_modifier_v1 on each of its surfaces. The
 * background keeps its frame meanwhile (it shows through translucent
 * windows), redrawn plain only once the cover hides it or is gone. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "alpha-modifier-v1-client-protocol.h"
#include "buffer.h"
#include "fractional-scale-v1-client-protocol.h"
#include "loop.h"
#include "util.h"
#include "viewporter-client-protocol.h"
#include "wallpaper.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/* A GPU-drawn edge strip: a synchronised subsurface over the strip. */
struct strip {
  struct wl_surface *surface;
  struct wl_subsurface *sub;
  struct wp_viewport *viewport;
  struct wp_gl_win *win;
  struct wp_alpha_modifier_surface_v1 *alpha; /* a cover's, else NULL */
  uint32_t mult;                              /* as last set */
  int lx, ly, lw, lh; /* logical, in the parent */
  int bw, bh;         /* buffer px */
};

/* What a buffer holds, as of its last paint. A new buffer holds nothing
 * and is stale; each paint records all of it. */
struct painted {
  bool stale;        /* needs a full repaint before partial ones are valid */
  bool ringed;       /* visualiser bars, outside the small logo tile */
  bool still;        /* the frozen frame outside the bar (dozing) */
  struct wp_text_key text; /* the title and artist */
  bool showed;       /* a launch or power-off frame */
  double cpu_bright; /* the cairo map's brightness */
  bool shook;        /* the cairo map, shaken */
  long cpu_drift[2]; /* the cairo map's drift, whole px */
  bool logoed;       /* the CPU's snowflake */
  bool streaked;     /* speed lines */
  bool hexed;        /* the hexagon (cover, progress) */
  bool wandered;     /* something off the centre */
  bool peeked;       /* the popup's contents */
};

/* What the GPU's snowflake is drawn from (logo_key()). */
#define LOGO_KEY 13

/* Either buffer holds it. */
#define EITHER(out, field) ((out)->painted[0].field || (out)->painted[1].field)

struct output {
  struct wl_list link;
  struct swell_output *so;
  struct wl_surface *surface;
  struct zwlr_layer_surface_v1 *layer;
  struct wp_viewport *viewport;
  struct wp_fractional_scale_v1 *fractional;
  struct wl_callback *frame_cb;
  struct loop_timer *fps_timer; /* fps cap: the delayed next frame */
  struct buffer *bufs[2];
  struct painted painted[2];     /* of bufs[] */
  double bar_level, bar_percent; /* of the last commit */
  struct strip strips[2]; /* left, right; surface NULL unless shown */
  /* The topo style with a GPU: the map on a subsurface under the buffers,
   * which leave it clear (ARGB). */
  bool under;
  struct strip map;     /* surface NULL unless under */
  double map_bright;    /* as last drawn, -1: not drawn since a change */
  bool map_moved;       /* the last map frame had rings, sway, the spectrum or a
                           gather */
  double map_drift[2];  /* the GPU map's drift as last drawn, px */
  /* The topo snowflake on the GPU, on a subsurface over the logo's box. */
  bool lgpu;            /* the GPU draws it (but in shows and slides) */
  struct strip logo;    /* surface NULL until the GPU first draws it */
  bool logo_on;         /* it holds a snowflake, not nothing */
  double logo_key[LOGO_KEY]; /* the frame it was drawn from, NAN: redraw */
  double bar_sx, bar_sy; /* the shake of the last commit, px */
  double bar_fade;       /* of the last commit */
  struct wp_text_key text_shown; /* the title the compositor last got */
  int width, height; /* logical, from configure */
  uint32_t scale120;  /* preferred fractional scale, 0 if unknown */
  bool configured;
  bool retry;         /* a draw found both buffers busy */
  bool held;          /* a draw waits for a replaced daemon (wp_join_wait) */
  bool presented;     /* a frame of ours has been shown (for the handover) */
  int64_t last_draw;  /* ms */
  /* The saver over windows. A background output's cover (NULL until first
   * wanted; its surface only while it shows); a cover's background. */
  bool is_cover;
  struct output *cover, *bg;
  struct wp_alpha_modifier_surface_v1 *alpha; /* a cover's, else NULL */
  uint32_t mult;                              /* as last set */
  /* A background's: its frame isn't the plain one (wandering, the bar
   * faded) or is stale, to redraw once the cover hides it or is gone. */
  bool unplain;
  bool skipped; /* a background's: a full draw was skipped under a cover */
};

static struct swell_wl *wl;
static struct wl_list outputs; /* output.link */
static bool gl_broken; /* a GPU draw failed: the CPU draws from now on */

static void draw(struct output *out, bool full);

/* ---- frame pacing ---- */

static void request_frame(struct output *out);

/* Taking over from a replaced daemon: once every output shows our frame,
 * it may go. */
static void check_presented(void) {
  struct output *out;
  wl_list_for_each(out, &outputs,
                   link) if (out->surface && !out->presented) return;
  wp_joined();
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
  struct output *out = data;
  wl_callback_destroy(cb);
  out->frame_cb = NULL;
  if (!out->presented && !out->is_cover) {
    out->presented = true;
    if (wp_joining())
      check_presented();
  }
  /* (A cover draws once more after its fade, to go.) */
  if (!wp_ticking() && !out->retry && !out->is_cover)
    return; /* frozen: stop here, the last frame stays */
  int fps = wp_fps_cap();
  int64_t wait = fps > 0 ? out->last_draw + 1000 / fps - now_ms() : 0;
  if (wait > 1 && !out->retry)
    loop_timer_arm(out->fps_timer, wait, 0);
  else
    draw(out, false);
}

static const struct wl_callback_listener frame_listener = {.done = frame_done};

static void request_frame(struct output *out) {
  if (out->frame_cb)
    return;
  out->frame_cb = wl_surface_frame(out->surface);
  wl_callback_add_listener(out->frame_cb, &frame_listener, out);
}

static void on_fps_timer(struct loop_timer *t, void *data) {
  struct output *out = data;
  if (!out->surface)
    return;
  if (out->held) {
    out->held = false;
    draw(out, true);
  } else if (wp_ticking() || out->is_cover) {
    draw(out, false);
  }
}

/* ---- drawing ---- */

static int output_scale_px(struct output *out, int logical) {
  if (out->scale120 && out->viewport)
    return (int)lround(logical * out->scale120 / 120.0);
  return logical * (out->so->scale > 0 ? out->so->scale : 1);
}

static int get_buffer(struct output *out, int pw, int ph, uint32_t format) {
  for (int i = 0; i < 2; ++i) {
    struct buffer *b = out->bufs[i];
    if (b && b->busy)
      continue;
    if (b && (b->width != pw || b->height != ph || b->format != format)) {
      buffer_destroy(b);
      b = out->bufs[i] = NULL;
    }
    if (!b) {
      b = out->bufs[i] = buffer_create(wl->shm, pw, ph, format);
      if (!b)
        return -1;
      out->painted[i] = (struct painted){.stale = true};
    }
    return i;
  }
  return -1;
}

/* ---- GPU edge strips ---- */

static bool gpu_edges(void) {
  const struct wallpaper_config *c = wp_config();
  return c->viz && c->viz_edges && c->viz_edge_smooth && c->viz_gpu &&
         !gl_broken && wl->subcompositor && wp_gl_init(wl->display);
}

static void strip_destroy(struct strip *st) {
  if (st->alpha)
    wp_alpha_modifier_surface_v1_destroy(st->alpha);
  wp_gl_win_destroy(st->win);
  if (st->viewport)
    wp_viewport_destroy(st->viewport);
  if (st->sub)
    wl_subsurface_destroy(st->sub);
  if (st->surface)
    wl_surface_destroy(st->surface);
  *st = (struct strip){0};
}

static void strips_destroy(struct output *out) {
  for (int s = 0; s < 2; ++s)
    strip_destroy(&out->strips[s]);
}

/* ---- the cover's fade ---- */

static uint32_t cover_mult(void) {
  double a = wp_cover_alpha();
  return a <= 0 ? 0 : a >= 1 ? UINT32_MAX : (uint32_t)(a * UINT32_MAX);
}

/* A new subsurface of a cover fades with it. */
static void strip_fade(struct output *out, struct strip *st) {
  if (!out->is_cover || !wl->alpha_modifier || st->alpha)
    return;
  st->alpha = wp_alpha_modifier_v1_get_surface(wl->alpha_modifier,
                                               st->surface);
  st->mult = cover_mult();
  wp_alpha_modifier_surface_v1_set_multiplier(st->alpha, st->mult);
}

/* Set a cover's alpha on a subsurface; it's committed (cached till the
 * parent's commit, the subsurfaces being synchronised). */
static void strip_mult(struct strip *st, uint32_t m) {
  if (!st->surface || !st->alpha || st->mult == m)
    return;
  wp_alpha_modifier_surface_v1_set_multiplier(st->alpha, m);
  wl_surface_commit(st->surface);
  st->mult = m;
}

/* Before a cover's commit: every surface of it at the cover's alpha. */
static void cover_fade(struct output *out) {
  if (!out->is_cover)
    return;
  uint32_t m = cover_mult();
  strip_mult(&out->map, m);
  strip_mult(&out->logo, m);
  for (int s = 0; s < 2; ++s)
    strip_mult(&out->strips[s], m);
  if (out->alpha && out->mult != m) {
    wp_alpha_modifier_surface_v1_set_multiplier(out->alpha, m);
    out->mult = m;
  }
}

bool wp_surfaces_can_fade(void) { return wl && wl->alpha_modifier; }

static long gcd(long a, long b) { return b ? gcd(b, a % b) : a; }

/* Place strip st over buffer rect r of the pw x ph parent buffer, created
 * on first use: it covers r in whole logical px, widened so its corner
 * lands on a whole buffer px where the scale allows. Gives where its top
 * left pixel's corner sits in the parent buffer (ox, oy) and how much of
 * it a pixel spans (sx, sy). -1 if the GPU failed, 0 if r is empty. */
static int strip_place(struct output *out, struct strip *st, struct wp_rect r,
                       int pw, double *ox, double *oy, double *sx,
                       double *sy) {
  bool fractional = out->scale120 && out->viewport;
  double k = (double)pw / out->width; /* buffer px per logical px */
  int m = fractional ? (int)(120 / gcd(out->scale120, 120)) : 1;
  int lx0 = (int)floor(r.x / k / m) * m, ly0 = (int)floor(r.y / k / m) * m;
  int lx1 = (int)ceil((r.x + r.w) / k / m) * m;
  int ly1 = (int)ceil((r.y + r.h) / k / m) * m;
  lx1 = lx1 < out->width ? lx1 : out->width;
  ly1 = ly1 < out->height ? ly1 : out->height;
  int lw = lx1 - lx0, lh = ly1 - ly0;
  if (lw <= 0 || lh <= 0)
    return 0;
  int bw = fractional ? (int)lround(lw * k) : lw * (int)lround(k);
  int bh = fractional ? (int)lround(lh * k) : lh * (int)lround(k);
  if (!st->surface) {
    st->surface = wl_compositor_create_surface(wl->compositor);
    st->sub = wl_subcompositor_get_subsurface(wl->subcompositor, st->surface,
                                              out->surface);
    /* Pointer input goes through to the wallpaper. */
    struct wl_region *none = wl_compositor_create_region(wl->compositor);
    wl_surface_set_input_region(st->surface, none);
    wl_region_destroy(none);
    strip_fade(out, st);
    if (fractional)
      st->viewport = wp_viewporter_get_viewport(wl->viewporter, st->surface);
    st->win = wp_gl_win_create(st->surface, bw, bh);
    if (!st->win)
      return -1;
    st->lx = st->ly = -1;
  } else if (bw != st->bw || bh != st->bh) {
    wp_gl_win_resize(st->win, bw, bh);
  }
  st->bw = bw;
  st->bh = bh;
  if (lx0 != st->lx || ly0 != st->ly)
    wl_subsurface_set_position(st->sub, lx0, ly0);
  if (st->viewport)
    wp_viewport_set_destination(st->viewport, lw, lh);
  else
    wl_surface_set_buffer_scale(st->surface, (int)lround(k));
  st->lx = lx0;
  st->ly = ly0;
  st->lw = lw;
  st->lh = lh;
  *ox = lx0 * k;
  *oy = ly0 * k;
  *sx = lw * k / bw;
  *sy = lh * k / bh;
  return 1;
}

/* Draw side s over buffer rect r of the pw x ph parent buffer. False if
 * the GPU failed. */
static bool strip_draw(struct output *out, int s, struct wp_rect r, int pw,
                       int ph, const struct wp_frame *f) {
  struct strip *st = &out->strips[s];
  double ox, oy, sx, sy;
  int placed = strip_place(out, st, r, pw, &ox, &oy, &sx, &sy);
  if (placed <= 0)
    return placed == 0;
  return wp_gl_edge(st->win, wp_config(), pw, ph, f, s, ox, oy, sx, sy,
                    st->bw, st->bh);
}

/* Show the frame's edge bars in the strips (false: the GPU failed). */
static bool strips_draw(struct output *out, int pw, int ph,
                        const struct wp_frame *f) {
  struct wp_rect r[WP_REGIONS];
  wp_regions(wp_config(), pw, ph, true, false, r);
  for (int s = 0; s < 2; ++s)
    if (!strip_draw(out, s, r[2 + s], pw, ph, f))
      return false;
  return true;
}

/* ---- the GPU's topo map ---- */

static bool gpu_map(void) {
  const struct wallpaper_config *c = wp_config();
  return wp_topo(c) && c->viz_gpu && !gl_broken && wl->subcompositor &&
         wp_gl_init(wl->display);
}

/* Draw the map over the whole output, under the buffers. False if the GPU
 * failed. */
static bool map_draw(struct output *out, int pw, int ph,
                     const struct wp_frame *f) {
  struct strip *st = &out->map;
  bool fractional = out->scale120 && out->viewport;
  if (!st->surface) {
    st->surface = wl_compositor_create_surface(wl->compositor);
    st->sub = wl_subcompositor_get_subsurface(wl->subcompositor, st->surface,
                                              out->surface);
    wl_subsurface_place_below(st->sub, out->surface);
    struct wl_region *none = wl_compositor_create_region(wl->compositor);
    wl_surface_set_input_region(st->surface, none);
    wl_region_destroy(none);
    strip_fade(out, st);
    if (fractional)
      st->viewport = wp_viewporter_get_viewport(wl->viewporter, st->surface);
    st->win = wp_gl_win_create(st->surface, pw, ph);
    if (!st->win)
      return false;
  } else if (pw != st->bw || ph != st->bh) {
    wp_gl_win_resize(st->win, pw, ph);
  }
  st->bw = pw;
  st->bh = ph;
  if (st->viewport)
    wp_viewport_set_destination(st->viewport, out->width, out->height);
  else
    wl_surface_set_buffer_scale(st->surface, pw / out->width);
  return wp_gl_map(st->win, wp_config(), pw, ph, f, 0, 0, 1, 1, pw, ph);
}

/* ---- the GPU's topo snowflake ---- */

static bool gpu_logo(void) {
  const struct wallpaper_config *c = wp_config();
  return c->logo_gpu && c->logo_radius > 0 && gpu_map();
}

/* Draw frame f's snowflake over its box, or nothing (f NULL). False if the
 * GPU failed. */
static bool logo_draw(struct output *out, int pw, int ph,
                      const struct wp_frame *f) {
  /* Not whole: wp_gl_logo() clears it. */
  static const struct wp_frame none = {.logo_shrink = 1};
  struct strip *st = &out->logo;
  double ox, oy, sx, sy;
  int placed = strip_place(out, st, wp_logo_box(wp_config(), pw, ph, f), pw,
                           &ox, &oy, &sx, &sy);
  if (placed <= 0)
    return placed == 0;
  out->logo_on = f != NULL;
  return wp_gl_logo(st->win, wp_config(), pw, ph, f ? f : &none, ox, oy, sx,
                    sy, st->bw, st->bh);
}

/* What the GPU's snowflake is drawn from. */
static void logo_key(const struct wp_frame *f, double key[LOGO_KEY]) {
  double k[LOGO_KEY] = {f->angle,    f->pulse,    f->glow,     f->over,
                        f->flash,    f->map_fade, f->shake_x,  f->shake_y,
                        f->wander_x, f->wander_y, wp_peek_side(wp_config(), f),
                        f->tilt_x,   f->tilt_y};
  memcpy(key, k, sizeof(k));
}

const char *wp_surfaces_logo_renderer(void) {
  if (!wp_config()->logo_gpu || !wp_config()->viz_gpu)
    return "cpu";
  if (gl_broken || !wl->subcompositor || !wp_gl_init(wl->display))
    return "cpu (no GPU)";
  return "gpu";
}

const char *wp_surfaces_viz_renderer(void) {
  if (!wp_config()->viz_gpu)
    return "cpu";
  if (gl_broken || !wl->subcompositor || !wp_gl_init(wl->display))
    return "cpu (no GPU)";
  return "gpu";
}

static void destroy_surface(struct output *out);
static void draw_one(struct output *out, bool full, bool plain);

/* The cover faded off (or the windows went): it goes, and the background,
 * if the frame it kept isn't the plain one, gets it. */
static void cover_drop(struct output *cover) {
  destroy_surface(cover);
  struct output *bg = cover->bg;
  if (!wp_ticking() && (bg->unplain || bg->skipped))
    draw_one(bg, true, false);
}

/* Draw out's next frame (full: all of it). Under a cover, a background
 * keeps the frame it has; once the cover hides it, it's redrawn plain
 * (centred, the bar shown: the frozen look under windows) if it isn't. */
static void draw(struct output *out, bool full) {
  if (out->is_cover) {
    if (wp_cover_alpha() < 0) {
      cover_drop(out);
      return;
    }
    draw_one(out, full, false);
    struct output *bg = out->bg;
    if (out->surface && wp_bg_frozen() && wp_cover_alpha() >= 1 &&
        (bg->unplain || bg->skipped))
      draw_one(bg, true, true);
    return;
  }
  if (out->cover && out->cover->surface && wp_bg_frozen()) {
    if (full)
      out->skipped = true;
    return;
  }
  draw_one(out, full, false);
}

static void draw_one(struct output *out, bool full, bool plain) {
  if (!out->surface || !out->configured || out->width <= 0)
    return;
  cover_fade(out);
  /* Taking over from a replaced daemon: its wallpaper stays on screen
   * until its lines are gone. */
  int64_t wait = wp_join_wait();
  if (wait > 0) {
    out->held = true;
    loop_timer_arm(out->fps_timer, wait, 0);
    return;
  }
  if (out->held) {
    out->held = false;
    full = true;
  }
  int pw = output_scale_px(out, out->width);
  int ph = output_scale_px(out, out->height);
  if (full)
    out->painted[0].stale = out->painted[1].stale = true;
  bool under = gpu_map();
  if (under != out->under) {
    out->under = under;
    out->painted[0].stale = out->painted[1].stale = true;
    if (!under)
      strip_destroy(&out->map);
  }
  if (full || out->map.bw != pw || out->map.bh != ph)
    out->map_bright = -1;
  /* The snowflake moving between the CPU and the GPU: repaint the buffers
   * with it or without it. */
  bool lgpu = under && gpu_logo();
  if (lgpu != out->lgpu) {
    out->lgpu = lgpu;
    out->painted[0].stale = out->painted[1].stale = true;
    if (!lgpu) {
      strip_destroy(&out->logo);
      out->logo_on = false;
    }
  }
  if (full)
    out->logo_key[0] = NAN;
  int i = get_buffer(out, pw, ph,
                     under ? WL_SHM_FORMAT_ARGB8888 : WL_SHM_FORMAT_XRGB8888);
  if (i < 0) {
    /* Both busy; one is released by the time the next frame is due. */
    out->retry = true;
    request_frame(out);
    wl_surface_commit(out->surface);
    return;
  }
  out->retry = false;
  struct buffer *b = out->bufs[i];
  const struct painted *p = &out->painted[i];
  bool whole = p->stale;

  int64_t now = now_ms();
  struct wp_frame f;
  wp_frame_at(now, &f);
  if (plain) {
    f.wander_x = f.wander_y = 0;
    f.bar_fade = 0;
  }
  if (!out->is_cover) {
    out->unplain = f.wander_x != 0 || f.wander_y != 0 || f.bar_fade > 0;
    out->skipped = false;
  }
  /* A show moves things far outside the regions, so it is drawn whole, and
   * so is the first frame after it in each buffer. */
  bool showing = wp_showing();
  if (showing || p->showed)
    whole = true;
  /* The map fading: the GPU redraws it, or cairo the whole buffer. */
  double bright = wp_map_brightness(wp_config(), &f);
  bool shaking = f.shake_x != 0 || f.shake_y != 0;
  /* The held popup key gathers the lines in and lights them. */
  /* (Or, side layout, the snowflake moves aside and the lines follow.) */
  double side = wp_peek_side(wp_config(), &f);
  bool sided = strcmp(wp_config()->peek_layout, "flanks") != 0;
  bool gathering =
      sided ? side > 0 && side < 1
            : (f.peek_gather != 0 && wp_config()->topo_gather_px > 0) ||
                  (f.peek && f.peek_hold > 0 && f.peek_pop > 0 &&
                   f.peek_expand < 1 && wp_config()->topo_hold_light > 0);
  /* The pointer pushes the lines on its own output only. */
  if (f.poke_out != out || !wp_config()->topo_poke || out->width <= 0 ||
      out->height <= 0)
    f.npokes = 0;
  for (int i = 0; i < f.npokes; ++i) {
    f.pokes[i].x *= (double)pw / out->width;
    f.pokes[i].y *= (double)ph / out->height;
  }
  bool moving = f.nrings > 0 || f.sway > 0 || f.spec_on || shaking ||
                gathering || f.smap.on || f.npokes > 0;
  /* The slow drift at rest redraws the GPU's map only once it has moved a
   * quarter pixel (its bend moves the lines up to 1.3 times the drift), and
   * cairo's whole buffer once it has moved a whole one. */
  double dk = pw / WP_REF_WIDTH;
  double dx = f.drift_x * dk, dy = f.drift_y * dk;
  bool drifted =
      1.3 * fmax(fabs(dx - out->map_drift[0]), fabs(dy - out->map_drift[1])) >=
      0.25;
  long cdx = lround(dx), cdy = lround(dy);
  bool map_due = under && (out->map_bright != bright || moving || drifted ||
                           out->map_moved || !out->map.surface);
  if (!under && wp_topo(wp_config()) &&
      (p->cpu_bright != bright || shaking || p->shook ||
       cdx != p->cpu_drift[0] || cdy != p->cpu_drift[1]))
    whole = true;
  /* The GPU's snowflake, but in shows and while the lambdas slide (the
   * CPU draws those, and the GPU's surface shows nothing). */
  bool gl_logo = lgpu && !showing && wp_logo_whole(&f);
  /* Off the centre, what the buffer draws round the snowflake (all of it on
   * the CPU, speed lines on the GPU) leaves the regions: draw whole, and
   * again once it's back. */
  /* Moved aside, so is the hexagon. */
  bool hex = wp_hex_shows(wp_config(), &f);
  bool off = f.wander_x != 0 || f.wander_y != 0 || side > 0;
  bool wandering = off && (!gl_logo || f.over > 0 || (side > 0 && hex));
  if (wandering || p->wandered)
    whole = true;
  double key[LOGO_KEY];
  logo_key(&f, key);
  bool logo_due = gl_logo ? !out->logo_on || memcmp(key, out->logo_key,
                                                    sizeof(key)) != 0
                          : out->logo_on;
  /* Dozing, once both buffers hold the frozen frame, only the bar is
   * repainted, and nothing at all while it looks the same (the bar sits at
   * bar_min for much of each period): the timer paces until it changes. */
  /* The popup's contents: repainted while they show, and once more in
   * each buffer that still holds them. */
  bool peeking = f.peek && f.peek_pop > 0;
  bool peek_area = peeking || EITHER(out, peeked);
  bool bar_only = wp_dozing() && !whole && !map_due && !logo_due &&
                  !peek_area && out->painted[0].still && out->painted[1].still;
  if (bar_only && fabs(f.level - out->bar_level) < 0.5 / 255 &&
      fabs(f.percent - out->bar_percent) < 0.01 &&
      f.bar_fade == out->bar_fade) {
    int fps = wp_fps_cap();
    loop_timer_arm(out->fps_timer, fps > 0 ? 1000 / fps : 16, 0);
    return;
  }
  /* While either buffer holds visualiser bars, repaint and damage their
   * larger area (the ring's or the edge strips): that clears this buffer's
   * old bars and covers the difference from the other one. */
  /* The GPU's strips take the edge bars off the cairo buffer. */
  bool gpu = f.nbars > 0 && gpu_edges();
  if (gpu && !strips_draw(out, pw, ph, &f)) {
    warn("wallpaper: GPU drawing failed; the visualiser is back on the CPU");
    gl_broken = true;
    gpu = false;
  }
  if (!gpu)
    strips_destroy(out);
  if (map_due) {
    if (!map_draw(out, pw, ph, &f)) {
      warn("wallpaper: GPU drawing failed; the map is back on the CPU");
      gl_broken = true;
      draw(out, true); /* again, on the CPU */
      return;
    }
    out->map_bright = bright;
    out->map_moved = moving;
    out->map_drift[0] = dx;
    out->map_drift[1] = dy;
  }
  if (logo_due) {
    if (!logo_draw(out, pw, ph, gl_logo ? &f : NULL)) {
      warn("wallpaper: GPU drawing failed; the snowflake is back on the CPU");
      gl_broken = true;
      draw(out, true); /* again, on the CPU */
      return;
    }
    memcpy(out->logo_key, key, sizeof(key));
  }
  struct wp_frame cf = f;
  cf.map_under = under;
  cf.peek_area = peek_area;
  if (gpu)
    cf.nbars = 0;
  /* While a buffer holds the CPU's snowflake or speed lines, the whole
   * logo box is repainted, so they're cleared. */
  bool streaking = f.over > 0;
  cf.logo_gpu = gl_logo;
  cf.logo_box =
      gl_logo && (streaking || EITHER(out, logoed) || EITHER(out, streaked));
  bool ring = cf.nbars > 0 || EITHER(out, ringed);
  /* The title and artist are repainted (or cleared once gone) in a buffer
   * that doesn't hold them as they are now, and damaged while the
   * compositor has them otherwise. */
  struct wp_text_key tk;
  wp_text_key(wp_config(), pw, &f, &tk);
  bool text =
      !wp_text_key_eq(&tk, &p->text) || !wp_text_key_eq(&tk, &out->text_shown);
  cf.text_kept = !text;
  /* With the snowflake on the GPU, and no hexagon, bars or changing title,
   * only the bar can change in the buffer: repaint just it, and only if it
   * changed (or moved with a shake); otherwise commit the GPU's surfaces
   * alone. */
  double bsx = round(f.shake_x * pw / WP_REF_WIDTH);
  double bsy = round(f.shake_y * pw / WP_REF_WIDTH);
  if (gl_logo && !whole && !cf.logo_box && !ring && !text && !peek_area &&
      !hex && !EITHER(out, hexed)) {
    /* (A faded bar's pulse shows nothing.) */
    bool bar_gone = f.bar_fade >= 1 && out->bar_fade >= 1;
    if ((bar_gone || (fabs(f.level - out->bar_level) < 0.5 / 255 &&
                      fabs(f.percent - out->bar_percent) < 0.01 &&
                      f.bar_fade == out->bar_fade)) &&
        bsx == out->bar_sx && bsy == out->bar_sy) {
      if (wp_ticking() || wp_joining() || out->is_cover)
        request_frame(out);
      wl_surface_commit(out->surface);
      out->last_draw = now;
      wp_count_frame(now);
      return;
    }
    bar_only = true;
  }
  struct wp_rect r[WP_REGIONS];
  wp_regions_ex(wp_config(), pw, ph, ring, text, cf.logo_gpu, cf.logo_box, r);
  cairo_t *cr = cairo_create(b->cairo);
  if (bar_only) {
    cairo_rectangle(cr, r[0].x, r[0].y, r[0].w, r[0].h);
    cairo_clip(cr);
  }
  wp_render(cr, wp_config(), pw, ph, &cf, whole, ring, text);
  cairo_destroy(cr);
  cairo_surface_flush(b->cairo);
  out->painted[i] = (struct painted){
      .ringed = cf.nbars > 0,
      .still = wp_dozing(),
      /* (A bar-only repaint leaves what the buffer held round it.) */
      .text = bar_only ? p->text : tk,
      .showed = showing,
      .cpu_bright = bright,
      .shook = shaking,
      .cpu_drift = {cdx, cdy},
      .logoed = !cf.logo_gpu,
      .streaked = streaking,
      .hexed = bar_only ? p->hexed : hex,
      .wandered = wandering,
      .peeked = peeking,
  };
  if (whole || (text && !bar_only))
    out->text_shown = tk; /* damaged below */
  out->bar_fade = f.bar_fade;
  out->bar_level = f.level;
  out->bar_percent = f.percent;
  out->bar_sx = bsx;
  out->bar_sy = bsy;

  if (out->scale120 && out->viewport) {
    wl_surface_set_buffer_scale(out->surface, 1);
    wp_viewport_set_destination(out->viewport, out->width, out->height);
  } else {
    wl_surface_set_buffer_scale(out->surface, pw / out->width);
  }
  wl_surface_attach(out->surface, b->wl_buffer, 0, 0);
  /* The other buffer was last painted with the same background, so only
   * the bar and the logo can differ from what the compositor has. */
  if (whole)
    wl_surface_damage_buffer(out->surface, 0, 0, pw, ph);
  else {
    for (int k = 0; k < (bar_only ? 1 : WP_REGIONS); ++k)
      if (r[k].w > 0 && r[k].h > 0)
        wl_surface_damage_buffer(out->surface, r[k].x, r[k].y, r[k].w,
                                 r[k].h);
    if (peek_area) {
      struct wp_rect pr[WP_PEEK_REGIONS];
      wp_peek_regions(wp_config(), pw, ph, pr);
      for (int k = 0; k < WP_PEEK_REGIONS; ++k)
        if (pr[k].w > 0 && pr[k].h > 0)
          wl_surface_damage_buffer(out->surface, pr[k].x, pr[k].y, pr[k].w,
                                   pr[k].h);
    }
  }
  if (wp_ticking() || wp_joining() || out->is_cover)
    request_frame(out);
  wl_surface_commit(out->surface);
  b->busy = true;
  out->last_draw = now;
  wp_count_frame(now);
}

/* ---- surfaces ---- */

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *layer,
                            uint32_t serial, uint32_t w, uint32_t h) {
  struct output *out = data;
  zwlr_layer_surface_v1_ack_configure(layer, serial);
  bool changed = !out->configured || out->width != (int)w ||
                 out->height != (int)h;
  out->width = w;
  out->height = h;
  out->configured = true;
  if (changed)
    draw(out, true);
}

static void destroy_surface(struct output *out);

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *layer) {
  destroy_surface(data);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

static void preferred_scale(void *data, struct wp_fractional_scale_v1 *f,
                            uint32_t scale120) {
  struct output *out = data;
  if (out->scale120 == scale120)
    return;
  out->scale120 = scale120;
  draw(out, true);
}

static const struct wp_fractional_scale_v1_listener fractional_listener = {
    .preferred_scale = preferred_scale,
};

static void create_surface(struct output *out) {
  if (out->surface || !wl->layer_shell)
    return;
  out->surface = wl_compositor_create_surface(wl->compositor);
  if (wl->viewporter)
    out->viewport = wp_viewporter_get_viewport(wl->viewporter, out->surface);
  if (wl->fractional && out->viewport) {
    out->fractional = wp_fractional_scale_manager_v1_get_fractional_scale(
        wl->fractional, out->surface);
    wp_fractional_scale_v1_add_listener(out->fractional, &fractional_listener,
                                        out);
  }
  char ns[sizeof(wp_config()->namespace) + 8];
  snprintf(ns, sizeof ns, "%s%s", wp_config()->namespace,
           out->is_cover ? "-saver" : "");
  out->layer = zwlr_layer_shell_v1_get_layer_surface(
      wl->layer_shell, out->surface, out->so->wl_output,
      out->is_cover ? ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY
                    : ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
      ns);
  zwlr_layer_surface_v1_add_listener(out->layer, &layer_listener, out);
  zwlr_layer_surface_v1_set_anchor(out->layer,
                                   ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                       ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                       ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                       ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
  zwlr_layer_surface_v1_set_size(out->layer, 0, 0);
  zwlr_layer_surface_v1_set_exclusive_zone(out->layer, -1);
  zwlr_layer_surface_v1_set_keyboard_interactivity(
      out->layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
  if (out->is_cover) {
    /* Clicks go through to the windows (they come back as it fades). */
    struct wl_region *none = wl_compositor_create_region(wl->compositor);
    wl_surface_set_input_region(out->surface, none);
    wl_region_destroy(none);
    if (wl->alpha_modifier) {
      out->alpha =
          wp_alpha_modifier_v1_get_surface(wl->alpha_modifier, out->surface);
      out->mult = cover_mult();
      wp_alpha_modifier_surface_v1_set_multiplier(out->alpha, out->mult);
    }
  } else {
    wl_set_input(wl, out->surface, &wp_input, out);
  }
  wl_surface_commit(out->surface);
}

static void destroy_surface(struct output *out) {
  loop_timer_disarm(out->fps_timer);
  strips_destroy(out);
  strip_destroy(&out->map);
  out->under = false;
  strip_destroy(&out->logo);
  out->lgpu = out->logo_on = false;
  if (out->surface && !out->is_cover)
    wl_clear_input(wl, out->surface);
  if (out->alpha)
    wp_alpha_modifier_surface_v1_destroy(out->alpha);
  out->alpha = NULL;
  if (out->frame_cb)
    wl_callback_destroy(out->frame_cb);
  if (out->fractional)
    wp_fractional_scale_v1_destroy(out->fractional);
  if (out->viewport)
    wp_viewport_destroy(out->viewport);
  if (out->layer)
    zwlr_layer_surface_v1_destroy(out->layer);
  if (out->surface)
    wl_surface_destroy(out->surface);
  for (int i = 0; i < 2; ++i) {
    buffer_destroy(out->bufs[i]);
    out->bufs[i] = NULL;
  }
  out->frame_cb = NULL;
  out->fractional = NULL;
  out->viewport = NULL;
  out->layer = NULL;
  out->surface = NULL;
  out->configured = false;
  out->retry = false;
  out->held = false;
  out->presented = false;
  out->scale120 = 0;
  out->width = out->height = 0;
}

static void on_output(void *data, struct swell_output *so, bool added) {
  if (added) {
    struct output *out = xcalloc(1, sizeof(*out));
    out->so = so;
    out->fps_timer =
        loop_add_timer(wl->loop, CLOCK_MONOTONIC, on_fps_timer, out);
    wl_list_insert(outputs.prev, &out->link);
    create_surface(out);
    return;
  }
  struct output *out, *tmp;
  wl_list_for_each_safe(out, tmp, &outputs, link) {
    if (out->so != so)
      continue;
    if (out->cover) {
      destroy_surface(out->cover);
      loop_remove_timer(out->cover->fps_timer);
      free(out->cover);
    }
    destroy_surface(out);
    loop_remove_timer(out->fps_timer);
    wl_list_remove(&out->link);
    free(out);
  }
}

void wp_surfaces_init(struct swell_wl *w) {
  wl = w;
  wl_list_init(&outputs);
  struct swell_output *so;
  wl_list_for_each(so, &wl->outputs, link) if (so->done) on_output(NULL, so, true);
  wl_add_output_listener(wl, on_output, NULL);
}

void wp_surfaces_finish(void) {
  wl_remove_output_listener(wl, on_output, NULL);
  struct output *out, *tmp;
  wl_list_for_each_safe(out, tmp, &outputs, link) on_output(NULL, out->so, false);
}

void wp_surfaces_recreate(void) {
  struct output *out;
  wl_list_for_each(out, &outputs, link) {
    destroy_surface(out);
    create_surface(out);
    if (out->cover && out->cover->surface) {
      destroy_surface(out->cover);
      create_surface(out->cover);
    }
  }
}

void wp_surfaces_cover(void) {
  struct output *out;
  wl_list_for_each(out, &outputs, link) {
    if (!out->cover) {
      struct output *c = out->cover = xcalloc(1, sizeof(*c));
      c->so = out->so;
      c->is_cover = true;
      c->bg = out;
      c->fps_timer = loop_add_timer(wl->loop, CLOCK_MONOTONIC, on_fps_timer, c);
    }
    create_surface(out->cover); /* (if it hasn't one) */
  }
}

void wp_surfaces_draw(bool full) {
  struct output *out;
  wl_list_for_each(out, &outputs, link) {
    loop_timer_disarm(out->fps_timer);
    draw(out, full);
    if (out->cover && out->cover->surface) {
      loop_timer_disarm(out->cover->fps_timer);
      draw(out->cover, full);
    }
  }
}

int wp_surfaces_count(void) {
  int n = 0;
  struct output *out;
  wl_list_for_each(out, &outputs, link) n += out->surface != NULL;
  return n;
}

bool wp_surfaces_size(const void *o, int *w, int *h) {
  struct output *out;
  wl_list_for_each(out, &outputs, link) if (out == o && out->configured) {
    *w = out->width;
    *h = out->height;
    return true;
  }
  return false;
}
