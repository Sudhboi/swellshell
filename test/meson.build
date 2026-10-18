# Sanitizer runs: meson setup B -Db_sanitize=address,undefined, then
# meson test -C B --setup asan (font library caches aren't leaks).
add_test_setup('asan',
  env: {
    'LSAN_OPTIONS': 'suppressions=' + meson.current_source_dir() / 'lsan.supp'
      + ':print_suppressions=0',
    'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1',
  },
  timeout_multiplier: 3)

test_config = executable('test_config', 'test_config.c', dependencies: modules_dep)
test('config', test_config)

subdir('core')

# Per-module tests live in test/<module>/meson.build. Keep this list in sync
# with the one in ../meson.build and src/modules/modules.c.
foreach m : ['osd', 'popup', 'media', 'overview', 'workspaces', 'runner',
    'wallpaper', 'power', 'wsosd']
  subdir(m)
endforeach
subdir('live')

# config/config.example.toml must match `swell config --defaults`
# (regenerate with: build/swell config --defaults > config/config.example.toml)
test('example-config-up-to-date', find_program('sh'),
  args: ['-c', 'SWELL_CONFIG=/dev/null "$1" config --defaults | diff -u "$2" -',
    'sh', swell_exe, meson.project_source_root() / 'config/config.example.toml'])
