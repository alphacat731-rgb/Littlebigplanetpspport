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
PSP_MAIN_THREAD_STACK_SIZE_KB(512);
PSP_HEAP_SIZE_KB(-2048);

#define W 480
#define H 272
#define FBW 512
#define PACKETS 512
#define MPEG_MODE 0
#define SECTOR 2048
/* sceMpegGetAvcAu returns this while the ringbuffer has no complete AU yet. */
#define LBP_MPEG_ERROR_NOT_COMPLETED 0x80618001u
#define PCM_SAMPLES 512
#define PCM_BYTES (PCM_SAMPLES * 2 * sizeof(short))
#define PMF_PATH "ms0:/PSP/GAME/LBPPSP2008/INTRO.PMF"
#define PCM_PATH "ms0:/PSP/GAME/LBPPSP2008/INTRO.PCM"
#define LOG_PATH "ms0:/PSP/GAME/LBPPSP2008/PLAYBACK.LOG"
#define ROOT_LOG_PATH "ms0:/PLAYBACK.LOG"
#define DRAW_BYTES (FBW * H * 4)
#define DISP_BYTES (FBW * H * 4)

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
    volatile int error;
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
    pspDebugScreenPrintf("LBPPSP2008\n");
    pspDebugScreenPrintf("----------------------------------------------\n\n");
    pspDebugScreenSetTextColor(0xFF9BE24A);
    pspDebugScreenPrintf("  LittleBigPlanet PSP 2008 prototype\n\n");
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
    pspDebugScreenPrintf("LBPPSP2008\n");
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
            if (n < 0)
                out->error = n;
            out->done = 1;
            break;
        }
        if ((unsigned int)n < PCM_BYTES)
            memset((unsigned char *)out->buffer + n, 0, PCM_BYTES - n);
        {
            int output_result = sceAudioOutputBlocking(out->channel,
                                                       PSP_AUDIO_VOLUME_MAX,
                                                       out->buffer);
            if (output_result < 0) {
                out->error = output_result;
                out->done = 1;
                break;
            }
        }
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
    char message[128];
    out->running = 0;
    if (out->thread >= 0) {
        sceKernelTerminateDeleteThread(out->thread);
        out->thread = -1;
    }
    if (out->error < 0) {
        snprintf(message, sizeof(message), "PCM playback failed: 0x%08X",
                 (unsigned int)out->error);
        playback_log(message);
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

    file = sceIoOpen("ms0:/PSP/GAME/LBPPSP2008/LOADING.RGBA", PSP_O_RDONLY, 0);
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
    int result = -1;
    int avcodec = 0, mpegbase = 0, mpeginit = 0;
    int rb_ready = 0, mpeg_ready = 0, stream_ready = 0, gu_ready = 0;
    int au_es_ready = 0;
    SceUID file = -1;
    unsigned char header[0x800];
    SceMpeg mpeg = NULL;
    SceMpegRingbuffer ring;
    SceMpegStream *stream = NULL;
    SceMpegAu au_storage;
    SceMpegAu *au = &au_storage;
    SceMpegAvcMode mode;
    PmfInput input;
    PcmOutput audio;
    void *rb_mem = NULL, *mpeg_mem = NULL, *au_es = NULL;
    void *frame = NULL;
    int rb_size, mpeg_size;
    SceInt32 offset = 0, stream_size = 0, unknown = 0, decode_status = 0;
    unsigned int previous = 0;
    int audio_started = 0, frames = 0, decode_calls = 0;
    int au_wait_polls = 0, idle_after_eof = 0;
    unsigned long long first_frame_wait_start = 0;
    unsigned long long last_frame_time = 0;
    char logbuf[160];

    last_playback_stage = "Opening INTRO.PMF";
    last_playback_error = 0;
    playback_log("=== PSP standard sceMpeg player begin ===");
    memset(&input, 0, sizeof(input));
    memset(&audio, 0, sizeof(audio));
    input.file = -1;
    audio.file = -1;
    audio.thread = -1;
    audio.channel = -1;

    file = sceIoOpen(PMF_PATH, PSP_O_RDONLY, 0);
    if (file < 0) {
        set_playback_failure("Opening INTRO.PMF", file);
        goto cleanup;
    }
    {
        int n = sceIoRead(file, header, sizeof(header));
        if (n != (int)sizeof(header)) {
            set_playback_failure("Reading PMF header", n);
            goto cleanup;
        }
    }
    if (memcmp(header, "PSMF0014", 8) != 0) {
        set_playback_failure("Checking PSMF magic", -2);
        goto cleanup;
    }
    playback_log("PSMF header valid");

    last_playback_stage = "Loading AVCODEC module";
    {
        int ret = sceUtilityLoadAvModule(PSP_AV_MODULE_AVCODEC);
        if (ret < 0) {
            set_playback_failure("Loading AVCODEC module", ret);
            goto cleanup;
        }
    }
    avcodec = 1;

    last_playback_stage = "Loading MPEGBASE module";
    {
        int ret = sceUtilityLoadAvModule(PSP_AV_MODULE_MPEGBASE);
        if (ret < 0) {
            set_playback_failure("Loading MPEGBASE module", ret);
            goto cleanup;
        }
    }
    mpegbase = 1;

    last_playback_stage = "Initializing sceMpeg";
    {
        int ret = sceMpegInit();
        if (ret < 0) {
            set_playback_failure("Initializing sceMpeg", ret);
            goto cleanup;
        }
    }
    mpeginit = 1;
    playback_log("sceMpegInit succeeded");

    /*
     * Use the ordinary PMF playback setup proven by PSPRelay on a PSP-2000
     * and PPSSPP: sceMpeg mode 0, sceMpegMallocAvcEsBuf, and a decoded-frame
     * buffer in eDRAM immediately after the GU draw/display buffers.
     *
     * The previous experimental mode-4 / DDR-top / NULL-output / CSC route
     * was not the known-good sceMpegAvcDecode ABI and is removed here.
     */
    rb_size = sceMpegRingbufferQueryMemSize(PACKETS);
    mpeg_size = sceMpegQueryMemSize(MPEG_MODE);
    last_playback_stage = "Querying MPEG memory sizes";
    if (rb_size <= 0 || mpeg_size <= 0) {
        set_playback_failure("Querying MPEG memory sizes",
                             rb_size <= 0 ? rb_size : mpeg_size);
        goto cleanup;
    }

    rb_mem = memalign(64, rb_size);
    mpeg_mem = memalign(64, mpeg_size);
    if (!rb_mem || !mpeg_mem) {
        set_playback_failure("Allocating MPEG memory", -3);
        goto cleanup;
    }

    input.file = file;
    last_playback_stage = "Constructing MPEG ringbuffer";
    {
        int ret = sceMpegRingbufferConstruct(&ring, PACKETS, rb_mem, rb_size,
                                             pmf_read_callback, &input);
        if (ret < 0) {
            set_playback_failure("Constructing MPEG ringbuffer", ret);
            goto cleanup;
        }
    }
    rb_ready = 1;

    last_playback_stage = "Creating MPEG decoder";
    {
        int ret = sceMpegCreate(&mpeg, mpeg_mem, mpeg_size, &ring,
                                FBW, MPEG_MODE, 0);
        if (ret < 0) {
            set_playback_failure("Creating MPEG decoder", ret);
            goto cleanup;
        }
    }
    mpeg_ready = 1;
    playback_log("sceMpegCreate mode 0 succeeded");

    mode.iUnk0 = -1;
    mode.iPixelFormat = SCE_MPEG_AVC_FORMAT_8888;
    last_playback_stage = "Setting AVC decode mode";
    {
        int ret = sceMpegAvcDecodeMode(&mpeg, &mode);
        if (ret < 0) {
            set_playback_failure("Setting AVC decode mode", ret);
            goto cleanup;
        }
    }

    last_playback_stage = "Reading PMF stream metadata";
    {
        int ret = sceMpegQueryStreamOffset(&mpeg, header, &offset);
        if (ret < 0) {
            set_playback_failure("Reading PMF stream offset", ret);
            goto cleanup;
        }
    }
    {
        int ret = sceMpegQueryStreamSize(header, &stream_size);
        if (ret < 0) {
            set_playback_failure("Reading PMF stream size", ret);
            goto cleanup;
        }
    }
    if (offset < (int)sizeof(header) || stream_size <= 0 ||
        (stream_size % SECTOR) != 0) {
        set_playback_failure("Validating PMF stream bounds", -9);
        goto cleanup;
    }

    if (offset > (int)sizeof(header)) {
        int skip = offset - (int)sizeof(header);
        unsigned char scratch[512];
        while (skip > 0) {
            int n = sceIoRead(file, scratch,
                              skip > (int)sizeof(scratch) ?
                              (int)sizeof(scratch) : skip);
            if (n <= 0) {
                set_playback_failure("Skipping PMF stream padding",
                                     n < 0 ? n : -10);
                goto cleanup;
            }
            skip -= n;
        }
    }
    input.total = (unsigned int)stream_size;

    last_playback_stage = "Registering AVC stream";
    stream = sceMpegRegistStream(&mpeg, 0, 0);
    if (!stream) {
        set_playback_failure("Registering AVC stream", -4);
        goto cleanup;
    }
    stream_ready = 1;

    last_playback_stage = "Allocating AVC elementary-stream buffer";
    au_es = sceMpegMallocAvcEsBuf(&mpeg);
    if (!au_es) {
        set_playback_failure("Allocating AVC elementary-stream buffer", -5);
        goto cleanup;
    }
    au_es_ready = 1;
    memset(au, 0xFF, sizeof(*au));
    {
        int ret = sceMpegInitAu(&mpeg, au_es, au);
        if (ret < 0) {
            set_playback_failure("Initializing AVC access unit", ret);
            goto cleanup;
        }
    }

    /*
     * sceMpegAvcDecode writes directly to the PSP's eDRAM video area.
     * Its actual ABI expects a pointer to the output-buffer pointer.
     */
    frame = (unsigned char *)sceGeEdramGetAddr() + DRAW_BYTES + DISP_BYTES;
    if (!frame) {
        set_playback_failure("Locating eDRAM video buffer", -6);
        goto cleanup;
    }
    gu_video_init();
    gu_ready = 1;
    playback_log("eDRAM video buffer ready; entering standard decode loop");

    {
        int pcm_result = pcm_start(&audio);
        if (pcm_result == 0) {
            audio_started = 1;
            playback_log("PCM soundtrack thread started");
        } else {
            snprintf(logbuf, sizeof(logbuf),
                     "WARNING: PCM audio startup failed: %d", pcm_result);
            playback_log(logbuf);
        }
    }

    first_frame_wait_start = (unsigned long long)sceKernelGetSystemTimeWide();
    last_frame_time = first_frame_wait_start;

    for (;;) {
        SceCtrlData pad;
        unsigned int pressed;
        int available, got;

        sceCtrlReadBufferPositive(&pad, 1);
        pressed = pad.Buttons & ~previous;
        previous = pad.Buttons;
        if (pressed & PSP_CTRL_CIRCLE) {
            result = 1;
            break;
        }
        if (pressed & PSP_CTRL_CROSS)
            audio.paused = !audio.paused;
        if (audio.paused) {
            sceKernelDelayThread(1000);
            continue;
        }

        available = sceMpegRingbufferAvailableSize(&ring);
        if (available < 0) {
            set_playback_failure("Querying MPEG ringbuffer availability",
                                 available);
            goto cleanup;
        }
        if (available > 0) {
            int count = available > 32 ? 32 : available;
            int put_result = sceMpegRingbufferPut(&ring, count, available);
            if (input.read_error < 0) {
                set_playback_failure("Reading PMF ringbuffer data",
                                     input.read_error);
                goto cleanup;
            }
            if (put_result < 0) {
                set_playback_failure("Filling MPEG ringbuffer", put_result);
                goto cleanup;
            }
        }

        last_playback_stage = "Reading next AVC access unit";
        got = sceMpegGetAvcAu(&mpeg, stream, au, &unknown);
        if (got == 0) {
            int decoded;
            if (frames == 0)
                playback_log("First AVC access unit acquired");

            last_playback_stage = "Decoding AVC frame to eDRAM";
            /*
             * Do not pass NULL and do not pass a flat pixel buffer.
             * The known-good PSP ABI takes &frame (double indirection).
             */
            decoded = sceMpegAvcDecode(&mpeg, au, FBW, &frame,
                                       &decode_status);
            ++decode_calls;
            if (decode_calls == 1 || (decode_calls % 30) == 0) {
                snprintf(logbuf, sizeof(logbuf),
                         "AVC decode #%d: ret=%d status=%d pts=%u input=%u/%u",
                         decode_calls, decoded, (int)decode_status,
                         (unsigned int)au->iPts,
                         input.read_bytes, input.total);
                playback_log(logbuf);
            }
            if (decoded != 0) {
                set_playback_failure("Decoding AVC frame to eDRAM", decoded);
                goto cleanup;
            }

            if (decode_status == 1) {
                last_playback_stage = "Submitting decoded frame to GU";
                if (audio_started && wait_audio(&audio, au->iPts) < 0) {
                    result = 1;
                    break;
                }
                gu_video_frame(frame);
                ++frames;
                last_frame_time =
                    (unsigned long long)sceKernelGetSystemTimeWide();
                idle_after_eof = 0;
                if (frames == 1 || (frames % 30) == 0) {
                    snprintf(logbuf, sizeof(logbuf),
                             "GU frame submitted: frames=%d pts=%u",
                             frames, (unsigned int)au->iPts);
                    playback_log(logbuf);
                }
            }
        } else {
            ++au_wait_polls;
            if (au_wait_polls == 1 || (au_wait_polls % 240) == 0) {
                snprintf(logbuf, sizeof(logbuf),
                         "AVC AU pending: ret=0x%08X polls=%d input=%u/%u frames=%d",
                         (unsigned int)got, au_wait_polls,
                         input.read_bytes, input.total, frames);
                playback_log(logbuf);
            }
            sceKernelDelayThread(1000);
        }

        if (input.eof && available >= PACKETS && got != 0) {
            if (++idle_after_eof > 120) {
                playback_log("PMF ringbuffer drained; end of video reached");
                break;
            }
        } else {
            idle_after_eof = 0;
        }

        {
            unsigned long long now =
                (unsigned long long)sceKernelGetSystemTimeWide();
            unsigned long long reference =
                frames == 0 ? first_frame_wait_start : last_frame_time;
            if (now - reference > 10000000ULL) {
                set_playback_failure(frames == 0 ?
                    "Waiting 10 seconds for first decoded frame" :
                    "Waiting 10 seconds for the next decoded frame", -8);
                goto cleanup;
            }
        }
    }

    if (frames > 0 && result == -1)
        result = 0;
    if (frames == 0 && result == -1)
        set_playback_failure("Playback ended without a decoded frame", -11);

cleanup:
    snprintf(logbuf, sizeof(logbuf),
             "Playback cleanup: result=%d frames=%d decodes=%d au_wait=%d input=%u/%u",
             result, frames, decode_calls, au_wait_polls,
             input.read_bytes, input.total);
    playback_log(logbuf);
    pcm_stop(&audio);

    if (gu_ready) {
        sceGuTerm();
        pspDebugScreenInit();
    }
    if (au_es_ready)
        sceMpegFreeAvcEsBuf(&mpeg, au_es);
    if (stream_ready)
        sceMpegUnRegistStream(mpeg, stream);
    if (mpeg_ready)
        sceMpegDelete(&mpeg);
    if (rb_ready)
        sceMpegRingbufferDestruct(&ring);
    if (mpeginit)
        sceMpegFinish();
    if (mpegbase)
        sceUtilityUnloadAvModule(PSP_AV_MODULE_MPEGBASE);
    if (avcodec)
        sceUtilityUnloadAvModule(PSP_AV_MODULE_AVCODEC);
    if (rb_mem)
        free(rb_mem);
    if (mpeg_mem)
        free(mpeg_mem);
    if (file >= 0)
        sceIoClose(file);
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
