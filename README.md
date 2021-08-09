![txproto](./resources/logo.svg)

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

CLI
---
Arguments are optional. By default, txproto will take a screenshot on Wayland.

| Argument             | Description                                                                                 |
|----------------------|---------------------------------------------------------------------------------------------|
| -s `path`            | Specify a script to use                                                                     |
| -e `string`          | Specifies a script entry function (default: 'main')                                         |
| -V `string`          | Specifies a logging level (quiet, error, warn, info, verbose, debug, trace).                |
| -L `path`            | Specifies a log file. Warning: always at maximum verbose level, this will get big.          |
| -h                   | Display help (this).                                                                        |
| -v                   | Displays program version info.                                                              |

Discussions and help
--------------------

Join `#txproto` on Libera, or [#txproto:pars.ee](https://matrix.to/#/#txproto:pars.ee) on Matrix.

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
