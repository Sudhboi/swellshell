# tests for the osd module
test_osd_parse = executable('test_osd_parse', 'test_osd_parse.c',
  dependencies: modules_dep)
test('osd parse', test_osd_parse)

test_osd_render = executable('test_osd_render', 'test_osd_render.c',
  dependencies: modules_dep)
test('osd render', test_osd_render, args: [meson.current_build_dir()])

test_osd_config = executable('test_osd_config', 'test_osd_config.c',
  dependencies: modules_dep)
test('osd config render', test_osd_config, args: [meson.current_build_dir()])
