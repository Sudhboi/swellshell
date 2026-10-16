#!/bin/sh
# Start a nested headless sway for live tests, isolated from the real session.
#
#   eval "$(test/live/nested-sway.sh start [DIR])"   # exports the env below
#   ... run build/swell daemon, swell ..., grim, swaymsg ...
#   test/live/nested-sway.sh stop [DIR]
#
# DIR (default: a new mktemp dir under /tmp) holds a short XDG_RUNTIME_DIR,
# the sway config/log, and the scratch swell socket/config, so nothing touches
# the user's real sway, swell, or FIFOs.
set -eu
cmd=${1:-start}
dir=${2:-}

case $cmd in
start)
  [ -n "$dir" ] || dir=$(mktemp -d /tmp/swl.XXXXXX)
  mkdir -p "$dir/rt"
  chmod 700 "$dir/rt"
  cat >"$dir/sway.conf" <<CONF
output HEADLESS-1 resolution 1920x1080 scale 1.25
default_border none
exec_always true
CONF
  # The nested compositor must not inherit the outer session.
  env -u WAYLAND_DISPLAY -u SWAYSOCK -u I3SOCK -u NIRI_SOCKET -u DISPLAY \
    XDG_RUNTIME_DIR="$dir/rt" WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 \
    WLR_RENDERER=pixman sway -c "$dir/sway.conf" >"$dir/sway.log" 2>&1 &
  echo $! >"$dir/sway.pid"
  i=0
  while [ $i -lt 100 ]; do
    sock=$(ls "$dir"/rt/sway-ipc.*.sock 2>/dev/null | head -n1 || true)
    wl=$(ls "$dir"/rt/wayland-* 2>/dev/null | grep -v lock | head -n1 || true)
    [ -n "$sock" ] && [ -n "$wl" ] && break
    sleep 0.05
    i=$((i + 1))
  done
  [ -n "$sock" ] || { echo "nested sway failed; see $dir/sway.log" >&2; exit 1; }
  echo "export NESTED_DIR='$dir'"
  echo "export XDG_RUNTIME_DIR='$dir/rt'"
  echo "export WAYLAND_DISPLAY='$(basename "$wl")'"
  echo "export SWAYSOCK='$sock'"
  echo "export SWELL_SOCK='$dir/rt/swell.sock'"
  echo "export SWELL_CONFIG='$dir/config.toml'"
  echo "unset I3SOCK NIRI_SOCKET DISPLAY"
  ;;
stop)
  dir=${dir:-${NESTED_DIR:?}}
  [ -f "$dir/sway.pid" ] && kill "$(cat "$dir/sway.pid")" 2>/dev/null || true
  sleep 0.2
  # keep the dir with KEEP=1 (screenshots, logs)
  [ "${KEEP:-0}" = 1 ] || rm -rf "$dir"
  ;;
*)
  echo "usage: $0 start|stop [DIR]" >&2
  exit 2
  ;;
esac
