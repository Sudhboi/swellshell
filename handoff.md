# swellshell handoff

`swell` is one daemon that merges five older tools from `~/Projects/swell`:
- nixosd (`osd/`)
- popup (`popup/`)
- swayverview (`swayverview/`)
- dynwork (`i3c/`)
- awww-kill (`awww-kill/`)

All of it runs in a single event loop with one Wayland connection, one sway IPC connection and one control socket. Each original handoff (`../osd/handoff.md`, `../popup/handoff.md`, `../swayverview/HANDOFF.md`) still records the design decisions and look of its module. Read the one for the module you touch.

## Decisions (from the user, 2026-10-08)
- **One daemon.** `swell daemon` runs every module, and `swell <module> <cmd>` is a thin client over `$XDG_RUNTIME_DIR/swell.sock`, or `$SWELL_SOCK` if set.
- **New CLI only.** There are no compat FIFOs or dynwork.sock. The user's sway config is never edited by us; we give them a snippet instead (see README).
- **Runtime config.** `~/.config/swell/config.toml` (TOML) holds every tunable, with no compile-time `#define`s for settings. The defaults equal the old tools' constants and the user's current flags. Two ways to apply a change:
  - **Rerun `swell daemon`.** This replaces a running daemon, so `exec_always swell daemon` picks up edits on `swaymsg reload`.
  - **`swell reload`.** This applies the change live.
- **The runner (formerly awww-kill) is a general event-hook runner.** The `[[runner.hook]]` entries default to awww pause/unpause.
- **Build:** Meson, plus a nix flake with the package, a devShell and a home-manager module.
- **Feature parity with all five tools is the goal.** Keep going until it is reached.
- **Never touch the user's running session.** Don't install anything, edit `~/.config/sway`, or kill their daemons (nixosd, popup, dynwork and awww-kill are running). Live tests use a scratch `SWELL_SOCK` or a nested headless sway.

## Layout
```
meson.build             top-level build; modules list their sources in src/modules/<m>/meson.build as <m>_src
lib/i3ipc/              i3ipc-simple (swayverview's patched copy), compiled with -w
lib/tomlc17/            TOML parser (MIT), compiled with -w
protocols/              wlr-layer-shell XML (the rest comes from wayland-protocols)
src/main.c              CLI, daemon, config build/reload, built-in commands (reload/status/quit/help/config)
src/core/               shared code, see the interfaces below
src/modules/module.h    the module interface
src/modules/modules.c   module registry (init order)
src/modules/<m>/        osd, popup, overview, workspaces, runner
test/                   meson tests
config/config.example.toml   generated: `swell config --defaults`
nix/                    package.nix, hm-module.nix
```
Build and test with `meson setup build && ninja -C build && meson test -C build`.

## Core interfaces (fixed; ask the main agent before changing)
- **`core/loop.h`:** `loop_add_fd`, `loop_add_timer` (timerfd, `loop_timer_arm(ms, interval)` / `loop_timer_arm_abs`), `loop_add_prepare(priority, fn)` (runs before every poll and may return a timeout; Wayland flushes at priority 100, so modules use < 100), `loop_watch_child(pid, fn)` (pidfd; the loop reaps). SIGINT/SIGTERM quit the loop, and SIGPIPE is ignored.
- **`core/wl.h`:** the `struct swell_wl` globals (compositor, shm, layer_shell, viewporter, fractional, cursor_shape, source/copy managers, seat, xkb state, repeat info).
  - Outputs: `wl->outputs` holds `struct swell_output` (name, integer scale). `wl_add_output_listener` reports add and remove.
  - Input routing: `wl_set_input(wl, surface, &handler, data)`, cleared with `wl_clear_input` before the surface is destroyed. Key events give xkb keycodes; translate them with `wl->xkb_state`. Key repeat is the module's job (`wl->repeat_rate/delay`).
  - `wl_set_cursor(wl, shape)`.
  - `wl_bind_toplevel_list(wl)` binds a fresh ext_foreign_toplevel_list on demand; attach its listener immediately.
- **`core/surface.h`:** `layer_surface_*` gives one layer surface that is created on show and destroyed on hide, with double-buffered shm, frame-callback pacing and fractional scale. `layer_draw_fn(cr, data)` draws in logical coordinates and returns true to keep animating. The `struct layer_opts` passed at show time gives the size, anchor, layer, margins, click-through and keyboard. The osd and the popup use it.
- **`core/buffer.h`:** shm buffers wrapped as cairo surfaces (from swayverview).
- **`core/sway.h`:** `sway_init`. i3ipc queries (`i3ipc_get_tree()` …) may be used directly afterwards. Also:
  - `sway_subscribe(fn)` for raw `I3ipc_event`s (workspace, window and output events);
  - `sway_workspaces()` for the cached workspace list;
  - `sway_command(fmt, …)`.

  Sway events are also published on the bus (see the comment in sway.h for names and env). `workspace.empty` and `workspace.occupied` are the awww-kill transition events.
- **`core/bus.h`:** `bus_emit("osd.show", "KEY=VAL", NULL)` and `bus_subscribe(fn)`. Event names are `<module>.<what>`.
- **`core/config.h`:** `cfg_field` tables via `CFG_INT/DOUBLE/BOOL/STRING/COLOR(struct, field, ...)`. The TOML key equals the field name. Colours are `uint32_t 0xRRGGBBAA`, written `"#rrggbb[aa]"` in TOML; `col_r/g/b/a()` give cairo components. Strings are fixed `char[N]` buffers, so config structs are flat, memcpy-able values.
- **`core/ipc.h`:** `struct reply` + `reply_printf`, used for command replies.
- **`core/anim.h`:** `ease_out_cubic`, `anim_progress`.
- **`core/util.h`:** `xcalloc`, `warn`, `die`, `now_ms`, `spawn_shell[_detached]`, `runtime_dir`.
- **`modules/module.h`:** `struct swell_module`, with name, aliases, usage, `needs_wayland`/`needs_sway`, config (`defaults`, `fields`, `extra_keys`, `configure`, `free_config`, `dump_extra`), and `init`/`reconfigure`/`finish`/`command`/`run_local`. Module state lives in file-static variables (one instance each). The core owns config structs, so `reconfigure(new)` must copy what it needs; the old struct is freed right after.

## Module conventions
- Config keys are descriptive snake_case named after the old macro: `OSD_WIDTH` → `width`, `HIDE_TIMEOUT_MS` → `hide_timeout_ms`, `COL_BG` → `background`, `COL_RING_DARK` → `ring_dark`. Keep units in the name (`_ms`). Each key gets a one-line doc string, which `swell config` prints.
- Sizes and colours that render code used as macros become reads from the module's current config (a file-static `struct <m>_config cfg` copied on init/reconfigure, passed to render functions as `const struct <m>_config *`). Derived values (surface height = height + gap, …) are computed from it, never cached across a reconfigure.
- Every module emits bus events for state changes that someone may want to hook (`osd.show`, `osd.hide`, `popup.show`, `popup.hide`, `popup.expand`, `overview.open`, `overview.close`).
- Messages go through `warn()` (prefix `swell:`).

## Status
- [x] Core: util, loop, config (+ test), bus, sway, ipc, wl, surface, buffer, main (daemon, client, replace, reload, status, quit, config dump). Smoke-tested: status, replace and quit against a scratch socket.
- [x] osd port (subagent); live-tested in nested sway
- [x] popup port (subagent); live-tested in nested sway
- [x] overview port (subagent)
- [x] workspaces (dynwork) port (main); live-tested in nested sway
- [x] runner (awww-kill generalisation) (main); live-tested in nested sway
- [x] config.example.toml (generated; meson test `example-config-up-to-date` keeps it in sync), README with the sway snippet
- [x] nix flake, package, devShell, home-manager module (HM eval checked; `nix build` passes and runs the test suite)
- [x] nested-sway live test harness (`test/live/`) + parity pass (stress test, 15 bugs fixed; see the log)

## Notes / log (append below, newest last, prefix with who)

- main: workspaces module (`src/modules/workspaces/`). It keeps dynwork's semantics:
  - `next` creates workspace n+1 when on the last one;
  - `prev` stops at 1;
  - `goto N` runs `workspace N` and then renumbers;
  - `reorder` renumbers. Renumbering renames every workspace to 1..n in sway's order, named ones included, exactly as dynwork did.

  dynwork's blocking 30 ms drain became a debounce timer (`debounce_ms`), re-armed on every workspace event. Config keys are `renumber` and `debounce_ms`. Without a daemon, `swell ws ...` acts directly via `run_local` (dynwork's standalone mode). The pure planner is in `plan.c`, tested by test/workspaces. It emits `workspaces.renumber`.

  The gesture mapping changed: dynwork's old `left` called move_right, i.e. "next". The README snippet maps swipe:left to `ws next` and swipe:right to `ws prev`, the same behaviour.
- main: runner module (`src/modules/runner/`).
  - `[[runner.hook]]` takes `on` (string or array of fnmatch globs), `run` (string or array) and `detach` (bool).
  - All matching commands go into one serial queue: each waits for the previous via pidfd, with `timeout_ms` (default 10 s) after which the queue moves on. `detach` double-forks and doesn't wait.
  - Defining any hook replaces the defaults; `hook = []` means none.
  - The defaults reproduce awww-kill: `workspace.empty` gives a detached `awww unpause`, and `workspace.occupied` gives a waited `awww pause`.
  - The transition dedupe (awww-kill's `daemon_state`) lives in core/sway.c (`set_focused_empty`), so the events fire only on change, plus once whenever the runner starts (`sway_emit_focused_state`, called from runner init, which also covers it being enabled by `swell reload`).
  - Commands: `swell runner fire EVENT [K=V..]` and `swell runner list`. Config keys: `timeout_ms` and `log`.
- main: `test/live/nested-sway.sh start|stop` is a nested headless sway (pixman, scale 1.25) with a scratch SWELL_SOCK and SWELL_CONFIG. `zenity` works as a test window; foot isn't installed.
- main: the nix flake provides packages/apps/devShells/checks/overlays/homeManagerModules (also as homeModules). The package wraps PATH (suffix) with wireplumber, networkmanager and playerctl. `nix/hm-module.nix` is `services.swell` with `enable/package/settings/systemdTarget`; settings are written via formats.toml, `onChange` runs `swell reload`, and there is a systemd user service. It evaluates with the current home-manager, and the generated TOML parses.
- osd/popup agent: osd (`src/modules/osd/`: osd.c, parse.c, tween.c, render.c, nixlogo.c, osd.h) and popup (`src/modules/popup/`: popup.c, info.c, render.c, popup.h) are ported. No core changes were needed.
  - **Config `[osd]`:** `namespace` ("nixosd"), `width`, `height`, `corner`, `top_gap`, `ring_cy`, `ring_radius`, `ring_width`, `logo_radius`, `text_baseline`, `font`, `font_size`; colours `background`, `border`, `track`, `nix_dark`, `nix_light`, `ring_dark`, `ring_light`, `over_dark`, `over_light`, `pulse_light`, `pulse_dark`, `muted`, `text`; `logo_pulse_alpha`, `max_pct`, `hide_timeout_ms`, `tween_ms`, `slide_ms`, `pulse_ms`. The surface height is `height + top_gap`.
  - **Config `[popup]`:** `namespace` ("popup"), `width`, `height` (the maximum and surface height; the panel height comes from `full_height()`), `pad`, `bottom_gap`, `font`, `font_clock`, `font_seconds`, `font_date`, `font_row`, `font_ws`; colours `background`, `border`, `track`, `bar_dark`, `bar_light`, `text`, `dim`, `muted`, `ws_focus`, `warn`; `battery_low_pct`, `refresh_ms`, `pop_ms`, `close_ms`, `expand_delay_ms`, `expand_ms`, `media_ms`; the old render.c layout constants as doubles: `gap_date`, `gap_sep`, `gap_cols`, `gap_bar`, `bar_h`, `col_inset`, `gap_rows`, `row_step`, `sec_gap`, `icon_gap`, `mini_font`, `mini_gap`, `ws_h`, `ws_min_w`, `ws_text_pad`, `ws_gap`, `gap_ws`, `wsc_min_w`, `wsc_max_w`, `wsc_inset`, `wsc_arrow`, `ws_arrow_w`. The defaults equal the old constants. WS_MAX (32) stays a compile-time array limit. The 3 s command kill is now `command_timeout_ms` (main, later).
  - **Bus events:**
    - `osd.show` (SWELL_OSD_KIND=volume|brightness, SWELL_OSD_LEVEL, SWELL_OSD_MUTED=0|1) fires when the OSD becomes visible (hidden or retracting → shown), not on level updates while shown.
    - `osd.hide` fires when the retract starts.
    - `popup.show` and `popup.hide` fire on real visibility changes (a reopen mid-close counts as a show).
    - `popup.expand` fires when the details reveal starts.
  - **Commands:** `swell osd vol|volume N [mute]`, `bri|brightness N`, `N` (the argv is joined and parsed like nixosd's FIFO lines; unparsable input is an error reply). `swell popup show|hide|toggle`.
  - **Runtime:**
    - The retract/close end is a prepare hook (priority 50) that returns the remaining ms, so it finishes without frame callbacks.
    - popup ticks on a CLOCK_REALTIME loop timer (abs, sec+1 +5 ms, interval refresh_ms); expand is a monotonic one-shot.
    - info.c children: pipes go through loop_add_fd and the processes are reaped by loop_watch_child (waitpid fallback). A command is "running" until both its pipe hit EOF and it was reaped. Children get an empty sigmask and SIG_DFL SIGPIPE.
    - Workspaces come from `sway_workspaces()`: they are re-queried on show, read from the cache each tick, and refreshed on workspace events while shown. Without sway they are empty.
  - **reconfigure:** if a surface is shown it is hidden and re-shown at the new size (a retracting/closing one is just finished).
  - **Tests:** `osd parse`, `osd render`, `osd config render`, `popup render` (port of render_test.c, 17 PNGs), `popup config render` and `popup workspaces` (sorting/WS_MAX/flags); PNGs go to build/test/<m>/. The default render PNGs are byte-identical to the original tools' render code (built in scratch).
  - **Live-tested** in a nested headless sway at scale 1.25:
    - osd: drop-down position and slide, one show/hide event per burst, hide after 1.2 s.
    - popup: slide, compact, expand at 2.0 s, workspace strip updated by events between ticks, reopen mid-close, close.
    - Fake hanging wpctl: no respawn, SIGKILL after 3 s, reaped.
    - Live `swell reload` resizes shown surfaces.
    - Without sway: the popup works without workspaces.
    - Idle: 0 CPU ticks and no zombies.
  - **Deviations:**
    - Fractional scaling now comes from core/surface (the originals used the integer scale), so text is crisper; layout is unchanged.
    - `expand_delay_ms = 0` expands immediately (the original's 0 would have disarmed it).
    - Workspace events refresh the strip immediately, not only on the next tick.
  - **Not verified live:** wpctl/volume inside the nested session (the scratch XDG_RUNTIME_DIR hides PipeWire, so it shows "--"); a real media start/stop tween (same as before the port).
- overview agent: swayverview is ported to `src/modules/overview/`. No core changes were needed.
  - **Files:**

    | file | role (swayverview origin) |
    |---|---|
    | `overview.c` | module, config table, open/close lifecycle, per-output overlays (layer surface, fractional scale + viewporter, frame pacing; from main.c + wayland.c), model reload that keeps selection/drag/zoom by id, prepare hook, `dump` via `run_local` |
    | `model.c/.h` | sway tree → model, tab spreading, "New workspace" slot, focus/move/kill commands; pure `ov_renumber_command` / `ov_reorder_plan` for reordering (sway.c) |
    | `capture.c/.h` | foreign toplevel list (listener attached at bind), one ext-image-copy-capture session per window, double-buffered shm, `fresh_constraints` ordering, XBGR swizzle, throttled fps, 2-slot thumbnail cache |
    | `layout.c/.h` | grid, hit testing, directional/Tab navigation, reorder slots |
    | `render.c/.h` | cairo/pango drawing (`push_group_in` kept) |
    | `input.c/.h` | `struct swell_input` handler: click, drag, middle-click kill, keyboard, key repeat on a loop timer |
    | `zoom.c/.h` | hover preview grow/shrink |
    | `overview.h` | shared structs + `struct overview_config` |

  - **Commands:** `swell overview [open|close|toggle|status]` (no argument = toggle; open while open and close while closed are no-ops). `swell overview dump FILE.png [OUTPUT]` runs in the client with its own `wl_create` + `sway_init(NULL)`.
  - **Lifecycle:**
    - `open` binds a fresh toplevel list and does a roundtrip, builds one overlay per `wl->outputs` entry, loads the model, then adds an output listener (hotplug), `sway_subscribe` (marks the model dirty), a prepare hook (priority 50) and the repeat timer.
    - `close` (also Esc/q, a right or background click, or activating a target; input handlers set `close_pending` and the prepare hook closes) removes all of that, stops and destroys the toplevel list, frees the model and calls `malloc_trim(0)`.
  - **Config `[overview]`** (applies on the next open):
    - the old CLI options: `capture`, `live`, `fps`, `new_workspace_slot`, `font` (default "IosevkaTerm Nerd Font 12");
    - sizes: `layout_pad`, `layout_gap`, `label_height`, `caption_height`, `spread_gap`, `drag_threshold`;
    - zoom: `zoom_delay_ms`, `zoom_anim_ms`, `zoom_max_width`, `zoom_max_height`, `zoom_min_gain`, `zoom_margin`;
    - colours: `background` (scrim), `tile`, `tile_border`, `tile_visible`, `accent`, `urgent`, `window`, `window_border`, `hover`, `text`, `text_dim`, `focused_border`, `drop_fill`, `new_slot_fill`, `new_slot_drop_fill`, `shadow`, `zoom_shadow`, `zoom_title_band`;
    - `drag_source_alpha`, `drag_ghost_alpha`.
  - **Bus events:** `overview.open` and `overview.close` (no env).
  - **Cooperation with workspaces:** a reorder is one sway command list (temp names `swayverview-reorder-N`). Selection and drag of a workspace are resolved by its sway con_id first, so renames by the workspaces module keep them; verified live.
  - **Tests:** `test/overview` (`overview`): renumber command planning, reorder planning, and layout/hit-test/navigation/drop-slot on a synthetic model.
  - **Live-tested** in a nested headless sway at scale 1.25 with ghostty/zenity, plus a scratch zwlr_virtual_pointer/zwp_virtual_keyboard driver:
    - dump is pixel-for-pixel the same layout as the original binary built in scratch;
    - tab spreading with live hidden tabs; hover zoom grows and shrinks (slowed down via config to see the frames);
    - click to focus a window, a workspace or New workspace;
    - right click, background click, Esc and q close;
    - drag a window to a workspace and to New workspace; drag a workspace by its label;
    - Ctrl+arrows and Ctrl+h/l reorder; Shift+arrows and H/L move the window;
    - arrows, Tab and Enter; 1-9 jump; key repeat (23 repeats in a 1.5 s hold);
    - middle-click kill; output hotplug and unplug while open;
    - `live=false` (0 ticks once captured), `capture=false`, `new_workspace_slot=false`, `fps`;
    - renames by the workspaces module while open;
    - `swell quit` while open.
  - **Resources:** closed, the daemon uses 0 CPU ticks. Over 12 open/close cycles fds return to the baseline and RSS stays flat (about 51 MB after the first open, which is pango/fontconfig caches). Open with live top/htop it uses about 47 ticks per 5 s; the original uses 60. ASan/UBSan show no errors and no leaks in overview code.
  - **Deviations:**
    - The default font is "IosevkaTerm Nerd Font 12".
    - Colours are 8-bit `#rrggbbaa`, rounded from the old float values.
    - The layer namespace is "swell-overview" (was "swayverview").
    - A workspace selection survives renames (it is matched by con_id, then by name).
    - New: a `status` command, and `zoom_anim_ms = 0` means no animation.
  - **Not verified:** real touchpad gestures, a real multi-monitor setup (cross-output drag is still not supported, as before), and XWayland windows.
- tester agent: stress-tested core, ws and runner. Added test/core (`core-ipc`, `core-dump-roundtrip`) and test/live/{daemon-lifecycle,runner-transitions}.sh. Run the live scripts with `</dev/null`; both pass now.
- main: fixed the 15 bugs the tester found:
  - **Daemon lock:** the daemon holds `flock($SWELL_SOCK.lock)` for its lifetime. A replacement keeps sending `quit` until it gets the lock (5 s), so concurrent starts leave exactly one daemon. The socket is unlinked on stop only if its inode is still ours.
  - **`ws goto`:** takes digits only (`ws_parse_number`) and sends `workspace N`, which closes a sway command injection.
  - **Reload of a broken TOML:** keeps the running config and returns an error (`cfg_doc.broken`/`error`). At startup a broken file still means defaults, with a warning.
  - **IPC server:** clients time out after 2 s (prepare hook), at most 64 clients, accept backs off 100 ms on EMFILE, and more than 64 args is an error.
  - **IPC client:** uses MSG_NOSIGNAL, a 10 s SO_RCV/SNDTIMEO, and Ctrl-C works (signals are unblocked after loop_create).
  - **Close-on-exec:** the i3ipc sockets get FD_CLOEXEC in `sway_init`.
  - **Dump escaping:** `cfg_dump_string` escapes control characters for `swell config` and the hook dump.
  - **Renumbering:** goes through temporary names (`swell-renumber-N`) only when an in-order rename would clash (`ws_plan_commands`, tested), and is sent as one command list. Workspace names are up to 255 bytes; longer ones disable renumbering with a warning.
  - **Runner and hooks:** the runner emits the current empty/occupied state when it starts; `runner fire` with more than 31 vars is an error; empty `on`/`run` lists are warned about.
  - **Smaller fixes:** `swell config` warns about unknown sections; `loop_destroy` frees child watches; CFG_DOUBLE rejects NaN.
- main: not changed, on purpose:
  - `swell ws …` with the workspaces module disabled in a running daemon is an error. Disabled means disabled; there is no fallback to standalone mode.
  - `swell config` reads the default path (or `-c`), not the running daemon's `-c`.
- main: `test/live/vin` (built by meson, not installed) is the overview agent's virtual pointer/keyboard driver: `vin m X Y | g X Y N | bd | bu | click | mclick | rclick | k [ctrl+][shift+]KEY | kd KEY | ku KEY | s MS`. `nested-sway.sh stop` deletes its dir unless KEEP=1.
- main: the popup's command kill time is now `command_timeout_ms` (default 3000).

## Adding a feature
- **A new setting:** add a field to the module's config struct, its default to the defaults initialiser, and one `CFG_*` line with a doc string. Then regenerate `config/config.example.toml` with `build/swell config --defaults > config/config.example.toml`.
- **A new module:**
  - create `src/modules/<m>/<m>.c` exporting `const struct swell_module <m>_module`, and `src/modules/<m>/meson.build` with `<m>_src`;
  - add it to the list in `meson.build` and to `src/modules/modules.c`;
  - add `test/<m>/meson.build` and put `<m>` in the list in `test/meson.build`.

  Use `layer_surface_*` for a simple overlay, `bus_emit` for hookable events, `sway_subscribe` for sway events, and loop timers instead of sleeping. Never block the loop.

## Not verified (needs the real session)
- Real touchpad gestures and sway keybindings calling the CLI. They should work: they're plain `exec swell …`.
- A real multi-monitor setup.
- wpctl volume in the popup (PipeWire isn't visible in the nested session).
- A real media start/stop tween.
- XWayland windows in the overview.
- The systemd user service from the home-manager module in a real login.
