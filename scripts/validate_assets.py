#!/usr/bin/env python3
"""Validate the PSP presentation images and the freshly transcoded intro MP4."""
from __future__ import annotations
import argparse, json, struct, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PNG_FILES = ("LBPPSP_Alpha_ICON0.png", "LBPPSP_Alpha_LoadingScreen.png", "LBPPSP_Alpha_PIC1.png")
AUDIO_SOURCE = "LBPPSP_Alpha_Savedata_SND0.ogg.mp3"

def png_dimensions(path: Path) -> tuple[int, int]:
    data = path.read_bytes()
    if len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        raise ValueError(f"{path.name}: invalid PNG header")
    width, height = struct.unpack(">II", data[16:24])
    if width <= 0 or height <= 0:
        raise ValueError(f"{path.name}: invalid dimensions {width}x{height}")
    return width, height

def probe_media(path: Path, ffprobe: str) -> dict:
    result = subprocess.run([ffprobe, "-v", "error", "-show_format", "-show_streams", "-of", "json", str(path)],
                            check=True, capture_output=True, text=True)
    return json.loads(result.stdout)

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--video", type=Path, default=ROOT / "build" / "LBPPSP_ALPHA_INTRO_SOURCE.mp4")
    args = parser.parse_args()
    errors = []
    for name in PNG_FILES:
        path = ROOT / name
        if not path.is_file():
            errors.append(f"Missing required image: {name}")
            continue
        try:
            w, h = png_dimensions(path)
            print(f"OK image: {name} ({w}x{h}, {path.stat().st_size} bytes)")
        except (OSError, ValueError) as exc:
            errors.append(str(exc))

    video_path = args.video.resolve()
    if not video_path.is_file() or video_path.stat().st_size == 0:
        errors.append(f"Missing or empty clean MP4 source: {video_path}")
    else:
        try:
            info = probe_media(video_path, args.ffprobe)
            streams = info.get("streams", [])
            video = next((s for s in streams if s.get("codec_type") == "video"), None)
            audio = next((s for s in streams if s.get("codec_type") == "audio"), None)
            if not video:
                errors.append("Clean MP4 has no video stream.")
            else:
                print("OK MP4 video: {} {}x{} {} {}".format(video.get("codec_name"), video.get("width"),
                      video.get("height"), video.get("profile"), video.get("pix_fmt")))
                if video.get("codec_name") != "h264": errors.append("Clean MP4 video must be H.264.")
                if (video.get("width"), video.get("height")) != (480, 272): errors.append("Clean MP4 video must be 480x272.")
                if video.get("profile") != "Constrained Baseline": errors.append("Clean MP4 video must use Constrained Baseline.")
                if video.get("pix_fmt") != "yuv420p": errors.append("Clean MP4 video must use yuv420p.")
                if video.get("has_b_frames") != 0: errors.append("Clean MP4 video must not contain B-frames.")
            if not audio:
                errors.append("Clean MP4 has no audio stream.")
            else:
                print("OK MP4 audio: {} {} Hz, {} channels".format(audio.get("codec_name"), audio.get("sample_rate"), audio.get("channels")))
                if audio.get("codec_name") != "aac": errors.append("Clean MP4 audio must be AAC.")
                if audio.get("sample_rate") != "44100" or audio.get("channels") != 2:
                    errors.append("Clean MP4 audio must be 44.1 kHz stereo.")
            if "mp4" not in info.get("format", {}).get("format_name", ""):
                errors.append("Clean source container is not MP4.")
        except (OSError, ValueError, subprocess.CalledProcessError, json.JSONDecodeError) as exc:
            errors.append(f"Could not inspect clean MP4: {exc}")

    if (ROOT / AUDIO_SOURCE).is_file():
        print("NOTE: the separate MP3/XMB asset is preserved; intro voice audio is rebuilt from the clean MP4.")
    if errors:
        print("\nAsset validation failed:", file=sys.stderr)
        for error in errors: print(" - " + error, file=sys.stderr)
        return 1
    print("All presentation assets and fresh MP4 passed validation.")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
