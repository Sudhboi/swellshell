test_ipc = executable('test_ipc', 'test_ipc.c', dependencies: core_dep)
test('core-ipc', test_ipc, timeout: 20)

test_dump = executable('test_dump_roundtrip', 'test_dump_roundtrip.c',
  dependencies: core_dep)
test('core-dump-roundtrip', test_dump)

test_niri = executable('test_niri', 'test_niri.c', dependencies: core_dep)
test('core-niri', test_niri)
