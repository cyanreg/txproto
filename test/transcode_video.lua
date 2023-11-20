
common = require "common"

function muxer_eos(event)
	print("EOS on muxer")
	-- muxer_v.ctrl("flush")
	-- tx.commit()
	-- common.sleep(1)
	muxer_v.destroy()
	src_frames = common.get_nb_of_frames(src)
	dst_frames = common.get_nb_of_frames(dst)
    	print("Number of frames found in the src: "..src_frames) 
    	print("Number of frames found in the dst: "..dst_frames)
	assert(src_frames == dst_frames, "source and destination tests do not have the same number of frames")
	tx.quit()
end

function main(...)
    local arg = {...}
    src, dst = arg[1], arg[2]

    common.create_video_sample(src)

    tx.set_epoch(0)

    source_f = tx.create_demuxer({
        in_url = src,
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
        out_url = dst,
        priv_options = {
		dump_info = true,
		low_latency = false,
		fifo_flags = "block_no_input,block_max_output"
	},
    })
    muxer_v.link(encoder_v)
    muxer_v.schedule("eos", muxer_eos)

    tx.commit()
end
