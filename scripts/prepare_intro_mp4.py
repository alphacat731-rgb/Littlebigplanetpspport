#!/usr/bin/env python3
"""Create a fresh, standards-compliant H.264/AAC MP4 from the supplied source."""
from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_INPUT = ROOT / "LBPPSP_Alpha_Video_01_tut_vo.mp4.240p.vp9.mp4"


def run(command: list[str]) -> None:
    print("+ " + " ".join(command), flush=True)
    subprocess.run(command, check=True)


def probe(path: Path, ffprobe: str) -> dict:
    result = subprocess.run(
        [
            ffprobe, "-v", "error", "-show_format", "-show_streams",
            "-of", "json", str(path),
        ],
        check=True, capture_output=True, text=True,
    )
    return json.loads(result.stdout)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, default=DEFAULT_INPUT)
    parser.add_argument("--output", type=Path, default=ROOT / "build" / "LBPPSP_ALPHA_INTRO_SOURCE.mp4")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    args = parser.parse_args()

    source = args.input.resolve()
    output = args.output.resolve()
    if not source.is_file() or source.stat().st_size == 0:
        raise SystemExit(f"Source video is missing or empty: {source}")

    source_info = probe(source, args.ffprobe)
    streams = source_info.get("streams", [])
    video = next((s for s in streams if s.get("codec_type") == "video"), None)
    audio = next((s for s in streams if s.get("codec_type") == "audio"), None)
    if not video:
        raise SystemExit("Source file contains no readable video stream.")
    if not audio:
        raise SystemExit("Source file contains no audio stream; the intro voice track is required.")

    output.parent.mkdir(parents=True, exist_ok=True)
    # Decode the original container, discard corrupt packets where possible,
    # then write a completely new MP4 with fresh H.264/AAC streams and indexes.
    command = [
        args.ffmpeg, "-hide_banner", "-y",
        "-fflags", "+discardcorrupt", "-err_detect", "ignore_err",
        "-i", str(source),
        "-map", "0:v:0", "-map", "0:a:0",
        "-map_metadata", "-1", "-map_chapters", "-1",
        "-vf",
        "scale=480:272:force_original_aspect_ratio=decrease:force_divisible_by=2,"
        "pad=480:272:(ow-iw)/2:(oh-ih)/2:black,setsar=1,format=yuv420p",
        "-r", "30000/1001", "-fps_mode", "cfr",
        "-c:v", "libx264", "-profile:v", "baseline", "-level:v", "3.0",
        "-pix_fmt", "yuv420p", "-preset", "medium", "-refs", "1", "-bf", "0",
        "-g", "30", "-keyint_min", "30", "-sc_threshold", "0",
        "-x264-params",
        "aud=1:repeat-headers=1:scenecut=0:bframes=0:ref=1:weightp=0:keyint=30:min-keyint=30",
        "-b:v", "768k", "-maxrate", "768k", "-bufsize", "1536k",
        "-c:a", "aac", "-profile:a", "aac_low", "-b:a", "128k",
        "-ar", "44100", "-ac", "2",
        "-movflags", "+faststart", "-f", "mp4",
        str(output),
    ]
    run(command)

    info = probe(output, args.ffprobe)
    out_streams = info.get("streams", [])
    out_video = next((s for s in out_streams if s.get("codec_type") == "video"), None)
    out_audio = next((s for s in out_streams if s.get("codec_type") == "audio"), None)
    fmt = info.get("format", {})
    checks = [
        (bool(out_video), "output has no video stream"),
        (out_video.get("codec_name") == "h264" if out_video else False, "video is not H.264"),
        (out_video.get("profile") == "Constrained Baseline" if out_video else False, "video is not Constrained Baseline"),
        ((out_video.get("width"), out_video.get("height")) == (480, 272) if out_video else False, "video is not 480x272"),
        (out_video.get("pix_fmt") == "yuv420p" if out_video else False, "video pixel format is not yuv420p"),
        (out_video.get("has_b_frames") == 0 if out_video else False, "video unexpectedly contains B-frames"),
        (bool(out_audio), "output has no audio stream"),
        (out_audio.get("codec_name") == "aac" if out_audio else False, "audio is not AAC"),
        (out_audio.get("sample_rate") == "44100" if out_audio else False, "audio is not 44.1 kHz"),
        (out_audio.get("channels") == 2 if out_audio else False, "audio is not stereo"),
        ("mp4" in fmt.get("format_name", ""), "output is not an MP4 container"),
    ]
    failed = [message for passed, message in checks if not passed]
    if failed:
        raise SystemExit("Fresh MP4 validation failed: " + "; ".join(failed))

    # Decode every output packet before this file is allowed into the PSP build.
    run([args.ffmpeg, "-v", "error", "-xerror", "-i", str(output), "-map", "0:v:0", "-an", "-f", "null", "-"])
    run([args.ffmpeg, "-v", "error", "-xerror", "-i", str(output), "-map", "0:a:0", "-vn", "-f", "null", "-"])

    output_info = {
        "source": str(source.name),
        "output": str(output.name),
        "format": fmt.get("format_name"),
        "duration": fmt.get("duration"),
        "video": {
            "codec": out_video.get("codec_name"),
            "profile": out_video.get("profile"),
            "width": out_video.get("width"),
            "height": out_video.get("height"),
            "pixel_format": out_video.get("pix_fmt"),
            "frame_rate": out_video.get("avg_frame_rate"),
            "frames": out_video.get("nb_frames"),
        },
        "audio": {
            "codec": out_audio.get("codec_name"),
            "sample_rate": out_audio.get("sample_rate"),
            "channels": out_audio.get("channels"),
        },
        "size_bytes": output.stat().st_size,
    }
    print(json.dumps(output_info, indent=2))
    print(f"Fresh MP4 created and fully decoded: {output} ({output.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
