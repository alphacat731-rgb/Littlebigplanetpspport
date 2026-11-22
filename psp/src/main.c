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
#define DRAW_BYTES (FBW * H * 4)
#define DISP_BYTES (FBW * H * 4)
#define VIDEO_TEXTURE_BYTES (FBW * 512 * 4)

typedef struct {
    SceUID file;
    unsigned int total;
    unsigned int read_bytes;
    int eof;
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
            break;
        }
        got += (unsigned int)n;
    }
    in->read_bytes += got;
    if (got != wanted || (got % SECTOR) != 0) {
        in->eof = 1;
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
    SceInt32 offset = 0, stream_size = 0, unknown = 0, decode_status = 0;
    unsigned int previous = 0;
    int audio_started = 0, frames = 0;

    memset(&input, 0, sizeof(input));
    memset(&audio, 0, sizeof(audio));
    input.file = -1;
    audio.file = -1;
    audio.thread = -1;
    audio.channel = -1;

    file = sceIoOpen(PMF_PATH, PSP_O_RDONLY, 0);
    if (file < 0) goto cleanup;
    if (sceIoRead(file, header, sizeof(header)) != (int)sizeof(header)) goto cleanup;
    if (memcmp(header, "PSMF0014", 8) != 0) goto cleanup;

    if (sceUtilityLoadAvModule(PSP_AV_MODULE_AVCODEC) < 0) goto cleanup;
    avcodec = 1;
    if (sceUtilityLoadAvModule(PSP_AV_MODULE_MPEGBASE) < 0) goto cleanup;
    mpegbase = 1;
    if (sceMpegInit() < 0) goto cleanup;
    mpeginit = 1;

    rb_size = sceMpegRingbufferQueryMemSize(PACKETS);
    /* 480x272 is the PSP low-resolution AVC path (mode 4). */
    mpeg_size = sceMpegQueryMemSize(MPEG_MODE);
    if (rb_size <= 0 || mpeg_size <= 0) goto cleanup;
    rb_mem = memalign(64, rb_size);
    mpeg_mem = memalign(64, mpeg_size);
    /* sceMpegCreate requires a 4 MiB-aligned, 2 MiB Media Engine workspace. */
    ddrtop = memalign(DDRTOP_ALIGN, DDRTOP_SIZE);
    /* The AU descriptor is 64-byte aligned and initialized with 0xFF sentinels. */
    au_mem = memalign(64, 64);
    if (!rb_mem || !mpeg_mem || !ddrtop || !au_mem) goto cleanup;
    memset(au_mem, 0xFF, 64);
    au = (SceMpegAu *)au_mem;
    au_es = (unsigned char *)ddrtop + DDRTOP_AU_OFFSET;

    input.file = file;
    if (sceMpegRingbufferConstruct(&ring, PACKETS, rb_mem, rb_size,
                                   pmf_read_callback, &input) < 0) goto cleanup;
    rb_ready = 1;
    if (sceMpegCreate(&mpeg, mpeg_mem, mpeg_size, &ring, FBW,
                      MPEG_MODE, (SceInt32)ddrtop) < 0) goto cleanup;
    mpeg_ready = 1;

    mode.iUnk0 = -1;
    mode.iPixelFormat = SCE_MPEG_AVC_FORMAT_8888;
    if (sceMpegQueryStreamOffset(&mpeg, header, &offset) < 0) goto cleanup;
    if (sceMpegQueryStreamSize(header, &stream_size) < 0) goto cleanup;
    if (offset < (int)sizeof(header) || stream_size <= 0 ||
        (stream_size % SECTOR) != 0) goto cleanup;
    if (offset > (int)sizeof(header)) {
        int skip = offset - (int)sizeof(header);
        unsigned char scratch[512];
        while (skip > 0) {
            int n = sceIoRead(file, scratch, skip > (int)sizeof(scratch) ? (int)sizeof(scratch) : skip);
            if (n <= 0) goto cleanup;
            skip -= n;
        }
    }
    input.total = (unsigned int)stream_size;
    stream = sceMpegRegistStream(&mpeg, 0, 0);
    if (!stream) goto cleanup;
    stream_ready = 1;
    if (sceMpegInitAu(&mpeg, au_es, au) < 0) goto cleanup;
    if (sceMpegAvcDecodeMode(&mpeg, &mode) < 0) goto cleanup;

    /*
     * Keep the converted RGBA texture in main RAM. A 512x512 texture is
     * required by the GU, even though the visible video is only 480x272.
     */
    frame = memalign(64, VIDEO_TEXTURE_BYTES);
    if (!frame) goto cleanup;
    memset(frame, 0, VIDEO_TEXTURE_BYTES);
    frame_uncached = (void *)(((unsigned int)frame) | 0x40000000u);
    gu_video_init();
    gu_ready = 1;

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
        if (available > 0) {
            int count = available > 32 ? 32 : available;
            if (sceMpegRingbufferPut(&ring, count, available) < 0)
                goto cleanup;
        }
        got = sceMpegGetAvcAu(&mpeg, stream, au, &unknown);
        if (got == 0) {
            int decoded = sceMpegAvcDecode(&mpeg, au, FBW, NULL, &decode_status);
            if (decoded < 0) goto cleanup;
            if (decode_status > 0) {
                unsigned int x, y;
                unsigned int *pixels;
                LbpAvcInfo *info;
                LbpAvcYuv *yuv;

                if (sceMpegAvcDecodeDetail2(&mpeg, &detail2) < 0 ||
                    !detail2 || !detail2->info_buffer || !detail2->yuv_buffer)
                    goto cleanup;

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
                    csc.width > 32 || csc.height > 32)
                    goto cleanup;

                /*
                 * The AVC engine leaves frames in YCbCr. Convert them to
                 * ABGR8888 before passing the texture to the Graphics Engine.
                 * Use an uncached alias for the Media Engine's CSC output.
                 */
                sceKernelDcacheWritebackInvalidateRange(frame, VIDEO_TEXTURE_BYTES);
                if (sceMpegBaseCscAvc(frame_uncached, 0, FBW, &csc) < 0)
                    goto cleanup;
                sceKernelDcacheInvalidateRange(frame, VIDEO_TEXTURE_BYTES);

                /* The hardware CSC writes alpha as zero; make pixels opaque. */
                pixels = (unsigned int *)frame;
                for (y = 0; y < H; ++y) {
                    for (x = 0; x < W; ++x)
                        pixels[y * FBW + x] |= 0xFF000000u;
                }
                sceKernelDcacheWritebackInvalidateRange(frame, VIDEO_TEXTURE_BYTES);

                if (!audio_started) {
                    if (pcm_start(&audio) != 0) goto cleanup;
                    audio_started = 1;
                }
                if (wait_audio(&audio, au->iPts) < 0) { result = 1; break; }
                gu_video_frame(frame);
                ++frames;
            }
        } else {
            if (input.eof && sceMpegRingbufferAvailableSize(&ring) >= PACKETS)
                break;
            sceKernelDelayThread(1000);
        }
    }
    if (frames > 0 && result == -1) result = 0;

cleanup:
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

    setup_callbacks();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    pspDebugScreenInit();

    /* Automatic boot sequence: splash (3 s), black (2 s), then intro. */
    show_startup_splash();
    startup_status = play_intro();
    if (startup_status < 0) {
        draw_page("INTRO PLAYER ERROR",
                  "The automatic intro could not be played.\n"
                  "Check that INTRO.PMF and INTRO.PCM are beside EBOOT.PBP.");
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
