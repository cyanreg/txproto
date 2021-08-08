# `tx` module

The main programming API is fully atomic and asynchronous. All actions, unless otherwise indicated,
will only take effect if `tx.commit()` is called, and can be discarded via `tx.discard()`.
All scripts and functions may call `coroutine.yield()` at any point. txproto will automatically
resume execution of other coroutines, if any, and then resume. Callbacks may yield at any point as
well. Finally, certain txproto API calls will yield, notably any event's `await()` method.

Context creation functions
--------------------------

### `tx.register_io_cb(callback)`

Registers a callback that will be called every time a usable input/output appears, is updated or disappears.
Upon calling, the callback function will be ran immediately with everything available and brought up to date
before the register function returns.

Returns a handle for the event, with the following methods available:

| Method      | Action                              |
|-------------|-------------------------------------|
| `destroy()` | Destroy the event                   |
| `await()`   | Await for the event to be triggered |

The callback will be called with a single argument, a Lua table, with the following fields:
```lua
{
    name,               -- String of the input or output (usually device, or device port).
    api,                -- String, name of the API through which data will be transferred. May not be available.
    type,               -- String, type and direction, e.g. "video in+out", or "audio input", or "clock sink".
    description,        -- String, usually a longer, more descriptive version of the name.
    identifier,         -- Userdata. Context needed to activate the device by passing it through `tx.create_io`.
    api_id,             -- Integer, an opaque ID that to the API used identifies the entry
    default,            -- Boolean, true means that this is the default device used (e.g. monitor or audio device).

    video = {           -- Table, only available if the device type is video
        width,          -- Integer, scaled width of the resolution in pixels.
        height,         -- Integer, scaled height of the resolution in pixels.
        scale,          -- Integer, resolution scale. Values greater than 1 signal it's a HiDPI display.
        framerate,      -- Number, framerate in frames per second.
    },

    audio = {           -- Table, only available if the device type is audio
        channel_layout, -- String, identifies the channel layout (e.g. "stereo" or "5.1").
        channels,       -- Integer, number of channels.
        sample_rate,    -- Integer, identifies the native sample rate of the device.
        volume,         -- Number, 0.0 to 1.0, signals the current volume setting of the device.
        format,         -- String, signals the native sample format of the device (e.g. "s16" or "fltp").
    },
}
```

### `tx.create_io(identifier, { table of initial options })`

Initializes an input or output context from a given identifier. The initial options field is optional,
and is equivalent to an `ctrl("opts")` command sent before a `tx.commit()`.

Returns a handle, with the following methods available:

| Method                   | Action                                                                           |
|--------------------------|----------------------------------------------------------------------------------|
| `ctrl(string)`           | Control the device. Read [below](#events-and-control).                           |
| `hook(string, callback)` | Hook a callback to be called every time an [event](#events-and-control) happens. |
| `link(handle)`           | Link two components together. Will start both on `tx.commit()`                   |
| `destroy()`              | Destroy the handle and stop capturing.                                           |

A `ctrl("opts")` will return all available options along with their current value and description.

### `tx.create_encoder(identifier, { table of initial options })`

Initializes an encoder context.

Returns a handle, with the following methods available:

| Method                   | Action                                                                           |
|--------------------------|----------------------------------------------------------------------------------|
| `ctrl(string)`           | Control the device. Read [below](#events-and-control).                           |
| `hook(string, callback)` | Hook a callback to be called every time an [event](#events-and-control) happens. |
| `link(handle)`           | Link two components together. Will start both on `tx.commit()`                   |
| `destroy()`              | Destroy the handle and stop capturing.                                           |

A `ctrl("opts")` will return all available options along with their current value and description. They're
the equivalent to the options `ffmpeg -help=encoder` will provide. The common options like `b` for bitrate
are also available.

# Events and control

The following syntax is used for events:

| Event    | When                                                                                           |
|----------|------------------------------------------------------------------------------------------------|
| `commit` | When either a `ctrl("ctrl:commit")` is sent or `tx.commit()` is called.                        |
| `init`   | After the device has started and has been initialized, and before its configured and ready.    |
| `config` | After the device has started, has established the final parameters, and just before its ready. |

