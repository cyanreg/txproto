io_list = {}
video_source_id = nil
audio_source_id = nil
audio_mic_id = nil

function io_update_cb(identifier, entry)
    io_list[identifier] = entry
    if entry ~= nil and video_source_id == nil and entry.name == "eDP-1" then video_source_id = identifier end
    if entry ~= nil and video_source_id == nil and entry.name == "/dev/video0" then video_cam_id = identifier end
    if entry ~= nil and audio_source_id == nil and entry.name == "alsa_output.usb-FiiO_FiiO_USB_DAC_K1-01.analog-stereo.monitor" then audio_source_id = identifier end
    if entry ~= nil and audio_source_id == nil and entry.name == "alsa_output.usb-262a_FiiO_USB_DAC_K1-01.analog-stereo.monitor" then audio_source_id = identifier end
    if entry ~= nil and audio_source_id == nil and entry.name == "alsa_output.usb-Focusrite_Scarlett_Solo_USB_Y7GVA3A0647820-00.analog-stereo.monitor" then audio_source_id = identifier end
    if entry ~= nil and audio_mic_id == nil and entry.name == "alsa_input.pci-0000_00_1f.3.analog-stereo" then audio_mic_id = identifier end
    if entry ~= nil and audio_mic_id == nil and entry.name == "alsa_input.usb-Focusrite_Scarlett_Solo_USB_Y7GVA3A0647820-00.analog-stereo" then audio_mic_id = identifier end
end

function initial_config(...)
    event = tx.register_io_cb(io_update_cb)
    event.destroy()

    tx.set_epoch(0)

    --[[ VIDEO ]] --
    source_video = tx.create_io(video_source_id, {
            capture_cursor = "1",
            capture_mode = "screencopy",
        })

    filter_vid = tx.create_filtergraph({
            graph = "scale=w=1280:h=-1,format=nv12",
            priv_options = { dump_graph = false },
        })
    filter_vid.link(source_video)

    encoder_v = tx.create_encoder({
            encoder = "libx264",
            options = {
                b = 10^3 * 3000,
                preset = "medium",
                keyint_min = 180,
                g = 180,
            }
        })
    encoder_v.link(filter_vid)

    --[[ AUDIO ]]--
    source_audio = tx.create_io(audio_source_id, {
            buffer_ms = 320,
        })

    filter_mic = tx.create_filter({
            filter = "loudnorm",
            options = {
                I = "-16",
            },
            priv_options = { dump_graph = false },
        })
    filter_mic.link(source_audio)

    encoder_a = tx.create_encoder({
            encoder = "libopus",
            options = {
                b = 10^3 --[[ Kbps ]] * 128,
                application = "audio",
                frame_duration = 120,
                vbr = "on",
            },
        })
    encoder_a.link(filter_mic)

    muxer = tx.create_muxer({
            out_url = "rec.mp4",
            priv_options = { dump_info = true, low_latency = false },
        })
    muxer.link(encoder_v)
    muxer.link(encoder_a)

    tx.commit()
end
