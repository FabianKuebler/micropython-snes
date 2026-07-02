# M4: the constructs that broke the monolithic VM on Calypsi (recursion,
# Python-to-Python calls, list iteration, method calls) plus classes,
# closures, generators and nested exception unwinding. Frozen into
# build/mpy4.sfc; output asserted byte-for-byte by tests/test_m4.py.
# NB: "except ... as e" avoided (DELETE_NAME is omitted by the minimal ROM
# config), same as port/main.py.
print("hello from micropython on snes")


def fact(n):
    if n <= 1:
        return 1
    return n * fact(n - 1)


print("fact(10):", fact(10))


def is_even(n):
    return True if n == 0 else is_odd(n - 1)


def is_odd(n):
    return False if n == 0 else is_even(n - 1)


print("is_even(9):", is_even(9))

xs = [10, 20, 30, 40]
total = 0
for v in xs:
    total += v
xs.append(50)
print("list sum:", total, "len:", len(xs))

print("join:", ",".join(["a", "b", "c"]))
print("lower:", "HELLO".lower())

d = {"a": 1, "b": 2}
d["c"] = d["a"] + d["b"]
print("dict c:", d["c"])


class Counter:
    def __init__(self, start):
        self.n = start

    def bump(self, k):
        self.n += k
        return self.n


c = Counter(5)
c.bump(3)
print("counter:", c.bump(2))


def adder(k):
    def add(x):
        return x + k
    return add


print("closure:", adder(7)(35))


def gen(n):
    i = 0
    while i < n:
        yield i * i
        i += 1


g = 0
for v in gen(5):
    g += v
print("gen sum:", g)


def boom():
    raise ValueError("boom")


hit = 0
try:
    try:
        boom()
    finally:
        hit = 1
except ValueError:
    print("caught nested, finally ran:", hit)

print("done")
