#!/usr/bin/env python3
"""Validate the source assets required by the PSP build."""

from __future__ import annotations

import argparse
import json
import struct
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PNG_FILES = (
    "LBPPSP_Alpha_ICON0.png",
    "LBPPSP_Alpha_LoadingScreen.png",
    "LBPPSP_Alpha_PIC1.png",
)
VIDEO_FILE = "LBPPSP_Alpha_Video_01_tut_vo.mp4.240p.vp9.mp4"
AUDIO_SOURCE = "LBPPSP_Alpha_Savedata_SND0.ogg.mp3"


def png_dimensions(path: Path) -> tuple[int, int]:
    data = path.read_bytes()
    if len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path.name}: missing or invalid PNG header")
    if data[12:16] != b"IHDR":
        raise ValueError(f"{path.name}: PNG does not begin with an IHDR chunk")
    width, height = struct.unpack(">II", data[16:24])
    if width <= 0 or height <= 0:
        raise ValueError(f"{path.name}: invalid dimensions {width}x{height}")
    return width, height


def probe_video(path: Path, ffprobe: str) -> dict:
    command = [
        ffprobe,
        "-v", "error",
        "-show_entries", "stream=codec_type,codec_name,width,height",
        "-of", "json",
        str(path),
    ]
    result = subprocess.run(command, check=True, capture_output=True, text=True)
    info = json.loads(result.stdout)
    streams = info.get("streams", [])
    video = [stream for stream in streams if stream.get("codec_type") == "video"]
    if not video:
        raise ValueError(f"{path.name}: no video stream found")
    return {
        "video": video[0],
        "audio": [stream for stream in streams if stream.get("codec_type") == "audio"],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ffprobe", default="ffprobe")
    args = parser.parse_args()

    errors = []
    for name in PNG_FILES:
        path = ROOT / name
        if not path.is_file():
            errors.append(f"Missing required image: {name}")
            continue
        try:
            width, height = png_dimensions(path)
            print(f"OK image: {name} ({width}x{height}, {path.stat().st_size} bytes)")
        except (OSError, ValueError) as exc:
            errors.append(str(exc))

    video_path = ROOT / VIDEO_FILE
    if not video_path.is_file() or video_path.stat().st_size == 0:
        errors.append(f"Missing or empty source video: {VIDEO_FILE}")
    else:
        try:
            info = probe_video(video_path, args.ffprobe)
            video = info["video"]
            audio = info["audio"]
            print(
                "OK source video: "
                f"{video.get('codec_name', 'unknown')} "
                f"{video.get('width', '?')}x{video.get('height', '?')}"
            )
            if audio:
                print(f"OK source audio stream: {audio[0].get('codec_name', 'unknown')}")
            else:
                print("NOTE: source has no audio stream; the converted intro will be silent.")
        except (OSError, ValueError, subprocess.CalledProcessError, json.JSONDecodeError) as exc:
            errors.append(f"Could not inspect source video: {exc}")

    audio_path = ROOT / AUDIO_SOURCE
    if audio_path.is_file():
        print(
            f"NOTE: {AUDIO_SOURCE} is preserved as source media. "
            "It is not embedded as SND0 because PSP XMB sound requires ATRAC3, not MP3."
        )

    if errors:
        print("\nAsset validation failed:", file=sys.stderr)
        for error in errors:
            print(f" - {error}", file=sys.stderr)
        return 1

    print("All required source assets passed validation.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
