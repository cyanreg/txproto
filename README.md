# txproto

A fully scriptable and flexible multimedia streaming/handling program.

Features
--------
 * Fully scriptable via Lua
 * Fully atomic API
 * Every frame is perfect
 * Frame-perfect synchronization between video and audio capture
 * Custom first-class capture/output code for minimal overhead:
     * Most feature-complete and accurate Pulseaudio implementation, including isolated client capture
     * Zero-copy Wayland capture via the wlr-export-dmabuf-unstable protocol
     * Wayland capture via the wlr-screencopy-unstable protocol (both software or DMA-BUF frames supported)
 * Second-class libavdevice capture/output support
 * Headless operation supported
 * Optional Vulkan-only GUI via libplacebo, supported window systems:
     * Wayland
 * Minimal dependencies (FFmpeg and Lua required, libplacebo and all custom capture code optional)
 * Liberally licensed (LGPL v2.1)

Discussions and help
--------------------

Join [#txproto:pars.ee](https://matrix.to/#/#txproto:pars.ee) on Matrix.

Feature policy
--------------
| New feature          | Policy                                 |
|----------------------|----------------------------------------|
| Custom inputs        | Always accepted                        |
| Custom outputs       | Always accepted                        |
| Platform integration | Accepted, via Lua scripts              |
| Custom muxers        | Welcome                                |
| Custom filters       | Unlikely, submit to FFmpeg first       |
| Custom de/encoders   | Never, submit any to FFmpeg            |
