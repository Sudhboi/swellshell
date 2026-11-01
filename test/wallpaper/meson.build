# tests for the wallpaper module
test_wallpaper_render = executable('test_wallpaper_render',
  'test_wallpaper_render.c', dependencies: modules_dep)
test('wallpaper render', test_wallpaper_render,
  args: [meson.current_build_dir()])

test_wallpaper_viz = executable('test_wallpaper_viz',
  'test_wallpaper_viz.c', dependencies: modules_dep)
test('wallpaper viz', test_wallpaper_viz)

test_wallpaper_gl = executable('test_wallpaper_gl',
  'test_wallpaper_gl.c', dependencies: modules_dep)
test('wallpaper gl', test_wallpaper_gl,
  args: [meson.current_build_dir()])

test_wallpaper_peek = executable('test_wallpaper_peek',
  'test_wallpaper_peek.c', dependencies: modules_dep)
test('wallpaper peek', test_wallpaper_peek,
  args: [meson.current_build_dir()])

test_wallpaper_plymouth = executable('test_wallpaper_plymouth',
  'test_wallpaper_plymouth.c', dependencies: modules_dep)
test('wallpaper plymouth', test_wallpaper_plymouth)
