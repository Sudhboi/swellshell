test_config = executable('test_config', 'test_config.c', dependencies: modules_dep)
test('config', test_config)

subdir('core')

# Per-module tests live in test/<module>/meson.build.
foreach m : ['osd', 'popup', 'overview', 'workspaces', 'runner', 'wallpaper']
  subdir(m)
endforeach
subdir('live')

# config/config.example.toml must match `swell config --defaults`
# (regenerate with: build/swell config --defaults > config/config.example.toml)
test('example-config-up-to-date', find_program('sh'),
  args: ['-c', 'SWELL_CONFIG=/dev/null "$1" config --defaults | diff -u "$2" -',
    'sh', swell_exe, meson.project_source_root() / 'config/config.example.toml'])
