project('wlstream', 'c',
    license: 'LGPL2.1+',
    default_options: [ 'buildtype=debugoptimized', 'c_std=gnu11'],
)

cc = meson.get_compiler('c')

#Common deps
libm      = cc.find_library('m')
pthreads  = dependency('threads')

#Wayland deps
wayland_client    = dependency('wayland-client')
wayland_protocols = dependency('wayland-protocols', version: '>=1.14')

#Library deps
libavutil     = dependency('libavutil')
libavcodec    = dependency('libavcodec')
libavformat   = dependency('libavformat')
libswresample = dependency('libswresample')
libpulse      = dependency('libpulse')

subdir('protocols')
subdir('src')
