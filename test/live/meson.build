# Virtual pointer/keyboard driver for live tests in a nested headless sway
# (see nested-sway.sh). Not installed.
vin_xml = ['wlr-virtual-pointer-unstable-v1', 'virtual-keyboard-unstable-v1']
vin_src = []
foreach p : vin_xml
  xml = meson.project_source_root() / 'protocols' / p + '.xml'
  vin_src += custom_target(p + '-vin.c', input: xml, output: p + '-vin-protocol.c',
    command: [wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
  vin_src += custom_target(p + '-vin.h', input: xml, output: p + '-client-protocol.h',
    command: [wl_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
endforeach
executable('vin', 'vin/vin.c', vin_src, dependencies: [wayland_client, xkbcommon],
  c_args: ['-w'])
