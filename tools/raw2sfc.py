#!/usr/bin/env python3
"""Pad a Calypsi raw HiROM image to a power-of-two .sfc and fix the checksum.

The linker emits one raw image starting at $C00000. Mesen wants full banks
and a plausible header; we pad with 0xFF to the next power of two (min 128 KB)
and patch the checksum/complement at $FFDC-$FFDF so header scoring is happy.
"""
import sys


def main(src: str, dst: str) -> None:
    data = bytearray(open(src, "rb").read())
    size = 128 * 1024
    while size < len(data):
        size *= 2
    data.extend(b"\xff" * (size - len(data)))

    # checksum: sum of all bytes with checksum fields preset
    data[0xFFDC:0xFFDE] = b"\xff\xff"  # complement placeholder
    data[0xFFDE:0xFFE0] = b"\x00\x00"  # checksum placeholder
    chk = sum(data) & 0xFFFF
    data[0xFFDC] = ~chk & 0xFF
    data[0xFFDD] = (~chk >> 8) & 0xFF
    data[0xFFDE] = chk & 0xFF
    data[0xFFDF] = (chk >> 8) & 0xFF

    open(dst, "wb").write(data)
    print(f"{dst}: {len(data) // 1024} KB, checksum {chk:#06x}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
