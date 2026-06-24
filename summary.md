# Owlet Dream Camera — Live Video Streaming (Summary)

Reverse-engineering effort to pull live H.264 video off an **Owlet Dream** baby
monitor over ThroughTek **TUTK P2P**, on a desktop Linux (glibc x86_64) machine,
and pipe it into ffmpeg for RTP/file output.

**Status: ✅ Working.** Streams live **1920×1080 H.264 @ 25 fps**.

---

## 1. Overview

The Owlet app does not use RTSP/RTP. It talks to the camera through the TUTK
ThroughTek P2P SDK (IOTC session layer + AV channel layer). To stream without the
app we had to:

1. Authenticate to Owlet/Firebase and fetch TUTK credentials from Owlet's KMS.
2. Load the TUTK Android (`.so`) libraries on glibc Linux via `dlopen`.
3. Connect over P2P, authenticate the AV channel (DTLS), start video, and read
   raw H.264 frames.
4. Pipe the H.264 Annex-B stream to ffmpeg.

All credentials, the TUTK SDK license key, and the struct layouts were recovered
by decompiling the APK and disassembling the native libraries.

---

## 2. Pipeline / connection flow

```
TUTK_SDK_Set_License_Key(key)
IOTC_Initialize2(0)
avInitialize(512)
IOTC_Get_SessionID()                         -> session_id
IOTC_Connect_ByUIDEx(tutkid, sid, &St_IOTCConnectInput)   # authKey here
avClientStartEx(&St_AVClientStartInConfig, &out)          # DTLS auth here
avSendIOCtrl(ch, 0x1FF /*IPCAM_START*/, byte[8]{0}, 8)
avRecvIOCtrl(...) until type 393396 (LIVE_VIDEO_START_RESP)
loop: avRecvFrameData2(...) -> H.264 Annex-B NALUs -> stdout -> ffmpeg
```

---

## 3. Credentials & endpoints (from the APK)

- **Firebase API key:** `AIzaSyDlfp3urNTbyhCtHOCxZBOjHQf4WuN_Aws` (project `owletcare-prod`)
- **Auth:** `POST https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key={FIREBASE_API_KEY}`
- **TUTK creds:** `GET https://camera-kms.owletdata.com/kms/{dsn}` → `{tutkid, password, authKey}`
- **TUTK SDK license key:** hardcoded in `OwletCareApp.java` DI constructor (long base64 blob).

Three distinct secrets, used in different places:
- `authKey` — 8 alphanumeric chars (e.g. `I0wlWs00`) → used in `IOTC_Connect_ByUIDEx`.
- `password` — KMS password → used as the DTLS password in `avClientStartEx`.
- DTLS **identity is the literal string `"admin"`** (not the tutkid) — app default.

---

## 4. The hard problems and their fixes

### 4.1 Bionic vs glibc ABI mismatches (running Android `.so` on glibc)
- Missing Bionic symbols provided via an `LD_PRELOAD` shim (`bionic_compat.c`):
  `__errno`, `__get_h_errno`, `__sF[3]` (stdin/out/err), `__FD_SET_chk`,
  `__strlen_chk`, `__strncpy_chk2`, `res_init`.
- ELF versioning patched on the `.so` files so glibc would load them.

### 4.2 `IOTC_Connect_ByUIDEx` → -46 (invalid argument)
`St_IOTCConnectInput` had to be reverse-engineered to its exact 160-byte layout.
Key gotcha: `authKey` is an **inline 8-char string at offset 8**, not a pointer,
and a validator requires all 8 chars to be alphanumeric.

### 4.3 SIGSEGV inside the P2P master-name resolver  ← root cause of "no output"
gdb showed `inet_ntop(af=2, src=0x4, …)` in `iotc_lookup_host`. Cause: **Bionic's
`struct addrinfo` swaps `ai_canonname` and `ai_addr`** relative to glibc. The lib
read `ai_addr` at offset 32, where glibc stores a NULL `ai_canonname` → deref of
`NULL+4 = 0x4`.
**Fix:** intercept `getaddrinfo`/`freeaddrinfo` in `bionic_compat.c`, call the real
glibc functions, then deep-copy each node into Bionic-layout structs (tagged with a
magic word so our `freeaddrinfo` can free them safely).

### 4.4 `avClientStartEx` → -20041 (`AV_ER_DTLS_AUTH_FAIL`)
The DTLS PSK **identity** must be `"admin"`, not the tutkid. The app defaults the
`username` arg to `"admin"` (`v0/c.java:208`); `password` is the KMS password;
`security_mode=2`; cipher `"DEFAULT:@SECLEVEL=0"`.

### 4.5 Camera connected but sent no frames
- The frame loop checked for `-11` as "data not ready", but the real TUTK code is
  **`-20012` (`AV_ER_DATA_NOREADY`)** — so it bailed immediately. Fixed the codes:
  `-20012` → poll, `-20013/-20014` (incomplete/lost) → skip, other `<0` → fatal.
- After `IPCAM_START`, the app's request helper waits for the camera's IOCtrl
  reply. We now **drain `avRecvIOCtrl` until `LIVE_VIDEO_START_RESP` (393396)**,
  which makes the camera begin streaming.

### 4.6 Frames received but file empty
`avRecvFrameData2` is the **video-only** buffer (audio comes separately via
`avRecvAudioData`). A bogus "audio flag" check was dropping every video frame.
Removed it — every frame here is H.264 (codec_id=78).

### 4.7 Truncated MP4 (`moov atom not found`)
Only a packaging artifact: `timeout` was SIGKILL/SIGTERM-ing ffmpeg mid-write. Let
`owlet_stream` take **SIGINT** so it exits cleanly, closes stdout, and ffmpeg
finalizes the file. Added `-movflags +faststart` for MP4.

---

## 5. Key struct layouts (reverse-engineered)

**St_IOTCConnectInput — 160 bytes (0xa0):**
| off | field | notes |
|----|-------|-------|
| 0  | `cbSize` u32 = 0xa0 | checked, else -46 |
| 4  | u32 = 0 | must be zero |
| 8  | `authKey[8]` | inline string, 8 alphanumeric chars |
| 16 | `deviceRegion[129]` | inline string, empty OK |
| 148| `timeout` u32 | |
| 152| `auth_type` u32 | 0 = password |

**St_AVClientStartInConfig — 64 bytes (0x40):**
| off | field | value |
|----|-------|-------|
| 0  | `cbSize` u32 | 0x40 |
| 4  | `iotc_session_id` int | |
| 8  | `iotc_channel_id` byte | 0 |
| 12 | `timeout_sec` int | |
| 16 | `account_or_identity` char* | **"admin"** |
| 24 | `password_or_token` char* | KMS password |
| 32 | `resend` int | 1 |
| 36 | `security_mode` int | 2 (DTLS) |
| 40 | `auth_type` int | 0 |
| 44 | `sync_recv_data` int | |
| 48 | `dtls_cipher_suites` char* | "DEFAULT:@SECLEVEL=0" |

**St_AVClientStartOutConfig — 32 bytes (0x20):** `cbSize=0x20`, rest output.

**Frame header (28 bytes):** `[0-1]` codec_id (78 = H264), `[2]` flags (bit0 = I-frame).

---

## 6. Files

| File | Purpose |
|------|---------|
| `owlet_auth.py` | Firebase auth + KMS credential fetch → JSON `{tutkid, password, authKey}` |
| `owlet_stream.c` / `owlet_stream` | TUTK P2P client; writes raw H.264 to stdout |
| `bionic_compat.c` / `bionic_compat.so` | `LD_PRELOAD` shim: Bionic symbols + addrinfo ABI fix |
| `stream.sh` | End-to-end launcher (auth → stream → ffmpeg) |
| `tutk_libs/` | x86_64 TUTK `.so` files extracted/patched from the APK |

---

## 7. Usage

```bash
# Stream to RTP (default).  View: ffplay "rtp://127.0.0.1:5004?overrun_nonfatal=1"
./stream.sh email@example.com 'PASSWORD' OCXXXXCAMERA_DSN

# Stream to MP4 file (Ctrl-C to stop cleanly so the file finalizes)
./stream.sh email@example.com 'PASSWORD' OCXXXXCAMERA_DSN /tmp/owlet.mp4
```

Required environment (set by `stream.sh`):
`TUTK_LIB_DIR=tutk_libs`, `LD_LIBRARY_PATH=tutk_libs:…`, `LD_PRELOAD=bionic_compat.so`.

---

## 8. Verified result

```
codec_name=h264   width=1920   height=1080
avg_frame_rate=25/1   nb_frames=200   duration=13.33s
```

First frame ~35 KB keyframe (codec_id=78), followed by P-frames; valid playable MP4.
