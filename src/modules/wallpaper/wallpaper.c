/* wallpaper: the battery bar + Nix snowflake wallpaper, rendered live,
 * with an audio visualiser ringing the snowflake while sound plays.
 *
 * It animates only while the focused workspace is empty (workspace.empty /
 * workspace.occupied on the bus) and the user is not idle
 * (ext-idle-notify-v1). Idle on an empty workspace it dozes: only the
 * battery bar pulses (idle_bar_pulse). Otherwise no frame callbacks are
 * requested and no timer is armed: the last frame stays on screen and the
 * module costs nothing; that includes the PipeWire capture, which is torn
 * down (it is while dozing too). Moving
 * the pointer over the wallpaper spins the logo faster. */
#include "wallpaper.h"

#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "anim.h"
#include "battery.h"
#include "bus.h"
#include "cursor-shape-v1-client-protocol.h"
#include "ext-idle-notify-v1-client-protocol.h"
#include "ipc.h"
#include "loop.h"
#include "module.h"
#include "wm.h"
#include "util.h"

static const struct wallpaper_config defaults = {
    .namespace = "swell-wallpaper",
    .fps = 30,
    .fps_spin = 0,
    .fps_audio = 0,
    .idle_timeout_s = 300,
    .battery_bar = true,
    .idle_bar_pulse = true,
    .battery_interval_s = 30,
    .background = 0x000000ffu,
    .bar = 0xffffffffu,
    .logo = 0x181818ffu,
    .logo_alt = 0x111111ffu,
    .logo_glow = 0x5a5a5affu,
    .logo_glow_alt = 0x444444ffu,
    .style = "topo",
    .topo_bg_left = 0x000000ffu,
    .topo_bg_right = 0x000000ffu,
    .topo_line_left = 0xf5b88affu,
    .topo_line_mid = 0xa6e3a1ffu,
    .topo_line_right = 0x89dceaffu,
    .topo_seed = 7,
    .topo_new_map = true,
    .topo_angle = 0,
    .topo_levels = 16,
    .topo_index_every = 4,
    .topo_scale = 1500,
    .topo_warp = 0.9,
    .topo_line_width = 4,
    .topo_index_width = 1.5,
    .topo_line_alpha = 0.9,
    .topo_logo_fill = 0x2c2f40e0u,
    .topo_logo_glow = 0x3a3e54e0u,
    .topo_edge_a = 0xf5c2e7ffu,
    .topo_edge_mid = 0xfab387ffu,
    .topo_edge_b = 0x89b4faffu,
    .topo_edge_width = 6,
    .topo_shadow_px = 22,
    .topo_shadow_alpha = 0.6,
    .topo_fade_ms = 600,
    .topo_waves = true,
    .topo_ring_px = 28,
    .topo_ring_width = 160,
    .topo_ring_speed = 1600,
    .topo_ring_ms = 1800,
    .topo_sway_px = 10,
    .topo_text = 0xf0f0f0ffu,
    .topo_text_dim = 0xb8b8b8ffu,
    .topo_text_backdrop = 0.9,
    .bar_top = 10,
    .bar_width = 600,
    .bar_height = 10,
    .bar_border = 2,
    .bar_gap = 2,
    .bar_min = 25 / 255.0,
    .pulse_period_s = 4.0,
    .pulse_period_min_s = 0.5,
    .pulse_sigma_s = 0.4,
    .pulse_hold_s = 1.0,
    .tween_ms = 600,
    .logo_radius = 225,
    .spin_speed = 15,
    .spin_gain = 0.008,
    .spin_max = 100,
    .spin_decay_s = 1.2,
    .spin_hold_ms = 200,
    .spin_visible_max = 200,
    .bloom = 0x808080ffu,
    .bloom_radius = 1.6,
    .bloom_alpha = 0.25,
    .streak = 0xa0a0a0ffu,
    .streak_count = 9,
    .streak_length = 70,
    .streak_width = 5,
    .streak_alpha = 0.7,
    .viz = true,
    .viz_target = "",
    .viz_bars = 48,
    .viz_edges = true,
    .viz_edge_length = 600,
    .viz_edge_gap = 0,
    .viz_edge_margin = 0,
    .viz_edge_smooth = true,
    .viz_edge_blur = 1.5,
    .viz_edge_fade = 0.1,
    .viz_edge_rim = 0xa0a0a0ffu,
    .viz_edge_rim_width = 3,
    .viz_gpu = true,
    .viz_radius = 1.6,
    .viz_length = 260,
    .viz_inward = 0.4,
    .viz_width = 0.55,
    .viz_color = 0x5a5a5affu,
    .viz_alpha = 1.0,
    .viz_low_hz = 50,
    .viz_high_hz = 10000,
    .viz_fall = 4,
    .viz_rise_ms = 40,
    .viz_jitter = 0,
    .react_slide = false,
    .react_slide_dist = 0.1,
    .react_pulse = true,
    .react_pulse_scale = 0.1,
    .react_pulse_ms = 150,
    .react_pulse_min = 0.6,
    .react_glow = true,
    .react_glow_amount = 0.6,
    .react_glow_spin = 1.0,
    .react_glow_base = 0.5,
    .react_spin = true,
    .react_spin_gain = 6,
    .react_spin_kick = 0,
    .react_spin_kick_ms = 400,
    .react_shake = true,
    .react_shake_rise = 20,
    .react_shake_px = 14,
    .react_shake_ms = 200,
    .media = true,
    .media_player = "",
    .media_interval_ms = 2000,
    .media_progress = true,
    .media_chase_ms = 2000,
    .media_art = true,
    .media_art_alpha = 0.5,
    .media_line = 0xb4b4b4ffu,
    .media_track = 0x262626ffu,
    .media_line_width = 6,
    .media_hex_size = 0.9,
    .media_title = true,
    .media_font = "IosevkaTerm Nerd Font",
    .media_title_size = 44,
    .media_artist_size = 32,
    .media_text = 0x8a8a8affu,
    .media_dim = 0x5a5a5affu,
    .media_gap = 70,
    .media_max_width = 1400,
    .media_fade_ms = 400,
    .media_paused_alpha = 0.5,
    .intro = true,
    .intro_ms = 1100,
    .intro_stagger_ms = 90,
    .intro_dist = 4,
    .intro_turn = 120,
    .outro = true,
    .outro_ms = 1800,
    .outro_crt_chance = 0.15,
    .outro_crt_ms = 1500,
    .outro_hold_ms = 5000,
    .crt_color = 0xe8f0ffffu,
};

#define S struct wallpaper_config
static const struct cfg_field fields[] = {
    CFG_STRING(S, namespace, "layer-shell namespace of the surfaces"),
    CFG_INT(S, fps, 0, 1000,
            "frame rate cap at the resting spin (0 = the display rate)"),
    CFG_INT(S, fps_spin, 0, 1000,
            "frame rate cap while the pointer spins the logo faster (0 = "
            "the display rate)"),
    CFG_INT(S, fps_audio, 0, 1000,
            "frame rate cap while the visualiser shows (0 = the display "
            "rate)"),
    CFG_INT(S, idle_timeout_s, 0, 86400,
            "freeze after this long without input (0 = never)"),
    CFG_BOOL(S, battery_bar, "the pulsing battery bar at the top"),
    CFG_BOOL(S, idle_bar_pulse,
             "the battery bar keeps pulsing while idle on an empty workspace "
             "(the rest stays frozen; an open window freezes it too)"),
    CFG_INT(S, battery_interval_s, 1, 3600,
            "battery re-read period while animating"),
    CFG_COLOR(S, background, "background"),
    CFG_COLOR(S, bar, "battery bar at full pulse brightness"),
    CFG_COLOR(S, logo, "snowflake"),
    CFG_COLOR(S, logo_alt, "snowflake, every other lambda"),
    CFG_COLOR(S, logo_glow,
              "snowflake at the fastest spin (it brightens with the speed)"),
    CFG_COLOR(S, logo_glow_alt,
              "snowflake at the fastest spin, every other lambda"),
    CFG_STRING(S, style,
               "the look: \"topo\" (rainbow contour lines, a glass "
               "snowflake) or \"classic\" (black, the solid snowflake)"),
    CFG_COLOR(S, topo_bg_left, "topo background at the left (needs topo)"),
    CFG_COLOR(S, topo_bg_right, "topo background at the right"),
    CFG_COLOR(S, topo_line_left, "contour lines at the left"),
    CFG_COLOR(S, topo_line_mid, "contour lines in the middle"),
    CFG_COLOR(S, topo_line_right, "contour lines at the right"),
    CFG_INT(S, topo_seed, 0, 1000000, "which contour map (any number)"),
    CFG_BOOL(S, topo_new_map,
             "a new contour map and colour direction each time swell "
             "starts (ignores topo_seed, topo_angle)"),
    CFG_DOUBLE(S, topo_angle, 0, 360,
               "the map's colours run this many degrees clockwise from left "
               "to right"),
    CFG_INT(S, topo_levels, 1, 200,
            "contour lines from the lowest ground to the highest"),
    CFG_INT(S, topo_index_every, 0, 200,
            "every this many'th contour line is wider (0 = none)"),
    CFG_DOUBLE(S, topo_scale, 10, 100000, "the hills' size, px"),
    CFG_DOUBLE(S, topo_warp, 0, 2, "how much the hills meander"),
    CFG_DOUBLE(S, topo_line_width, 0, 100, "contour line width, px"),
    CFG_DOUBLE(S, topo_index_width, 1, 10,
               "the wider contour lines' width, x topo_line_width"),
    CFG_DOUBLE(S, topo_line_alpha, 0, 1, "contour line opacity"),
    CFG_COLOR(S, topo_logo_fill,
              "the glass snowflake's fill (its alpha lets the lines show "
              "through)"),
    CFG_COLOR(S, topo_logo_glow,
              "the glass snowflake's fill when it glows (music, spin)"),
    CFG_COLOR(S, topo_edge_a, "the glass snowflake's outline at the top left"),
    CFG_COLOR(S, topo_edge_mid,
              "the glass snowflake's outline across the middle"),
    CFG_COLOR(S, topo_edge_b,
              "the glass snowflake's outline at the bottom right"),
    CFG_DOUBLE(S, topo_edge_width, 0, 100,
               "the glass snowflake's outline width, px"),
    CFG_DOUBLE(S, topo_shadow_px, 0, 500,
               "the glass snowflake's shadow, px down and right"),
    CFG_DOUBLE(S, topo_shadow_alpha, 0, 1, "the shadow's darkness"),
    CFG_INT(S, topo_fade_ms, 0, 10000,
            "the map fades out when a window opens on the workspace and back "
            "in when the last one goes"),
    CFG_BOOL(S, topo_waves,
             "the contour lines move with the music instead of the edge "
             "visualiser: bass kicks send rings out from the snowflake, "
             "loudness sways them (needs viz and a GPU; without one they "
             "stay still)"),
    CFG_DOUBLE(S, topo_ring_px, 0, 1000,
               "how far a ring moves the lines at the hardest kick, px"),
    CFG_DOUBLE(S, topo_ring_width, 1, 10000, "a ring's width, px"),
    CFG_DOUBLE(S, topo_ring_speed, 1, 100000, "how fast rings spread, px/s"),
    CFG_INT(S, topo_ring_ms, 1, 60000, "how long a ring lasts"),
    CFG_DOUBLE(S, topo_sway_px, 0, 1000,
               "how far the lines sway at full loudness, px"),
    CFG_COLOR(S, topo_text, "title over the map"),
    CFG_COLOR(S, topo_text_dim, "artist over the map"),
    CFG_DOUBLE(S, topo_text_backdrop, 0, 1,
               "the dark glow behind the title and artist, hiding the lines"),
    CFG_DOUBLE(S, bar_top, 0, 10000,
               "bar top edge (px on a 2880 px wide output; all sizes scale "
               "with the output width)"),
    CFG_DOUBLE(S, bar_width, 1, 10000, "bar length, px"),
    CFG_DOUBLE(S, bar_height, 1, 1000, "bar thickness, px"),
    CFG_DOUBLE(S, bar_border, 0, 100, "outline width, px"),
    CFG_DOUBLE(S, bar_gap, 0, 100, "gap between the outline and the fill, px"),
    CFG_DOUBLE(S, bar_min, 0, 1, "bar brightness between pulses (0..1)"),
    CFG_DOUBLE(S, pulse_period_s, 0.01, 3600,
               "time between pulses at 50% (scales with the percent)"),
    CFG_DOUBLE(S, pulse_period_min_s, 0.01, 3600,
               "shortest time between pulses (near 0%)"),
    CFG_DOUBLE(S, pulse_sigma_s, 0.001, 3600,
               "pulse fade width at 50% (Gaussian sigma)"),
    CFG_DOUBLE(S, pulse_hold_s, 0, 3600,
               "time at full brightness per pulse at 50%"),
    CFG_INT(S, tween_ms, 0, 60000, "fill change animation"),
    CFG_DOUBLE(S, logo_radius, 0, 10000, "snowflake radius, px"),
    CFG_DOUBLE(S, spin_speed, -3600, 3600,
               "snowflake rotation, degrees/s clockwise"),
    CFG_DOUBLE(S, spin_gain, 0, 100,
               "extra speed (x spin_speed) per px of pointer travel"),
    CFG_DOUBLE(S, spin_max, 0, 1000, "most extra speed (x spin_speed)"),
    CFG_DOUBLE(S, spin_decay_s, 0.001, 3600,
               "extra speed decay time constant"),
    CFG_INT(S, spin_hold_ms, 0, 60000,
            "the extra speed only decays once the pointer has been still "
            "this long, so it keeps building while the pointer moves"),
    CFG_DOUBLE(S, spin_visible_max, 0, 3600,
               "fastest the snowflake visibly turns, degrees/s; more speed "
               "shows as a bloom and speed lines"),
    CFG_COLOR(S, bloom, "glow behind the snowflake past spin_visible_max"),
    CFG_DOUBLE(S, bloom_radius, 1, 3,
               "bloom radius at the fastest spin (x logo_radius)"),
    CFG_DOUBLE(S, bloom_alpha, 0, 1, "bloom opacity at the fastest spin"),
    CFG_COLOR(S, streak, "speed lines around the snowflake"),
    CFG_INT(S, streak_count, 0, 64, "speed lines (0 = none)"),
    CFG_DOUBLE(S, streak_length, 0, 360,
               "speed line length at the fastest spin, degrees"),
    CFG_DOUBLE(S, streak_width, 0, 100, "speed line thickness, px"),
    CFG_DOUBLE(S, streak_alpha, 0, 1,
               "speed line opacity at the fastest spin"),
    CFG_BOOL(S, viz,
             "audio visualiser, along the screen's sides or around the "
             "snowflake (PipeWire; only while "
             "the wallpaper animates and something plays)"),
    CFG_STRING(S, viz_target,
               "PipeWire sink to visualise (node name or serial; \"\" = the "
               "default sink)"),
    CFG_INT(S, viz_bars, 1, WP_VIZ_MAX,
            "visualiser bars per side (left channel on the left, right on "
            "the right)"),
    CFG_BOOL(S, viz_edges,
             "bars along the screen's left and right edges, growing inwards; "
             "false: a ring around the snowflake"),
    CFG_DOUBLE(S, viz_edge_length, 0, 10000,
               "edge bar length at full level, px (needs viz_edges)"),
    CFG_DOUBLE(S, viz_edge_gap, 0, 10000,
               "edge bars' distance from the screen's side, px (needs "
               "viz_edges)"),
    CFG_DOUBLE(S, viz_edge_margin, 0, 10000,
               "space left free above and below the edge bars, px (needs "
               "viz_edges)"),
    CFG_BOOL(S, viz_edge_smooth,
             "one smooth filled curve through the levels on each side "
             "instead of separate bars (needs viz_edges)"),
    CFG_DOUBLE(S, viz_edge_blur, 0, 32,
               "how far each level spreads over its neighbours in the smooth "
               "curve, in bars (0 = through every level exactly; needs "
               "viz_edge_smooth)"),
    CFG_DOUBLE(S, viz_edge_fade, 0, 1,
               "the smooth curve's fill opacity at full length, as a share of "
               "its opacity at the screen's side (1 = flat; needs "
               "viz_edge_smooth)"),
    CFG_COLOR(S, viz_edge_rim,
              "the line along the smooth curve (needs viz_edge_smooth)"),
    CFG_DOUBLE(S, viz_edge_rim_width, 0, 100,
               "width of the line along the smooth curve, px (0 = none; needs "
               "viz_edge_smooth)"),
    CFG_BOOL(S, viz_gpu,
             "draw the smooth edge curve (needs viz_edge_smooth) and the "
             "topo map, with its waves, with the GPU (EGL); falls back to "
             "the CPU if that fails"),
    CFG_DOUBLE(S, viz_radius, 0, 10,
               "the line the ring's bars grow from, both ways (x "
               "logo_radius)"),
    CFG_DOUBLE(S, viz_length, 0, 10000,
               "ring bar length at full level, both ends together, px"),
    CFG_DOUBLE(S, viz_inward, 0, 1,
               "share of a ring bar's length that grows inwards (0 = all "
               "outwards from viz_radius)"),
    CFG_DOUBLE(S, viz_width, 0, 1, "bar thickness, as a fraction of its slot"),
    CFG_COLOR(S, viz_color, "visualiser bars"),
    CFG_DOUBLE(S, viz_alpha, 0, 1, "visualiser opacity"),
    CFG_DOUBLE(S, viz_low_hz, 1, 100000, "lowest frequency (the bottom bars)"),
    CFG_DOUBLE(S, viz_high_hz, 1, 100000, "highest frequency (the top bars)"),
    CFG_DOUBLE(S, viz_fall, 0.01, 1000,
               "how fast the bars fall (gravity, full heights per s^2)"),
    CFG_DOUBLE(S, viz_rise_ms, 0, 1000,
               "how smoothly the bars rise: the time to cover ~63% of a jump, "
               "ms (0 = at once)"),
    CFG_DOUBLE(S, viz_jitter, 0, 1,
               "irregularity: each bar's angle, width, length and gain vary "
               "a little (0 = a perfect ring)"),
    CFG_BOOL(S, react_slide,
             "the snowflake's lambdas slide outwards with the music, each by "
             "its own slice of the spectrum (needs viz)"),
    CFG_DOUBLE(S, react_slide_dist, 0, 1,
               "lambda slide at full level (x logo_radius)"),
    CFG_BOOL(S, react_pulse, "the snowflake grows on bass hits (needs viz)"),
    CFG_DOUBLE(S, react_pulse_scale, 0, 1,
               "growth per bass hit (x logo_radius)"),
    CFG_DOUBLE(S, react_pulse_min, 0, 1,
               "growth for the softest bass hit, of react_pulse_scale; bigger "
               "hits grow more, fully from a 15 dB jump"),
    CFG_INT(S, react_pulse_ms, 1, 10000, "how fast a bass hit eases back"),
    CFG_BOOL(S, react_glow,
             "the snowflake brightens with loudness (needs viz)"),
    CFG_DOUBLE(S, react_glow_base, 0, 1,
               "brightness whenever music plays, of the way to logo_glow; "
               "the loudness and spin glow build on top (needs react_glow)"),
    CFG_DOUBLE(S, react_glow_spin, 0, 1,
               "brightness as the music spins the snowflake: this much of "
               "the way to logo_glow at spin_visible_max (needs react_glow and "
               "react_spin)"),
    CFG_DOUBLE(S, react_glow_amount, 0, 1,
               "brightness at full loudness, of the way to logo_glow"),
    CFG_BOOL(S, react_spin,
             "loudness and beats speed the snowflake up (needs viz)"),
    CFG_DOUBLE(S, react_spin_gain, 0, 100,
               "extra speed at full loudness (x spin_speed)"),
    CFG_DOUBLE(S, react_spin_kick, 0, 100,
               "extra speed per beat (x spin_speed)"),
    CFG_INT(S, react_spin_kick_ms, 1, 10000,
            "how fast a beat's extra speed eases off"),
    CFG_BOOL(S, react_shake,
             "the whole wallpaper shakes on the biggest bass hits (needs "
             "viz)"),
    CFG_DOUBLE(S, react_shake_rise, 6, 100,
               "how many dB a bass hit must jump to shake (most jump 6-10)"),
    CFG_DOUBLE(S, react_shake_px, 0, 200,
               "shake distance at its start (reference px)"),
    CFG_INT(S, react_shake_ms, 1, 10000, "how fast a shake dies down"),
    CFG_BOOL(S, media,
             "show what's playing (playerctl): the title and artist under "
             "the snowflake, its progress on the lambdas"),
    CFG_STRING(S, media_player,
               "playerctl player to follow (its -p; \"\" = playerctl's "
               "pick)"),
    CFG_INT(S, media_interval_ms, 100, 60000,
            "how often playerctl is asked while the wallpaper animates"),
    CFG_BOOL(S, media_progress,
             "a line runs clockwise round the snowflake's hexagonal hole from "
             "its top as the track plays; without a track length a sixth of "
             "it keeps running round"),
    CFG_INT(S, media_chase_ms, 100, 60000,
            "without a track length, the running line moves a sixth of the "
            "way round in this long"),
    CFG_COLOR(S, media_line,
              "the progress line (classic; topo uses the outline's colours)"),
    CFG_COLOR(S, media_track,
              "the rest of the hexagon's border (classic; topo: the outline's "
              "colours, as faint)"),
    CFG_DOUBLE(S, media_line_width, 0, 100,
               "the progress line's width, px (0 = no line)"),
    CFG_BOOL(S, media_art,
             "the track's cover, in grey, fills the hexagon (file:// covers "
             "are read directly, http(s) ones fetched with curl)"),
    CFG_DOUBLE(S, media_art_alpha, 0, 1, "the cover's opacity"),
    CFG_DOUBLE(S, media_hex_size, 0, 1,
               "the hexagon's size, of the snowflake's hole"),
    CFG_BOOL(S, media_title,
             "show the title and artist under the snowflake (false: only the "
             "cover and progress)"),
    CFG_STRING(S, media_font, "font of the title and artist"),
    CFG_DOUBLE(S, media_title_size, 1, 1000, "title size, px"),
    CFG_DOUBLE(S, media_artist_size, 1, 1000, "artist size, px"),
    CFG_COLOR(S, media_text, "title"),
    CFG_COLOR(S, media_dim, "artist"),
    CFG_DOUBLE(S, media_gap, -10000, 10000,
               "space between the snowflake (at its biggest pulse) and the "
               "title, px"),
    CFG_DOUBLE(S, media_max_width, 1, 10000,
               "widest title or artist, px (longer ones end in an ellipsis)"),
    CFG_INT(S, media_fade_ms, 0, 10000,
            "how long the details take to fade in or out"),
    CFG_DOUBLE(S, media_paused_alpha, 0, 1,
               "opacity of the details and the hexagon while paused"),
    CFG_BOOL(S, intro,
             "launch animation: the lambdas fly in and lock together, when "
             "the daemon starts on an empty workspace (once per login)"),
    CFG_INT(S, intro_ms, 0, 10000, "each lambda's flight"),
    CFG_INT(S, intro_stagger_ms, 0, 2000,
            "between one lambda's start and the next one's"),
    CFG_DOUBLE(S, intro_dist, 0, 100,
               "where the lambdas start, in logo radii from the centre"),
    CFG_DOUBLE(S, intro_turn, -3600, 3600,
               "the snowflake turns this many degrees into place as they "
               "fly in"),
    CFG_BOOL(S, outro,
             "power-off animation (`swell power off`) on an empty workspace: "
             "the lambdas scatter"),
    CFG_INT(S, outro_ms, 1, 10000, "the scatter's length"),
    CFG_DOUBLE(S, outro_crt_chance, 0, 1,
               "chance of an old CRT's collapse instead of the scatter"),
    CFG_INT(S, outro_crt_ms, 1, 10000, "the CRT collapse's length"),
    CFG_INT(S, outro_hold_ms, 0, 600000,
            "the screen stays black this long after a power-off animation, "
            "then the wallpaper comes back (if the system is still up)"),
    CFG_COLOR(S, crt_color, "the CRT's dot and line"),
};
#undef S

static struct {
  struct swell *sw;
  struct wallpaper_config cfg;
  bool ws_empty, idle, animating;
  bool dozing; /* idle on an empty workspace: only the bar pulses */

  /* battery: percent is the target, -1 when no battery was found */
  int battery;
  int override; /* -1: auto */
  struct loop_timer *battery_timer;

  /* idle */
  struct ext_idle_notification_v1 *idle_note;
  struct loop_timer *idle_timer; /* fallback without ext-idle-notify */

  /* animation */
  int64_t last; /* ms of the last advance */
  double angle, boost, phase;
  double tween_from, tween_to, tween_elapsed; /* percent, percent, ms */
  bool have_pointer;
  double px, py;
  int64_t last_motion; /* ms of the last boosting pointer motion */
  struct wp_viz viz;
  double viz_time; /* s, drives the bars' wobble */
  struct wp_react react;
  double spin_glow; /* spin_glow(), smoothed so beats swell, not flash */
  double playing;   /* 0..1: music is playing, faded in and out */

  /* what's playing: the details shown (kept through a fade-out) */
  char title[256], artist[256];
  double media_alpha; /* the details' opacity, following the player */
  double progress;    /* the track's, 0..1, -1: none */
  double chase;       /* the running line, 0..6, while the length is unknown */
  bool chasing;

  /* the launch or power-off animation: a power-off ends in black, which
   * stays outro_hold_ms (no frames), then fades back over SHOW_RETURN_MS */
  enum wp_show show;
  int64_t show_start; /* ms */
  bool show_black;    /* a power-off's black frame has been drawn */
  struct loop_timer *hold_timer;

  /* the topo map: shown on an empty workspace, faded out under windows */
  double map_alpha;
  int64_t map_last; /* ms of its last step */
  /* rings spreading from bass kicks (topo_waves) */
  struct {
    double age, size; /* s; react_pulse_min..1 */
  } rings[WP_RINGS];
  int nrings;
  unsigned kicks_seen;

  /* measured frame rate */
  int64_t fps_start;
  int fps_frames;
  double fps;
} w;

const struct wallpaper_config *wp_config(void) { return &w.cfg; }
bool wp_animating(void) { return w.animating; }
bool wp_dozing(void) { return w.dozing; }

#define SHOW_RETURN_MS 800

static bool outro_show(void) {
  return w.show == WP_SHOW_SCATTER || w.show == WP_SHOW_CRT;
}

/* After a power-off: black, waiting for hold_timer. */
static bool show_holding(void) {
  return outro_show() && w.show_black &&
         now_ms() - w.show_start <
             wp_show_ms(&w.cfg, w.show) + (int64_t)w.cfg.outro_hold_ms;
}

bool wp_showing(void) { return w.show != WP_SHOW_NONE; }

static double map_target(void) { return w.ws_empty ? 1 : 0; }

/* The map is fading out or in (only the topo style has one). */
static bool map_fading(void) {
  return wp_topo(&w.cfg) && fabs(w.map_alpha - map_target()) > 1e-6;
}

bool wp_ticking(void) {
  if (show_holding())
    return false;
  return w.animating || w.dozing || wp_showing() || map_fading();
}

static bool viz_showing(void) {
  int n = w.cfg.viz_bars;
  for (int s = 0; s < 2; ++s)
    for (int i = 0; i < n; ++i)
      if (w.viz.bars[s][i] >= 1e-3)
        return true;
  return false;
}

/* The logo still shows the music (it lingers a little after the bars). */
static bool reacting(void) {
  if (w.react.loud > 1e-3 || w.react.pulse > 0 || w.react.kick > 0 ||
      w.react.shake > 0 ||
      w.spin_glow > 1e-3 || w.playing > 1e-3 || w.nrings > 0)
    return true;
  for (int k = 0; k < 6; ++k)
    if (w.react.slices[k] > 1e-3)
      return true;
  return false;
}

/* The resting spin is slow enough for a low rate; a pointer-boosted one
 * and moving bars need more frames to look smooth. */
int wp_fps_cap(void) {
  if (wp_showing() || map_fading())
    return 0;
  if (w.boost > 0.05)
    return w.cfg.fps_spin;
  return viz_showing() || reacting() ? w.cfg.fps_audio : w.cfg.fps;
}

/* ---- animation ---- */

static double target_percent(void) {
  if (w.override >= 0)
    return w.override;
  return w.battery >= 0 ? w.battery : 100;
}

static double shown_percent(void) {
  double t = w.cfg.tween_ms > 0 ? w.tween_elapsed / w.cfg.tween_ms : 1;
  return w.tween_from + (w.tween_to - w.tween_from) * ease_out_cubic(t);
}

static void set_target(double pct) {
  if (pct == w.tween_to)
    return;
  if (wp_ticking()) {
    w.tween_from = shown_percent();
    w.tween_elapsed = 0;
  } else {
    w.tween_from = pct; /* nobody sees a tween while frozen */
  }
  w.tween_to = pct;
  if (!wp_ticking())
    wp_surfaces_draw(false);
}

/* Returns the time advanced, in seconds. bar_only: the logo stays put. */
static double advance(int64_t now, bool bar_only) {
  /* Clamped, so a stall (or a freeze) doesn't make anything jump. */
  double dt = (now - w.last) / 1000.0;
  w.last = now;
  if (dt <= 0)
    return 0;
  if (dt > 0.1)
    dt = 0.1;
  w.tween_elapsed += dt * 1000;
  w.phase = fmod(w.phase + dt / wp_pulse_period(&w.cfg, shown_percent()), 1);
  if (bar_only)
    return dt;
  double music =
      w.cfg.react_spin ? w.cfg.react_spin_gain * w.react.energy + w.react.kick
                       : 0;
  double omega = w.cfg.spin_speed * (1 + w.boost + music);
  omega = copysign(fmin(fabs(omega), w.cfg.spin_visible_max), omega) * M_PI /
          180;
  w.angle = fmod(w.angle + omega * dt, 2 * M_PI);
  if (now - w.last_motion >= w.cfg.spin_hold_ms)
    w.boost *= exp(-dt / w.cfg.spin_decay_s);
  if (w.boost < 1e-3)
    w.boost = 0;
  return dt;
}

static double spin_glow(void);

/* Age the rings by dt and start one on a new kick. */
static void advance_rings(double dt) {
  int n = 0;
  for (int i = 0; i < w.nrings; ++i) {
    w.rings[i].age += dt;
    if (w.rings[i].age * 1000 < w.cfg.topo_ring_ms)
      w.rings[n++] = w.rings[i];
  }
  w.nrings = n;
  if (w.react.kicks == w.kicks_seen)
    return;
  w.kicks_seen = w.react.kicks;
  if (!wp_topo(&w.cfg) || !w.cfg.topo_waves)
    return;
  if (w.nrings == WP_RINGS) /* the oldest makes way */
    memmove(w.rings, w.rings + 1, sizeof(w.rings[0]) * --w.nrings);
  w.rings[w.nrings].age = 0;
  w.rings[w.nrings++].size = w.react.kick_size;
}

/* Fade the map towards the workspace's state. */
static void advance_map(int64_t now) {
  double dt = fmin(fmax((now - w.map_last) / 1000.0, 0), 0.1);
  w.map_last = now;
  double target = map_target();
  if (w.cfg.topo_fade_ms <= 0 || !wp_topo(&w.cfg)) {
    w.map_alpha = target;
    return;
  }
  double step = dt * 1000 / w.cfg.topo_fade_ms;
  w.map_alpha = target > w.map_alpha ? fmin(w.map_alpha + step, target)
                                     : fmax(w.map_alpha - step, target);
}

/* Move the bars on by dt towards the latest audio (silence without any). */
static void advance_viz(double dt) {
  if (dt <= 0)
    return;
  static float samples[2][WP_VIZ_FFT];
  float db[2][WP_VIZ_MAX];
  int rate;
  bool fresh = w.cfg.viz && wp_audio_samples(samples[0], samples[1], &rate);
  if (fresh)
    for (int s = 0; s < 2; ++s)
      wp_viz_analyse(samples[s], rate, &w.cfg, db[s]);
  wp_viz_step(&w.viz, &w.cfg, fresh ? (const float (*)[WP_VIZ_MAX])db : NULL,
              dt);
  wp_react_step(&w.react, &w.viz, &w.cfg, dt);
  advance_rings(dt);
  double target = spin_glow();
  double tau = target > w.spin_glow ? 0.3 : 0.8;
  w.spin_glow = target + (w.spin_glow - target) * exp(-dt / tau);
  target = viz_showing() ? 1 : 0;
  tau = target > w.playing ? 0.4 : 1.5;
  w.playing = target + (w.playing - target) * exp(-dt / tau);
  w.viz_time = fmod(w.viz_time + dt, 3600);
}

/* Move the details and the hexagon on by dt towards the player. */
static void advance_media(int64_t now, double dt) {
  const struct wp_media *m = wp_media_get();
  bool has = w.cfg.media && m->has;
  if (has) {
    snprintf(w.title, sizeof w.title, "%s", m->title);
    snprintf(w.artist, sizeof w.artist, "%s", m->artist);
  }
  double target = has ? (m->playing ? 1 : w.cfg.media_paused_alpha) : 0;
  double step = w.cfg.media_fade_ms > 0 ? dt * 1000 / w.cfg.media_fade_ms : 1;
  w.media_alpha = target > w.media_alpha
                      ? fmin(w.media_alpha + step, target)
                      : fmax(w.media_alpha - step, target);
  w.progress = has && w.cfg.media_progress ? wp_media_progress(m, now) : -1;
  /* No length: a lit wedge goes round while it plays. */
  w.chasing = has && w.cfg.media_progress && w.progress < 0;
  if (w.chasing && m->playing)
    w.chase = fmod(w.chase + dt * 1000 / w.cfg.media_chase_ms, 6);
}

/* The logo brightens with the pointer boost, fully at spin_max. */
static double glow(void) {
  return w.cfg.spin_max > 0 ? fmin(w.boost / w.cfg.spin_max, 1) : 0;
}

/* How fast the music spins the logo, 0 (resting) .. 1 (spin_visible_max). */
static double spin_glow(void) {
  if (!w.cfg.react_spin)
    return 0;
  double rest = fabs(w.cfg.spin_speed), cap = w.cfg.spin_visible_max;
  double music = w.cfg.react_spin_gain * w.react.energy + w.react.kick;
  double v = fmin(rest * (1 + music), cap);
  return cap > rest ? fmin(fmax((v - rest) / (cap - rest), 0), 1) : 0;
}

/* Speed past spin_visible_max, as a fraction of the most there can be. */
static double over(void) {
  double cap = w.cfg.spin_visible_max;
  double v = fabs(w.cfg.spin_speed) * (1 + w.boost);
  double vmax = fabs(w.cfg.spin_speed) * (1 + w.cfg.spin_max);
  return vmax > cap ? fmin(fmax((v - cap) / (vmax - cap), 0), 1) : 0;
}

/* Turn f into the show's frame at now, and end the show when it's over. */
static void show_frame(int64_t now, struct wp_frame *f) {
  if (!wp_showing())
    return;
  double t = now - w.show_start;
  int len = wp_show_ms(&w.cfg, w.show);
  if (w.show == WP_SHOW_INTRO) {
    if (t >= len)
      w.show = WP_SHOW_NONE;
    else
      wp_show_apply(&w.cfg, w.show, t, f);
    return;
  }
  double hold_end = len + (double)w.cfg.outro_hold_ms;
  if (t < hold_end) {
    wp_show_apply(&w.cfg, w.show, t, f);
    if (t >= len)
      w.show_black = true;
    return;
  }
  double u = (t - hold_end) / SHOW_RETURN_MS;
  if (u >= 1) {
    w.show = WP_SHOW_NONE;
    return;
  }
  f->fade = 1 - u;
}

void wp_frame_at(int64_t now, struct wp_frame *f) {
  if (w.animating) {
    double dt = advance(now, false);
    advance_viz(dt);
    advance_media(now, dt);
  } else if (w.dozing)
    advance(now, true);
  f->percent = shown_percent();
  f->level = wp_bar_level(&w.cfg, w.phase);
  f->angle = w.angle;
  f->glow = glow();
  if (w.cfg.react_glow) {
    /* A base brightness while music plays, the reactions on top of it. */
    double base = w.cfg.react_glow_base * w.playing;
    double react = fmax(fmin(w.cfg.react_glow_amount * w.react.loud, 1),
                        w.cfg.react_glow_spin * w.spin_glow);
    f->glow = fmax(f->glow, base + (1 - base) * react);
  }
  f->pulse = w.cfg.react_pulse ? w.react.pulse : 0;
  for (int k = 0; k < 6; ++k)
    f->slide[k] =
        w.cfg.react_slide ? w.cfg.react_slide_dist * w.react.slices[k] : 0;
  /* Two sines at unrelated rates, so the shake doesn't run on a line. */
  double shake = w.cfg.react_shake ? w.cfg.react_shake_px * w.react.shake : 0;
  f->shake_x = shake * sin(2 * M_PI * 11 * w.react.shake_t);
  f->shake_y = shake * sin(2 * M_PI * 14 * w.react.shake_t + 1.3);
  f->media_alpha = w.media_alpha;
  /* Idle, the hexagon (cover and progress) goes; the title stays. */
  f->progress = w.animating ? w.progress : -1;
  f->chase = w.animating && w.chasing ? w.chase : -1;
  f->art = w.animating && w.cfg.media_art ? wp_media_art(&f->art_id) : NULL;
  f->title = w.title;
  f->artist = w.artist;
  f->over = over();
  f->dir = w.cfg.spin_speed < 0 ? -1 : 1;
  /* Under the topo style the lines are the visualiser. */
  f->nbars = viz_showing() && !wp_topo(&w.cfg) ? w.cfg.viz_bars : 0;
  for (int s = 0; s < 2; ++s)
    for (int i = 0; i < f->nbars; ++i)
      f->bars[s][i] = (float)fmin(
          w.viz.bars[s][i] * wp_viz_wobble(&w.cfg, s, i, w.viz_time), 1);
  for (int k = 0; k < 6; ++k)
    f->lambda_fade[k] = 0;
  f->logo_shrink = f->bar_fade = f->flash = f->fade = f->bg_fade = 0;
  f->crt_w = f->crt_h = f->crt_alpha = 0;
  advance_map(now);
  f->map_fade = 1 - w.map_alpha;
  f->map_under = false;
  bool waves = wp_topo(&w.cfg) && w.cfg.topo_waves;
  f->sway = waves ? fmin(w.react.loud * w.playing, 1) : 0;
  f->time = w.viz_time;
  f->nrings = waves ? w.nrings : 0;
  for (int i = 0; i < f->nrings; ++i) {
    double life = 1 - w.rings[i].age * 1000 / w.cfg.topo_ring_ms;
    f->rings[i].radius = w.rings[i].age * w.cfg.topo_ring_speed;
    f->rings[i].size = w.rings[i].size * life * life;
  }
  show_frame(now, f);
}

void wp_count_frame(int64_t now) {
  w.fps_frames++;
  if (now - w.fps_start >= 1000) {
    w.fps = w.fps_frames * 1000.0 / (now - w.fps_start);
    w.fps_start = now;
    w.fps_frames = 0;
  }
}

/* ---- battery ---- */

static void read_battery(void) {
  int pct;
  bool charging;
  w.battery = battery_read(&pct, &charging) ? pct : -1;
  if (w.battery > 100)
    w.battery = 100;
  set_target(target_percent());
}

static void on_battery_timer(struct loop_timer *t, void *data) {
  read_battery();
}

/* ---- the launch and power-off animations ---- */

static void show_start(enum wp_show s) {
  w.show = s;
  w.show_start = now_ms();
  w.show_black = false;
  loop_timer_disarm(w.hold_timer);
  if (outro_show())
    loop_timer_arm(w.hold_timer,
                   wp_show_ms(&w.cfg, s) + (int64_t)w.cfg.outro_hold_ms, 0);
  bus_emit(s == WP_SHOW_INTRO ? "wallpaper.intro" : "wallpaper.outro",
           s == WP_SHOW_INTRO   ? NULL
           : s == WP_SHOW_CRT ? "SWELL_OUTRO=crt"
                                : "SWELL_OUTRO=scatter",
           NULL);
  wp_surfaces_draw(false); /* and every draw asks for the next frame */
}

static void show_stop(void) {
  if (!wp_showing())
    return;
  w.show = WP_SHOW_NONE;
  loop_timer_disarm(w.hold_timer);
  wp_surfaces_draw(false); /* the normal frame, whole */
}

/* The black is over: fade the wallpaper back in. */
static void on_hold_timer(struct loop_timer *t, void *data) {
  if (outro_show())
    wp_surfaces_draw(false);
}

/* The intro plays once per login session: the marker sits next to the
 * control socket in the runtime dir, which goes with the session. */
static bool intro_first(void) {
  char path[4096];
  snprintf(path, sizeof path, "%s.intro", ipc_socket_path());
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0)
    return false;
  close(fd);
  return true;
}

/* ---- the animate/freeze state machine ---- */

static void arm_idle_fallback(void) {
  if (!w.idle_note && w.cfg.idle_timeout_s > 0 && w.animating)
    loop_timer_arm(w.idle_timer, w.cfg.idle_timeout_s * 1000LL, 0);
}

static void update_animating(void) {
  bool want = w.ws_empty && !w.idle;
  bool doze =
      w.ws_empty && w.idle && w.cfg.battery_bar && w.cfg.idle_bar_pulse;
  if (want == w.animating && doze == w.dozing)
    return;
  bool was_animating = w.animating, was_ticking = wp_ticking();
  w.animating = want;
  w.dozing = doze;
  /* The bar (fill and pulse) moves in both states, so the battery is read
   * in both. */
  if (wp_ticking() && !was_ticking) {
    w.last = now_ms();
    w.fps_start = w.last;
    w.fps_frames = 0;
    int64_t ms = w.cfg.battery_interval_s * 1000LL;
    loop_timer_arm(w.battery_timer, ms, ms);
    read_battery();
  } else if (!wp_ticking()) {
    loop_timer_disarm(w.battery_timer);
  }
  /* A window open freezes the plain wallpaper, as before the details
   * existed: no title and no hexagon. Idle on an empty workspace keeps
   * the title only. */
  /* A show is only for the wallpaper; once it's covered, it's over. */
  if (!w.ws_empty)
    show_stop();
  bool cleared = !w.ws_empty && w.media_alpha > 0;
  if (cleared)
    w.media_alpha = 0;
  if (want) {
    arm_idle_fallback();
    if (w.cfg.viz)
      wp_audio_start(w.sw->loop, &w.cfg);
    if (w.cfg.media)
      wp_media_start(w.sw->loop, &w.cfg);
    wp_surfaces_draw(false); /* and every draw asks for the next frame */
    bus_emit("wallpaper.animate", NULL);
    return;
  }
  if (!was_animating) {
    if (doze || cleared)
      wp_surfaces_draw(false); /* start the bar's frames */
    return;
  }
  loop_timer_disarm(w.idle_timer);
  wp_audio_stop();
  wp_media_stop(); /* idle, the frozen frame keeps the details it shows */
  bool glowing = w.boost > 0 || viz_showing() || reacting();
  w.boost = 0;
  memset(&w.viz, 0, sizeof(w.viz));
  memset(&w.react, 0, sizeof(w.react));
  w.kicks_seen = 0;
  w.nrings = 0;
  w.spin_glow = 0;
  w.playing = 0;
  if (!doze)
    w.fps = 0;
  /* A window opened (or the user idled) mid-spin or mid-song: don't
   * leave the brightened logo, the bars or the hexagon as the frozen
   * frame. Dozing draws on for the bar. */
  if (glowing || doze || cleared || w.media_alpha > 0)
    wp_surfaces_draw(false);
  bus_emit("wallpaper.freeze", NULL);
}

static void on_bus(const struct bus_event *ev, void *data) {
  if (strcmp(ev->name, "workspace.empty") == 0)
    w.ws_empty = true;
  else if (strcmp(ev->name, "workspace.occupied") == 0)
    w.ws_empty = false;
  else
    return;
  /* The map fades with its own frames, even into a frozen wallpaper. */
  bool was_ticking = wp_ticking();
  if (!was_ticking)
    w.map_last = now_ms();
  update_animating();
  if (map_fading() && !was_ticking && !wp_animating() && !wp_dozing())
    wp_surfaces_draw(false); /* and every draw asks for the next frame */
}

/* ---- idle ---- */

static void idled(void *data, struct ext_idle_notification_v1 *n) {
  w.idle = true;
  update_animating();
}

static void resumed(void *data, struct ext_idle_notification_v1 *n) {
  w.idle = false;
  update_animating();
}

static const struct ext_idle_notification_v1_listener idle_listener = {
    .idled = idled,
    .resumed = resumed,
};

static void on_idle_timer(struct loop_timer *t, void *data) {
  w.idle = true;
  update_animating();
}

static void setup_idle(void) {
  if (w.idle_note)
    ext_idle_notification_v1_destroy(w.idle_note);
  w.idle_note = NULL;
  loop_timer_disarm(w.idle_timer);
  w.idle = false;
  struct swell_wl *wl = w.sw->wl;
  if (w.cfg.idle_timeout_s > 0 && wl->idle_notifier && wl->seat) {
    w.idle_note = ext_idle_notifier_v1_get_idle_notification(
        wl->idle_notifier, w.cfg.idle_timeout_s * 1000u, wl->seat);
    ext_idle_notification_v1_add_listener(w.idle_note, &idle_listener, NULL);
  }
  arm_idle_fallback();
}

/* ---- pointer ---- */

static void pointer_enter(void *data, struct wl_surface *s, double x,
                          double y) {
  wl_set_cursor(w.sw->wl, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
  w.have_pointer = true;
  w.px = x;
  w.py = y;
}

static void pointer_leave(void *data, struct wl_surface *s) {
  w.have_pointer = false;
}

static void pointer_motion(void *data, double x, double y) {
  double d = w.have_pointer ? hypot(x - w.px, y - w.py) : 0;
  w.have_pointer = true;
  w.px = x;
  w.py = y;
  if (!w.idle_note && w.idle) {
    /* Without ext-idle-notify only motion over the wallpaper wakes us. */
    w.idle = false;
    update_animating();
    return;
  }
  if (!w.animating)
    return;
  arm_idle_fallback();
  w.boost = fmin(w.boost + w.cfg.spin_gain * d, w.cfg.spin_max);
  if (d > 0)
    w.last_motion = now_ms();
}

const struct swell_input wp_input = {
    .pointer_enter = pointer_enter,
    .pointer_leave = pointer_leave,
    .pointer_motion = pointer_motion,
};

/* ---- module ---- */

/* An unknown style draws as topo. */
static void check_style(void) {
  if (strcmp(w.cfg.style, "topo") && strcmp(w.cfg.style, "classic"))
    warn("wallpaper: unknown style '%s' (topo or classic); using topo",
         w.cfg.style);
}

/* topo_new_map: one map per daemon, so reloads keep it. */
static void pick_map(void) {
  static int seed;
  static double angle;
  if (!w.cfg.topo_new_map)
    return;
  if (!seed) {
    seed = 1 + (int)(drand48() * 1000000);
    angle = floor(drand48() * 360);
  }
  w.cfg.topo_seed = seed;
  w.cfg.topo_angle = angle;
}

static bool init(struct swell *sw, const void *cfg) {
  if (!sw->wl->layer_shell) {
    warn("wallpaper: the compositor lacks wlr-layer-shell");
    return false;
  }
  memset(&w, 0, sizeof(w));
  w.sw = sw;
  w.cfg = *(const struct wallpaper_config *)cfg;
  w.override = -1;
  check_style();
  wp_audio_init();
  w.battery_timer =
      loop_add_timer(sw->loop, CLOCK_MONOTONIC, on_battery_timer, NULL);
  w.idle_timer = loop_add_timer(sw->loop, CLOCK_MONOTONIC, on_idle_timer, NULL);
  w.hold_timer = loop_add_timer(sw->loop, CLOCK_MONOTONIC, on_hold_timer, NULL);
  srand48(now_ms() ^ getpid());
  pick_map();
  int pct;
  bool charging;
  w.battery = battery_read(&pct, &charging) ? (pct > 100 ? 100 : pct) : -1;
  w.tween_from = w.tween_to = target_percent();
  setup_idle();
  wp_surfaces_init(sw->wl); /* a frozen first frame on every output */

  bus_subscribe(on_bus, NULL);
  int empty = wm_focused_empty();
  if (empty < 0 && sw->wm != WM_NONE) {
    wm_emit_focused_state(); /* the answer arrives through on_bus */
    empty = wm_focused_empty();
  }
  w.ws_empty = empty != 0; /* without a window manager: always animate */
  w.map_alpha = map_target();
  w.map_last = now_ms();
  update_animating();
  if (w.cfg.intro && w.animating && intro_first())
    show_start(WP_SHOW_INTRO);
  return true;
}

static void reconfigure(const void *cfg) {
  struct wallpaper_config old = w.cfg;
  w.cfg = *(const struct wallpaper_config *)cfg;
  pick_map();
  if (strcmp(old.style, w.cfg.style))
    check_style();
  if (old.idle_timeout_s != w.cfg.idle_timeout_s) {
    setup_idle();
    update_animating();
  } else if (old.idle_bar_pulse != w.cfg.idle_bar_pulse ||
             old.battery_bar != w.cfg.battery_bar) {
    update_animating();
  }
  if (old.battery_interval_s != w.cfg.battery_interval_s && w.animating) {
    int64_t ms = w.cfg.battery_interval_s * 1000LL;
    loop_timer_arm(w.battery_timer, ms, ms);
  }
  if (old.viz != w.cfg.viz || strcmp(old.viz_target, w.cfg.viz_target)) {
    wp_audio_stop();
    if (w.animating && w.cfg.viz)
      wp_audio_start(w.sw->loop, &w.cfg);
  }
  if (old.media != w.cfg.media || old.media_art != w.cfg.media_art ||
      old.media_interval_ms != w.cfg.media_interval_ms ||
      strcmp(old.media_player, w.cfg.media_player)) {
    wp_media_stop();
    wp_media_clear();
    if (w.animating && w.cfg.media)
      wp_media_start(w.sw->loop, &w.cfg);
  }
  if (strcmp(old.namespace, w.cfg.namespace) != 0)
    wp_surfaces_recreate();
  else
    wp_surfaces_draw(true);
}

static void finish(void) {
  bus_unsubscribe(on_bus, NULL);
  wp_surfaces_finish();
  wp_gl_finish();
  wp_render_finish();
  wp_audio_finish();
  wp_media_stop();
  wp_media_clear();
  memset(&w.viz, 0, sizeof(w.viz));
  if (w.idle_note)
    ext_idle_notification_v1_destroy(w.idle_note);
  w.idle_note = NULL;
  loop_remove_timer(w.battery_timer);
  loop_remove_timer(w.idle_timer);
  loop_remove_timer(w.hold_timer);
  w.battery_timer = w.idle_timer = w.hold_timer = NULL;
  w.show = WP_SHOW_NONE;
  w.animating = false;
  w.dozing = false;
}

static int command(int argc, char **argv, struct reply *r) {
  if (argc == 0 || strcmp(argv[0], "status") == 0) {
    reply_printf(r, "state: %s\n",
                 w.animating ? "animating"
                 : w.dozing  ? "dozing (only the bar pulses)"
                             : "frozen");
    reply_printf(r, "workspace: %s\n", w.ws_empty ? "empty" : "occupied");
    reply_printf(r, "idle: %s%s\n", w.idle ? "yes" : "no",
                 w.idle_note || w.cfg.idle_timeout_s == 0
                     ? ""
                     : " (no ext-idle-notify: pointer only)");
    if (w.battery >= 0)
      reply_printf(r, "battery: %d%%\n", w.battery);
    else
      reply_printf(r, "battery: none\n");
    reply_printf(r, "percent: %.0f%s (shown %.1f)\n", target_percent(),
                 w.override >= 0 ? " (override)" : "", shown_percent());
    reply_printf(r, "fps: %.1f\n", wp_ticking() ? w.fps : 0.0);
    reply_printf(r, "spin: x%.2f\n", 1 + w.boost);
    if (wp_topo(&w.cfg))
      reply_printf(r, "topo seed: %d, angle %.0f%s\n", w.cfg.topo_seed,
                   w.cfg.topo_angle,
                   w.cfg.topo_new_map ? " (new each start)" : "");
    reply_printf(r, "audio: %s\n", w.cfg.viz ? wp_audio_state() : "disabled");
    if (w.cfg.viz && w.cfg.viz_edges && w.cfg.viz_edge_smooth)
      reply_printf(r, "viz render: %s\n", wp_surfaces_viz_renderer());
    if (w.cfg.viz) {
      float top[2] = {0, 0};
      for (int s = 0; s < 2; ++s)
        for (int i = 0; i < w.cfg.viz_bars; ++i)
          top[s] = fmaxf(top[s], w.viz.bars[s][i]);
      reply_printf(r, "viz: left %.2f, right %.2f (peak %.0f dB)\n", top[0],
                   top[1], w.viz.peak);
      reply_printf(r,
                   "react: loud %.2f, pulse %.2f, shake %.2f, spin x%.2f "
                   "(%s%s%s%s%s)\n",
                   w.react.loud, w.react.pulse, w.react.shake,
                   1 + (w.cfg.react_spin ? w.cfg.react_spin_gain * w.react.energy +
                                               w.react.kick
                                         : 0),
                   w.cfg.react_slide ? " slide" : "",
                   w.cfg.react_pulse ? " pulse" : "",
                   w.cfg.react_glow ? " glow" : "",
                   w.cfg.react_spin ? " spin" : "",
                   w.cfg.react_shake ? " shake" : "");
    }
    if (w.cfg.media) {
      const struct wp_media *m = wp_media_get();
      if (!m->has) {
        reply_printf(r, "media: none\n");
      } else {
        double p = wp_media_progress(m, now_ms());
        char prog[16];
        if (p >= 0)
          snprintf(prog, sizeof prog, "%.0f%%", p * 100);
        else
          snprintf(prog, sizeof prog, "chase");
        reply_printf(r, "media: %s, \"%s\"%s%s, %s\n",
                     m->playing ? "playing" : "paused", m->title,
                     m->artist[0] ? " — " : "", m->artist, prog);
        unsigned id;
        reply_printf(r, "cover: %s\n",
                     wp_media_art(&id) ? "yes"
                     : m->art_url[0] && w.cfg.media_art ? "none (unreadable)"
                                                         : "none");
      }
    }
    if (wp_showing())
      reply_printf(r, "show: %s%s\n", wp_show_name(w.show),
                   show_holding() ? " (black)" : "");
    reply_printf(r, "outputs: %d\n", wp_surfaces_count());
    return 0;
  }
  /* Play the launch animation, or the power-off one; the reply is the ms
   * until it ends (for a power-off, in black), 0 if it doesn't play. */
  if ((strcmp(argv[0], "intro") == 0 || strcmp(argv[0], "outro") == 0) &&
      argc == 1) {
    bool intro = argv[0][0] == 'i';
    int64_t left = 0;
    if (outro_show() && !intro) {
      /* Already powering off: the same one, not a restart. */
      left = w.show_start + wp_show_ms(&w.cfg, w.show) - now_ms();
    } else if (w.ws_empty && (intro || w.cfg.outro)) {
      enum wp_show s = intro ? WP_SHOW_INTRO
                       : drand48() < w.cfg.outro_crt_chance ? WP_SHOW_CRT
                                                            : WP_SHOW_SCATTER;
      show_start(s);
      left = wp_show_ms(&w.cfg, s);
    }
    reply_printf(r, "%lld\n", (long long)(left > 0 ? left : 0));
    return 0;
  }
  if (strcmp(argv[0], "percent") == 0 && argc == 2) {
    if (strcmp(argv[1], "auto") == 0) {
      w.override = -1;
    } else {
      char *end;
      long v = strtol(argv[1], &end, 10);
      if (end == argv[1] || *end || v < 0 || v > 100) {
        reply_printf(r, "wallpaper: percent takes 0..100 or auto\n");
        return 1;
      }
      w.override = (int)v;
    }
    set_target(target_percent());
    return 0;
  }
  reply_printf(r, "wallpaper: unknown command '%s'\n", argv[0]);
  return 1;
}

const struct swell_module wallpaper_module = {
    .name = "wallpaper",
    .aliases = (const char *const[]){"wp", NULL},
    .usage = "  status                 animation state, battery, fps\n"
             "  percent N|auto         show N% instead of the battery\n"
             "  intro                  play the launch animation\n"
             "  outro                  play the power-off animation (see "
             "`swell power`)\n",
    .needs_wayland = true,
    .config_size = sizeof(struct wallpaper_config),
    .defaults = &defaults,
    .fields = fields,
    .nfields = ARRAY_LEN(fields),
    .init = init,
    .reconfigure = reconfigure,
    .finish = finish,
    .command = command,
};
