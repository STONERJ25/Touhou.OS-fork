"""libc numerics and stdio: exactly rounded strtod/printf, stdio throughput, malloc, strtol/scanf, math.

Every check types one command into gapsh over the serial console (see test_shell.py) and compares the output with strings
computed here on the host with Python's exact arithmetic (float(), fractions, decimal). The guest tools that reach the libc
functions under test:

  strtod   the gapsh `printf` builtin (parses its arguments with strtod and prints with the libc printf), `sort -n` and
           `sort -g` (they compare with strtod), `seq` (atof)
  printf   awk's printf (vsnprintf with the user's flags, width and precision), gapsh's printf, seq's %g
  stdio    cat/wc/sort/sed/grep/awk/tr/cut/rev on a large file
"""
import random
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
