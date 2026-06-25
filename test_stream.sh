#!/usr/bin/env bash
# test_stream.sh — Verify ffmpeg pipe mode works inside the container.
#
# Extracts the raw H.264 Annex-B stream from owlet.mp4 and re-pipes it through
# ffmpeg exactly as stream.sh does, but without needing a real camera.
#
# Usage (inside container):  ./test_stream.sh
#   Pass "rtp" as first arg to send to RTP instead of stdout count.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODE="${1:-count}"

echo "[test] ffmpeg version:"
ffmpeg -version 2>&1 | head -3
echo ""

if [[ "$MODE" == "rtp" ]]; then
    # Stream to RTP on loopback so the port is actually exercised
    echo "[test] Streaming owlet.mp4 → RTP rtp://127.0.0.1:5004 (30 s max)..."
    ffmpeg -re -i "$SCRIPT_DIR/owlet.mp4" \
           -c:v copy -an \
           -f rtp "rtp://127.0.0.1:5004" \
           -t 30 -v warning
    echo "[test] RTP send completed successfully."
else
    # Pipe raw H.264 through the same -f h264 -i pipe:0 path that stream.sh uses
    echo "[test] Extracting raw H.264 from owlet.mp4 and piping through ffmpeg (-f h264 -i pipe:0)..."
    ffmpeg -i "$SCRIPT_DIR/owlet.mp4" -c:v copy -an -bsf:v h264_mp4toannexb -f h264 pipe:1 2>/dev/null \
    | ffmpeg -loglevel warning \
             -fflags +genpts \
             -analyzeduration 10M \
             -probesize 10M \
             -f h264 \
             -i pipe:0 \
             -c:v copy -an \
             -f null /dev/null
    echo "[test] Pipe test PASSED — no segfault."
fi
