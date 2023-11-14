
common = require "common"

function main(...)
    local arg = {...}
    input, output = arg[1], arg[2]

    common.create_audio_sample(input)

    tx.set_epoch(0)

    source_f = tx.create_demuxer({
            in_url = input,
        })

    dec_f = tx.create_decoder({
            decoder = "opus",
        })
    dec_f.link(source_f);

    encoder_a = tx.create_encoder({
            encoder = "libopus",
            priv_options = {
		fifo_size = 10,
		fifo_flags = "block_no_input,block_max_output" },
        })
    encoder_a.link(dec_f)

    muxer_a = tx.create_muxer({
            out_url = output,
            priv_options = { dump_info = true, low_latency = false },
        })
    muxer_a.link(encoder_a)
    muxer_a.schedule("eos", common.muxer_eos);

    tx.commit()
end
