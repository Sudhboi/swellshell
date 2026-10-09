# tests for the popup module
test_popup_render = executable('test_popup_render', 'test_popup_render.c',
  dependencies: modules_dep)
test('popup render', test_popup_render, args: [meson.current_build_dir()])

test_popup_config = executable('test_popup_config', 'test_popup_config.c',
  dependencies: modules_dep)
test('popup config render', test_popup_config,
  args: [meson.current_build_dir()])

test_popup_ws = executable('test_popup_ws', 'test_popup_ws.c',
  dependencies: modules_dep)
test('popup workspaces', test_popup_ws)
