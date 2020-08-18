-- State: you don't have to keep this as a single table, but its more convenient
state = {
    muxers = {
        file_1 = { -- value-less entries and unknown keys, are ignored
            name = nil, -- custom name, if nil, will create a sane, possibly non-unique one on muxer creation
            out_url = "dmabuf_recording_01.nut",
            out_format = nil, -- this is populated upon muxer creation if empty
            priv_options = {
                low_latency = true,
            },
        },
        file_2 = { -- value-less entries and unknown keys, are ignored
            out_url = "dmabuf_recording_02.mkv",
            out_format = nil, -- this is populated upon muxer creation if empty
            priv_options = {
                low_latency = true,
            },
        },
    },
    encoders = {
        video_1 = {
            encoder = "libx264",
            options = {
                b = 10^3 --[[ Kbps ]] * 3000,
            },
        },
        audio_1 = {
            encoder = "libopus",
            sample_rate = 0, -- Use input sample rate
            options = {
                b = 10^3 --[[ Kbps ]] * 128,
                frame_duration = 20,
                vbr = "off",
                application = "lowdelay",
            },
        },
        video_2 = {
            encoder = "libx264",
            options = {
                b = 10^3 --[[ Kbps ]] * 2000,
            },
        },
        audio_2 = {
            encoder = "aac",
            sample_rate = 0, -- Use input sample rate
            options = {
                b = 10^3 --[[ Kbps ]] * 128,
            },
        },
    },
    filters = {
        crop = {
            name = "monitor_crop",
            filter = "crop",
            options = {
                w = 1920,
                h = 1080,
            },
        },
        scale = {
            name = "camera_scale",
            filter = "scale",
            options = {
                w = 640,
                h = 360,
            },
        },
        overlay = {
            filter = "overlay",
            options = {
                x = 0,
                y = 0,
            },
            input_pads = { -- Custom pad names, will be set but completely ignored if filter has only 1 input pad
                "screen",
                "camera",
            },
        },
        format = {
            filter = "format",
            options = {
                pix_fmts = "nv12",
            },
        },
        amix = {
            name = "mixer", -- Custom name for logging, if missing will be derived from filter name
            filter = "amix",
            options = {
                weights = "1 1",
            },
            input_pads = { -- Custom pad names, will be set but completely ignored if filter has only 1 input pad
                "main",
                "microphone",
            },
            output_pads = { -- Same as with input_pads, unnecessary with 1-pad filters
                "output",
            },
        }
    }
}

function dump_table_to_string(node, tree, indentation)
    local cache, stack, output = {},{},{}
    local depth = 1

    if type(node) ~= "table" then
        return "only table type is supported, got " .. type(node)
    end

    if nil == indentation then indentation = 1 end

    local NEW_LINE = "\n"
    local TAB_CHAR = " "

    if nil == tree then
        NEW_LINE = "\n"
    elseif not tree then
        NEW_LINE = ""
        TAB_CHAR = ""
    end

    local output_str = "{" .. NEW_LINE

    while true do
        local size = 0
        for k,v in pairs(node) do
            size = size + 1
        end

        local cur_index = 1
        for k,v in pairs(node) do
            if (cache[node] == nil) or (cur_index >= cache[node]) then

                if (string.find(output_str,"}",output_str:len())) then
                    output_str = output_str .. "," .. NEW_LINE
                elseif not (string.find(output_str,NEW_LINE,output_str:len())) then
                    output_str = output_str .. NEW_LINE
                end

                -- This is necessary for working with HUGE tables otherwise we run out of memory using concat on huge strings
                table.insert(output,output_str)
                output_str = ""

                local key
                if (type(k) == "number" or type(k) == "boolean") then
                    key = "["..tostring(k).."]"
                else
                    key = "['"..tostring(k).."']"
                end

                if (type(v) == "number" or type(v) == "boolean") then
                    output_str = output_str .. string.rep(TAB_CHAR,depth*indentation) .. key .. " = "..tostring(v)
                elseif (type(v) == "table") then
                    output_str = output_str .. string.rep(TAB_CHAR,depth*indentation) .. key .. " = {" .. NEW_LINE
                    table.insert(stack,node)
                    table.insert(stack,v)
                    cache[node] = cur_index+1
                    break
                else
                    output_str = output_str .. string.rep(TAB_CHAR,depth*indentation) .. key .. " = '"..tostring(v).."'"
                end

                if (cur_index == size) then
                    output_str = output_str .. NEW_LINE .. string.rep(TAB_CHAR,(depth-1)*indentation) .. "}"
                else
                    output_str = output_str .. ","
                end
            else
                -- close the table
                if (cur_index == size) then
                    output_str = output_str .. NEW_LINE .. string.rep(TAB_CHAR,(depth-1)*indentation) .. "}"
                end
            end

            cur_index = cur_index + 1
        end

        if (size == 0) then
            output_str = output_str .. NEW_LINE .. string.rep(TAB_CHAR,(depth-1)*indentation) .. "}"
        end

        if (#stack > 0) then
            node = stack[#stack]
            stack[#stack] = nil
            depth = cache[node] == nil and depth + 1 or depth - 1
        else
            break
        end
    end

    -- This is necessary for working with HUGE tables otherwise we run out of memory using concat on huge strings
    table.insert(output,output_str)
    output_str = table.concat(output)

    return output_str
end

io_list = {}
video_source_id = nil
audio_source_id = nil
audio_mic_id = nil

function io_update_cb(identifier, entry)
    io_list[identifier] = entry
    if entry ~= nil and entry.name == "eDP-1" then video_source_id = identifier end
    if entry ~= nil and entry.name == "/dev/video0" then video_cam_id = identifier end
    if entry ~= nil and entry.name == "alsa_output.pci-0000_00_1f.3.analog-stereo.monitor" then audio_source_id = identifier end
    if entry ~= nil and entry.name == "alsa_input.pci-0000_00_1f.3.analog-stereo" then audio_mic_id = identifier end
end

function initial_config(...)
    -- Register callback to get input/output list
    tx.register_io_cb(io_update_cb)

    print(dump_table_to_string(io_list, true, 4))

    -- Create a video source
    video_source = tx.create_io(video_source_id)
    video_cam = tx.create_io(video_cam_id, { input_format="mjpeg" })

    -- Create audio sources
    audio_source = tx.create_io(audio_source_id, { buffer_ms=20 })
    audio_mic = tx.create_io(audio_mic_id, { buffer_ms=20 })

    -- Create muxers
    for name,muxer in pairs(state.muxers) do
        tx.create_muxer(muxer)
    end

    -- Create encoders
    for name,encoder in pairs(state.encoders) do
        tx.create_encoder(encoder)
    end

    for name,filter in pairs(state.filters) do
        tx.create_filter(filter)
    end

    tx.link(state.muxers.file_1, state.encoders.video_1)
    tx.link(state.muxers.file_1, state.encoders.audio_1)
    tx.link(state.muxers.file_2, state.encoders.video_2)
    tx.link(state.muxers.file_2, state.encoders.audio_2)

    tx.link(state.encoders.video_1, state.filters.format)
    tx.link(state.encoders.video_2, state.filters.format)
    tx.link(state.encoders.audio_1, state.filters.amix)
    tx.link(state.encoders.audio_2, state.filters.amix)

    tx.link(state.filters.format, state.filters.overlay)
    tx.link(state.filters.overlay, state.filters.crop, "screen")
    tx.link(state.filters.overlay, state.filters.scale, "camera")
    tx.link(state.filters.crop, video_source)
    tx.link(state.filters.scale, video_cam)

    tx.link(state.filters.amix, audio_source, "main")
    tx.link(state.filters.amix, audio_mic, "microphone")

    -- Generated timestamps will start from zero
    tx.set_epoch("zero")

    -- Start everything
    tx.ctrl(video_source, "start")
    tx.ctrl(video_cam, "start")
    tx.ctrl(audio_source, "start")
    tx.ctrl(audio_mic, "start")
    tx.ctrl(state.filters.crop, "start")
    tx.ctrl(state.filters.scale, "start")
    tx.ctrl(state.filters.overlay, "start")
    tx.ctrl(state.filters.format, "start")
    tx.ctrl(state.filters.amix, "start")
    tx.ctrl(state.encoders.video_1, "start")
    tx.ctrl(state.encoders.video_2, "start")
    tx.ctrl(state.encoders.audio_1, "start")
    tx.ctrl(state.encoders.audio_2, "start")
    tx.ctrl(state.muxers.file_1, "start")
    tx.ctrl(state.muxers.file_2, "start")

    tx.commit()
end
