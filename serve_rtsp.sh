#!/usr/bin/env bash
# serve_rtsp.sh — Start an embedded RTSP server (MediaMTX) and publish the live
# Owlet stream to it, so VLC (or any RTSP client) can connect.
#
# Usage (inside the container):
#   ./serve_rtsp.sh <email> <password> <camera_dsn> [rtsp_path]
#
# Then point VLC at:   rtsp://<host-ip>:8554/<rtsp_path>   (default path: owlet)
# Run the container with --network host so port 8554 is reachable.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

EMAIL="${1:-}"
PASSW="${2:-}"
DSN="${3:-}"
RTSP_PATH="${4:-owlet}"

if [[ -z "$EMAIL" || -z "$PASSW" || -z "$DSN" ]]; then
    echo "Usage: $0 <email> <password> <camera_dsn> [rtsp_path]" >&2
    exit 1
fi

MTX_CONF="${MEDIAMTX_CONF:-/etc/mediamtx.yml}"

# Start the RTSP server in the background and stop it when we exit.
echo "[serve_rtsp] Starting MediaMTX RTSP server on :8554 ..." >&2
mediamtx "$MTX_CONF" &
MTX_PID=$!
trap 'kill "$MTX_PID" 2>/dev/null || true' EXIT INT TERM

# Wait for the server to bind 8554 (up to ~5 s).
for _ in $(seq 1 25); do
    if bash -c '</dev/tcp/127.0.0.1/8554' 2>/dev/null; then break; fi
    sleep 0.2
done

echo "[serve_rtsp] RTSP server is up." >&2
echo "[serve_rtsp] Connect VLC to:  rtsp://<host-ip>:8554/${RTSP_PATH}" >&2
echo "[serve_rtsp] (use the container host's LAN IP; 127.0.0.1 works only on the host itself)" >&2

# Publish the camera into MediaMTX. stream.sh detects the rtsp:// target and
# pushes H.264 over RTSP/TCP.
exec "${SCRIPT_DIR}/stream.sh" "$EMAIL" "$PASSW" "$DSN" \
     "rtsp://127.0.0.1:8554/${RTSP_PATH}"
