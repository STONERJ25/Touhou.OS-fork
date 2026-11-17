"""The random module with fixed seeds: the Mersenne Twister's output (the same numbers on every system), the methods built on it, the system generator and secrets. Functions that use the
math library are printed to twelve digits (the last bits depend on the processor's variant of it)."""
import os
import random
import string


def show(label, value=""):
    print(f"{label}: {value}")


def r12(x):
    return float(f"{x:.12g}")


random.seed(12345)
show("random", [random.random() for _ in range(5)])
show("getrandbits", (random.getrandbits(8), random.getrandbits(32), random.getrandbits(64), random.getrandbits(100)))
show("randint", [random.randint(1, 100) for _ in range(10)])
show("randrange", [random.randrange(0, 1000, 7) for _ in range(6)])
show("uniform", [r12(random.uniform(-5, 5)) for _ in range(4)])
show("choice", [random.choice("abcdefghij") for _ in range(8)])
deck = list(range(20))
random.shuffle(deck)
show("shuffle", deck)
show("sample", random.sample(range(1000), 8))
show("choices with weights", random.choices(["a", "b", "c"], weights=[10, 1, 5], k=12))
show("choices with cumulative weights", random.choices("xyz", cum_weights=[1, 2, 10], k=8))
show("randbytes", random.randbytes(10).hex())
show("gauss", [r12(random.gauss(0, 1)) for _ in range(4)])
show("normalvariate", [r12(random.normalvariate(10, 2)) for _ in range(3)])
show("others", [r12(f()) for f in (lambda: random.expovariate(1.5), lambda: random.betavariate(2, 3), lambda: random.gammavariate(2, 2), lambda: random.lognormvariate(0, 0.5), lambda: random.triangular(0, 10, 3), lambda: random.vonmisesvariate(1, 2), lambda: random.paretovariate(3), lambda: random.weibullvariate(1, 1.5))])
show("binomial", random.binomialvariate(20, 0.3) if hasattr(random, "binomialvariate") else "n/a")
state = random.getstate()
first = [random.random() for _ in range(3)]
random.setstate(state)
show("setstate", first == [random.random() for _ in range(3)])
random.seed("a string seed")
show("string seed", [random.randint(0, 9) for _ in range(10)])
random.seed(b"bytes seed")
show("bytes seed", random.random())
random.seed(2 ** 100 + 7)
show("big seed", random.random())
random.seed(3.25)
show("float seed", random.random())
random.seed(0)
show("seed 0", (random.random(), random.getrandbits(31)))
r1, r2 = random.Random(99), random.Random(99)
show("independent generators", ([r1.random() for _ in range(3)] == [r2.random() for _ in range(3)], random.Random(99).random() != random.Random(100).random()))
big = random.Random(2026)
total = 0
for _ in range(200000):
    total += big.getrandbits(16)
show("sum of 200000 draws", total)
show("passwords", ["".join(random.Random(n).choice(string.ascii_letters + string.digits) for _ in range(12)) for n in range(3)])
sr = random.SystemRandom()
show("system random", (0 <= sr.random() < 1, len(sr.randbytes(5)), sr.randint(5, 5), len({sr.getrandbits(64) for _ in range(20)})))
show("os.urandom", (len(os.urandom(32)), os.urandom(16) != os.urandom(16)))
try:
    random.sample(range(5), 10)
except ValueError as e:
    show("sample too large", e)
try:
    random.choice([])
except IndexError as e:
    show("choice of nothing", e)
try:
    random.randrange(5, 5)
except ValueError as e:
    show("empty range", e)
print("--- done")
