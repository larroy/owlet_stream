#!/usr/bin/env bash
# stream.sh — Authenticate, connect, and stream Owlet Dream camera to RTP/RTSP.
#
# Usage:
#   ./stream.sh <email> <password> <camera_dsn> [output_url]
#
# output_url defaults to rtp://127.0.0.1:5004
# To view the RTP stream:
#   ffplay "rtp://127.0.0.1:5004?overrun_nonfatal=1" -fflags nobuffer
#
# Or save to file:
#   ./stream.sh email pass DSN /tmp/owlet.mp4

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

EMAIL="${1:-}"
PASSW="${2:-}"
DSN="${3:-}"
OUTPUT="${4:-rtp://127.0.0.1:5004}"

if [[ -z "$EMAIL" || -z "$PASSW" || -z "$DSN" ]]; then
    echo "Usage: $0 <email> <password> <camera_dsn> [output_url]" >&2
    exit 1
fi

TUTK_LIB_DIR="${TUTK_LIB_DIR:-${SCRIPT_DIR}/tutk_libs}"
STREAM_BIN="${SCRIPT_DIR}/owlet_stream"

# Build if needed
if [[ ! -x "$STREAM_BIN" ]]; then
    echo "[stream.sh] Building owlet_stream..." >&2
    gcc -O2 -o "$STREAM_BIN" "${SCRIPT_DIR}/owlet_stream.c" -ldl -lpthread
fi

# Authenticate and get credentials
echo "[stream.sh] Authenticating..." >&2
CREDS_FILE=$(mktemp /tmp/owlet_creds.XXXXXX.json)
trap 'rm -f "$CREDS_FILE"' EXIT

python3 "${SCRIPT_DIR}/owlet_auth.py" "$EMAIL" "$PASSW" "$DSN" > "$CREDS_FILE"

TUTKID=$(python3 -c "import json,sys; d=json.load(open('$CREDS_FILE')); print(d['tutkid'])")
CAMPASS=$(python3 -c "import json,sys; d=json.load(open('$CREDS_FILE')); print(d['password'])")
AUTHKEY=$(python3 -c "import json,sys; d=json.load(open('$CREDS_FILE')); print(d.get('authKey',''))")

echo "[stream.sh] tutkid=$TUTKID" >&2
echo "[stream.sh] Streaming to: $OUTPUT" >&2

# Determine ffmpeg output args
if [[ "$OUTPUT" == *.mp4 ]]; then
    FFMPEG_ARGS=(-c:v copy -movflags +faststart "$OUTPUT")
elif [[ "$OUTPUT" == *.mkv || "$OUTPUT" == *.h264 ]]; then
    FFMPEG_ARGS=(-c:v copy "$OUTPUT")
elif [[ "$OUTPUT" == rtp://* ]]; then
    FFMPEG_ARGS=(-c:v copy -f rtp "$OUTPUT")
elif [[ "$OUTPUT" == rtsp://* ]]; then
    # -pkt_size 1200 keeps RTP payloads under the MTU so MediaMTX doesn't remux.
    FFMPEG_ARGS=(-c:v copy -rtsp_transport tcp -pkt_size 1200 -f rtsp "$OUTPUT")
else
    FFMPEG_ARGS=(-c:v copy "$OUTPUT")
fi

# Build compat shim if needed
COMPAT_SO="${SCRIPT_DIR}/bionic_compat.so"
if [[ ! -f "$COMPAT_SO" ]]; then
    echo "[stream.sh] Building bionic_compat.so..." >&2
    gcc -shared -fPIC -o "$COMPAT_SO" "${SCRIPT_DIR}/bionic_compat.c"
fi

# Stream: owlet_stream → ffmpeg
#
# IMPORTANT: the Bionic env (LD_PRELOAD/LD_LIBRARY_PATH) must apply ONLY to
# owlet_stream, not to ffmpeg. bionic_compat.so overrides getaddrinfo to return
# Bionic-layout addrinfo structs for the Android TUTK libs; if ffmpeg inherits
# the preload it reads ai_addr at the wrong offset and every connect() fails with
# "Bad address" (e.g. it can't reach the RTSP server). So we set these inline on
# the owlet_stream command only — the pipe leaves ffmpeg's environment clean.
#   LD_LIBRARY_PATH: tutk_libs first so libc.so/libm.so/libdl.so symlinks resolve
#   LD_PRELOAD: bionic_compat.so supplies missing Bionic symbols (__errno, __sF, …)
#
# ffmpeg needs a generous probe window: the P2P handshake delays the first
# bytes, and the camera sends H.264 without an explicit size until the first
# SPS/I-frame arrives. analyzeduration/probesize give it time to find them.
#
# Timestamps: the raw H.264 the camera sends carries no PTS/DTS, so with -c:v copy
# ffmpeg emits packets with unset timestamps. That is fine for a plain file but
# breaks RTP timing and makes VLC reject the RTSP stream ("Timestamps are unset").
# We synthesize them with the setts bitstream filter, which assigns a fixed-rate
# timestamp per output frame (N = frame index; the H.264 demuxer timebase is
# 1/1200000, so N*48000 = 25 fps). Unlike -use_wallclock_as_timestamps, this is
# immune to the bursty way the camera delivers NALUs — wallclock stamps at read
# time, which goes non-monotonic when one read spans frame boundaries. setts is
# deterministic and always monotonic, and still keeps -c:v copy (no re-encode).
TUTK_LIB_DIR="$TUTK_LIB_DIR" \
LD_LIBRARY_PATH="${TUTK_LIB_DIR}:${LD_LIBRARY_PATH:-}" \
LD_PRELOAD="${COMPAT_SO}${LD_PRELOAD:+:$LD_PRELOAD}" \
"$STREAM_BIN" "$TUTKID" "$CAMPASS" "$AUTHKEY" | \
    ffmpeg -loglevel warning \
           -analyzeduration 10M \
           -probesize 10M \
           -f h264 \
           -i pipe:0 \
           -bsf:v setts=ts=N*48000 \
           "${FFMPEG_ARGS[@]}"
