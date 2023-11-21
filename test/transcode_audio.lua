
common = require "common"

function muxer_eos(event)
	print("EOS on muxer")
	-- ask to destroy the muxer to flush everything
	-- tx.quit()
	-- muxer_a.ctrl("flush")
	-- tx.commit()
	muxer_a.destroy()
	src_frames = common.get_nb_of_frames(src)
	dst_frames = common.get_nb_of_frames(dst)
    	print("Number of frames found in the src: "..src_frames) 
    	print("Number of frames found in the dst: "..dst_frames)
	-- assert(src_frames == dst_frames, "source and destination tests do not have the same number of frames")
	tx.quit()
end

function main(...)
    local arg = {...}
    src, dst = arg[1], arg[2]

    common.create_audio_sample(src)

    tx.set_epoch(0)

    source_f = tx.create_demuxer({
            in_url = src,
        })

    dec_f = tx.create_decoder({
            decoder = "flac",
        })
    dec_f.link(source_f);

    encoder_a = tx.create_encoder({
            encoder = "flac",
	    options = {
		    sample_rate = 48000,
		    frame_duration = 20,
    	    },
            priv_options = {
		fifo_size = 10,
		fifo_flags = "block_no_input,block_max_output" },
        })
    encoder_a.link(dec_f)

    muxer_a = tx.create_muxer({
            out_url = dst,
            priv_options = { dump_info = true, low_latency = false },
        })
    muxer_a.link(encoder_a)
    muxer_a.schedule("eos", muxer_eos);

    tx.commit()
end
