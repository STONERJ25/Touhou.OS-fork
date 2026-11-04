#!/usr/bin/env python3
"""Differential test of Kosuzu's BigInt (user/lib/js/src/bigint.c) against Python's integers.

    python tools/hosttest.py discover -s tests/host -p "test_js_bigint.py" -v

Random operands (a few bits to a few thousand, signs, and structured ones made of all-ones and all-zero limbs that
exercise the carries and the rare "add back" step of Knuth's division) go through every operator, the conversions and
the comparisons with doubles; each result must equal Python's.  The program runs under AddressSanitizer and UBSan.
JS_BIGINT_FUZZ=1500 (pairs) and JS_BIGINT_FUZZ_SEED=7 run a bigger or different one.  The fixed default seed makes
failures reproducible: the first mismatches are printed with their operands.
"""
import json
import math
import os
import random
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_js  # noqa: E402  (the tool builder and runner)

if hasattr(sys, "set_int_max_str_digits"):
    sys.set_int_max_str_digits(0)         # Python 3.11 refuses to print integers of more than 4300 digits by default


def tdiv(a, b):                       # BigInt division truncates toward zero; Python's floors
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def tmod(a, b):                       # the remainder takes the sign of the dividend
    return a - b * tdiv(a, b)


def f64_bits(f):
    lo, hi = struct.unpack("<II", struct.pack("<d", f))
    return "%x:%x" % (hi, lo)


def to_float(a):
    try:
        return float(a)
    except OverflowError:
        return math.inf if a > 0 else -math.inf


def to_base(n, base):
    digits = "0123456789abcdefghijklmnopqrstuvwxyz"
    if n == 0:
        return "0"
    s = ""
    m = abs(n)
    while m:
        s = digits[m % base] + s
        m //= base
    return ("-" if n < 0 else "") + s


def as_uint_n(bits, a):
    return a % (1 << bits) if bits else 0


def as_int_n(bits, a):
    if bits == 0:
        return 0
    m = a % (1 << bits)
    return m - (1 << bits) if m >= (1 << (bits - 1)) else m


def structured(rng):
    """A number made of runs of limbs that are all ones, all zeros, 0x80000000 or random."""
    limbs = []
    for _ in range(rng.randint(1, 12)):
        kind = rng.choice(["ones", "zero", "high", "rand", "one"])
        limb = {"ones": 0xFFFFFFFF, "zero": 0, "high": 0x80000000, "rand": rng.getrandbits(32), "one": 1}[kind]
        limbs.extend([limb] * rng.randint(1, 3))
    n = 0
    for limb in limbs:
        n = (n << 32) | limb
    return n


def operand(rng):
    r = rng.random()
    if r < 0.25:
        n = rng.getrandbits(rng.randint(0, 64))
    elif r < 0.55:
        n = rng.getrandbits(rng.randint(65, 400))
    elif r < 0.65:
        n = rng.getrandbits(rng.randint(400, 3000))
    elif r < 0.90:
        n = structured(rng)
    else:
        k = rng.choice([31, 32, 33, 52, 53, 54, 63, 64, 65, 95, 96, 97, 127, 128, 129, 1023, 1024, 1025])
        n = (1 << k) + rng.choice([-2, -1, 0, 1, 2])
    return -n if rng.random() < 0.4 else n


# (dividend, divisor) pairs that take step D6 of Knuth's algorithm D (the estimate of a quotient digit was one too big
# and the divisor is added back), found by running an instrumented port of the algorithm over structured inputs
ADD_BACK = [
    (0x400000010000000180000000800000000000000100000001, 0x80000000ffffffffffffffff00000002),
    (0x800000007ffffffec000000280000000400000010000000080000000, 0xffffffff000000008000000100000001),
    (0xfffffffe000000033ffffffcc000000140000001fffffffd80000000, 0xffffffff800000017fffffff7fffffff00000001),
    (0x70000000d800000027ffffffc, 0x3800000017fffffff),
    (0x3fffffff70000000200000005fffffffb0000000000000001, 0x3ffffffff0000000000000002),
    (0xffffffff00000001000000007fffffff7fffffff, 0x1fffffffe0000000200000000ffffffff),
    (0x1000000037ffffffe0000000080000001, 0x1ffffffff0000000000000001),
    (0x1fffffffefffffffffffffffffffffffd, 0xffffffff7fffffffffffffffffffffff),
    (0x4000000140000001c000000180000003800000017ffffffc, 0x80000001800000007fffffff),
    (0xc0000002800000058490e8a8ef11020fa52285a9, 0x80000001000000027fffffff),
    (0x3fffffff800000017ffffffe40000001800000007fffffffffffffff, 0x8000000000000002000000008000000000000000),
    (0xbffffffec0000000fffffffd7ffffffeffffffff, 0x7fffffff00000000fffffffe),
]

DOUBLES = ["0", "-0", "1", "-1", "0.5", "-0.5", "1.5", "-1.5", "2.5", "4294967295", "4294967296", "4294967297.5", "9007199254740991",
           "9007199254740992", "18446744073709551616", "-18446744073709551616", "1.8446744073709552e+19", "1e21", "-1e21", "1e300",
           "1.7976931348623157e+308", "5e-324", "Infinity", "-Infinity", "123456789.125", "-123456789.125", "3.5e+30", "2e+100"]

JS = r"""
var xs = %(xs)s, ps = %(ps)s, ss = %(ss)s, ds = %(ds)s, es = %(es)s;
function b2(x) { var f = new Float64Array(1); f[0] = x; var u = new Uint32Array(f.buffer); return u[1].toString(16) + ":" + u[0].toString(16); }
function t(v) { return v ? 1 : 0; }
var out = [];
for (var k = 0; k < ps.length; k += 2) {
  var a = BigInt(xs[ps[k]]), b = BigInt(xs[ps[k + 1]]);
  var line = [a + b, a - b, a * b, b !== 0n ? a / b : "x", b !== 0n ? a %% b : "x", a & b, a | b, a ^ b, -a, ~a,
              t(a < b), t(a <= b), t(a > b), t(a >= b), t(a == b), t(a === b), t(a != b)];
  out.push(line.join(" "));
}
for (var k = 0; k < xs.length; k++) {
  var a = BigInt(xs[k]), line = [];
  for (var r of [2, 3, 7, 8, 10, 16, 32, 36]) line.push(a.toString(r));
  line.push(b2(Number(a)), String(a), a.toLocaleString().replace(/,/g, ""));
  for (var m = 0; m < 3; m++) {
    var bits = ss[(k * 3 + m) %% ss.length];
    line.push(BigInt.asIntN(bits, a), BigInt.asUintN(bits, a), a << BigInt(bits), a >> BigInt(bits), a << BigInt(-bits), a >> BigInt(-bits));
  }
  var h = a < 0n ? -a : a;
  line.push(BigInt("0x" + h.toString(16)), BigInt("0b" + h.toString(2)), BigInt("0o" + h.toString(8)), BigInt(" " + h.toString() + "\n"), BigInt("-" + h.toString()));
  if (h < (1n << 100n)) { var e = es[k %% es.length]; line.push(a ** BigInt(e)); }
  for (var d of ds) { var x = Number(d); line.push("" + t(a < x) + t(a > x) + t(a == x) + t(x < a) + t(x > a) + t(x == a)); }
  out.push(line.join(" "));
}
print(out.join("\n"));
"""


def expected(xs, ps, ss, ds, es):
    lines = []
    for k in range(0, len(ps), 2):
        a, b = xs[ps[k]], xs[ps[k + 1]]
        row = [a + b, a - b, a * b, tdiv(a, b) if b else "x", tmod(a, b) if b else "x", a & b, a | b, a ^ b, -a, ~a,
               int(a < b), int(a <= b), int(a > b), int(a >= b), int(a == b), int(a == b), int(a != b)]
        lines.append(" ".join(map(str, row)))
    for k, a in enumerate(xs):
        row = [to_base(a, r) for r in (2, 3, 7, 8, 10, 16, 32, 36)]
        row += [f64_bits(to_float(a)), str(a), str(a)]
        for m in range(3):
            bits = ss[(k * 3 + m) % len(ss)]
            # a << BigInt(-bits) is a >> bits and a >> BigInt(-bits) is a << bits: a negative count reverses the direction
            row += [as_int_n(bits, a), as_uint_n(bits, a), a << bits, a >> bits, a >> bits, a << bits]
        h = abs(a)
        row += [h, h, h, h, -h]
        if h < (1 << 100):
            row.append(a ** es[k % len(es)])
        for d in ds:
            x = float(d)
            row.append("%d%d%d%d%d%d" % (a < x, a > x, a == x, x < a, x > a, x == a))
        lines.append(" ".join(map(str, row)))
    return lines


class Differential(unittest.TestCase):
    def test_against_python_ints(self):
        rng = random.Random(int(os.environ.get("JS_BIGINT_FUZZ_SEED", "20261103")))
        npairs = int(os.environ.get("JS_BIGINT_FUZZ", "500"))
        xs = [0, 1, -1, 2, -2, 10, 2 ** 32 - 1, 2 ** 32, -(2 ** 32), 2 ** 64, 2 ** 64 - 1, -(2 ** 63), 2 ** 63] + [operand(rng) for _ in range(150)]
        ps = []
        for u, v in ADD_BACK:
            for sa, sb in ((1, 1), (-1, 1), (1, -1), (-1, -1)):
                xs += [sa * u, sb * v]
                ps += [len(xs) - 2, len(xs) - 1]
        for _ in range(npairs):
            i, j = rng.randrange(len(xs)), rng.randrange(len(xs))
            ps += [i, j]
            if rng.random() < 0.3:                   # a dividend that is nearly a multiple of the divisor: the rare division paths
                q, r = operand(rng), rng.choice([-1, 0, 1])
                if xs[j]:
                    xs.append(xs[j] * q + r)
                    ps += [len(xs) - 1, j]
        ss = [rng.randint(0, 200) for _ in range(40)] + [0, 1, 31, 32, 33, 63, 64, 65]
        es = [rng.randint(0, 25) for _ in range(30)] + [0, 1, 2]
        ds = DOUBLES
        js = JS % {"xs": json.dumps([str(x) for x in xs]), "ps": json.dumps(ps), "ss": json.dumps(ss), "ds": json.dumps(ds), "es": json.dumps(es)}
        path = test_js.OUT / "bigint_fuzz.js"
        test_js.OUT.mkdir(parents=True, exist_ok=True)
        path.write_text(js, encoding="utf-8")
        rc, out, err = test_js.run([str(path)], timeout=1200)
        self.assertEqual(rc, 0, out[-1500:] + err[-1500:])
        self.assertNotIn("AddressSanitizer", err)
        got = out.rstrip("\n").split("\n")
        want = expected(xs, ps, ss, ds, es)
        self.assertEqual(len(got), len(want))
        bad = 0
        for n, (g, w) in enumerate(zip(got, want)):
            if g != w:
                bad += 1
                if bad <= 3:
                    gs, ws = g.split(" "), w.split(" ")
                    diffs = [(i, a[:60], b[:60]) for i, (a, b) in enumerate(zip(gs, ws)) if a != b][:4]
                    where = ("pair %d: a=%s b=%s" % (n, str(xs[ps[2 * n]])[:80], str(xs[ps[2 * n + 1]])[:80])) if n < len(ps) // 2 else "operand %d: %s" % (n - len(ps) // 2, str(xs[n - len(ps) // 2])[:80])
                    print("MISMATCH in %s: fields (index, ours, python) %r" % (where, diffs))
        self.assertEqual(bad, 0, "%d of %d lines disagree (first ones printed above)" % (bad, len(want)))


if __name__ == "__main__":
    unittest.main()
