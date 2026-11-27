#!/usr/bin/env python3
"""Build a Sony PSMF movie with H.264 video and retail-rate ATRAC3+ audio.

The encoder is ATRACDEnc with the repository's LGPL-compatible 744-byte-frame
patch.  The muxer writes one 2048-byte MPEG-2 pack per mux unit, including
the stream descriptors and AU index required by PSP sceMpeg.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import struct
import subprocess
import tempfile
import wave
from pathlib import Path

from build_intro_pmf import (
    SECTOR, FPS_PTS, START_PTS, SCR_DELAY, SCR_STEP, MUX_RATE,
    split_access_units, pack_header, padding_packet, video_pes_header,
    pts_bytes, u16, u32,
)

VIDEO_W = 480
VIDEO_H = 272
VIDEO_FRAME_TICKS = 3003
VIDEO_STREAM_ID = 0xE0
AUDIO_STREAM_ID = 0xBD
AUDIO_RATE = 44100
AUDIO_SAMPLES_PER_FRAME = 2048
AUDIO_CODEC_FRAME = 744
AUDIO_PSMF_FRAME = 8 + AUDIO_CODEC_FRAME
AUDIO_FIRST_PTS = 85069
AUDIO_LEAD_SAMPLES = 2416
ATRAC_ENCODER_DELAY = 4464
ATRAC_FACT_OFFSET = 2048
ATRAC_DECODER_SKIP = 0x170
MAX_GOP_INDEX_FRAMES = 59

SYSTEM_HEADER_AV = bytes.fromhex("000001bb000c80c35180f07fb9e0fbbde008")
AUDIO_DESCRIPTOR = bytes.fromhex("bd002004000000000000000000000202")
ATRAC_GUID = bytes.fromhex("bfaa23e958cb7144a119fffa01e4ce62")
ATRAC_CONFIG = bytes.fromhex("285c")


def run(command: list[str], *, env: dict[str, str] | None = None) -> None:
    print("+ " + " ".join(command), flush=True)
    subprocess.run(command, check=True, env=env)


def probe(path: Path, ffprobe: str, *, input_format: str | None = None) -> dict:
    command = [ffprobe, "-v", "error"]
    if input_format:
        command += ["-f", input_format]
    command += ["-show_format", "-show_streams", "-of", "json", str(path)]
    result = subprocess.run(command, check=True, capture_output=True, text=True)
    return json.loads(result.stdout)


def encode_h264(source: Path, output: Path, ffmpeg: str, ffprobe: str) -> list[tuple[bytes, bool]]:
    vf = (
        "scale=480:272:force_original_aspect_ratio=decrease:force_divisible_by=2,"
        "pad=480:272:(ow-iw)/2:(oh-ih)/2:black,setsar=1,format=yuv420p"
    )
    run([
        ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", str(source),
        "-map", "0:v:0", "-an", "-sn", "-dn", "-vf", vf,
        "-r", "30000/1001", "-fps_mode", "cfr",
        "-c:v", "libx264", "-preset", "slow",
        "-profile:v", "main", "-level:v", "3.0", "-pix_fmt", "yuv420p",
        "-refs", "1", "-bf", "0", "-g", "48", "-keyint_min", "48",
        "-sc_threshold", "0",
        "-x264-params",
        "aud=1:repeat-headers=1:scenecut=0:bframes=0:ref=1:weightp=0:nal-hrd=vbr:vbv-maxrate=1200:vbv-bufsize=1800",
        "-b:v", "900k", "-maxrate", "1200k", "-bufsize", "1800k",
        "-f", "h264", str(output),
    ])
    stream_info = probe(output, ffprobe, input_format="h264")["streams"][0]
    if (stream_info.get("codec_name") != "h264" or
        stream_info.get("width") != VIDEO_W or
        stream_info.get("height") != VIDEO_H or
        stream_info.get("pix_fmt") != "yuv420p" or
        stream_info.get("has_b_frames") != 0 or
        stream_info.get("level") != 30):
        raise ValueError(f"Unexpected PSP AVC profile: {stream_info}")
    run([ffmpeg, "-hide_banner", "-v", "error", "-xerror",
         "-f", "h264", "-i", str(output), "-f", "null", "-"])
    access_units = split_access_units(output.read_bytes())
    if not access_units or not access_units[0][1]:
        raise ValueError("AVC elementary stream does not begin with an IDR access unit.")
    return access_units


def encode_atrac3plus(source: Path, encoder: Path, ffmpeg: str,
                      temp_dir: Path) -> tuple[list[bytes], int, int]:
    """Encode stereo 44.1 kHz PCM to standard 744-byte ATRAC3+ frames."""
    input_wav = temp_dir / "atrac_input.wav"
    raw_path = temp_dir / "atrac_frames.raw"
    run([
        ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", str(source),
        "-map", "0:a:0", "-vn", "-sn", "-dn",
        "-ar", str(AUDIO_RATE), "-ac", "2", "-c:a", "pcm_s16le",
        "-sample_fmt", "s16", str(input_wav),
    ])

    with wave.open(str(input_wav), "rb") as wav:
        if (wav.getframerate(), wav.getnchannels(), wav.getsampwidth()) != (AUDIO_RATE, 2, 2):
            raise ValueError("ATRAC3+ input must be 44.1 kHz, stereo, signed 16-bit PCM.")
        original_samples = wav.getnframes()
        pcm = wav.readframes(original_samples)

    # The first PMF ATRAC3+ packet is timestamped 54.8 ms before video PTS.
    # This lead-in is used by existing PSP movie streams to align decoder preroll.
    lead = bytes(AUDIO_LEAD_SAMPLES * 2 * 2)
    pcm = lead + pcm
    source_samples = len(pcm) // 4

    # atracdenc has a measured 4464-sample encoder delay. Pad its input so the
    # final delayed samples remain available to the PSP decoder.
    fact_end_sample = source_samples + (ATRAC_ENCODER_DELAY - ATRAC_FACT_OFFSET - ATRAC_DECODER_SKIP)
    required_samples = fact_end_sample + ATRAC_FACT_OFFSET + ATRAC_DECODER_SKIP + 400
    padded_frame_count = math.ceil(required_samples / AUDIO_SAMPLES_PER_FRAME) + 1
    padded_samples = padded_frame_count * AUDIO_SAMPLES_PER_FRAME
    if padded_samples < source_samples:
        raise ValueError("Internal ATRAC padding calculation underflow.")
    pcm += bytes((padded_samples - source_samples) * 4)

    with wave.open(str(input_wav), "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(2)
        wav.setframerate(AUDIO_RATE)
        wav.writeframes(pcm)

    env = dict(os.environ)
    env["AT3P_FRAME"] = str(AUDIO_CODEC_FRAME)
    run([
        str(encoder), "-e", "atrac3plus", "--container", "raw",
        "-i", str(input_wav), "-o", str(raw_path),
    ], env=env)

    raw = raw_path.read_bytes()
    if not raw or len(raw) % AUDIO_CODEC_FRAME:
        raise ValueError(
            f"ATRAC3+ output is not aligned to {AUDIO_CODEC_FRAME}-byte frames: {len(raw)} bytes"
        )
    frames = [
        raw[i:i + AUDIO_CODEC_FRAME]
        for i in range(0, len(raw), AUDIO_CODEC_FRAME)
    ]
    if len(frames) * AUDIO_SAMPLES_PER_FRAME < required_samples:
        raise ValueError(
            "ATRAC3+ encoder omitted too much delayed tail audio: "
            f"{len(frames)} frames for {required_samples} required samples"
        )
    return frames, fact_end_sample, original_samples


def build_atrac_test_wave(frames: list[bytes], fact_samples: int) -> bytes:
    raw = b"".join(frames)
    avg_bytes = AUDIO_CODEC_FRAME * AUDIO_RATE // AUDIO_SAMPLES_PER_FRAME
    fmt = struct.pack(
        "<HHIIHHHHI",
        0xFFFE, 2, AUDIO_RATE, avg_bytes, AUDIO_CODEC_FRAME,
        0, 34, AUDIO_SAMPLES_PER_FRAME, 3,
    ) + ATRAC_GUID + struct.pack("<H", 1) + ATRAC_CONFIG + bytes(8)
    if len(fmt) != 52:
        raise AssertionError("ATRAC3+ extensible fmt chunk must be 52 bytes.")
    body = (
        b"WAVE"
        + b"fmt " + u32(len(fmt)) + fmt
        + b"fact" + u32(8) + u32(fact_samples) + u32(ATRAC_FACT_OFFSET)
        + b"data" + u32(len(raw)) + raw
    )
    return b"RIFF" + u32(len(body)) + body


def audio_frame_pts(frame_index: int) -> int:
    elapsed = (
        frame_index * AUDIO_SAMPLES_PER_FRAME * 90000 + AUDIO_RATE // 2
    ) // AUDIO_RATE
    return AUDIO_FIRST_PTS + elapsed


def wrap_atrac_frame(frame: bytes) -> bytes:
    frame_size_code = (AUDIO_CODEC_FRAME - 8) // 8
    config = (1 << 13) | (2 << 10) | frame_size_code
    return b"\x0f\xd0" + u16(config) + bytes(4) + frame


def audio_pes_header(payload_size: int, pts: int | None,
                      include_pstd: bool) -> bytes:
    if pts is None:
        options = b"\x81\x00\x00"
    elif include_pstd:
        options = b"\x81\x81\x08" + pts_bytes(2, pts) + b"\x1e\x60\x04"
    else:
        options = b"\x81\x80\x05" + pts_bytes(2, pts)
    packet_length = len(options) + payload_size
    if packet_length > 0xFFFF:
        raise ValueError("ATRAC3+ PES packet exceeds the MPEG-2 length limit.")
    return b"\x00\x00\x01\xbd" + u16(packet_length) + options


def build_private_stream2(frame_sizes: list[int]) -> bytes:
    if not frame_sizes or len(frame_sizes) > MAX_GOP_INDEX_FRAMES:
        raise ValueError(
            f"Invalid AVC GOP index length {len(frame_sizes)}; maximum is {MAX_GOP_INDEX_FRAMES}."
        )
    if any(size <= 0 or size > 0xFFFF for size in frame_sizes):
        raise ValueError("AVC access unit exceeds the Sony PSMF index size field.")
    payload = bytearray(bytes.fromhex("01e0000400040005000600000000"))
    table_size = 2 + 4 * len(frame_sizes)
    payload += u16(table_size)
    payload += u16(len(frame_sizes))
    for size in frame_sizes:
        payload += u16(0x0080)
        payload += u16(size)
    return b"\x00\x00\x01\xbf" + u16(len(payload)) + bytes(payload)


def build_video_units(access_units: list[tuple[bytes, bool]]) -> list[dict]:
    idr_indexes = [i for i, (_, is_idr) in enumerate(access_units) if is_idr]
    if not idr_indexes or idr_indexes[0] != 0:
        raise ValueError("The first AVC access unit must be an IDR frame.")
    group_ends = {
        start: (idr_indexes[n + 1] if n + 1 < len(idr_indexes) else len(access_units))
        for n, start in enumerate(idr_indexes)
    }

    units: list[dict] = []
    for frame_index, (au_data, is_idr) in enumerate(access_units):
        pts = START_PTS + frame_index * VIDEO_FRAME_TICKS
        dts = pts - VIDEO_FRAME_TICKS
        remaining = memoryview(au_data)
        first = True
        bodies: list[bytes] = []
        while remaining:
            extra = b""
            if first and is_idr:
                next_idr = group_ends[frame_index]
                sizes = [len(access_units[i][0]) for i in range(frame_index, next_idr)]
                extra = SYSTEM_HEADER_AV + build_private_stream2(sizes)

            packet_pts = pts if first else None
            packet_dts = dts if first else None
            empty_header = video_pes_header(0, packet_pts, packet_dts)
            capacity = SECTOR - 14 - len(extra) - len(empty_header)
            payload_size = min(len(remaining), capacity)
            spare = capacity - payload_size
            if 0 < spare < 6:
                payload_size -= 6 - spare
            if payload_size <= 0:
                raise ValueError("PSMF video pack leaves no room for H.264 data.")

            payload = bytes(remaining[:payload_size])
            remaining = remaining[payload_size:]
            pes_header = video_pes_header(payload_size, packet_pts, packet_dts)
            padding = padding_packet(SECTOR - 14 - len(extra) - len(pes_header) - payload_size)
            body = extra + pes_header + payload + padding
            if len(body) != SECTOR - 14:
                raise AssertionError("Video MPEG pack is not exactly 2048 bytes.")
            bodies.append(body)
            first = False
        units.append({"timestamp": dts, "video": True, "bodies": bodies})
    return units


def build_audio_units(frames: list[bytes], pstd_after: list[int]) -> list[dict]:
    audio_data = b"".join(wrap_atrac_frame(frame) for frame in frames)
    pending_pstd = sorted(pstd_after)
    pstd_index = 0
    units: list[dict] = []
    offset = 0

    while offset < len(audio_data):
        remaining = len(audio_data) - offset
        frame_remainder = offset % AUDIO_PSMF_FRAME
        next_frame_offset = 0 if frame_remainder == 0 else AUDIO_PSMF_FRAME - frame_remainder
        next_frame_exists = next_frame_offset < remaining
        candidate_pts = (
            audio_frame_pts((offset + next_frame_offset) // AUDIO_PSMF_FRAME)
            if next_frame_exists else None
        )

        candidate_pstd_index = pstd_index
        if candidate_pts is not None:
            while (candidate_pstd_index < len(pending_pstd) and
                   pending_pstd[candidate_pstd_index] <= candidate_pts):
                candidate_pstd_index += 1
        candidate_has_pstd = (offset == 0) or (candidate_pstd_index > pstd_index)

        candidate_header = audio_pes_header(4, candidate_pts, candidate_has_pstd)
        candidate_capacity = SECTOR - 14 - len(candidate_header) - 4
        candidate_size = min(remaining, candidate_capacity)
        candidate_spare = candidate_capacity - candidate_size
        if 0 < candidate_spare < 6:
            candidate_size -= 6 - candidate_spare
        has_frame_start = (
            candidate_pts is not None and next_frame_offset < candidate_size
        )

        pts = candidate_pts if has_frame_start else None
        include_pstd = candidate_has_pstd if has_frame_start else False
        if has_frame_start:
            pstd_index = candidate_pstd_index
        deadline = candidate_pts if candidate_pts is not None else audio_frame_pts(len(frames))

        prefix_value = next_frame_offset if pts is not None else 0xFFFF
        stream_prefix = b"\x00\x00" + u16(prefix_value)
        empty_header = audio_pes_header(len(stream_prefix), pts, include_pstd)
        capacity = SECTOR - 14 - len(empty_header) - len(stream_prefix)
        payload_size = min(remaining, capacity)
        if pts is None and next_frame_exists:
            payload_size = min(payload_size, next_frame_offset)
        spare = capacity - payload_size
        if 0 < spare < 6:
            payload_size -= 6 - spare
        if payload_size <= 0:
            raise ValueError("PSMF ATRAC3+ pack leaves no room for audio data.")

        payload = audio_data[offset:offset + payload_size]
        pes_header = audio_pes_header(len(stream_prefix) + len(payload), pts, include_pstd)
        padding = padding_packet(
            SECTOR - 14 - len(pes_header) - len(stream_prefix) - len(payload)
        )
        body = pes_header + stream_prefix + payload + padding
        if len(body) != SECTOR - 14:
            raise AssertionError("Audio MPEG pack is not exactly 2048 bytes.")
        units.append({"timestamp": deadline, "video": False, "bodies": [body]})
        offset += payload_size
    return units


def make_psmf_header(stream_size: int, access_unit_count: int,
                     audio_frame_count: int) -> bytes:
    start_pts = START_PTS
    video_end = start_pts + access_unit_count * VIDEO_FRAME_TICKS
    audio_end = audio_frame_pts(audio_frame_count)
    end_pts = max(video_end, audio_end)
    hdr = bytearray(0x800)
    hdr[:8] = b"PSMF0014"
    hdr[8:12] = u32(0x800)
    hdr[12:16] = u32(stream_size)
    extra = 0x10  # second stream descriptor for ATRAC3+
    hdr[0x50:0x54] = u32(0x3E + extra)
    hdr[0x54:0x5A] = start_pts.to_bytes(6, "big")
    hdr[0x5A:0x60] = end_pts.to_bytes(6, "big")
    hdr[0x60:0x64] = u32(MUX_RATE)
    hdr[0x64:0x68] = u32(start_pts)
    hdr[0x68] = 2  # AVC + ATRAC3+
    hdr[0x69] = 1
    hdr[0x6A:0x6E] = u32(0x24 + extra)
    hdr[0x70:0x74] = u32(start_pts)
    hdr[0x74:0x76] = u16(0)
    hdr[0x76:0x7A] = u32(end_pts)
    hdr[0x7A:0x7C] = u16(1)
    hdr[0x7C:0x80] = u32(0x12 + extra)
    hdr[0x80:0x82] = u16(2)
    hdr[0x82:0x92] = bytes((
        0xE0, 0x00, 0x20, 0xFB,
        0, 0, 0, 0, 0, 0, 0, 0,
        VIDEO_W // 16, VIDEO_H // 16, 0, 0,
    ))
    hdr[0x92:0xA2] = AUDIO_DESCRIPTOR
    return bytes(hdr)


def mux_psmf(access_units: list[tuple[bytes, bool]], audio_frames: list[bytes]) -> bytes:
    video_units = build_video_units(access_units)
    idr_indexes = [i for i, (_, is_idr) in enumerate(access_units) if is_idr]
    pstd_after = [
        video_units[i]["timestamp"] for i in idr_indexes if i > 0
    ]
    audio_units = build_audio_units(audio_frames, pstd_after)

    # The first AVC unit carries the program-level system header and GOP index,
    # and is kept first in the PSMF program stream.
    ordered = [video_units[0]]
    rest = video_units[1:] + audio_units
    rest.sort(key=lambda unit: (unit["timestamp"], 0 if unit["video"] else 1))
    ordered.extend(rest)

    sectors = bytearray()
    last_scr = -SCR_STEP
    for unit in ordered:
        target_scr = max(0, (unit["timestamp"] - SCR_DELAY) * 300)
        for packet_index, body in enumerate(unit["bodies"]):
            if packet_index == 0:
                last_scr = (last_scr + SCR_STEP) if (last_scr + SCR_STEP > target_scr) else target_scr
            else:
                last_scr += SCR_STEP
            sectors.extend(pack_header(last_scr))
            sectors.extend(body)

    if not sectors or len(sectors) % SECTOR:
        raise ValueError("MPEG program stream is not 2048-byte sector aligned.")
    return make_psmf_header(len(sectors), len(access_units), len(audio_frames)) + bytes(sectors)


def extract_psmf_tracks(data: bytes) -> tuple[bytes, bytes]:
    if len(data) < 0x800 or data[:8] != b"PSMF0014":
        raise ValueError("Output does not contain a PSMF0014 header.")
    stream = data[0x800:]
    if len(stream) % SECTOR:
        raise ValueError("PSMF MPEG stream is not sector aligned.")
    video = bytearray()
    audio = bytearray()
    for sector_index, start in enumerate(range(0, len(stream), SECTOR)):
        sector = stream[start:start + SECTOR]
        if sector[:4] != b"\x00\x00\x01\xba":
            raise ValueError(f"Missing MPEG pack start code in sector {sector_index}.")
        body = sector[14:]
        pos = 0
        while pos + 6 <= len(body):
            if body[pos:pos + 3] != b"\x00\x00\x01":
                break
            stream_id = body[pos + 3]
            packet_length = int.from_bytes(body[pos + 4:pos + 6], "big")
            packet_end = pos + 6 + packet_length
            if packet_end > len(body):
                raise ValueError(f"Truncated MPEG packet in sector {sector_index}.")
            if stream_id in (VIDEO_STREAM_ID, AUDIO_STREAM_ID):
                if pos + 9 > packet_end:
                    raise ValueError("Truncated PES header.")
                header_length = body[pos + 8]
                payload_start = pos + 9 + header_length
                if payload_start > packet_end:
                    raise ValueError("Invalid PES optional-header size.")
                payload = body[payload_start:packet_end]
                if stream_id == VIDEO_STREAM_ID:
                    video.extend(payload)
                else:
                    if len(payload) < 4:
                        raise ValueError("ATRAC3+ PES packet is missing its stream prefix.")
                    audio.extend(payload[4:])
            pos = packet_end
    return bytes(video), bytes(audio)


def make_at3_wave(frames: list[bytes], fact_samples: int) -> bytes:
    raw = b"".join(frames)
    avg_bytes = AUDIO_CODEC_FRAME * AUDIO_RATE // AUDIO_SAMPLES_PER_FRAME
    fmt = struct.pack(
        "<HHIIHHHHI",
        0xFFFE, 2, AUDIO_RATE, avg_bytes, AUDIO_CODEC_FRAME, 0, 34,
        AUDIO_SAMPLES_PER_FRAME, 3,
    ) + ATRAC_GUID + struct.pack("<H", 1) + ATRAC_CONFIG + bytes(8)
    body = (
        b"WAVE"
        + b"fmt " + u32(len(fmt)) + fmt
        + b"fact" + u32(8) + u32(fact_samples) + u32(ATRAC_FACT_OFFSET)
        + b"data" + u32(len(raw)) + raw
    )
    return b"RIFF" + u32(len(body)) + body


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--encoder", type=Path, required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--test-at3-output", type=Path)
    args = parser.parse_args()

    source = args.input.resolve()
    target = args.output.resolve()
    encoder = args.encoder.resolve()
    if not source.is_file() or source.stat().st_size == 0:
        raise SystemExit(f"Input MP4 is missing or empty: {source}")
    if not encoder.is_file() or not os.access(encoder, os.X_OK):
        raise SystemExit(f"ATRACDEnc binary is missing or not executable: {encoder}")
    target.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="lbppsp2008-psmf-") as work:
        temp_dir = Path(work)
        h264_path = temp_dir / "intro.h264"
        print("Encoding PSP Main@L3.0 H.264 elementary stream...")
        access_units = encode_h264(source, h264_path, args.ffmpeg, args.ffprobe)
        idr_indexes = [i for i, (_, is_idr) in enumerate(access_units) if is_idr]
        largest_gop = max(
            (idr_indexes[i + 1] - start) if i + 1 < len(idr_indexes)
            else (len(access_units) - start)
            for i, start in enumerate(idr_indexes)
        )
        if largest_gop > MAX_GOP_INDEX_FRAMES:
            raise ValueError(f"AVC GOP has {largest_gop} frames; PSMF maximum is {MAX_GOP_INDEX_FRAMES}.")

        print("Encoding 44.1 kHz stereo ATRAC3+ at 128 kbit/s (744-byte frames)...")
        audio_frames, fact_samples, original_samples = encode_atrac3plus(
            source, encoder, args.ffmpeg, temp_dir
        )

        if args.test_at3_output:
            args.test_at3_output.parent.mkdir(parents=True, exist_ok=True)
            args.test_at3_output.write_bytes(make_at3_wave(audio_frames, fact_samples))
            run([
                args.ffmpeg, "-hide_banner", "-v", "error", "-xerror",
                "-i", str(args.test_at3_output), "-f", "null", "-",
            ])

        print("Muxing AVC + ATRAC3+ into a Sony-layout PSMF...")
        pmf = mux_psmf(access_units, audio_frames)
        target.write_bytes(pmf)

        recovered_video, recovered_audio = extract_psmf_tracks(pmf)
        expected_video = h264_path.read_bytes()
        expected_audio = b"".join(wrap_atrac_frame(frame) for frame in audio_frames)
        if recovered_video != expected_video:
            raise ValueError("H.264 elementary stream changed during PSMF muxing.")
        if recovered_audio != expected_audio:
            raise ValueError("ATRAC3+ elementary stream changed during PSMF muxing.")
        recovered_frames = [
            recovered_audio[i:i + AUDIO_PSMF_FRAME]
            for i in range(0, len(recovered_audio), AUDIO_PSMF_FRAME)
        ]
        if len(recovered_frames) != len(audio_frames):
            raise ValueError("PSMF audio frame count does not match the encoded ATRAC3+ stream.")
        for index, frame in enumerate(recovered_frames):
            if len(frame) != AUDIO_PSMF_FRAME or frame[:8] != b"\x0f\xd0\x28\x5c\x00\x00\x00\x00":
                raise ValueError(f"Invalid PSMF ATRAC3+ frame wrapper at frame {index}.")

        recovered_h264 = temp_dir / "recovered.h264"
        recovered_at3 = temp_dir / "recovered.at3"
        recovered_h264.write_bytes(recovered_video)
        raw_frames = [frame[8:] for frame in recovered_frames]
        recovered_at3.write_bytes(make_at3_wave(raw_frames, fact_samples))
        run([
            args.ffmpeg, "-hide_banner", "-v", "error", "-xerror",
            "-f", "h264", "-i", str(recovered_h264), "-f", "null", "-",
        ])
        run([
            args.ffmpeg, "-hide_banner", "-v", "error", "-xerror",
            "-i", str(recovered_at3), "-f", "null", "-",
        ])

        header = pmf[:0x800]
        if header[0x68] != 2 or int.from_bytes(header[0x80:0x82], "big") != 2:
            raise ValueError("PSMF header does not advertise both AVC and ATRAC3+ streams.")
        if header[0x92:0xA2] != AUDIO_DESCRIPTOR:
            raise ValueError("PSMF ATRAC3+ stream descriptor is invalid.")
        if pmf[12:16] != u32(len(pmf) - 0x800):
            raise ValueError("PSMF stream size header does not match the actual file.")
        if (len(pmf) - 0x800) % SECTOR:
            raise ValueError("PSMF program stream is not 2048-byte aligned.")

        print(json.dumps({
            "output": str(target),
            "bytes": target.stat().st_size,
            "video": {
                "profile": "Main",
                "width": VIDEO_W,
                "height": VIDEO_H,
                "frames": len(access_units),
                "idr_frames": len(idr_indexes),
                "largest_gop": largest_gop,
            },
            "audio": {
                "codec": "ATRAC3plus",
                "bitrate_kbps": 128,
                "frame_bytes": AUDIO_CODEC_FRAME,
                "frames": len(audio_frames),
                "source_samples": original_samples,
            },
            "container": "PSMF0014 (AVC 0xE0 + ATRAC3+ 0xBD)",
            "validation": [
                "recovered AVC payload matches mux input",
                "recovered ATRAC3+ payload matches mux input",
                "full H.264 decode of recovered stream",
                "full ATRAC3+ decode of recovered stream",
                "PSMF descriptors and 2048-byte sectors",
            ],
        }, indent=2))


if __name__ == "__main__":
    main()
