/*
 * owlet_stream.c  –  Connect to Owlet Dream camera via TUTK P2P, pull raw
 * H.264 Annex-B frames, write them to stdout for piping into ffmpeg.
 *
 * Build:
 *   gcc -o owlet_stream owlet_stream.c -ldl -lpthread
 *
 * Run:
 *   TUTK_LIB_DIR=/path/to/tutk_libs \
 *   ./owlet_stream <tutkid> <password> [authKey] | \
 *   ffmpeg -f h264 -i pipe:0 -c copy output.mp4
 *
 * Struct layouts reverse-engineered from libIOTCAPIs.so / libAVAPIs.so
 * JNI wrappers (Java_com_tutk_IOTC_*) and the native IOTC_Connect_ByUIDEx
 * / avClientStartEx entry-point disassembly.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>

/* ── TUTK SDK License key (extracted from owlet APK DI module) ─────────── */
#define TUTK_LICENSE_KEY \
    "AQAAAGHr2tF3sL8TGR+XirMqZSd8hKY3eBRqKIceLcUSy2okTWYU27qQmwzBORp3" \
    "tw1yoqiX7l+yoikFTI+Dzh9M+utHJ/3UBjL8FkYk4kuTSdcE6FtpD3Gidjxnmu2z" \
    "9TONdpEx15uXvTATqSexOCGDcldb3xtVXRmH0GoVx9SPKwVPaj7/iYJnPaaURxPzEb" \
    "Er2Yfd0ckSZoZ8jRH5jxmcJdob"

#define TUTK_LAN_SEARCH_PORT 63616
#define TUTK_SESSION_TIMEOUT 20     /* seconds */
#define AV_CHANNEL_ID        0
#define VIDEO_BUF_SIZE       (768 * 1024)  /* 768 KiB – same as app */
#define FRAME_INFO_SIZE      28

/* IOCTRL command IDs (from g1/b0.java IOTYPE_* enum) */
#define IOTYPE_USER_IPCAM_START      0x01FF  /* 511 – start live video */
#define IOTYPE_USER_IPCAM_STOP       0x02FF  /* 767 – stop live video */
#define IOTYPE_USER_IPCAM_AUDIOSTART 0x0300  /* 768 – start audio */
#define IOTYPE_USER_IPCAM_LIVE_VIDEO_START_RESP 393396 /* camera ack for START */

/* ── TUTK SDK structures (reverse-engineered from JNI + disasm) ─────────── */

/*
 * St_IOTCConnectInput  —  160 bytes (0xa0), passed to IOTC_Connect_ByUIDEx.
 *
 * Disassembly checks:
 *   [0x00] cmpl $0xa0,(%rdx)          → cbSize must equal 0xa0
 *   [0x04] cmpl $0x0,0x4(%rdx)        → must be zero
 *   [0x08] call UID_validator(rdx+8)   → authKey inline string validated (8 alnum chars)
 *   [0x98] mov 0x98(%rdx),%ecx; cmp 2 → native auth_type ≤ 2  (0=pass, 2=Nebula)
 *
 * JNI field mapping (Java St_IOTCConnectInput → C offsets):
 *   authenticationType (int)  → [4]   (must be 0 for password mode)
 *   authKey (String, 8 chars) → [8]   inline char[8]
 *   deviceRegion (String)     → [16]  inline char[129]
 *   timeout (int)             → [148] 0x94
 *   lanModeDisable (byte→i32) → [152] 0x98  (native code sees this as auth_type)
 *   p2pModeDisable (byte)     → [156] 0x9c
 *   dataTransmitMode (byte)   → [157] 0x9d
 */
typedef struct __attribute__((packed)) {
    uint32_t cbSize;              /* [0]   0xa0 = 160 */
    uint32_t must_be_zero;        /* [4]   0, also authenticationType=0 */
    char     authKey[8];          /* [8]   8-char inline key from KMS authKey */
    char     deviceRegion[129];   /* [16]  region string or empty */
    uint8_t  pad[3];              /* [145] align to 148 */
    uint32_t timeout;             /* [148] 0x94 – connection timeout (seconds) */
    uint32_t auth_type_native;    /* [152] 0x98 – 0=password, 2=Nebula */
    uint8_t  p2pModeDisable;      /* [156] 0x9c */
    uint8_t  dataTransmitMode;    /* [157] 0x9d */
    uint8_t  reserved[2];         /* [158] padding to 160 */
} St_IOTCConnectInput;            /* total: 160 bytes */

_Static_assert(sizeof(St_IOTCConnectInput) == 160, "St_IOTCConnectInput must be 160 bytes");

/*
 * St_AVClientStartInConfig  —  64 bytes (0x40), passed to avClientStartEx.
 *
 * Disassembly checks:
 *   [0x00] cmpl $0x40,(%rdi)    → cbSize ≥ 0x40
 *   [0x08] movzbl 0x8(%rdi)     → iotc_channel_id (byte, must be ≤ 31)
 *
 * JNI field mapping (Java St_AVClientStartInConfig → C offsets):
 *   iotc_session_id (int)          → [4]
 *   iotc_channel_id (byte/int)     → [8]   stored as byte
 *   timeout_sec (int)              → [12]
 *   account_or_identity (String*)  → [16]  pointer
 *   password_or_token (String*)    → [24]  pointer
 *   resend (int)                   → [32]
 *   security_mode (int)            → [36]
 *   auth_type (int)                → [40]
 *   sync_recv_data (int)           → [44]
 *   dtls_cipher_suites (String*)   → [48]  pointer
 *   (reserved zeros)               → [56..63]
 */
typedef struct {
    uint32_t cbSize;               /* [0]  0x40 = 64 */
    int32_t  iotc_session_id;      /* [4]  */
    uint8_t  iotc_channel_id;      /* [8]  byte, 0-31 */
    uint8_t  pad1[3];              /* [9]  */
    int32_t  timeout_sec;          /* [12] */
    char    *account_or_identity;  /* [16] pointer */
    char    *password_or_token;    /* [24] pointer */
    int32_t  resend;               /* [32] */
    int32_t  security_mode;        /* [36] 2=DTLS */
    int32_t  auth_type;            /* [40] 0=password */
    int32_t  sync_recv_data;       /* [44] */
    char    *dtls_cipher_suites;   /* [48] pointer */
    uint32_t reserved1;            /* [56] */
    uint32_t reserved2;            /* [60] */
} St_AVClientStartInConfig;        /* total: 64 bytes */

_Static_assert(sizeof(St_AVClientStartInConfig) == 64, "St_AVClientStartInConfig must be 64 bytes");

/*
 * St_AVClientStartOutConfig  —  32 bytes (0x20), filled by avClientStartEx.
 *
 * Disassembly: cmpl $0x20,(%rsi) — cbSize must be ≥ 0x20.
 * We only need to pre-fill cbSize; the rest is output.
 */
typedef struct {
    uint32_t cbSize;       /* [0]  0x20 = 32 */
    uint8_t  data[28];     /* [4]  output from SDK */
} St_AVClientStartOutConfig; /* total: 32 bytes */

_Static_assert(sizeof(St_AVClientStartOutConfig) == 32, "St_AVClientStartOutConfig must be 32 bytes");

/* 28-byte per-frame header (decoded in c1/c.java) */
typedef struct __attribute__((packed)) {
    uint16_t codec_id;        /* 0 */
    uint8_t  flags;           /* 2: bit0=IFrame, bit1=motion, bit2=alarm, bit3=audio */
    uint8_t  cam_index;       /* 3 */
    uint8_t  unknown;         /* 4 */
    uint8_t  resolution_flag; /* 5: 1=Low,2=Std,3=High,4=Quad */
    uint8_t  dasa_enabled;    /* 6 */
    uint8_t  pad;             /* 7 */
    int32_t  timestamp_ms;    /* 8 */
    int32_t  timestamp_s;     /* 12 */
    int32_t  temperature;     /* 16 */
    int32_t  camera_utc_time; /* 20 */
    int32_t  audio_db;        /* 24 */
} FrameInfo;

/* ── Function pointer types ─────────────────────────────────────────────── */
typedef int  (*fn_TUTK_SDK_Set_License_Key)(const char *key);
typedef int  (*fn_TUTK_SDK_Set_Region)(int region);
typedef int  (*fn_IOTC_Set_LanSearchPort)(uint16_t port);
typedef void (*fn_IOTC_Setup_Session_Alive_Timeout)(uint32_t timeout);
typedef int  (*fn_IOTC_Initialize2)(uint32_t udpPort);
typedef void (*fn_IOTC_DeInitialize)(void);
typedef int  (*fn_IOTC_Get_SessionID)(void);
typedef int  (*fn_IOTC_Connect_ByUIDEx)(const char *uid, int sessionId,
                                         St_IOTCConnectInput *in);
typedef void (*fn_IOTC_Session_Close)(int sessionId);
typedef int  (*fn_IOTC_Session_Get_Free_Channel)(int sessionId);
typedef int  (*fn_avInitialize)(int maxChannels);
typedef void (*fn_avDeInitialize)(void);
typedef int  (*fn_avClientStartEx)(St_AVClientStartInConfig *in,
                                    St_AVClientStartOutConfig *out);
typedef int  (*fn_avSendIOCtrl)(int channelId, int type,
                                const char *data, int dataSize);
typedef int  (*fn_avRecvIOCtrl)(int channelId, int *type,
                                char *data, int dataSize, int timeoutMs);
typedef void (*fn_avClientStop)(int channelId);
typedef void (*fn_avClientExit)(int sessionId, int channelId);
typedef int  (*fn_avRecvFrameData2)(int channelId, char *videoBuf,
                                     int videoBufSize, int *actualFrameSize,
                                     int *expectedFrameSize, char *frameInfoBuf,
                                     int frameInfoBufSize,
                                     int *actualFrameInfoSize,
                                     int *frameIndex);

/* ── Globals ────────────────────────────────────────────────────────────── */
static volatile int g_stop = 0;
static int g_session_id  = -1;
static int g_channel_id  = -1;

static fn_IOTC_Session_Close  g_IOTC_Session_Close  = NULL;
static fn_avClientStop        g_avClientStop        = NULL;
static fn_avClientExit        g_avClientExit        = NULL;
static fn_IOTC_DeInitialize   g_IOTC_DeInitialize   = NULL;
static fn_avDeInitialize      g_avDeInitialize      = NULL;

static void handle_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void cleanup(void)
{
    if (g_channel_id >= 0) {
        if (g_avClientStop)   g_avClientStop(g_channel_id);
        if (g_avClientExit && g_session_id >= 0)
            g_avClientExit(g_session_id, g_channel_id);
    }
    if (g_session_id >= 0 && g_IOTC_Session_Close)
        g_IOTC_Session_Close(g_session_id);
    if (g_avDeInitialize)   g_avDeInitialize();
    if (g_IOTC_DeInitialize) g_IOTC_DeInitialize();
}

#define LOAD_SYM(handle, name, type, var)                          \
    do {                                                           \
        (var) = (type)dlsym((handle), #name);                      \
        if (!(var)) {                                              \
            fprintf(stderr, "dlsym %s: %s\n", #name, dlerror()); \
            return 1;                                              \
        }                                                          \
    } while (0)

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
            "Usage: %s <tutkid> <password> [authKey]\n"
            "  tutkid   : camera UID from KMS (tutkid field)\n"
            "  password : camera password from KMS (password field)\n"
            "  authKey  : 8-char IOTC auth key from KMS (authKey field)\n"
            "\nPipe stdout to ffmpeg:\n"
            "  %s <tutkid> <pass> <authKey> | ffmpeg -f h264 -i pipe:0 -c copy out.mp4\n",
            argv[0], argv[0]);
        return 1;
    }

    const char *tutkid   = argv[1];
    const char *password = argv[2];
    const char *authkey  = argc > 3 ? argv[3] : "";

    fprintf(stderr, "[TUTK] tutkid=%s  authKey='%.8s'\n", tutkid, authkey);

    /* ── locate TUTK lib dir ──────────────────────────────────────────── */
    const char *lib_dir = getenv("TUTK_LIB_DIR");
    if (!lib_dir) lib_dir = ".";

    char path[512];
    void *h_global, *h_iotc, *h_av;

#define LIB(dir, name) (snprintf(path, sizeof(path), "%s/%s", (dir), (name)), path)

    /* Load order matters: global deps first */
    h_global = dlopen(LIB(lib_dir, "libTUTKGlobalAPIs.so"), RTLD_NOW | RTLD_GLOBAL);
    if (!h_global) { fprintf(stderr, "dlopen TUTKGlobalAPIs: %s\n", dlerror()); return 1; }

    h_iotc = dlopen(LIB(lib_dir, "libIOTCAPIs.so"), RTLD_NOW | RTLD_GLOBAL);
    if (!h_iotc)   { fprintf(stderr, "dlopen IOTCAPIs: %s\n",     dlerror()); return 1; }

    h_av = dlopen(LIB(lib_dir, "libAVAPIs.so"), RTLD_NOW | RTLD_GLOBAL);
    if (!h_av)     { fprintf(stderr, "dlopen AVAPIs: %s\n",       dlerror()); return 1; }

    /* ── resolve symbols ─────────────────────────────────────────────── */
    fn_TUTK_SDK_Set_License_Key       TUTK_SDK_Set_License_Key;
    fn_TUTK_SDK_Set_Region            TUTK_SDK_Set_Region;
    fn_IOTC_Set_LanSearchPort         IOTC_Set_LanSearchPort;
    fn_IOTC_Setup_Session_Alive_Timeout IOTC_Setup_Session_Alive_Timeout;
    fn_IOTC_Initialize2               IOTC_Initialize2;
    fn_IOTC_Get_SessionID             IOTC_Get_SessionID;
    fn_IOTC_Connect_ByUIDEx           IOTC_Connect_ByUIDEx;
    fn_IOTC_Session_Get_Free_Channel  IOTC_Session_Get_Free_Channel;
    fn_avInitialize                   avInitialize;
    fn_avClientStartEx                avClientStartEx;
    fn_avSendIOCtrl                   avSendIOCtrl;
    fn_avRecvIOCtrl                   avRecvIOCtrl;
    fn_avRecvFrameData2               avRecvFrameData2;

    LOAD_SYM(h_global, TUTK_SDK_Set_License_Key,        fn_TUTK_SDK_Set_License_Key,       TUTK_SDK_Set_License_Key);
    LOAD_SYM(h_global, TUTK_SDK_Set_Region,             fn_TUTK_SDK_Set_Region,            TUTK_SDK_Set_Region);
    LOAD_SYM(h_iotc,   IOTC_Set_LanSearchPort,          fn_IOTC_Set_LanSearchPort,         IOTC_Set_LanSearchPort);
    LOAD_SYM(h_iotc,   IOTC_Setup_Session_Alive_Timeout, fn_IOTC_Setup_Session_Alive_Timeout, IOTC_Setup_Session_Alive_Timeout);
    LOAD_SYM(h_iotc,   IOTC_Initialize2,                fn_IOTC_Initialize2,               IOTC_Initialize2);
    LOAD_SYM(h_iotc,   IOTC_Get_SessionID,              fn_IOTC_Get_SessionID,             IOTC_Get_SessionID);
    LOAD_SYM(h_iotc,   IOTC_Connect_ByUIDEx,            fn_IOTC_Connect_ByUIDEx,           IOTC_Connect_ByUIDEx);
    LOAD_SYM(h_iotc,   IOTC_Session_Get_Free_Channel,   fn_IOTC_Session_Get_Free_Channel,  IOTC_Session_Get_Free_Channel);
    LOAD_SYM(h_av,     avInitialize,                    fn_avInitialize,                   avInitialize);
    LOAD_SYM(h_av,     avClientStartEx,                 fn_avClientStartEx,                avClientStartEx);
    LOAD_SYM(h_av,     avSendIOCtrl,                    fn_avSendIOCtrl,                   avSendIOCtrl);
    LOAD_SYM(h_av,     avRecvIOCtrl,                    fn_avRecvIOCtrl,                   avRecvIOCtrl);
    LOAD_SYM(h_av,     avRecvFrameData2,                fn_avRecvFrameData2,               avRecvFrameData2);

    g_IOTC_Session_Close = (fn_IOTC_Session_Close) dlsym(h_iotc, "IOTC_Session_Close");
    g_avClientStop       = (fn_avClientStop)        dlsym(h_av,   "avClientStop");
    g_avClientExit       = (fn_avClientExit)        dlsym(h_av,   "avClientExit");
    g_IOTC_DeInitialize  = (fn_IOTC_DeInitialize)   dlsym(h_iotc, "IOTC_DeInitialize");
    g_avDeInitialize     = (fn_avDeInitialize)      dlsym(h_av,   "avDeInitialize");

    signal(SIGINT,  handle_sigint);
    signal(SIGTERM, handle_sigint);

    /* ── 1. Set license key (must be first) ──────────────────────────── */
    fprintf(stderr, "[TUTK] Setting license key...\n");
    int rc = TUTK_SDK_Set_License_Key(TUTK_LICENSE_KEY);
    if (rc < 0) {
        fprintf(stderr, "[TUTK] TUTK_SDK_Set_License_Key failed: %d\n", rc);
        /* Non-fatal – some builds only warn */
    }

    /* ── 2. Set region (0 = IOTC_Region_ALL / auto) ─────────────────── */
    TUTK_SDK_Set_Region(0);

    /* ── 3. Configure LAN search port (63616) ────────────────────────── */
    IOTC_Set_LanSearchPort(TUTK_LAN_SEARCH_PORT);

    /* ── 4. Session alive timeout (20 s, same as app) ───────────────── */
    IOTC_Setup_Session_Alive_Timeout(TUTK_SESSION_TIMEOUT);

    /* ── 5. Initialize IOTC (0 = OS-chosen UDP port) ────────────────── */
    fprintf(stderr, "[TUTK] IOTC_Initialize2...\n");
    rc = IOTC_Initialize2(0);
    if (rc < 0 && rc != -22 /* IOTC_ER_ALREADY_INITIALIZED */) {
        fprintf(stderr, "[TUTK] IOTC_Initialize2 failed: %d\n", rc);
        return 1;
    }

    /* ── 6. Initialize AV layer (max 512 channels, same as app) ─────── */
    fprintf(stderr, "[TUTK] avInitialize...\n");
    rc = avInitialize(512);
    if (rc < 0) {
        fprintf(stderr, "[TUTK] avInitialize failed: %d\n", rc);
        cleanup(); return 1;
    }

    /* ── 7. Allocate a session ID ────────────────────────────────────── */
    g_session_id = IOTC_Get_SessionID();
    if (g_session_id < 0) {
        fprintf(stderr, "[TUTK] IOTC_Get_SessionID failed: %d\n", g_session_id);
        cleanup(); return 1;
    }
    fprintf(stderr, "[TUTK] Session ID: %d\n", g_session_id);

    /* ── 8. Connect to camera via P2P ───────────────────────────────── */
    fprintf(stderr, "[TUTK] Connecting to camera %s (authKey='%.8s')...\n",
            tutkid, authkey);

    St_IOTCConnectInput conn_in;
    memset(&conn_in, 0, sizeof(conn_in));
    conn_in.cbSize       = 0xa0;                        /* MUST be 160 */
    conn_in.must_be_zero = 0;                           /* MUST be 0 */
    /* authKey: inline 8-char string at offset 8; copy exactly 8 bytes */
    strncpy(conn_in.authKey, authkey, 8);               /* null-padded */
    /* deviceRegion: leave empty (null-terminate at [0] = 0 from memset) */
    conn_in.timeout          = TUTK_SESSION_TIMEOUT;    /* offset 148 */
    conn_in.auth_type_native = 0;                       /* offset 152: 0=password */

    rc = IOTC_Connect_ByUIDEx(tutkid, g_session_id, &conn_in);
    if (rc < 0) {
        fprintf(stderr, "[TUTK] IOTC_Connect_ByUIDEx failed: %d\n", rc);
        cleanup(); return 1;
    }
    fprintf(stderr, "[TUTK] IOTC connected (session=%d)\n", g_session_id);

    /* ── 9. Start AV client with DTLS ───────────────────────────────── */
    fprintf(stderr, "[TUTK] Starting AV client...\n");

    St_AVClientStartInConfig av_in;
    St_AVClientStartOutConfig av_out;
    memset(&av_in,  0, sizeof(av_in));
    memset(&av_out, 0, sizeof(av_out));

    av_in.cbSize              = 0x40;               /* MUST be >= 64 */
    av_in.iotc_session_id     = g_session_id;        /* [4]  */
    av_in.iotc_channel_id     = AV_CHANNEL_ID;       /* [8]  byte, 0-31 */
    av_in.timeout_sec         = TUTK_SESSION_TIMEOUT; /* [12] */
    av_in.account_or_identity = (char *)"admin";      /* [16] DTLS PSK identity — app defaults to "admin" (v0/c.java:208) */
    av_in.password_or_token   = (char *)password;     /* [24] */
    av_in.resend              = 1;                    /* [32] */
    av_in.security_mode       = 2;                    /* [36] DTLS auto */
    av_in.auth_type           = 0;                    /* [40] password */
    av_in.sync_recv_data      = 0;                    /* [44] */
    av_in.dtls_cipher_suites  = (char *)"DEFAULT:@SECLEVEL=0"; /* [48] */

    av_out.cbSize = 0x20;                             /* MUST be >= 32 */

    g_channel_id = avClientStartEx(&av_in, &av_out);
    if (g_channel_id < 0) {
        fprintf(stderr, "[TUTK] avClientStartEx failed: %d\n", g_channel_id);
        cleanup(); return 1;
    }
    fprintf(stderr, "[TUTK] AV channel opened (id=%d)\n", g_channel_id);

    /* ── 9b. Tell the camera to start sending live video ────────────────
     * The Owlet app sends IOTYPE_USER_IPCAM_START (0x1FF) with an 8-byte
     * zero payload right after avClientStartEx (g1/z.java:1869). Without
     * this the camera stays silent / sends only a trickle, so ffmpeg can't
     * determine the frame size. */
    {
        char start_payload[8] = {0};
        rc = avSendIOCtrl(g_channel_id, IOTYPE_USER_IPCAM_START,
                          start_payload, sizeof(start_payload));
        fprintf(stderr, "[TUTK] avSendIOCtrl(START) -> %d\n", rc);
        if (rc < 0) {
            fprintf(stderr, "[TUTK] WARNING: start-video command failed (%d); "
                            "camera may not stream video.\n", rc);
        }

        /* The app's X() helper waits for the camera's IOCtrl reply after each
         * command. Drain pending IOCtrl responses so the camera advances its
         * state machine and begins streaming video. */
        int   io_type = 0;
        char  io_buf[1024];
        for (int i = 0; i < 20 && !g_stop; i++) {
            int r = avRecvIOCtrl(g_channel_id, &io_type, io_buf,
                                 sizeof(io_buf), 500);
            if (r >= 0) {
                fprintf(stderr, "[TUTK] IOCtrl resp type=0x%x len=%d\n",
                        io_type, r);
                if (io_type == IOTYPE_USER_IPCAM_LIVE_VIDEO_START_RESP)
                    break;
            } else if (r == -20012) {
                continue;   /* no response yet */
            } else {
                fprintf(stderr, "[TUTK] avRecvIOCtrl -> %d\n", r);
                break;
            }
        }
    }

    fprintf(stderr, "[TUTK] Streaming H.264 to stdout. Press Ctrl-C to stop.\n");

    /* ── 10. Frame receive loop ──────────────────────────────────────── */
    char *video_buf = malloc(VIDEO_BUF_SIZE);
    char  frame_info_buf[FRAME_INFO_SIZE];
    if (!video_buf) { perror("malloc"); cleanup(); return 1; }

    uint64_t frame_count = 0;
    uint64_t byte_count  = 0;

    while (!g_stop) {
        int actual_frame_size    = 0;
        int expected_frame_size  = 0;
        int actual_frame_info_sz = 0;
        int frame_idx            = 0;

        int n = avRecvFrameData2(
            g_channel_id,
            video_buf, VIDEO_BUF_SIZE,
            &actual_frame_size,
            &expected_frame_size,
            frame_info_buf, FRAME_INFO_SIZE,
            &actual_frame_info_sz,
            &frame_idx
        );

        if (n == -20012) {
            /* AV_ER_DATA_NOREADY – no frame yet, keep polling */
            usleep(5000);
            continue;
        }
        if (n == -20013 || n == -20014) {
            /* AV_ER_INCOMPLETE_FRAME / AV_ER_LOSED_THIS_FRAME – skip, keep going */
            usleep(2000);
            continue;
        }
        if (n < 0) {
            fprintf(stderr, "[TUTK] avRecvFrameData2 error: %d (stopping)\n", n);
            break;
        }
        if (n == 0) {
            usleep(1000);
            continue;
        }

        /* avRecvFrameData2 is the video-only channel buffer (audio is delivered
         * separately via avRecvAudioData), so every frame here is H.264 video. */
        if (frame_count == 0 && actual_frame_info_sz >= 4) {
            uint16_t codec_id = (uint8_t)frame_info_buf[0] |
                                ((uint8_t)frame_info_buf[1] << 8);
            uint8_t  fflags   = (uint8_t)frame_info_buf[2];
            fprintf(stderr, "[TUTK] first frame: codec_id=%u flags=0x%02x size=%d\n",
                    codec_id, fflags, n);
        }

        /* Write raw H.264 Annex-B NALUs to stdout */
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(STDOUT_FILENO, video_buf + written, n - written);
            if (w < 0) { perror("write"); goto done; }
            written += w;
        }

        frame_count++;
        byte_count += n;

        if (frame_count % 100 == 0) {
            FrameInfo *fi = (FrameInfo *)frame_info_buf;
            fprintf(stderr,
                "[TUTK] frames=%llu  bytes=%llu  res_flag=%u  iframe=%u  "
                "ts=%d  temp=%d\n",
                (unsigned long long)frame_count,
                (unsigned long long)byte_count,
                (unsigned)fi->resolution_flag,
                (unsigned)(fi->flags & 1),
                fi->timestamp_ms,
                fi->temperature);
        }
    }

done:
    free(video_buf);
    cleanup();
    fprintf(stderr, "[TUTK] Done. frames=%llu  bytes=%llu\n",
            (unsigned long long)frame_count, (unsigned long long)byte_count);
    return 0;
}
