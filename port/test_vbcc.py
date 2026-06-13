print("hello from micropython on snes")
def fact(n):
    if n <= 1:
        return 1
    return n * fact(n - 1)
print("fact(5):", fact(5))
total = 0
for v in [10, 20, 30, 40]:
    total += v
print("list sum:", total)
print("join:", ",".join(["a", "b", "c"]))
print("lower:", "HELLO".lower())
try:
    raise ValueError("boom")
except ValueError:
    print("caught exception")
print("done")
