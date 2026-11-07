# swell

> [!NOTE]
> **AI disclaimer:** most of the code and documentation in this repository was written by an AI coding assistant (Claude Code), directed and tested by me on my own machines. Expect rough edges, and please report anything that breaks.

A small desktop shell for Wayland compositors (sway, niri and labwc), running as a single daemon.

swell gives you the parts a tiling compositor leaves out: a volume/brightness OSD, a hold-to-peek info panel, a workspace overview, dynamic workspaces, an animated wallpaper with an idle screensaver, and shell hooks for compositor events. Every part is a module that you can turn off. All of them share one event loop, one Wayland connection and one control socket, so they stay cheap: the wallpaper only animates while you can see it, and sits at 0% CPU otherwise.

The look is built around the Nix snowflake.

## Features

- **OSD**: volume and brightness levels with a turning snowflake. You call it with the level (`swell osd vol 40`), so it works with whatever controls your audio.
- **Popup**: hold a key to peek at the clock, date, volume/brightness/battery, network, CPU/RAM and your workspaces. On an empty workspace it is drawn into the wallpaper instead of a panel.
- **Media**: the current track (cover, title, artist, progress) from any MPRIS player.
- **Overview**: every workspace with live window thumbnails. Drag windows between workspaces, reorder workspaces, keyboard navigation.
- **Dynamic workspaces**: GNOME/niri-style. There is always one empty workspace after the last, and they stay numbered 1..n.
- **Workspace indicator** (`wsosd`): a small row of dots when you switch workspaces.
- **Wallpaper**: a contour map with a glass snowflake, a ring of audio bars around it while music plays (PipeWire), and the battery level. It also has launch and power-off animations, plus an idle screensaver that keeps away while a video plays.
- **Low battery warning**: a panel at 15%, and one at 5% that stays until you plug in.
- **Hooks**: run shell commands on workspace, window, OSD, popup and wallpaper events.
- **Extras**: touchpad gestures for compositors that can't bind them (labwc), a fade between workspaces (sway), and a matching plymouth boot splash.

## Installation

### Nix

The repository is a flake. To try it without installing anything:

```sh
nix run github:Sudhboi/swellshell -- daemon
```

To install it with [home-manager](https://github.com/nix-community/home-manager), add the flake to your inputs and enable the module:

```nix
{
  inputs.swell.url = "github:Sudhboi/swellshell";
  inputs.swell.inputs.nixpkgs.follows = "nixpkgs";
}
```

```nix
# in your home configuration
{ inputs, ... }:
{
  imports = [ inputs.swell.homeManagerModules.default ];

  services.swell = {
    enable = true;
    settings = {
      osd.hide_timeout_ms = 1500;
    };
  };
}
```

This installs swell, writes `settings` to `~/.config/swell/config.toml` (and reloads the running daemon when it changes), and adds a systemd user service bound to `graphical-session.target`. You can change the target with `systemdTarget`.

sway doesn't start `graphical-session.target` by itself. Either start it from your sway config:

```
exec systemctl --user import-environment WAYLAND_DISPLAY SWAYSOCK
exec systemctl --user start graphical-session.target
```

or leave out `enable` and start swell from the compositor instead (see [Setup](#setup)). The flake also provides `packages.<system>.default` and `overlays.default`, if you'd rather install the package yourself.

### From source

You need meson, ninja, pkg-config, a C compiler and these libraries: wayland-client, wayland-egl, wayland-protocols (≥ 1.40), wayland-scanner, cairo, pango, xkbcommon, libpipewire-0.3, EGL/GLESv2, libjpeg, libinput, libudev and libsystemd.

```sh
meson setup build
ninja -C build
meson test -C build
sudo ninja -C build install
```

Some features call these tools at runtime: `wpctl` (WirePlumber, volume in the popup), `nmcli` (NetworkManager, network), `playerctl` (media) and `curl` (cover art from URLs). The Nix package includes them.

### Fonts

Fonts aren't bundled. The default is **IosevkaTerm Nerd Font** (`nerd-fonts.iosevka-term` in nixpkgs). To use another font, set the `font` keys in the config.

## Setup

Start the daemon from your compositor, then bind keys to its commands. swell doesn't change volume or brightness itself: your binds do that, then pass the new level to `swell osd`.

### sway

```
exec_always swell daemon

set $volosd swell osd vol $(pactl get-sink-volume @DEFAULT_SINK@ | awk 'NR==1{print substr($5,1,length($5)-1)}') $(pactl get-sink-mute @DEFAULT_SINK@ | grep -q yes && echo mute)
bindsym XF86AudioRaiseVolume exec pactl set-sink-volume @DEFAULT_SINK@ +5% && $volosd
bindsym XF86AudioLowerVolume exec pactl set-sink-volume @DEFAULT_SINK@ -5% && $volosd
bindsym XF86AudioMute        exec pactl set-sink-mute @DEFAULT_SINK@ toggle && $volosd
bindsym --locked XF86MonBrightnessDown exec swell osd $(brightnessctl set 5%- | sed -En 's/.*\(([0-9]+)%\).*/bri \1/p')
bindsym --locked XF86MonBrightnessUp   exec swell osd $(brightnessctl set +5% | sed -En 's/.*\(([0-9]+)%\).*/bri \1/p')

# hold F10 to peek
bindsym F10 exec swell popup show
bindsym --release F10 exec swell popup hide

bindsym $mod+Tab exec swell overview
bindgesture swipe:up   exec swell overview open
bindgesture swipe:down exec swell overview close

bindgesture swipe:left  exec swell ws next
bindgesture swipe:right exec swell ws prev
bindsym $mod+1 exec swell ws goto 1
# ... and so on up to $mod+0 -> swell ws goto 10
```

Because of `exec_always`, `swaymsg reload` restarts swell too. A new daemon replaces the running one without a flicker. If you use the home-manager service, drop the `exec_always` line.

To fade between workspaces, set `[wsfade] enable = true`. This only covers switches made through `swell ws`, and it adds about one frame of delay.

### niri

Everything except the overview works on niri (use niri's own overview). niri already manages dynamic workspaces, so `swell ws next|prev|goto` just forwards to it.

```kdl
spawn-at-startup "swell" "daemon"

binds {
    XF86AudioRaiseVolume allow-when-locked=true { spawn-sh "pactl set-sink-volume @DEFAULT_SINK@ +5% && swell osd vol $(pactl get-sink-volume @DEFAULT_SINK@ | awk 'NR==1{print substr($5,1,length($5)-1)}') $(pactl get-sink-mute @DEFAULT_SINK@ | grep -q yes && echo mute)"; }
    // ... XF86AudioLowerVolume / XF86AudioMute likewise
    XF86MonBrightnessUp allow-when-locked=true { spawn-sh "swell osd $(brightnessctl set +5% | sed -En 's/.*\\(([0-9]+)%\\).*/bri \\1/p')"; }
    XF86MonBrightnessDown allow-when-locked=true { spawn-sh "swell osd $(brightnessctl set 5%- | sed -En 's/.*\\(([0-9]+)%\\).*/bri \\1/p')"; }

    // niri has no key-release binds yet, so F10 toggles instead of hold-to-peek.
    F10 repeat=false { spawn "swell" "popup" "toggle"; }
}

// Show the wallpaper behind niri's overview too.
layer-rule {
    match namespace="^swell-wallpaper$"
    place-within-backdrop true
}
```

With the home-manager service, `NIRI_SOCKET` must be in the service environment (`niri-session` takes care of this).

### labwc

You need labwc 0.8.3 or later. Everything except `wsfade` works. labwc's workspaces are a fixed pool, so swell shows them as dynamic: only the occupied workspaces plus the focused one, numbered 1..n. Give it a pool that's big enough, and turn off labwc's own workspace popup:

```xml
<desktops number="10" popupTime="0" />
<keyboard>
  <keybind key="W-Right"><action name="Execute" command="swell ws next" /></keybind>
  <keybind key="W-Left"><action name="Execute" command="swell ws prev" /></keybind>
  <keybind key="W-1"><action name="Execute" command="swell ws goto 1" /></keybind>
  <keybind key="F10"><action name="Execute" command="swell popup toggle" /></keybind>
</keyboard>
```

Start swell from `~/.config/labwc/autostart` with `swell daemon &`. With the home-manager service, `LABWC_PID` (or `XDG_CURRENT_DESKTOP=labwc`) must be in the service environment.

labwc can't bind touchpad gestures, so the `gestures` module reads the touchpad itself. By default, a 3-finger swipe left or right switches workspaces, and up or down opens or closes the overview. It needs read access to the touchpad, which this udev rule grants to the logged-in user (touchpads only):

```sh
sudo install -m 644 config/70-swell-touchpad.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger --subsystem-match=input
```

On NixOS, put the rule's last line in `services.udev.extraRules` instead.

<details>
<summary>labwc limitations</summary>

labwc has no IPC, so swell has to work everything out from Wayland protocols:

- No protocol says which workspace a window is on. swell assumes it's the workspace that was active when the window opened or was last focused. A window sent away with `SendToDesktop follow="no"` stays counted where it was until you focus it. Windows that were already open when swell started don't count anywhere until you focus them.
- In the overview, windows are laid out in a grid rather than where they really are. You can't drag them to another workspace or reorder workspaces, and every workspace is shown on one output.

</details>

## Usage

Everything goes through the `swell` command, which talks to the running daemon:

```
swell daemon                        run swell (replaces a running daemon)
swell reload | status | quit
swell config [--defaults]           print the configuration as TOML

swell osd vol N [mute] | bri N      show a level
swell popup show | hide | toggle
swell media show | hide | toggle | play-pause | status
swell overview [toggle | open | close]
swell ws next | prev | goto N | reorder
swell wallpaper status | percent N|auto | intro | outro
swell inhibit on | off | toggle | status
swell lowbat show | hide | status
swell power off                     play the power-off animation
swell runner fire EVENT | list
```

`swell help` lists every command. `swell power off` only plays the animation and then exits; it never shuts down the machine itself. Bind it like this:

```
bindsym $mod+Shift+e exec swell power off && systemctl poweroff
```

## Configuration

swell reads `~/.config/swell/config.toml` (or `$SWELL_CONFIG`). Every key is optional. `swell config --defaults` prints all of them with a short description, and [`config/config.example.toml`](config/config.example.toml) contains the same output.

```toml
[osd]
hide_timeout_ms = 1500

[overview]
font = "IosevkaTerm Nerd Font 12"

[popup]
enable = false            # any module can be turned off

[wallpaper]
style = "classic"         # "topo" (default) or "classic"
```

Apply changes with `swell reload`. If a value is invalid, swell reports it with the file and line and uses the default for that key.

### Hooks

The `runner` module runs shell commands on events. There are none by default.

```toml
[[runner.hook]]
on = "workspace.occupied"     # a glob, e.g. "window.*"
run = ["notify-send \"$SWELL_WORKSPACE has windows\""]
detach = true
```

Commands run through `sh -c`, one at a time and in order. Each gets `$SWELL_EVENT` plus the details listed below.

| event | details |
|---|---|
| `workspace.empty`, `workspace.occupied` | the focused workspace became empty or got windows; `SWELL_WORKSPACE` |
| `workspace.focus`, `.init`, `.removed`, `.rename`, `.move`, `.urgent`, `.reload` | `SWELL_WORKSPACE` |
| `window.new`, `.close`, `.focus`, `.title`, `.move`, `.fullscreen`, `.floating`, `.urgent`, `.mark` | `SWELL_CON_ID`, `SWELL_APP_ID`, `SWELL_TITLE` |
| `output.change` | outputs changed (sway only) |
| `osd.show`, `osd.hide` | `SWELL_OSD_KIND`, `SWELL_OSD_LEVEL`, `SWELL_OSD_MUTED` |
| `popup.show`, `popup.hide`, `popup.expand` | `SWELL_POPUP_IN` (`panel` or `wallpaper`) |
| `media.show`, `media.hide` | `SWELL_MEDIA_TITLE`, `SWELL_MEDIA_ARTIST`, `SWELL_MEDIA_PLAYER` |
| `overview.open`, `overview.close` | |
| `workspaces.renumber` | workspaces were renumbered 1..n (sway only) |
| `wallpaper.animate`, `wallpaper.freeze` | the wallpaper started or stopped animating |
| `idle.inhibit` | `SWELL_INHIBITED` (`1` or `0`) |

Some events don't exist on every compositor. niri has no `window.fullscreen` or `.mark`, and labwc only has `window.new`, `.close`, `.focus` and `.title`. On niri and labwc, `SWELL_WORKSPACE` and `SWELL_CON_ID` are niri's names and ids, or numbers that swell assigns.

### Screensaver and video

After a while idle on an empty workspace, the wallpaper turns into a screensaver. It also covers windows, unless `saver_windows = false`. To keep it away during videos, swell provides `org.freedesktop.ScreenSaver` on the session bus, which browsers, mpv and VLC already call. You can also keep it away by hand with `swell inhibit on`.

## Boot splash

swell can write a [plymouth](https://www.freedesktop.org/wiki/Software/Plymouth/) theme that matches the wallpaper: the glass snowflake on black, with its glow going round while the system loads. Pass your screen size:

```sh
swell wallpaper plymouth /tmp/swell-topo 2880x1800
sudo mkdir -p /usr/share/plymouth/themes/swell-topo
sudo cp /tmp/swell-topo/* /usr/share/plymouth/themes/swell-topo/
sudo plymouth-set-default-theme -R swell-topo
```

On a HiDPI screen, plymouth may upscale the theme and make it blurry. Set `DeviceScale=1` under `[Daemon]` in `/etc/plymouth/plymouthd.conf` before running the last command.

## Development

```sh
nix develop               # compiler, meson, libraries, plus sway and grim for live tests
meson setup build && ninja -C build && meson test -C build
```

Live tests run in a nested headless sway, so they never touch your session:

```sh
eval "$(test/live/nested-sway.sh start)"
build/swell daemon & build/swell osd vol 40; grim shot.png
test/live/nested-sway.sh stop
```

The code is formatted with clang-format (`.clang-format`).

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).

Bundled libraries keep their own licenses: [cJSON](https://github.com/DaveGamble/cJSON) and [tomlc17](https://github.com/cktan/tomlc17) are MIT (see `lib/*/LICENSE`), and i3ipc-simple is CC0.
