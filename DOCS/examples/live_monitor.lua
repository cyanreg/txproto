video_source_id = nil

function io_update_cb(identifier, entry)
    if video_display_id == nil and entry.name == "/dev/video0" then
        video_source_id = identifier
    end
end

function main(...)
    event = tx.register_io_cb(io_update_cb)
    event.destroy()

    tx.set_epoch(0)

    --[[ VIDEO ]] --
    source_video = tx.create_io(video_source_id, {
        })

    filter = tx.create_filtergraph({
            graph = "format=yuv422p",
            priv_options = { dump_graph = false, fifo_size = 1 },
        })
    filter.link(source_video)

    interface = tx.create_interface()

    window = interface.create_display()

    window.link(filter)

    tx.commit()
end
