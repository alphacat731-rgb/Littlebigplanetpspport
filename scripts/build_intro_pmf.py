#!/usr/bin/env python3
"""Build a silent, full-screen PSP PSMF/PMF cutscene from the repository intro."""
from __future__ import annotations
import argparse, json, subprocess, tempfile
from pathlib import Path

SECTOR = 2048
FPS_PTS = 3003
START_PTS = 90000
SCR_DELAY = 76850
SCR_STEP = 44237
MUX_RATE = 25000
PES_EXT_PSTD = bytes.fromhex('1e6050')  # P-STD buffer bound: 80 KiB


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


def scr_bytes(scr27: int) -> bytes:
    # Sony PMF pack SCR encoding, including the required marker bits.
    base, ext = divmod(scr27, 300)
    return bytes((
        0x44 | ((base >> 27) & 0x38) | ((base >> 28) & 0x03),
        (base >> 20) & 0xff,
        ((base >> 12) & 0xf8) | 0x04 | ((base >> 13) & 0x03),
        (base >> 5) & 0xff,
        ((base << 3) & 0xf8) | 0x04 | ((ext >> 7) & 0x03),
        ((ext << 1) & 0xfe) | 1,
    ))


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


def video_pes_header(payload_len: int, pts: int | None, dts: int | None,
                     pstd: bool = False) -> bytes:
    if pts is None or dts is None:
        opt = b'\x80\x00\x00'
    elif pstd:
        # Sony PMF samples set PES_extension_flag and repeat the 80 KiB
        # P-STD bound at the first video PES of each GOP.
        opt = (b'\x81\xc1\x0d' + pts_bytes(3, pts) +
               pts_bytes(1, dts) + PES_EXT_PSTD)
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


SYSTEM_HEADER = bytes.fromhex('000001bb000c80c35180f07fb9e050bde008')

def private_stream2(frame_sizes: list[int], first_chunk: bool) -> bytes:
    # Sony-layout PSMF private_stream_2 GOP index (0xBF). The first GOP and
    # subsequent GOPs use different six-word headers; the remaining table
    # records each access unit's flags and byte length.
    if not frame_sizes:
        raise ValueError('GOP index table must contain at least one access unit')
    header_words = (0, 0, 1, 1, 0, 0) if first_chunk else (0, 1, 1, 2, 0, 0)
    payload = bytearray(b'\x01\xe0')
    for value in header_words:
        payload += u16(value)
    payload += u16(2 + 4 * len(frame_sizes))
    payload += u16(len(frame_sizes))
    for index, size in enumerate(frame_sizes):
        payload += u16(0x0000 if index == len(frame_sizes) - 1 else 0x0080)
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

    # Match the PSMF0014 field layout used by Sony's reference PMF samples.
    # The six-byte timestamps at 0x6E and 0x74 are a second stream-table
    # timestamp pair, not 32-bit fields at 0x70 and 0x76.
    h[0x50:0x54] = u32(0x3e)
    h[0x54:0x5a] = u48(START_PTS)
    h[0x5a:0x60] = u48(end_pts)
    h[0x60:0x64] = u32(MUX_RATE)
    h[0x68:0x6a] = bytes((1, 1))  # one video stream
    h[0x6a:0x6e] = u32(0x24)
    h[0x6e:0x74] = u48(START_PTS)
    h[0x74:0x7a] = u48(end_pts)
    h[0x7a:0x7c] = u16(1)
    h[0x7c:0x80] = u32(0x12)
    h[0x80:0x82] = u16(1)

    # P-STD video buffer bound: scale=0, size=0x050 (80 KiB).
    # The stream ID is MPEG video stream 0 (0xE0).
    stream_entry = bytes((
        0xe0, 0x00, 0x20, 0x50,
        0, 0, 0, 0, 0, 0, 0, 0,
        width // 16, height // 16, 0, 0,
    ))
    h[0x82:0x92] = stream_entry
    return bytes(h)


def validate_psmf_layout(path: Path, aus: list[tuple[bytes, bool]],
                         width: int, height: int) -> None:
    data = path.read_bytes()
    if len(data) < 0x800 + SECTOR or data[:8] != b'PSMF0014':
        raise ValueError('Generated file has no complete PSMF0014 header')
    h = data[:0x800]
    stream_offset = int.from_bytes(h[8:12], 'big')
    stream_size = int.from_bytes(h[12:16], 'big')
    end_pts = START_PTS + len(aus) * FPS_PTS
    expected_entry = bytes((
        0xe0, 0x00, 0x20, 0x50,
        0, 0, 0, 0, 0, 0, 0, 0,
        width // 16, height // 16, 0, 0,
    ))
    checks = (
        (stream_offset == 0x800, 'stream offset must be 0x800'),
        (stream_size == len(data) - 0x800, 'header stream size does not match file'),
        (stream_size > 0 and stream_size % SECTOR == 0, 'program stream must be sector aligned'),
        (h[0x50:0x54] == u32(0x3e), 'invalid PSMF fixed header field'),
        (h[0x54:0x5a] == u48(START_PTS), 'invalid first PSMF timestamp'),
        (h[0x5a:0x60] == u48(end_pts), 'invalid last PSMF timestamp'),
        (h[0x60:0x64] == u32(MUX_RATE), 'invalid mux rate'),
        (h[0x64:0x68] == b'\x00' * 4, 'reserved PSMF header bytes must be zero'),
        (h[0x68:0x6a] == bytes((1, 1)), 'invalid video stream counts'),
        (h[0x6a:0x6e] == u32(0x24), 'invalid PSMF stream-table header'),
        (h[0x6e:0x74] == u48(START_PTS), 'invalid stream-table first timestamp'),
        (h[0x74:0x7a] == u48(end_pts), 'invalid stream-table last timestamp'),
        (h[0x7a:0x7c] == u16(1), 'invalid PSMF stream-table count'),
        (h[0x7c:0x80] == u32(0x12), 'invalid PSMF stream-table size'),
        (h[0x80:0x82] == u16(1), 'expected exactly one stream entry'),
        (h[0x82:0x92] == expected_entry, 'invalid video stream/P-STD entry'),
    )
    for passed, message in checks:
        if not passed:
            raise ValueError(f'PSMF layout validation failed: {message}')

    stream = data[stream_offset:]
    if stream[:4] != b'\x00\x00\x01\xba':
        raise ValueError('program stream does not begin with an MPEG-2 pack')
    if len(pack_header(0)) != 14:
        raise ValueError('MPEG-2 pack header must be 14 bytes')
    pos = 14
    if stream[pos:pos + len(SYSTEM_HEADER)] != SYSTEM_HEADER:
        raise ValueError('first pack is missing the Sony-layout system header')
    pos += len(SYSTEM_HEADER)
    if stream[pos:pos + 4] != b'\x00\x00\x01\xbf':
        raise ValueError('first pack is missing the private_stream_2 GOP index')
    bf_size = int.from_bytes(stream[pos + 4:pos + 6], 'big')
    bf = stream[pos + 6:pos + 6 + bf_size]
    if len(bf) != bf_size or bf[:2] != b'\x01\xe0' or bf_size < 18:
        raise ValueError('first GOP index packet is truncated or malformed')
    table_size = int.from_bytes(bf[14:16], 'big')
    table_count = int.from_bytes(bf[16:18], 'big')
    next_idr = next((i for i in range(1, len(aus)) if aus[i][1]), len(aus))
    first_gop_sizes = [len(aus[i][0]) for i in range(next_idr)]
    if table_size != 2 + 4 * table_count or table_count != len(first_gop_sizes):
        raise ValueError('first GOP index count/length does not match its access units')
    indexed_sizes = [int.from_bytes(bf[20 + 4 * i:22 + 4 * i], 'big')
                     for i in range(table_count)]
    if indexed_sizes != first_gop_sizes:
        raise ValueError('first GOP index sizes do not match encoded access units')
    pes_pos = pos + 6 + bf_size
    if stream[pes_pos:pes_pos + 4] != b'\x00\x00\x01\xe0':
        raise ValueError('video PES does not follow the first GOP index packet')
    if stream[pes_pos + 6:pes_pos + 9] != b'\x81\xc1\x0d':
        raise ValueError('first GOP PES is missing the P-STD PES extension')
    if stream[pes_pos + 19:pes_pos + 22] != PES_EXT_PSTD:
        raise ValueError('first GOP PES contains an invalid 80 KiB P-STD bound')

    for sector in range(0, stream_size, SECTOR):
        if stream[sector:sector + 4] != b'\x00\x00\x01\xba':
            raise ValueError(f'MPEG pack start code missing at stream sector {sector // SECTOR}')
    print('PSMF layout validation: PSMF0014 fields, Sony P-STD/system header, '
          'GOP index and 2048-byte packs passed')


def mux_video(aus: list[tuple[bytes, bool]], width: int, height: int) -> bytes:
    keyframes = [i for i, (_, idr) in enumerate(aus) if idr]
    if not keyframes or keyframes[0] != 0:
        raise ValueError('first access unit must be an IDR frame')
    next_key = {start: (keyframes[n+1] if n+1 < len(keyframes) else len(aus)) for n, start in enumerate(keyframes)}
    sectors = bytearray()

    for frame_index, (au, idr) in enumerate(aus):
        pts = START_PTS + frame_index * FPS_PTS
        dts = pts - FPS_PTS
        extra = b''
        if idr:
            gop_end = next_key[frame_index]
            # The system header is emitted once at program-stream start.
            # Every IDR GOP has its own correctly typed BF access-unit index.
            extra = (SYSTEM_HEADER if frame_index == 0 else b'') + private_stream2(
                [len(aus[n][0]) for n in range(frame_index, gop_end)],
                first_chunk=(frame_index == 0),
            )
        remaining = memoryview(au)
        first_packet = True
        while remaining:
            p_pts = pts if first_packet else None
            p_dts = dts if first_packet else None
            pstd = first_packet and idr
            empty_header = video_pes_header(0, p_pts, p_dts, pstd)
            capacity = SECTOR - 14 - len(extra) - len(empty_header)
            payload_len = min(len(remaining), capacity)
            spare = capacity - payload_len
            if 0 < spare < 6:
                payload_len -= 6 - spare
            if payload_len <= 0:
                raise ValueError('PES metadata leaves no room for H.264 data')
            payload = bytes(remaining[:payload_len])
            remaining = remaining[payload_len:]
            pes = video_pes_header(payload_len, p_pts, p_dts, pstd)
            pad = padding_packet(SECTOR - 14 - len(extra) - len(pes) - payload_len)
            sector_body = extra + pes + payload + pad
            if len(sector_body) != SECTOR - 14:
                raise AssertionError(f'internal sector size mismatch: {len(sector_body)}')

            # SCR values are patched after muxing so every pack follows the
            # Sony reference cadence derived from total PTS span / pack count.
            sectors += pack_header(0) + sector_body
            extra = b''
            first_packet = False

    packet_count = len(sectors) // SECTOR
    if packet_count <= 0 or len(sectors) % SECTOR:
        raise ValueError('program stream is not a whole number of 2048-byte packs')
    scr0 = (START_PTS - FPS_PTS - SCR_DELAY) * 300
    total_pts = len(aus) * FPS_PTS
    pace = max(SCR_STEP, min(FPS_PTS * 300, total_pts * 300 // packet_count))
    for packet_index in range(packet_count):
        pack_start = packet_index * SECTOR
        sectors[pack_start + 4:pack_start + 10] = scr_bytes(scr0 + packet_index * pace)

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
        validate_psmf_layout(target, aus, 480, 272)
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
