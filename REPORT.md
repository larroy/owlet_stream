# Owlet Dream Baby Monitor — Reverse Engineering & Live Streaming Report

## Overview

This report documents the process of reverse engineering the Owlet Dream baby monitor Android app to discover how video is transmitted, and the subsequent implementation of a live H.264 streaming pipeline without the official app.

---

## 1. Target

**App:** Owlet Dream Baby Monitor  
**Package:** `com.owletcare.sleep`  
**Version:** 3.35.1 (build 62160)  
**APK format:** APKM bundle (ZIP containing `base.apk` + architecture/DPI/language split APKs)  
**File:** `com.owletcare.sleep_3.35.1-62160_4arch_7dpi_24lang_25673a869f9cb19be9bebfe54acaef70_apkmirror.com.apkm`

---

## 2. APK Extraction

The `.apkm` format is a ZIP archive. The main application code lives in `base.apk`, while native libraries are distributed in per-architecture split APKs.

```bash
# Extract base APK from bundle
unzip *.apkm base.apk -d owlet_extracted/

# Extract x86_64 native libraries for use on a Linux desktop
unzip *.apkm split_config.x86_64.apk
cd tutk_libs && unzip -j ../split_config.x86_64.apk "lib/x86_64/*"
```

Native libraries extracted from the x86_64 split:

| Library | Purpose |
|---|---|
| `libTUTKGlobalAPIs.so` | TUTK SDK global init, license, region |
| `libIOTCAPIs.so` | P2P IOTC session management |
| `libAVAPIs.so` | Audio/video channel over IOTC |
| `libP2PTunnelAPIs.so` | Optional tunnel layer |
| `libRDTAPIs.so` | Reliable data transfer layer |
| `libKinesisVideoProducerJNI.so` | AWS Kinesis (used for recorded clips only) |

Resource decoding was done with `apktool`:

```bash
apktool d owlet_extracted/base.apk -o owlet_base_dec/
```

This decoded the binary XML resources into readable form, including `res/values/strings.xml` which contains Firebase configuration constants.

---

## 3. Java Decompilation

### Tool: jadx

`jadx` is an open-source Java decompiler for Android DEX bytecode. It converts `.dex` files back to human-readable Java source. It was not available in the distribution package manager, so it was downloaded manually from GitHub releases (v1.5.0).

```bash
/tmp/jadx/bin/jadx -d owlet_src/ owlet_extracted/base.apk
```

`jadx` handles:
- Converting Dalvik bytecode (`.dex`) → Java source
- Resolving obfuscated class/method/field names where possible
- Decoding Kotlin metadata annotations to recover original names
- Producing `sources/` (Java) and `resources/` output directories

The app uses ProGuard/R8 obfuscation, so most package and class names are single-letter identifiers (e.g. `c1.a`, `u0.m`). Despite 143 decompilation errors in a 200+ MB app, all required classes were successfully recovered.

---

## 4. Discovering the Streaming Technology

The first investigation goal was identifying *how* video is transmitted. The obvious candidates were RTP/RTSP, WebRTC, and HLS/DASH. None of these were found in the network layer.

Instead, the app imports:

```java
import com.tutk.IOTC.AVAPIs;
import com.tutk.IOTC.IOTCAPIs;
import com.tutk.IOTC.TUTKGlobalAPIs;
```

**TUTK (Throughtek)** is a proprietary peer-to-peer SDK widely used in consumer IoT cameras. It establishes a direct encrypted channel between client and device using TUTK's cloud relay/STUN infrastructure, without requiring the app to expose any standard streaming port.

Key source files that confirmed this:

- `com/owlet/tutk/AndroidTutkSdk.java` — wrapper implementing `TutkSdk` interface, calling native TUTK JNI methods
- `com/owlet/tutk/TutkSdk.java` — Kotlin interface defining all TUTK operations
- `c1/x.java` — SDK initializer: calls `TUTK_SDK_Set_License_Key`, `IOTC_Initialize2`, `avInitialize`
- `c1/a.java` — AV client authenticator: sets DTLS security mode
- `d1/f.java` — frame receiver: calls `avRecvFrameData2` in a coroutine loop

---

## 5. Authentication Flow

### 5.1 Firebase Authentication

The app uses Firebase email/password authentication. The Firebase project credentials were found in `owlet_base_dec/res/values/strings.xml`:

```xml
<string name="google_api_key">AIzaSyDlfp3urNTbyhCtHOCxZBOjHQf4WuN_Aws</string>
<string name="project_id">owletcare-prod</string>
<string name="firebase_database_url">https://owletcare-prod.firebaseio.com</string>
```

Firebase's REST identity toolkit accepts email/password and returns a short-lived ID token (JWT):

```
POST https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key={API_KEY}
Body: {"email": "...", "password": "...", "returnSecureToken": true}
→ {"idToken": "eyJ..."}
```

### 5.2 TUTK Credential Retrieval

The Firebase token is then exchanged for TUTK-specific camera credentials via Owlet's own camera key management service. This was found in `owlet_src/sources/q0/p.java` (the `CameraNetworker` class):

```java
// retrievePassword method:
"https://camera-kms" + environment.getEndpoint() + ".owletdata.com/kms/" + dsn
// HTTP GET, header: Authorization: {firebaseToken}
```

The production endpoint (`getEndpoint()` returns `""` for production):

```
GET https://camera-kms.owletdata.com/kms/{camera_dsn}
Authorization: {firebase_id_token}
→ {"tutkid": "...", "password": "...", "authKey": "..."}
```

The response model was found in `com/owletcare/cam/core/PasswordResponse.java`:

```java
@Json(name = "password") String password;
@Json(name = "tutkid")   String tutkid;
@Json(name = "authKey")  String authKey;
```

---

## 6. TUTK Connection Protocol

### 6.1 SDK License Key

The TUTK SDK requires a license key before any other call. This key was hardcoded in `OwletCareApp.java` inside a Kodein dependency injection constructor:

```java
this.camManager = new com.owletcare.cam.core.b(
    ...,
    "AQAAAGHr2tF3sL8TGR+XirMqZSd8hKY3eBRqKIceLcUSy2okTWYU27qQmwzBORp3" +
    "tw1yoqiX7l+yoikFTI+Dzh9M+utHJ/3UBjL8FkYk4kuTSdcE6FtpD3Gidjxnmu2z" +
    "9TONdpEx15uXvTATqSexOCGDcldb3xtVXRmH0GoVx9SPKwVPaj7/iYJnPaaURxPzEb" +
    "Er2Yfd0ckSZoZ8jRH5jxmcJdob",
    ...
);
```

### 6.2 IOTC Session Establishment

From `c1/m.java` (the session connector):

```java
St_IOTCConnectInput in = new St_IOTCConnectInput();
in.authenticationType = 0;      // 0 = Password
in.authKey            = authKey; // "password" field from KMS response
in.timeout            = 20;      // seconds
IOTCAPIs.IOTC_Connect_ByUIDEx(tutkid, sessionId, in);
```

### 6.3 AV Channel with DTLS

From `c1/a.java` (the AV authenticator):

```java
St_AVClientStartInConfig cfg = new St_AVClientStartInConfig();
cfg.iotc_session_id    = sessionId;
cfg.iotc_channel_id    = 0;
cfg.account_or_identity = username;    // tutkid
cfg.password_or_token   = password;    // "password" from KMS
cfg.timeout_sec         = 20;
cfg.resend              = 1;
cfg.security_mode       = 2;           // DTLS auto-negotiate
cfg.dtls_cipher_suites  = "DEFAULT:@SECLEVEL=0";
AVAPIs.avClientStartEx(cfg, outCfg);
```

Security mode 2 means DTLS is used for encryption, with a deliberately relaxed cipher suite (`SECLEVEL=0`) for compatibility with older firmware.

### 6.4 Frame Reception

From `d1/f.java` (the video frame provider):

```java
// Video buffer: 768,000 bytes (768 KiB)
byte[] videoBuffer  = new byte[768000];
byte[] frameInfo    = new byte[28];

AVAPIs.avRecvFrameData2(
    channelId,
    videoBuffer, 768000,
    outBufSize, outFrameSize,
    frameInfo, 28,
    outFrmInfoBufSize, frameNumber
);
```

The call is pull-based and non-blocking: it returns `AV_ER_DATA_NOREADY` when no frame is available, so the app spins with a small sleep.

---

## 7. Frame Format

Each call to `avRecvFrameData2` delivers one frame. The frame data is raw H.264 Annex-B NALUs. Prepended is a 28-byte metadata header, decoded in `c1/c.java`:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 2 | `codec_id` | Video codec identifier |
| 2 | 1 | `flags` | bit0=I-frame, bit1=motion, bit2=alarm, **bit3=audio** |
| 3 | 1 | `cam_index` | Camera index |
| 5 | 1 | `resolution_flag` | 1=Low, 2=Std, 3=High, 4=Quad |
| 6 | 1 | `dasa_enabled` | DASA feature flag |
| 8 | 4 | `timestamp_ms` | Millisecond timestamp |
| 12 | 4 | `timestamp_s` | Unix second timestamp |
| 16 | 4 | `temperature` | Room temperature reading |
| 20 | 4 | `camera_utc_time` | Camera's UTC clock |
| 24 | 4 | `audio_db` | Audio level in dB |

When `flags & 0x08` is set, the frame is audio (AAC-LC, 8000 Hz, mono) rather than video.

**Video codec:** `video/avc` (H.264), decoded in the app via Android `MediaCodec`.  
**Audio codec:** `audio/mp4a-latm` (AAC), 8000 Hz, mono, decoded via `MediaCodec`.

---

## 8. Implementation

### 8.1 Authentication Script (`owlet_auth.py`)

A Python script that performs the full authentication chain:

1. POST to Firebase Identity Toolkit with email/password → gets `idToken`
2. GET `https://camera-kms.owletdata.com/kms/{dsn}` with the Firebase token → gets `{tutkid, password, authKey}`
3. Outputs a JSON object with all fields needed by the streaming client

```bash
python3 owlet_auth.py user@example.com password AC000Wxxxxxxxxxx
```

### 8.2 Streaming Client (`owlet_stream.c`)

A C program that:

1. Loads the x86_64 TUTK `.so` libraries via `dlopen` (no linking required)
2. Resolves all needed symbols via `dlsym`
3. Executes the full TUTK connection sequence
4. Loops on `avRecvFrameData2`, discards audio frames (bit3 of flags), writes H.264 NALUs to `stdout`

The binary is compiled with:

```bash
gcc -O2 -o owlet_stream owlet_stream.c -ldl -lpthread
```

### 8.3 End-to-End Launcher (`stream.sh`)

A shell script that chains the two components and pipes the H.264 bytestream into `ffmpeg`:

```bash
# Stream to RTP (then view with ffplay)
./stream.sh user@example.com password AC000Wxxxxxxxxxx

# Save to file
./stream.sh user@example.com password AC000Wxxxxxxxxxx /tmp/owlet.mp4
```

The ffmpeg invocation:

```bash
./owlet_stream "$TUTKID" "$CAMPASS" "$AUTHKEY" | \
    ffmpeg -f h264 -i pipe:0 -c copy -f rtp rtp://127.0.0.1:5004
```

---

## 9. Summary of Tools Used

| Tool | Version | Purpose |
|---|---|---|
| `unzip` | system | Extract `.apkm` bundle and split APKs |
| `apktool` | 2.x | Decode binary XML resources from `base.apk` |
| `jadx` | 1.5.0 | Decompile DEX bytecode to Java source |
| `nm` | binutils | Inspect exported symbols in TUTK `.so` files |
| `file` | system | Verify ELF architecture of extracted libraries |
| `strings` | binutils | Search raw DEX bytecode for embedded strings |
| `gcc` | system | Compile the C streaming client |
| `python3` + `requests` | 3.x | Firebase auth and HTTP API client |
| `ffmpeg` | any | Consume H.264 bytestream and mux/stream |

---

## 10. Key Findings

1. **No standard streaming protocol.** The Owlet Dream uses TUTK Throughtek P2P exclusively for live video. There is no RTSP, RTP, WebRTC, or HLS endpoint exposed by the camera.

2. **Credentials are per-camera, per-session.** The Firebase token is short-lived (~1 hour). The KMS `password` and `authKey` are obtained fresh for each streaming session.

3. **DTLS is mandatory.** The AV channel `security_mode=2` means the connection will refuse to work without DTLS. The relaxed cipher suite (`SECLEVEL=0`) is needed for firmware compatibility.

4. **H.264 Annex-B frames are delivered raw.** No container wrapping. ffmpeg can consume them directly from a pipe with `-f h264`.

5. **AWS Kinesis Video Streams is also present** in the app but is only used for event-triggered cloud recording clips, not for live viewing.

6. **The TUTK SDK license key is hardcoded** in the app's dependency injection constructor, making it straightforward to extract.
