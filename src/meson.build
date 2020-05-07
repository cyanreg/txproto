build_opts = [
    '-D_ISOC99_SOURCE', '-D_GNU_SOURCE', '-D_XOPEN_SOURCE=700',
    '-U__STRICT_ANSI__', '-fvisibility=hidden',

    # Warnings
    '-Wall', '-Wundef', '-Wmissing-prototypes', '-Wshadow', '-Wparentheses',
    '-Wpointer-arith', '-Wno-pointer-sign',

    # Warnings to treat as errors
    '-Werror=implicit-function-declaration',
]

cc = meson.get_compiler('c')

static_build = get_option('default_library') == 'static'

dependencies = [
    dependency('libavcodec', version: '>= 58.82.100'),
    dependency('libavformat', version: '>= 58.42.100'),
    dependency('libswresample', version: '>= 3.6.100'),
    dependency('libavfilter', version: '>= 7.79.100'),
    dependency('libavutil', version: '>= 56.43.100'),
    dependency('libavdevice', version: '>= 58.9.100'),
    dependency('libpulse', version: '>= 0.29.0'),
    dependency('threads'),
    wayland_client_protos,
]

# Check for wmain support (Windows/MinGW)
# if cc.links('int wmain(){return 0;}', args: '-municode')
#     conf.set('HAVE_WMAIN', 1)
#     build_opts += '-municode'
# endif

version_src = vcs_tag(command: ['git', 'rev-parse', '--short', 'HEAD'],
                      input: 'version.c.in', output: 'version.c',
                      fallback: 'release')

sources = [
    'txproto_main.c',

    'wayland_common.c',

    'src_pulse.c',
    'src_wayland.c',
    'src_lavd.c',

    'filtering.c',

    'encoding.c',

    'utils.c',
    version_src,
]

executable('txproto', install: true, sources: sources, dependencies: dependencies)
