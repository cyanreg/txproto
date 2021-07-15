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
    sp.dump(result)
    selection = result
end

function prompt_cb(result)
    if result ~= nil and result.input ~= nil then
        print("Filename: " .. result.input)
        filename = result.input
    else
        filename = "txproto_screen.png"
    end
end

--[[ All events, unless indicated, are atomic and require either a tx.commit()
     to actually run or tx.discard() to scrap the entire command buffer and
     start over. ]]--
function initial_config(...)

    --[[ Trailing command line arguments appear here. if more than one exists,
         it can be initialized via `local argument1, argument2, etc = ... ]]--
    filename = ...

    --[[ Create a callback to get all current capturable devices on the system.
         This creates an event that can be .destroy()ed to stop it.
         When a callback is registered, this function call will block until
         the callback has been sent for all devices, so no need to .await(). ]]--
    event = tx.register_io_cb(io_update_cb, { "wayland" })
    event.destroy()

    --[[ Create a GUI interface context. Backedn will be autodetected. ]]--
    iface = tx.create_interface()

    --[[ Create a selection/highlight context. Takes one argument, a callback
         which will be called with one argument, a region which may be
         nil if the user does not make a selection. ]]--
    event = iface.create_selection(selection_cb)

    --[[ Sets the offset at which all timestamps start at. Should always be
         zero unless you know what you're doing ]]--
    tx.set_epoch(0)

    --[[ Creates a sink/source context. Takes an identifier (sent as an argument
         to the register_io_cb function) and a table containing device-specific
         options. For wayland, capture_mode may be dmabuf, screencopy or
         screencopy-dmabuf. oneshot = true means only a single frame will be
         captured once started. ]]--
    source_v = tx.create_io(video_monitor_id, {
            capture_mode = "screencopy",
            oneshot = true,
        })

    --[[ Creates a libavfilter graph. Same syntax as ffmpeg. Filter options
         must be supplied as part of the graph. Users can rename pads via
         input_pads (or output_pads) = { list, of, names}. ]]--
    filter = tx.create_filtergraph({
            graph = "format=rgb24,crop",
        })

    --[[ Link the encoder to the source. Actual order is irrelevant,
         source_v.link(filter) will result in the same operation.
         This will also automatically schedule a start event for both upon
         comitting. Takes 1 optional argument - a table of options which may
         contain the following: autostart = bool, to disable autostarting,
         and src_pad and dst_pad to set the desired pad to link to. ]]--
    filter.link(source_v)

    --[[ Creates an encoder. Takes one table of options. ]]--
    encoder = tx.create_encoder({
            encoder = "png",
        })
    encoder.link(filter)

    --[[ While waiting for the user to make a selection we inidialized all we
         need, but now we have no choice but to wait. ]]--
    event.await()

    --[[ If no filename is given, prompt the user to enter one ]]--
    if filename == nil then
        prompt_event = tx.prompt("Enter filename:", prompt_cb)
        prompt_event.await()
    end

    --[[ Creates muxer. Self explanatory. start_number = 0 is a workaround for
         creating screenshots, because libavfilter. ]]--
    muxer = tx.create_muxer({
            out_url = filename,
            options = { start_number = 0 },
        })
    muxer.link(encoder)

    --[[ A region will contain the scale entry to indicate how much it has been
         scaled. All regions are pre-scaled. Just remove the entry, as the crop
         filter doesn't have a scale option. ]]--
    selection.region.scale = nil

    --[[ Send commands to the crop filter from the filtergraph. This commands
         set the w, h, x and y options, which coincide with what out region has.
         Takes in the same options as libavfilter. ]]--
    filter.command("crop", selection.region)

    --[[ Runs all events queued. tx.discard() to discard them. ]]--
    tx.commit()

    --[[ Quit. If this is not called, txproto will keep on running whatever
         it was configured with. ]]--
    tx.quit()
end
