# Frozen into the ROM by mpy-cross + mpy-tool; the first Python to run on
# the SNES. Output is asserted byte-for-byte by tests/test_mpy.py.
# NB: no "except ... as e" — that emits DELETE_NAME, which the minimal ROM
# config legitimately omits (raises NotImplementedError: opcode).
print("hello from micropython on snes")

total = 0
for i in range(10):
    total += i * i
print("sum of squares:", total)

caught = "nothing"
try:
    raise ValueError("boom")
except ValueError:
    caught = "ValueError"
print("caught:", caught)

words = ["py", "on", "65816"]
print("-".join(sorted(words)))

print("done")
