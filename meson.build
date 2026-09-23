project('wslg-protocol-bridge', 'c', version: '0.1.0', default_options: ['c_std=c17', 'warning_level=2'])

wl_client = dependency('wayland-client')
wl_server = dependency('wayland-server')
drm = dependency('libdrm')
gbm = dependency('gbm')
egl = dependency('egl')
gles = dependency('glesv2')

bridge_src = files('src/bridge.c', 'gen/xdg-shell-protocol.c', 'gen/linux-dmabuf-protocol.c')
inc = include_directories('gen')

executable('wslg-bridge', bridge_src,
  include_directories: inc,
  dependencies: [wl_client, wl_server, drm],
  install: false)

executable('mock-parent', ['tests/mock-parent.c', 'gen/xdg-shell-protocol.c'],
  include_directories: inc,
  dependencies: [wl_server],
  install: false)

executable('bridge-suite', ['tests/suite.c', 'gen/xdg-shell-protocol.c', 'gen/linux-dmabuf-protocol.c'],
  include_directories: inc,
  dependencies: [wl_client, drm, gbm],
  install: false)

executable('qualify-drm', 'qualify/qualify.c',
  dependencies: [drm, gbm, egl, gles],
  install: false)
