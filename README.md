# Simple ESP32-S3 video streaming from WiFi
# Requirements
- Install FFmpeg
## How to use
`ffmpeg -i input.mp4 -vf "fps=20,scale=160:128:force_original_aspect_ratio=decrease,pad=160:128:(ow-iw)/2:(oh-ih)/2" -f rawvideo -pix_fmt rgb565le video.rgb`

use that script to convert your mp4 to RGB565
