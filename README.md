# swell

A small shell for sway and niri in one daemon. It merges five earlier tools and adds a live wallpaper:

| module | was | what it does |
|---|---|---|
| `osd` | nixosd | volume/brightness OSD with a rotating Nix snowflake |
| `popup` | popup | hold-to-peek panel: clock, date, levels, network, CPU/RAM, workspaces (on an empty workspace drawn into the wallpaper instead, in its colours; `[popup] wallpaper = false` keeps the panel) |
| `media` | (new) | what's playing (cover, title, artist, album, progress) in a panel at the top while the popup shows its details |
| `overview` | swayverview | workspace overview with live window thumbnails, drag and drop, keyboard nav (sway only; niri has its own) |
| `workspaces` (`ws`) | dynwork | GNOME/niri-style dynamic workspaces, always numbered 1..n (on niri: `next`/`prev`/`goto` map to niri's own actions) |
| `runner` | awww-kill | runs shell commands on events (none by default) |
| `wallpaper` (`wp`) | battery GIFs + awww | black wallpaper with a pulsing battery bar, a turning Nix snowflake and a cava-like ring of audio bars around it (PipeWire, stereo); animates only on an empty workspace while you're active, otherwise frozen at 0% CPU |

Every module runs in a single event loop. They share one Wayland connection and one sway or niri IPC connection (niri when `$NIRI_SOCKET` is set), and are controlled through one socket:

```
swell daemon                 # run everything (replaces a running daemon)
swell osd vol 40 [mute]      # swell osd bri 80, swell osd 40
swell popup show|hide|toggle
swell media show|hide|toggle|play-pause|status
swell overview [open|close|toggle]
swell ws next|prev|goto N|reorder
swell runner fire EVENT [KEY=VALUE...] | list
swell wallpaper status | percent N|auto
swell reload | status | quit | help
swell config [--defaults]    # print the effective configuration as TOML
```

## Configuration

The config file is `~/.config/swell/config.toml` (or `$SWELL_CONFIG`). Every key is optional, and the defaults are the old tools' settings. `swell config --defaults` prints every key with its documentation; `config/config.example.toml` is a copy of that output.

```toml
[osd]
hide_timeout_ms = 1500

[overview]
font = "IosevkaTerm Nerd Font 12"

[popup]
enable = false            # any module can be turned off

[[runner.hook]]               # hooks: none unless you add some
on = "workspace.occupied"
run = ["notify-send \"$SWELL_WORKSPACE has windows\""]
detach = true
```

After editing the file, apply it with `swell reload`, or rerun `swell daemon`, which replaces the running one. Nothing needs recompiling. A bad value is reported with its file and line, and that key keeps its default.

### Runner events

Hook `on` patterns are globs (`window.*`). Commands run through `sh -c` in a single queue, so their order is kept. They get `$SWELL_EVENT` plus the event's details:

| event | details |
|---|---|
| `workspace.empty` / `workspace.occupied` | the focused workspace became empty or got windows (transitions only, and once at startup); `SWELL_WORKSPACE` |
| `workspace.focus`, `.init`, `.removed`, `.rename`, `.move`, `.urgent`, `.reload` | workspace events; `SWELL_WORKSPACE` (on niri: the name, else the index on its output) |
| `window.new`, `.close`, `.focus`, `.title`, `.move`, `.fullscreen`, `.floating`, `.urgent`, `.mark` | window events; `SWELL_CON_ID` (niri: the window id), `SWELL_APP_ID`, `SWELL_TITLE`; niri has no `.fullscreen` or `.mark` |
| `output.change` | outputs changed (sway only) |
| `osd.show`, `osd.hide` | `SWELL_OSD_KIND`, `SWELL_OSD_LEVEL`, `SWELL_OSD_MUTED` |
| `popup.show`, `popup.hide`, `popup.expand` | `SWELL_POPUP_IN` (`panel` or `wallpaper`) |
| `media.show`, `media.hide` | `SWELL_MEDIA_TITLE`, `SWELL_MEDIA_ARTIST`, `SWELL_MEDIA_PLAYER` |
| `overview.open`, `overview.close` | swell's overview on sway, niri's own on niri |
| `workspaces.renumber` | the workspaces were renamed to 1..n (sway only) |
| `wallpaper.animate`, `wallpaper.freeze` | the wallpaper started or stopped animating |

## Sway config

Replace the old tools' lines with:

```
exec_always swell daemon

set $volosd swell osd vol $(pactl get-sink-volume @DEFAULT_SINK@ | awk 'NR==1{print substr($5,1,length($5)-1)}') $(pactl get-sink-mute @DEFAULT_SINK@ | grep -q yes && echo mute)
bindsym XF86AudioRaiseVolume exec pactl set-sink-volume @DEFAULT_SINK@ +5% && $volosd
bindsym XF86AudioLowerVolume exec pactl set-sink-volume @DEFAULT_SINK@ -5% && $volosd
bindsym XF86AudioMute        exec pactl set-sink-mute @DEFAULT_SINK@ toggle && $volosd
bindsym --locked XF86MonBrightnessDown exec swell osd $(brightnessctl set 5%- | sed -En 's/.*\(([0-9]+)%\).*/bri \1/p')
bindsym --locked XF86MonBrightnessUp   exec swell osd $(brightnessctl set +5% | sed -En 's/.*\(([0-9]+)%\).*/bri \1/p')

bindsym f10 exec swell popup show
bindsym --release f10 exec swell popup hide

bindsym $mod+Tab exec swell overview
bindgesture swipe:up   exec swell overview open
bindgesture swipe:down exec swell overview close

bindgesture swipe:left  exec swell ws next
bindgesture swipe:right exec swell ws prev
bindsym $mod+1 exec swell ws goto 1
# ... through $mod+0 -> swell ws goto 10
```

Remove the old `dynwork daemon`, `awww-kill`, `nixosd` and `popup` exec lines, `awww-daemon` and any wallpaper script (the `wallpaper` module draws the wallpaper), the FIFO variables, and the `socat` bindings. `exec_always` means a `swaymsg reload` restarts the daemon with the current config. If you use the home-manager module's systemd service instead, drop the `exec_always` line.

## niri config

Everything except `overview` runs on niri (use niri's own overview; `wsosd` retracts while it's open). niri numbers workspaces and keeps one empty workspace after the last by itself, so `swell ws` only forwards `next`/`prev`/`goto` and `reorder` does nothing. niri's trailing empty workspace isn't shown in the popup strip or `wsosd` unless it's focused, and with several outputs both show the focused output's workspaces.

```kdl
spawn-at-startup "swell" "daemon"

binds {
    XF86AudioRaiseVolume allow-when-locked=true { spawn-sh "pactl set-sink-volume @DEFAULT_SINK@ +5% && swell osd vol $(pactl get-sink-volume @DEFAULT_SINK@ | awk 'NR==1{print substr($5,1,length($5)-1)}') $(pactl get-sink-mute @DEFAULT_SINK@ | grep -q yes && echo mute)"; }
    // ... XF86AudioLowerVolume / XF86AudioMute likewise
    XF86MonBrightnessUp allow-when-locked=true { spawn-sh "swell osd $(brightnessctl set +5% | sed -En 's/.*\\(([0-9]+)%\\).*/bri \\1/p')"; }
    XF86MonBrightnessDown allow-when-locked=true { spawn-sh "swell osd $(brightnessctl set 5%- | sed -En 's/.*\\(([0-9]+)%\\).*/bri \\1/p')"; }

    // niri has no key-release binds yet, so F10 toggles instead of hold-to-peek.
    F10 repeat=false { spawn "swell" "popup" "toggle"; }

    Mod+1 { spawn "swell" "ws" "goto" "1"; }   // or niri's own focus-workspace 1
}

// Show the wallpaper behind niri's overview too, not only inside workspaces.
layer-rule {
    match namespace="^swell-wallpaper$"
    place-within-backdrop true
}
```

Touchpad workspace swipes are built into niri; swell's swipe binds aren't needed. `swell power off && systemctl poweroff` works the same in a `spawn-sh` bind. With the home-manager systemd service, `NIRI_SOCKET` must be in the service environment (niri-session imports it).

## Building

```
nix build                 # or: nix run . -- daemon
nix develop               # meson, ninja, wayland, cairo, pango, xkbcommon, sway, grim
meson setup build && ninja -C build && meson test -C build
```

The runtime tools used by the popup and media panel (`wpctl`, `nmcli`, `playerctl`) are added to `PATH` by the nix package. Fonts are not bundled. The default font is IosevkaTerm Nerd Font (`nerd-fonts.iosevka-term`).

### home-manager

```nix
{
  inputs.swell.url = "path:/home/you/Projects/swell/swellshell";
  # in your home configuration:
  imports = [ inputs.swell.homeManagerModules.default ];
  services.swell = {
    enable = true;
    settings.osd.hide_timeout_ms = 1500;
  };
}
```

This installs swell and writes `settings` to `~/.config/swell/config.toml`, which runs `swell reload` when the file changes. It also adds a systemd user service bound to `graphical-session.target` (set `systemdTarget` to change that). Without `SWAYSOCK` in the service environment, swell asks `sway --get-socketpath`.

## Development

See `HANDOFF.md` for the architecture, the core interfaces and how to add a module. Live tests run in a nested headless sway, so they never touch the real session:

```
eval "$(test/live/nested-sway.sh start)"
build/swell daemon & build/swell osd vol 40; grim shot.png
test/live/nested-sway.sh stop
```
