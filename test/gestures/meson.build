# tests for the gestures module: telling a swipe's direction
test_gestures = executable('test_gestures', 'test_gestures.c',
  '../../src/modules/gestures/swipe.c',
  dependencies: core_dep,
  include_directories: include_directories('../../src/modules/gestures'))
test('gestures-swipe', test_gestures)
