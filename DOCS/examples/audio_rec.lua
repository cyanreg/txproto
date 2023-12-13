audio_mic_id = nil

function io_update_cb(identifier, entry)
    if audio_mic_id == nil and entry.type == "microphone" and entry.default then
        audio_mic_id = identifier
    end
end

function muxer_stats(stats)
    statusline = "Encoding, bitrate: " .. math.floor(stats.bitrate / 1000) .. " Kbps"
    tx.set_status(statusline)
end

-- Setup
event = tx.register_io_cb(io_update_cb)
event.destroy()

tx.set_epoch(0)

source_mic = tx.create_io(audio_mic_id, {
        buffer_ms = 8,
    })

encoder_a = tx.create_encoder({
        encoder = "libopus",
        options = {
            b = 10^3 --[[ Kbps ]] * 256,
            application = "audio",
            frame_duration = 20,
            vbr = "on",
        },
    })
encoder_a.link(source_mic)

muxer_a = tx.create_muxer({
        out_url = "rec.opus",
        priv_options = { dump_info = true, low_latency = true },
    })
muxer_a.link(encoder_a)
muxer_a.schedule("stats", muxer_stats)

tx.commit()

function main(...)
    while (true)
    do
        coroutine.yield()
        print("Test")
    end
end
