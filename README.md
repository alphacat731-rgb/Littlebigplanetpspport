# LBPPSP2008

A bootable PSP homebrew prototype shell with automatic startup, an intro movie player, and the supplied XMB presentation assets.

## Automatic startup

1. Show the loading image for 3 seconds.
2. Hold a black screen for 2 seconds.
3. Start the intro movie and soundtrack automatically.
4. Show the prototype menu when both movie streams finish.

During playback, X pauses/resumes playback and O stops the movie. The D-pad selects menu items.

## PSP movie format

The PSP's `sceMpeg` movie player expects Sony's PSMF/PMF container. A plain MP4 is a conversion source, not the file that `sceMpeg` plays.

The new pipeline creates a single multiplexed PSMF movie containing:

- AVC/H.264 video on stream `0xE0`, 480x272, 29.97 fps, Main profile, yuv420p, no B-frames, fixed 48-frame GOPs, one reference frame, bounded video bitrate.
- ATRAC3+ audio on stream `0xBD`, 44.1 kHz stereo, 128 kbit/s retail frame size (744-byte codec frames).
- Sony-style 2048-byte MPEG packs, PSMF stream descriptors, video GOP access-unit indexes, audio PES timestamps, and ATRAC3+ frame wrappers.

There is no separate PCM sidecar. The soundtrack is inside `INTRO.PMF`.

## ATRAC3+ encoder

Upstream ATRACDEnc defaults to 2048-byte ATRAC3+ frames, which is not the retail-rate frame size used by this PSP movie. The build therefore pins ATRACDEnc commit `cb170709f836cb43dc0312455188c791159d81dc` and applies `scripts/atracdenc-744.patch`. The patch exposes the existing encoder frame-size setting through `AT3P_FRAME`; the build sets it to 744 bytes.

The build script is `scripts/build_atracdenc.sh`. It uses the included PCM16 WAV backend (`scripts/atracdenc_pcm_io_simple.cpp`) so local builds do not depend on libsndfile. ATRACDEnc is LGPL-2.1-or-later and its license is copied alongside the built encoder in the CI artifact. The frame-size patch and backend source are included in this repository for reproducibility.

The PSMF builder is `scripts/build_intro_psmf.py`. It re-encodes the video as a PSP-oriented H.264 elementary stream, prepares 44.1 kHz stereo PCM for ATRACDEnc, creates the retail-rate ATRAC3+ frames, and multiplexes both tracks into a PSMF file. It then extracts both tracks again, checks that their payloads are byte-identical to the mux inputs, and fully decodes the recovered H.264 and ATRAC3+ tracks with FFmpeg before accepting the movie.

## PSP decoder

The player uses the standard `sceMpeg` mode-0 pipeline used by PSPRelay:

- `sceMpegQueryMemSize(0)` and `sceMpegCreate(..., 512, 0, 0)`.
- AVC elementary-stream allocation through `sceMpegMallocAvcEsBuf`.
- Separate registrations for AVC stream 0 and ATRAC3+ stream 1.
- ATRAC3+ decoder buffer sizing through `sceMpegQueryAtracEsSize`, then `sceMpegGetAtracAu` and `sceMpegAtracDecode`.
- Four 64-byte-aligned audio output buffers, consumed on a separate audio thread.
- AVC decoding with `sceMpegGetAvcAu` and `sceMpegAvcDecode`, with the decoded frame written to eDRAM.

The player uses a 512-sector ringbuffer and a 128 KiB main thread stack. The four ATRAC output buffers are small and retained for the app lifetime because the audio DMA may still be reading the last submitted buffer.

PSPRelay documents tests on a PSP-2000 running ARK-5 6.61 and in PPSSPP. This project targets a PSP-1000 (model 1004), which has less system memory; the smaller stack and bounded queue reduce its memory footprint. This does not substitute for an actual PSP-1000 runtime test.

## XMB presentation

- XMB title: `LBPPSP2008`.
- `ICON0.PNG`: supplied custom icon at 144x80.
- `PIC0.PNG`: intentionally empty so an information overlay cannot cover the wallpaper.
- `PIC1.PNG`: supplied full-screen wallpaper at 480x272.
- The loading artwork is only converted to `LOADING.RGBA` and shown inside the application during startup.

## Install

Download `LBPPSP2008_build.zip` from the successful GitHub Actions artifact and extract its `PSP` folder to the root of the Memory Stick.

The game directory is:

    ms0:/PSP/GAME/LBPPSP2008/
        EBOOT.PBP
        INTRO.PMF
        LOADING.RGBA

Launch `LBPPSP2008` under Game > Memory Stick.

If playback fails, retrieve `PLAYBACK.LOG` from the game folder. A fallback is written to `ms0:/PLAYBACK.LOG`.

## Local build

Dependencies: FFmpeg, FFprobe, Python 3.12, CMake, a C/C++ toolchain, Docker, and the PSPSDK Docker image. The patched encoder build uses the included minimal WAV PCM16 backend, so it does not require libsndfile development headers.

    python scripts/prepare_intro_mp4.py
    python scripts/validate_assets.py
    scripts/build_atracdenc.sh build/atracdenc
    python scripts/build_intro_psmf.py --input build/LBPPSP_ALPHA_INTRO_SOURCE.mp4 --encoder build/atracdenc --output build/INTRO.PMF --test-at3-output build/ATRAC3P_TEST.at3
    docker run --rm -v "$PWD:/workspace" -w /workspace/psp pspdev/pspdev:latest make clean
    docker run --rm -v "$PWD:/workspace" -w /workspace/psp pspdev/pspdev:latest make
    python scripts/validate_pbp.py psp/EBOOT.PBP

## Project scope

This repository contains the bootable homebrew shell, menu, video player, and media/presentation pipeline. It does not contain the original LittleBigPlanet PSP gameplay source or full game engine, so this is a bootable prototype foundation rather than a reconstruction of the original commercial game.
