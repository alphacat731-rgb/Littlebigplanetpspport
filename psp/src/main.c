#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspge.h>
#include <pspaudio.h>
#include <psputility.h>
#include <psputility_avmodules.h>
#include <pspmpeg.h>
#include <psputils.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PSP_MODULE_INFO("LBPPSP Alpha", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(64);

#define W 480
#define H 272
#define FBW 512
#define PACKETS 32
#define MPEG_MODE 4
#define DDRTOP_SIZE 0x200000
#define DDRTOP_ALIGN 0x400000
#define DDRTOP_AU_OFFSET 0x10000
#define SECTOR 2048
#define PCM_SAMPLES 512
#define PCM_BYTES (PCM_SAMPLES * 2 * sizeof(short))
#define PMF_PATH "ms0:/PSP/GAME/LBPPSP_ALPHA/INTRO.PMF"
#define PCM_PATH "ms0:/PSP/GAME/LBPPSP_ALPHA/INTRO.PCM"
#define LOG_PATH "ms0:/PSP/GAME/LBPPSP_ALPHA/PLAYBACK.LOG"
#define ROOT_LOG_PATH "ms0:/PLAYBACK.LOG"
#define DRAW_BYTES (FBW * H * 4)
#define DISP_BYTES (FBW * H * 4)
#define VIDEO_TEXTURE_BYTES (FBW * 512 * 4)

typedef struct {
    SceUID file;
    unsigned int total;
    unsigned int read_bytes;
    int eof;
    int read_error;
} PmfInput;

typedef struct {
    SceUID file;
    SceUID thread;
    int channel;
    volatile int running;
    volatile int paused;
    volatile int done;
    volatile unsigned int samples_played;
    void *buffer;
} PcmOutput;

typedef struct {
    short u, v, x, y, z;
} VideoVertex;

/* Internal sceMpeg AVC detail structures used by the PSP's YCbCr-to-RGB CSC. */
typedef struct {
    SceInt32 unknown0, unknown1, width, height;
    SceInt32 unknown4, unknown5, unknown6, unknown7, unknown8, unknown9;
} LbpAvcInfo;

typedef struct {
    ScePVoid buffer0, buffer1, buffer2, buffer3;
    ScePVoid buffer4, buffer5, buffer6, buffer7;
    SceInt32 unknown0, unknown1, unknown2;
} LbpAvcYuv;

typedef struct {
    SceInt32 unknown0, unknown1, unknown2, unknown3;
    LbpAvcInfo *info_buffer;
    SceInt32 unknown5, unknown6, unknown7, unknown8, unknown9, unknown10;
    LbpAvcYuv *yuv_buffer;
    SceInt32 unknown12, unknown13, unknown14, unknown15, unknown16, unknown17;
    SceInt32 unknown18, unknown19, unknown20, unknown21, unknown22, unknown23;
} LbpAvcDetail2;

typedef struct {
    SceInt32 height, width, mode0, mode1;
    ScePVoid buffer0, buffer1, buffer2, buffer3;
    ScePVoid buffer4, buffer5, buffer6, buffer7;
} LbpAvcCsc;

/* Exported by sceMpeg / sceMpegbase, but not declared by the stock PSPSDK headers. */
extern SceInt32 sceMpegAvcDecodeDetail2(SceMpeg *mpeg, LbpAvcDetail2 **detail);
extern SceInt32 sceMpegBaseCscAvc(ScePVoid rgb_buffer, SceInt32 unknown,
                                  SceInt32 buffer_width, LbpAvcCsc *csc);

static unsigned int __attribute__((aligned(16))) gu_list[8192];
static const char *last_playback_stage = "not started";
static int last_playback_error = 0;

static int exit_callback(int a, int b, void *c)
{
    (void)a; (void)b; (void)c;
    sceKernelExitGame();
    return 0;
}

static int callback_thread(SceSize args, void *argp)
{
    SceUID cb;
    (void)args; (void)argp;
    cb = sceKernelCreateCallback("LBPPSP Exit", exit_callback, NULL);
    if (cb >= 0) sceKernelRegisterExitCallback(cb);
    sceKernelSleepThreadCB();
    return 0;
}

static void setup_callbacks(void)
{
    SceUID th = sceKernelCreateThread("LBPPSP Callback", callback_thread,
                                      0x11, 0xFA0, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);
}

static int playback_log_to(const char *path, const char *message)
{
    SceUID fd;
    size_t length = strlen(message);
    size_t written = 0;

    fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd < 0)
        return fd;

    while (written < length) {
        int n = sceIoWrite(fd, message + written, (unsigned int)(length - written));
        if (n <= 0) {
            sceIoClose(fd);
            return n < 0 ? n : -1;
        }
        written += (size_t)n;
    }

    if (sceIoWrite(fd, "\n", 1) != 1) {
        sceIoClose(fd);
        return -1;
    }

    return sceIoClose(fd);
}

static void playback_log(const char *message)
{
    /* Fall back if opening OR writing the game-folder log fails. */
    if (playback_log_to(LOG_PATH, message) < 0)
        (void)playback_log_to(ROOT_LOG_PATH, message);
}

static void set_playback_failure(const char *stage, int error)
{
    char message[160];
    last_playback_stage = stage;
    last_playback_error = error;
    snprintf(message, sizeof(message), "ERROR at %s: 0x%08X", stage, (unsigned int)error);
    playback_log(message);
}

static void draw_menu(int selected)
{
    int i;
    const char *items[] = { "Start Prototype", "Play Intro Video", "Credits", "Exit" };
    pspDebugScreenClear();
    pspDebugScreenSetXY(3, 2);
    pspDebugScreenSetTextColor(0xFFFFFFFF);
    pspDebugScreenPrintf("LITTLEBIGPLANET PSP: ALPHA\n");
    pspDebugScreenPrintf("----------------------------------------------\n\n");
    pspDebugScreenSetTextColor(0xFF9BE24A);
    pspDebugScreenPrintf("  A small world is loading...\n\n");
    for (i = 0; i < 4; ++i) {
        pspDebugScreenSetTextColor(i == selected ? 0xFF9BE24A : 0xFFE8E8E8);
        pspDebugScreenPrintf(i == selected ? "  > %s\n" : "    %s\n", items[i]);
    }
    pspDebugScreenSetTextColor(0xFFB0B0B0);
    pspDebugScreenPrintf("\n\n  D-Pad: Select    X: Confirm    O: Back\n");
    pspDebugScreenPrintf("  HOME: Exit to the PSP menu\n");
}

static void draw_page(const char *title, const char *body)
{
    pspDebugScreenClear();
    pspDebugScreenSetXY(3, 2);
    pspDebugScreenSetTextColor(0xFF9BE24A);
    pspDebugScreenPrintf("LITTLEBIGPLANET PSP: ALPHA\n");
    pspDebugScreenPrintf("----------------------------------------------\n\n");
    pspDebugScreenSetTextColor(0xFFFFFFFF);
    pspDebugScreenPrintf("%s\n\n", title);
    pspDebugScreenSetTextColor(0xFFE0E0E0);
    pspDebugScreenPrintf("%s\n", body);
    pspDebugScreenSetTextColor(0xFFB0B0B0);
    pspDebugScreenPrintf("\n\n  Press O to return to the menu.\n");
}

/* The Media Engine ringbuffer consumes complete 2048-byte MPEG sectors. */
static SceInt32 pmf_read_callback(ScePVoid data, SceInt32 packets, ScePVoid param)
{
    PmfInput *in = (PmfInput *)param;
    unsigned int wanted = (unsigned int)packets * SECTOR;
    unsigned int got = 0;
    if (in->read_bytes >= in->total) {
        in->eof = 1;
        return 0;
    }
    if (wanted > in->total - in->read_bytes)
        wanted = in->total - in->read_bytes;
    wanted -= wanted % SECTOR;
    while (got < wanted) {
        int n = sceIoRead(in->file, (unsigned char *)data + got, wanted - got);
        if (n <= 0) {
            in->eof = 1;
            in->read_error = n < 0 ? n : -10;
            break;
        }
        got += (unsigned int)n;
    }
    in->read_bytes += got;
    if (got != wanted || (got % SECTOR) != 0) {
        in->eof = 1;
        if (in->read_error == 0)
            in->read_error = -11;
        return -1;
    }
    return (SceInt32)(got / SECTOR);
}

static int pcm_thread(SceSize args, void *argp)
{
    PcmOutput *out = (PcmOutput *)argp;
    (void)args;
    while (out->running) {
        int n;
        if (out->paused) {
            sceKernelDelayThread(1000);
            continue;
        }
        n = sceIoRead(out->file, out->buffer, PCM_BYTES);
        if (n <= 0) {
            out->done = 1;
            break;
        }
        if ((unsigned int)n < PCM_BYTES)
            memset((unsigned char *)out->buffer + n, 0, PCM_BYTES - n);
        sceAudioOutputBlocking(out->channel, PSP_AUDIO_VOLUME_MAX, out->buffer);
        out->samples_played += (unsigned int)(n / 4);
        if ((unsigned int)n < PCM_BYTES) {
            out->done = 1;
            break;
        }
    }
    return 0;
}

static int pcm_start(PcmOutput *out)
{
    memset(out, 0, sizeof(*out));
    out->file = sceIoOpen(PCM_PATH, PSP_O_RDONLY, 0);
    out->thread = -1;
    out->channel = -1;
    if (out->file < 0) return -1;
    out->channel = sceAudioChReserve(-1, PCM_SAMPLES, PSP_AUDIO_FORMAT_STEREO);
    if (out->channel < 0) return -2;
    out->buffer = memalign(64, PCM_BYTES);
    if (!out->buffer) return -3;
    out->running = 1;
    out->thread = sceKernelCreateThread("LBPPSP PCM", pcm_thread, 0x18,
                                        0x4000, PSP_THREAD_ATTR_USER, NULL);
    if (out->thread < 0) return -4;
    if (sceKernelStartThread(out->thread, sizeof(*out), out) < 0) return -5;
    return 0;
}

static void pcm_stop(PcmOutput *out)
{
    out->running = 0;
    if (out->thread >= 0) {
        sceKernelTerminateDeleteThread(out->thread);
        out->thread = -1;
    }
    if (out->channel >= 0) {
        sceAudioChRelease(out->channel);
        out->channel = -1;
    }
    if (out->file >= 0) {
        sceIoClose(out->file);
        out->file = -1;
    }
    if (out->buffer) {
        free(out->buffer);
        out->buffer = NULL;
    }
}

static void gu_video_init(void)
{
    sceGuInit();
    sceGuStart(GU_DIRECT, gu_list);
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, FBW);
    sceGuDispBuffer(W, H, (void *)DISP_BYTES, FBW);
    sceGuOffset(2048 - W / 2, 2048 - H / 2);
    sceGuViewport(2048, 2048, W, H);
    sceGuScissor(0, 0, W, H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_BLEND);
    sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuClearColor(0xFF000000);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
    sceGuDisplay(GU_TRUE);
}


static void gu_video_frame(void *frame)
{
    VideoVertex *v;
    sceGuStart(GU_DIRECT, gu_list);
    sceGuClearColor(0xFF000000);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    sceGuTexImage(0, 512, 512, FBW, frame);
    v = (VideoVertex *)sceGuGetMemory(2 * sizeof(VideoVertex));
    v[0].u = 0; v[0].v = 0; v[0].x = 0; v[0].y = 0; v[0].z = 0;
    v[1].u = W; v[1].v = H; v[1].x = W; v[1].y = H; v[1].z = 0;
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT |
                   GU_TRANSFORM_2D, 2, NULL, v);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
}

static void show_startup_splash(void)
{
    SceUID file;
    void *image;
    unsigned int loaded = 0;
    const unsigned int image_size = 512 * 512 * 4;

    file = sceIoOpen("ms0:/PSP/GAME/LBPPSP_ALPHA/LOADING.RGBA", PSP_O_RDONLY, 0);
    if (file < 0)
        goto black_fallback;

    image = memalign(64, image_size);
    if (!image) {
        sceIoClose(file);
        goto black_fallback;
    }

    while (loaded < image_size) {
        int n = sceIoRead(file, (unsigned char *)image + loaded, image_size - loaded);
        if (n <= 0)
            break;
        loaded += (unsigned int)n;
    }
    sceIoClose(file);

    if (loaded != image_size) {
        free(image);
        goto black_fallback;
    }

    gu_video_init();
    gu_video_frame(image);
    sceKernelDelayThread(3 * 1000 * 1000);

    /* Hold a clean black frame for two seconds before the intro starts. */
    sceGuStart(GU_DIRECT, gu_list);
    sceGuClearColor(0xFF000000);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
    sceKernelDelayThread(2 * 1000 * 1000);

    sceGuTerm();
    pspDebugScreenInit();
    free(image);
    return;

black_fallback:
    /* Missing splash asset should not prevent the actual intro from playing. */
    pspDebugScreenInit();
    sceKernelDelayThread(2 * 1000 * 1000);
}

static int wait_audio(PcmOutput *audio, unsigned int pts)
{
    unsigned int target;
    if (pts == 0xFFFFFFFFu || pts <= 90000u) return 0;
    target = (unsigned int)(((unsigned long long)(pts - 90000u) * 44100u) / 90000u);
    while (audio->running && !audio->done && audio->samples_played < target) {
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(&pad, 1);
        if (pad.Buttons & PSP_CTRL_CIRCLE) return -1;
        sceKernelDelayThread(1000);
    }
    return 0;
}

static int play_intro(void)
{
    int result = -1, avcodec = 0, mpegbase = 0, mpeginit = 0;
    int rb_ready = 0, mpeg_ready = 0, stream_ready = 0, gu_ready = 0;
    SceUID file = -1;
    unsigned char header[0x800];
    SceMpeg mpeg = NULL;
    SceMpegRingbuffer ring;
    SceMpegStream *stream = NULL;
    SceMpegAu *au = NULL;
    SceMpegAvcMode mode;
    PmfInput input;
    PcmOutput audio;
    void *rb_mem = NULL, *mpeg_mem = NULL, *ddrtop = NULL;
    void *au_es = NULL, *au_mem = NULL, *frame = NULL, *frame_uncached = NULL;
    LbpAvcDetail2 *detail2 = NULL;
    LbpAvcCsc csc;
    int rb_size, mpeg_size;
    SceInt32 offset = 0, stream_size = 0, unknown = 0, pic_num = 0;
    unsigned int previous = 0;
    int audio_started = 0, frames = 0;
    int first_au_logged = 0, first_decode_logged = 0, first_csc_logged = 0;
    unsigned long long first_frame_wait_start = 0;
    char logbuf[128];

    last_playback_stage = "Opening INTRO.PMF";
    last_playback_error = 0;
    playback_log("=== play_intro begin ===");
    memset(&input, 0, sizeof(input));
    memset(&audio, 0, sizeof(audio));
    input.file = -1;
    audio.file = -1;
    audio.thread = -1;
    audio.channel = -1;

    file = sceIoOpen(PMF_PATH, PSP_O_RDONLY, 0);
    if (file < 0) { set_playback_failure("Opening INTRO.PMF", file); goto cleanup; }
    playback_log("PMF opened");
    last_playback_stage = "Reading PMF header";
    { int n = sceIoRead(file, header, sizeof(header));
      if (n != (int)sizeof(header)) { set_playback_failure("Reading PMF header", n); goto cleanup; }
    }
    if (memcmp(header, "PSMF0014", 8) != 0) { set_playback_failure("Checking PSMF magic", -2); goto cleanup; }
    playback_log("PSMF header valid");

    last_playback_stage = "Loading AVCODEC module";
    { int ret = sceUtilityLoadAvModule(PSP_AV_MODULE_AVCODEC);
      if (ret < 0) { set_playback_failure("Loading AVCODEC module", ret); goto cleanup; }
    }
    avcodec = 1;
    playback_log("AVCODEC module loaded");
    last_playback_stage = "Loading MPEGBASE module";
    { int ret = sceUtilityLoadAvModule(PSP_AV_MODULE_MPEGBASE);
      if (ret < 0) { set_playback_failure("Loading MPEGBASE module", ret); goto cleanup; }
    }
    playback_log("MPEGBASE module loaded");
    mpegbase = 1;
    last_playback_stage = "Initializing sceMpeg";
    { int ret = sceMpegInit();
      if (ret < 0) { set_playback_failure("Initializing sceMpeg", ret); goto cleanup; }
    }
    mpeginit = 1;
    playback_log("sceMpegInit succeeded");

    rb_size = sceMpegRingbufferQueryMemSize(PACKETS);
    /* 480x272 is the PSP low-resolution AVC path (mode 4). */
    mpeg_size = sceMpegQueryMemSize(MPEG_MODE);
    last_playback_stage = "Querying MPEG memory sizes";
    if (rb_size <= 0 || mpeg_size <= 0) { set_playback_failure("Querying MPEG memory sizes", rb_size <= 0 ? rb_size : mpeg_size); goto cleanup; }
    rb_mem = memalign(64, rb_size);
    mpeg_mem = memalign(64, mpeg_size);
    /* sceMpegCreate requires a 4 MiB-aligned, 2 MiB Media Engine workspace. */
    ddrtop = memalign(DDRTOP_ALIGN, DDRTOP_SIZE);
    /* The AU descriptor is 64-byte aligned and initialized with 0xFF sentinels. */
    au_mem = memalign(64, 64);
    last_playback_stage = "Allocating MPEG memory";
    if (!rb_mem || !mpeg_mem || !ddrtop || !au_mem) { set_playback_failure("Allocating MPEG memory", -3); goto cleanup; }
    memset(au_mem, 0xFF, 64);
    au = (SceMpegAu *)au_mem;
    au_es = (unsigned char *)ddrtop + DDRTOP_AU_OFFSET;

    input.file = file;
    last_playback_stage = "Constructing MPEG ringbuffer";
    { int ret = sceMpegRingbufferConstruct(&ring, PACKETS, rb_mem, rb_size,
                                           pmf_read_callback, &input);
      if (ret < 0) { set_playback_failure("Constructing MPEG ringbuffer", ret); goto cleanup; }
    }
    rb_ready = 1;
    playback_log("MPEG ringbuffer constructed");
    last_playback_stage = "Creating MPEG decoder";
    { int ret = sceMpegCreate(&mpeg, mpeg_mem, mpeg_size, &ring, FBW,
                              MPEG_MODE, (SceInt32)ddrtop);
      if (ret < 0) { set_playback_failure("Creating MPEG decoder", ret); goto cleanup; }
    }
    playback_log("sceMpegCreate succeeded");
    mpeg_ready = 1;

    mode.iUnk0 = -1;
    mode.iPixelFormat = SCE_MPEG_AVC_FORMAT_8888;
    last_playback_stage = "Reading PMF stream metadata";
    { int ret = sceMpegQueryStreamOffset(&mpeg, header, &offset);
      if (ret < 0) { set_playback_failure("Reading PMF stream offset", ret); goto cleanup; }
    }
    { int ret = sceMpegQueryStreamSize(header, &stream_size);
      if (ret < 0) { set_playback_failure("Reading PMF stream size", ret); goto cleanup; }
    }
    if (offset < (int)sizeof(header)) {
        set_playback_failure("Validating PMF stream offset", -9);
        goto cleanup;
    }
    if (stream_size <= 0) {
        set_playback_failure("Validating PMF stream size", -10);
        goto cleanup;
    }
    if ((stream_size % SECTOR) != 0) {
        set_playback_failure("Validating PMF sector alignment", -11);
        goto cleanup;
    }
    if (offset > (int)sizeof(header)) {
        int skip = offset - (int)sizeof(header);
        unsigned char scratch[512];
        while (skip > 0) {
            int n = sceIoRead(file, scratch, skip > (int)sizeof(scratch) ? (int)sizeof(scratch) : skip);
            if (n <= 0) {
                set_playback_failure("Skipping PMF stream padding", n < 0 ? n : -12);
                goto cleanup;
            }
            skip -= n;
        }
    }
    input.total = (unsigned int)stream_size;
    last_playback_stage = "Registering AVC stream";
    stream = sceMpegRegistStream(&mpeg, 0, 0);
    if (!stream) { set_playback_failure("Registering AVC stream", -4); goto cleanup; }
    stream_ready = 1;
    playback_log("AVC stream registered");
    last_playback_stage = "Initializing AVC access-unit buffer";
    { int ret = sceMpegInitAu(&mpeg, au_es, au);
      if (ret < 0) { set_playback_failure("Initializing AVC access-unit buffer", ret); goto cleanup; }
    }
    last_playback_stage = "Setting AVC decode mode";
    { int ret = sceMpegAvcDecodeMode(&mpeg, &mode);
      if (ret < 0) { set_playback_failure("Setting AVC decode mode", ret); goto cleanup; }
    }
    playback_log("AVC decode mode configured");

    /*
     * Keep the converted RGBA texture in main RAM. A 512x512 texture is
     * required by the GU, even though the visible video is only 480x272.
     */
    frame = memalign(64, VIDEO_TEXTURE_BYTES);
    last_playback_stage = "Allocating video frame buffer";
    if (!frame) { set_playback_failure("Allocating video frame buffer", -5); goto cleanup; }
    memset(frame, 0, VIDEO_TEXTURE_BYTES);
    frame_uncached = (void *)(((unsigned int)frame) | 0x40000000u);
    gu_video_init();
    gu_ready = 1;
    playback_log("GU initialized; entering playback loop");

    /* Start PCM independently of the first video frame. A decoder failure must
       not suppress the soundtrack or leave the user with a silent splash. */
    if (pcm_start(&audio) == 0) {
        audio_started = 1;
        playback_log("PCM audio thread started");
    } else {
        playback_log("WARNING: PCM audio thread could not start");
    }
    first_frame_wait_start = (unsigned long long)sceKernelGetSystemTimeWide();

    for (;;) {
        SceCtrlData pad;
        unsigned int pressed;
        int available, got;

        sceCtrlReadBufferPositive(&pad, 1);
        pressed = pad.Buttons & ~previous;
        previous = pad.Buttons;
        if (pressed & PSP_CTRL_CIRCLE) { result = 1; break; }
        if (pressed & PSP_CTRL_CROSS) audio.paused = !audio.paused;
        if (audio.paused) { sceKernelDelayThread(1000); continue; }

        available = sceMpegRingbufferAvailableSize(&ring);
        if (available < 0) {
            set_playback_failure("Querying MPEG ringbuffer availability", available);
            goto cleanup;
        }
        if (available > 0) {
            int count = available > 32 ? 32 : available;
            last_playback_stage = "Filling MPEG ringbuffer";
            { int ret = sceMpegRingbufferPut(&ring, count, available);
              if (input.read_error < 0) {
                  set_playback_failure("Reading PMF ringbuffer data", input.read_error);
                  goto cleanup;
              }
              if (ret < 0) {
                  set_playback_failure("Filling MPEG ringbuffer", ret);
                  goto cleanup;
              }
            }
        }
        last_playback_stage = "Reading next AVC access unit";
        got = sceMpegGetAvcAu(&mpeg, stream, au, &unknown);
        if (got < 0) { set_playback_failure("Reading AVC access unit", got); goto cleanup; }
        if (got == 0) {
            int decoded;
            if (!first_au_logged) {
                playback_log("First AVC access unit acquired");
                first_au_logged = 1;
            }
            /*
             * Match the established PMPlayer AVC path: pBuffer is NULL here.
             * The decoded YCbCr planes are obtained from DecodeDetail2 below,
             * then converted into the GU texture by sceMpegBaseCscAvc.
             */
            sceKernelDcacheWritebackInvalidateAll();
            if (!first_decode_logged)
                playback_log("Entering sceMpegAvcDecode");
            last_playback_stage = "Decoding first AVC frame";
            decoded = sceMpegAvcDecode(&mpeg, au, FBW, NULL, &pic_num);
            if (!first_decode_logged) {
                snprintf(logbuf, sizeof(logbuf), "First AVC decode returned: ret=%d pic_num=%d", decoded, pic_num);
                playback_log(logbuf);
                first_decode_logged = 1;
            }
            if (decoded != 0) { set_playback_failure("Decoding AVC frame", decoded); goto cleanup; }
            if (pic_num > 0) {
                unsigned int x, y;
                unsigned int *pixels;
                LbpAvcInfo *info;
                LbpAvcYuv *yuv;

                last_playback_stage = "Reading decoded-frame details";
                { int ret = sceMpegAvcDecodeDetail2(&mpeg, &detail2);
                  if (ret != 0 || !detail2 || !detail2->info_buffer || !detail2->yuv_buffer) {
                      set_playback_failure("Reading decoded-frame details", ret < 0 ? ret : -6);
                      goto cleanup;
                  }
                }

                info = detail2->info_buffer;
                yuv = detail2->yuv_buffer;
                memset(&csc, 0, sizeof(csc));
                csc.width = (info->width + 15) >> 4;
                csc.height = (info->height + 15) >> 4;
                csc.mode0 = 0;
                csc.mode1 = 0;
                csc.buffer0 = yuv->buffer0;
                csc.buffer1 = yuv->buffer1;
                csc.buffer2 = yuv->buffer2;
                csc.buffer3 = yuv->buffer3;
                csc.buffer4 = yuv->buffer4;
                csc.buffer5 = yuv->buffer5;
                csc.buffer6 = yuv->buffer6;
                csc.buffer7 = yuv->buffer7;
                if (csc.width <= 0 || csc.height <= 0 ||
                    csc.width > 32 || csc.height > 32) {
                    set_playback_failure("Checking decoded-frame dimensions", -7);
                    goto cleanup;
                }

                /*
                 * The AVC engine leaves frames in YCbCr. Convert them to
                 * ABGR8888 before passing the texture to the Graphics Engine.
                 * Use an uncached alias for the Media Engine's CSC output.
                 */
                sceKernelDcacheWritebackInvalidateAll();
                last_playback_stage = "Converting decoded frame to RGB";
                if (!first_csc_logged) playback_log("Entering sceMpegBaseCscAvc");
                { SceInt32 csc_result = sceMpegBaseCscAvc(frame_uncached, 0, FBW, &csc);
                  if (csc_result != 0) {
                      set_playback_failure("Converting decoded frame to RGB", csc_result);
                      goto cleanup;
                  }
                }
                if (!first_csc_logged) { playback_log("First CSC conversion succeeded"); first_csc_logged = 1; }
                sceKernelDcacheInvalidateRange(frame, VIDEO_TEXTURE_BYTES);

                /* The hardware CSC writes alpha as zero; make pixels opaque. */
                pixels = (unsigned int *)frame;
                for (y = 0; y < H; ++y) {
                    for (x = 0; x < W; ++x)
                        pixels[y * FBW + x] |= 0xFF000000u;
                }
                sceKernelDcacheWritebackInvalidateRange(frame, VIDEO_TEXTURE_BYTES);

                if (audio_started && wait_audio(&audio, au->iPts) < 0) { result = 1; break; }
                gu_video_frame(frame);
                ++frames;
                if (frames == 1) playback_log("First video frame displayed");
            }
        } else {
            if (input.eof && sceMpegRingbufferAvailableSize(&ring) >= PACKETS)
                break;
            sceKernelDelayThread(1000);
        }
        if (frames == 0 &&
            ((unsigned long long)sceKernelGetSystemTimeWide() - first_frame_wait_start) > 10000000ULL) {
            set_playback_failure("Waiting for first decoded video frame (10s timeout)", -8);
            goto cleanup;
        }
    }
    if (frames > 0 && result == -1) result = 0;

cleanup:
    snprintf(logbuf, sizeof(logbuf), "Playback cleanup: result=%d frames=%d", result, frames);
    playback_log(logbuf);
    pcm_stop(&audio);
    if (gu_ready) {
        sceGuTerm();
        pspDebugScreenInit();
    }
    if (au_mem) free(au_mem);
    if (stream_ready) sceMpegUnRegistStream(mpeg, stream);
    if (mpeg_ready) sceMpegDelete(&mpeg);
    if (rb_ready) sceMpegRingbufferDestruct(&ring);
    if (mpeginit) sceMpegFinish();
    if (mpegbase) sceUtilityUnloadAvModule(PSP_AV_MODULE_MPEGBASE);
    if (avcodec) sceUtilityUnloadAvModule(PSP_AV_MODULE_AVCODEC);
    if (rb_mem) free(rb_mem);
    if (mpeg_mem) free(mpeg_mem);
    if (ddrtop) free(ddrtop);
    if (frame) free(frame);
    if (file >= 0) sceIoClose(file);
    return result;
}

static void open_selected(int selected)
{
    if (selected == 0) {
        draw_page("PROTOTYPE BOOTED",
                  "The PSP executable and controller input are running.\n\n"
                  "This is the bootable foundation for the project.");
    } else if (selected == 1) {
        int status = play_intro();
        if (status == 0)
            draw_page("INTRO FINISHED", "The in-game cutscene reached the end.");
        else if (status == 1)
            draw_page("INTRO STOPPED", "Playback was stopped. Press O to return.");
        else
            draw_page("VIDEO PLAYER ERROR",
                      "Could not open or decode the intro. Check that INTRO.PMF and "
                      "INTRO.PCM are beside EBOOT.PBP in the game folder.");
    } else if (selected == 2) {
        draw_page("CREDITS", "LittleBigPlanet PSP: Alpha\nHomebrew boot and video player prototype.");
    } else {
        sceKernelExitGame();
    }
}

int main(void)
{
    SceCtrlData pad;
    unsigned int previous = 0;
    int selected = 0;
    int startup_status;

    playback_log("Application booted");
    setup_callbacks();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    pspDebugScreenInit();

    /* Automatic boot sequence: splash (3 s), black (2 s), then intro. */
    show_startup_splash();
    startup_status = play_intro();
    if (startup_status < 0) {
        {
            char error_body[256];
            snprintf(error_body, sizeof(error_body),
                     "Failed at: %s\nError code: 0x%08X\n\nA diagnostic log is also attempted at ms0:/PLAYBACK.LOG.",
                     last_playback_stage, (unsigned int)last_playback_error);
            draw_page("INTRO PLAYER ERROR", error_body);
        }
        for (;;) {
            sceCtrlReadBufferPositive(&pad, 1);
            if (pad.Buttons & PSP_CTRL_CIRCLE)
                break;
            sceKernelDelayThread(1000 * 1000 / 60);
        }
    }

    selected = 0;
    draw_menu(selected);

    for (;;) {
        unsigned int pressed;
        sceCtrlReadBufferPositive(&pad, 1);
        pressed = pad.Buttons & ~previous;
        previous = pad.Buttons;
        if (pad.Buttons & PSP_CTRL_HOME) sceKernelExitGame();
        if (pressed & PSP_CTRL_UP) {
            if (selected > 0) --selected;
            draw_menu(selected);
        } else if (pressed & PSP_CTRL_DOWN) {
            if (selected < 3) ++selected;
            draw_menu(selected);
        } else if (pressed & PSP_CTRL_CROSS) {
            open_selected(selected);
            if (selected != 3) {
                while (1) {
                    sceCtrlReadBufferPositive(&pad, 1);
                    if (pad.Buttons & PSP_CTRL_CIRCLE) break;
                    sceKernelDelayThread(1000 * 1000 / 60);
                }
                draw_menu(selected);
            }
        }
        sceKernelDelayThread(1000 * 1000 / 60);
    }
    return 0;
}
