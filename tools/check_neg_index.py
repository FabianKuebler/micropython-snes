#!/usr/bin/env python3
"""Scan Calypsi .lst files for the negative-Y far-indexing bug.

Calypsi sometimes compiles `p[-1]` on a far pointer as `ldy ##-2` followed by
`lda [dp],y`. The 65816's indirect-long-indexed mode adds Y as an UNSIGNED
16-bit value to the full 24-bit base, so a "negative" index carries into the
bank byte and reads/writes the NEXT bank instead of just below the pointer.
Any such pair is a latent memory-corruption bug.

Usage: check_neg_index.py file.lst [file.lst ...]
Reports the containing function for each hit; exits 1 if any found.
"""
import re
import sys

LDY_RE = re.compile(r"\bldy\s+##(-\d+|0x[fF][0-9a-fA-F]{3})\b")
LONG_IDX_RE = re.compile(r"\b(lda|sta)\s+\[[^]]+\],y")
FUNC_RE = re.compile(r"^\s*\\ [0-9a-f]{6}\s+(\S+):")


def scan(path):
    hits = []
    func = "?"
    pending = None  # (line_no, index_value)
    for lineno, line in enumerate(open(path), 1):
        m = FUNC_RE.match(line)
        if m and not m.group(1).startswith("`?L"):
            func = m.group(1)
        m = LDY_RE.search(line)
        if m:
            val = m.group(1)
            iv = int(val, 0) & 0xFFFF
            if iv >= 0x8000:
                pending = (lineno, iv)
            else:
                pending = None
            continue
        if pending and LONG_IDX_RE.search(line):
            hits.append((func, pending[0], pending[1], line.strip()))
            pending = None
        elif line.strip() and "\\" in line:
            # any other instruction between ldy and the indexed access:
            # keep pending only across non-code listing lines
            if re.search(r"\\ [0-9a-f]{6} [0-9a-f]", line):
                pending = None
    return hits


def main(paths):
    total = 0
    for p in paths:
        try:
            hits = scan(p)
        except OSError:
            continue
        for func, ln, iv, txt in hits:
            print(f"{p}:{ln}: NEGATIVE-Y FAR INDEX (y=0x{iv:04x}) in {func}: {txt}")
            total += 1
    if total:
        print(f"check_neg_index: {total} latent bank-crossing access(es)")
        return 1
    print("check_neg_index: clean")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
