# swell

A small shell for sway in one daemon. It merges five earlier tools and adds a live wallpaper:

| module | was | what it does |
|---|---|---|
| `osd` | nixosd | volume/brightness OSD with a rotating Nix snowflake |
| `popup` | popup | hold-to-peek panel: clock, date, levels, network, CPU/RAM, workspaces |
| `media` | (new) | what's playing (cover, title, artist, album, progress) in a panel at the top while the popup shows its details |
| `overview` | swayverview | workspace overview with live window thumbnails, drag and drop, keyboard nav |
| `workspaces` (`ws`) | dynwork | GNOME/niri-style dynamic workspaces, always numbered 1..n |
| `runner` | awww-kill | runs shell commands on events (none by default) |
| `wallpaper` (`wp`) | battery GIFs + awww | black wallpaper with a pulsing battery bar, a turning Nix snowflake and a cava-like ring of audio bars around it (PipeWire, stereo); animates only on an empty workspace while you're active, otherwise frozen at 0% CPU |

Every module runs in a single event loop. They share one Wayland connection and one sway IPC connection, and are controlled through one socket:

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
| `workspace.focus`, `.init`, `.removed`, `.rename`, `.move`, `.urgent`, `.reload` | sway workspace events; `SWELL_WORKSPACE` |
| `window.new`, `.close`, `.focus`, `.title`, `.move`, `.fullscreen`, `.floating`, `.urgent`, `.mark` | sway window events; `SWELL_CON_ID`, `SWELL_APP_ID`, `SWELL_TITLE` |
| `output.change` | outputs changed |
| `osd.show`, `osd.hide` | `SWELL_OSD_KIND`, `SWELL_OSD_LEVEL`, `SWELL_OSD_MUTED` |
| `popup.show`, `popup.hide`, `popup.expand` | |
| `media.show`, `media.hide` | `SWELL_MEDIA_TITLE`, `SWELL_MEDIA_ARTIST`, `SWELL_MEDIA_PLAYER` |
| `overview.open`, `overview.close` | |
| `workspaces.renumber` | the workspaces were renamed to 1..n |
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
