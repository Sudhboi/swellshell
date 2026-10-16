#!/bin/sh
# Start a nested niri for live tests, isolated from the real session. niri
# has no headless backend, so it runs as a window inside the current
# Wayland session (winit backend).
#
#   eval "$(test/live/nested-niri.sh start [DIR])"   # exports the env below
#   ... run build-dev/swell daemon, swell ..., niri msg ...
#   test/live/nested-niri.sh stop [DIR]
#
# DIR (default: a new mktemp dir under /tmp) holds a short XDG_RUNTIME_DIR,
# the niri config/log, and the scratch swell socket/config. NIRI=path picks
# the niri binary (default: niri on PATH); NIRI_EXTRA is appended to the
# config (niri may miss later edits to it).
set -eu
cmd=${1:-start}
dir=${2:-}
niri=${NIRI:-niri}

case $cmd in
start)
  : "${WAYLAND_DISPLAY:?needs a Wayland session to open the niri window in}"
  [ -n "$dir" ] || dir=$(mktemp -d /tmp/swn.XXXXXX)
  mkdir -p "$dir/rt"
  chmod 700 "$dir/rt"
  cat >"$dir/niri.kdl" <<CONF
hotkey-overlay { skip-at-startup; }
prefer-no-csd
animations { off; }
${NIRI_EXTRA:-}
CONF
  # The parent display by absolute path, since XDG_RUNTIME_DIR moves.
  parent=$WAYLAND_DISPLAY
  case $parent in /*) ;; *) parent=${XDG_RUNTIME_DIR:?}/$parent ;; esac
  env -u SWAYSOCK -u I3SOCK -u NIRI_SOCKET -u DISPLAY \
    XDG_RUNTIME_DIR="$dir/rt" WAYLAND_DISPLAY="$parent" \
    "$niri" -c "$dir/niri.kdl" >"$dir/niri.log" 2>&1 &
  echo $! >"$dir/niri.pid"
  i=0
  while [ $i -lt 100 ]; do
    sock=$(ls "$dir"/rt/niri.*.sock 2>/dev/null | head -n1 || true)
    wl=$(ls "$dir"/rt/wayland-* 2>/dev/null | grep -v lock | head -n1 || true)
    [ -n "$sock" ] && [ -n "$wl" ] && break
    sleep 0.05
    i=$((i + 1))
  done
  [ -n "$sock" ] || { echo "nested niri failed; see $dir/niri.log" >&2; exit 1; }
  echo "export NESTED_DIR='$dir'"
  echo "export XDG_RUNTIME_DIR='$dir/rt'"
  echo "export WAYLAND_DISPLAY='$(basename "$wl")'"
  echo "export NIRI_SOCKET='$sock'"
  echo "export SWELL_SOCK='$dir/rt/swell.sock'"
  echo "export SWELL_CONFIG='$dir/config.toml'"
  echo "unset SWAYSOCK I3SOCK DISPLAY"
  ;;
stop)
  dir=${dir:-${NESTED_DIR:?}}
  [ -f "$dir/niri.pid" ] && kill "$(cat "$dir/niri.pid")" 2>/dev/null || true
  sleep 0.2
  # keep the dir with KEEP=1 (logs)
  [ "${KEEP:-0}" = 1 ] || rm -rf "$dir"
  ;;
*)
  echo "usage: $0 start|stop [DIR]" >&2
  exit 2
  ;;
esac
