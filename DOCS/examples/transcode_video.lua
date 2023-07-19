function muxer_stats(stats)
    statusline = "Encoding, bitrate: " .. math.floor(stats.bitrate / 1000) .. " Kbps"
    tx.set_status(statusline)
end

function main(...)
    tx.set_epoch(0)

    source_f = tx.create_demuxer({
        in_url = "test.webm",
    })

    dec_v = tx.create_decoder({
        decoder = "vp9",
    })
    dec_v.link(source_f, 0);

    encoder_v = tx.create_encoder({
        encoder = "libx264",
        options = {
            b = "5M",
        },
    })
    encoder_v.link(dec_v)

    muxer_v = tx.create_muxer({
        out_url = "test-transcoded.mkv",
        priv_options = { dump_info = true, low_latency = false },
    })
    muxer_v.link(encoder_v)
    muxer_v.schedule("stats", muxer_stats)

    tx.commit()
end
