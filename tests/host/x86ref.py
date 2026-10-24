"""Helpers for the x86emu host tests: build and drive tests/host/x86tool.c, and a small reference model of the 8086's
arithmetic written from the instruction set definitions (signed and unsigned ranges, nibble carries, the documented
OF formulas), deliberately not from the C code: it shares no tricks with user/lib/pc98/src/x86cpu.c.
"""
import os
import shutil
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
OUT = REPO / "build" / "host-x86"
LIB = REPO / "user" / "lib" / "pc98"

CF, PF, AF, ZF, SF, TF, IF, DF, OF = 0x1, 0x4, 0x10, 0x40, 0x80, 0x100, 0x200, 0x400, 0x800
ARITH = CF | PF | AF | ZF | SF | OF
REGS = ["ax", "cx", "dx", "bx", "sp", "bp", "si", "di", "es", "cs", "ss", "ds", "ip", "flags"]
R8 = ["al", "cl", "dl", "bl", "ah", "ch", "dh", "bh"]
R16 = ["ax", "cx", "dx", "bx", "sp", "bp", "si", "di"]

# Where the tests put things: code at CS:IP = 1000:0100, stack at 2000:FFFE, data at DS = 3000, ES = 4000.
DEFAULT = dict(ax=0, cx=0, dx=0, bx=0, sp=0xFFFE, bp=0, si=0, di=0, es=0x4000, cs=0x1000, ss=0x2000, ds=0x3000,
               ip=0x0100, flags=0xF002)


def have_gcc():
    return shutil.which("gcc") is not None


def build(name, srcs, sanitize=True):
    """Builds a host tool from the given sources plus every libpc98 source. Returns its path."""
    OUT.mkdir(parents=True, exist_ok=True)
    exe = OUT / name
    lib_srcs = sorted((LIB / "src").glob("*.c"))
    deps = list(srcs) + lib_srcs + sorted((LIB / "include").glob("*.h")) + sorted((LIB / "src").glob("*.h"))
    if exe.exists() and all(d.stat().st_mtime <= exe.stat().st_mtime for d in deps):
        return exe
    flags = ["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"] if sanitize else ["-O2"]
    cmd = ["gcc", *flags, "-std=gnu17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-sign-compare",
           "-I", str(LIB / "include"), *map(str, srcs), *map(str, lib_srcs), "-o", str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("build of %s failed:\n%s%s" % (name, r.stdout, r.stderr))
    return exe


def tool():
    return build("x86tool", [REPO / "tests" / "host" / "x86tool.c"])


def lin(seg, off):
    return ((seg << 4) + off) & 0xFFFFF


class Result:
    def __init__(self, line):
        v = [int(x, 16) for x in line.split()[1:]]
        self.regs = dict(zip(REGS, v[:14]))
        self.cycles, self.halted, self.stop, self.instructions = v[14], v[15], v[16], v[17]
        self.writes = {}
        self.outs = []
        self.dumps = []

    def __getitem__(self, k):
        k = k.lower()
        if k in self.regs:
            return self.regs[k]
        if k in R8:
            i = R8.index(k)
            v = self.regs[R16[i & 3]]
            return (v >> 8) & 0xFF if i >= 4 else v & 0xFF


def run_script(lines):
    """Runs x86tool on a list of command lines; returns the Results (one per step/run) in order."""
    r = subprocess.run([str(tool())], input="\n".join(lines) + "\n", capture_output=True, text=True,
                       env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
    if r.returncode != 0:
        raise RuntimeError("x86tool failed (%d): %s" % (r.returncode, r.stderr[-2000:]))
    out, cur, pend_outs = [], None, []
    for line in r.stdout.splitlines():
        if line.startswith("O "):
            _, p, v, w = line.split()
            pend_outs.append((int(p, 16), int(v, 16), int(w)))
        elif line.startswith("R "):
            cur = Result(line)
            cur.outs, pend_outs = pend_outs, []
            out.append(cur)
        elif line.startswith("W "):
            _, a, v = line.split()
            cur.writes[int(a, 16)] = int(v, 16)
        elif line.startswith("D") and cur is not None:
            cur.dumps.append(bytes(int(x, 16) for x in line.split()[1:]))
    return out


def case_lines(code, regs=None, mem=None, ports=None, steps=1, run=False, irq=None, model=None):
    """Script lines for one test: registers (DEFAULT updated by regs), code at CS:IP, extra memory {addr: bytes}."""
    lines = []
    if model:
        lines.append("model " + model)
    rg = dict(DEFAULT)
    if regs:
        rg.update({k.lower(): v for k, v in regs.items()})
    lines.append("r " + " ".join("%x" % (rg[k] & 0xFFFF) for k in REGS))
    if code:
        lines.append("m %x %s" % (lin(rg["cs"], rg["ip"]), " ".join("%x" % b for b in code)))
    for a, data in (mem or {}).items():
        if isinstance(data, int):
            data = bytes([data])
        lines.append("m %x %s" % (a, " ".join("%x" % b for b in data)))
    for p, v in (ports or {}).items():
        lines.append("p %x %x" % (p, v))
    if irq is not None:
        lines.append("irq %x" % irq)
    lines.append(("run %x" if run else "step %x") % steps)
    lines.append("clear")
    return lines


# ------------------------------------------------------------------ reference arithmetic

def bits_of(w):
    return 16 if w else 8


def signed(v, w):
    b = bits_of(w)
    return v - (1 << b) if v & (1 << (b - 1)) else v


def szp(r, w):
    f = 0
    if r == 0:
        f |= ZF
    if r >> (bits_of(w) - 1):
        f |= SF
    if bin(r & 0xFF).count("1") % 2 == 0:
        f |= PF
    return f


def ref_alu(op, a, b, cf, w):
    """op: 0 ADD 1 OR 2 ADC 3 SBB 4 AND 5 SUB 6 XOR 7 CMP. Returns (result, flags, defined-mask)."""
    n = bits_of(w)
    m = (1 << n) - 1
    if op in (0, 2):
        c = cf if op == 2 else 0
        full = a + b + c
        r = full & m
        f = szp(r, w)
        if full > m:
            f |= CF
        if not (-(1 << (n - 1)) <= signed(a, w) + signed(b, w) + c < (1 << (n - 1))):
            f |= OF
        if (a & 15) + (b & 15) + c > 15:
            f |= AF
        return r, f, ARITH
    if op in (3, 5, 7):
        c = cf if op == 3 else 0
        r = (a - b - c) & m
        f = szp(r, w)
        if a < b + c:
            f |= CF
        if not (-(1 << (n - 1)) <= signed(a, w) - signed(b, w) - c < (1 << (n - 1))):
            f |= OF
        if (a & 15) < (b & 15) + c:
            f |= AF
        return r, f, ARITH
    r = {1: a | b, 4: a & b, 6: a ^ b}[op]
    return r, szp(r, w), ARITH & ~AF      # AF undefined after logic ops


def ref_incdec(a, dec, w):
    n = bits_of(w)
    m = (1 << n) - 1
    r = (a - 1 if dec else a + 1) & m
    f = szp(r, w)
    sr = signed(a, w) + (-1 if dec else 1)
    if not (-(1 << (n - 1)) <= sr < (1 << (n - 1))):
        f |= OF
    if (dec and (a & 15) == 0) or (not dec and (a & 15) == 15):
        f |= AF
    return r, f, ARITH & ~CF


def ref_shift(op, a, count, cf, w, mask_count):
    """Group 2 (0 ROL 1 ROR 2 RCL 3 RCR 4 SHL 5 SHR 7 SAR) with closed forms. Returns (result, flags, defined, cf_in_flags)."""
    n = bits_of(w)
    m = (1 << n) - 1
    if mask_count:
        count &= 31
    if count == 0:
        return a, None, 0
    msb = lambda v: (v >> (n - 1)) & 1
    if op == 0:
        k = count % n
        r = ((a << k) | (a >> (n - k))) & m if k else a
        c = r & 1
        o = msb(r) ^ c
    elif op == 1:
        k = count % n
        r = ((a >> k) | (a << (n - k))) & m if k else a
        c = msb(r)
        o = msb(r) ^ ((r >> (n - 2)) & 1)
    elif op in (2, 3):
        k = count % (n + 1)
        big = (cf << n) | a                   # n+1 bit quantity: carry above the operand
        mm = (1 << (n + 1)) - 1
        if op == 2:
            big = ((big << k) | (big >> (n + 1 - k))) & mm if k else big
        else:
            big = ((big >> k) | (big << (n + 1 - k))) & mm if k else big
        r, c = big & m, big >> n
        o = msb(r) ^ c if op == 2 else msb(r) ^ ((r >> (n - 2)) & 1)
    elif op == 4:
        r = (a << count) & m
        c = (a >> (n - count)) & 1 if count <= n else 0
        o = msb(r) ^ c
    elif op == 5:
        r = a >> count if count < n else 0
        c = (a >> (count - 1)) & 1 if count <= n else 0
        o = msb(a) if count == 1 else 0
    else:
        sa = signed(a, w)
        k = min(count, n)
        r = (sa >> k) & m
        c = (sa >> (k - 1)) & 1
        o = 0
    f = (CF if c else 0) | (OF if o else 0)
    defined = CF | OF
    if op >= 4:
        f |= szp(r, w)
        defined |= SF | ZF | PF
    return r, f, defined


def ref_mul(op, acc, src, w):
    """op 4 MUL, 5 IMUL. acc = AX (byte: AL used) or DX:AX. Returns ((ax, dx), flags, defined)."""
    n = bits_of(w)
    m = (1 << n) - 1
    if op == 4:
        p = (acc & m) * src
        hi = p >> n
        over = hi != 0
    else:
        p = signed(acc & m, w) * signed(src, w)
        over = not (-(1 << (n - 1)) <= p < (1 << (n - 1)))
        p &= (1 << (2 * n)) - 1
    f = (CF | OF) if over else 0
    if w:
        return (p & 0xFFFF, p >> 16), f, CF | OF
    return (p & 0xFFFF, None), f, CF | OF


def ref_div(op, num, src, w, model="8086"):
    """op 6 DIV, 7 IDIV. num = AX (byte) or DX:AX (word). Returns (quotient, remainder) or None for a divide error."""
    n = bits_of(w)
    if src == 0:
        return None
    if op == 6:
        q, r = divmod(num, src)
        return None if q >> n else (q, r)
    sn = num - (1 << (2 * n)) if num >> (2 * n - 1) else num
    sd = signed(src, w)
    q = abs(sn) // abs(sd)
    if (sn < 0) != (sd < 0):
        q = -q
    r = sn - q * sd
    lo = -(1 << (n - 1)) if model == "286" else -(1 << (n - 1)) + 1
    if not (lo <= q < (1 << (n - 1))):
        return None
    m = (1 << n) - 1
    return q & m, r & m


def bcd(v):
    return ((v // 10) << 4) | (v % 10)


def unbcd(v):
    return (v >> 4) * 10 + (v & 15)
