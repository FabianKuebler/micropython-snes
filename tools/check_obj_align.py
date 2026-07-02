#!/usr/bin/env python3
"""Fail if any MicroPython object symbol got linked at an odd address.

MICROPY_OBJ_REPR_B tags mp_obj_t values in bit 0: a pointer object whose
target sits at an odd address is indistinguishable from a small int. Calypsi
ignores aligned() on struct members/types (it only honors it on variable
definitions, Calypsi guide 11.7), so every ROM/static object definition must
carry the attribute at variable position -- this script is the safety net
that proves none was missed. Usage: check_obj_align.py build/mpy.map
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
    sys.exit(main(sys.argv[1]))
