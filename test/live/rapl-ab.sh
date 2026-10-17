#!/bin/sh
# Package power (RAPL, needs sudo to read) of the wallpaper with the topo
# snowflake on the GPU vs the CPU, in a nested headless sway (gles2,
# 2880x1800@120 scale 1.5). Keep the machine otherwise idle, the same
# screen brightness and load for every run.
#
#   test/live/rapl-ab.sh [SWELL] [SECONDS] [RUNS]   (default build-dev/swell 60 3)
#
# Extra wallpaper settings for every run in $AB_TOML (e.g. 'fps = 30').
set -eu
swell=${1:-build-dev/swell}
secs=${2:-60}
runs=${3:-3}
rapl=/sys/class/powercap/intel-rapl:0/energy_uj
sudo -v
energy() { sudo cat "$rapl"; }
max=$(sudo cat /sys/class/powercap/intel-rapl:0/max_energy_range_uj)
for run in $(seq "$runs"); do
  for gpu in true false; do
    eval "$(NESTED_RENDERER=gles2 test/live/nested-sway.sh start </dev/null)"
    swaymsg -q output HEADLESS-1 resolution 2880x1800@120Hz scale 1.5
    printf '[wallpaper]\nintro = false\nlogo_gpu = %s\n%b\n' "$gpu" \
      "${AB_TOML:-}" >"$SWELL_CONFIG"
    "$swell" daemon </dev/null >/dev/null 2>&1 &
    pid=$!
    sleep 5
    e0=$(energy)
    sleep "$secs"
    e1=$(energy)
    [ "$e1" -ge "$e0" ] || e1=$((e1 + max))
    echo "run $run logo_gpu=$gpu: $(((e1 - e0) / secs / 1000)) mW" \
      "($("$swell" wallpaper status | grep -E '^(fps|logo render)' | tr '\n' ' '))"
    kill $pid
    wait $pid 2>/dev/null || true
    test/live/nested-sway.sh stop "$NESTED_DIR" </dev/null
  done
done
