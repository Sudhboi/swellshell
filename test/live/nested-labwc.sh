#!/bin/sh
# Start a nested labwc for live tests, isolated from the real session. It
# runs as a window inside the current Wayland session (wlroots wayland
# backend), with its own XDG_RUNTIME_DIR and config dir (labwc -C DIR).
#
#   eval "$(test/live/nested-labwc.sh start [DIR])"   # exports the env below
#   ... run build-dev/swell daemon, swell ..., zenity ...
#   test/live/nested-labwc.sh stop [DIR]
#
# DIR (default: a new mktemp dir under /tmp) holds a short XDG_RUNTIME_DIR,
# the labwc config dir (rc.xml) and log, and the scratch swell socket/config.
# LABWC=path picks the labwc binary (default: labwc on PATH); LABWC_EXTRA is
# inserted into rc.xml's <labwc_config>; LABWC_DESKTOPS replaces the default
# '<desktops number="10" popupTime="0"/>' element. Keep DIR short: labwc
# fails if the wayland socket path is too long (sun_path, 108 bytes).
#
# Arch host, no labwc installed: take it from nixpkgs. Nixpkgs GL binaries
# need nixpkgs mesa, so point LABWC at a wrapper like:
#   nix build nixpkgs#labwc --out-link S/labwc
#   nix build nixpkgs#mesa --out-link S/mesa
#   S/labwc-wrap:
#     #!/bin/sh
#     m=S/mesa
#     export __EGL_VENDOR_LIBRARY_FILENAMES=$m/share/glvnd/egl_vendor.d/50_mesa.json
#     export GBM_BACKENDS_PATH=$m/lib/gbm
#     export LIBGL_DRIVERS_PATH=$m/lib/dri
#     exec S/labwc/bin/labwc "$@"
#   LABWC=S/labwc-wrap test/live/nested-labwc.sh start
# (only labwc gets the mesa env; test clients run with the host's.)
set -eu
cmd=${1:-start}
dir=${2:-}
labwc=${LABWC:-labwc}

case $cmd in
start)
  : "${WAYLAND_DISPLAY:?needs a Wayland session to open the labwc window in}"
  [ -n "$dir" ] || dir=$(mktemp -d /tmp/swl.XXXXXX)
  mkdir -p "$dir/rt" "$dir/labwc"
  chmod 700 "$dir/rt"
  desktops=${LABWC_DESKTOPS:-'<desktops number="10" popupTime="0"/>'}
  cat >"$dir/labwc/rc.xml" <<CONF
<?xml version="1.0"?>
<labwc_config>
  <core><decoration>server</decoration><gap>0</gap></core>
  $desktops
  ${LABWC_EXTRA:-}
</labwc_config>
CONF
  # Empty autostart/environment so nothing from /etc/xdg/labwc leaks in.
  : >"$dir/labwc/autostart"
  : >"$dir/labwc/environment"
  # The parent display by absolute path, since XDG_RUNTIME_DIR moves.
  parent=$WAYLAND_DISPLAY
  case $parent in /*) ;; *) parent=${XDG_RUNTIME_DIR:?}/$parent ;; esac
  env -u SWAYSOCK -u I3SOCK -u NIRI_SOCKET -u DISPLAY -u LABWC_PID \
    XDG_RUNTIME_DIR="$dir/rt" WAYLAND_DISPLAY="$parent" \
    WLR_BACKENDS=wayland WLR_WL_OUTPUTS=1 \
    "$labwc" -C "$dir/labwc" >"$dir/labwc.log" 2>&1 &
  echo $! >"$dir/labwc.pid"
  i=0
  wl=
  while [ $i -lt 100 ]; do
    wl=$(ls "$dir"/rt/wayland-* 2>/dev/null | grep -v lock | head -n1 || true)
    [ -n "$wl" ] && break
    kill -0 "$(cat "$dir/labwc.pid")" 2>/dev/null || break
    sleep 0.05
    i=$((i + 1))
  done
  [ -n "$wl" ] || { echo "nested labwc failed; see $dir/labwc.log" >&2; exit 1; }
  # LABWC=wrapper execs, so $! is labwc's own pid.
  echo "export NESTED_DIR='$dir'"
  echo "export XDG_RUNTIME_DIR='$dir/rt'"
  echo "export WAYLAND_DISPLAY='$(basename "$wl")'"
  echo "export LABWC_PID='$(cat "$dir/labwc.pid")'"
  echo "export SWELL_SOCK='$dir/rt/swell.sock'"
  echo "export SWELL_CONFIG='$dir/config.toml'"
  echo "unset SWAYSOCK I3SOCK NIRI_SOCKET DISPLAY"
  ;;
stop)
  dir=${dir:-${NESTED_DIR:?}}
  [ -f "$dir/labwc.pid" ] && kill "$(cat "$dir/labwc.pid")" 2>/dev/null || true
  sleep 0.2
  # keep the dir with KEEP=1 (logs)
  [ "${KEEP:-0}" = 1 ] || rm -rf "$dir"
  ;;
*)
  echo "usage: $0 start|stop [DIR]" >&2
  exit 2
  ;;
esac
