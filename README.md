# txproto

Records and streams. Think of it as a bloatless, NIH-less OBS.
Configuration is currently done via src/txproto_main.c, at the very end of the file.
Toggle the if 0/1 flags to enable/disable features like mixing and filtering.
Change the encoder to h264_vaapi to enable hardware encoding.
Change the source via the _target parameters of the capture context structures.
All available sources are listed on capture system init.

There's currently a single command line argument used - the destination URL.
If there's http in the name, the muxer is switched to dash. If rtmp is present, the flv protocol is used.
