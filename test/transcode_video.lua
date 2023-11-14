
common = require "common"

function main(...)
    local arg = {...}
    input, output = arg[1], arg[2]

    common.create_video_sample(input)

    tx.set_epoch(0)

    source_f = tx.create_demuxer({
        in_url = input,
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
	priv_options = {
		fifo_size = 16,
		fifo_flags = "block_no_input,block_max_output" },
    })
    encoder_v.link(dec_v)

    muxer_v = tx.create_muxer({
        out_url = output,
        priv_options = { dump_info = true, low_latency = false },
    })
    muxer_v.link(encoder_v)
    muxer_v.schedule("eos", common.muxer_eos);

    tx.commit()
end
