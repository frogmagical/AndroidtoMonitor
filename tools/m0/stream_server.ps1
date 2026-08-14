while ($true) {
  & ffmpeg -hide_banner -f lavfi -readrate 1 -readrate_initial_burst 0 -i "ddagrab=output_idx=2:framerate=30,hwdownload,format=bgra,setpts=N/(30*TB)" -fps_mode passthrough -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -g 60 -b:v 12M -maxrate 12M -bufsize 2M -muxdelay 0 -f mpegts -listen 1 http://127.0.0.1:5001 *>> C:\Users\daiki\tools\vdd\ffmpeg_log.txt
  Start-Sleep -Milliseconds 500
}
