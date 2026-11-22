#!/usr/bin/env python3
"""Check the PBP header, section offsets, and embedded executable section."""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path


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
        f"PBP header valid: {args.pbp} "
        f"({args.pbp.stat().st_size} bytes; version 0x{version:08X}; "
        f"DATA.PSP {executable_size} bytes)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
