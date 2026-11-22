#!/usr/bin/env python3
"""Build a silent, full-screen PSP PSMF/PMF cutscene from the repository intro."""
from __future__ import annotations
import argparse, json, subprocess, tempfile
from pathlib import Path

SECTOR = 2048
FPS_PTS = 3003
START_PTS = 90000
SCR_DELAY = 56850
SCR_STEP = 44237
MUX_RATE = 25000


def run(cmd: list[str]) -> None:
    print('+ ' + ' '.join(cmd), flush=True)
    subprocess.run(cmd, check=True)


def u16(v: int) -> bytes:
    return int(v & 0xffff).to_bytes(2, 'big')

def u32(v: int) -> bytes:
    return int(v & 0xffffffff).to_bytes(4, 'big')

def u48(v: int) -> bytes:
    return int(v & 0xffffffffffff).to_bytes(6, 'big')

def pts_bytes(prefix: int, ts: int) -> bytes:
    return bytes([
        (prefix << 4) | (((ts >> 30) & 7) << 1) | 1,
        (ts >> 22) & 0xff,
        (((ts >> 15) & 0x7f) << 1) | 1,
        (ts >> 7) & 0xff,
        ((ts & 0x7f) << 1) | 1,
    ])

class BitWriter:
    def __init__(self) -> None:
        self.bits: list[int] = []
    def put(self, value: int, count: int) -> None:
        for shift in range(count - 1, -1, -1):
            self.bits.append((value >> shift) & 1)
    def bytes(self) -> bytes:
        out = bytearray()
        for i in range(0, len(self.bits), 8):
            value = 0
            for bit in self.bits[i:i+8]:
                value = (value << 1) | bit
            value <<= max(0, 8 - len(self.bits[i:i+8]))
            out.append(value)
        return bytes(out)


def pack_header(scr27: int) -> bytes:
    base, ext = divmod(scr27, 300)
    b = BitWriter()
    b.put(1, 2); b.put((base >> 30) & 7, 3); b.put(1, 1)
    b.put((base >> 15) & 0x7fff, 15); b.put(1, 1)
    b.put(base & 0x7fff, 15); b.put(1, 1)
    b.put(ext, 9); b.put(1, 1); b.put(MUX_RATE, 22)
    b.put(1, 1); b.put(1, 1); b.put(0x1f, 5); b.put(0, 3)
    return b'\x00\x00\x01\xba' + b.bytes()


def padding_packet(size: int) -> bytes:
    if size == 0:
        return b''
    if size < 6:
        raise ValueError('padding packet cannot be shorter than six bytes')
    return b'\x00\x00\x01\xbe' + u16(size - 6) + b'\xff' * (size - 6)


def video_pes_header(payload_len: int, pts: int | None, dts: int | None) -> bytes:
    if pts is None or dts is None:
        opt = b'\x80\x00\x00'
    else:
        opt = b'\x81\xc0\x0a' + pts_bytes(3, pts) + pts_bytes(1, dts)
    packet_len = len(opt) + payload_len
    if packet_len > 0xffff:
        raise ValueError('PES packet exceeds MPEG-2 packet length limit')
    return b'\x00\x00\x01\xe0' + u16(packet_len) + opt


def split_nals(data: bytes) -> list[bytes]:
    starts: list[tuple[int, int]] = []
    i = 0
    while i < len(data) - 3:
        if data[i:i+4] == b'\x00\x00\x00\x01':
            starts.append((i, i+4)); i += 4
        elif data[i:i+3] == b'\x00\x00\x01':
            starts.append((i, i+3)); i += 3
        else:
            i += 1
    nals = []
    for n, (_, begin) in enumerate(starts):
        end = starts[n+1][0] if n+1 < len(starts) else len(data)
        nal = data[begin:end].rstrip(b'\x00')
        if nal:
            nals.append(nal)
    return nals


def split_access_units(raw: bytes) -> list[tuple[bytes, bool]]:
    aus: list[tuple[bytes, bool]] = []
    current = bytearray()
    is_idr = False
    for nal in split_nals(raw):
        kind = nal[0] & 0x1f
        if kind == 9 and current:
            aus.append((bytes(current), is_idr))
            current.clear(); is_idr = False
        # Preserve AUD/SPS/PPS/SEI/slice NALs for the hardware decoder.
        current += b'\x00\x00\x00\x01' + nal
        if kind == 5:
            is_idr = True
    if current:
        aus.append((bytes(current), is_idr))
    if not aus or not aus[0][1]:
        raise ValueError('H.264 stream must begin with an IDR access unit; check x264 AUD/repeat-header settings')
    return aus


SYSTEM_HEADER = bytes.fromhex('000001bb000c80c35180f07fb9e0fbbde308')

def private_stream2(frame_sizes: list[int]) -> bytes:
    payload = bytearray(bytes.fromhex('01e0000400040005000600000000'))
    payload += u16(2 + 4 * len(frame_sizes))
    payload += u16(len(frame_sizes))
    for index, size in enumerate(frame_sizes):
        payload += u16(0 if index == len(frame_sizes) - 1 else 0x80)
        payload += u16(size)
    if len(payload) > 0xffff:
        raise ValueError('GOP index table is too large')
    return b'\x00\x00\x01\xbf' + u16(len(payload)) + payload


def make_psmf_header(stream: bytes, frame_count: int, width: int, height: int) -> bytes:
    h = bytearray(0x800)
    h[0:8] = b'PSMF0014'
    h[8:12] = u32(0x800)
    h[12:16] = u32(len(stream))
    end_pts = START_PTS + frame_count * FPS_PTS
    h[0x50:0x54] = u32(0x3e)
    h[0x54:0x5a] = u48(START_PTS)
    h[0x5a:0x60] = u48(end_pts)
    h[0x60:0x64] = u32(MUX_RATE)
    h[0x64:0x68] = u32(START_PTS)
    h[0x68] = 1  # one stream, video only
    h[0x69] = 1
    h[0x6a:0x6e] = u32(0x24)
    h[0x70:0x74] = u32(START_PTS)
    h[0x74:0x76] = u16(0)
    h[0x76:0x7a] = u32(end_pts)
    h[0x7a:0x7c] = u16(1)
    h[0x7c:0x80] = u32(0x12)
    h[0x80:0x82] = u16(1)
    h[0x82] = 0xe0
    h[0x83] = 0
    h[0x84:0x86] = bytes((0x20, 0xfb))
    h[0x8e] = width // 16
    h[0x8f] = height // 16
    return bytes(h)


def mux_video(aus: list[tuple[bytes, bool]], width: int, height: int) -> bytes:
    keyframes = [i for i, (_, idr) in enumerate(aus) if idr]
    if not keyframes or keyframes[0] != 0:
        raise ValueError('first access unit must be an IDR frame')
    next_key = {start: (keyframes[n+1] if n+1 < len(keyframes) else len(aus)) for n, start in enumerate(keyframes)}
    sectors = bytearray()
    last_scr = -SCR_STEP

    for frame_index, (au, idr) in enumerate(aus):
        pts = START_PTS + frame_index * FPS_PTS
        dts = pts - FPS_PTS
        extra = b''
        if idr:
            gop_end = next_key[frame_index]
            extra = SYSTEM_HEADER + private_stream2([len(aus[n][0]) for n in range(frame_index, gop_end)])
        remaining = memoryview(au)
        first_packet = True
        while remaining:
            p_pts = pts if first_packet else None
            p_dts = dts if first_packet else None
            empty_header = video_pes_header(0, p_pts, p_dts)
            capacity = SECTOR - 14 - len(extra) - len(empty_header)
            payload_len = min(len(remaining), capacity)
            spare = capacity - payload_len
            if 0 < spare < 6:
                payload_len -= 6 - spare
            if payload_len <= 0:
                raise ValueError('PES metadata leaves no room for H.264 data')
            payload = bytes(remaining[:payload_len])
            remaining = remaining[payload_len:]
            pes = video_pes_header(payload_len, p_pts, p_dts)
            pad = padding_packet(SECTOR - 14 - len(extra) - len(pes) - payload_len)
            sector_body = extra + pes + payload + pad
            if len(sector_body) != SECTOR - 14:
                raise AssertionError(f'internal sector size mismatch: {len(sector_body)}')

            target = max(0, (dts - SCR_DELAY) * 300)
            if first_packet:
                last_scr = last_scr + SCR_STEP if last_scr + SCR_STEP > target else target
            else:
                last_scr += SCR_STEP
            sectors += pack_header(last_scr) + sector_body
            extra = b''
            first_packet = False

    return make_psmf_header(bytes(sectors), len(aus), width, height) + bytes(sectors)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument('--input', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--audio-output', type=Path, required=True)
    ap.add_argument('--ffmpeg', default='ffmpeg')
    ap.add_argument('--ffprobe', default='ffprobe')
    args = ap.parse_args()
    source = args.input.resolve(); target = args.output.resolve()
    audio_target = args.audio_output.resolve()
    if not source.is_file():
        raise SystemExit(f'Input video not found: {source}')
    probe = subprocess.run([args.ffprobe, '-v', 'error', '-show_entries', 'stream=codec_type', '-of', 'json', str(source)], check=True, capture_output=True, text=True)
    input_streams = json.loads(probe.stdout).get('streams', [])
    if not any(s.get('codec_type') == 'video' for s in input_streams):
        raise SystemExit('Input file has no video stream')
    if not any(s.get('codec_type') == 'audio' for s in input_streams):
        raise SystemExit('Input file has no audio stream; this build expects the intro voice track')
    target.parent.mkdir(parents=True, exist_ok=True)
    audio_target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='lbppsp-pmf-') as td:
        elementary = Path(td) / 'intro.h264'
        run([args.ffmpeg, '-hide_banner', '-y', '-i', str(source), '-map', '0:v:0', '-an', '-sn', '-dn',
             '-vf', 'scale=480:272:force_original_aspect_ratio=decrease:force_divisible_by=2,pad=480:272:(ow-iw)/2:(oh-ih)/2:black,setsar=1,format=yuv420p',
             '-r', '30000/1001', '-c:v', 'libx264', '-profile:v', 'baseline', '-level:v', '3.0',
             '-preset', 'medium', '-refs', '1', '-bf', '0', '-g', '60', '-keyint_min', '60',
             '-x264-params', 'aud=1:repeat-headers=1:scenecut=0:bframes=0:ref=1',
             '-b:v', '768k', '-maxrate', '1500k', '-bufsize', '2000k', '-f', 'h264', str(elementary)])
        raw = elementary.read_bytes()
        aus = split_access_units(raw)
        pmf = mux_video(aus, 480, 272)
        target.write_bytes(pmf)
        run([args.ffmpeg, '-hide_banner', '-y', '-i', str(source), '-map', '0:a:0', '-vn', '-sn', '-dn',
             '-ar', '44100', '-ac', '2', '-c:a', 'pcm_s16le', '-f', 's16le', str(audio_target)])
    # Probe the generated PSMF and force a full decode before accepting it.
    probe = subprocess.run([args.ffprobe, '-v', 'error', '-show_entries', 'format=format_name,duration,size:stream=codec_name,codec_type,width,height,profile', '-of', 'json', str(target)], check=True, capture_output=True, text=True)
    details = json.loads(probe.stdout)
    video = next((s for s in details.get('streams', []) if s.get('codec_type') == 'video'), None)
    if not video or video.get('codec_name') != 'h264' or (video.get('width'), video.get('height')) != (480, 272):
        raise SystemExit(f'Generated PMF did not probe as 480x272 H.264: {details}')
    run([args.ffmpeg, '-v', 'error', '-i', str(target), '-f', 'null', '-'])
    if not audio_target.is_file() or audio_target.stat().st_size == 0 or audio_target.stat().st_size % 4:
        raise SystemExit('PCM audio output is missing, empty, or not stereo 16-bit aligned')
    print(f"Created verified in-game intro: {target} ({target.stat().st_size} bytes; {len(aus)} frames; 480x272 H.264)")
    print(f"Created PCM audio track: {audio_target} ({audio_target.stat().st_size} bytes; 44.1 kHz stereo 16-bit)")

if __name__ == '__main__':
    main()
