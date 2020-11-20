video_monitor_id = nil

function io_update_cb(identifier, entry)
    if video_monitor_id == nil then
        if entry.name == "eDP-1" or entry.name == "DP-1" then
            video_monitor_id = identifier
        end
    end
end

selection = nil

function selection_cb(result)
    selection = result
end

function initial_config(...)
    local filename = ...
    if filename == nil then filename = "txproto_screen.png" end

    event = tx.register_io_cb(io_update_cb, { "wayland" })
    event.destroy()

    iface = tx.create_interface()
    event = iface.create_selection(selection_cb)

    tx.set_epoch(0)

    source_v = tx.create_io(video_monitor_id, {
            capture_mode = "screencopy",
            oneshot = true,
        })
    source_v.ctrl("start")

    filter = tx.create_filtergraph({
            graph = "format=rgb24,crop",
        })
    filter.link(source_v)
    filter.ctrl("start")

    encoder = tx.create_encoder({
            encoder = "png",
        })
    encoder.link(filter)
    encoder.ctrl("start")

    muxer = tx.create_muxer({
            out_url = filename,
            options = { start_number = 0 },
        })
    muxer.link(encoder)
    muxer.ctrl("start")

    event.await()
    selection.region.scale = nil
    filter.command("crop", selection.region)

    tx.commit()

    tx.quit()
end
