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
import hashlib
import math
import random
import re
import sys
import time
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


# ---------------------------------------------------------------------------------------------------------------- stdio

def guest_seconds(vm, cmd, timeout=900):
    """Wall time of a command according to the guest's own clock (/proc/uptime, centiseconds)."""
    out, rc = sh(vm, f"a=$(cat /proc/uptime); {cmd} > /dev/null; b=$(cat /proc/uptime); echo \"$a $b\" | awk '{{printf \"%.2f\", $2-$1}}'",
                 timeout=timeout)
    return float(out.strip().split()[-1])


@test
def libc_stdio_bigfile():
    """stdio on a 6.9 MB file and a 1.3 MB one: cat, wc, sort, sed, grep, awk, tr, cut, rev, tac give exactly the right answers."""
    nums = [str(i) for i in range(1, 1000001)]
    big = ("\n".join(nums) + "\n").encode()
    mid_nums = nums[:200000]
    mid = ("\n".join(mid_nums) + "\n").encode()
    md5 = lambda b: hashlib.md5(b).hexdigest()
    with_9 = sum(1 for n in nums if "9" in n)
    checks = [
        ("cat /tmp/big | md5sum", md5(big)),
        ("wc -l < /tmp/big", "1000000"),
        ("wc -c < /tmp/big", str(len(big))),
        ("sort -n /tmp/big | tail -n 1", "1000000"),
        ("sed -n '$p' /tmp/big", "1000000"),
        ("grep -c 9 /tmp/big", str(with_9)),
        ("awk 'END{print NR}' /tmp/big", "1000000"),
        ("awk '{s += $1} END{printf \"%d\\n\", s}' /tmp/big", str(sum(range(1, 1000001)))),
        ("head -n 3 /tmp/big | tail -n 1", "3"),
        ("sort /tmp/mid | md5sum", md5(("\n".join(sorted(mid_nums)) + "\n").encode())),
        ("sed s/1/2/ /tmp/mid | md5sum", md5(("\n".join(n.replace("1", "2", 1) for n in mid_nums) + "\n").encode())),
        ("tr 1 2 < /tmp/mid | md5sum", md5(mid.replace(b"1", b"2"))),
        ("cut -c1-3 /tmp/mid | md5sum", md5(("\n".join(n[:3] for n in mid_nums) + "\n").encode())),
        ("rev /tmp/mid | md5sum", md5(("\n".join(n[::-1] for n in mid_nums) + "\n").encode())),
        ("tac /tmp/mid | md5sum", md5(("\n".join(reversed(mid_nums)) + "\n").encode())),
        ("sort -n -r /tmp/mid | head -n 1", "200000"),
    ]
    with guest("t-libc-stdio") as vm:
        out, rc = sh(vm, "seq 1 1000000 > /tmp/big; seq 1 200000 > /tmp/mid; ls -l /tmp/big", timeout=300)
        assert str(len(big)) in out, out
        bad = []
        for cmd, want in checks:
            out, rc = sh(vm, cmd, timeout=900)
            got = out.strip().split()[0] if out.strip() else ""
            if got != want or rc != 0:
                bad.append(f"{cmd}: got {out.strip()[:60]!r} rc {rc}, want {want!r}")
        assert not bad, "\n".join(bad)
        for name, cmd in (("cat", "cat /tmp/big"), ("wc -l", "wc -l /tmp/big"), ("sort -n", "sort -n /tmp/big"), ("sed", "sed s/1/2/ /tmp/big"),
                          ("grep -c", "grep -c 9 /tmp/big"), ("awk NR", "awk 'END{print NR}' /tmp/big")):
            print(f"  [libc timing] {name:8} {guest_seconds(vm, cmd):6.2f} s (guest clock)")


@test
def libc_malloc_patterns():
    """malloc/realloc under stress through the tools: a million allocate+free cycles, fragmentation, long lines, growing strings."""
    nsort = 200000
    nums = [str(i) for i in range(1, nsort + 1)]
    sorted_md5 = hashlib.md5(("\n".join(sorted(nums)) + "\n").encode()).hexdigest()
    cases = [
        ("seq 1 1000000 | awk '{a[$1] = $1; delete a[$1]} END{print length(a)}'", "0"),
        ("seq 1 100000 | awk '{a[NR] = $1} END{for (i = 1; i <= NR; i += 2) delete a[i]; "
         "for (i = 1; i <= NR; i++) b[i] = sprintf(\"%040d\", i); for (i = 2; i <= NR; i += 2) delete a[i]; "
         "n = 0; for (k in b) n++; print n, length(a), b[99999]}'", "100000 0 0000000000000000000000000000000000099999"),
        ("awk 'BEGIN{for (i = 0; i < 1000000; i++) printf \"x\"; print \"\"}' | rev | wc -c", "1000001"),
        ("awk 'BEGIN{for (i = 0; i < 20000; i++) s = s \"ab\"; print length(s)}'", "40000"),
        ("awk 'BEGIN{s = \"x\"; for (i = 0; i < 22; i++) s = s s; print length(s)}'", "4194304"),
        (f"seq 1 {nsort} | sort | md5sum", sorted_md5),
        (f"seq 1 {nsort} | tac | tac | md5sum", hashlib.md5(("\n".join(nums) + "\n").encode()).hexdigest()),
    ]
    with guest("t-libc-malloc") as vm:
        bad = []
        for cmd, want in cases:
            t0 = time.time()
            out, rc = sh(vm, cmd, timeout=900)
            got = out.strip().split(None, 0)[0] if out.strip() else ""
            if " ".join(out.split()[:len(want.split())]) != want or rc != 0:
                bad.append(f"{cmd[:80]}: got {out.strip()[:60]!r} rc {rc}, want {want!r}")
            print(f"  [libc timing] {cmd[:60]:60} {time.time() - t0:6.1f} s wall")
        assert not bad, "\n".join(bad)
        out, rc = sh(vm, "free | head -n 2")
        assert rc == 0


@test
def libc_strtol_family():
    """strtoll/strtoull through the shell's printf (base 0): prefixes, signs, saturation, junk; expr and date -d use strtoll/sscanf."""
    cmds = [
        ("printf '%d|%d|%d|%d|%d\\n' 0x1F 010 -0x10 '  42' +7", "31|8|-16|42|7\n"),
        ("printf '%d|%d\\n' 9223372036854775807 9223372036854775808", "9223372036854775807|9223372036854775807\n"),
        ("printf '%d|%d\\n' -9223372036854775808 -9223372036854775809", "-9223372036854775808|-9223372036854775808\n"),
        ("printf '%u|%u|%x\\n' -1 18446744073709551616 0xFFFFFFFFFFFFFFFF", "18446744073709551615|18446744073709551615|ffffffffffffffff\n"),
        ("printf '%d|%d|%d|%d\\n' 0x 0xg 12abc ''", "0|0|12|0\n"),
        ("printf '%d|%d|%d\\n' 0777 08 0x7fffffff", "511|0|2147483647\n"),
        ("expr 99999999999999999999 + 1", "-9223372036854775808\n"),
        ("expr 007 + 1", "8\n"),
        ("date -u -d '2024-02-29 12:34:56' +%Y-%m-%d_%H:%M:%S", "2024-02-29_12:34:56\n"),
        ("date -u -d '2024-02-29T01:02:03' +%s", "1709168523\n"),
        ("head -n 2x /dev/null; echo rc=$?", None),
    ]
    with guest("t-libc-strtol") as vm:
        bad = []
        for cmd, want in cmds:
            out, rc = sh(vm, cmd)
            if want is None:
                continue
            if out != want:
                bad.append(f"{cmd[:80]}\n   got  {out[:200]!r}\n   want {want[:200]!r}")
        assert not bad, "\n".join(bad)


# ---------------------------------------------------------------------------------------------------------------- math

# (awk statements run first, awk expression, expected "%.17g" text). The expected values are correctly rounded results computed
# once with mpmath at 150 digits; the inputs are numbers awk builds exactly (short literals, doubling and halving loops).
MATH_TABLE = [
    ('', 'sin(0.5)', '0.47942553860420301'),
    ('', 'cos(0.5)', '0.87758256189037276'),
    ('', 'sin(1)', '0.8414709848078965'),
    ('', 'cos(1)', '0.54030230586813977'),
    ('', 'sin(2)', '0.90929742682568171'),
    ('', 'cos(2)', '-0.41614683654714241'),
    ('', 'sin(10)', '-0.54402111088936977'),
    ('', 'cos(10)', '-0.83907152907645244'),
    ('', 'sin(100)', '-0.50636564110975879'),
    ('', 'cos(100)', '0.86231887228768389'),
    ('', 'sin(1e6)', '-0.34999350217129294'),
    ('', 'cos(1e6)', '0.93675212753314474'),
    ('', 'sin(123456789.123)', '0.99984293955204506'),
    ('', 'cos(123456789.123)', '0.017722760166677856'),
    ('', 'sin(1e15)', '0.85827279317023586'),
    ('', 'cos(1e15)', '-0.51319373778697031'),
    ('', 'sin(1e22)', '-0.85220084976718879'),
    ('', 'cos(1e22)', '0.52321478539513899'),
    ('', 'sin(3.14159)', '2.65358979335273e-06'),
    ('', 'cos(3.14159)', '-0.99999999999647926'),
    ('', 'sin(0.001)', '0.00099999983333334168'),
    ('', 'cos(0.001)', '0.99999950000004167'),
    ('x=1;for(i=0;i<100;i++)x*=2;', 'sin(x)', '-0.87218360541826734'),
    ('x=1;for(i=0;i<100;i++)x*=2;', 'cos(x)', '0.48917865697472146'),
    ('x=1;for(i=0;i<700;i++)x*=2;', 'sin(x)', '-0.46054855909065234'),
    ('x=1;for(i=0;i<700;i++)x*=2;', 'cos(x)', '0.88763451077542266'),
    ('x=1;for(i=0;i<1000;i++)x*=2;', 'sin(x)', '-0.15920170308624243'),
    ('x=1;for(i=0;i<1000;i++)x*=2;', 'cos(x)', '0.98724607759891347'),
    ('pi=4*atan2(1,1);', 'sin(pi)', '1.2246467991473532e-16'),
    ('pi=4*atan2(1,1);', 'cos(pi/2)', '6.123233995736766e-17'),
    ('pi=4*atan2(1,1);', 'sin(pi*1000000)', '-2.2319121813608711e-10'),
    ('', 'atan2(1,1)', '0.78539816339744828'),
    ('', 'atan2(-1,-1)', '-2.3561944901923448'),
    ('', 'atan2(0.5,3)', '0.16514867741462683'),
    ('', 'atan2(1e10,1e-10)', '1.5707963267948966'),
    ('', 'atan2(2,-5)', '2.7610862764774282'),
    ('', 'atan2(-3,0.25)', '-1.4876550949064553'),
    ('', 'atan2(1,-1e15)', '3.1415926535897922'),
    ('', 'atan2(0.1,0.3)', '0.32175055439664224'),
    ('', 'exp(1)', '2.7182818284590451'),
    ('', 'exp(0.5)', '1.6487212707001282'),
    ('', 'exp(-0.5)', '0.60653065971263342'),
    ('', 'exp(10)', '22026.465794806718'),
    ('', 'exp(100)', '2.6881171418161356e+43'),
    ('', 'exp(700)', '1.0142320547350045e+304'),
    ('', 'exp(-700)', '9.8596765437597708e-305'),
    ('', 'exp(1e-5)', '1.0000100000500001'),
    ('', 'exp(2.5)', '12.182493960703473'),
    ('', 'exp(-37.7)', '4.2373860474966705e-17'),
    ('', 'exp(0.1)', '1.1051709180756477'),
    ('', 'log(2)', '0.69314718055994529'),
    ('', 'log(10)', '2.3025850929940459'),
    ('', 'log(0.5)', '-0.69314718055994529'),
    ('', 'log(1.5)', '0.40546510810816438'),
    ('', 'log(123456789)', '18.631401766168018'),
    ('', 'log(0.999999)', '-1.0000005000290891e-06'),
    ('', 'log(1.000001)', '9.9999949991806676e-07'),
    ('', 'log(1e15)', '34.538776394910684'),
    ('', 'log(7.77)', '2.050270164379556'),
    ('x=1;for(i=0;i<1000;i++)x/=2;', 'log(x)', '-693.14718055994535'),
    ('x=1;for(i=0;i<1074;i++)x/=2;', 'sqrt(x)', '2.2227587494850775e-162'),
    ('', 'sqrt(2)', '1.4142135623730951'),
    ('', 'sqrt(0.5)', '0.70710678118654757'),
    ('', 'sqrt(1e15)', '31622776.601683792'),
    ('', 'sqrt(123456789)', '11111.111060555555'),
    ('', 'sqrt(7)', '2.6457513110645907'),
    ('', 'sqrt(1e-5)', '0.0031622776601683794'),
    ('', '(2)^(0.5)', '1.4142135623730951'),
    ('', '(10)^(-3)', '0.001'),
    ('', '(1.5)^(2.5)', '2.7556759606310752'),
    ('', '(7)^(13)', '96889010407'),
    ('', '(123.456)^(7.89)', '31771028258180936'),
    ('', '(-2)^(3)', '-8'),
    ('', '(10)^(15)', '1000000000000000'),
    ('', '(10)^(22)', '1e+22'),
    ('', '(10)^(23)', '9.9999999999999992e+22'),
    ('', '(3)^(40)', '1.2157665459056929e+19'),
    ('', '(5)^(27)', '7.4505805969238282e+18'),
    ('', '(0.1)^(3)', '0.0010000000000000002'),
    ('', '(2)^(10)', '1024'),
    ('', '(2.5)^(3)', '15.625'),
    ('', '(9)^(0.5)', '3'),
    ('', '(10)^(-22)', '1e-22'),
    ('', '(0.5)^(1073.5)', '4.9406564584124654e-324'),
    ('', '(0.5)^(1074.4)', '4.9406564584124654e-324'),
    ('', '(0.5)^(1074.6)', '4.9406564584124654e-324'),
    ('', '(2)^(1023.5)', '1.2711610061536464e+308'),
    ('', '(10)^(-323.5)', '4.9406564584124654e-324'),
    ('', '(10)^(308.25)', '1.7782794100389228e+308'),
    ('', '(1.0000001)^(99999.5)', '1.0100501160825357'),
    ('x=1;for(i=0;i<1074;i++)x/=2;', 'x', '4.9406564584124654e-324'),
    ('', '2^1023', '8.9884656743115795e+307'),
    ('', '(5.5)%(2)', '1.5'),
    ('', '(-7)%(3)', '-1'),
    ('', '(7)%(-3)', '1'),
    ('', '(1e15)%(7)', '6'),
    ('', '(123456.789)%(0.1)', '0.088999999997454143'),
    ('', '(2.5)%(0.5)', '0'),
    ('', '(1e15)%(0.3)', '0.13700743415417188'),
    ('', '(-0.7)%(0.2)', '-0.099999999999999922'),
    ('x=1;for(i=0;i<100;i++)x*=2;', 'x%3', '1'),
]


@test
def libc_math_awk():
    """sin cos atan2 exp log sqrt pow (^) and fmod (%) through awk: correctly rounded, huge arguments, exact powers, subnormals."""
    with guest("t-libc-math") as vm:
        bad = []
        i = 0
        while i < len(MATH_TABLE):
            chunk, size = [], 0
            while i < len(MATH_TABLE) and size < 520:
                setup, expr, want = MATH_TABLE[i]
                chunk.append(MATH_TABLE[i])
                size += len(setup) + len(expr) + 24
                i += 1
            prog = "BEGIN{" + "".join(f'{setup}printf "%.17g\\n",{expr};' for setup, expr, _ in chunk) + "}"
            out, rc = sh(vm, f"awk '{prog}'")
            got = out.strip("\n").split("\n")
            if len(got) != len(chunk):
                bad.append(f"{prog[:100]}: {len(got)} lines, expected {len(chunk)}: {out[:200]!r}")
                continue
            for (setup, expr, want), g in zip(chunk, got):
                if g != want:
                    bad.append(f"{setup}{expr}: got {g}, want {want}")
        assert not bad, f"{len(bad)} of {len(MATH_TABLE)} differ:\n" + "\n".join(bad[:15])
