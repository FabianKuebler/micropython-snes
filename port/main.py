# Frozen into the ROM by mpy-cross + mpy-tool; the first Python to run on
# the SNES (M3). Output is asserted byte-for-byte by tests/test_mpy.py.
#
# Scope is the M3 smoke set per PLAN: print, integer arithmetic, a loop, and
# exception raise/catch. The heavier constructs (recursion, methods, classes,
# generators) live in port/main_m4.py / tests/test_m4.py — green since the
# ROM-object alignment fix + split VM (DECISIONS.md 2026-07-02).
# "except ... as e" is avoided (emits DELETE_NAME, which the minimal ROM
# config legitimately omits).
print("hello from micropython on snes")

total = 0
for i in range(10):
    total += i * i
print("sum of squares:", total)

try:
    raise ValueError("boom")
except ValueError:
    print("caught exception")

print("done")
