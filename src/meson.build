sources = [
    'wlstream.c',
    'src_pulse.c',
    'src_wldmabuf.c',
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
                   libpulse,
                   libavformat,
                   libswresample,
                   pthreads,
                   libm, ]
)
