# LBPPSP2008

This repository builds a bootable PSP homebrew prototype with an automatic startup sequence and an in-game intro-video player.

## Automatic startup

1. The loading-screen image is shown for 3 seconds.
2. The screen holds black for 2 seconds.
3. The intro video starts automatically, with its soundtrack.
4. The prototype menu appears after the intro ends.

During playback, X pauses or resumes the soundtrack clock and O stops the intro and returns to the menu. The D-pad selects menu items.

## Rebuilt media pipeline

The repository's original video input is a VP9/Opus file carrying an MP4 suffix. The build no longer feeds that mismatched container directly into the PMF muxer.

GitHub Actions now:
1. Decodes the original source and writes a fresh MP4 with H.264 Constrained Baseline video (480x272, yuv420p, no B-frames) and AAC stereo audio (44.1 kHz).
2. Fully decodes both streams and validates the new MP4 before accepting it.
3. Copies the validated H.264 stream into a Sony PSMF/PMF video container with 2048-byte MPEG packs, GOP access-unit indexes, and PSMF header/stream descriptors.
4. Extracts the fresh AAC soundtrack to 44.1 kHz stereo PCM for the homebrew audio thread.
5. Compiles the PSP executable from a clean PSPSDK build and assembles the Memory Stick package.

The clean MP4 is included in the workflow artifact for inspection. It is a build input, not the file the PSP plays. PSP sceMpeg playback uses the PMF container, not a plain MP4. The installed folder contains INTRO.PMF, INTRO.PCM, and LOADING.RGBA.

## Player implementation

The video player now follows the conventional sceMpeg path used by PSPRelay, which reports testing on a PSP-2000 running ARK-5 6.61 and in PPSSPP. It uses sceMpegQueryMemSize(0), sceMpegCreate(..., 512, 0, 0), sceMpegMallocAvcEsBuf, and sceMpegInitAu. The decoded frame is placed in eDRAM immediately after the GU draw and display buffers. sceMpegAvcDecode receives the output-buffer pointer by double indirection (&frame), matching the PSP hardware ABI used by the reference player. The previous experimental DDR-top / NULL output / custom CSC implementation has been removed. The MPEG ringbuffer uses 512 sectors.

The fresh MP4 encoder uses 480x272, yuv420p, 29.97 fps, Constrained Baseline, no B-frames, a 30-frame GOP, and a bounded 768 kbit/s video bitrate. The PMF builder rejects GOP indexes over Sony's 59-access-unit limit and marks each entry with the 0x0080 flag.

XMB resources are deliberately separated: ICON0.PNG is the supplied 144x80 game icon; PIC0.PNG is omitted so no information overlay can cover the background; PIC1.PNG is the supplied wallpaper scaled to exactly 480x272. The loading artwork is only converted to LOADING.RGBA and shown during the application's three-second startup splash.

Technical references used to verify the playback approach:
- PSPSDK MPEG API: https://github.com/pspdev/pspsdk/blob/master/src/mpeg/pspmpeg.h
- PPSSPP MPEG/PSMF internals: https://www.ppsspp.org/docs/development/ppsspp-internals/mpeg/
- PSPRelay PSP/PPSSPP H.264 player: https://github.com/SarahRoseLives/PSPRelay

## Install on PSP or PPSSPP

Download LBPPSP2008_build.zip from the successful GitHub Actions artifact. Extract it and copy its dist/PSP folder to the root of the Memory Stick.

The final layout must be:

    ms0:/PSP/GAME/LBPPSP2008/
        EBOOT.PBP
        INTRO.PMF
        INTRO.PCM
        LOADING.RGBA

Launch it under Game > Memory Stick.

If playback fails, retrieve PLAYBACK.LOG from the game folder. A fallback log is also written to ms0:/PLAYBACK.LOG.

## Project scope

This repository currently contains the bootable homebrew shell, menu, video playback code, and media/presentation assets. It does not contain the original LittleBigPlanet PSP source code or its complete gameplay systems, so this build is a bootable prototype foundation rather than a rebuilt copy of the original game.

## Local build

The media pipeline requires FFmpeg and Python 3.12. The PSP executable requires Docker and the PSPSDK image:

    python scripts/prepare_intro_mp4.py
    python scripts/validate_assets.py
    python scripts/build_intro_pmf.py --input build/LBPPSP2008_INTRO_SOURCE.mp4 --output build/LBPPSP2008_INTRO.PMF --audio-output build/LBPPSP2008_INTRO.PCM
    docker run --rm -v "$PWD:/workspace" -w /workspace/psp pspdev/pspdev:latest make clean
    docker run --rm -v "$PWD:/workspace" -w /workspace/psp pspdev/pspdev:latest make
    python scripts/validate_pbp.py psp/EBOOT.PBP
