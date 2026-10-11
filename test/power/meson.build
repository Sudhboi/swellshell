# `swell power off` must succeed at once with no daemon, so a keybind's
# `&& systemctl poweroff` still runs.
test('power-off-no-daemon', find_program('sh'),
  args: ['-c', 'SWELL_SOCK=/nonexistent/swell.sock timeout 2 "$1" power off',
    'sh', swell_exe])
