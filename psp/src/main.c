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
PSP_MAIN_THREAD_STACK_SIZE_KB(128);
PSP_HEAP_SIZE_KB(-2048);

#define W 480
#define H 272
#define FBW 512
#define PACKETS 512
#define MPEG_MODE 0
#define SECTOR 2048
/* sceMpegGetAvcAu returns this while the ringbuffer has no complete AU yet. */
#define LBP_MPEG_ERROR_NOT_COMPLETED 0x80618001u
#define AUDIO_BUFS 4
#define PMF_PATH "ms0:/PSP/GAME/LBPPSP2008/INTRO.PMF"
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
    SceUID thread;
    SceUID lock;
    int channel;
    int volume;
    volatile int done;
    volatile int paused;
    volatile int error;
    int output_size;
    void *buffers[AUDIO_BUFS];
    int head;
    int tail;
    int count;
} AtracOutput;

/* Small ATRAC3+ buffers are retained for the app lifetime because the PSP
   audio DMA may still be reading the last submitted buffer. */
static void *g_atrac_output_buffers[AUDIO_BUFS] = { NULL, NULL, NULL, NULL };
static int g_atrac_output_size = 0;

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

static int atrac_thread(SceSize args, void *argp)
{
    AtracOutput *out = *(AtracOutput **)argp;
    (void)args;

    while (!out->done) {
        void *buffer = NULL;

        if (out->paused) {
            sceKernelDelayThread(1000);
            continue;
        }

        if (sceKernelWaitSema(out->lock, 1, NULL) < 0) {
            sceKernelDelayThread(1000);
            continue;
        }
        if (out->count > 0)
            buffer = out->buffers[out->head];
        sceKernelSignalSema(out->lock, 1);

        if (!buffer) {
            sceKernelDelayThread(1000);
            continue;
        }

        {
            int ret = sceAudioOutputBlocking(out->channel, out->volume, buffer);
            if (ret < 0) {
                out->error = ret;
                out->done = 1;
                break;
            }
        }

        sceKernelWaitSema(out->lock, 1, NULL);
        if (out->count > 0) {
            out->head = (out->head + 1) % AUDIO_BUFS;
            out->count--;
        }
        sceKernelSignalSema(out->lock, 1);
    }
    return 0;
}

static int atrac_start(AtracOutput *out, int es_output_size)
{
    int i;
    AtracOutput *arg = out;

    memset(out, 0, sizeof(*out));
    out->thread = -1;
    out->lock = -1;
    out->channel = -1;
    out->volume = PSP_AUDIO_VOLUME_MAX;
    out->output_size = es_output_size;

    if (es_output_size <= 0 || (es_output_size & 3) != 0)
        return -1;

    if (g_atrac_output_size != es_output_size) {
        /* Previous buffers are intentionally retained until process exit. */
        for (i = 0; i < AUDIO_BUFS; ++i) {
            g_atrac_output_buffers[i] = memalign(64, es_output_size);
            if (!g_atrac_output_buffers[i])
                return -2;
        }
        g_atrac_output_size = es_output_size;
    }
    for (i = 0; i < AUDIO_BUFS; ++i)
        out->buffers[i] = g_atrac_output_buffers[i];

    out->channel = sceAudioChReserve(-1, 512, PSP_AUDIO_FORMAT_STEREO);
    if (out->channel < 0)
        return out->channel;

    {
        int ret = sceAudioSetChannelDataLen(out->channel, es_output_size / 4);
        if (ret < 0)
            return ret;
    }

    out->lock = sceKernelCreateSema("LBPPSP ATRAC queue", 0, 1, 1, NULL);
    if (out->lock < 0)
        return out->lock;

    out->thread = sceKernelCreateThread("LBPPSP ATRAC", atrac_thread, 0x3E,
                                        0x4000, PSP_THREAD_ATTR_USER, NULL);
    if (out->thread < 0)
        return out->thread;

    if (sceKernelStartThread(out->thread, sizeof(arg), &arg) < 0)
        return -3;

    playback_log("ATRAC3+ output queue started (four 64-byte-aligned buffers)");
    return 0;
}

static void *atrac_reserve_buffer(AtracOutput *out)
{
    void *buffer = NULL;
    if (out->lock < 0)
        return NULL;
    sceKernelWaitSema(out->lock, 1, NULL);
    if (out->count < AUDIO_BUFS)
        buffer = out->buffers[out->tail];
    sceKernelSignalSema(out->lock, 1);
    return buffer;
}

static int atrac_queue_buffer(AtracOutput *out)
{
    int ret = -1;
    if (out->lock < 0)
        return ret;
    sceKernelWaitSema(out->lock, 1, NULL);
    if (out->count < AUDIO_BUFS) {
        out->tail = (out->tail + 1) % AUDIO_BUFS;
        out->count++;
        ret = 0;
    }
    sceKernelSignalSema(out->lock, 1);
    return ret;
}

static int atrac_pending_buffers(AtracOutput *out)
{
    int count = 0;
    if (out->lock < 0)
        return 0;
    sceKernelWaitSema(out->lock, 1, NULL);
    count = out->count;
    sceKernelSignalSema(out->lock, 1);
    return count;
}

static void atrac_stop(AtracOutput *out)
{
    char message[128];

    out->done = 1;
    if (out->thread >= 0) {
        /* sceAudioOutputBlocking may block until its current buffer finishes. */
        sceKernelTerminateDeleteThread(out->thread);
        out->thread = -1;
    }
    if (out->error < 0) {
        snprintf(message, sizeof(message), "ATRAC output failed: 0x%08X",
                 (unsigned int)out->error);
        playback_log(message);
    }
    if (out->lock >= 0) {
        sceKernelDeleteSema(out->lock);
        out->lock = -1;
    }
    if (out->channel >= 0) {
        sceAudioChRelease(out->channel);
        out->channel = -1;
    }
    /* Global audio output buffers are intentionally retained for DMA safety. */
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

static int play_intro(void)
{
    int result = -1;
    int avcodec = 0, mpegbase = 0, mpeginit = 0;
    int rb_ready = 0, mpeg_ready = 0;
    int video_stream_ready = 0, audio_stream_ready = 0, gu_ready = 0;
    int video_es_ready = 0, audio_attempted = 0;
    SceUID file = -1;
    unsigned char header[0x800];
    SceMpeg mpeg = NULL;
    SceMpegRingbuffer ring;
    SceMpegStream *video_stream = NULL, *audio_stream = NULL;
    SceMpegAu video_au_storage, audio_au_storage;
    SceMpegAu *video_au = &video_au_storage;
    SceMpegAu *audio_au = &audio_au_storage;
    SceMpegAvcMode mode;
    PmfInput input;
    AtracOutput audio;
    void *rb_mem = NULL, *mpeg_mem = NULL;
    void *video_es = NULL, *audio_es = NULL, *frame = NULL;
    int rb_size, mpeg_size;
    SceInt32 offset = 0, stream_size = 0, unknown = 0, decode_status = 0;
    SceInt32 atrac_es_size = 0, atrac_out_size = 0;
    int audio_init = 1, audio_clock = 90000;
    unsigned int previous = 0;
    int frames = 0, video_decode_calls = 0, audio_decode_calls = 0;
    int video_wait_polls = 0, audio_wait_polls = 0;
    unsigned long long first_frame_wait_start = 0;
    unsigned long long last_frame_time = 0;
    char logbuf[176];

    last_playback_stage = "Opening multiplexed INTRO.PMF";
    last_playback_error = 0;
    playback_log("=== AVC + ATRAC3+ sceMpeg player begin ===");
    memset(&input, 0, sizeof(input));
    memset(&audio, 0, sizeof(audio));
    input.file = -1;
    audio.thread = -1;
    audio.lock = -1;
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
    if (header[0x81] != 0x02 || header[0x92] != 0xBD) {
        set_playback_failure("Checking AVC/ATRAC3+ stream descriptors", -3);
        goto cleanup;
    }
    playback_log("PSMF header advertises AVC 0xE0 and ATRAC3+ 0xBD");

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

    /* Standard PSPRelay setup, with conservative PSP-1000-sized buffers. */
    rb_size = sceMpegRingbufferQueryMemSize(PACKETS);
    mpeg_size = sceMpegQueryMemSize(0);
    last_playback_stage = "Querying MPEG memory sizes";
    if (rb_size <= 0 || mpeg_size <= 0) {
        set_playback_failure("Querying MPEG memory sizes",
                             rb_size <= 0 ? rb_size : mpeg_size);
        goto cleanup;
    }

    rb_mem = memalign(64, rb_size);
    mpeg_mem = memalign(64, mpeg_size);
    if (!rb_mem || !mpeg_mem) {
        set_playback_failure("Allocating MPEG memory", -4);
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
        int ret = sceMpegCreate(&mpeg, mpeg_mem, mpeg_size, &ring, FBW, 0, 0);
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
        set_playback_failure("Validating PMF stream bounds", -5);
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
                                     n < 0 ? n : -6);
                goto cleanup;
            }
            skip -= n;
        }
    }
    input.total = (unsigned int)stream_size;

    last_playback_stage = "Registering AVC stream 0xE0";
    video_stream = sceMpegRegistStream(&mpeg, 0, 0);
    if (!video_stream) {
        set_playback_failure("Registering AVC stream", -7);
        goto cleanup;
    }
    video_stream_ready = 1;

    last_playback_stage = "Allocating AVC elementary-stream buffer";
    video_es = sceMpegMallocAvcEsBuf(&mpeg);
    if (!video_es) {
        set_playback_failure("Allocating AVC elementary-stream buffer", -8);
        goto cleanup;
    }
    video_es_ready = 1;
    memset(video_au, 0xFF, sizeof(*video_au));
    {
        int ret = sceMpegInitAu(&mpeg, video_es, video_au);
        if (ret < 0) {
            set_playback_failure("Initializing AVC access unit", ret);
            goto cleanup;
        }
    }

    last_playback_stage = "Registering ATRAC3+ stream 0xBD";
    audio_stream = sceMpegRegistStream(&mpeg, 1, 0);
    if (!audio_stream) {
        set_playback_failure("Registering ATRAC3+ audio stream", -9);
        goto cleanup;
    }
    audio_stream_ready = 1;

    last_playback_stage = "Querying ATRAC3+ decoder buffer sizes";
    {
        int ret = sceMpegQueryAtracEsSize(&mpeg, &atrac_es_size, &atrac_out_size);
        if (ret < 0 || atrac_es_size <= 0 || atrac_out_size <= 0 ||
            (atrac_out_size & 3) != 0) {
            set_playback_failure("Querying ATRAC3+ buffer sizes",
                                 ret < 0 ? ret : -10);
            goto cleanup;
        }
    }
    audio_es = memalign(64, atrac_es_size);
    if (!audio_es) {
        set_playback_failure("Allocating ATRAC3+ access-unit buffer", -11);
        goto cleanup;
    }
    memset(audio_au, 0xFF, sizeof(*audio_au));
    {
        int ret = sceMpegInitAu(&mpeg, audio_es, audio_au);
        if (ret < 0) {
            set_playback_failure("Initializing ATRAC3+ access unit", ret);
            goto cleanup;
        }
    }

    last_playback_stage = "Starting ATRAC3+ hardware audio output";
    audio_attempted = 1;
    {
        int ret = atrac_start(&audio, atrac_out_size);
        if (ret < 0) {
            set_playback_failure("Starting ATRAC3+ audio output", ret);
            goto cleanup;
        }
    }

    /* Use the eDRAM region after the two display buffers.  This avoids a
       second 512x512 software framebuffer on the 32 MB PSP-1000. */
    frame = (unsigned char *)sceGeEdramGetAddr() + DRAW_BYTES + DISP_BYTES;
    if (!frame) {
        set_playback_failure("Locating eDRAM video buffer", -12);
        goto cleanup;
    }
    gu_video_init();
    gu_ready = 1;
    playback_log("eDRAM video buffer ready; multiplexed playback loop started");

    first_frame_wait_start = (unsigned long long)sceKernelGetSystemTimeWide();
    last_frame_time = first_frame_wait_start;

    for (;;) {
        SceCtrlData pad;
        unsigned int pressed;
        int available, video_got = -1, audio_got = -1;
        int video_frames_this_step = 0;

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
            set_playback_failure("Querying MPEG ringbuffer availability", available);
            goto cleanup;
        }
        if (available > 0) {
            int count = available > 32 ? 32 : available;
            int put_result = sceMpegRingbufferPut(&ring, count, available);
            if (input.read_error < 0) {
                set_playback_failure("Reading PMF ringbuffer data", input.read_error);
                goto cleanup;
            }
            if (put_result < 0) {
                set_playback_failure("Filling MPEG ringbuffer", put_result);
                goto cleanup;
            }
        }

        /* Decode ATRAC3+ into a four-buffer queue; audio output runs separately. */
        if (atrac_pending_buffers(&audio) < AUDIO_BUFS) {
            void *audio_buffer = atrac_reserve_buffer(&audio);
            if (audio_buffer) {
                last_playback_stage = "Reading ATRAC3+ access unit";
                audio_got = sceMpegGetAtracAu(&mpeg, audio_stream, audio_au, &unknown);
                if (audio_got == 0) {
                    int decoded;
                    last_playback_stage = "Decoding ATRAC3+ access unit";
                    decoded = sceMpegAtracDecode(&mpeg, audio_au, audio_buffer, audio_init);
                    ++audio_decode_calls;
                    if (audio_decode_calls == 1 || (audio_decode_calls % 100) == 0) {
                        snprintf(logbuf, sizeof(logbuf),
                                 "ATRAC3+ decode #%d: ret=%d init=%d pts=%u",
                                 audio_decode_calls, decoded, audio_init,
                                 (unsigned int)audio_au->iPts);
                        playback_log(logbuf);
                    }
                    if (decoded != 0) {
                        set_playback_failure("Decoding ATRAC3+ access unit", decoded);
                        goto cleanup;
                    }
                    audio_init = 0;
                    if (audio_au->iPts == 0xFFFFFFFFu)
                        audio_clock += 4180;
                    else
                        audio_clock = (int)audio_au->iPts;
                    if (atrac_queue_buffer(&audio) < 0) {
                        set_playback_failure("Queuing decoded ATRAC3+ audio", -13);
                        goto cleanup;
                    }
                    audio_wait_polls = 0;
                } else {
                    ++audio_wait_polls;
                    if (audio_wait_polls == 1 || (audio_wait_polls % 240) == 0) {
                        snprintf(logbuf, sizeof(logbuf),
                                 "ATRAC3+ AU pending: ret=0x%08X polls=%d input=%u/%u",
                                 (unsigned int)audio_got, audio_wait_polls,
                                 input.read_bytes, input.total);
                        playback_log(logbuf);
                    }
                    if (audio_got == LBP_MPEG_ERROR_NOT_COMPLETED)
                        audio_got = -1;
                }
            }
        }

        /* Decode video close to the latest audio timestamp, at most 3 frames. */
        while (video_frames_this_step < 3) {
            int decoded;
            video_got = sceMpegGetAvcAu(&mpeg, video_stream, video_au, &unknown);
            if (video_got != 0) {
                ++video_wait_polls;
                if (video_wait_polls == 1 || (video_wait_polls % 240) == 0) {
                    snprintf(logbuf, sizeof(logbuf),
                             "AVC AU pending: ret=0x%08X polls=%d input=%u/%u frames=%d",
                             (unsigned int)video_got, video_wait_polls,
                             input.read_bytes, input.total, frames);
                    playback_log(logbuf);
                }
                if (video_got == LBP_MPEG_ERROR_NOT_COMPLETED)
                    video_got = -1;
                break;
            }
            video_wait_polls = 0;
            last_playback_stage = "Decoding AVC frame to eDRAM";
            decoded = sceMpegAvcDecode(&mpeg, video_au, FBW, &frame, &decode_status);
            ++video_decode_calls;
            if (video_decode_calls == 1 || (video_decode_calls % 30) == 0) {
                snprintf(logbuf, sizeof(logbuf),
                         "AVC decode #%d: ret=%d status=%d pts=%u audio_pts=%d",
                         video_decode_calls, decoded, decode_status,
                         (unsigned int)video_au->iPts, audio_clock);
                playback_log(logbuf);
            }
            if (decoded != 0) {
                set_playback_failure("Decoding AVC frame to eDRAM", decoded);
                goto cleanup;
            }
            if (decode_status == 1) {
                if (audio_clock > 90000 &&
                    (int)video_au->iPts > audio_clock + 3003)
                    break;
                last_playback_stage = "Submitting decoded frame to GU";
                gu_video_frame(frame);
                ++frames;
                ++video_frames_this_step;
                last_frame_time =
                    (unsigned long long)sceKernelGetSystemTimeWide();
                if (frames == 1 || (frames % 30) == 0) {
                    snprintf(logbuf, sizeof(logbuf),
                             "GU frame submitted: frames=%d video_pts=%u audio_pts=%d",
                             frames, (unsigned int)video_au->iPts, audio_clock);
                    playback_log(logbuf);
                }
            }
        }

        /* Finish only after the stream ring and queued audio have drained. */
        {
            int left = sceMpegRingbufferAvailableSize(&ring);
            if (input.eof && left >= PACKETS &&
                video_got != 0 && audio_got != 0 &&
                atrac_pending_buffers(&audio) == 0) {
                playback_log("PSMF ringbuffer and both media streams drained");
                break;
            }
        }

        if (audio.error < 0) {
            set_playback_failure("ATRAC3+ audio output thread", audio.error);
            goto cleanup;
        }

        {
            unsigned long long now =
                (unsigned long long)sceKernelGetSystemTimeWide();
            unsigned long long reference =
                frames == 0 ? first_frame_wait_start : last_frame_time;
            if (!input.eof && now - reference > 10000000ULL) {
                set_playback_failure(frames == 0 ?
                    "Waiting 10 seconds for first decoded frame" :
                    "Waiting 10 seconds for the next decoded frame", -14);
                goto cleanup;
            }
        }

        if (video_got != 0 && audio_got != 0)
            sceKernelDelayThread(1000);
    }

    if (frames > 0 && result == -1)
        result = 0;
    if (frames == 0 && result == -1)
        set_playback_failure("Playback ended without a decoded frame", -15);

cleanup:
    snprintf(logbuf, sizeof(logbuf),
             "Playback cleanup: result=%d frames=%d video_decodes=%d atrac_decodes=%d "
             "video_wait=%d audio_wait=%d input=%u/%u audio_queue=%d",
             result, frames, video_decode_calls, audio_decode_calls,
             video_wait_polls, audio_wait_polls,
             input.read_bytes, input.total,
             audio.lock >= 0 ? atrac_pending_buffers(&audio) : 0);
    playback_log(logbuf);

    if (audio_attempted)
        atrac_stop(&audio);
    if (gu_ready) {
        sceGuTerm();
        pspDebugScreenInit();
    }
    if (video_es_ready)
        sceMpegFreeAvcEsBuf(&mpeg, video_es);
    if (audio_stream_ready)
        sceMpegUnRegistStream(mpeg, audio_stream);
    if (video_stream_ready)
        sceMpegUnRegistStream(mpeg, video_stream);
    if (mpeg_ready)
        sceMpegDelete(&mpeg);
    if (rb_ready)
        sceMpegRingbufferDestruct(&ring);
    if (mpeginit)
        sceMpegFinish();
    if (audio_es)
        free(audio_es);
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
                      " are beside EBOOT.PBP in the game folder.");
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
