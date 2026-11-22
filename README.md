# LittleBigPlanet PSP: Alpha

This repository contains a bootable PSP homebrew prototype and an in-game intro-video player.

## What the workflow builds

On every push or pull request to `main`, and when started manually, GitHub Actions:

1. Checks the presentation PNGs and source intro video.
2. Re-encodes the intro as a 480×272 H.264 Baseline PSMF/PMF cutscene for the PSP Media Engine.
3. Converts the source soundtrack to 44.1 kHz stereo 16-bit PCM. The EBOOT streams this alongside the video and synchronizes audio to video presentation timestamps.
4. Compiles and packages `psp/EBOOT.PBP` with PSPSDK.
5. Validates the PBP structure and assembles a Memory Stick installation folder.

The workflow is in [Build LBPPSP Alpha](.github/workflows/build-psp.yml).

## Install on a PSP or PPSSPP

Copy the generated `dist/PSP` folder to the root of the Memory Stick. The folder should contain:

- `PSP/GAME/LBPPSP_ALPHA/EBOOT.PBP` — the bootable application.
- `PSP/GAME/LBPPSP_ALPHA/INTRO.PMF` — the in-game H.264 cutscene.
- `PSP/GAME/LBPPSP_ALPHA/INTRO.PCM` — the synchronized 44.1 kHz stereo soundtrack.

Launch the application under **Game → Memory Stick**, then select **Play Intro Video**. Press **X** to pause/resume and **O** to return to the menu.

The PMF is video-only; the application streams the PCM soundtrack through the PSP audio output on a separate thread and schedules frames against the audio sample clock. Both media files must remain beside `EBOOT.PBP`.

## Current project scope

The EBOOT is a bootable menu/prototype shell with controller input, credits, and an in-game intro player. **It does not contain the original LittleBigPlanet gameplay systems.** The repository currently contains presentation/media assets, not the source code for the original game.

## Presentation assets

The EBOOT package embeds:

- `LBPPSP_Alpha_ICON0.png` as the XMB icon.
- `LBPPSP_Alpha_LoadingScreen.png` as PIC0.
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
docker run --rm -v "$PWD:/workspace" -w /workspace/psp pspdev/pspdev:latest make clean
docker run --rm -v "$PWD:/workspace" -w /workspace/psp pspdev/pspdev:latest make
python scripts/validate_pbp.py psp/EBOOT.PBP
```

The generated executable is `psp/EBOOT.PBP`.
