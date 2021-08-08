audio_monitor_id = nil
video_display_id = nil

function io_update_cb(identifier, entry)
    if video_display_id == nil and entry.type == "display" and entry.default then
        video_display_id = identifier
    end
    if audio_monitor_id == nil and entry.type == "monitor" and entry.default then
        audio_monitor_id = identifier
    end
end

function muxer_stats(stats)
    statusline = "Encoding, bitrate: " .. math.floor(stats.bitrate / 1000) .. " Kbps"
    tx.set_status(statusline)
end

function main(...)
    event = tx.register_io_cb(io_update_cb)
    event.destroy()

    tx.set_epoch(0)

    --[[ VIDEO ]] --
    source_video = tx.create_io(video_display_id, {
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
    source_audio = tx.create_io(audio_monitor_id, {
            buffer_ms = 60,
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
                frame_duration = 60,
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
    muxer.hook("stats", muxer_stats)

    tx.commit()
end
