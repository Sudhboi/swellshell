/* Offscreen render test (port of popup's test/render_test.c): draws a few
 * info variants and animation frames to PNGs at scale 2.
 * Usage: test_popup_render [outdir]   (default: current directory) */
#include <stdio.h>
#include <string.h>

#include "../png.h"
#include "module.h"
#include "popup/info.h"
#include "popup/render.h"

extern const struct swell_module popup_module;

static struct popup_config cfg;

static int dump_media(const struct info *in, const char *dir,
                      const char *name, double pop, double expand,
                      double media)
{
	cairo_t *cr = canvas(cfg.width, popup_surface_height(&cfg));
	popup_render(cr, &cfg, in, pop, expand, media);
	save_png(cr, dir, name);
	/* Fully popped, the panel's bottom edge rests at y = height. */
	int bad = pop >= 1 && (pixel(cr, 0.5, cfg.height - 4) != cfg.border ||
	                       pixel(cr, 4, cfg.height + 4) != 0);
	if (bad)
		fprintf(stderr, "%s: panel not resting on y = height\n", name);
	cairo_destroy(cr);
	return bad;
}

static int dump_anim(const struct info *in, const char *dir, const char *name,
                     double pop, double expand)
{
	return dump_media(in, dir, name, pop, expand, in->has_media);
}

static int dump(const struct info *in, const char *dir, const char *name)
{
	return dump_anim(in, dir, name, 1, 1);
}

static struct info normal(void)
{
	struct info in;
	memset(&in, 0, sizeof in);
	in.now = 1791295200 + 13 * 3600 + 47 * 60 + 9; /* fixed time, shown in local TZ */
	in.has_battery = true;
	in.battery = 78;
	in.charging = false;
	in.has_power = true;
	in.power_w = 7.4;
	in.has_volume = true;
	in.volume = 45;
	in.has_brightness = true;
	in.brightness = 60;
	in.net = NET_WIFI;
	snprintf(in.net_name, sizeof in.net_name, "home-5G");
	in.net_signal = 72;
	in.has_media = true;
	in.playing = true;
	snprintf(in.media_title, sizeof in.media_title, "Teardrop");
	snprintf(in.media_artist, sizeof in.media_artist, "Massive Attack");
	in.cpu = 12;
	in.mem_used_gib = 5.1;
	in.mem_total_gib = 15.5;
	in.uptime_s = 3 * 3600 + 12 * 60;
	in.ws_count = 5;
	for (int i = 0; i < in.ws_count; i++)
		in.ws[i] = (struct workspace){ .num = i + 1 };
	for (int i = 0; i < in.ws_count; i++)
		snprintf(in.ws[i].name, sizeof in.ws[i].name, "%d", i + 1);
	in.ws[2].focused = true;
	return in;
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : ".";
	int rc = 0;
	cfg = *(const struct popup_config *)popup_module.defaults;

	struct info a = normal();
	rc |= dump(&a, dir, "normal");
	rc |= dump_anim(&a, dir, "anim_pop_half", 0.5, 0);
	rc |= dump_anim(&a, dir, "anim_compact", 1, 0);
	rc |= dump_anim(&a, dir, "anim_expand_quarter", 1, 0.25);
	rc |= dump_anim(&a, dir, "anim_expand_half", 1, 0.5);
	rc |= dump_media(&a, dir, "anim_media_quarter", 1, 1, 0.25);
	rc |= dump_media(&a, dir, "anim_media_three_quarter", 1, 1, 0.75);

	struct info b = normal();
	b.muted = true;
	b.battery = 9;
	b.net = NET_OFFLINE;
	b.net_name[0] = '\0';
	b.has_media = false;
	b.cpu = -1;
	b.uptime_s = 7 * 60;
	b.ws_count = 2;
	b.ws[1].urgent = true;
	b.ws[2].focused = false;
	b.ws[0].focused = true;
	rc |= dump(&b, dir, "muted_low_offline");
	rc |= dump_anim(&b, dir, "anim_compact_muted_low", 1, 0);

	struct info c = normal();
	c.playing = false;
	snprintf(c.media_title, sizeof c.media_title,
	         "Symphony No. 9 in D minor, Op. 125 \"Choral\": IV. Presto "
	         "– Allegro assai – Ode an die Freude");
	snprintf(c.media_artist, sizeof c.media_artist,
	         "Berliner Philharmoniker, Herbert von Karajan");
	c.charging = true;
	c.has_power = true;
	c.power_charging = true;
	c.power_w = 38.71;
	c.battery = 100;
	c.volume = 130;
	c.net = NET_WIRED;
	snprintf(c.net_name, sizeof c.net_name, "Wired connection 1");
	c.uptime_s = 4 * 86400 + 5 * 3600 + 30 * 60;
	c.cpu = 100;
	c.ws_count = 12;
	for (int i = 5; i < c.ws_count; i++) {
		c.ws[i] = (struct workspace){ .num = i + 1 };
		snprintf(c.ws[i].name, sizeof c.ws[i].name, "%d", i + 1);
	}
	snprintf(c.ws[7].name, sizeof c.ws[7].name, "mail");
	c.ws[2].focused = false;
	c.ws[11].focused = true;
	rc |= dump(&c, dir, "long_media");
	/* the widest stats row: on battery, with every field long */
	struct info g = c;
	g.charging = false;
	g.power_charging = false;
	g.has_power = true;
	g.power_w = 45.62;
	g.mem_used_gib = 27.3;
	g.mem_total_gib = 31.0;
	rc |= dump(&g, dir, "wide_stats");
	rc |= dump_anim(&c, dir, "anim_compact_many_ws", 1, 0);
	rc |= dump_anim(&c, dir, "anim_expand_quarter_many_ws", 1, 0.25);

	struct info d = normal();
	d.has_battery = false;
	d.has_volume = false;
	d.has_brightness = false;
	d.net = NET_UNKNOWN;
	d.net_name[0] = '\0';
	d.media_artist[0] = '\0';
	d.charging = true;
	d.has_power = false;
	d.net_signal = 20;
	d.ws_count = 0;
	rc |= dump(&d, dir, "missing");
	rc |= dump_anim(&d, dir, "anim_compact_missing", 1, 0);

	struct info e = normal();
	e.charging = true;
	e.has_power = false;
	e.battery = 43;
	e.volume = 10;
	e.net_signal = 20;
	e.brightness = 0;
	e.uptime_s = 86400 + 59;
	rc |= dump(&e, dir, "weak_wifi");

	struct info f = normal();
	f.ws_count = 1;
	f.ws[2].focused = false;
	f.ws[0].focused = true;
	rc |= dump_anim(&f, dir, "anim_compact_one_ws", 1, 0);
	rc |= dump_anim(&f, dir, "anim_expand_quarter_one_ws", 1, 0.25);

	return rc;
}
