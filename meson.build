project('swell', 'c',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>=1.0',
  default_options: ['c_std=gnu11', 'warning_level=2', 'buildtype=debugoptimized'])

fs = import('fs')
cc = meson.get_compiler('c')
add_project_arguments(
  '-D_GNU_SOURCE',
  '-DSWELL_VERSION="@0@"'.format(meson.project_version()),
  cc.get_supported_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers'),
  language: 'c')

wayland_client = dependency('wayland-client')
wayland_protos = dependency('wayland-protocols', version: '>=1.37')
wayland_scanner_dep = dependency('wayland-scanner', native: true)
cairo = dependency('cairo')
pango = dependency('pangocairo')
libjpeg = dependency('libjpeg')
xkbcommon = dependency('xkbcommon')
math = cc.find_library('m', required: false)
pipewire = dependency('libpipewire-0.3')
egl = dependency('egl')
glesv2 = dependency('glesv2')
wayland_egl = dependency('wayland-egl')

# ---- protocols ----
wl_scanner = find_program(wayland_scanner_dep.get_variable('wayland_scanner'), native: true)
wlp_dir = wayland_protos.get_variable('pkgdatadir')
protocol_xml = [
  'protocols/wlr-layer-shell-unstable-v1.xml',
  wlp_dir / 'stable/xdg-shell/xdg-shell.xml',
  wlp_dir / 'stable/viewporter/viewporter.xml',
  wlp_dir / 'staging/fractional-scale/fractional-scale-v1.xml',
  wlp_dir / 'stable/tablet/tablet-v2.xml',
  wlp_dir / 'staging/cursor-shape/cursor-shape-v1.xml',
  wlp_dir / 'staging/ext-idle-notify/ext-idle-notify-v1.xml',
  wlp_dir / 'staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml',
  wlp_dir / 'staging/ext-image-capture-source/ext-image-capture-source-v1.xml',
  wlp_dir / 'staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml',
]
protocol_src = []
protocol_hdr = []
foreach xml : protocol_xml
  base = fs.stem(xml)
  protocol_src += custom_target(base + '-protocol.c',
    input: xml, output: base + '-protocol.c',
    command: [wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
  protocol_hdr += custom_target(base + '-client-protocol.h',
    input: xml, output: base + '-client-protocol.h',
    command: [wl_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
endforeach
protocols = static_library('protocols', protocol_src + protocol_hdr,
  dependencies: wayland_client)
protocols_dep = declare_dependency(link_with: protocols, sources: protocol_hdr)

# ---- vendored libraries (their own warnings stay quiet) ----
i3ipc = static_library('i3ipc', 'lib/i3ipc/i3ipc.c', c_args: ['-w'])
i3ipc_dep = declare_dependency(link_with: i3ipc,
  include_directories: include_directories('lib/i3ipc'))
tomlc17 = static_library('tomlc17', 'lib/tomlc17/tomlc17.c', c_args: ['-w'])
tomlc17_dep = declare_dependency(link_with: tomlc17,
  include_directories: include_directories('lib/tomlc17'))

# ---- core ----
core_inc = include_directories('src/core', 'src/modules')
core_src = files(
  'src/core/anim.h',
  'src/core/battery.c',
  'src/core/buffer.c',
  'src/core/bus.c',
  'src/core/config.c',
  'src/core/ipc.c',
  'src/core/loop.c',
  'src/core/nixlogo.c',
  'src/core/surface.c',
  'src/core/sway.c',
  'src/core/util.c',
  'src/core/wl.c',
)
deps = [wayland_client, cairo, pango, libjpeg, xkbcommon, math, pipewire, egl, glesv2,
  wayland_egl, protocols_dep,
  i3ipc_dep, tomlc17_dep]
core = static_library('swellcore', core_src,
  include_directories: core_inc, dependencies: deps)
core_dep = declare_dependency(link_with: core, include_directories: core_inc,
  dependencies: deps)

# ---- modules: each subdir lists its sources in a <name>_src variable ----
# Keep this list in sync with src/modules/modules.c and test/meson.build.
module_src = files('src/modules/modules.c')
foreach m : ['osd', 'popup', 'overview', 'workspaces', 'runner', 'wallpaper',
    'power']
  subdir('src/modules' / m)
  module_src += get_variable(m + '_src')
endforeach
modules = static_library('swellmodules', module_src, dependencies: core_dep)
modules_dep = declare_dependency(link_whole: modules, dependencies: core_dep)

swell_exe = executable('swell', 'src/main.c', dependencies: modules_dep, install: true)

install_data('config/config.example.toml',
  install_dir: get_option('datadir') / 'doc' / 'swell')

subdir('test')
