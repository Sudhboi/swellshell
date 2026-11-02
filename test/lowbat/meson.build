# tests for the lowbat module
test_lowbat_state = executable('test_lowbat_state', 'test_lowbat_state.c',
  dependencies: modules_dep)
test('lowbat state', test_lowbat_state)
test_lowbat_render = executable('test_lowbat_render', 'test_lowbat_render.c',
  dependencies: modules_dep)
test('lowbat render', test_lowbat_render, args: [meson.current_build_dir()])
