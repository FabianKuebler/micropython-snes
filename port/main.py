# Frozen into the ROM by mpy-cross + mpy-tool; the first Python to run on
# the SNES (M3). Output is asserted byte-for-byte by tests/test_mpy.py.
#
# Scope is the M3 smoke set per PLAN: print, integer arithmetic, a loop, and
# exception raise/catch. Method calls (str.join etc.) are deliberately NOT
# used here -- they hit an unresolved Calypsi codegen issue tracked for M4
# (see DECISIONS.md). "except ... as e" is also avoided (emits DELETE_NAME,
# which the minimal ROM config legitimately omits).
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
