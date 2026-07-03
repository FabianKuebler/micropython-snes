#!/usr/bin/env python3
"""Fail if any MicroPython object symbol got linked at an odd address.

MICROPY_OBJ_REPR_B tags mp_obj_t values in bit 0: a pointer object whose
target sits at an odd address is indistinguishable from a small int. Calypsi
ignores aligned() on struct members/types (it only honors it on variable
definitions, Calypsi guide 11.7), so every ROM/static object definition must
carry the attribute at variable position -- this script is the safety net
that proves none was missed.

The map only names EXPORTED symbols; file-static objects (mpy-tool's frozen
const_obj_*) are invisible there, so .lst files can be passed after the map:
every const object label in them must be preceded by an `.align 2` directive
(Calypsi also ignores aligned() at variable position when the declared type
is an ANONYMOUS struct -- found the hard way when frozen tuples landed odd).

Usage: check_obj_align.py build/mpy.map [frozen_content*.lst ...]
"""
import re
import sys

# Data sections whose symbols can be mp_obj_t targets.
DATA_SECTIONS = {"cfar", "zfar", "ifar", "data", "znear", "inear", "cnear"}

# Symbol names that are (or may be) targets of mp_obj_t pointers.
OBJ_RE = re.compile(
    r"(_obj\d*$|^mp_type_|^mp_module_(?!.*_table$)|^mp_const_|_exception$|_globals$)"
)
# Anonymous C string literals (Calypsi names them after their content) are
# not objects.
EXCLUDE_RE = re.compile(r"^_StringLiteral_")

SYM_RE = re.compile(r"^(\S+) in section '(\w+)'$")
PLACED_RE = re.compile(r"^\s*placed at address ([0-9a-fA-F]+)-")

# .lst labels that are mp_obj_t targets and must be 2-aligned
LST_OBJ_RE = re.compile(r"^\s*\\ [0-9a-f]+\s+(const_obj_\S+):")
LST_ALIGN_RE = re.compile(r"^\s*\\ [0-9a-f]+\s+\.align\s+2")


def check_lst(path):
    """Return labels of const objects not preceded by .align 2."""
    bad = []
    prev_align = False
    for line in open(path):
        if LST_ALIGN_RE.match(line):
            prev_align = True
            continue
        m = LST_OBJ_RE.match(line)
        if m:
            if not prev_align:
                bad.append(m.group(1))
            prev_align = False
        elif line.strip().startswith("\\"):
            prev_align = False
    return bad


def main(path):
    bad = []
    seen = 0
    sym = sect = None
    for line in open(path):
        m = SYM_RE.match(line.strip())
        if m:
            sym, sect = m.group(1), m.group(2)
            continue
        m = PLACED_RE.match(line)
        if (m and sym and sect in DATA_SECTIONS and OBJ_RE.search(sym)
                and not EXCLUDE_RE.search(sym)):
            seen += 1
            addr = int(m.group(1), 16)
            if addr & 1:
                bad.append((sym, addr))
            sym = None
    if not seen:
        print(f"check_obj_align: no object symbols found in {path}?!")
        return 1
    if bad:
        print(f"check_obj_align: {len(bad)} object symbol(s) at ODD addresses "
              f"(REPR_B misreads them as small ints):")
        for name, addr in sorted(bad, key=lambda x: x[1]):
            print(f"  {addr:06x} {name}")
        return 1
    print(f"check_obj_align: {seen} object symbols, all even")
    return 0

if __name__ == "__main__":
    rc = main(sys.argv[1])
    for lst in sys.argv[2:]:
        bad = check_lst(lst)
        if bad:
            print(f"check_obj_align: {len(bad)} const object(s) in {lst} "
                  f"WITHOUT .align 2 (anonymous-struct aligned() drop?):")
            for name in bad:
                print(f"  {name}")
            rc = 1
        else:
            print(f"check_obj_align: {lst}: all const objects .align 2")
    sys.exit(rc)
