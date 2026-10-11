# tests for the wallpaper module
test_wallpaper_render = executable('test_wallpaper_render',
  'test_wallpaper_render.c', dependencies: modules_dep)
test('wallpaper render', test_wallpaper_render,
  args: [meson.current_build_dir()])

test_wallpaper_viz = executable('test_wallpaper_viz',
  'test_wallpaper_viz.c', dependencies: modules_dep)
test('wallpaper viz', test_wallpaper_viz)
