# LittleBigPlanet PSP: Alpha

This repository now has a first PSP homebrew build project.

## What the workflow creates

On every push or pull request to `main`, and when started manually, GitHub Actions:

1. Checks that the three PNG presentation assets and the source video exist and can be read.
2. Converts the source intro to a PSP-compatible MP4: H.264 Baseline, 480x272, 29.97 fps, no B-frames, and AAC audio when the source has audio.
3. Builds `psp/EBOOT.PBP` with the PSPSDK Docker image.
4. Checks the PBP magic, its eight section offsets, and that the executable section is not empty.
5. Packages a Memory Stick folder layout and uploads the files as a workflow artifact.

The workflow is in [Build LBPPSP Alpha](.github/workflows/build-psp.yml).

## Install on a PSP or PPSSPP

Copy the contents of the generated `dist` folder to the root of the Memory Stick:

- `PSP/GAME/LBPPSP_ALPHA/EBOOT.PBP` is the bootable homebrew application.
- `VIDEO/LBPPSP_ALPHA_INTRO.MP4` is the converted intro video. Open it from the PSP XMB Video menu.

The current EBOOT is an interactive bootable foundation with a menu, controller input, credits, and prototype/media information screens. **It does not yet play the intro from inside the application or contain the original game's gameplay systems.** The repository currently contains presentation/media assets, not the source code for the original game.

## Presentation assets

The EBOOT package embeds:

- `LBPPSP_Alpha_ICON0.png` as the XMB icon.
- `LBPPSP_Alpha_LoadingScreen.png` as PIC0.
- `LBPPSP_Alpha_PIC1.png` as the XMB background.

The MP3 source named `LBPPSP_Alpha_Savedata_SND0.ogg.mp3` is preserved in the repository but is not packed as `SND0.AT3`. PSP XMB sound uses ATRAC3 data, not an MP3 file.

## Local build

The media conversion requires FFmpeg. The PSP build requires Docker and the PSPSDK image:

```bash
python scripts/validate_assets.py
python scripts/convert_intro.py
docker run --rm -v "$PWD:/workspace" -w /workspace/psp pspdev/pspdev:latest make clean
docker run --rm -v "$PWD:/workspace" -w /workspace/psp pspdev/pspdev:latest make
python scripts/validate_pbp.py psp/EBOOT.PBP
```

The generated executable is `psp/EBOOT.PBP`.
