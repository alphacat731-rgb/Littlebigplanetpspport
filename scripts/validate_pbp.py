#!/usr/bin/env python3
"""Check PBP structure, LBPPSP2008 title, icon and full-screen XMB background."""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path


def png_dimensions(data: bytes, label: str) -> tuple[int, int]:
    if len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        raise ValueError(f"{label} is not a valid embedded PNG.")
    width, height = struct.unpack(">II", data[16:24])
    if width <= 0 or height <= 0:
        raise ValueError(f"{label} has invalid dimensions {width}x{height}.")
    return width, height


def validate_pbp(path: Path) -> tuple[int, int]:
    data = path.read_bytes()
    if len(data) < 40:
        raise ValueError("PBP is smaller than its 40-byte header.")
    if data[:4] != b"\x00PBP":
        raise ValueError("Invalid PBP magic; expected \\x00PBP.")

    version = struct.unpack_from("<I", data, 4)[0]
    offsets = struct.unpack_from("<8I", data, 8)
    if offsets[0] < 40:
        raise ValueError("First PBP section starts inside the header.")
    if any(offset > len(data) for offset in offsets):
        raise ValueError("PBP section offset points beyond the end of the file.")
    if any(left > right for left, right in zip(offsets, offsets[1:])):
        raise ValueError("PBP section offsets are not in ascending order.")

    param_sfo = data[offsets[0]:offsets[1]]
    if b"LBPPSP2008" not in param_sfo:
        raise ValueError("PARAM.SFO does not contain the XMB title LBPPSP2008.")

    icon0 = data[offsets[1]:offsets[2]]
    icon_size = png_dimensions(icon0, "ICON0.PNG")
    if icon_size != (144, 80):
        raise ValueError(f"ICON0.PNG must be 144x80, got {icon_size[0]}x{icon_size[1]}.")

    # PIC0 is an optional information overlay. Keep it empty so the loading
    # artwork cannot cover the full-screen PIC1 background in the XMB.
    pic0 = data[offsets[3]:offsets[4]]
    if pic0:
        raise ValueError(f"PIC0.PNG must be empty, found {len(pic0)} bytes.")

    pic1 = data[offsets[4]:offsets[5]]
    pic1_size = png_dimensions(pic1, "PIC1.PNG")
    if pic1_size != (480, 272):
        raise ValueError(f"PIC1.PNG must be 480x272, got {pic1_size[0]}x{pic1_size[1]}.")

    # Section 6 is DATA.PSP; section 7 begins DATA.PSAR (or marks the end).
    executable_end = offsets[7]
    executable_size = executable_end - offsets[6]
    if executable_size <= 0:
        raise ValueError("DATA.PSP is empty; this is not a bootable application package.")

    return version, executable_size


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("pbp", type=Path)
    args = parser.parse_args()

    try:
        version, executable_size = validate_pbp(args.pbp)
    except (OSError, ValueError) as exc:
        print(f"PBP validation failed: {exc}", file=sys.stderr)
        return 1

    print(
        f"PBP validated: {args.pbp} "
        f"({args.pbp.stat().st_size} bytes; version 0x{version:08X}; "
        f"DATA.PSP {executable_size} bytes; title LBPPSP2008; "
        f"ICON0 144x80; PIC0 empty; PIC1 480x272)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
