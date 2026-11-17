"""Arithmetic: integers of any size, floating point (the exact results of the basic operations, the library's functions to twelve digits: the last bits of exp, sin and the like
depend on the processor's variant of the math library), decimal, fractions, statistics, bit operations, conversions and the limits of the types."""
import cmath
import decimal
import fractions
import itertools
import math
import operator
import statistics
import struct
import sys


def show(label, value=""):
    print(f"{label}: {value}")


def r12(x):
    return float(f"{x:.12g}")


print("--- integers")
show("small", (7 // 2, -7 // 2, 7 % 3, -7 % 3, divmod(-7, 2), 7 / 2, 2 ** 10, 2 ** -2, pow(3, 4, 5), pow(2, -1, 7)))
show("big", (2 ** 100, 3 ** 200 % 10 ** 9, math.factorial(30), math.comb(50, 25), math.perm(20, 5)))
show("huge", len(str(math.factorial(1000))))
show("fibonacci 1000", (lambda n: (lambda f: f(f, n))(lambda f, k: (0, 1) if k == 0 else (lambda a, b: (b, a + b))(*f(f, k - 1))))(300)[0])
a = 123456789 ** 20
show("division", (a // 987654321, a % 987654321, divmod(a, -10 ** 50)[1], -a // 7))
show("gcd/lcm", (math.gcd(2 ** 64, 6 ** 30), math.lcm(12, 18, 27), math.isqrt(10 ** 40 + 12345), math.isqrt(2 ** 200)))
show("bits", (bin(0b1011 << 70), 0xFF ^ 0x0F, ~5, -8 >> 1, 1 << 100 >> 98, (2 ** 70).bit_length(), (255).bit_count(), (-1) & 0xFFFF))
show("bytes", ((2 ** 40 + 5).to_bytes(6, "big"), int.from_bytes(b"\x01\x02\x03", "little"), (-2).to_bytes(2, "big", signed=True), int.from_bytes(b"\xff\xfe", "big", signed=True)))
show("parse", (int("ff", 16), int("0b101", 0), int("  42  "), int("1_000_000"), int("z", 36), int(10 ** 30 * 3.0)))
show("format", (f"{255:x} {255:#o} {255:08b} {10 ** 20:,} {-5:+d} {5:>6} {5:<6}| {1234567:_}", format(2 ** 70, "x")))
show("str of a big number", str(7 ** 5000)[:30] + "..." + str(7 ** 5000)[-30:])
show("power tower", len(str(3 ** (3 ** 5))))
try:
    str(10 ** 5000)
except ValueError as e:
    show("digit limit", str(e)[:40])
sys.set_int_max_str_digits(0)
show("no limit", len(str(10 ** 5000)))
try:
    1 // 0
except ZeroDivisionError as e:
    show("division by zero", e)
try:
    0 ** -1
except ZeroDivisionError as e:
    show("zero to a negative power", e)

print("--- floating point")
show("basic", (0.1 + 0.2, 1 / 3, 2 / 3, 1e16 + 1, 1e-5 * 3, 5e-324, 1.7976931348623157e308, 0.1 * 3, 4.35 * 100))
show("repr", [repr(x) for x in (1.0, 1e22, 1e23, 123456789.123456789, 0.000001, 1e-7, 1.5e300, 2.5e-300, 100.0, 12345678901234567890.0)])
show("round", (round(2.5), round(3.5), round(-2.5), round(2.675, 2), round(1234.5678, -2), round(0.5), round(1.5)))
show("floor/ceil/trunc", (math.floor(-2.5), math.ceil(-2.5), math.trunc(-2.5), math.floor(2 ** 70 + 0.0), int(-2.9)))
show("special", (float("inf"), -float("inf"), float("nan") != float("nan"), math.isnan(float("nan")), math.isinf(1e308 * 10), float("-0.0"), 0.0 == -0.0, math.copysign(1, -0.0)))
show("hex", ((0.1).hex(), float.fromhex("0x1.8p3"), (1e-300).hex(), (255.5).hex()))
show("as_integer_ratio", ((0.75).as_integer_ratio(), (0.1).as_integer_ratio()))
show("struct", (struct.pack("<d", 1.5).hex(), struct.pack("<f", 0.1).hex(), struct.unpack("<f", struct.pack("<f", 0.1))[0], struct.pack(">d", -0.0).hex()))
show("frexp/ldexp/modf", (math.frexp(1000.0), math.ldexp(0.5, 11), math.modf(-3.75), math.fmod(-7.5, 2), math.remainder(7.5, 2)))
show("exact", (math.sqrt(2), math.sqrt(1e-10), math.hypot(3, 4), math.fsum([0.1] * 10), sum([0.1] * 10), math.prod([1.5, 2, 3]), math.dist((0, 0), (3, 4))))
show("nextafter/ulp", (math.nextafter(1.0, 2.0), math.ulp(1.0), math.ulp(1e300)))
show("float limits", (sys.float_info.max, sys.float_info.min, sys.float_info.epsilon, sys.float_info.dig, sys.float_info.mant_dig))
show("int to float", (float(2 ** 53 + 1), float(2 ** 1000), 2 ** 53 + 1 == float(2 ** 53 + 1), float(10 ** 22)))
try:
    float(2 ** 1100)
except OverflowError as e:
    show("overflow", e)
show("exp/log (12 digits)", [r12(x) for x in (math.exp(1), math.exp(-10), math.log(10), math.log2(1000), math.log10(2), math.log(2 ** 100), math.expm1(1e-5), math.log1p(1e-5))])
show("trigonometry (12 digits)", [r12(x) for x in (math.sin(1), math.cos(1), math.tan(1), math.atan2(1, 2), math.asin(0.3), math.acos(0.3), math.sin(1e6), math.cos(1e10), math.sinh(2), math.tanh(0.5), math.atanh(0.5))])
show("pi and e", (math.pi, math.e, math.tau, math.degrees(math.pi), math.radians(180)))
show("pow", (r12(2 ** 0.5), r12(10 ** -3.5), r12(math.pow(1.0001, 10000)), r12(math.cbrt(27.5)), r12(math.erf(0.5)), r12(math.gamma(5.5)), r12(math.lgamma(100))))
show("complex", ((1 + 2j) * (3 - 1j), r12(abs(3 + 4j)), r12(cmath.sqrt(-1).imag), (cmath.exp(1j * cmath.pi).real, round(cmath.exp(1j * cmath.pi).imag, 10)), (1 + 2j) ** 2, complex("3+4j")))

print("--- decimal and fractions")
decimal.getcontext().prec = 40
show("decimal", (decimal.Decimal(1) / decimal.Decimal(7), decimal.Decimal("0.1") + decimal.Decimal("0.2"), decimal.Decimal(2).sqrt(), decimal.Decimal(10).ln()))
with decimal.localcontext() as ctx:
    ctx.prec = 5
    ctx.rounding = decimal.ROUND_HALF_UP
    show("precision and rounding", (decimal.Decimal("2.345678") * 1, +decimal.Decimal("2.345678"), decimal.Decimal("1234567") + 0))
show("quantize", (decimal.Decimal("2.675").quantize(decimal.Decimal("0.01")), decimal.Decimal("2.665").quantize(decimal.Decimal("0.01"), rounding=decimal.ROUND_HALF_EVEN)))
try:
    decimal.Decimal(1) / decimal.Decimal(0)
except decimal.DivisionByZero as e:
    show("DivisionByZero", type(e).__name__)
show("decimal money", sum(decimal.Decimal("19.99") for _ in range(100)))
show("fractions", (fractions.Fraction(3, 4) + fractions.Fraction(5, 6), fractions.Fraction("0.125"), fractions.Fraction(1.5), fractions.Fraction(7, 3).limit_denominator(2), fractions.Fraction(10 ** 30, 3 ** 40)))
show("fraction pi", fractions.Fraction(math.pi).limit_denominator(1000))
show("harmonic", sum(fractions.Fraction(1, n) for n in range(1, 31)))

print("--- statistics and iteration")
data = [2.5, 3.5, 1.0, 4.0, 4.0, 6.5, 8.0, 2.0]
show("statistics", (statistics.mean(data), statistics.median(data), statistics.mode(data), statistics.stdev(data), statistics.pvariance(data), statistics.quantiles(data, n=4)))
show("fmean/geometric", (statistics.fmean(data), r12(statistics.geometric_mean(data)), r12(statistics.harmonic_mean(data))))
show("correlation", r12(statistics.correlation([1, 2, 3, 4, 5], [2, 4, 5, 4, 5])))
show("itertools", (list(itertools.islice(itertools.count(10, 5), 4)), list(itertools.accumulate(range(1, 8), operator.mul)), list(itertools.combinations("abcd", 2))[:4], len(list(itertools.permutations(range(6)))), list(itertools.product("ab", repeat=2))))
show("sum of squares", sum(n * n for n in range(1, 100001)))
show("primes below 200", [n for n in range(2, 200) if all(n % d for d in range(2, int(n ** 0.5) + 1))][-5:])
sieve = bytearray([1]) * 2000000
sieve[0] = sieve[1] = 0
for i in range(2, 1415):
    if sieve[i]:
        sieve[i * i::i] = bytearray(len(range(i * i, 2000000, i)))
show("primes below 2 million", sum(sieve))


def collatz(n):
    steps = 0
    while n != 1:
        n = n // 2 if n % 2 == 0 else 3 * n + 1
        steps += 1
    return steps


show("longest Collatz chain below 3000", max((collatz(n), n) for n in range(1, 3000)))
print("--- done")
