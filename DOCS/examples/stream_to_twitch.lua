audio_monitor_id = nil
video_display_id = nil
audio_mic_id = nil

function io_update_cb(identifier, entry)
    if video_display_id == nil and entry.type == "display" and entry.default then
        video_display_id = identifier
    end
    if audio_monitor_id == nil and entry.type == "monitor" and entry.default then
        audio_monitor_id = identifier
    end
    if audio_mic_id == nil and entry.type == "microphone" and entry.default then
        audio_mic_id = identifier
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
                preset = "fast",
                keyint_min = 180,
                g = 180,
            }
        })
    encoder_v.link(filter_vid)

    --[[ AUDIO ]]--
    source_monitor = tx.create_io(audio_monitor_id, {
            buffer_ms = 1000 * (1024.0/48000) * 2, -- 2 frames of audio latency is okay
        })

    source_mic = tx.create_io(audio_mic_id, {
            buffer_ms = 1000 * (1024.0/48000) * 2,
        })

    filter_audio = tx.create_filtergraph({
            graph = "[monitor] [mic] amix=inputs=2:weights='0.5 0.9',aresample=192000," ..
                    "loudnorm=I=-22,aresample=48000",
            priv_options = { dump_graph = false },
        })
    filter_audio.link(source_monitor, "monitor")
    filter_audio.link(source_mic, "mic")

    encoder_a = tx.create_encoder({
            encoder = "aac",
            options = {
                b = 10^3 --[[ Kbps ]] * 160,
                aac_coder = "twoloop",
            },
        })
    encoder_a.link(filter_audio, "out0")

    muxer = tx.create_muxer({
            out_url = "rtmp://live-cdg.twitch.tv/app/    TWITCH PRIVATE STREAM KEY GOES HERE",
            out_format = "flv",
            priv_options = { dump_info = true, low_latency = true },
        })
    muxer.link(encoder_v)
    muxer.link(encoder_a)
    muxer.hook("stats", muxer_stats)

    tx.set_status("Initializing...")
    tx.commit()
end
