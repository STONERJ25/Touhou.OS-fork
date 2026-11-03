"""Helpers for the x86emu host tests: build and drive tests/host/x86tool.c, and a small reference model of the x86
arithmetic written from the instruction set definitions (signed and unsigned ranges, nibble carries, the documented
OF formulas), deliberately not from the C code: it shares no tricks with user/lib/pc98/src/x86cpu.c or x86cpu32.c.

Operand sizes are given as w: 0 byte, 1 word, 2 doubleword (the 386's 32-bit operands). The 386 additions (SHLD/SHRD,
the bit tests and scans, BSWAP, the truncating IMUL) follow the 80386 and i486 programmer's reference manuals.
"""
import os
import shutil
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
def _in_wsl():
    try:
        return "microsoft" in os.uname().release.lower()
    except AttributeError:
        return False


OUT = REPO / "build" / ("host-x86-wsl" if _in_wsl() else "host-x86")           # (Linux binaries built under WSL must not be mistaken for a native build's)
LIB = REPO / "user" / "lib" / "pc98"

CF, PF, AF, ZF, SF, TF, IF, DF, OF = 0x1, 0x4, 0x10, 0x40, 0x80, 0x100, 0x200, 0x400, 0x800
AC = 0x40000                                    # 486: EFLAGS bit 18
ARITH = CF | PF | AF | ZF | SF | OF
REGS = ["ax", "cx", "dx", "bx", "sp", "bp", "si", "di", "es", "cs", "ss", "ds", "ip", "flags"]
R8 = ["al", "cl", "dl", "bl", "ah", "ch", "dh", "bh"]
R16 = ["ax", "cx", "dx", "bx", "sp", "bp", "si", "di"]
R32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
XREGS = R32 + ["fs", "gs", "eflags", "cr0"]      # x86tool's "X" line (and, without cr0, its "x" command)

# Where the tests put things: code at CS:IP = 1000:0100, stack at 2000:FFFE, data at DS = 3000, ES = 4000.
DEFAULT = dict(ax=0, cx=0, dx=0, bx=0, sp=0xFFFE, bp=0, si=0, di=0, es=0x4000, cs=0x1000, ss=0x2000, ds=0x3000,
               ip=0x0100, flags=0xF002)


def _in_dirs(name, dirs):
    for d in dirs:
        if d:
            for ext in ("", ".exe"):
                f = Path(d) / (name + ext)
                if f.is_file():
                    return str(f)
    return None


def find_cc():
    """A C compiler for the host tools: gcc, else clang from the PATH, else (on Windows) the clang that tools/build.py uses (TOUHOU_LLVM_DIR, then C:\\Program Files\\LLVM\\bin)."""
    for name in ("gcc", "clang"):
        found = shutil.which(name)
        if found:
            return found
    return _in_dirs("clang", [os.environ.get("TOUHOU_LLVM_DIR"), r"C:\Program Files\LLVM\bin"]) if os.name == "nt" else None


def find_nasm():
    """nasm from the PATH, TOUHOU_NASM (the program or its folder), or the Windows install folder tools/build.py looks in."""
    env = os.environ.get("TOUHOU_NASM")
    if env and Path(env).is_file():
        return env
    return shutil.which("nasm") or _in_dirs("nasm", [env, os.path.expandvars(r"%LOCALAPPDATA%\bin\NASM")])


def have_gcc():
    """(The name is old: any C compiler that find_cc() finds.)"""
    return find_cc() is not None


def build(name, srcs, sanitize=True):
    """Builds a host tool from the given sources plus every libpc98 source. Returns its path."""
    OUT.mkdir(parents=True, exist_ok=True)
    exe = OUT / name
    lib_srcs = sorted((LIB / "src").glob("*.c"))
    deps = list(srcs) + lib_srcs + sorted((LIB / "include").glob("*.h")) + sorted((LIB / "src").glob("*.h"))
    if exe.exists() and all(d.stat().st_mtime <= exe.stat().st_mtime for d in deps):
        return exe
    cc = find_cc()
    is_gcc = "gcc" in Path(cc).name.lower()
    # gcc gets AddressSanitizer, UBSan and -Werror; clang on Windows (no sanitizer runtime on the PATH, different warnings) builds plainly unless TOUHOU_SANITIZE=1 asks for ASan
    san = sanitize and (is_gcc or os.environ.get("TOUHOU_SANITIZE") == "1")
    flags = ["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"] if san else ["-O2", "-g"]
    warn = ["-Wall", "-Wextra", "-Werror"] if is_gcc else ["-Wall", "-Wextra"]
    cmd = [cc, *flags, "-std=gnu17", *warn, "-Wno-unused-parameter", "-Wno-sign-compare",
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
        self.ext = {}                           # the "X" line: eax..edi, fs, gs, eflags, cr0
        self.writes = {}
        self.outs = []
        self.dumps = []

    def __getitem__(self, k):
        k = k.lower()
        if k in self.regs:
            return self.regs[k]
        if k in self.ext:
            return self.ext[k]
        if k in R8:
            i = R8.index(k)
            v = self.regs[R16[i & 3]]
            return (v >> 8) & 0xFF if i >= 4 else v & 0xFF
        raise KeyError(k)

    def mem(self, addr, n, default=None):
        """n bytes the instruction wrote at addr, as a little-endian number (default: the bytes it did not write)."""
        v = 0
        for i in range(n):
            b = self.writes.get(addr + i, None if default is None else (default >> (8 * i)) & 0xFF)
            if b is None:
                raise KeyError("%x not written" % (addr + i))
            v |= b << (8 * i)
        return v


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
        elif line.startswith("X ") and cur is not None:
            cur.ext = dict(zip(XREGS, (int(x, 16) for x in line.split()[1:])))
        elif line.startswith("W "):
            _, a, v = line.split()
            cur.writes[int(a, 16)] = int(v, 16)
        elif line.startswith("D") and cur is not None:
            cur.dumps.append(bytes(int(x, 16) for x in line.split()[1:]))
    return out


def case_lines(code, regs=None, mem=None, ports=None, steps=1, run=False, irq=None, model=None):
    """Script lines for one test: registers (DEFAULT updated by regs), code at CS:IP, extra memory {addr: bytes}.
    386/486: regs may also name eax..edi, fs, gs, eflags (a 32-bit name wins over its 16-bit half), cr0 and idt
    ((base, limit))."""
    lines = []
    if model:
        lines.append("model " + model)
    rg = dict(DEFAULT)
    if regs:
        rg.update({k.lower(): v for k, v in regs.items()})
    for r16, r32 in zip(R16, R32):
        if r32 in rg:
            rg[r16] = rg[r32] & 0xFFFF
    if "eflags" in rg:
        rg["flags"] = rg["eflags"] & 0xFFFF
    lines.append("r " + " ".join("%x" % (rg[k] & 0xFFFF) for k in REGS))
    if any(k in rg for k in R32 + ["fs", "gs", "eflags"]):
        full = [rg.get(r32, rg[r16]) for r16, r32 in zip(R16, R32)]
        lines.append("x " + " ".join("%x" % (v & 0xFFFFFFFF) for v in full) +
                     " %x %x %x" % (rg.get("fs", 0), rg.get("gs", 0), rg.get("eflags", rg["flags"])))
    if "cr0" in rg:
        lines.append("cr 0 %x" % rg["cr0"])
    if "idt" in rg:
        lines.append("idt %x %x" % rg["idt"])
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
    """w: 0 byte, 1 word, 2 doubleword."""
    return 8 << int(w)


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
    """Group 2 (0 ROL 1 ROR 2 RCL 3 RCR 4 SHL 5 SHR 7 SAR) with closed forms. Returns (result, flags, defined); for a
    count of zero (after the 186+ masking to five bits) (a, None, 0)."""
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
    """op 4 MUL, 5 IMUL. acc = AX (byte: AL used), DX:AX or EDX:EAX. Returns ((low, high), flags, defined): low is AX,
    AX or EAX and high is None, DX or EDX."""
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
    if w == 2:
        return (p & 0xFFFFFFFF, p >> 32), f, CF | OF
    if w:
        return (p & 0xFFFF, p >> 16), f, CF | OF
    return (p & 0xFFFF, None), f, CF | OF


def ref_div(op, num, src, w, model="8086"):
    """op 6 DIV, 7 IDIV. num = AX (byte), DX:AX (word) or EDX:EAX (doubleword). Returns (quotient, remainder) or None
    for a divide error. From the 286 on, the most negative quotient is allowed."""
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
    lo = -(1 << (n - 1)) if model in ("286", "386", "486") else -(1 << (n - 1)) + 1
    if not (lo <= q < (1 << (n - 1))):
        return None
    m = (1 << n) - 1
    return q & m, r & m


def ref_imul_trunc(a, b, w):
    """IMUL r, r/m and IMUL r, r/m, imm: (the product truncated to the operand size, CF=OF set when it did not fit)."""
    p = signed(a, w) * signed(b, w)
    r = p & ((1 << bits_of(w)) - 1)
    return r, (CF | OF) if signed(r, w) != p else 0


def ref_shxd(left, dst, src, count, w):
    """SHLD (left) / SHRD: count masked to 5 bits; returns (result, flags, defined) or (dst, None, 0) for a zero count.
    The definitions: the destination shifted, the vacated bits filled from the source; CF the last bit shifted out of
    the destination; OF (a single-bit shift only) set when the sign changed; SF ZF PF from the result. Counts above the
    operand size (16-bit operands) are undefined and not modelled here."""
    n = bits_of(w)
    m = (1 << n) - 1
    count &= 31
    if count == 0:
        return dst, None, 0
    assert count <= n, "undefined count"
    if left:
        full = ((dst << n) | src) << count
        r = (full >> n) & m
        c = (full >> (2 * n)) & 1
    else:
        full = (src << n) | dst
        r = (full >> count) & m
        c = (full >> (count - 1)) & 1
    f = szp(r, w) | (CF if c else 0)
    defined = CF | SF | ZF | PF
    if count == 1:
        defined |= OF
        if (r ^ dst) >> (n - 1) & 1:
            f |= OF
    return r, f, defined


def ref_bit(kind, value, bit, w):
    """BT (0), BTS (1), BTR (2), BTC (3) on a register-sized value: (new value, CF)."""
    n = bits_of(w)
    bit %= n
    cf = (value >> bit) & 1
    new = [value, value | (1 << bit), value & ~(1 << bit), value ^ (1 << bit)][kind] & ((1 << n) - 1)
    return new, cf


def ref_bitstring(base_off, bitoff, w, addr16=True):
    """Where BT* with a memory operand and the bit number in a register look: (offset of the word or doubleword, bit in
    it). The bit number is signed (operand size); the operand is the one at base + size * floor(bitoff / width)."""
    n = bits_of(w)
    s = signed(bitoff, w)
    off = base_off + (n // 8) * (s // n)          # Python's // floors, as the instruction does
    return (off & 0xFFFF) if addr16 else (off & 0xFFFFFFFF), s % n


def ref_bsf(v, w, reverse=False):
    """BSF / BSR: (index or None for a zero source, ZF)."""
    if v == 0:
        return None, ZF
    if reverse:
        return v.bit_length() - 1, 0
    return (v & -v).bit_length() - 1, 0


def ref_bswap(v):
    return int.from_bytes((v & 0xFFFFFFFF).to_bytes(4, "little"), "big")


def ref_cond(cc, f):
    """Is condition cc (the low nibble of Jcc/SETcc) true for flags f?"""
    sf, of = bool(f & SF), bool(f & OF)
    r = [f & OF, f & CF, f & ZF, f & (CF | ZF), f & SF, f & PF, sf != of, (f & ZF) or sf != of][cc >> 1]
    return bool(r) != bool(cc & 1)


def bcd(v):
    return ((v // 10) << 4) | (v % 10)


def unbcd(v):
    return (v >> 4) * 10 + (v & 15)
