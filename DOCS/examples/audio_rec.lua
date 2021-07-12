audio_source_id = nil

function io_update_cb(identifier, entry)
    if entry ~= nil and audio_source_id == nil and entry.name == "alsa_output.usb-FiiO_FiiO_USB_DAC_K1-01.analog-stereo.monitor" then audio_source_id = identifier end
    if entry ~= nil and audio_source_id == nil and entry.name == "alsa_output.usb-Focusrite_Scarlett_Solo_USB_Y7GVA3A0647820-00.analog-stereo.monitor" then audio_source_id = identifier end
end

function initial_config(...)
    event = tx.register_io_cb(io_update_cb)
    event.destroy()

    tx.set_epoch(0)

    source_mic = tx.create_io(audio_source_id, {
            buffer_ms = 8,
        })

    encoder_a = tx.create_encoder({
            encoder = "libopus",
            options = {
                b = 10^3 --[[ Kbps ]] * 256,
                application = "audio",
                frame_duration = 120,
                vbr = "on",
            },
        })
    encoder_a.link(source_mic)

    muxer_a = tx.create_muxer({
            out_url = "rec.opus",
            priv_options = { dump_info = true, low_latency = false },
        })
    muxer_a.link(encoder_a)

    tx.commit()

end
