wl_protocol_dir = wayland_protocols.get_pkgconfig_variable('pkgdatadir')

wayland_scanner = find_program('wayland-scanner')

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
    'wlr-export-dmabuf-unstable-v1.xml',
    'wlr-screencopy-unstable-v1.xml',
]

wlstream_protos_src     = []
wlstream_protos_headers = []

foreach p : protocols
	xml = join_paths(p)
	wlstream_protos_src     += wayland_scanner_code.process(xml)
	wlstream_protos_headers += wayland_scanner_client.process(xml)
endforeach

lib_wl_protos = static_library('wlstream_protos', wlstream_protos_src + wlstream_protos_headers,
                                dependencies: [wayland_client]) # for the include directory

wlstream_protos = declare_dependency(
    link_with: lib_wl_protos,
    sources: wlstream_protos_headers,
)
