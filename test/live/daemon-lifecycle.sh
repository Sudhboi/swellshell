#!/bin/bash
# Live test: daemon replace/quit/signals, reload robustness, ws goto
# validation, idle CPU and fd stability, in a nested headless sway.
#
#   test/live/daemon-lifecycle.sh [path/to/swell]
#
# Prints ok/FAIL per check; exits non-zero if any check failed.
set -u
here=$(cd "$(dirname "$0")" && pwd)
SWELL=$(realpath "${1:-$here/../../build/swell}")
eval "$("$here/nested-sway.sh" start)"
cleanup() {
  for p in $(pgrep -f "^$SWELL daemon"); do
    # only daemons of this nested session
    tr '\0' '\n' </proc/$p/environ 2>/dev/null | grep -qx "SWELL_SOCK=$SWELL_SOCK" &&
      kill "$p"
  done
  "$here/nested-sway.sh" stop "$NESTED_DIR"
  sleep 0.3
  rm -rf "$NESTED_DIR"
}
trap cleanup EXIT

fail=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1${2:+ ($2)}"; fail=1; }
t() { if eval "$2"; then ok "$1"; else bad "$1" "${3:-}"; fi; }
daemons() { # pids of swell daemons on this session's socket
  for p in $(pgrep -f "^$SWELL daemon"); do
    tr '\0' '\n' </proc/$p/environ 2>/dev/null | grep -qx "SWELL_SOCK=$SWELL_SOCK" &&
      echo "$p"
  done
}
wait_up() { for _ in $(seq 50); do "$SWELL" status >/dev/null 2>&1 && return; sleep 0.1; done; }
pid_of() { "$SWELL" status 2>/dev/null | sed -n 's/^pid //p'; }

cat >"$SWELL_CONFIG" <<EOF
[osd]
enable = false
[popup]
enable = false
[overview]
enable = false
[runner]
hook = []
EOF

"$SWELL" daemon >"$NESTED_DIR/d1.log" 2>&1 &
wait_up
p1=$(pid_of)
t "daemon starts" '[ -n "$p1" ]'

"$SWELL" daemon --no-replace >/dev/null 2>&1
t "--no-replace refuses while one runs" '[ $? -ne 0 ] && [ "$(pid_of)" = "$p1" ]'

"$SWELL" daemon >"$NESTED_DIR/d2.log" 2>&1 &
sleep 0.2
wait_up
p2=$(pid_of)
sleep 0.3
t "replace: the new daemon serves" '[ -n "$p2" ] && [ "$p2" != "$p1" ]'
t "replace: the old daemon exited" '! kill -0 $p1 2>/dev/null'
t "replace: socket exists" '[ -S "$SWELL_SOCK" ]'

kill -9 "$p2"
sleep 0.2
t "SIGKILL leaves a stale socket" '[ -S "$SWELL_SOCK" ]'
"$SWELL" daemon --no-replace >/dev/null 2>&1 &
wait_up
p3=$(pid_of)
t "a stale socket is taken over (even with --no-replace)" '[ -n "$p3" ]'

for sig in TERM INT; do
  kill -$sig "$(pid_of)"
  sleep 0.4
  t "SIG$sig removes the socket" '[ ! -e "$SWELL_SOCK" ] && [ -z "$(daemons)" ]'
  "$SWELL" daemon >/dev/null 2>&1 &
  wait_up
done
"$SWELL" quit >/dev/null
sleep 0.4
t "quit removes the socket" '[ ! -e "$SWELL_SOCK" ] && [ -z "$(daemons)" ]'

# Concurrent starts (exec_always on two quick `swaymsg reload`s): exactly
# one daemon must survive and own the socket. Known bug: check-then-bind in
# ipc_server_start races and leaves orphaned daemons running.
for round in 1 2 3 4 5 6 7 8; do
  for _ in 1 2 3 4 5 6; do "$SWELL" daemon >/dev/null 2>&1 & done
  sleep 4
  n=$(daemons | wc -l)
  [ "$n" -eq 1 ] || break
done
t "concurrent starts leave one daemon" '[ "$n" -eq 1 ]' "$n daemons running"
for p in $(daemons); do kill "$p"; done
sleep 0.5

"$SWELL" daemon >"$NESTED_DIR/d3.log" 2>&1 &
wait_up
p=$(pid_of)

# Known bug: a syntax error makes `swell reload` apply all defaults (turning
# disabled modules on and replacing the hooks) and still report success.
printf '[osd\n' >>"$SWELL_CONFIG"
"$SWELL" reload >/dev/null 2>&1
rc=$?
t "reload with a syntax error keeps the running config" \
  '"$SWELL" status | grep -q "module osd disabled"' "osd was started"
t "reload with a syntax error reports an error" '[ $rc -ne 0 ]'
sed -i '$d' "$SWELL_CONFIG"
"$SWELL" reload >/dev/null

# Known bug: `ws goto` passes its argument into a quoted sway command.
rm -f "$NESTED_DIR/pwned"
"$SWELL" ws goto "1\"; exec touch $NESTED_DIR/pwned; workspace \"1" >/dev/null 2>&1
sleep 0.3
t "ws goto rejects non-numeric arguments" '[ ! -e "$NESTED_DIR/pwned" ]' \
  "sway ran the injected exec"

fds1=$(ls /proc/$p/fd | wc -l)
for _ in $(seq 30); do "$SWELL" status >/dev/null; "$SWELL" reload >/dev/null; "$SWELL" ws reorder; done
t1=$(awk '{print $14+$15}' /proc/$p/stat)
sleep 5
t2=$(awk '{print $14+$15}' /proc/$p/stat)
t "idle CPU stays at zero" '[ $((t2 - t1)) -le 1 ]' "$((t2 - t1)) ticks in 5 s"
t "fd count is stable" '[ "$(ls /proc/$p/fd | wc -l)" -eq "$fds1" ]'

# Hook children must not inherit the daemon's sway IPC sockets.
cat >>"$SWELL_CONFIG" <<EOF
[[runner.hook]]
on = "t.fds"
run = 'ls -l /proc/\$\$/fd > $NESTED_DIR/childfds'
EOF
sed -i '/^hook = \[\]$/d' "$SWELL_CONFIG"
"$SWELL" reload >/dev/null
"$SWELL" runner fire t.fds
sleep 0.3
n=$(grep -c 'socket:' "$NESTED_DIR/childfds")
t "hook children inherit no daemon sockets" '[ "$n" -eq 0 ]' "$n sockets"

exit $fail
