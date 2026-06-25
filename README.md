# Owlet Dream — Live Video Streaming

Stream live H.264 video from an **Owlet Dream** baby monitor on Linux, without the
Owlet app. The camera uses ThroughTek **TUTK P2P** (not RTSP/RTP); this tool
authenticates to Owlet, connects over P2P, and pipes raw H.264 to ffmpeg.

Output: **1920×1080 H.264 @ 25 fps.**

> Background on how it works and how it was built: see [`summary.md`](summary.md).

---

## Requirements

- Linux x86_64 (glibc)
- `gcc`, `ffmpeg` / `ffplay`
- `python3` with the `requests` package
- Your Owlet account email + password, and the camera's **DSN** (e.g. `OCXXXXCAMERA_DSN`)

```bash
sudo apt install gcc ffmpeg python3 python3-requests   # Debian/Ubuntu
# or:  pip install requests
```

The TUTK native libraries live in `tutk_libs/` (already extracted/patched from the
APK). Nothing else to install.

---


## Using docker
```
docker build -t owlet-stream:latest .
docker run --rm -p 5004:5004 -p 8554:8554 -it owlet-stream:latest ./serve_rtsp.sh you@example.com 'YOUR_PASSWORD' OCXXXXCAMERA_DS
```

Then connect to stream: `rtsp://<host-ip>:8554/owlet`
 
---

## Quick start

```bash
# Stream to RTP (default). In another terminal:
#   ffplay "rtp://127.0.0.1:5004?overrun_nonfatal=1" -fflags nobuffer
./stream.sh you@example.com 'YOUR_PASSWORD' OCXXXXCAMERA_DSN
```

`stream.sh` builds the binaries on first run, authenticates, fetches the camera's
TUTK credentials, connects, and streams. Press **Ctrl-C** to stop.

### Save to a file

```bash
./stream.sh you@example.com 'YOUR_PASSWORD' OCXXXXCAMERA_DSN /tmp/owlet.mp4
```

Stop with **Ctrl-C** — this lets the stream end cleanly so the MP4 is finalized
(don't `kill -9`). `.mkv` and `.h264` are also accepted.

### Custom output

The 4th argument is any ffmpeg-compatible target:

```bash
./stream.sh ... rtp://127.0.0.1:5004     # RTP (default)
./stream.sh ... rtsp://127.0.0.1:8554/cam # RTSP (needs a server listening)
./stream.sh ... /tmp/owlet.mp4            # MP4 file
```

---

## RTSP for VLC (Docker)

To watch in **VLC**, run the container with the embedded RTSP server
(MediaMTX). `serve_rtsp.sh` starts the server and publishes the camera to it:

```bash
docker run --rm -it --network host owlet-stream:latest \
  ./serve_rtsp.sh you@example.com 'YOUR_PASSWORD' OCXXXXCAMERA_DSN
```

Then in VLC: **Media → Open Network Stream** and enter

```
rtsp://<host-ip>:8554/owlet
```

Use the host machine's LAN IP (e.g. `rtsp://192.168.1.50:8554/owlet`). On the
same machine `rtsp://127.0.0.1:8554/owlet` also works. `--network host` is
required so VLC can reach port 8554 and so the camera's UDP P2P traffic can
leave the container. Pass a custom path as a 4th argument
(`./serve_rtsp.sh ... mycam` → `rtsp://<host-ip>:8554/mycam`).

> VLC buffers ~1 s by default; for lower latency set
> *Tools → Preferences → Input/Codecs → Network caching* to a smaller value.

> Tip: quote your password if it contains shell-special characters (`@ # $ ! ^ …`).

---

## Usage notes

- **Single connection.** The camera allows one P2P client at a time. Close the
  Owlet app (and any other instance of this tool) before streaming.
- **First frame.** Expect a few seconds for the P2P handshake; the first keyframe
  (~35 KB) arrives, then smaller P-frames.
- **Audio** is delivered on a separate channel and is not captured by this tool
  (video only).

---

## How it fits together

```
owlet_auth.py   → Firebase login + KMS  → {tutkid, password, authKey}
        │
        ▼
owlet_stream    → TUTK P2P connect + DTLS auth + start video
   (LD_PRELOAD=bionic_compat.so)   → raw H.264 Annex-B to stdout
        │
        ▼
ffmpeg          → RTP / RTSP / MP4
```

`stream.sh` wires these together and sets the required environment:
`TUTK_LIB_DIR`, `LD_LIBRARY_PATH=tutk_libs:…`, `LD_PRELOAD=bionic_compat.so`.

---

## Running pieces manually

Fetch credentials only:

```bash
python3 owlet_auth.py you@example.com 'YOUR_PASSWORD' OCXXXXCAMERA_DSN
# → {"dsn":..., "tutkid":..., "password":..., "authKey":..., "firebase_token":...}
```

Stream by hand (what `stream.sh` does under the hood):

```bash
export TUTK_LIB_DIR="$PWD/tutk_libs"
export LD_LIBRARY_PATH="$PWD/tutk_libs:$LD_LIBRARY_PATH"
export LD_PRELOAD="$PWD/bionic_compat.so"

./owlet_stream <tutkid> <password> <authKey> | \
  ffmpeg -fflags +genpts -analyzeduration 10M -probesize 10M \
         -f h264 -i pipe:0 -c:v copy -movflags +faststart out.mp4
```

Rebuild manually if needed:

```bash
gcc -O2 -o owlet_stream owlet_stream.c -ldl -lpthread
gcc -shared -fPIC -o bionic_compat.so bionic_compat.c -ldl
```

> `bionic_compat.so` is mandatory — it supplies Bionic symbols missing on glibc
> and fixes an `addrinfo` ABI mismatch that otherwise crashes the P2P resolver.
> Rebuild it whenever you edit `bionic_compat.c`.

---

## Troubleshooting

| Symptom | Cause / fix |
|--------|-------------|
| `IOTC_Connect_ByUIDEx failed: -46` | Bad `authKey` (must be 8 alphanumeric chars from KMS) or another client is connected. |
| `avClientStartEx failed: -20041` | DTLS auth fail — wrong KMS password (DTLS identity must stay `"admin"`). |
| Connects but `frames=0` | Camera in standby / no `START` ack. Tool drains `avRecvIOCtrl` until `LIVE_VIDEO_START_RESP`; ensure the camera is awake. |
| `dlopen ...: cannot open shared object file` | `TUTK_LIB_DIR` not set — use `stream.sh`, or export it manually. |
| `moov atom not found` (MP4) | ffmpeg was hard-killed. Stop with Ctrl-C so the file finalizes. |
| ffmpeg "Could not find codec parameters … unspecified size" | No frames reached ffmpeg — check `owlet_stream` stderr above the ffmpeg output. |

---

## Files

| File | Purpose |
|------|---------|
| `stream.sh` | End-to-end launcher (auth → stream → ffmpeg) |
| `serve_rtsp.sh` | Starts the embedded MediaMTX RTSP server and publishes the stream (for VLC) |
| `owlet_auth.py` | Firebase login + KMS credential fetch |
| `owlet_stream.c` / `owlet_stream` | TUTK P2P client → raw H.264 on stdout |
| `bionic_compat.c` / `bionic_compat.so` | `LD_PRELOAD` shim (Bionic symbols + addrinfo ABI fix) |
| `tutk_libs/` | x86_64 TUTK `.so` libraries (extracted/patched from the APK) |
| `summary.md`, `REPORT.md` | How it works / reverse-engineering write-up |
