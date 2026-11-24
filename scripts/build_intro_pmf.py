#!/usr/bin/env python3
"""Build and validate a video-only Sony PSMF/PMF cutscene plus PCM sidecar."""
from __future__ import annotations

import argparse
import json
import subprocess
import tempfile
from pathlib import Path

SECTOR = 2048
FPS_PTS = 3003
START_PTS = 90000
SCR_DELAY = 56850
SCR_STEP = 44237
MUX_RATE = 25000
SYSTEM_HEADER = bytes.fromhex("000001bb000c80c35180f07fb9e0fbbde308")


def run(command: list[str]) -> None:
    print("+ " + " ".join(command), flush=True)
    subprocess.run(command, check=True)


def probe(path: Path, ffprobe: str) -> dict:
    result = subprocess.run(
        [ffprobe, "-v", "error", "-show_format", "-show_streams", "-of", "json", str(path)],
        check=True, capture_output=True, text=True,
    )
    return json.loads(result.stdout)


def u16(value: int) -> bytes:
    return int(value & 0xFFFF).to_bytes(2, "big")


def u32(value: int) -> bytes:
    return int(value & 0xFFFFFFFF).to_bytes(4, "big")


def pts_bytes(prefix: int, timestamp: int) -> bytes:
    return bytes((
        (prefix << 4) | (((timestamp >> 30) & 7) << 1) | 1,
        (timestamp >> 22) & 0xFF,
        (((timestamp >> 15) & 0x7F) << 1) | 1,
        (timestamp >> 7) & 0xFF,
        ((timestamp & 0x7F) << 1) | 1,
    ))


class BitWriter:
    def __init__(self) -> None:
        self.bits: list[int] = []

    def put(self, value: int, count: int) -> None:
        for shift in range(count - 1, -1, -1):
            self.bits.append((value >> shift) & 1)

    def bytes(self) -> bytes:
        out = bytearray()
        for i in range(0, len(self.bits), 8):
            chunk = self.bits[i:i + 8]
            value = 0
            for bit in chunk:
                value = (value << 1) | bit
            value <<= 8 - len(chunk)
            out.append(value)
        return bytes(out)


def pack_header(scr27: int) -> bytes:
    base, ext = divmod(scr27, 300)
    bits = BitWriter()
    bits.put(0b01, 2)
    bits.put((base >> 30) & 7, 3)
    bits.put(1, 1)
    bits.put((base >> 15) & 0x7FFF, 15)
    bits.put(1, 1)
    bits.put(base & 0x7FFF, 15)
    bits.put(1, 1)
    bits.put(ext, 9)
    bits.put(1, 1)
    bits.put(MUX_RATE, 22)
    bits.put(1, 1)
    bits.put(1, 1)
    bits.put(0x1F, 5)
    bits.put(0, 3)
    result = b"\x00\x00\x01\xba" + bits.bytes()
    if len(result) != 14:
        raise AssertionError("MPEG-2 pack header must be 14 bytes")
    return result


def padding_packet(total_size: int) -> bytes:
    if total_size == 0:
        return b""
    if total_size < 6:
        raise ValueError("padding packet is shorter than its six-byte header")
    return b"\x00\x00\x01\xbe" + u16(total_size - 6) + b"\xff" * (total_size - 6)


def video_pes_header(payload_size: int, pts: int | None, dts: int | None) -> bytes:
    if pts is None or dts is None:
        options = b"\x80\x00\x00"
    else:
        options = b"\x81\xc0\x0a" + pts_bytes(3, pts) + pts_bytes(1, dts)
    packet_length = len(options) + payload_size
    if packet_length > 0xFFFF:
        raise ValueError("video PES packet exceeds the MPEG-2 length limit")
    return b"\x00\x00\x01\xe0" + u16(packet_length) + options


def find_nals(data: bytes) -> list[tuple[int, int, int, int]]:
    """Return (start, header, end, nal_type) without rewriting source bytes."""
    found: list[tuple[int, int, int, int]] = []
    i = 0
    while i < len(data) - 3:
        if data[i:i + 4] == b"\x00\x00\x00\x01":
            start, header = i, i + 4
            i += 4
        elif data[i:i + 3] == b"\x00\x00\x01":
            start, header = i, i + 3
            i += 3
        else:
            i += 1
            continue
        if header < len(data):
            found.append((start, header, 0, data[header] & 0x1F))
    return [
        (start, header, found[n + 1][0] if n + 1 < len(found) else len(data), nal_type)
        for n, (start, header, _, nal_type) in enumerate(found)
    ]


def split_access_units(data: bytes) -> list[tuple[bytes, bool]]:
    """Split on AUD boundaries while preserving every original Annex-B byte."""
    nals = find_nals(data)
    if not nals or nals[0][0] != 0:
        raise ValueError("H.264 stream does not begin with an Annex-B start code")

    access_units: list[tuple[bytes, bool]] = []
    current_start = nals[0][0]
    current_is_idr = False
    current_has_picture = False

    for start, _header, _end, kind in nals:
        # AUD starts a new picture. Keep SPS/PPS/SEI before the first AUD
        # attached to frame one, and split only once a picture has begun.
        if kind == 9 and current_has_picture and start > current_start:
            access_units.append((data[current_start:start], current_is_idr))
            current_start = start
            current_is_idr = False
            current_has_picture = False

        if 1 <= kind <= 5:
            current_has_picture = True
        if kind == 5:
            current_is_idr = True

    if current_has_picture and current_start < len(data):
        access_units.append((data[current_start:], current_is_idr))

    if not access_units or not access_units[0][1]:
        raise ValueError("H.264 elementary stream must begin with an IDR access unit")
    if any(len(au) > 0xFFFF for au, _ in access_units):
        raise ValueError("An H.264 access unit is too large for this PMF index")
    return access_units


def private_stream2(frame_sizes: list[int]) -> bytes:
    if not frame_sizes:
        raise ValueError("GOP index cannot be empty")
    # Sony-style PSMF access-unit index entry used by established PSP PMF muxers.
    payload = bytearray(bytes.fromhex("01e0000400040005000600000000"))
    payload += u16(2 + 4 * len(frame_sizes))
    payload += u16(len(frame_sizes))
    for index, size in enumerate(frame_sizes):
        payload += u16(0x0000 if index == len(frame_sizes) - 1 else 0x0080)
        payload += u16(size)
    if len(payload) > 0xFFFF:
        raise ValueError("GOP index packet exceeds the MPEG-2 packet length limit")
    return b"\x00\x00\x01\xbf" + u16(len(payload)) + bytes(payload)


def make_psmf_header(stream_size: int, frame_count: int, width: int, height: int) -> bytes:
    header = bytearray(0x800)
    header[0:8] = b"PSMF0014"
    header[8:12] = u32(0x800)
    header[12:16] = u32(stream_size)
    end_pts = START_PTS + frame_count * FPS_PTS

    header[0x50:0x54] = u32(0x3E)
    header[0x54:0x5A] = START_PTS.to_bytes(6, "big")
    header[0x5A:0x60] = end_pts.to_bytes(6, "big")
    header[0x60:0x64] = u32(MUX_RATE)
    header[0x64:0x68] = u32(START_PTS)
    header[0x68] = 1  # one video stream
    header[0x69] = 1  # one stream descriptor
    header[0x6A:0x6E] = u32(0x24)
    header[0x70:0x74] = u32(START_PTS)
    header[0x74:0x76] = u16(0)
    header[0x76:0x7A] = u32(end_pts)
    header[0x7A:0x7C] = u16(1)
    header[0x7C:0x80] = u32(0x12)
    header[0x80:0x82] = u16(1)
    header[0x82:0x92] = bytes((
        0xE0, 0x00, 0x20, 0xFB,
        0, 0, 0, 0, 0, 0, 0, 0,
        width // 16, height // 16, 0, 0,
    ))
    return bytes(header)


def mux_video(access_units: list[tuple[bytes, bool]], width: int, height: int) -> bytes:
    keyframes = [i for i, (_, is_idr) in enumerate(access_units) if is_idr]
    if not keyframes or keyframes[0] != 0:
        raise ValueError("The first PMF access unit must be an IDR frame")
    next_key = {
        start: keyframes[index + 1] if index + 1 < len(keyframes) else len(access_units)
        for index, start in enumerate(keyframes)
    }

    sectors = bytearray()
    last_scr = -SCR_STEP
    for frame_index, (access_unit, is_idr) in enumerate(access_units):
        pts = START_PTS + frame_index * FPS_PTS
        dts = pts - FPS_PTS
        extra = b""
        if is_idr:
            gop_end = next_key[frame_index]
            sizes = [len(access_units[i][0]) for i in range(frame_index, gop_end)]
            extra = SYSTEM_HEADER + private_stream2(sizes)

        remaining = memoryview(access_unit)
        first_packet = True
        while remaining:
            pts_field = pts if first_packet else None
            dts_field = dts if first_packet else None
            empty_pes = video_pes_header(0, pts_field, dts_field)
            capacity = SECTOR - 14 - len(extra) - len(empty_pes)
            payload_size = min(len(remaining), capacity)
            spare = capacity - payload_size
            if 0 < spare < 6:
                payload_size -= 6 - spare
            if payload_size <= 0:
                raise ValueError("MPEG pack metadata leaves no room for H.264 bytes")

            payload = bytes(remaining[:payload_size])
            remaining = remaining[payload_size:]
            pes = video_pes_header(payload_size, pts_field, dts_field)
            padding = padding_packet(SECTOR - 14 - len(extra) - len(pes) - payload_size)
            body = extra + pes + payload + padding
            if len(body) != SECTOR - 14:
                raise AssertionError(f"PSMF sector body has {len(body)} bytes, expected {SECTOR - 14}")

            target_scr = max(0, (dts - SCR_DELAY) * 300)
            if first_packet:
                if last_scr + SCR_STEP > target_scr:
                    last_scr += SCR_STEP
                else:
                    last_scr = target_scr
            else:
                last_scr += SCR_STEP
            sectors += pack_header(last_scr) + body
            extra = b""
            first_packet = False

    if not sectors or len(sectors) % SECTOR:
        raise ValueError("Program stream is not aligned to 2048-byte MPEG packs")
    return make_psmf_header(len(sectors), len(access_units), width, height) + bytes(sectors)



def extract_h264_from_psmf(data: bytes) -> bytes:
    if len(data) < 0x800 or data[:8] != b"PSMF0014":
        raise ValueError("Cannot extract AVC from a non-PSMF file")
    stream = data[0x800:]
    if len(stream) % SECTOR:
        raise ValueError("PSMF stream is not sector aligned")
    recovered = bytearray()
    for sector_number, offset in enumerate(range(0, len(stream), SECTOR)):
        sector = stream[offset:offset + SECTOR]
        if sector[:4] != b"\x00\x00\x01\xba":
            raise ValueError(f"Missing pack header in sector {sector_number}")
        body = sector[14:]
        pes = body.find(b"\x00\x00\x01\xe0")
        if pes < 0 or pes + 9 > len(body):
            raise ValueError(f"No complete video PES in sector {sector_number}")
        packet_length = int.from_bytes(body[pes + 4:pes + 6], "big")
        header_data_length = body[pes + 8]
        payload_length = packet_length - 3 - header_data_length
        payload_start = pes + 9 + header_data_length
        payload_end = payload_start + payload_length
        if payload_length <= 0 or payload_end > len(body):
            raise ValueError(f"Invalid video PES payload length in sector {sector_number}")
        recovered += body[payload_start:payload_end]
    return bytes(recovered)

def validate_psmf(path: Path, access_units: list[tuple[bytes, bool]], width: int, height: int) -> None:
    data = path.read_bytes()
    if len(data) < 0x800 + SECTOR or data[:8] != b"PSMF0014":
        raise ValueError("Missing or truncated PSMF0014 header")
    header = data[:0x800]
    stream_size = int.from_bytes(header[12:16], "big")
    end_pts = START_PTS + len(access_units) * FPS_PTS
    expected_descriptor = bytes((
        0xE0, 0, 0x20, 0xFB, 0, 0, 0, 0, 0, 0, 0, 0,
        width // 16, height // 16, 0, 0,
    ))
    checks = [
        (int.from_bytes(header[8:12], "big") == 0x800, "invalid stream offset"),
        (stream_size == len(data) - 0x800, "stream size mismatch"),
        (stream_size > 0 and stream_size % SECTOR == 0, "stream is not sector aligned"),
        (header[0x50:0x54] == u32(0x3E), "invalid PSMF fixed field"),
        (header[0x54:0x5A] == START_PTS.to_bytes(6, "big"), "invalid first PTS"),
        (header[0x5A:0x60] == end_pts.to_bytes(6, "big"), "invalid final PTS"),
        (header[0x60:0x64] == u32(MUX_RATE), "invalid mux rate"),
        (header[0x64:0x68] == u32(START_PTS), "invalid stream timestamp"),
        (header[0x68:0x6A] == b"\x01\x01", "invalid video stream count"),
        (header[0x6A:0x6E] == u32(0x24), "invalid stream-table header"),
        (header[0x70:0x74] == u32(START_PTS), "invalid stream-table start timestamp"),
        (header[0x74:0x76] == u16(0), "invalid stream-table timestamp padding"),
        (header[0x76:0x7A] == u32(end_pts), "invalid stream-table end timestamp"),
        (header[0x7A:0x7C] == u16(1), "invalid stream-table descriptor count"),
        (header[0x7C:0x80] == u32(0x12), "invalid stream-table descriptor size"),
        (header[0x80:0x82] == u16(1), "expected one video descriptor"),
        (header[0x82:0x92] == expected_descriptor, "invalid video descriptor/P-STD size"),
    ]
    for passed, message in checks:
        if not passed:
            raise ValueError("PSMF validation failed: " + message)

    stream = data[0x800:]
    for offset in range(0, len(stream), SECTOR):
        if stream[offset:offset + 4] != b"\x00\x00\x01\xba":
            raise ValueError(f"Missing MPEG pack start code in sector {offset // SECTOR}")

    first = stream[:SECTOR]
    if first[14:14 + len(SYSTEM_HEADER)] != SYSTEM_HEADER:
        raise ValueError("First MPEG pack is missing the expected Sony system header")
    index_pos = 14 + len(SYSTEM_HEADER)
    if first[index_pos:index_pos + 4] != b"\x00\x00\x01\xbf":
        raise ValueError("First MPEG pack is missing its GOP index")
    index_length = int.from_bytes(first[index_pos + 4:index_pos + 6], "big")
    index_data = first[index_pos + 6:index_pos + 6 + index_length]
    if len(index_data) != index_length or index_data[:2] != b"\x01\xe0":
        raise ValueError("First GOP index packet is malformed")
    count = int.from_bytes(index_data[16:18], "big")
    table_size = int.from_bytes(index_data[14:16], "big")
    next_idr = next((i for i in range(1, len(access_units)) if access_units[i][1]), len(access_units))
    expected_sizes = [len(access_units[i][0]) for i in range(next_idr)]
    indexed_sizes = [int.from_bytes(index_data[20 + 4 * i:22 + 4 * i], "big") for i in range(count)]
    if count != len(expected_sizes) or table_size != 2 + 4 * count or indexed_sizes != expected_sizes:
        raise ValueError("First GOP index does not match its H.264 access units")

    pes_pos = index_pos + 6 + index_length
    if first[pes_pos:pes_pos + 4] != b"\x00\x00\x01\xe0":
        raise ValueError("Video PES does not immediately follow the GOP index")
    if first[pes_pos + 6:pes_pos + 9] != b"\x81\xc0\x0a":
        raise ValueError("Video PES does not use the standard PTS/DTS header")
    print("PSMF validation passed: Sony layout, P-STD descriptor, GOP index and every 2048-byte pack")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--audio-output", type=Path, required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    args = parser.parse_args()

    source = args.input.resolve()
    target = args.output.resolve()
    audio_target = args.audio_output.resolve()
    if not source.is_file() or source.stat().st_size == 0:
        raise SystemExit(f"Clean MP4 source is missing: {source}")

    source_info = probe(source, args.ffprobe)
    streams = source_info.get("streams", [])
    video = next((s for s in streams if s.get("codec_type") == "video"), None)
    audio = next((s for s in streams if s.get("codec_type") == "audio"), None)
    if not video or video.get("codec_name") != "h264" or (video.get("width"), video.get("height")) != (480, 272):
        raise SystemExit("Input must be the freshly validated 480x272 H.264 MP4.")
    if not audio or audio.get("codec_name") != "aac":
        raise SystemExit("Input must contain the freshly encoded AAC voice track.")

    target.parent.mkdir(parents=True, exist_ok=True)
    audio_target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="lbppsp-clean-mp4-") as temp_dir:
        elementary = Path(temp_dir) / "intro.h264"
        # The MP4 was already transcoded and fully decoded. Copy its H.264
        # elementary stream through Annex-B conversion without another lossy encode.
        run([
            args.ffmpeg, "-hide_banner", "-y", "-i", str(source),
            "-map", "0:v:0", "-an", "-c:v", "copy",
            "-bsf:v", "h264_mp4toannexb", "-f", "h264", str(elementary),
        ])
        raw = elementary.read_bytes()
        # Verify the elementary stream before the custom PMF packer touches it.
        run([args.ffmpeg, "-v", "error", "-xerror", "-f", "h264",
             "-i", str(elementary), "-f", "null", "-"])
        access_units = split_access_units(raw)
        pmf_data = mux_video(access_units, 480, 272)
        target.write_bytes(pmf_data)
        validate_psmf(target, access_units, 480, 272)

    run([
        args.ffmpeg, "-hide_banner", "-y", "-i", str(source),
        "-map", "0:a:0", "-vn", "-sn", "-dn",
        "-ar", "44100", "-ac", "2", "-c:a", "pcm_s16le",
        "-f", "s16le", str(audio_target),
    ])
    recovered = extract_h264_from_psmf(target.read_bytes())
    with tempfile.TemporaryDirectory(prefix="lbppsp-pmf-verify-") as temp_dir:
        recovered_path = Path(temp_dir) / "recovered.h264"
        recovered_path.write_bytes(recovered)
        # The access-unit splitter preserves the original Annex-B byte stream.
        expected_mux_input = b"".join(access_unit for access_unit, _ in access_units)
        if recovered != expected_mux_input:
            raise SystemExit(
                "PMF H.264 payload differs from mux input: "
                f"expected={len(expected_mux_input)} recovered={len(recovered)} bytes"
            )
        run([args.ffmpeg, "-v", "error", "-xerror", "-f", "h264",
             "-i", str(recovered_path), "-f", "null", "-"])
    if not audio_target.is_file() or audio_target.stat().st_size == 0 or audio_target.stat().st_size % 4:
        raise SystemExit("PCM audio is missing, empty, or not stereo 16-bit aligned")

    print(f"PMF: {target.stat().st_size} bytes; {len(access_units)} frames; 480x272 H.264")
    print(f"PCM: {audio_target.stat().st_size} bytes; 44.1 kHz stereo signed 16-bit")
    print("Both video and audio outputs passed complete decode validation.")


if __name__ == "__main__":
    main()
