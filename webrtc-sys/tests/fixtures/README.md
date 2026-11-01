# H.264 decoder fixtures

These two synthetic, single-frame Annex B streams contain only solid colors.
Different SPS dimensions exercise VideoToolbox session replacement without screen
capture or network access. They were generated with FFmpeg and libx264:

```sh
ffmpeg -f lavfi -i color=c=blue:s=64x64 -frames:v 1 -c:v libx264 -preset ultrafast -tune zerolatency -x264-params keyint=1 -f h264 h264-64x64.h264
ffmpeg -f lavfi -i color=c=red:s=96x64 -frames:v 1 -c:v libx264 -preset ultrafast -tune zerolatency -x264-params keyint=1 -f h264 h264-96x64.h264
```
