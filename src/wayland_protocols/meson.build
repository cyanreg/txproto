wl_protocol_dir = wayland_deps.get(2).get_variable(pkgconfig : 'pkgdatadir')

wayland_scanner_code = generator(
    wayland_scanner,
    output: '@BASENAME@-protocol.c',
    arguments: ['private-code', '@INPUT@', '@OUTPUT@'],
)

wayland_scanner_client = generator(
    wayland_scanner,
    output: '@BASENAME@-client-protocol.h',
    arguments: ['client-header', '@INPUT@', '@OUTPUT@'],
)

protocols = [
    [wl_protocol_dir, 'stable/xdg-shell/xdg-shell.xml'],
    [wl_protocol_dir, 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml'],
    [wl_protocol_dir, 'unstable/xdg-output/xdg-output-unstable-v1.xml'],
    [wl_protocol_dir, 'unstable/idle-inhibit/idle-inhibit-unstable-v1.xml'],
    [wl_protocol_dir, 'unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml'],
    'wlr-export-dmabuf-unstable-v1.xml',
    'wlr-screencopy-unstable-v1.xml',
    'wlr-layer-shell-unstable-v1.xml',
]

wayland_client_protos_src     = []
wayland_client_protos_headers = []

foreach p : protocols
    xml = join_paths(p)
    wayland_client_protos_src     += wayland_scanner_code.process(xml)
    wayland_client_protos_headers += wayland_scanner_client.process(xml)
endforeach

lib_wl_protos = static_library('wayland_client_protos',
                               wayland_client_protos_src + wayland_client_protos_headers,
                               dependencies: [wayland_deps.get(0), wayland_deps.get(1)])

wayland_client_protos = declare_dependency(
    link_with: lib_wl_protos,
    sources: wayland_client_protos_headers,
)
