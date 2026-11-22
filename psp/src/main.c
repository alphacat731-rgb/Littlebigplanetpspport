#include <pspkernel.h>
#include <pspdebug.h>
#include <pspctrl.h>

#include <stdio.h>

PSP_MODULE_INFO("LBPPSP Alpha", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(64);

typedef enum {
    SCREEN_MENU = 0,
    SCREEN_PROTOTYPE,
    SCREEN_MEDIA,
    SCREEN_CREDITS
} Screen;

static const char *menu_items[] = {
    "Start Prototype",
    "Intro Video",
    "Credits",
    "Exit"
};

static const int menu_item_count = (int)(sizeof(menu_items) / sizeof(menu_items[0]));
static int selected_item = 0;
static Screen current_screen = SCREEN_MENU;

static int exit_callback(int arg1, int arg2, void *common)
{
    (void)arg1;
    (void)arg2;
    (void)common;
    sceKernelExitGame();
    return 0;
}

static int callback_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;

    SceUID callback_id = sceKernelCreateCallback("LBPPSP Exit", exit_callback, NULL);
    if (callback_id >= 0) {
        sceKernelRegisterExitCallback(callback_id);
    }

    sceKernelSleepThreadCB();
    return 0;
}

static void setup_callbacks(void)
{
    SceUID thread_id = sceKernelCreateThread(
        "LBPPSP Callback Thread",
        callback_thread,
        0x11,
        0xFA0,
        0,
        NULL
    );

    if (thread_id >= 0) {
        sceKernelStartThread(thread_id, 0, NULL);
    }
}

static void draw_menu(void)
{
    int i;

    pspDebugScreenClear();
    pspDebugScreenSetTextColor(0xFFFFFFFF);
    pspDebugScreenSetXY(3, 2);
    pspDebugScreenPrintf("LITTLEBIGPLANET PSP: ALPHA\n");
    pspDebugScreenPrintf("----------------------------------------------\n\n");

    pspDebugScreenSetTextColor(0xFF9BE24A);
    pspDebugScreenPrintf("  A small world is loading...\n\n");

    for (i = 0; i < menu_item_count; ++i) {
        if (i == selected_item) {
            pspDebugScreenSetTextColor(0xFF9BE24A);
            pspDebugScreenPrintf("  > %s\n", menu_items[i]);
        } else {
            pspDebugScreenSetTextColor(0xFFE8E8E8);
            pspDebugScreenPrintf("    %s\n", menu_items[i]);
        }
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

static void open_selected_item(void)
{
    switch (selected_item) {
    case 0:
        current_screen = SCREEN_PROTOTYPE;
        draw_page(
            "PROTOTYPE BOOTED",
            "The PSP executable and controller input are running.\n\n"
            "This repository currently contains presentation assets, not\n"
            "the original game's source or gameplay systems.\n\n"
            "This first build is the bootable foundation."
        );
        break;

    case 1:
        current_screen = SCREEN_MEDIA;
        draw_page(
            "INTRO VIDEO",
            "The build workflow creates a PSP-compatible MP4 at:\n\n"
            "  ms0:/VIDEO/LBPPSP_ALPHA_INTRO.MP4\n\n"
            "Open it from the PSP XMB Video menu. Automatic playback\n"
            "inside this homebrew is not implemented in this first build."
        );
        break;

    case 2:
        current_screen = SCREEN_CREDITS;
        draw_page(
            "CREDITS",
            "LittleBigPlanet PSP: Alpha\n"
            "Homebrew build foundation\n\n"
            "Artwork and source media are kept in the repository."
        );
        break;

    default:
        sceKernelExitGame();
        break;
    }
}

int main(void)
{
    SceCtrlData pad;
    unsigned int previous_buttons = 0;

    setup_callbacks();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);

    pspDebugScreenInit();
    draw_menu();

    for (;;) {
        unsigned int pressed;

        sceCtrlReadBufferPositive(&pad, 1);
        pressed = pad.Buttons & ~previous_buttons;
        previous_buttons = pad.Buttons;

        if (current_screen == SCREEN_MENU) {
            int changed = 0;

            if ((pressed & PSP_CTRL_UP) && selected_item > 0) {
                --selected_item;
                changed = 1;
            } else if ((pressed & PSP_CTRL_DOWN) && selected_item < menu_item_count - 1) {
                ++selected_item;
                changed = 1;
            } else if (pressed & PSP_CTRL_CROSS) {
                open_selected_item();
            }

            if (changed) {
                draw_menu();
            }
        } else if (pressed & PSP_CTRL_CIRCLE) {
            current_screen = SCREEN_MENU;
            draw_menu();
        }

        sceKernelDelayThread(1000 * 1000 / 60);
    }

    return 0;
}
