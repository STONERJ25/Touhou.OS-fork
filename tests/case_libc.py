"""libc numerics and stdio: exactly rounded strtod/printf, stdio throughput, malloc, strtol/scanf, math.

Every check types one command into gapsh over the serial console (see test_shell.py) and compares the output with strings
computed here on the host with Python's exact arithmetic (float(), fractions, decimal). The guest tools that reach the libc
functions under test:

  strtod   the gapsh `printf` builtin (parses its arguments with strtod and prints with the libc printf), `sort -n` and
           `sort -g` (they compare with strtod), `seq` (atof)
  printf   awk's printf (vsnprintf with the user's flags, width and precision), gapsh's printf, seq's %g
  stdio    cat/wc/sort/sed/grep/awk/tr/cut/rev on a large file

Typing into the guest is slow (about 10 ms per character through the serial console and the tty), so the tests keep their
input small and let programs inside the guest generate the bulk.
"""
import math
import random
import re
import sys
from contextlib import contextmanager
from decimal import Decimal, getcontext
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_shell
from harness import test, Qemu

getcontext().prec = 1200


@contextmanager
def guest(name):
    with Qemu(cmdline="text", name=name) as vm:
        vm.expect(test_shell.PROMPT, timeout=120)
        yield vm


def sh(vm, cmd, timeout=60):
    return test_shell.run(vm, cmd, timeout=timeout)


def put_lines(vm, path, lines, per_cmd=700):
    """Writes `lines` into a guest file with a few printf commands (a typed line may not exceed ~1000 characters)."""
    sh(vm, f": > {path}")
    batch, size = [], 0
    for ln in lines:
        assert "'" not in ln and "\\" not in ln and "%" not in ln
        if size + len(ln) + 3 > per_cmd and batch:
            sh(vm, "printf '%s\\n' " + " ".join(f"'{x}'" for x in batch) + f" >> {path}")
            batch, size = [], 0
        batch.append(ln)
        size += len(ln) + 3
    if batch:
        sh(vm, "printf '%s\\n' " + " ".join(f"'{x}'" for x in batch) + f" >> {path}")


# ---------------------------------------------------------------------------------------------------------------- strtod

def next_up(x):
    import struct
    u = struct.unpack("<Q", struct.pack("<d", x))[0]
    return struct.unpack("<d", struct.pack("<Q", u + 1))[0]


def next_down(x):
    import struct
    u = struct.unpack("<Q", struct.pack("<d", x))[0]
    return struct.unpack("<d", struct.pack("<Q", u - 1))[0]


def decimal_text(d):
    """Plain positional text of a Decimal."""
    s = format(d, "f")
    return s.rstrip("0").rstrip(".") if "." in s else s


def strtod_strings(seed=7, bases=5):
    """Decimal strings that sit around the boundaries between adjacent doubles (at most 62 characters each, because sort -n
    reads 63 characters of a key): the shortest spelling of x and its neighbours, exact expansions, midpoints between
    doubles, and midpoints nudged up and down."""
    rnd = random.Random(seed)
    out = set()
    for _ in range(bases):
        x = rnd.uniform(1, 1e9) if rnd.random() < 0.7 else rnd.uniform(0.5, 3)
        lo, hi = next_down(x), next_up(x)
        for v in (lo, x, hi):
            out.add(repr(v))
            out.add(f"{v:.17g}")
            out.add(f"{v:.16e}")
        for a, b in ((lo, x), (x, hi)):
            mid = (Decimal(a) + Decimal(b)) / 2
            txt = decimal_text(mid)
            if len(txt) <= 56:
                out.add(txt)
                out.add(txt[:-1] + "4999")                  # a hair below the midpoint
                out.add(txt + "0001")                       # a hair above it
                out.add(txt[:-1] + "5000")                  # the midpoint again, spelled differently
    return sorted(s for s in out if len(s) <= 62)


@test
def libc_strtod_order():
    """strtod is correctly rounded: sort -n puts neighbouring doubles and their midpoint spellings in the exact order."""
    strings = strtod_strings()
    expect = sorted(strings, key=lambda s: (float(s), s))
    shuffled = strings[:]
    random.Random(3).shuffle(shuffled)
    with guest("t-libc-strtod") as vm:
        put_lines(vm, "/tmp/nums", shuffled)
        out, rc = sh(vm, "sort -n /tmp/nums", timeout=120)
        assert rc == 0
        got = out.strip("\n").split("\n")
        assert len(got) == len(expect), f"{len(got)} lines back, expected {len(expect)}"
        bad = [(i, g, e) for i, (g, e) in enumerate(zip(got, expect)) if g != e]
        assert not bad, f"{len(bad)} lines out of order, first: {bad[:4]}"


_NUM = re.compile(
    r"[ \t\n\v\f\r]*(?P<sign>[+-]?)(?:(?P<inf>inf(?:inity)?)|(?P<nan>nan(?:\([A-Za-z0-9_]*\))?)"
    r"|(?P<hex>0x(?:[0-9a-f]+\.?[0-9a-f]*|\.[0-9a-f]+)(?:p[+-]?[0-9]+)?)"
    r"|(?P<dec>(?:[0-9]+\.?[0-9]*|\.[0-9]+)(?:e[+-]?[0-9]+)?))", re.I)


def c_strtod(s):
    """What C's strtod returns for s (longest valid prefix, correctly rounded), computed with Python's exact float parsing."""
    m = _NUM.match(s)
    if not m:
        return 0.0
    sign = -1.0 if m.group("sign") == "-" else 1.0
    if m.group("inf"):
        return sign * math.inf
    if m.group("nan"):
        return math.nan
    if m.group("hex"):
        try:
            return sign * float.fromhex(m.group("hex"))
        except OverflowError:
            return sign * math.inf
    try:
        return sign * float(m.group("dec"))
    except OverflowError:
        return sign * math.inf


def g17(x):
    """printf("%.17g", x)"""
    return "nan" if x != x else "%.17g" % x


ROUNDTRIP_EXTRA = [
    "9007199254740993", "9007199254740992.5", "9007199254740991.5", "18014398509481985",
    "1.00000000000000011102230246251565404236316680908203125", "1.00000000000000011102230246251565404236316680908203126",
    "1.00000000000000011102230246251565404236316680908203124", "0.30000000000000004", "0.1", "0.3", "1e23", "1e22", "8.41e21",
    "4.9406564584124654e-324", "2.4703282292062327e-324", "2.4703282292062328e-324", "2.2250738585072011e-308",
    "2.2250738585072014e-308", "1.7976931348623157e308", "1.7976931348623158e308", "1.7976931348623159e308", "1e400", "-1e-400",
    "0x1.8p3", "0x.8p1", "0X1P-1074", "0x1.fffffffffffffp1023", "0x1.fffffffffffff8p1023", "-0x0p0", "0x", "0xg",
    "inf", "-Infinity", "nan", "-0", "1e", "1e+", "5.", ".5", "+.5e1", "1e-2x", "  12  ", "1_000", "123456789012345678901234567890",
    "0.000000000000000000000000000000000000001", "1" + "0" * 100 + "e-100", "0." + "0" * 300 + "1",
    "1.7976931348623158" + "0" * 280 + "1e308", "0.1" + "0" * 400 + "1", "8" * 120 + "e-130",
]


@test
def libc_strtod_roundtrip():
    """strtod via the shell's printf builtin: parsed and printed back with %.17g, tie and range cases included."""
    strings = ROUNDTRIP_EXTRA + strtod_strings(seed=11, bases=3)
    with guest("t-libc-strtod2") as vm:
        failures = []
        batch = []

        def flush():
            if not batch:
                return
            out, rc = sh(vm, "printf '%.17g\\n' " + " ".join(f"'{x}'" for x in batch), timeout=120)
            got = out.strip("\n").split("\n")
            if len(got) != len(batch):
                failures.append(f"batch {batch[:2]}...: {len(got)} lines for {len(batch)} strings")
            for s, g in zip(batch, got):
                want = g17(c_strtod(s))
                if g != want:
                    failures.append(f"{s[:70]!r}: got {g}, want {want}")
            batch.clear()

        size = 0
        for s in strings:
            if size + len(s) + 3 > 600 and batch:
                flush()
                size = 0
            batch.append(s)
            size += len(s) + 3
        flush()
        assert not failures, "\n".join(failures[:20])


# ---------------------------------------------------------------------------------------------------------------- printf

# (awk expression for the value, the same double in Python). Everything awk needs to build is exact: literals of at most 15
# digits and small exponents, divisions and multiplications of exact numbers.
VALUES = [
    ("0", 0.0), ("0.1", 0.1), ("0.5", 0.5), ("1.5", 1.5), ("2.5", 2.5), ("0.125", 0.125), ("2.675", 2.675), ("1/3", 1 / 3),
    ("100", 100.0), ("99.5", 99.5), ("999999.5", 999999.5), ("0.000123456", 0.000123456), ("1e22", 1e22),
    ("123456789012345678", 123456789012345678.0), ("-17.03125", -17.03125), ("tiny", 2.0 ** -1074), ("huge", sys.float_info.max),
    ("1e-22", 1e-22), ("-log(0)", math.inf), ("log(0)", -math.inf), ("-0", -0.0), ("65", 65.0),
]

def c_printf(fmt, x):
    """printf of one double: Python's % follows C for finite numbers; C never pads inf and nan with zeros."""
    if math.isinf(x) or x != x:
        fmt = re.sub(r"^%([-+ #]*)0+", lambda m: "%" + m.group(1), fmt)
    return fmt % x


FORMATS = [
    "%f", "%.0f", "%.1f", "%.2f", "%.10f", "%10.3f", "%-10.3f", "%+.3f", "% .3f", "%010.3f", "%#.0f", "%.25f",
    "%e", "%.0e", "%#.0e", "%.3e", "%.15e", "%E", "%12.4E", "%-12.4e", "%+e", "%020.8e",
    "%g", "%G", "%.0g", "%.1g", "%.3g", "%.10g", "%.17g", "%#g", "%#.3g", "%12.4g", "%-12.4g", "%+g", "% g", "%012g", "%.30g",
    "%.60g", "%F",
]


@test
def libc_printf_float_matrix():
    """printf %f %e %g %E %G %F with flags, width and precision: exact digits, ties to even, inf, subnormal, DBL_MAX."""
    per_cmd = 7                                             # values per awk run: a typed line may not exceed ~1000 characters
    bad = []
    with guest("t-libc-printf") as vm:
        for lo in range(0, len(VALUES), per_cmd):
            chunk = VALUES[lo:lo + per_cmd]
            stmts = ";".join(f"v[{i + 1}]={expr}" for i, (expr, _) in enumerate(chunk))
            prog = ("BEGIN{tiny=1;for(i=0;i<1074;i++)tiny/=2;huge=1;for(i=0;i<1023;i++)huge*=2;huge*=2-1/4503599627370496;"
                    + stmts + ";nf=split(\"" + ":".join(FORMATS) + "\",f,\":\");"
                    f"for(i=1;i<={len(chunk)};i++)for(j=1;j<=nf;j++)printf f[j] \"|\\n\",v[i]}}")
            cmd = f"awk '{prog}'"
            assert len(cmd) < 940, len(cmd)
            expected = [c_printf(fmt, val) + "|" for _, val in chunk for fmt in FORMATS]
            out, rc = sh(vm, cmd, timeout=240)
            assert rc == 0, out[-500:]
            got = out.rstrip("\n").split("\n")
            assert len(got) == len(expected), f"{len(got)} lines, expected {len(expected)}: {out[:300]!r}"
            for k, (g, e) in enumerate(zip(got, expected)):
                if g != e:
                    bad.append(f"{chunk[k // len(FORMATS)][0]} {FORMATS[k % len(FORMATS)]}: got {g!r} want {e!r}")
        assert not bad, f"{len(bad)} lines differ:\n" + "\n".join(bad[:12])
        # a NaN prints as nan or -nan, in capitals for %F %E %G
        out, rc = sh(vm, "awk 'BEGIN{x=log(-1); printf \"%f|%F|%e|%G|%8.2f|%-8g|\\n\", x, x, x, x, x, x}'")
        assert re.fullmatch(r"-?nan\|-?NAN\|-?nan\|-?NAN\|\s*-?nan\|-?nan\s*\|\n?", out), out


@test
def libc_printf_misc():
    """printf integers, strings, characters, `*` widths, long output (snprintf's would-be length) and the shell's printf."""
    third = "%.600f" % 0.1
    sixth = "%.500f" % (1 / 3)
    cmds = [
        # (command, expected output)
        ("awk 'BEGIN{printf \"%d|%5d|%-5d|%05d|%+d|% d|%.3d|%x|%X|%#x|%o|%#o|%u|%c|%c|%%\\n\", 42,42,42,42,42,42,7,255,255,255,8,8,3000000000,65,\"hello\"}'",
         "42|   42|42   |00042|+42| 42|007|ff|FF|0xff|10|010|3000000000|A|h|%\n"),
        ("awk 'BEGIN{printf \"%s|%10s|%-10s|%.3s|%10.2s|%*d|%-*d|%.*f|\\n\", \"abcdef\",\"abc\",\"abc\",\"abcdef\",\"abcdef\",6,42,6,42,3,3.14159}'",
         "abcdef|       abc|abc       |abc|        ab|    42|42    |3.142|\n"),
        ("awk 'BEGIN{printf \"%d %d %i\\n\", -2147483648, 9007199254740993, -5}'", "-2147483648 9007199254740992 -5\n"),
        ("awk 'BEGIN{printf \"%x %o %X\\n\", 4294967295, 4294967296, 18446744073709551615}'", "ffffffff 40000000000 FFFFFFFFFFFFFFFF\n"),
        ("awk 'BEGIN{s = sprintf(\"%.600f\", 0.1); print length(s), substr(s, 1, 30), substr(s, 580)}'",
         "602 " + third[:30] + " " + third[579:] + "\n"),
        ("awk 'BEGIN{s = sprintf(\"%.500f\", 1/3); print length(s), substr(s, 1, 60), substr(s, 100)}'",
         "502 " + sixth[:60] + " " + sixth[99:] + "\n"),
        ("awk 'BEGIN{s = sprintf(\"%1000d\", 7); print length(s), substr(s, 998)}'", "1000   7\n"),
        ("printf '%d|%5d|%-5d|%05d|%+d|%x|%X|%o|%u|%c|%s|%10s|%-10s|%.2s|%%\\n' -5 42 42 42 42 255 255 8 7 xyz hello world abc def",
         "-5|   42|42   |00042|+42|ff|FF|10|7|x|hello|     world|abc       |de|%\n"),
        ("printf '%.3f|%10.2e|%-12g|%G|%+.1f\\n' 2.675 0.000123 1e23 1e-10 0.25",
         "%.3f|%10.2e|%-12g|%G|%+.1f\n" % (2.675, 0.000123, 1e23, 1e-10, 0.25)),
        ("printf '%.100f' 0.5 | awk '{print length($0)}'", "102\n"),
        ("printf '%5.1f|%.0f|%.0f|%.0f|%.0f\\n' 3.14159 0.5 1.5 2.5 3.5", "  3.1|0|2|2|4\n"),
        ("printf 'a%sb%dc\\n' 'x y' 12 z 7", "ax yb12c\nazb7c\n"),
    ]
    with guest("t-libc-printf2") as vm:
        bad = []
        for cmd, want in cmds:
            out, rc = sh(vm, cmd)
            if out != want:
                bad.append(f"{cmd[:90]}\n   got  {out[:200]!r}\n   want {want[:200]!r}")
        assert not bad, "\n".join(bad)


def seq_g(first, step, last):
    out, v = [], first
    while (v <= last + 1e-9) if step > 0 else (v >= last - 1e-9):
        out.append("%g" % v)
        v += step
    return out


@test
def libc_seq_float():
    """seq with fractional steps (atof for the arguments, %g for the numbers)."""
    cases = [(0.1, 0.1, 1.0), (0.5, 0.25, 3.0), (1.1, 1.1, 12.0), (10, -2.5, 0), (0.3, 0.3, 0.9)]
    with guest("t-libc-seq") as vm:
        for first, step, last in cases:
            out, rc = sh(vm, f"seq {first} {step} {last}")
            assert out.strip("\n").split("\n") == seq_g(float(first), float(step), float(last)), (first, step, last, out)
