# LittleBigPlanet PSP: Alpha

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

The AVC decoder uses the PSP's low-resolution hardware path for this 480x272 video. It queries MPEG memory with mode 4, creates a 4 MiB-aligned 2 MiB Media Engine workspace, passes that workspace to sceMpegCreate, and places the AVC access-unit buffer at workspace + 0x10000. The AU descriptor is 64-byte aligned and initialized with 0xFF sentinels, following the established PMPlayer Advance initialization pattern. The workspace is retained for the lifetime of the app instead of being returned to the heap during player teardown. The MPEG ringbuffer uses 512 sectors to reduce starvation while the decoder is assembling access units.

Decoded YCbCr planes are obtained with sceMpegAvcDecodeDetail2 and converted to the 8888 texture through sceMpegBaseCscAvc. The player keeps the converted texture in aligned main RAM, handles cache coherency before GU upload, and records detailed playback diagnostics. The PPSSPP-only direct-output fallback is used only if its HLE rejects the hardware-style NULL output argument. The PSMF muxer marks every GOP access-unit index entry, including each GOP's final entry, with the Sony-style 0x0080 flag and validates those flags before packaging.

Technical references used to verify the playback approach:
- PSPSDK MPEG API: https://github.com/pspdev/pspsdk/blob/master/src/mpeg/pspmpeg.h
- PPSSPP MPEG/PSMF internals: https://www.ppsspp.org/docs/development/ppsspp-internals/mpeg/
- PSPRelay PSP/PPSSPP H.264 player: https://github.com/SarahRoseLives/PSPRelay

## Install on PSP or PPSSPP

Download LBPPSP_ALPHA_build.zip from the successful GitHub Actions artifact. Extract it and copy its dist/PSP folder to the root of the Memory Stick.

The final layout must be:

    ms0:/PSP/GAME/LBPPSP_ALPHA/
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
    python scripts/build_intro_pmf.py --input build/LBPPSP_ALPHA_INTRO_SOURCE.mp4 --output build/LBPPSP_ALPHA_INTRO.PMF --audio-output build/LBPPSP_ALPHA_INTRO.PCM
    docker run --rm -v "$PWD:/workspace" -w /workspace/psp pspdev/pspdev:latest make clean
    docker run --rm -v "$PWD:/workspace" -w /workspace/psp pspdev/pspdev:latest make
    python scripts/validate_pbp.py psp/EBOOT.PBP
