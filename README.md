# LittleBigPlanet PSP: Alpha

This repository contains a bootable PSP homebrew prototype with an automatic startup sequence and an in-game intro-video player.

## Startup sequence

When the application launches, it automatically:

1. Displays the repository's loading-screen image for **3 seconds**.
2. Shows a black screen for **2 seconds**.
3. Starts the intro video automatically, including its synchronized soundtrack.
4. Opens the prototype menu after the intro finishes. Press **X** to pause/resume during playback or **O** to stop the intro and go to the menu.

No menu selection is required to start the video.

## What the workflow builds

On every push or pull request to `main`, and when started manually, GitHub Actions:

1. Validates the presentation PNGs and source intro video.
2. Converts the intro to a 480×272 H.264 Baseline PSMF/PMF cutscene for the PSP Media Engine.
3. Converts the source soundtrack to 44.1 kHz stereo 16-bit PCM. The EBOOT plays it through PSP audio output in sync with the video's presentation timestamps.
4. Converts the loading-screen PNG to a 512×512 RGBA texture that the PSP startup code displays.
5. Compiles and packages `psp/EBOOT.PBP` with PSPSDK, validates the PBP structure, and assembles the Memory Stick folder.

The workflow is in [Build LBPPSP Alpha](.github/workflows/build-psp.yml).

## Install on a PSP or PPSSPP

Copy the generated `dist/PSP` folder to the root of the Memory Stick. The game directory contains:

- `PSP/GAME/LBPPSP_ALPHA/EBOOT.PBP` — the bootable application.
- `PSP/GAME/LBPPSP_ALPHA/LOADING.RGBA` — the loading screen texture.
- `PSP/GAME/LBPPSP_ALPHA/INTRO.PMF` — the in-game H.264 cutscene.
- `PSP/GAME/LBPPSP_ALPHA/INTRO.PCM` — the synchronized 44.1 kHz stereo soundtrack.

Launch the application under **Game → Memory Stick**. The loading screen, two-second black interval, and intro start automatically. Keep all three media files beside `EBOOT.PBP`.

## Current project scope

The EBOOT is still a bootable prototype shell with a startup sequence, video player, controller input, and credits. **It does not contain the original LittleBigPlanet gameplay systems.** The repository currently contains presentation/media assets, not the original game's source code.

## Presentation assets

The EBOOT package embeds:

- `LBPPSP_Alpha_ICON0.png` as the XMB icon.
- `LBPPSP_Alpha_LoadingScreen.png` as PIC0 and as the in-game startup image source.
- `LBPPSP_Alpha_PIC1.png` as the XMB background.

The MP3 source named `LBPPSP_Alpha_Savedata_SND0.ogg.mp3` is preserved in the repository but is not packed as `SND0.AT3`.

## Local build

The media conversion requires FFmpeg. The PSP build requires Docker and the PSPSDK image:

```bash
python scripts/validate_assets.py
python scripts/build_intro_pmf.py \
  --input "LBPPSP_Alpha_Video_01_tut_vo.mp4.240p.vp9.mp4" \
  --output build/LBPPSP_ALPHA_INTRO.PMF \
  --audio-output build/LBPPSP_ALPHA_INTRO.PCM
ffmpeg -hide_banner -loglevel error -y \
  -i "LBPPSP_Alpha_LoadingScreen.png" \
  -vf "scale=480:272:flags=lanczos,format=rgba,pad=512:512:0:0:color=black" \
  -frames:v 1 -pix_fmt rgba -f rawvideo build/LOADING.RGBA
docker run --rm -v "$PWD:/workspace" -w /workspace/psp pspdev/pspdev:latest make clean
docker run --rm -v "$PWD:/workspace" -w /workspace/psp pspdev/pspdev:latest make
python scripts/validate_pbp.py psp/EBOOT.PBP
```

The generated executable is `psp/EBOOT.PBP`.
