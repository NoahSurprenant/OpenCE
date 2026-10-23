#!/bin/sh
# The stream image's entrypoint (docker/server/Dockerfile, target stream): the game in stream mode
# (port/linux/README.md, "Streaming") writing its picture and sound to two FIFOs, and ffmpeg encoding
# them to HLS (H.264 and AAC, short segments, the old ones deleted) for a web server to serve.
#
#   STREAM_SERVER    the server to watch: an IPv4 address or a DNS name (looked up here, as
#                    network.broadcast takes addresses only). Required. A name is looked up again
#                    every STREAM_CHECK_SECONDS; when its address changes (the server's pod was
#                    replaced), this exits with status 3 so the container starts again on the new one.
#   STREAM_HLS_DIR   where the playlist (live.m3u8) and its segments go (default /hls)
#   STREAM_WIDTH, STREAM_HEIGHT, STREAM_FPS                    the picture (854 x 480 at 30)
#   STREAM_VIDEO_BITRATE, STREAM_AUDIO_BITRATE                 x264's and AAC's (1500k, 128k)
#   STREAM_PRESET    x264's preset (veryfast)
#   STREAM_SEGMENT_SECONDS, STREAM_PLAYLIST_SIZE               HLS (2 s segments, 6 in the playlist)
#   STREAM_ENCODER_THREADS                                     x264's threads (2)
#
# The game and ffmpeg run side by side; when either exits, the other is stopped and this exits
# non-zero. SIGTERM stops both (the game leaves its game and exits 0 in under a second).
set -eu

server=${STREAM_SERVER:?STREAM_SERVER: the server to watch (an address or a DNS name)}
hls=${STREAM_HLS_DIR:-/hls}
width=${STREAM_WIDTH:-854}
height=${STREAM_HEIGHT:-480}
fps=${STREAM_FPS:-30}
video_bitrate=${STREAM_VIDEO_BITRATE:-1500k}
audio_bitrate=${STREAM_AUDIO_BITRATE:-128k}
preset=${STREAM_PRESET:-veryfast}
segment=${STREAM_SEGMENT_SECONDS:-2}
playlist_size=${STREAM_PLAYLIST_SIZE:-6}
encoder_threads=${STREAM_ENCODER_THREADS:-2}
check=${STREAM_CHECK_SECONDS:-10}
pipes=${STREAM_PIPE_DIR:-/tmp/stream}

log() { echo "stream.sh: $*" >&2; }

# the server's IPv4 address (a name looked up; nothing while it does not resolve)
resolve() {
  case "$1" in
    *[!0-9.]*) getent ahostsv4 "$1" 2>/dev/null | awk 'NR == 1 { print $1 }' ;;
    *) echo "$1" ;;
  esac
}

address=
until address=$(resolve "$server") && [ -n "$address" ]; do
  log "waiting for $server to resolve"
  sleep 2
done
log "watching $server ($address): ${width}x${height} at $fps fps to $hls/live.m3u8"

mkdir -p "$pipes" "$hls"
rm -f "$pipes/video" "$pipes/audio" "$hls"/live*.ts "$hls"/live.m3u8*
mkfifo "$pipes/video" "$pipes/audio"

game=
encoder=
stop() {
  trap - TERM INT
  [ -z "$game" ] || kill -TERM "$game" 2>/dev/null || true
  [ -z "$encoder" ] || kill -INT "$encoder" 2>/dev/null || true
  wait 2>/dev/null || true
}
trap 'log "stopping"; stop; exit 0' TERM INT

HALO_STREAM=1 HALO_STREAM_VIDEO="$pipes/video" HALO_STREAM_AUDIO="$pipes/audio" \
  HALO_STREAM_WIDTH="$width" HALO_STREAM_HEIGHT="$height" HALO_STREAM_FPS="$fps" \
  HALO_NET_BROADCAST="$address" /opt/opence/halo &
game=$!

# The pipes read as they come (wall-clock timestamps), so the picture and the sound stay together;
# a keyframe every segment, so each segment starts with one.
gop=$((fps * segment))
ffmpeg -hide_banner -loglevel warning -nostdin \
  -f rawvideo -pix_fmt rgb24 -s "${width}x${height}" -r "$fps" -use_wallclock_as_timestamps 1 \
    -thread_queue_size 64 -i "$pipes/video" \
  -f s16le -ar 48000 -ac 2 -use_wallclock_as_timestamps 1 -thread_queue_size 1024 -i "$pipes/audio" \
  -map 0:v -map 1:a \
  -c:v libx264 -preset "$preset" -tune zerolatency -pix_fmt yuv420p -threads "$encoder_threads" \
    -r "$fps" -g "$gop" -keyint_min "$gop" -sc_threshold 0 \
    -b:v "$video_bitrate" -maxrate "$video_bitrate" -bufsize "$video_bitrate" \
  -c:a aac -b:a "$audio_bitrate" -ar 48000 \
  -f hls -hls_time "$segment" -hls_list_size "$playlist_size" -hls_delete_threshold 2 \
    -hls_flags delete_segments+temp_file+independent_segments+omit_endlist \
    -hls_segment_filename "$hls/live%06d.ts" "$hls/live.m3u8" &
encoder=$!

while :; do
  sleep "$check" &
  wait $! || true
  if ! kill -0 "$game" 2>/dev/null; then
    status=0; wait "$game" || status=$?
    game=
    log "the game exited ($status)"; stop; exit 1
  fi
  if ! kill -0 "$encoder" 2>/dev/null; then
    status=0; wait "$encoder" || status=$?
    encoder=
    log "ffmpeg exited ($status)"; stop; exit 1
  fi
  now=$(resolve "$server")
  if [ -n "$now" ] && [ "$now" != "$address" ]; then
    log "$server is now $now (was $address): starting again"
    stop; exit 3
  fi
done
