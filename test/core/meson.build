test_ipc = executable('test_ipc', 'test_ipc.c', dependencies: core_dep)
test('core-ipc', test_ipc, timeout: 20)

test_dump = executable('test_dump_roundtrip', 'test_dump_roundtrip.c',
  dependencies: core_dep)
test('core-dump-roundtrip', test_dump)

test_niri = executable('test_niri', 'test_niri.c', dependencies: core_dep)
test('core-niri', test_niri)

test_peek = executable('test_peek', 'test_peek.c', dependencies: core_dep)
test('core-peek', test_peek)

test_handover = executable('test_handover', 'test_handover.c',
  dependencies: core_dep)
test('core-handover', test_handover, timeout: 20)

test_mpris = executable('test_mpris', 'test_mpris.c', dependencies: core_dep)
test('core-mpris', test_mpris, timeout: 20)
