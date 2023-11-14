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
	f = io.popen("ffmpeg -f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=10.0' -c:a libopus '"..filename.."'")
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

function get_duration(filename)
	f = io.popen("ffprobe -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 '"..filename.."'")
	output = f:read("*a")
	io.close(f)
	return output
end

function common.check_duration(sample1, sample2)
	duration1 = tonumber(get_duration(sample1))
	duration2 = tonumber(get_duration(sample2))
	return duration1 == duration2
end

function common.muxer_eos(event)
	tx.quit()
end

return common
