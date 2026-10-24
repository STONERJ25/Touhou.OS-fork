#!/usr/bin/env python3
"""A tiny Csmith: random integer and floating-point expressions over every C type, compiled by mcc and by gcc (-fwrapv)
in WSL and compared.  Everything is made free of undefined behaviour except signed wrap-around, which gcc is told to wrap.

  python tools/mcc_fuzz.py [--count 60] [--seed 1] [--keep]

Used by tests/case_mcc.py (a small fixed batch) and by hand for long runs."""
import argparse
import random
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tests"))
import mcc_util as U  # noqa: E402

INT_TYPES = ["signed char", "unsigned char", "short", "unsigned short", "int", "unsigned", "long long", "unsigned long long", "long", "unsigned long"]
SUFFIX = ["", "u", "l", "ul", "ll", "ull"]


class Gen:
    def __init__(self, rnd, nvars):
        self.r = rnd
        self.vars = []
        for i in range(nvars):
            ty = rnd.choice(INT_TYPES)
            self.vars.append((ty, f"v{i}", self.literal_value(ty)))

    def literal_value(self, ty):
        bits = {"signed char": 8, "unsigned char": 8, "short": 16, "unsigned short": 16, "int": 32, "unsigned": 32}.get(ty, 64)
        kind = self.r.random()
        if kind < 0.3:
            return self.r.randrange(-8, 9)
        if kind < 0.6:
            return self.r.randrange(-(1 << (bits - 1)), 1 << (bits - 1))
        return self.r.choice([0, 1, -1, 127, 128, 255, 256, 32767, 65535, 2147483647, 4294967295, (1 << 63) - 1, 1 << 31, 1 << 62, 12345])

    def lit(self):
        v = self.r.choice([0, 1, 2, 3, 7, 10, 100, 255, 256, 1000, 65535, 70000, 2147483647, 4294967295, 123456789, 0x7fffffffffffffff, self.r.randrange(0, 1 << 20)])
        s = self.r.choice(SUFFIX)
        if v > 2147483647 and s in ("", "l"):
            s = "ll" if s == "" else "l"
        if v > 4294967295 and s in ("u", ""):
            s = "ull"
        return f"{v}{s}" if self.r.random() < 0.8 else f"(-{v}{s})"

    def expr(self, depth):
        r = self.r
        if depth <= 0 or r.random() < 0.15:
            if r.random() < 0.6 and self.vars:
                return r.choice(self.vars)[1]
            return self.lit()
        k = r.random()
        if k < 0.10:
            return f"(({r.choice(INT_TYPES)})({self.expr(depth - 1)}))"
        if k < 0.17:
            return f"(-({self.expr(depth - 1)}))"
        if k < 0.22:
            return f"(~({self.expr(depth - 1)}))"
        if k < 0.26:
            return f"(!({self.expr(depth - 1)}))"
        if k < 0.34:
            return f"(({self.expr(depth - 1)}) ? ({self.expr(depth - 1)}) : ({self.expr(depth - 1)}))"
        if k < 0.42:
            op = r.choice(["<<", ">>"])
            return f"(({self.expr(depth - 1)}) {op} (({self.expr(depth - 1)}) & 7))"
        if k < 0.50:
            op = r.choice(["/", "%"])
            d = self.expr(depth - 1)
            return f"(({self.expr(depth - 1)}) {op} ((({d}) == 0 || ({d}) == -1) ? 1 : ({d})))"
        op = r.choice(["+", "-", "*", "&", "|", "^", "==", "!=", "<", "<=", ">", ">=", "&&", "||", "+", "-", "*"])
        return f"(({self.expr(depth - 1)}) {op} ({self.expr(depth - 1)}))"


FLT = ["float", "double"]


def float_program(rnd):
    """Floating point: finite values, conversions in range."""
    lines = ["#include <stdio.h>", "volatile double dv[4] = {%s};" % ", ".join(f"{rnd.uniform(-1000, 1000):.4f}" for _ in range(4)),
             "volatile float fv[4] = {%s};" % ", ".join(f"{rnd.uniform(-100, 100):.3f}f" for _ in range(4)),
             "volatile int iv[4] = {%s};" % ", ".join(str(rnd.randrange(-100000, 100000)) for _ in range(4)), "int main(void) {"]

    def fx(depth):
        if depth <= 0 or rnd.random() < 0.2:
            return rnd.choice(["dv[%d]" % rnd.randrange(4), "fv[%d]" % rnd.randrange(4), "(double)iv[%d]" % rnd.randrange(4), f"{rnd.uniform(-50, 50):.3f}", f"{rnd.uniform(-5, 5):.2f}f"])
        k = rnd.random()
        if k < 0.1:
            return f"(-({fx(depth - 1)}))"
        if k < 0.2:
            return f"((float)({fx(depth - 1)}))"
        if k < 0.3:
            return f"((double)(long long)({fx(depth - 1)} * 0.001))"
        if k < 0.4:
            return f"(({fx(depth - 1)}) < ({fx(depth - 1)}) ? ({fx(depth - 1)}) : ({fx(depth - 1)}))"
        op = rnd.choice(["+", "-", "*"])
        return f"(({fx(depth - 1)}) {op} ({fx(depth - 1)}))"

    for i in range(40):
        e = fx(4)
        lines.append(f'  {{ double r = (double)({e}); if (r != r || r > 1e30 || r < -1e30) r = 1; printf("%.6f\\n", r); }}')
        lines.append(f'  printf("%d\\n", ({fx(3)}) > ({fx(3)}));')
    lines.append("  return 0; }")
    return "\n".join(lines) + "\n"


def int_program(rnd):
    g = Gen(rnd, 8)
    lines = ["#include <stdio.h>"]
    for ty, name, val in g.vars:
        lines.append(f"volatile {ty} {name} = ({ty})({val}LL);")
    lines.append("int main(void) {")
    for i in range(60):
        e = g.expr(rnd.randrange(2, 6))
        lines.append(f'  printf("%llu\\n", (unsigned long long)({e}));')
    # assignments and compound operators on narrow and wide variables
    lines.append("  {")
    for i, (ty, name, _) in enumerate(g.vars[:4]):
        lines.append(f"    {ty} t{i} = {name};")
    for i in range(12):
        t = rnd.randrange(4)
        op = rnd.choice(["+=", "-=", "*=", "&=", "|=", "^=", "<<=", ">>=", "/=", "%="])
        rhs = g.expr(2)
        if op in ("<<=", ">>="):
            rhs = f"(({rhs}) & 7)"
        elif op in ("/=", "%="):
            rhs = f"((({rhs}) == 0 || ({rhs}) == -1) ? 1 : ({rhs}))"
        lines.append(f"    t{t} {op} {rhs};")
        if rnd.random() < 0.4:
            lines.append(f"    t{t}{rnd.choice(['++', '--'])};")
    for i in range(4):
        lines.append(f'    printf("%llu\\n", (unsigned long long)t{i});')
    lines.append("  }")
    lines.append("  return 0; }")
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=40)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()
    out = ROOT / "build" / "fuzz"
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("*"):
        old.unlink()
    for i in range(args.count):
        rnd = random.Random(args.seed * 100003 + i)
        src = float_program(rnd) if i % 5 == 4 else int_program(rnd)
        (out / f"f{i}.c").write_text(src)
    U.build_host()
    U.make_sysroot()
    script = ROOT / "build" / "fuzz" / "run.sh"
    script.write_text(
        "#!/bin/bash\ncd \"$(dirname \"$0\")/../..\"\nexport MCC_ROOT=$PWD/build/mcc-root\nfail=0\nfor f in build/fuzz/f*.c; do\n"
        "  n=${f%.c}\n"
        "  if ! build/host/mcc -c $f -o $n.o 2> $n.err; then echo \"FAIL $f: mcc: $(head -c 200 $n.err)\"; fail=1; continue; fi\n"
        "  gcc -no-pie -o $n.mcc $n.o 2>/dev/null || { echo \"FAIL $f: link\"; fail=1; continue; }\n"
        "  gcc -w -O0 -fwrapv -o $n.gcc $f 2>/dev/null || { echo \"FAIL $f: gcc\"; fail=1; continue; }\n"
        "  ./$n.mcc > $n.mcc.out 2>&1; ./$n.gcc > $n.gcc.out 2>&1\n"
        "  if ! cmp -s $n.mcc.out $n.gcc.out; then echo \"FAIL $f: output differs\"; diff $n.mcc.out $n.gcc.out | head -4; fail=1; fi\n"
        "done\n[ $fail = 0 ] && echo \"fuzz: all ok\"\nexit $fail\n", newline="\n")
    r = subprocess.run(U.linux_cmd(["bash", U.wsl_path(script)]), capture_output=True, text=True, cwd=ROOT)
    print(r.stdout + r.stderr)
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
