#!/bin/bash
# Live test: runner hooks for workspace.empty/occupied transitions, env vars,
# and window events, in a nested headless sway (never the real session).
#
#   test/live/runner-transitions.sh [path/to/swell]
#
# Exits non-zero on the first failed check. Needs zenity for test windows.
set -u
here=$(cd "$(dirname "$0")" && pwd)
SWELL=${1:-$here/../../build/swell}
SWELL=$(realpath "$SWELL")
eval "$("$here/nested-sway.sh" start)"
log=$NESTED_DIR/events.log
: >"$log"
pids=()
cleanup() {
  "$SWELL" quit >/dev/null 2>&1
  for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
  "$here/nested-sway.sh" stop "$NESTED_DIR"
  sleep 0.3
  rm -rf "$NESTED_DIR"
}
trap cleanup EXIT

cat >"$SWELL_CONFIG" <<EOF
[osd]
enable = false
[popup]
enable = false
[overview]
enable = false
[[runner.hook]]
on = ["workspace.empty", "workspace.occupied"]
run = 'echo "\$SWELL_EVENT \$SWELL_WORKSPACE" >> $log'
[[runner.hook]]
on = "window.new"
run = 'echo "\$SWELL_EVENT app=\$SWELL_APP_ID" >> $log'
EOF

fail=0
check() { # check DESCRIPTION EXPECTED-LOG
  sleep 0.6
  got=$(cat "$log")
  : >"$log"
  if [ "$got" == "$2" ]; then
    echo "ok   $1"
  else
    echo "FAIL $1"
    echo "  expected: $(printf '%s' "$2" | tr '\n' '|')"
    echo "  got:      $(printf '%s' "$got" | tr '\n' '|')"
    fail=1
  fi
}
win() { # open a window with title $1 on the focused workspace
  zenity --info --title="$1" --text="$1" >/dev/null 2>&1 &
  pids+=($!)
  for _ in $(seq 50); do
    swaymsg -t get_tree | grep -q "\"name\": \"$1\"" && return
    sleep 0.1
  done
  echo "window $1 did not appear" >&2
}

"$SWELL" daemon >"$NESTED_DIR/daemon.log" 2>&1 &
for _ in $(seq 50); do "$SWELL" status >/dev/null 2>&1 && break; sleep 0.1; done
check "initial state" "workspace.empty 1"

win A
check "first window" "window.new app=zenity
workspace.occupied 1"
win B
check "second window: no transition" "window.new app=zenity"

swaymsg -q workspace 2
check "switch to an empty workspace" "workspace.empty 2"
swaymsg -q workspace 1
check "switch back" "workspace.occupied 1"

swaymsg -q '[title="^B$"] move container to workspace 2'
check "move one of two windows away: no transition" ""
swaymsg -q workspace 2
check "switch between two non-empty workspaces" ""
swaymsg -q '[title="^B$"] move container to workspace 1'
check "move the last window away" "workspace.empty 2"
swaymsg -q '[title="^A$"] kill'
swaymsg -q '[title="^B$"] kill'
check "closing windows on an unfocused workspace" ""
swaymsg -q workspace 1
check "focus a workspace (now empty) after its windows closed" ""

win C
check "window on empty workspace" "window.new app=zenity
workspace.occupied 1"
swaymsg -q '[title="^C$"] floating enable'
check "floating toggle: no transition" ""
swaymsg -q '[title="^C$"] move scratchpad'
check "last window to the scratchpad" "workspace.empty 1"
swaymsg -q '[title="^C$"] scratchpad show'
check "scratchpad show" "workspace.occupied 1"
swaymsg -q '[title="^C$"] kill'
check "last window closes" "workspace.empty 1"

if grep -q 'swell' "$NESTED_DIR/daemon.log"; then
  echo "daemon log:"
  sed 's/^/  /' "$NESTED_DIR/daemon.log"
fi
exit $fail
