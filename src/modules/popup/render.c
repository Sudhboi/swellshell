/* Cairo drawing of the popup: clock, date, levels, network, media, stats. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "render.h"

/* Nerd Font icons, UTF-8 */
static const char *const ICON_BAT[] = {   /* 0, 10, 20 .. 100 % */
	"\U000F008E", "\U000F007A", "\U000F007B", "\U000F007C",
	"\U000F007D", "\U000F007E", "\U000F007F", "\U000F0080",
	"\U000F0081", "\U000F0082", "\U000F0079",
};
static const char *const ICON_BAT_CHG[] = {
	"\U000F089F", "\U000F089C", "\U000F0086", "\U000F0087",
	"\U000F0088", "\U000F089D", "\U000F0089", "\U000F089E",
	"\U000F008A", "\U000F008B", "\U000F0085",
};
static const char ICON_BAT_NONE[] = "\U000F0091";
static const char ICON_VOL_HIGH[] = "\U000F057E";
static const char ICON_VOL_MED[]  = "\U000F0580";
static const char ICON_VOL_LOW[]  = "\U000F057F";
static const char ICON_MUTED[]    = "\U000F0581";
static const char ICON_BRIGHT[]   = "\U000F00E0";
static const char *const ICON_WIFI[] = {  /* weakest .. strongest */
	"\U000F091F", "\U000F0922", "\U000F0925", "\U000F0928",
};
static const char ICON_WIRED[]    = "\U000F0200";
static const char ICON_OFFLINE[]  = "\U000F0319";
static const char ICON_NET_UNK[]  = "\U000F06F5";
static const char ICON_PLAY[]     = "\U000F040A";
static const char ICON_PAUSE[]    = "\U000F03E4";
static const char ELLIPSIS[]      = "…";

/* The config of the frame being drawn (set by popup_render). Every size is
 * logical pixels; see struct popup_config. */
static const struct popup_config *C;

/* The footer (separator + workspace strip) is pinned to the panel's bottom;
 * its separator's y in a panel of height h: */
static double footer_h(double h)
{
	return (double)(long)(h - C->pad - C->ws_h - C->gap_ws);
}

static void set_rgba(cairo_t *cr, uint32_t c, double alpha)
{
	cairo_set_source_rgba(cr,
		((c >> 24) & 0xff) / 255.0,
		((c >> 16) & 0xff) / 255.0,
		((c >> 8) & 0xff) / 255.0,
		(c & 0xff) / 255.0 * alpha);
}

static void add_stop(cairo_pattern_t *p, double off, uint32_t c)
{
	cairo_pattern_add_color_stop_rgba(p, off,
		((c >> 24) & 0xff) / 255.0,
		((c >> 16) & 0xff) / 255.0,
		((c >> 8) & 0xff) / 255.0,
		(c & 0xff) / 255.0);
}

static void font(cairo_t *cr, double size, bool bold)
{
	cairo_select_font_face(cr, C->font, CAIRO_FONT_SLANT_NORMAL,
	                       bold ? CAIRO_FONT_WEIGHT_BOLD
	                            : CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, size);
}

static double advance(cairo_t *cr, const char *s)
{
	cairo_text_extents_t ext;
	cairo_text_extents(cr, s, &ext);
	return ext.x_advance;
}

/* Height of a capital/digit above the baseline in the current font. */
static double cap_height(cairo_t *cr)
{
	cairo_text_extents_t ext;
	cairo_text_extents(cr, "H0", &ext);
	return -ext.y_bearing;
}

static void text_at(cairo_t *cr, double x, double y, const char *s,
                    uint32_t col)
{
	set_rgba(cr, col, 1.0);
	cairo_move_to(cr, x, y);
	cairo_show_text(cr, s);
	cairo_new_path(cr);
}

static void text_centred(cairo_t *cr, double cx, double y, const char *s,
                         uint32_t col)
{
	text_at(cr, cx - advance(cr, s) / 2.0, y, s, col);
}

/* Cut s (in place) on a UTF-8 boundary so that it plus "…" fits maxw. */
static void ellipsize(cairo_t *cr, char *s, size_t cap, double maxw)
{
	if (advance(cr, s) <= maxw)
		return;
	size_t n = strlen(s);
	char buf[512];
	while (n > 0) {
		do
			n--;
		while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80);
		size_t k = n;
		while (k > 0 && s[k - 1] == ' ')
			k--;
		snprintf(buf, sizeof buf, "%.*s%s", (int)k, s, ELLIPSIS);
		if (advance(cr, buf) <= maxw)
			break;
	}
	if (n == 0)
		snprintf(buf, sizeof buf, "%s", ELLIPSIS);
	snprintf(s, cap, "%s", buf);
}

static void draw_bar(cairo_t *cr, double x, double y, double w, int pct,
                     bool dim)
{
	set_rgba(cr, C->track, 1.0);
	cairo_rectangle(cr, x, y, w, C->bar_h);
	cairo_fill(cr);

	if (pct <= 0)
		return;
	double fw = w * (pct > 100 ? 100 : pct) / 100.0;
	if (dim) {
		set_rgba(cr, C->muted, 1.0);
	} else {
		cairo_pattern_t *g = cairo_pattern_create_linear(x, 0, x + w, 0);
		add_stop(g, 0.0, C->bar_dark);
		add_stop(g, 1.0, C->bar_light);
		cairo_set_source(cr, g);
		cairo_pattern_destroy(g);
	}
	cairo_rectangle(cr, x, y, fw, C->bar_h);
	cairo_fill(cr);
}

/* date_alpha (the expand) also fades in the seconds. */
static double draw_clock(cairo_t *cr, const struct info *in, double date_alpha)
{
	struct tm tm;
	char hhmm[16], ss[8], date[96];

	localtime_r(&in->now, &tm);
	strftime(hhmm, sizeof hhmm, "%H:%M", &tm);
	strftime(ss, sizeof ss, ":%S", &tm);

	font(cr, C->font_clock, true);
	double base = C->pad + cap_height(cr);
	double w_hm = advance(cr, hhmm);
	font(cr, C->font_seconds, false);
	double w_ss = advance(cr, ss);

	/* Clock-only, HH:MM is centred on its own; as the details reveal, it
	 * shifts left to make room and :SS fades in beside it. */
	double x = (C->width - w_hm) / 2.0 -
		(C->sec_gap + w_ss) / 2.0 * date_alpha;
	font(cr, C->font_clock, true);
	text_at(cr, x, base, hhmm, C->text);
	if (date_alpha > 0) {
		font(cr, C->font_seconds, false);
		cairo_push_group(cr);
		text_at(cr, x + w_hm + C->sec_gap, base, ss, C->dim);
		cairo_pop_group_to_source(cr);
		cairo_paint_with_alpha(cr, date_alpha);
	}

	/* "Tuesday, 6 October 2026" */
	char wday[32], rest[32];
	strftime(wday, sizeof wday, "%A", &tm);
	strftime(rest, sizeof rest, "%B %Y", &tm);
	snprintf(date, sizeof date, "%s, %d %s", wday, tm.tm_mday, rest);
	font(cr, C->font_date, false);
	base += C->gap_date + cap_height(cr);
	cairo_push_group(cr);
	text_centred(cr, C->width / 2.0, base, date, C->dim);
	cairo_pop_group_to_source(cr);
	cairo_paint_with_alpha(cr, date_alpha);
	return base;
}

static const char *battery_icon(const struct info *in)
{
	if (!in->has_battery)
		return ICON_BAT_NONE;
	int b = in->battery < 0 ? 0 : in->battery > 100 ? 100 : in->battery;
	int i = (b + 5) / 10;
	return in->charging ? ICON_BAT_CHG[i] : ICON_BAT[i];
}

static const char *volume_icon(const struct info *in)
{
	if (in->muted)
		return ICON_MUTED;
	if (in->volume >= 66)
		return ICON_VOL_HIGH;
	if (in->volume >= 33)
		return ICON_VOL_MED;
	return ICON_VOL_LOW;
}

/* A run of text pieces drawn side by side; icons are spaced by their ink
 * width since Nerd Font glyphs overhang their advance. */
struct piece {
	const char *s;
	uint32_t col;
	bool icon;
};

static double piece_width(cairo_t *cr, const struct piece *p)
{
	cairo_text_extents_t ext;
	cairo_text_extents(cr, p->s, &ext);
	if (!p->icon)
		return ext.x_advance;
	double ink = ext.x_bearing + ext.width;
	return (ink > ext.x_advance ? ink : ext.x_advance) + C->icon_gap;
}

static double pieces_width(cairo_t *cr, const struct piece *p, int n)
{
	double w = 0;
	for (int i = 0; i < n; i++)
		w += piece_width(cr, &p[i]);
	return w;
}

static void draw_pieces(cairo_t *cr, double cx, double base,
                        const struct piece *p, int n)
{
	double x = cx - pieces_width(cr, p, n) / 2.0;
	for (int i = 0; i < n; i++) {
		text_at(cr, x, base, p[i].s, p[i].col);
		x += piece_width(cr, &p[i]);
	}
}

/* One level column: "icon NN%" centred over a bar. */
static void draw_level(cairo_t *cr, double x, double w, double base,
                       const char *icon, bool has, int pct, uint32_t col,
                       bool dim_bar)
{
	char val[16];
	if (has)
		snprintf(val, sizeof val, "%d%%", pct);
	else
		snprintf(val, sizeof val, "--");
	if (!has)
		col = C->muted;
	struct piece p[] = { { icon, col, true }, { val, col, false } };
	draw_pieces(cr, x + w / 2.0, base, p, 2);
	draw_bar(cr, x + C->col_inset, base + C->gap_bar, w - 2 * C->col_inset,
	         has ? pct : 0, dim_bar);
}

static double draw_levels(cairo_t *cr, const struct info *in, double top)
{
	const double inner = C->width - 2 * C->pad;
	const double cw = inner / 3.0;

	font(cr, C->font_row, false);
	double base = top + cap_height(cr);

	bool low = in->has_battery && !in->charging &&
	           in->battery <= C->battery_low_pct;
	draw_level(cr, C->pad, cw, base, battery_icon(in),
	           in->has_battery, in->battery, low ? C->warn : C->text,
	           false);
	draw_level(cr, C->pad + cw, cw, base, volume_icon(in),
	           in->has_volume, in->volume,
	           in->muted ? C->muted : C->text, in->muted);
	draw_level(cr, C->pad + 2 * cw, cw, base, ICON_BRIGHT,
	           in->has_brightness, in->brightness, C->text, false);
	return base + C->gap_bar + C->bar_h;
}

/* One compact-panel indicator: icon and "NN%", centred vertically on cy.
 * The numbers are right-aligned to the pad and the icons sit in a column
 * left of the widest value, so they line up. */
static void draw_mini(cairo_t *cr, double cy, const char *icon, bool has,
                      int pct, uint32_t col)
{
	char val[16];
	if (has)
		snprintf(val, sizeof val, "%d%%", pct);
	else
		snprintf(val, sizeof val, "--");
	if (!has)
		col = C->muted;

	double right = C->width - C->pad;
	double base = cy + cap_height(cr) / 2.0;
	text_at(cr, right - advance(cr, val), base, val, col);

	cairo_text_extents_t ext;
	cairo_text_extents(cr, icon, &ext);
	double ix = right - advance(cr, "100%") - C->mini_gap;
	text_at(cr, ix - (ext.x_bearing + ext.width),
	        cy - ext.y_bearing - ext.height / 2.0, icon, col);
}

/* Battery, volume and brightness stacked to the right of the clock, spread
 * over its cap height; shown only on the clock-only panel. */
static void draw_minis(cairo_t *cr, const struct info *in)
{
	font(cr, C->font_clock, true);
	double cap = cap_height(cr);
	font(cr, C->mini_font, false);

	bool low = in->has_battery && !in->charging &&
	           in->battery <= C->battery_low_pct;
	double step = cap / 3.0, cy = C->pad + step / 2.0;
	draw_mini(cr, cy, battery_icon(in), in->has_battery, in->battery,
	          low ? C->warn : C->dim);
	draw_mini(cr, cy + step, volume_icon(in), in->has_volume, in->volume,
	          in->muted ? C->muted : C->dim);
	draw_mini(cr, cy + 2 * step, ICON_BRIGHT, in->has_brightness,
	          in->brightness, C->dim);
}

static void draw_net(cairo_t *cr, const struct info *in, double base)
{
	char sig[16];
	const char *name = in->net_name[0] ? in->net_name : NULL;
	struct piece p[3];
	int n = 2;

	switch (in->net) {
	case NET_WIFI: {
		int s = in->net_signal;
		int i = s < 0 ? 3 : s >= 75 ? 3 : s >= 50 ? 2 : s >= 25 ? 1 : 0;
		p[0] = (struct piece){ ICON_WIFI[i], C->text, true };
		p[1] = (struct piece){ name ? name : "wifi", C->text, false };
		if (s >= 0) {
			snprintf(sig, sizeof sig, "  %d%%", s);
			p[n++] = (struct piece){ sig, C->dim, false };
		}
		break;
	}
	case NET_WIRED:
		p[0] = (struct piece){ ICON_WIRED, C->text, true };
		p[1] = (struct piece){ name ? name : "wired", C->text, false };
		break;
	case NET_OFFLINE:
		p[0] = (struct piece){ ICON_OFFLINE, C->muted, true };
		p[1] = (struct piece){ "offline", C->muted, false };
		break;
	default:
		p[0] = (struct piece){ ICON_NET_UNK, C->dim, true };
		p[1] = (struct piece){ name ? name : "unknown", C->dim, false };
		break;
	}
	draw_pieces(cr, C->width / 2.0, base, p, n);
}

static void draw_media(cairo_t *cr, const struct info *in, double base)
{
	const char *title = in->media_title[0] ? in->media_title : "unknown";
	uint32_t col = in->playing ? C->text : C->dim;
	char text[512];

	if (in->media_artist[0])
		snprintf(text, sizeof text, "%s — %s", title,
		         in->media_artist);
	else
		snprintf(text, sizeof text, "%s", title);

	struct piece p[] = {
		{ in->playing ? ICON_PLAY : ICON_PAUSE, col, true },
		{ text, col, false },
	};
	double maxw = C->width - 2 * C->pad - piece_width(cr, &p[0]);
	ellipsize(cr, text, sizeof text, maxw);
	draw_pieces(cr, C->width / 2.0, base, p, 2);
}

static void draw_stats(cairo_t *cr, const struct info *in, double base)
{
	char cpu[16], pwr[24], mem[32], up[32];
	long m = in->uptime_s / 60, h = m / 60, d = h / 24;

	if (in->cpu < 0)
		snprintf(cpu, sizeof cpu, "--   ");
	else
		snprintf(cpu, sizeof cpu, "%d%%   ", in->cpu);
	snprintf(pwr, sizeof pwr, "%.1fW   ", in->power_w);
	snprintf(mem, sizeof mem, "%.1f/%.1fG   ",
	         in->mem_used_gib, in->mem_total_gib);
	if (d > 0)
		snprintf(up, sizeof up, "%ldd %ldh", d, h % 24);
	else if (h > 0)
		snprintf(up, sizeof up, "%ldh %02ldm", h, m % 60);
	else
		snprintf(up, sizeof up, "%ldm", m);

	/* The power draw is shown only on battery, after the CPU. */
	struct piece p[8] = {
		{ "CPU ", C->muted, false }, { cpu, C->dim, false },
	};
	int n = 2;
	if (in->has_power) {
		p[n++] = (struct piece){ "PWR ", C->muted, false };
		p[n++] = (struct piece){ pwr, C->dim, false };
	}
	p[n++] = (struct piece){ "RAM ", C->muted, false };
	p[n++] = (struct piece){ mem, C->dim, false };
	p[n++] = (struct piece){ "up ", C->muted, false };
	p[n++] = (struct piece){ up, C->dim, false };
	draw_pieces(cr, C->width / 2.0, base, p, n);
}

/* Height of the clock-only panel. */
static double compact_height(cairo_t *cr)
{
	font(cr, C->font_clock, true);
	return 2 * C->pad + cap_height(cr);
}

static double lerp(double a, double b, double t)
{
	return a + (b - a) * t;
}

/* --- workspaces ---
 * Clock-only: a column of three cells left of the clock (the focused one and
 * its neighbours). Expanded: a strip of all of them along the bottom. During
 * the expand the column's cells glide into their places in the strip. */

struct ws_layout {
	int first, n;           /* window into in->ws */
	double x, y, w, h;      /* first cell */
	double dx, dy;          /* step between cells */
	double font;
};

/* A window of up to max cells containing the focused workspace, centred on
 * it where possible. */
static int ws_focused(const struct info *in)
{
	int f = 0;
	for (int i = 0; i < in->ws_count; i++)
		if (in->ws[i].focused)
			f = i;
	return f;
}

static void ws_window(const struct info *in, int max, int *first, int *n)
{
	int f = ws_focused(in);
	*n = in->ws_count < max ? in->ws_count : max;
	int s = f - (*n - 1) / 2;
	if (s + *n > in->ws_count)
		s = in->ws_count - *n;
	*first = s < 0 ? 0 : s;
}

static double ws_text_w(cairo_t *cr, const struct info *in, int first, int n)
{
	double w = 0;
	for (int i = first; i < first + n; i++) {
		double a = advance(cr, in->ws[i].name);
		if (a > w)
			w = a;
	}
	return w;
}

static void ws_compact(cairo_t *cr, const struct info *in, struct ws_layout *l)
{
	font(cr, C->font_clock, true);
	double step = cap_height(cr) / 3.0;
	/* The focused workspace always takes the middle slot, so at the ends
	 * of the list a slot is left empty rather than shifting the window. */
	int f = ws_focused(in);
	l->first = f > 0 ? f - 1 : 0;
	l->n = (f + 2 < in->ws_count ? f + 2 : in->ws_count) - l->first;
	l->font = C->font_ws;
	font(cr, l->font, false);
	double w = ws_text_w(cr, in, l->first, l->n) + 2 * (C->ws_text_pad - 3);
	l->w = w < C->wsc_min_w ? C->wsc_min_w : w > C->wsc_max_w ? C->wsc_max_w : w;
	l->h = step - 2 * C->wsc_inset;
	l->x = C->pad;
	l->y = C->pad + C->wsc_inset + (l->first - (f - 1)) * step;
	l->dx = 0;
	l->dy = step;
}

static void ws_full(cairo_t *cr, const struct info *in, struct ws_layout *l)
{
	const double inner = C->width - 2 * C->pad;
	l->font = C->font_row;
	font(cr, l->font, false);
	double w = ws_text_w(cr, in, 0, in->ws_count) + 2 * C->ws_text_pad;
	l->w = w < C->ws_min_w ? C->ws_min_w : w > inner ? inner : w;
	int fit = (int)((inner + C->ws_gap) / (l->w + C->ws_gap));
	if (fit < in->ws_count)   /* leave room for the arrows */
		fit = (int)((inner - 2 * C->ws_arrow_w + C->ws_gap) / (l->w + C->ws_gap));
	ws_window(in, fit < 1 ? 1 : fit, &l->first, &l->n);
	l->h = C->ws_h;
	l->dx = l->w + C->ws_gap;
	l->dy = 0;
	l->x = (double)(long)((C->width - (l->n * l->dx - C->ws_gap)) / 2.0);
	l->y = -C->pad - C->ws_h;   /* from the panel bottom */
}

static void ws_cell(cairo_t *cr, const struct workspace *ws, double x,
                    double y, double w, double h, double size, double alpha)
{
	if (ws->focused) {
		set_rgba(cr, C->ws_focus, alpha);
		cairo_rectangle(cr, x, y, w, h);
		cairo_fill(cr);
	}
	char name[sizeof(ws->name) + 4];
	snprintf(name, sizeof name, "%s", ws->name);
	font(cr, size, ws->focused);
	ellipsize(cr, name, sizeof name, w - 6);
	uint32_t col = ws->urgent ? C->warn : ws->focused ? C->text : C->dim;
	set_rgba(cr, col, alpha);
	cairo_move_to(cr, x + (w - advance(cr, name)) / 2.0,
	              y + (h + cap_height(cr)) / 2.0);
	cairo_show_text(cr, name);
	cairo_new_path(cr);
}

/* Small "more" triangle at (cx, cy) pointing along (dx, dy), a unit axis. */
static void ws_arrow(cairo_t *cr, double cx, double cy, double dx, double dy,
                     double alpha)
{
	double a = C->wsc_arrow;
	set_rgba(cr, C->muted, alpha);
	cairo_move_to(cr, cx + dx * a, cy + dy * a);
	cairo_line_to(cr, cx - dy * a, cy + dx * a);
	cairo_line_to(cr, cx + dy * a, cy - dx * a);
	cairo_close_path(cr);
	cairo_fill(cr);
}

/* Arrows before/after a layout's cells when its window doesn't show all. */
static void ws_arrows(cairo_t *cr, const struct info *in,
                      const struct ws_layout *l, double off, double alpha)
{
	double ux = l->dx ? 1 : 0, uy = l->dy ? 1 : 0;
	double mx = l->x + l->w / 2.0, my = l->y + l->h / 2.0;
	/* half a cell plus off, along the layout axis */
	double bx = ux * (l->w / 2.0 + off), by = uy * (l->h / 2.0 + off);
	if (l->first > 0)
		ws_arrow(cr, mx - bx, my - by, -ux, -uy, alpha);
	if (l->first + l->n < in->ws_count) {
		double ex = mx + (l->n - 1) * l->dx, ey = my + (l->n - 1) * l->dy;
		ws_arrow(cr, ex + bx, ey + by, ux, uy, alpha);
	}
}

/* h is the current panel height. Cells are placed relative to the panel's
 * bottom edge, which stays put on screen while the panel grows upward, so
 * the column slides down and across into the strip instead of travelling
 * through the details. */
static void draw_workspaces(cairo_t *cr, const struct info *in, double e,
                            double h)
{
	if (in->ws_count <= 0)
		return;
	struct ws_layout c, f;
	ws_compact(cr, in, &c);
	ws_full(cr, in, &f);
	c.y += h - compact_height(cr);
	f.y += h;
	double fade = 1.0 - 2.0 * e;   /* compact-only parts, like the minis */
	double reveal = 2.0 * e - 1.0; /* strip-only parts, once there's room */

	for (int i = 0; i < in->ws_count; i++) {
		bool in_c = i >= c.first && i < c.first + c.n;
		bool in_f = i >= f.first && i < f.first + f.n;
		int ic = i - c.first, jf = i - f.first;
		double cx = c.x + ic * c.dx, cy = c.y + ic * c.dy;
		double fx = f.x + jf * f.dx, fy = f.y + jf * f.dy;
		if (in_c && in_f)
			ws_cell(cr, &in->ws[i], lerp(cx, fx, e), lerp(cy, fy, e),
			        lerp(c.w, f.w, e), lerp(c.h, f.h, e),
			        lerp(c.font, f.font, e), 1.0);
		else if (in_f && reveal > 0)
			ws_cell(cr, &in->ws[i], fx, fy, f.w, f.h, f.font, reveal);
		else if (in_c && fade > 0)
			ws_cell(cr, &in->ws[i], cx, cy, c.w, c.h, c.font, fade);
	}

	if (fade > 0)
		ws_arrows(cr, in, &c, C->wsc_inset + 6, fade);
	if (reveal > 0)
		ws_arrows(cr, in, &f, C->ws_arrow_w / 2.0, reveal);
}

/* Vertical layout below the date, from its baseline y. */
struct details_y {
	double sep;        /* upper separator */
	double rows;       /* first row baseline */
	double foot;       /* lower (footer) separator */
};

/* media (0..1) is how far the media row is opened up. */
static struct details_y details_layout(cairo_t *cr, double y, double media)
{
	struct details_y d;
	d.sep = (double)(long)(y + C->gap_sep);
	font(cr, C->font_row, false);
	double cap = cap_height(cr);
	/* draw_levels: cap top at sep + 1 + C->gap_cols, bars below the baseline */
	double bars = d.sep + 1 + C->gap_cols + cap + C->gap_bar + C->bar_h;
	d.rows = bars + C->gap_rows + cap;
	/* The last row sits as far above the footer separator as the level
	 * labels sit below the upper one. */
	double rows = 2 + media;
	d.foot = (double)(long)(d.rows + (rows - 1) * C->row_step + C->gap_cols + 0.5);
	return d;
}

/* Baseline of the date line, as drawn by draw_clock. */
static double date_baseline(cairo_t *cr)
{
	font(cr, C->font_clock, true);
	double base = C->pad + cap_height(cr);
	font(cr, C->font_date, false);
	return base + C->gap_date + cap_height(cr);
}

/* Height of the fully expanded panel; it depends on the content (the media
 * row), and is at most C->height. */
static double full_height(cairo_t *cr, double media)
{
	struct details_y d = details_layout(cr, date_baseline(cr), media);
	double h = d.foot + C->gap_ws + C->ws_h + C->pad;
	return h < C->height ? h : C->height;
}

static void draw_details(cairo_t *cr, const struct info *in, double y,
                         double media)
{
	struct details_y d = details_layout(cr, y, media);
	set_rgba(cr, C->track, 1.0);
	cairo_rectangle(cr, C->pad, d.sep, C->width - 2 * C->pad, 1);
	cairo_fill(cr);

	draw_levels(cr, in, d.sep + 1 + C->gap_cols);

	font(cr, C->font_row, false);
	double base = d.rows;
	draw_net(cr, in, base);
	base += C->row_step;
	/* Opening, the stats row moves down first and the media row fades in
	 * once there's room; closing runs the other way. The last track's text
	 * is kept in *in, so it can fade out after has_media drops. */
	double alpha = 2.0 * media - 1.0;
	if (alpha > 0) {
		cairo_push_group(cr);
		draw_media(cr, in, base);
		cairo_pop_group_to_source(cr);
		cairo_paint_with_alpha(cr, alpha);
	}
	draw_stats(cr, in, base + media * C->row_step);
}

void popup_render(cairo_t *cr, const struct popup_config *cfg,
                  const struct info *in, double pop, double expand,
                  double media)
{
	C = cfg;
	cairo_save(cr);

	/* The panel sits on the bottom of the C->height box and grows upward
	 * from clock-only to full_height; content keeps the full layout and is
	 * clipped to the panel. */
	double h = (double)(long)lerp(compact_height(cr), full_height(cr, media),
	                              expand);
	double top = C->height - h;
	/* Slide: at pop 0 the panel's top is at the bottom of the surface. */
	top += (double)(long)((1.0 - pop) * (h + C->bottom_gap));
	cairo_translate(cr, 0, top);

	set_rgba(cr, C->background, 1.0);
	cairo_rectangle(cr, 0, 0, C->width, h);
	cairo_fill(cr);
	set_rgba(cr, C->border, 1.0);
	cairo_set_line_width(cr, 1);
	cairo_rectangle(cr, 0.5, 0.5, C->width - 1, h - 1);
	cairo_stroke(cr);

	cairo_rectangle(cr, 1, 1, C->width - 2, h - 2);
	cairo_clip(cr);

	double y = draw_clock(cr, in, expand);
	/* The indicators clear out in the first half of the expand, before
	 * the full level columns have faded in. */
	double mini = 1.0 - 2.0 * expand;
	if (mini > 0) {
		cairo_push_group(cr);
		draw_minis(cr, in);
		cairo_pop_group_to_source(cr);
		cairo_paint_with_alpha(cr, mini);
	}
	if (expand > 0) {
		/* Details are revealed from behind the footer as it grows. */
		double foot = footer_h(h);
		cairo_save(cr);
		cairo_rectangle(cr, 0, 0, C->width, foot);
		cairo_clip(cr);
		cairo_push_group(cr);
		draw_details(cr, in, y, media);
		cairo_pop_group_to_source(cr);
		cairo_paint_with_alpha(cr, expand);
		cairo_restore(cr);

		if (in->ws_count > 0 && expand > 0.5) {
			set_rgba(cr, C->track, 2.0 * expand - 1.0);
			cairo_rectangle(cr, C->pad, foot,
			                C->width - 2 * C->pad, 1);
			cairo_fill(cr);
		}
	}
	draw_workspaces(cr, in, expand, h);

	cairo_restore(cr);
}
