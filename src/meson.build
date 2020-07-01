build_opts = [
    '-D_ISOC11_SOURCE', '-D_XOPEN_SOURCE=700', '-U__STRICT_ANSI__',

    # Warnings
    '-Wundef', '-Wmissing-prototypes', '-Wshadow', '-Wparentheses',
    '-Wpointer-arith', '-Wno-pointer-sign',

    # Warnings to treat as errors
    '-Werror=implicit-function-declaration',
]

# Required dependencies
threads_dep = dependency('threads')
dependencies = [

    # ffmpeg libs
    dependency('libavcodec', version: '>= 58.82.100'),
    dependency('libavformat', version: '>= 58.42.100'),
    dependency('libswresample', version: '>= 3.6.100'),
    dependency('libavfilter', version: '>= 7.79.100'),
    dependency('libavutil', version: '>= 56.43.100'),

    # placebo
    dependency('libplacebo', version: '>= 2.46.0'),

    # vulkan
    dependency('vulkan', version: '>= 1.1'),

    # lua5.4 required
    dependency('lua', version: '>=5.4'),

    # pthreads
    threads_dep,
    cc.find_library('m', required : true),
]

# Base files
sources = [
    'txproto_main.c',

    # Sources
    'iosys_common.c',

    # Outputs
    'interface.c',
    'interface_common.c',

    # Muxing
    'muxing.c',

    # Filtering
    'filtering.c',

    # Encoding
    'encoding.c',

    # Misc
    'utils.c',
    'fifo_frame.c',
    'fifo_packet.c',

    # Version
    vcs_tag(command: ['git', 'rev-parse', '--short', 'HEAD'],
                      input: 'version.c.in', output: 'version.c',
                      fallback: 'release')
]

# Check for wmain support (Windows/MinGW)
if cc.links('int wmain() { return 0; }', args: '-municode')
     conf.set('HAVE_WMAIN', 1)
     build_opts += '-municode'
endif

# Check for pthread_setname_np
if cc.has_function('pthread_setname_np', prefix: '#include <pthread.h>',
                   args: [ '-D_GNU_SOURCE' ], dependencies: threads_dep)
    conf.set('HAVE_PTHREAD_SETNAME_NP', 1)
    build_opts += '-D_GNU_SOURCE'
endif

# Check for memfd (currently wayland only)
has_memfd = false
if get_option('wayland').auto()
    has_memfd = cc.has_function('memfd_create', prefix: '#include <sys/mman.h>',
                                args: [ '-D_GNU_SOURCE' ])
endif

if has_memfd
    build_opts += '-D_GNU_SOURCE'
endif

add_global_arguments(build_opts, language: 'c')

# linenoise
linenoise_proj = subproject('linenoise')
dependencies += linenoise_proj.get_variable('linenoise_dep')

# libavdevice
libavdevice = dependency('libavdevice', version: '>= 58.9.100', required: get_option('libavdevice'))
if libavdevice.found()
    dependencies += libavdevice
    sources += 'iosys_lavd.c'
    conf.set('HAVE_LAVD', 1)
endif

# Pulseaudio
libpulse = dependency('libpulse', version: '>= 0.29.0', required: get_option('pulse'))
if libpulse.found()
    dependencies += libpulse
    sources += 'iosys_pulse.c'
    conf.set('HAVE_PULSEAUDIO', 1)
endif

# Wayland
wayland_scanner = find_program('wayland-scanner', required: get_option('wayland'))
wayland_deps = [ # Reorder with care, wayland_protocols expects them like this
    dependency('wayland-client', required: get_option('wayland')),
    dependency('wayland-cursor', required: get_option('wayland')),
    dependency('wayland-protocols', version: '>=1.14', required: get_option('wayland')),
    dependency('xkbcommon', version: '>= 0.3.0', required: get_option('wayland')),
    dependency('libdrm', version: '>=2.4.95', required: get_option('wayland')),
]

have_wayland = wayland_deps.length() == 5 and wayland_scanner.found() and has_memfd
if have_wayland
    subdir('wayland_protocols')
    dependencies += wayland_client_protos
    dependencies += wayland_deps.get(3)
    dependencies += wayland_deps.get(4)
    sources += 'iosys_wayland.c'
    sources += 'interface_wayland.c'
    sources += 'wayland_common.c'
    conf.set('HAVE_WAYLAND', 1)
endif

executable('txproto',
    install: true,
    sources: sources,
    dependencies: dependencies,
)

summary({
    'libavdevice': libavdevice.found(),
    'pulse': libpulse.found(),
    'wayland': have_wayland,
}, section: 'I/O systems', bool_yn: true)
