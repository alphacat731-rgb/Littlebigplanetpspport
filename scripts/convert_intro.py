#!/usr/bin/env python3
"""Convert the source intro into a conservative PSP-compatible MP4 and verify it."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_INPUT = ROOT / "LBPPSP_Alpha_Video_01_tut_vo.mp4.240p.vp9.mp4"


def run(command: list[str]) -> None:
    print("+ " + " ".join(command), flush=True)
    subprocess.run(command, check=True)


def probe(path: Path, ffprobe: str) -> dict:
    result = subprocess.run(
        [
            ffprobe,
            "-v", "error",
            "-show_entries", "format=format_name,duration:stream=codec_type,codec_name,width,height,profile",
            "-of", "json",
            str(path),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    return json.loads(result.stdout)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, default=DEFAULT_INPUT)
    parser.add_argument("--output", type=Path, default=ROOT / "build" / "LBPPSP_ALPHA_INTRO.MP4")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    args = parser.parse_args()

    source = args.input.resolve()
    output = args.output.resolve()
    if not source.is_file() or source.stat().st_size == 0:
        print(f"Input video is missing or empty: {source}", file=sys.stderr)
        return 1

    source_info = probe(source, args.ffprobe)
    source_streams = source_info.get("streams", [])
    if not any(stream.get("codec_type") == "video" for stream in source_streams):
        print("Input does not contain a video stream.", file=sys.stderr)
        return 1

    has_audio = any(stream.get("codec_type") == "audio" for stream in source_streams)
    output.parent.mkdir(parents=True, exist_ok=True)

    command = [
        args.ffmpeg,
        "-hide_banner", "-y",
        "-i", str(source),
        "-map", "0:v:0",
    ]
    if has_audio:
        command += ["-map", "0:a:0"]
    else:
        command += ["-an"]

    command += [
        "-sn", "-dn",
        "-vf",
        "scale=480:272:force_original_aspect_ratio=decrease:force_divisible_by=2,"
        "pad=480:272:(ow-iw)/2:(oh-ih)/2:black,setsar=1,format=yuv420p",
        "-r", "30000/1001",
        "-c:v", "libx264",
        "-profile:v", "baseline",
        "-level:v", "3.0",
        "-pix_fmt", "yuv420p",
        "-preset", "medium",
        "-refs", "1",
        "-bf", "0",
        "-g", "60",
        "-keyint_min", "60",
        "-x264-params", "scenecut=0:bframes=0:ref=1",
        "-b:v", "768k",
        "-maxrate", "1500k",
        "-bufsize", "2000k",
    ]
    if has_audio:
        command += [
            "-c:a", "aac",
            "-profile:a", "aac_low",
            "-b:a", "128k",
            "-ar", "44100",
            "-ac", "2",
        ]

    command += [
        "-movflags", "+faststart",
        "-f", "psp",
        str(output),
    ]
    run(command)

    output_info = probe(output, args.ffprobe)
    output_streams = output_info.get("streams", [])
    video = [stream for stream in output_streams if stream.get("codec_type") == "video"]
    audio = [stream for stream in output_streams if stream.get("codec_type") == "audio"]

    if not video:
        raise RuntimeError("Converted MP4 has no video stream.")
    if video[0].get("codec_name") != "h264":
        raise RuntimeError(f"Expected H.264 output, got {video[0].get('codec_name')!r}.")
    if (video[0].get("width"), video[0].get("height")) != (480, 272):
        raise RuntimeError(
            "Expected 480x272 video, got "
            f"{video[0].get('width')}x{video[0].get('height')}."
        )
    if has_audio and (not audio or audio[0].get("codec_name") != "aac"):
        raise RuntimeError("Expected AAC audio in the converted MP4.")

    # A complete decode catches truncated or malformed output before packaging.
    run([args.ffmpeg, "-v", "error", "-i", str(output), "-f", "null", "-"])

    print(
        f"Verified PSP MP4: {output} "
        f"({output.stat().st_size} bytes, H.264 480x272, "
        f"{'AAC audio' if audio else 'no audio'})"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, subprocess.CalledProcessError, json.JSONDecodeError, RuntimeError) as exc:
        print(f"Intro conversion failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
