#!/bin/bash
# Live test: swell under a nested niri (never the real session): backend
# choice, workspace/window bus events, wallpaper freeze, wsosd, ws commands,
# niri's overview, and the daemon exiting when niri goes away.
#
#   NIRI=path/to/niri test/live/niri-live.sh [path/to/swell]
#
# Opens a niri window in the current session. Needs zenity for test windows.
set -u
here=$(cd "$(dirname "$0")" && pwd)
SWELL=$(realpath "${1:-$here/../../build-dev/swell}")
NIRI=$(command -v "${NIRI:-niri}")
export NIRI
eval "$("$here/nested-niri.sh" start)"
log=$NESTED_DIR/events.log
: >"$log"
pids=()
cleanup() {
  "$SWELL" quit >/dev/null 2>&1
  for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
  KEEP=${KEEP:-0} "$here/nested-niri.sh" stop "$NESTED_DIR"
}
trap cleanup EXIT

cat >"$SWELL_CONFIG" <<EOF
[osd]
enable = false
[wsosd]
hide_timeout_ms = 300
[[runner.hook]]
on = ["workspace.empty", "workspace.occupied", "workspace.focus"]
run = 'echo "\$SWELL_EVENT \$SWELL_WORKSPACE" >> $log'
[[runner.hook]]
on = ["window.new", "window.close"]
run = 'echo "\$SWELL_EVENT app=\$SWELL_APP_ID" >> $log'
[[runner.hook]]
on = ["wsosd.show", "overview.*", "popup.show", "wallpaper.*"]
run = 'echo "\$SWELL_EVENT \${SWELL_WORKSPACE_INDEX:-}/\${SWELL_WORKSPACE_COUNT:-}" >> $log'
EOF

fail=0
report() { # report DESCRIPTION EXPECTED GOT
  if [ "$3" == "$2" ]; then
    echo "ok   $1"
  else
    echo "FAIL $1"
    echo "  expected: $(printf '%s' "$2" | tr '\n' '|')"
    echo "  got:      $(printf '%s' "$3" | tr '\n' '|')"
    fail=1
  fi
}
check() { # check DESCRIPTION EXPECTED-LOG
  sleep 0.8
  got=$(cat "$log")
  : >"$log"
  report "$1" "$2" "$got"
}
win() { # open a window with title $1 on the focused workspace
  zenity --info --title="$1" --text="$1" >/dev/null 2>&1 &
  pids+=($!)
  for _ in $(seq 50); do
    "$NIRI" msg windows 2>/dev/null | grep -q "Title: \"$1\"" && return
    sleep 0.1
  done
  echo "window $1 did not appear" >&2
}
close() { # close the zenity window titled $1
  pkill -f "^zenity --info --title=$1 "
}
wp_state() { "$SWELL" wallpaper status | sed -n 's/^state: //p'; }

"$SWELL" daemon >"$NESTED_DIR/daemon.log" 2>&1 &
dpid=$!
for _ in $(seq 50); do "$SWELL" status >/dev/null 2>&1 && break; sleep 0.1; done
st=$("$SWELL" status)
report "status says niri" "wm niri" "$(grep '^wm ' <<<"$st")"
report "overview unavailable" "module overview unavailable" \
  "$(grep '^module overview' <<<"$st")"
report "nothing else unavailable" "module overview" \
  "$(grep unavailable <<<"$st" | cut -d' ' -f1-2)"
check "initial state (intro once per login)" "workspace.empty 1
wallpaper.animate /
wallpaper.intro /"

win A
check "first window" "window.new app=zenity
workspace.occupied 1
wallpaper.freeze /"
report "wallpaper frozen" "frozen" "$(wp_state)"

"$SWELL" ws next
check "ws next from the last occupied one: new empty workspace" \
  "workspace.focus 2
wsosd.show 2/2
workspace.empty 2
wallpaper.animate /"
sleep 0.5
"$SWELL" ws next
check "ws next on the trailing empty one stays" ""
"$SWELL" ws prev
check "ws prev" "workspace.focus 1
wsosd.show 1/1
workspace.occupied 1
wallpaper.freeze /"
sleep 0.5
"$SWELL" ws prev
check "ws prev at the top stays" ""
"$SWELL" ws goto 2
check "ws goto 2" "workspace.focus 2
wsosd.show 2/2
workspace.empty 2
wallpaper.animate /"
report "ws reorder is a no-op" "niri numbers workspaces itself; nothing to reorder" \
  "$("$SWELL" ws reorder)"
"$SWELL" ws goto 1 && sleep 0.8 && : >"$log"

close A
check "last window closes" "window.close app=zenity
workspace.empty 1
wallpaper.animate /"

"$NIRI" msg action toggle-overview
check "niri overview opens" "overview.open /"
"$NIRI" msg action toggle-overview
check "niri overview closes" "overview.close /"

"$SWELL" popup toggle
check "popup toggle" "popup.show /"
"$SWELL" popup toggle

kill "$(cat "$NESTED_DIR/niri.pid")"
for _ in $(seq 30); do kill -0 "$dpid" 2>/dev/null || break; sleep 0.1; done
report "daemon exits with niri" "gone" "$(kill -0 "$dpid" 2>/dev/null && echo running || echo gone)"
# Whichever of the Wayland and IPC sockets closes first.
report "daemon logs the lost connection" "1" \
  "$(grep -cE 'lost the (niri IPC|Wayland) connection' "$NESTED_DIR/daemon.log")"

if [ $fail != 0 ]; then
  echo "daemon log:"
  sed 's/^/  /' "$NESTED_DIR/daemon.log"
fi
exit $fail
