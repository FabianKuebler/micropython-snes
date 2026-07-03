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
# Second Calypsi codegen bug: after a 32-bit null-test of a function return
# (`stx t; ora t`), the accumulator holds low|high, but some codegen then
# stores A as if it were still the low word. Pattern: ora dp -> (beq/bne,
# stx allowed in between) -> sta dp without reloading A.
ORA_RE = re.compile(r"\bora\s+dp:")
A_SAFE_RE = re.compile(r"\b(lda|pla|txa|tya|adc|sbc|and|eor|asl|lsr|ror|rol|jsl|jsr)\b")
A_STORE_RE = re.compile(r"\bsta\s+")
NEUTRAL_RE = re.compile(r"\b(beq|bne|bra|stx|sty|phx|phy|ldy|ldx|cpx|cpy|rep|sep)\b")


def scan(path):
    hits = []
    func = "?"
    pending = None  # (line_no, index_value)
    ora_line = None  # line number of a live ora-null-test
    ora_branched = False  # a beq/bne was seen after the ora (=> it was a test)
    for lineno, line in enumerate(open(path), 1):
        m = FUNC_RE.match(line)
        if m and not m.group(1).startswith("`?L"):
            func = m.group(1)
        is_code = re.search(r"\\ [0-9a-f]{6} [0-9a-f]", line) is not None

        # rule 2: A-clobbering null-test followed by sta of A. Only when a
        # conditional branch sits between the ora and the sta (that is what
        # makes the ora a *test*; an immediate sta is a legitimate OR).
        if is_code:
            if ORA_RE.search(line):
                ora_line = lineno
                ora_branched = False
            elif ora_line is not None:
                if re.search(r"\b(beq|bne)\b", line):
                    ora_branched = True
                elif A_STORE_RE.search(line):
                    if ora_branched:
                        hits.append((func, ora_line, 0xFFFF + 1,
                                     "A stored after ora null-test: " + line.strip()))
                    ora_line = None
                elif NEUTRAL_RE.search(line):
                    pass  # stx/sty/loads-of-XY etc leave A intact; keep watching
                else:
                    ora_line = None  # anything else redefines/consumes A

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
            if is_code:
                pending = None
    return hits


def main(paths):
    total = 0
    fatal = 0
    for p in paths:
        try:
            hits = scan(p)
        except OSError:
            continue
        for func, ln, iv, txt in hits:
            if iv > 0xFFFF:
                print(f"{p}:{ln}: WARN ora-clobbered store in {func}: {txt}")
            else:
                print(f"{p}:{ln}: NEGATIVE-Y FAR INDEX (y=0x{iv:04x}) in {func}: {txt}")
                fatal += 1
            total += 1
    if fatal:
        print(f"check_neg_index: {fatal} fatal, {total - fatal} warning(s)")
        return 1
    if total:
        print(f"check_neg_index: {total} warning(s) (ora-clobber pattern; "
              "verify stored value is only re-tested, never used as a value)")
        return 0
    print("check_neg_index: clean")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
