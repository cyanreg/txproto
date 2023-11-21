local common = {}

function file_exists(name)
   local f=io.open(name,"r")
   if f~=nil then io.close(f) return true else return false end
end

function common.create_audio_sample(filename)
	if file_exists(filename) then
		print("Found "..filename.." audio sample, skipping generation")
		return
	end
	print("Audio sample "..filename.." not found, generating it")
	f = io.popen("ffmpeg -f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=10.0' -c:a flac -frame_duration 20 '"..filename.."'")
	io.close(f)
end

function common.create_video_sample(filename)
	if file_exists(filename) then
		print("Found "..filename.." video sample, skipping generation")
		return
	end
	print("Video sample "..filename.." not found, generating it")
	f = io.popen("ffmpeg -t 10.0 -f lavfi -i 'color=c=black:s=1920x1080' -c:v vp9 '"..filename.."'")
	io.close(f)
end

function common.get_duration(filename)
	command = "ffprobe -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 '"..filename.."'"
	-- print("Launching: "..command)
	f = io.popen(command)
	output = f:read("*a")
	io.close(f)
	return tonumber(output)
end

function common.get_nb_of_frames(filename)
	command = "ffprobe -v error -select_streams 0 -count_frames -show_entries stream=nb_read_frames -of default=noprint_wrappers=1:nokey=1 '"..filename.."'"
	print("Launching: "..command)
	f = io.popen(command)
	output = f:read("*a")
	io.close(f)
	return tonumber(output)
end

function common.muxer_eos(event)
	tx.quit()
end

function common.sleep(dur)
	f = io.popen("sleep "..dur)
	io.close(f)
end


return common
