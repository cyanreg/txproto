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
     * Low-overhead X11 capture via XCB/SHM
     * Wayland capture via the wlr-screencopy-unstable protocol (both software or DMA-BUF frames supported)
 * Second-class libavdevice capture/output support
 * Headless operation supported
 * Optional Vulkan-only GUI via libplacebo, supported window systems:
     * Wayland
 * Minimal dependencies (FFmpeg and Lua required, libplacebo and all custom capture code optional)
 * Liberally licensed (LGPL v2.1)

Building
--------
Complete list of dependencies:

 * FFmpeg (git master currently required)
 * libplacebo (git master)
 * Lua 5.4

Optional dependencies

 * Editline 3.1 (for a command-line interface)
 * libpulse (for Pulseaudio capture)
 * Wayland (wayland-scanner,client,cursor,protocols,xkbcommon and libdrm, for Wayland capture)

To build, simply do:

`meson build`

`ninja -C build`

The resulting binary in `./build/src/txproto` is portable and can be ran anywhere.

To simplify building, `libplacebo` can be built and bundled simultaneously by creating
a `subprojects` directory and cloning the [libplacebo repository](https://code.videolan.org/videolan/libplacebo)
into `subprojects/libplacebo`. If an unsuitable libplacebo version already exist on the
system, use `meson setup --force-fallback-for libplacebo build` to force building from
the `subprojects` directory.

Running
-------
If no scripts are specified, `txproto` will, by default, shadow the entire screen to allow the user to screenshot a portion of the screen via the mouse.

To run a script: `./build/txproto -s <path to script>`. Example scripts can be found in the [examples](./DOCS/examples/)
directory. CLI parameters are described in the [section below](#cli).
Script syntax is described in the [Lua API documentation](./DOCS/lua-scripting.md).

CLI
---
| Argument             | Description                                                                                                                                                                                             |
|----------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| -s `path`            | Specify a script to run.                                                                                                                                                                                |
| -e `string`          | Specifies a script entry function (default: 'main').                                                                                                                                                    |
| -r `string`          | Comma-separated list of system Lua packages to include. Searches the local directory first, then the system directories. **Note:** for security, `io`, `os` and `require` are not loaded by default.    |
| -V `string`          | Specifies a global logging level (quiet, error, warn, info, verbose, debug, trace). For a single component, the syntax is `<component>=<level> `. Multiple values can be given if separated via commas. |
| -L `path`            | Specifies a log file. **Warning:** always at maximum verbose level, this will get big quickly.                                                                                                          |
| -C                   | Enable the command-line interface.                                                                                                                                                                      |
| -h                   | Display help (this).                                                                                                                                                                                    |
| -v                   | Displays program version info.                                                                                                                                                                          |
| `trailing arguments` | Given to the script as variable arguments to the entrypoint (`function main(...) local argument1, argument2, etc = ... end`).                                                                           |

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

Alternatively, for custom inputs, outputs, and muxers, a dynamic linking ABI will be provided to allow for the use of non-free binaries.
