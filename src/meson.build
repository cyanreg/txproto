sources = [
    'wlstream.c',

    'src_pulse.c',
    'src_wayland.c',
    'src_lavd.c',

    'filtering.c',

    'encoding.c',

    'utils.c',
]

executable(
    'wlstream',
    sources,
    install: true,
    dependencies: [wayland_client,
                   wlstream_protos,
                   libavutil,
                   libavcodec,
                   libavdevice,
                   libpulse,
                   libavformat,
                   libavfilter,
                   libswresample,
                   pthreads,
                   libm, ]
)
