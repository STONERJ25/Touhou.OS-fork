#!/usr/bin/env python3
"""Table-driven instruction tests for the x86emu CPU core (user/lib/pc98), one test class per instruction group.

Expected values come from two independent places: tables worked out by hand from the instruction set definitions (Intel's
8086 family manual, NEC's V30 manual, Intel's 80286 manual), and the reference arithmetic in tests/host/x86ref.py,
which computes flags from their definitions (signed ranges, nibble carries) and shares no code with the C core. The
instructions are hand-assembled byte strings, with the assembly in a comment. Needs gcc (the driver tests/host/x86tool.c
is built with AddressSanitizer and UBSan); no nasm, no network.

    python3 -m unittest discover -s tests/host -p "test_x86*.py" -v
"""
import random
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from x86ref import (AF, ARITH, CF, DEFAULT, DF, IF, OF, PF, SF, TF, ZF, bcd, case_lines, have_gcc, lin, ref_alu,  # noqa: E402
                    ref_div, ref_incdec, ref_mul, ref_shift, run_script, unbcd)

CODE = lin(DEFAULT["cs"], DEFAULT["ip"])      # 10100h
DATA = 0x0200                                  # offset of memory operands in DS (3000h)
HANDLER_SEG = 0x5000                           # interrupt handlers live at 5000:vector*16 (a HLT each)


def modrm(mod, reg, rm):
    return (mod << 6) | (reg << 3) | rm


def ivt(*vectors):
    """Memory for an interrupt vector table whose entries point at 5000:(vec*16), where a HLT waits."""
    mem = {}
    for v in vectors:
        mem[v * 4] = bytes([(v * 16) & 0xFF, (v * 16) >> 8, HANDLER_SEG & 0xFF, HANDLER_SEG >> 8])
        mem[lin(HANDLER_SEG, v * 16)] = b"\xF4"
    return mem


class Batch:
    """Collects cases, runs them through one x86tool process, then calls each check with its result."""

    def __init__(self, tc, model="8086", defaults=None):
        self.tc, self.lines, self.checks, self.defaults = tc, ["model " + model], [], defaults or {}

    def add(self, code, check, **kw):
        kw["regs"] = {**self.defaults, **kw.get("regs", {})}
        self.lines += case_lines(code, **kw)
        self.checks.append(check)

    def run(self):
        results = run_script(self.lines)
        self.tc.assertEqual(len(results), len(self.checks))
        for chk, res in zip(self.checks, results):
            chk(res)


def flags_ok(tc, res, want, mask, what):
    got = res["flags"] & mask
    tc.assertEqual(got, want & mask, "%s: flags %04x want %04x (mask %04x)" % (what, res["flags"], want, mask))


@unittest.skipUnless(have_gcc(), "needs gcc")
class Arithmetic(unittest.TestCase):
    """ADD OR ADC SBB AND SUB XOR CMP in every encoding, against the reference model."""

    EDGE8 = [0, 1, 0x0F, 0x10, 0x7F, 0x80, 0x81, 0xFE, 0xFF]
    EDGE16 = [0, 1, 0x00FF, 0x0100, 0x7FFF, 0x8000, 0x8001, 0xFFFE, 0xFFFF]

    def test_alu_random(self):
        rnd = random.Random(1234)
        b = Batch(self)
        names = ["add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"]
        for op in range(8):
            for w in (0, 1):
                edge = self.EDGE16 if w else self.EDGE8
                pairs = [(x, y) for x in edge for y in edge] + [(rnd.getrandbits(8 << w), rnd.getrandbits(8 << w)) for _ in range(150)]
                for i, (x, y) in enumerate(pairs):
                    cf = rnd.getrandbits(1)
                    form = i % 6
                    fl = 0xF002 | cf | (rnd.getrandbits(1) * (ZF | SF | OF | AF | PF) & ~CF)
                    r, f, defined = ref_alu(op, x, y, cf, w)
                    regs = {"ax": x if w else (0xAB00 | x), "bx": y if w else (0xCD00 | y), "flags": fl}
                    mem = {}
                    if form == 0:      # op al/ax, bl/bx           (00+8*op /r, modrm 11 011 000)
                        code = [op * 8 + w, modrm(3, 3, 0)]
                        where = "reg"
                    elif form == 1:    # op bl/bx-as-reg, [mem]    (02+8*op /r, reg <- reg op mem): x in reg AX
                        code = [op * 8 + 2 + w, modrm(0, 0, 6), DATA & 0xFF, DATA >> 8]
                        mem = {lin(DEFAULT["ds"], DATA): bytes([y & 0xFF, y >> 8] if w else [y])}
                        where = "reg"
                    elif form == 2:    # op [mem], bl/bx           (00+8*op /r): x in memory
                        code = [op * 8 + w, modrm(0, 3, 6), DATA & 0xFF, DATA >> 8]
                        mem = {lin(DEFAULT["ds"], DATA): bytes([x & 0xFF, x >> 8] if w else [x])}
                        where = "mem"
                    elif form == 3:    # op al/ax, imm             (04+8*op)
                        code = [op * 8 + 4 + w] + ([y & 0xFF, y >> 8] if w else [y])
                        where = "reg"
                    elif form == 4:    # group 1: op r/m, imm      (80/81 /op, modrm 11 op 000)
                        code = [0x80 + w, modrm(3, op, 0)] + ([y & 0xFF, y >> 8] if w else [y])
                        where = "reg"
                    else:              # 83 /op: imm8 sign-extended (word), or 82 (byte alias of 80 on the 8086)
                        if w:
                            y8 = y & 0xFF
                            y = (y8 | 0xFF00) if y8 & 0x80 else y8
                            r, f, defined = ref_alu(op, x, y, cf, w)
                            code = [0x83, modrm(3, op, 0), y8]
                        else:
                            code = [0x82, modrm(3, op, 0), y]
                        where = "reg"
                    what = "%s%d %04x,%04x cf=%d form %d" % (names[op], 8 << w, x, y, cf, form)

                    def check(res, r=r, f=f, defined=defined, where=where, what=what, w=w, op=op, x=x, form=form):
                        want = x if op == 7 else r
                        if where == "mem":
                            a = lin(DEFAULT["ds"], DATA)
                            got = res.writes.get(a, x & 0xFF) | (res.writes.get(a + 1, x >> 8) << 8 if w else 0)
                        else:
                            got = res["ax"] if w else res["al"]
                            if not w:
                                self.assertEqual(res["ah"], 0xAB, what + ": AH clobbered")
                        self.assertEqual(got, want, what + ": result")
                        flags_ok(self, res, f, defined, what)
                    b.add(code, check, regs=regs, mem=mem)
        b.run()

    def test_inc_dec_neg_not(self):
        rnd = random.Random(5)
        b = Batch(self)
        vals16 = Arithmetic.EDGE16 + [rnd.getrandbits(16) for _ in range(60)]
        for x in vals16:
            for dec in (0, 1):
                r, f, d = ref_incdec(x, dec, 1)
                cf = rnd.getrandbits(1)
                # inc/dec cx (41 / 49): CF must survive
                b.add([0x48 + 1 if dec else 0x41], lambda res, r=r, f=f, d=d, cf=cf, x=x, dec=dec: (
                    self.assertEqual(res["cx"], r), flags_ok(self, res, f | cf, d | CF, "incdec16 %04x %d" % (x, dec))),
                    regs={"cx": x, "flags": 0xF002 | cf})
                r8, f8, d8 = ref_incdec(x & 0xFF, dec, 0)
                # inc/dec byte [mem] (FE /0, /1)
                b.add([0xFE, modrm(0, dec, 6), DATA & 0xFF, DATA >> 8],
                      lambda res, r8=r8, f8=f8, d8=d8, x=x, dec=dec: (
                          self.assertEqual(res.writes[lin(DEFAULT["ds"], DATA)], r8),
                          flags_ok(self, res, f8, d8, "incdec8 %02x %d" % (x & 0xFF, dec))),
                      mem={lin(DEFAULT["ds"], DATA): x & 0xFF})
            # neg dx (F7 /3): flags as SUB 0, x
            r, f, d = ref_alu(5, 0, x, 0, 1)
            b.add([0xF7, modrm(3, 3, 2)], lambda res, r=r, f=f, d=d, x=x: (
                self.assertEqual(res["dx"], r), flags_ok(self, res, f, d, "neg %04x" % x)), regs={"dx": x})
            # not si (F7 /2): no flags change
            b.add([0xF7, modrm(3, 2, 6)], lambda res, x=x: (
                self.assertEqual(res["si"], x ^ 0xFFFF), self.assertEqual(res["flags"], 0xF8D7)),
                regs={"si": x, "flags": 0xF8D7})
        b.run()

    def test_cbw_cwd(self):
        b = Batch(self)
        for al, ax_want in ((0x7F, 0x007F), (0x80, 0xFF80), (0x00, 0x0000), (0xFF, 0xFFFF)):
            b.add([0x98], lambda res, ax_want=ax_want: self.assertEqual(res["ax"], ax_want), regs={"ax": 0x5500 | al})
        for ax, dx_want in ((0x7FFF, 0), (0x8000, 0xFFFF), (0, 0), (0xFFFF, 0xFFFF)):
            b.add([0x99], lambda res, dx_want=dx_want: self.assertEqual(res["dx"], dx_want), regs={"ax": ax, "dx": 0x1234})
        b.run()


@unittest.skipUnless(have_gcc(), "needs gcc")
class Shifts(unittest.TestCase):
    """ROL ROR RCL RCR SHL SHR SAR with 1, CL and (186+) immediate counts."""

    def run_model(self, model):
        rnd = random.Random(77)
        b = Batch(self, model)
        mask_count = model != "8086"
        for op in (0, 1, 2, 3, 4, 5, 7):
            for w in (0, 1):
                vals = [0, 1, 0x80 << (8 * w), (0xFF if not w else 0xFFFF), 0x55, 0xAA] + [rnd.getrandbits(8 << w) for _ in range(12)]
                counts = list(range(0, 20)) + [31, 32, 33, 40] + ([255] if not mask_count else [255])
                for x in vals:
                    for cnt in counts:
                        cf = rnd.getrandbits(1)
                        fl = 0xF002 | cf | OF
                        r, f, d = ref_shift(op, x, cnt, cf, w, mask_count)
                        if cnt == 1:
                            code = [0xD0 + w, modrm(3, op, 3)]          # op bl/bx, 1
                        else:
                            code = [0xD2 + w, modrm(3, op, 3)]          # op bl/bx, cl
                        what = "%s %d grp2/%d %04x by %d cf=%d" % (model, 8 << w, op, x, cnt, cf)

                        def check(res, r=r, f=f, d=d, w=w, what=what, fl=fl):
                            self.assertEqual(res["bx"] if w else res["bl"], r, what)
                            if f is None:
                                self.assertEqual(res["flags"], fl, what + ": count 0 must not touch flags")
                            else:
                                flags_ok(self, res, f, d, what)
                        b.add(code, check, regs={"bx": x, "cx": cnt, "flags": fl})
                        if mask_count and cnt in (3, 17, 33):          # C0/C1: op r/m, imm8
                            b.add([0xC0 + w, modrm(3, op, 3), cnt], check, regs={"bx": x, "flags": fl})
        b.run()

    def test_8086(self):
        self.run_model("8086")

    def test_v30(self):
        self.run_model("v30")

    def test_286(self):
        self.run_model("286")

    def test_setmo_8086(self):
        # D0 /6 is SETMO on the 8086 (operand becomes all ones); SHL on the 186-class chips.
        b = Batch(self, "8086")
        b.add([0xD1, modrm(3, 6, 3)], lambda res: self.assertEqual(res["bx"], 0xFFFF), regs={"bx": 0x1234})
        b.add([0xD2, modrm(3, 6, 3)], lambda res: self.assertEqual(res["bl"], 0x34), regs={"bx": 0x1234, "cx": 0})
        b.run()
        b = Batch(self, "286")
        b.add([0xD1, modrm(3, 6, 3)], lambda res: self.assertEqual(res["bx"], 0x2468), regs={"bx": 0x1234})
        b.run()


@unittest.skipUnless(have_gcc(), "needs gcc")
class MulDiv(unittest.TestCase):
    """MUL IMUL DIV IDIV (byte and word), divide errors, the two-operand IMUL of the 186."""

    def run_model(self, model):
        rnd = random.Random(99)
        b = Batch(self, model)
        mem = ivt(0)
        for op in (4, 5, 6, 7):
            for w in (0, 1):
                cases = [(0, 0), (1, 1), (0xFF, 0xFF), (0x80, 0xFF), (0x7F, 2), (0x8000 if w else 0x80, 0xFFFF if w else 0xFF)]
                cases += [(rnd.getrandbits(32 if w else 16), rnd.getrandbits(16 if w else 8)) for _ in range(200)]
                cases += [(rnd.getrandbits(16 if w else 8), rnd.getrandbits(16 if w else 8) or 1) for _ in range(100)]
                for acc, src in cases:
                    ax, dx = acc & 0xFFFF, (acc >> 16) & 0xFFFF
                    code = [0xF6 + w, modrm(3, op, 3)]     # mul/imul/div/idiv bl/bx
                    what = "%s grp3/%d w%d acc=%x src=%x" % (model, op, w, acc, src)
                    if op in (4, 5):
                        (rax, rdx), f, d = ref_mul(op, ax if w else ax & 0xFF, src, w)

                        def check(res, rax=rax, rdx=rdx, f=f, d=d, w=w, what=what):
                            self.assertEqual(res["ax"], rax, what)
                            if w:
                                self.assertEqual(res["dx"], rdx, what)
                            flags_ok(self, res, f, d, what)
                    else:
                        num = (dx << 16 | ax) if w else ax
                        q = ref_div(op, num, src, w, model)

                        def check(res, q=q, w=w, what=what):
                            if q is None:
                                self.assertEqual((res["cs"], res["ip"]), (HANDLER_SEG, 0), what + ": divide error expected")
                                return
                            self.assertEqual(res["cs"], DEFAULT["cs"], what + ": unexpected divide error")
                            if w:
                                self.assertEqual((res["ax"], res["dx"]), q, what)
                            else:
                                self.assertEqual((res["al"], res["ah"]), q, what)
                    b.add(code, check, regs={"ax": ax, "dx": dx, "bx": src}, mem=mem)
        # The return address a divide error pushes: next instruction (8086, V30) or the DIV itself (286).
        want_ip = 0x100 if model == "286" else 0x102
        sp = DEFAULT["sp"] - 6
        b.add([0xF6, modrm(3, 6, 3)], lambda res: self.assertEqual(
            res.writes[lin(DEFAULT["ss"], sp)] | res.writes[lin(DEFAULT["ss"], sp + 1)] << 8, want_ip, model + " divide error IP"),
            regs={"ax": 0x1234, "bx": 0}, mem=mem)
        b.run()

    def test_8086(self):
        self.run_model("8086")

    def test_286(self):
        self.run_model("286")

    def test_v30(self):
        self.run_model("v30")

    def test_idiv_minimum_quotient(self):
        # -128 (byte) and -32768 (word) quotients: a divide error on the 8086, a result on the 286.
        for model, err in (("8086", True), ("286", False)):
            b = Batch(self, model)
            b.add([0xF6, modrm(3, 7, 3)], lambda res, err=err: self.assertEqual(res["cs"] == HANDLER_SEG, err, model),
                  regs={"ax": (-256) & 0xFFFF, "bx": 2}, mem=ivt(0))       # -256 / 2 = -128
            b.add([0xF7, modrm(3, 7, 3)], lambda res, err=err: self.assertEqual(res["cs"] == HANDLER_SEG, err, model),
                  regs={"ax": 0, "dx": 0xFFFF, "bx": 2}, mem=ivt(0))      # -65536 / 2 = -32768
            b.run()

    def test_rep_idiv_negates_on_8086(self):
        # An undocumented 8086 effect (reenigne; in the SingleStepTests notes): a REP prefix negates IDIV's quotient.
        b = Batch(self, "8086")
        b.add([0xF3, 0xF6, modrm(3, 7, 3)], lambda res: self.assertEqual(res["al"], (-50) & 0xFF), regs={"ax": 100, "bx": 2})
        b.run()
        b = Batch(self, "286")
        b.add([0xF3, 0xF6, modrm(3, 7, 3)], lambda res: self.assertEqual(res["al"], 50), regs={"ax": 100, "bx": 2})
        b.run()

    def test_imul_imm_186(self):
        rnd = random.Random(3)
        b = Batch(self, "v30")
        for _ in range(200):
            x, imm = rnd.getrandbits(16), rnd.getrandbits(16)
            sx = x - 0x10000 if x & 0x8000 else x
            si = imm - 0x10000 if imm & 0x8000 else imm
            p = sx * si
            over = not (-0x8000 <= p < 0x8000)
            # imul cx, bx, imm16 (69 /r: modrm 11 001 011)
            b.add([0x69, modrm(3, 1, 3), imm & 0xFF, imm >> 8], lambda res, p=p, over=over: (
                self.assertEqual(res["cx"], p & 0xFFFF),
                self.assertEqual(bool(res["flags"] & CF), over), self.assertEqual(bool(res["flags"] & OF), over)),
                regs={"bx": x})
        b.run()


@unittest.skipUnless(have_gcc(), "needs gcc")
class Decimal(unittest.TestCase):
    """DAA DAS AAA AAS AAM AAD: the arithmetic they exist for, and the model differences."""

    def test_daa_das_on_bcd_sums(self):
        b = Batch(self)
        for x in range(0, 100, 3):
            for y in range(0, 100, 7):
                for cin in (0, 1):
                    s = x + y + cin
                    # adc al, bl ; daa  (12 C3 27)
                    b.add([0x12, 0xC3, 0x27], lambda res, s=s, what=(x, y, cin): (
                        self.assertEqual(res["al"], bcd(s % 100), what), self.assertEqual(res["flags"] & CF, int(s >= 100), what)),
                        regs={"ax": bcd(x), "bx": bcd(y), "flags": 0xF002 | cin}, steps=2)
                    d = x - y - cin
                    # sbb al, bl ; das  (1A C3 2F)
                    b.add([0x1A, 0xC3, 0x2F], lambda res, d=d, what=(x, y, cin): (
                        self.assertEqual(res["al"], bcd(d % 100), what), self.assertEqual(res["flags"] & CF, int(d < 0), what)),
                        regs={"ax": bcd(x), "bx": bcd(y), "flags": 0xF002 | cin}, steps=2)
        b.run()

    def test_aaa_aas_unpacked(self):
        b = Batch(self)
        for x in range(10):
            for y in range(10):
                s = x + y
                # add al, bl ; aaa  (02 C3 37): AH counts the tens
                b.add([0x02, 0xC3, 0x37], lambda res, s=s: (self.assertEqual((res["ah"], res["al"]), (s // 10, s % 10)),
                                                             self.assertEqual(res["flags"] & CF, int(s >= 10))),
                      regs={"ax": x, "bx": y}, steps=2)
                d = x - y
                # sub al, bl ; aas  (2A C3 3F)
                b.add([0x2A, 0xC3, 0x3F], lambda res, d=d: (self.assertEqual((res["ah"], res["al"]), ((d // 10) & 0xFF, d % 10)),
                                                             self.assertEqual(res["flags"] & CF, int(d < 0))),
                      regs={"ax": x, "bx": y}, steps=2)
        b.run()

    def test_aaa_carry_into_ah_differs(self):
        # AL = FAh, AF = 0: the 8086 adds 6 to AL and 1 to AH separately (AX = 0100h), the 286 adds 106h to AX (0200h).
        for model, want in (("8086", 0x0100), ("286", 0x0200)):
            b = Batch(self, model)
            b.add([0x37], lambda res, want=want: self.assertEqual(res["ax"], want, model), regs={"ax": 0x00FA})
            b.run()

    def test_aam_aad(self):
        b = Batch(self)
        for al in (0, 9, 10, 56, 99, 255):
            for base in (10, 16, 7):
                b.add([0xD4, base], lambda res, al=al, base=base: self.assertEqual((res["ah"], res["al"]), (al // base, al % base)),
                      regs={"ax": 0x1200 | al})
                ah = al % 13
                b.add([0xD5, base], lambda res, al=al, base=base, ah=ah: self.assertEqual(res["ax"], (ah * base + al) & 0xFF),
                      regs={"ax": ah << 8 | al})
        b.add([0xD4, 0x00], lambda res: self.assertEqual(res["cs"], HANDLER_SEG, "AAM 0 is a divide error"), mem=ivt(0))
        b.run()
        b = Batch(self, "v30")       # the NEC chips ignore the immediate and always use 10
        b.add([0xD5, 0x10], lambda res: self.assertEqual(res["ax"], 0x000B), regs={"ax": 0x0101})
        b.add([0xD4, 0x10], lambda res: self.assertEqual(res["ax"], 0x0305), regs={"ax": 0x0023})
        b.run()


@unittest.skipUnless(have_gcc(), "needs gcc")
class FlagsAndTransfer(unittest.TestCase):
    """Flag instructions, MOV in every addressing mode, XCHG, LEA, LDS/LES, XLAT, segment registers."""

    def test_flag_instructions(self):
        b = Batch(self)
        table = [  # (code, flags before, flags after)
            ([0xF8], 0xF0D7, 0xF0D6),   # clc
            ([0xF9], 0xF002, 0xF003),   # stc
            ([0xF5], 0xF003, 0xF002),   # cmc
            ([0xF5], 0xF002, 0xF003),
            ([0xFA], 0xF202, 0xF002),   # cli
            ([0xFB], 0xF002, 0xF202),   # sti
            ([0xFC], 0xF402, 0xF002),   # cld
            ([0xFD], 0xF002, 0xF402),   # std
        ]
        for code, before, after in table:
            b.add(code, lambda res, after=after, code=code: self.assertEqual(res["flags"], after, bytes(code).hex()),
                  regs={"flags": before})
        # sahf: AH = D5h sets SF ZF AF PF CF (bits 1, 3, 5 of AH are ignored)
        b.add([0x9E], lambda res: self.assertEqual(res["flags"], 0xF8D7), regs={"ax": 0xFF00, "flags": 0xF802})
        b.add([0x9E], lambda res: self.assertEqual(res["flags"], 0xF802), regs={"ax": 0x0000, "flags": 0xF8D7})
        # lahf
        b.add([0x9F], lambda res: self.assertEqual(res["ah"], 0xD7), regs={"flags": 0xF8D7})
        b.run()

    def test_pushf_popf_reserved_bits(self):
        # popf of 0000h: the 8086 and V30 read back F002h, the 286 in real mode 0002h.
        for model, want in (("8086", 0xF002), ("v30", 0xF002), ("286", 0x0002)):
            b = Batch(self, model)
            sp = DEFAULT["sp"]
            b.add([0x9D, 0x9C], lambda res, want=want: (
                self.assertEqual(res["flags"], want, model),
                self.assertEqual(res.writes[lin(DEFAULT["ss"], sp)] | res.writes[lin(DEFAULT["ss"], sp + 1)] << 8, want)),
                regs={"sp": sp}, mem={lin(DEFAULT["ss"], sp): b"\x00\x00"}, steps=2)
            b.run()

    def test_addressing_modes(self):
        # mov ax, [ea] for every mod/rm with byte and word displacements; BP-based forms use SS.
        regs = {"bx": 0x1000, "bp": 0x2000, "si": 0x0300, "di": 0x0040}
        bases = [regs["bx"] + regs["si"], regs["bx"] + regs["di"], regs["bp"] + regs["si"], regs["bp"] + regs["di"],
                 regs["si"], regs["di"], regs["bp"], regs["bx"]]
        b = Batch(self)
        for rm in range(8):
            for mod, disp in ((0, 0), (1, 0x7F), (1, -0x10), (2, 0x1234)):
                if mod == 0 and rm == 6:
                    ea, seg, dbytes = 0x5678, DEFAULT["ds"], [0x78, 0x56]
                else:
                    ea = (bases[rm] + disp) & 0xFFFF
                    seg = DEFAULT["ss"] if rm in (2, 3, 6) else DEFAULT["ds"]
                    dbytes = [] if mod == 0 else [disp & 0xFF] if mod == 1 else [disp & 0xFF, (disp >> 8) & 0xFF]
                for ovr, oseg in ((None, seg), (0x26, DEFAULT["es"]), (0x2E, DEFAULT["cs"]), (0x36, DEFAULT["ss"]), (0x3E, DEFAULT["ds"])):
                    code = ([ovr] if ovr else []) + [0x8B, modrm(mod, 0, rm)] + dbytes
                    a = lin(oseg, ea)
                    if DEFAULT["cs"] == oseg and lin(oseg, ea) in range(CODE, CODE + 8):
                        continue
                    val = (rm * 16 + mod * 4 + len(dbytes)) * 0x0101 ^ 0x5A00
                    b.add(code, lambda res, val=val, what=(rm, mod, disp, ovr): self.assertEqual(res["ax"], val, what),
                          regs=regs, mem={a: bytes([val & 0xFF, val >> 8])})
        b.run()

    def test_word_access_wraps_in_segment(self):
        # mov ax, [FFFFh]: the high byte comes from offset 0 of the same segment.
        b = Batch(self)
        b.add([0xA1, 0xFF, 0xFF], lambda res: self.assertEqual(res["ax"], 0xBBAA),
              mem={lin(DEFAULT["ds"], 0xFFFF): 0xAA, lin(DEFAULT["ds"], 0): 0xBB})
        b.run()

    def test_address_wraps_at_1mib(self):
        # FFFF:0010 is physical 0 on the 8086 and on a 286 with A20 closed; 100000h with A20 open.
        b = Batch(self)
        b.add([0x26, 0xA0, 0x10, 0x00], lambda res: self.assertEqual(res["al"], 0x42), regs={"es": 0xFFFF}, mem={0: 0x42})
        b.run()
        lines = ["model 286", "a20 1"] + case_lines([0x26, 0xA0, 0x10, 0x00], regs={"es": 0xFFFF}, mem={0: 0x42, 0x100000: 0x99})
        self.assertEqual(run_script(lines)[0]["al"], 0x99)

    def test_moves_and_exchanges(self):
        b = Batch(self)
        d = lin(DEFAULT["ds"], DATA)
        # mov [0200], 1234h (C7 06 00 02 34 12)
        b.add([0xC7, 0x06, 0x00, 0x02, 0x34, 0x12], lambda res: self.assertEqual((res.writes[d], res.writes[d + 1]), (0x34, 0x12)))
        # mov byte [0200], 56h (C6 06 00 02 56)
        b.add([0xC6, 0x06, 0x00, 0x02, 0x56], lambda res: self.assertEqual(res.writes[d], 0x56))
        # mov dh, 0E7h (B6 E7); mov sp, 1234h (BC 34 12)
        b.add([0xB6, 0xE7], lambda res: self.assertEqual(res["dx"], 0xE7CC), regs={"dx": 0x11CC})
        b.add([0xBC, 0x34, 0x12], lambda res: self.assertEqual(res["sp"], 0x1234))
        # mov al, [0200] / mov [0200], ax (A0 / A3)
        b.add([0xA0, 0x00, 0x02], lambda res: self.assertEqual(res["al"], 0x9C), mem={d: 0x9C})
        b.add([0xA3, 0x00, 0x02], lambda res: self.assertEqual((res.writes[d], res.writes[d + 1]), (0xEF, 0xBE)), regs={"ax": 0xBEEF})
        # xchg ax, bx (93); xchg cl, ch (86 E9); xchg [0200], dx (87 16 00 02)
        b.add([0x93], lambda res: self.assertEqual((res["ax"], res["bx"]), (2, 1)), regs={"ax": 1, "bx": 2})
        b.add([0x86, 0xE9], lambda res: self.assertEqual(res["cx"], 0x3412), regs={"cx": 0x1234})
        b.add([0x87, 0x16, 0x00, 0x02], lambda res: (self.assertEqual(res["dx"], 0x2211),
                                                     self.assertEqual((res.writes[d], res.writes[d + 1]), (0x44, 0x33))),
              regs={"dx": 0x3344}, mem={d: b"\x11\x22"})
        # lea si, [bp+di-2] (8D 73 FE): no memory access, no segment
        b.add([0x8D, 0x73, 0xFE], lambda res: self.assertEqual(res["si"], 0x3001), regs={"bp": 0x3000, "di": 3})
        # les di, [0200] (C4 3E 00 02) / lds si, [0200] (C5 36 00 02)
        b.add([0xC4, 0x3E, 0x00, 0x02], lambda res: self.assertEqual((res["di"], res["es"]), (0x1234, 0xABCD)),
              mem={d: b"\x34\x12\xCD\xAB"})
        b.add([0xC5, 0x36, 0x00, 0x02], lambda res: self.assertEqual((res["si"], res["ds"]), (0x1234, 0xABCD)),
              mem={d: b"\x34\x12\xCD\xAB"})
        # xlat (D7): AL = [DS:BX+AL]; with an ES override (26 D7)
        b.add([0xD7], lambda res: self.assertEqual(res["al"], 0x77), regs={"bx": DATA, "ax": 5}, mem={d + 5: 0x77})
        b.add([0x26, 0xD7], lambda res: self.assertEqual(res["al"], 0x66), regs={"bx": DATA, "ax": 5},
              mem={lin(DEFAULT["es"], DATA + 5): 0x66})
        # mov es, ax (8E C0); mov dx, ss (8C D2); 8E with reg=4..7 on the 8086 uses reg & 3 (8E E0 = mov es, ax)
        b.add([0x8E, 0xC0], lambda res: self.assertEqual(res["es"], 0x1357), regs={"ax": 0x1357})
        b.add([0x8C, 0xD2], lambda res: self.assertEqual(res["dx"], DEFAULT["ss"]))
        b.add([0x8E, 0xE0], lambda res: self.assertEqual(res["es"], 0x2468), regs={"ax": 0x2468})
        # salc (D6, undocumented): AL = CF ? FF : 00
        b.add([0xD6], lambda res: self.assertEqual(res["al"], 0xFF), regs={"flags": 0xF003})
        b.add([0xD6], lambda res: self.assertEqual(res["al"], 0x00), regs={"ax": 0x55, "flags": 0xF002})
        b.run()


@unittest.skipUnless(have_gcc(), "needs gcc")
class StackAndControl(unittest.TestCase):
    """PUSH/POP, jumps, calls, returns, conditional jumps and loops."""

    SP = 0x1000

    def test_push_pop(self):
        sp = self.SP
        ss = DEFAULT["ss"]
        b = Batch(self, defaults={"sp": sp})
        b.add([0x53], lambda res: (self.assertEqual(res["sp"], sp - 2),
                                   self.assertEqual((res.writes[lin(ss, sp - 2)], res.writes[lin(ss, sp - 1)]), (0x34, 0x12))),
              regs={"bx": 0x1234})
        b.add([0x5F], lambda res: (self.assertEqual(res["di"], 0xBEEF), self.assertEqual(res["sp"], sp + 2)),
              mem={lin(ss, sp): b"\xEF\xBE"})
        b.add([0x1E], lambda res: self.assertEqual(res.writes[lin(ss, sp - 2)] | res.writes[lin(ss, sp - 1)] << 8, DEFAULT["ds"]))
        b.add([0x07], lambda res: self.assertEqual(res["es"], 0x4321), mem={lin(ss, sp): b"\x21\x43"})
        # push word [0200] (FF 36 00 02); pop word [0202] (8F 06 02 02)
        d = lin(DEFAULT["ds"], DATA)
        b.add([0xFF, 0x36, 0x00, 0x02], lambda res: self.assertEqual(res.writes[lin(ss, sp - 2)], 0x9A), mem={d: b"\x9A\x00"})
        b.add([0x8F, 0x06, 0x02, 0x02], lambda res: self.assertEqual((res.writes[d + 2], res.writes[d + 3]), (0x11, 0x22)),
              mem={lin(ss, sp): b"\x11\x22"})
        # pop cs (0F) exists only on the 8086
        b.add([0x0F], lambda res: self.assertEqual((res["cs"], res["ip"]), (0x2345, 0x0101)), mem={lin(ss, sp): b"\x45\x23"})
        b.run()
        # push sp: the 8086 and V30 store the decremented SP, the 286 the old one
        for model, want in (("8086", sp - 2), ("v30", sp - 2), ("286", sp)):
            b = Batch(self, model, defaults={"sp": sp})
            b.add([0x54], lambda res, want=want: self.assertEqual(
                res.writes[lin(ss, sp - 2)] | res.writes[lin(ss, sp - 1)] << 8, want, model))
            b.run()

    def test_jumps_calls_returns(self):
        sp, ss = self.SP, DEFAULT["ss"]
        b = Batch(self, defaults={"sp": sp})
        b.add([0xEB, 0x10], lambda res: self.assertEqual(res["ip"], 0x112))                     # jmp short +10h
        b.add([0xEB, 0xFE], lambda res: self.assertEqual(res["ip"], 0x100))                     # jmp $
        b.add([0xE9, 0x00, 0x10], lambda res: self.assertEqual(res["ip"], 0x1103))              # jmp near +1000h
        b.add([0xEA, 0x78, 0x56, 0x34, 0x12], lambda res: self.assertEqual((res["cs"], res["ip"]), (0x1234, 0x5678)))
        b.add([0xFF, 0xE3], lambda res: self.assertEqual(res["ip"], 0x4444), regs={"bx": 0x4444})   # jmp bx
        b.add([0xFF, 0x2E, 0x00, 0x02], lambda res: self.assertEqual((res["cs"], res["ip"]), (0x2222, 0x1111)),
              mem={lin(DEFAULT["ds"], DATA): b"\x11\x11\x22\x22"})                                  # jmp far [0200]
        b.add([0xE8, 0x00, 0x01], lambda res: (self.assertEqual(res["ip"], 0x203), self.assertEqual(res["sp"], sp - 2),
                                               self.assertEqual(res.writes[lin(ss, sp - 2)], 0x03)))  # call near
        b.add([0x9A, 0x00, 0x00, 0x00, 0x60], lambda res: (
            self.assertEqual((res["cs"], res["ip"], res["sp"]), (0x6000, 0, sp - 4)),
            self.assertEqual([res.writes[lin(ss, sp - 4 + i)] for i in range(4)], [0x05, 0x01, 0x00, 0x10])))  # call far
        b.add([0xFF, 0x1E, 0x00, 0x02], lambda res: self.assertEqual((res["cs"], res["ip"]), (0x2222, 0x1111)),
              mem={lin(DEFAULT["ds"], DATA): b"\x11\x11\x22\x22"})                                  # call far [0200]
        b.add([0xC3], lambda res: (self.assertEqual(res["ip"], 0x0789), self.assertEqual(res["sp"], sp + 2)),
              mem={lin(ss, sp): b"\x89\x07"})                                                       # ret
        b.add([0xC2, 0x04, 0x00], lambda res: self.assertEqual(res["sp"], sp + 6), mem={lin(ss, sp): b"\x89\x07"})  # ret 4
        b.add([0xCB], lambda res: self.assertEqual((res["cs"], res["ip"], res["sp"]), (0x0BCD, 0x0789, sp + 4)),
              mem={lin(ss, sp): b"\x89\x07\xCD\x0B"})                                               # retf
        b.add([0xCA, 0x02, 0x00], lambda res: self.assertEqual(res["sp"], sp + 6), mem={lin(ss, sp): b"\x89\x07\xCD\x0B"})
        b.add([0xC9], lambda res: self.assertEqual((res["cs"], res["ip"]), (0x0BCD, 0x0789)),
              mem={lin(ss, sp): b"\x89\x07\xCD\x0B"})                                               # C9 = retf on the 8086
        b.add([0xC1], lambda res: self.assertEqual(res["ip"], 0x0789), mem={lin(ss, sp): b"\x89\x07"})  # C1 = ret on the 8086
        b.run()

    def test_conditional_jumps(self):
        conds = {
            0: lambda f: f & OF, 1: lambda f: not f & OF, 2: lambda f: f & CF, 3: lambda f: not f & CF,
            4: lambda f: f & ZF, 5: lambda f: not f & ZF, 6: lambda f: f & (CF | ZF), 7: lambda f: not f & (CF | ZF),
            8: lambda f: f & SF, 9: lambda f: not f & SF, 10: lambda f: f & PF, 11: lambda f: not f & PF,
            12: lambda f: bool(f & SF) != bool(f & OF), 13: lambda f: bool(f & SF) == bool(f & OF),
            14: lambda f: f & ZF or bool(f & SF) != bool(f & OF), 15: lambda f: not (f & ZF) and bool(f & SF) == bool(f & OF),
        }
        b = Batch(self)
        for cc, fn in conds.items():
            for fl in range(0, 64):
                flags = 0xF002 | (CF if fl & 1 else 0) | (PF if fl & 2 else 0) | (ZF if fl & 4 else 0) | (SF if fl & 8 else 0) | \
                    (OF if fl & 16 else 0) | (AF if fl & 32 else 0)
                taken = bool(fn(flags))
                for op in ([0x70 + cc] if True else []) + [0x60 + cc]:     # 60-6F: aliases on the 8086
                    b.add([op, 0x20], lambda res, taken=taken, w=(cc, flags): self.assertEqual(res["ip"], 0x122 if taken else 0x102, w),
                          regs={"flags": flags})
        b.run()

    def test_loops(self):
        b = Batch(self)
        table = [  # (opcode, cx, flags, taken, cx after)
            (0xE2, 5, 0xF002, True, 4), (0xE2, 1, 0xF002, False, 0), (0xE2, 0, 0xF002, True, 0xFFFF),
            (0xE1, 5, 0xF042, True, 4), (0xE1, 5, 0xF002, False, 4), (0xE1, 1, 0xF042, False, 0),
            (0xE0, 5, 0xF002, True, 4), (0xE0, 5, 0xF042, False, 4),
            (0xE3, 0, 0xF002, True, 0), (0xE3, 3, 0xF002, False, 3),
        ]
        for op, cx, fl, taken, after in table:
            b.add([op, 0xFC], lambda res, taken=taken, after=after, w=(op, cx, fl): (
                self.assertEqual(res["ip"], 0xFE if taken else 0x102, w), self.assertEqual(res["cx"], after, w)),
                regs={"cx": cx, "flags": fl})
        b.run()


@unittest.skipUnless(have_gcc(), "needs gcc")
class Strings(unittest.TestCase):
    """MOVS CMPS SCAS LODS STOS with and without REP prefixes, both directions, overrides."""

    def test_string_ops(self):
        ds, es = DEFAULT["ds"], DEFAULT["es"]
        src = bytes(range(0x40, 0x60))
        b = Batch(self)
        # rep movsb, CX=10, forwards
        b.add([0xF3, 0xA4], lambda res: (
            self.assertEqual(bytes(res.writes[lin(es, 0x100 + i)] for i in range(10)), src[:10]),
            self.assertEqual((res["cx"], res["si"], res["di"]), (0, 0x10A, 0x10A)),
            self.assertEqual(res.instructions, 10)),
            regs={"cx": 10, "si": 0x100, "di": 0x100}, mem={lin(ds, 0x100): src})
        # rep movsw backwards (DF=1), CX=3: words at SI=104, 102, 100
        b.add([0xF3, 0xA5], lambda res: (
            self.assertEqual(bytes(res.writes[lin(es, 0x200 + i)] for i in range(6)), src[:6]),
            self.assertEqual((res["si"], res["di"]), (0xFE, 0x1FE))),
            regs={"cx": 3, "si": 0x104, "di": 0x204, "flags": 0xF402}, mem={lin(ds, 0x100): src})
        # rep with CX=0 does nothing
        b.add([0xF3, 0xA4], lambda res: (self.assertEqual(res.writes, {}), self.assertEqual(res["ip"], 0x102)),
              regs={"cx": 0, "si": 0x100, "di": 0x100})
        # movsb with an ES: override reads ES:SI
        b.add([0x26, 0xA4], lambda res: self.assertEqual(res.writes[lin(es, 0x300)], 0x99),
              regs={"si": 0x50, "di": 0x300}, mem={lin(es, 0x50): 0x99, lin(ds, 0x50): 0x11})
        # repe cmpsb stops at the first difference (index 3): CX = 5 - 4, ZF = 0
        b.add([0xF3, 0xA6], lambda res: (self.assertEqual((res["cx"], res["si"]), (1, 0x104)), self.assertFalse(res["flags"] & ZF)),
              regs={"cx": 5, "si": 0x100, "di": 0x100}, mem={lin(ds, 0x100): b"abcde", lin(es, 0x100): b"abcXe"})
        # repne scasb finds 'k' at index 2: DI = 103h, ZF = 1
        b.add([0xF2, 0xAE], lambda res: (self.assertEqual((res["cx"], res["di"]), (7, 0x103)), self.assertTrue(res["flags"] & ZF)),
              regs={"cx": 10, "di": 0x100, "ax": ord("k")}, mem={lin(es, 0x100): b"aakaaaaaaa"})
        # rep stosw, lodsb, scasw without rep
        b.add([0xF3, 0xAB], lambda res: self.assertEqual(bytes(res.writes[lin(es, 0x10 + i)] for i in range(4)), b"\xCD\xAB\xCD\xAB"),
              regs={"cx": 2, "di": 0x10, "ax": 0xABCD})
        b.add([0xAC], lambda res: (self.assertEqual(res["al"], 0x5A), self.assertEqual(res["si"], 0x21)),
              regs={"si": 0x20}, mem={lin(ds, 0x20): 0x5A})
        b.add([0xAF], lambda res: (self.assertTrue(res["flags"] & ZF), self.assertEqual(res["di"], 0x32)),
              regs={"di": 0x30, "ax": 0x1234}, mem={lin(es, 0x30): b"\x34\x12"})
        # SI and DI wrap within the segment
        b.add([0xA4], lambda res: (self.assertEqual(res.writes[lin(es, 0xFFFF)], 0x77), self.assertEqual((res["si"], res["di"]), (0, 0))),
              regs={"si": 0xFFFF, "di": 0xFFFF}, mem={lin(ds, 0xFFFF): 0x77})
        b.run()

    def test_rep_interrupted_resumes_at_prefix(self):
        # An interrupt between iterations pushes the address of the first prefix (ES: REP MOVSB at 100h).
        ss, sp = DEFAULT["ss"], DEFAULT["sp"]
        lines = ["model 8086"] + case_lines([0x26, 0xF3, 0xA4], regs={"cx": 4, "si": 0, "di": 0x400, "flags": 0xF202},
                                            mem=ivt(0x20), steps=0)[:-2]
        lines += ["step 1", "irq 20", "step 1"]
        r1, r2 = run_script(lines)
        self.assertEqual(r1["cx"], 0, "the tool steps a whole REP instruction")
        # a REP with an interrupt raised before it starts: the interrupt comes first, the REP has not begun
        lines = ["model 8086"] + case_lines([0x26, 0xF3, 0xA4], regs={"cx": 4, "si": 0, "di": 0x400, "flags": 0xF202},
                                            mem=ivt(0x20), irq=0x20)
        r = run_script(lines)[0]
        self.assertEqual((r["cs"], r["ip"]), (HANDLER_SEG, 0x200))
        self.assertEqual(r.writes[lin(ss, sp - 6)] | r.writes[lin(ss, sp - 5)] << 8, 0x100)


@unittest.skipUnless(have_gcc(), "needs gcc")
class Interrupts(unittest.TestCase):
    """INT, INT3, INTO, IRET, external interrupts, interrupt shadows, HLT, I/O instructions."""

    def test_int_iret(self):
        ss, sp = DEFAULT["ss"], DEFAULT["sp"]
        b = Batch(self)
        # int 21h: pushes FLAGS, CS, IP (next instruction); clears IF
        b.add([0xCD, 0x21], lambda res: (
            self.assertEqual((res["cs"], res["ip"]), (HANDLER_SEG, 0x210)),
            self.assertEqual(res["flags"] & (IF | TF), 0),
            self.assertEqual([res.writes[lin(ss, sp - 6 + i)] for i in range(6)], [0x02, 0x01, 0x00, 0x10, 0x03, 0xF2])),
            regs={"flags": 0xF203}, mem=ivt(0x21))
        b.add([0xCC], lambda res: self.assertEqual((res["cs"], res["ip"]), (HANDLER_SEG, 0x30)), mem=ivt(3))
        b.add([0xCE], lambda res: self.assertEqual((res["cs"], res["ip"]), (HANDLER_SEG, 0x40)), regs={"flags": 0xF802}, mem=ivt(4))
        b.add([0xCE], lambda res: self.assertEqual(res["ip"], 0x101), regs={"flags": 0xF002}, mem=ivt(4))
        # iret
        b.add([0xCF], lambda res: self.assertEqual((res["ip"], res["cs"], res["flags"], res["sp"]), (0x1234, 0x5678, 0xF2D7, 0x1006)),
              regs={"sp": 0x1000}, mem={lin(ss, 0x1000): b"\x34\x12\x78\x56\xD7\x02"})
        b.run()

    def test_external_interrupt(self):
        b = Batch(self)
        # IF=1: the interrupt is taken before the instruction (a NOP here)
        b.add([0x90], lambda res: self.assertEqual((res["cs"], res["ip"]), (HANDLER_SEG, 0x80)),
              regs={"flags": 0xF202}, mem=ivt(8), irq=8)
        # IF=0: ignored, the NOP runs
        b.add([0x90], lambda res: self.assertEqual(res["ip"], 0x101), regs={"flags": 0xF002}, mem=ivt(8), irq=8)
        b.run()
        # STI shadow: sti; nop; nop with an interrupt pending -> taken after the first NOP (IP 102h pushed)
        ss, sp = DEFAULT["ss"], DEFAULT["sp"]
        lines = ["model 8086"] + case_lines([0xFB, 0x90, 0x90], regs={"flags": 0xF002}, mem=ivt(8), irq=8, steps=3)
        r = run_script(lines)[0]
        self.assertEqual((r["cs"], r["ip"]), (HANDLER_SEG, 0x80))
        self.assertEqual(r.writes[lin(ss, sp - 6)] | r.writes[lin(ss, sp - 5)] << 8, 0x102)
        # MOV SS shadow: mov ss, ax; mov sp, 100h; nop. An interrupt raised after the MOV SS waits until the MOV SP is done.
        lines = ["model 8086"] + case_lines([0x8E, 0xD0, 0xBC, 0x00, 0x01, 0x90], regs={"flags": 0xF202, "ax": 0x7000},
                                            mem=ivt(8), steps=1)[:-1]
        lines += ["irq 8", "step 1", "step 1"]
        r1, r2, r3 = run_script(lines)
        self.assertEqual((r2["cs"], r2["ip"], r2["ss"], r2["sp"]), (DEFAULT["cs"], 0x105, 0x7000, 0x100))
        self.assertEqual((r3["cs"], r3["ip"], r3["sp"]), (HANDLER_SEG, 0x80, 0xFA))
        # HLT then an interrupt: the pushed IP is the instruction after HLT
        lines = ["model 8086"] + case_lines([0xF4, 0x90], regs={"flags": 0xF202}, mem=ivt(8), steps=1)[:-1]
        lines += ["irq 8", "step 1"]
        r1, r2 = run_script(lines)
        self.assertEqual(r1.halted, 1)
        self.assertEqual((r2["cs"], r2["ip"], r2.halted), (HANDLER_SEG, 0x80, 0))

    def test_single_step_trap(self):
        b = Batch(self)
        b.add([0x90], lambda res: (self.assertEqual((res["cs"], res["ip"]), (HANDLER_SEG, 0x10)),
                                   self.assertEqual(res["flags"] & TF, 0)),
              regs={"flags": 0xF302}, mem=ivt(1))
        b.run()

    def test_io(self):
        b = Batch(self)
        b.add([0xE4, 0x40], lambda res: self.assertEqual(res["al"], 0x5C), ports={0x40: 0x5C})        # in al, 40h
        b.add([0xEC], lambda res: self.assertEqual(res["al"], 0x3D), regs={"dx": 0x3FDB}, ports={0x3FDB: 0x3D})  # in al, dx
        b.add([0xE5, 0x60], lambda res: self.assertEqual(res["ax"], 0xFFFF))                            # in ax, 60h: nothing there
        b.add([0xE6, 0x37], lambda res: self.assertEqual(res.outs, [(0x37, 0x06, 1)]), regs={"ax": 0x06})  # out 37h, al
        b.add([0xEF], lambda res: self.assertEqual(res.outs, [(0xA8, 0x1234, 2)]), regs={"ax": 0x1234, "dx": 0xA8})  # out dx, ax
        b.run()
        b = Batch(self, "v30")
        b.add([0xF3, 0x6E], lambda res: self.assertEqual(res.outs, [(0x60, 0x41, 1), (0x60, 0x42, 1), (0x60, 0x43, 1)]),
              regs={"cx": 3, "si": 0x10, "dx": 0x60}, mem={lin(DEFAULT["ds"], 0x10): b"ABC"})          # rep outsb
        b.add([0x6C], lambda res: self.assertEqual(res.writes[lin(DEFAULT["es"], 0x20)], 0x7E),
              regs={"di": 0x20, "dx": 0x41}, ports={0x41: 0x7E})                                       # insb
        b.run()


@unittest.skipUnless(have_gcc(), "needs gcc")
class Models(unittest.TestCase):
    """What sets the V30 and the 286 apart, and what the core refuses to do."""

    def test_286_invalid_opcode_and_msw(self):
        ss, sp = DEFAULT["ss"], DEFAULT["sp"]
        b = Batch(self, "286")
        for code in ([0x63, 0xC0], [0xFF, 0xF8], [0x8D, 0xC0], [0x0F, 0x00, 0xC0], [0x8E, 0xC8]):
            b.add(code, lambda res, code=code: (
                self.assertEqual((res["cs"], res["ip"]), (HANDLER_SEG, 0x60), bytes(code).hex()),
                self.assertEqual(res.writes[lin(ss, sp - 6)], 0x00, "INT 6 points at the faulting instruction")),
                mem=ivt(6))
        b.add([0x0F, 0x01, 0xE0], lambda res: self.assertEqual(res["ax"], 0xFFF0))                      # smsw ax
        b.add([0x0F, 0x01, 0xF0], lambda res: (self.assertEqual(res.stop, 1), self.assertEqual(res["ip"], 0x103)),
              regs={"ax": 1})                                                                           # lmsw ax with PE
        b.add([0x62, 0x06, 0x00, 0x02], lambda res: self.assertEqual((res["cs"], res["ip"]), (HANDLER_SEG, 0x50)),
              regs={"ax": 11}, mem={**ivt(5), lin(DEFAULT["ds"], DATA): b"\x00\x00\x0A\x00"})            # bound ax: 11 > 10
        b.run()

    def test_8086_stops_on_what_it_cannot_do(self):
        b = Batch(self, "8086")
        b.add([0xFE, 0xD0], lambda res: (self.assertEqual(res.stop, 3), self.assertEqual(res["ip"], 0x100)))  # FE /2
        b.run()

    def test_v30_bit_instructions(self):
        b = Batch(self, "v30")
        d = lin(DEFAULT["ds"], DATA)
        # test1 bl, cl (0F 10 C3): ZF = bit clear
        b.add([0x0F, 0x10, 0xC3], lambda res: self.assertTrue(res["flags"] & ZF), regs={"bx": 0xF7, "cx": 3})
        b.add([0x0F, 0x10, 0xC3], lambda res: self.assertFalse(res["flags"] & ZF), regs={"bx": 0x08, "cx": 3})
        # set1 word [0200], 15 (0F 1D 06 00 02 0F); clr1 bx, cl (0F 13 C3); not1 bl, 0 (0F 1E C3 00)
        b.add([0x0F, 0x1D, 0x06, 0x00, 0x02, 0x0F], lambda res: self.assertEqual(res.writes[d + 1], 0x80))
        b.add([0x0F, 0x13, 0xC3], lambda res: self.assertEqual(res["bx"], 0xFFFE - 0x8000 + 1), regs={"bx": 0xFFFF, "cx": 15})
        b.add([0x0F, 0x1E, 0xC3, 0x00], lambda res: self.assertEqual(res["bx"], 0x0001), regs={"bx": 0x0000})
        # rol4 bl (0F 28 C3): BL = BL << 4 | AL low nibble, AL low nibble = old BL high nibble
        b.add([0x0F, 0x28, 0xC3], lambda res: (self.assertEqual(res["bl"], 0x4A), self.assertEqual(res["al"], 0x93)),
              regs={"bx": 0x34, "ax": 0x9A})
        # ror4 bl (0F 2A C3)
        b.add([0x0F, 0x2A, 0xC3], lambda res: (self.assertEqual(res["bl"], 0xA3), self.assertEqual(res["al"], 0x94)),
              regs={"bx": 0x34, "ax": 0x9A})
        # repc cmpsb (65 A6): repeats while CF = 1 (stops after the third byte)
        b.add([0x65, 0xA6], lambda res: self.assertEqual(res["cx"], 1), regs={"cx": 4, "si": 0, "di": 0},
              mem={lin(DEFAULT["ds"], 0): b"\x01\x05\x09", lin(DEFAULT["es"], 0): b"\x02\x06\x03"})
        b.add([0x0F, 0x20], lambda res: self.assertEqual(res.stop, 3))                                  # ADD4S: not done
        b.run()


@unittest.skipUnless(have_gcc(), "needs gcc")
class Cycles(unittest.TestCase):
    """The cycle counter follows the 8086 data sheet for simple cases."""

    def test_counts(self):
        b = Batch(self)
        table = [
            ([0x90], 3),                              # nop
            ([0xB8, 0x34, 0x12], 4),                  # mov ax, imm
            ([0x89, 0xD8], 2),                        # mov ax, bx
            ([0x8B, 0x40, 0x10], 8 + 11),             # mov ax, [bx+si+10h]: 8 + EA 7+4
            ([0x26, 0x8B, 0x07], 2 + 8 + 5 + 2),      # es: mov ax, [bx]: prefix 2, 8, EA 5, override 2
            ([0x01, 0x07], 16 + 5),                   # add [bx], ax
            ([0xF3, 0xA4], 2 + 9 + 17 * 4),           # rep movsb, CX = 4
            ([0xEB, 0x00], 15),                       # jmp short
            ([0xCD, 0x10], 51),                       # int 10h
        ]
        for code, want in table:
            b.add(code, lambda res, want=want, code=code: self.assertEqual(res.cycles, want, bytes(code).hex()),
                  regs={"cx": 4, "bx": 0x10, "si": 0x20}, mem=ivt(0x10))
        b.run()


if __name__ == "__main__":
    unittest.main()
