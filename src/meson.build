# Base files
sources = [
    'txproto_main.c',

    # Lua
    builtin_lua_scripts,
    'lua_common.c',
    'lua_api.c',
    'lua_generic_api.c',

    # Sources
    'iosys_common.c',

    # Outputs
    'interface.c',
    'interface_common.c',

    # Muxing
    'muxing.c',

    # Demuxing
    'demuxing.c',

    # Filtering
    'filtering.c',

    # Encoding
    'encoding.c',

    # Decoding
    'decoding.c',

    # Misc
    'utils.c',
    'logging.c',
    'fifo_frame.c',
    'fifo_packet.c',
    'os_compat.c',
    'ctrl_template.c',

    # Version
    vcs_tag(command: ['git', 'rev-parse', '--short', 'HEAD'],
                      input: 'version.c.in', output: 'version.c',
                      fallback: 'release')
]

# libendit
libedit = dependency('libedit', version: '>= 3.1', required: get_option('libedit'))
if libedit.found()
    dependencies += libedit
    sources += 'cli.c'
    conf.set('HAVE_LIBEDIT', 1)
    features += ', ' + libedit.name() + ' ' + libedit.version()
endif

# libavdevice
libavdevice = dependency('libavdevice', version: '>= 58.9.100', required: get_option('libavdevice'))
if libavdevice.found()
    dependencies += libavdevice
    sources += 'iosys_lavd.c'
    conf.set('HAVE_LAVD', 1)
    features += ', ' + libavdevice.name() + ' ' + libavdevice.version()
endif

# Pulseaudio
libpulse = dependency('libpulse', version: '>= 0.29.0', required: get_option('pulse'))
if libpulse.found()
    dependencies += libpulse
    sources += 'iosys_pulse.c'
    conf.set('HAVE_PULSEAUDIO', 1)
    features += ', ' + libpulse.name() + ' ' + libpulse.version()
endif

# xcb
have_xcb = false
libxcb = dependency('xcb', version: '>= 1.14', required: get_option('xcb'))
if libxcb.found()
    libxcb_shm = dependency('xcb-shm', version: '>= 1.14', required: get_option('xcb'))
    libxcb_randr = dependency('xcb-randr', version: '>= 1.14', required: get_option('xcb'))
    if libxcb_shm.found()
        dependencies += libxcb
        dependencies += libxcb_shm
        dependencies += libxcb_randr
        conf.set('HAVE_XCB', 1)
        features += ', ' + 'xcb' + ' ' + libxcb.version()
        sources += 'iosys_xcb.c'
        have_xcb = true
    endif
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
    features += ', ' + 'wayland' + ' ' + wayland_deps.get(0).version()

    gbm_dep = dependency('gbm', version: '>=17.1.0', required: false)
    if gbm_dep.found()
        dependencies += gbm_dep
        conf.set('HAVE_GBM', 1)
    endif
endif

executable('txproto',
    install: true,
    sources: sources,
    dependencies: dependencies,
)

summary({
    'CLI': libedit.found(),
    'Wayland': have_wayland,
}, section: 'Interface', bool_yn: true)

summary({
    'pulse': libpulse.found(),
    'xcb': have_xcb,
    'wayland': have_wayland,
    'libavdevice': libavdevice.found(),
}, section: 'I/O systems', bool_yn: true)
