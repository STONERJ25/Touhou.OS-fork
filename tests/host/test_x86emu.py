#!/usr/bin/env python3
"""Table-driven instruction tests for the x86emu CPU core (user/lib/pc98), one test class per instruction group.

Expected values come from two independent places: tables worked out by hand from the instruction set definitions (Intel's
8086 family manual, NEC's V30 manual, Intel's 80286, 80386 and i486 manuals), and the reference arithmetic in
tests/host/x86ref.py, which computes flags from their definitions (signed ranges, nibble carries) and shares no code with
the C core. The instructions are hand-assembled byte strings, with the assembly in a comment. Needs gcc (the driver
tests/host/x86tool.c is built with AddressSanitizer and UBSan); no nasm, no network. The classes named ...386 cover the
80386 and 80486 models; the hardware behaviours the manuals leave open are checked by tests/host/x86sst.py --cpu 386.

    python3 -m unittest discover -s tests/host -p "test_x86*.py" -v
"""
import random
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from x86ref import (AC, AF, ARITH, CF, DEFAULT, DF, IF, OF, PF, SF, TF, ZF, bcd, case_lines, have_gcc, lin,  # noqa: E402
                    ref_alu, ref_bit, ref_bitstring, ref_bsf, ref_bswap, ref_cond, ref_div, ref_imul_trunc, ref_incdec,
                    ref_mul, ref_shift, ref_shxd, run_script, signed, unbcd)

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


# ====================================================================== 80386 / 80486 (real mode)

M386 = ("386", "486")
F386 = 0x0002                                  # the FLAGS these tests start a 386 with (a 386 cannot hold bit 15)
SS0, DS0, ES0 = DEFAULT["ss"], DEFAULT["ds"], DEFAULT["es"]
R32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
EDGE32 = [0, 1, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFE,
          0xFFFFFFFF]


def le(v, n):
    """v as n little-endian bytes."""
    return list((v & ((1 << (8 * n)) - 1)).to_bytes(n, "little"))


def sib(scale, index, base):
    return (scale << 6) | (index << 3) | base


def fault_at(tc, res, vec, what, ip=0x100):
    """The instruction at 1000:ip raised exception vec: its handler (5000:vec*16) runs and the frame holds that IP."""
    tc.assertEqual((res["cs"], res["ip"]), (HANDLER_SEG, vec * 16), "%s: exception %d expected" % (what, vec))
    tc.assertEqual(res.mem(lin(SS0, res["sp"]), 2), ip, what + ": pushed IP")


def no_fault(tc, res, what):
    tc.assertNotEqual(res["cs"], HANDLER_SEG, "%s: unexpected exception (handler offset %04x)" % (what, res["ip"]))


def reg_of(res, w, name):
    """The 8-, 16- or 32-bit view of a register named by its 32-bit name (ebx -> bl/bx/ebx)."""
    if w == 2:
        return res[name]
    return res[name[1:]] if w == 1 else res[name[1] + "l"]


@unittest.skipUnless(have_gcc(), "needs gcc")
class Arithmetic386(unittest.TestCase):
    """32-bit ALU operations (66h) in every form, INC/DEC/NEG/NOT, against the reference model; 8- and 16-bit operations
    on the 386 and 486 leave the upper halves alone."""

    def test_alu32(self):
        d = lin(DS0, DATA)
        for model in M386:
            rnd = random.Random(386)
            b = Batch(self, model)
            for op in range(8):
                pairs = [(x, y) for x in EDGE32 for y in EDGE32[::3]] + \
                        [(rnd.getrandbits(32), rnd.getrandbits(32)) for _ in range(40)]
                for i, (x, y) in enumerate(pairs):
                    cf = rnd.getrandbits(1)
                    form = i % 6
                    if form == 5:                       # an imm8, sign-extended to 32 bits
                        y = (y & 0xFF) | (0xFFFFFF00 if y & 0x80 else 0)
                    r, f, defined = ref_alu(op, x, y, cf, 2)
                    regs, mem, where = {"eax": x, "ebx": y, "eflags": F386 | cf}, {}, "reg"
                    if form == 0:                       # op eax, ebx       (66 01+8*op /r, modrm 11 011 000)
                        code = [0x66, op * 8 + 1, modrm(3, 3, 0)]
                    elif form == 1:                     # op eax, [DATA]    (66 03+8*op /r)
                        code = [0x66, op * 8 + 3, modrm(0, 0, 6)] + le(DATA, 2)
                        mem = {d: bytes(le(y, 4))}
                    elif form == 2:                     # op [DATA], ebx    (66 01+8*op /r)
                        code = [0x66, op * 8 + 1, modrm(0, 3, 6)] + le(DATA, 2)
                        mem, where = {d: bytes(le(x, 4))}, "mem"
                    elif form == 3:                     # op eax, imm32     (66 05+8*op id)
                        code = [0x66, op * 8 + 5] + le(y, 4)
                    elif form == 4:                     # op eax, imm32     (66 81 /op id)
                        code = [0x66, 0x81, modrm(3, op, 0)] + le(y, 4)
                    else:                               # op eax, imm8      (66 83 /op ib)
                        code = [0x66, 0x83, modrm(3, op, 0), y & 0xFF]
                    what = "%s alu32/%d %08x,%08x cf=%d form %d" % (model, op, x, y, cf, form)

                    def check(res, r=r, f=f, defined=defined, where=where, what=what, op=op, x=x):
                        want = x if op == 7 else r
                        got = res.mem(d, 4, x) if where == "mem" else res["eax"]
                        self.assertEqual(got, want, what + ": result")
                        flags_ok(self, res, f, defined, what)
                    b.add(code, check, regs=regs, mem=mem)
            b.run()

    def test_upper_halves_survive(self):
        table = [  # (code, registers, memory, register, value after)
            ([0xB8, 0x34, 0x12], {"eax": 0xAAAA5555}, {}, "eax", 0xAAAA1234),               # mov ax, 1234h
            ([0xB4, 0x77], {"eax": 0xAAAA5555}, {}, "eax", 0xAAAA7755),                     # mov ah, 77h
            ([0x04, 0x01], {"eax": 0x123456FF}, {}, "eax", 0x12345600),                     # add al, 1
            ([0x41], {"ecx": 0x8000FFFF}, {}, "ecx", 0x80000000),                           # inc cx
            ([0x49], {"ecx": 0x80000000}, {}, "ecx", 0x8000FFFF),                           # dec cx
            ([0x66, 0x41], {"ecx": 0x8000FFFF}, {}, "ecx", 0x80010000),                     # inc ecx
            ([0x93], {"eax": 0x11112222, "ebx": 0x33334444}, {}, "eax", 0x11114444),        # xchg ax, bx
            ([0x98], {"eax": 0xDEAD0080}, {}, "eax", 0xDEADFF80),                           # cbw
            ([0x66, 0x98], {"eax": 0xDEAD8000}, {}, "eax", 0xFFFF8000),                     # cwde
            ([0x99], {"eax": 0x8000, "edx": 0x12345678}, {}, "edx", 0x1234FFFF),            # cwd
            ([0x66, 0x99], {"eax": 0x80000000, "edx": 0x12345678}, {}, "edx", 0xFFFFFFFF),  # cdq
            ([0xF7, 0xE3], {"eax": 0x55550002, "ebx": 0x99990003, "edx": 0x7777FFFF}, {}, "edx", 0x77770000),  # mul bx
            ([0x5B], {"ebx": 0xCAFE0000, "sp": 0x1000}, {lin(SS0, 0x1000): b"\xEF\xBE"}, "ebx", 0xCAFEBEEF),   # pop bx
            ([0x8D, 0x47, 0x10], {"eax": 0xFFFF0000, "ebx": 0x20}, {}, "eax", 0xFFFF0030),  # lea ax, [bx+10h]
            ([0xAD], {"eax": 0x99990000, "si": 0x10}, {lin(DS0, 0x10): b"\x11\x22"}, "eax", 0x99992211),      # lodsw
        ]
        for model in M386:
            b = Batch(self, model)
            for code, regs, mem, reg, want in table:
                b.add(code, lambda res, reg=reg, want=want, code=code: self.assertEqual(
                    res[reg], want, "%s %s: %s=%08x" % (model, bytes(code).hex(), reg, res[reg])), regs=regs, mem=mem)
            b.run()

    def test_inc_dec_neg_not32(self):
        for model in M386:
            rnd = random.Random(7)
            b = Batch(self, model)
            for x in EDGE32 + [rnd.getrandbits(32) for _ in range(30)]:
                for dec in (0, 1):
                    r, f, d = ref_incdec(x, dec, 2)
                    cf = rnd.getrandbits(1)
                    # inc/dec ecx (66 41 / 66 49): CF must survive
                    b.add([0x66, 0x49 if dec else 0x41], lambda res, r=r, f=f, d=d, cf=cf, x=x: (
                        self.assertEqual(res["ecx"], r), flags_ok(self, res, f | cf, d | CF, "incdec32 %08x" % x)),
                        regs={"ecx": x, "eflags": F386 | cf})
                r, f, d = ref_alu(5, 0, x, 0, 2)
                # neg edx (66 F7 /3), not esi (66 F7 /2)
                b.add([0x66, 0xF7, modrm(3, 3, 2)], lambda res, r=r, f=f, d=d, x=x: (
                    self.assertEqual(res["edx"], r), flags_ok(self, res, f, d, "neg32 %08x" % x)), regs={"edx": x})
                b.add([0x66, 0xF7, modrm(3, 2, 6)], lambda res, x=x: (
                    self.assertEqual(res["esi"], x ^ 0xFFFFFFFF), self.assertEqual(res["flags"], 0x08D7)),
                    regs={"esi": x, "eflags": 0x08D7})
            b.run()


@unittest.skipUnless(have_gcc(), "needs gcc")
class Shifts386(unittest.TestCase):
    """Group 2 at 8, 16 and 32 bits (the count masked to five bits), and SHLD/SHRD."""

    def test_group2(self):
        for model in M386:
            rnd = random.Random(1386)
            b = Batch(self, model)
            for op in (0, 1, 2, 3, 4, 5, 7):
                for w in (0, 1, 2):
                    n = 8 << w
                    m = (1 << n) - 1
                    vals = [0, 1, 1 << (n - 1), m, 0x55555555 & m, 0xAAAAAAAA & m] + [rnd.getrandbits(n) for _ in range(5)]
                    for x in vals:
                        for cnt in (0, 1, 2, 7, 8, 9, 15, 16, 17, 31, 32, 33, 40, 255):
                            cf = rnd.getrandbits(1)
                            fl = F386 | cf | OF
                            r, f, d = ref_shift(op, x, cnt, cf, w, True)
                            mc = cnt & 31
                            if f is not None:
                                if mc != 1:
                                    d &= ~OF                    # OF is defined for single-bit shifts only
                                if op in (4, 5) and mc >= n:
                                    d &= ~CF                    # and CF of SHL/SHR only while the count is below the width
                            pre = [0x66] if w == 2 else []
                            what = "%s grp2/%d w%d %x by %d cf=%d" % (model, op, w, x, cnt, cf)

                            def check(res, r=r, f=f, d=d, w=w, what=what, fl=fl):
                                self.assertEqual(reg_of(res, w, "ebx"), r, what)
                                if f is None:
                                    self.assertEqual(res["flags"], fl, what + ": a zero count must not touch the flags")
                                else:
                                    flags_ok(self, res, f, d, what)
                            # op bl/bx/ebx, cl (D2/D3 /op, modrm 11 op 011)
                            b.add(pre + [0xD2 + (w > 0), modrm(3, op, 3)], check, regs={"ebx": x, "ecx": cnt, "eflags": fl})
                            if cnt in (1, 3, 17, 33):        # op r/m, imm8 (C0/C1), and the shift-by-1 forms (D0/D1)
                                b.add(pre + [0xC0 + (w > 0), modrm(3, op, 3), cnt], check, regs={"ebx": x, "eflags": fl})
                                if cnt == 1:
                                    b.add(pre + [0xD0 + (w > 0), modrm(3, op, 3)], check, regs={"ebx": x, "eflags": fl})
            b.run()

    def test_shld_shrd(self):
        d = lin(DS0, DATA)
        for model in M386:
            rnd = random.Random(2386)
            b = Batch(self, model)
            for w in (1, 2):
                n = 8 << w
                pre = [0x66] if w == 2 else []
                for left in (1, 0):
                    for _ in range(25):
                        dst, src = rnd.getrandbits(n), rnd.getrandbits(n)
                        for cnt in sorted({0, 1, 2, n // 2, n - 1, n, rnd.randrange(1, n + 1)}):
                            r, f, defined = ref_shxd(left, dst, src, cnt, w)
                            fl = F386 | OF | CF
                            what = "%s %s w%d %x,%x by %d" % (model, "shld" if left else "shrd", w, dst, src, cnt)
                            opc = 0xA4 if left else 0xAC

                            def check(res, r=r, f=f, defined=defined, w=w, what=what, fl=fl, mem=False):
                                got = res.mem(d, 2 << (w - 1), dst) if mem else reg_of(res, w, "ebx")
                                self.assertEqual(got, r, what)
                                if f is None:
                                    self.assertEqual(res["flags"], fl, what + ": count 0")
                                else:
                                    flags_ok(self, res, f, defined, what)
                            # shld/shrd bx/ebx, dx/edx, imm8 (0F A4/AC /r ib, modrm 11 010 011) and by CL (0F A5/AD)
                            b.add(pre + [0x0F, opc, modrm(3, 2, 3), cnt], check, regs={"ebx": dst, "edx": src, "eflags": fl})
                            b.add(pre + [0x0F, opc + 1, modrm(3, 2, 3)], check,
                                  regs={"ebx": dst, "edx": src, "ecx": cnt, "eflags": fl})
                            # the memory form: shld/shrd [DATA], dx/edx, imm8
                            b.add(pre + [0x0F, opc, modrm(0, 2, 6)] + le(DATA, 2) + [cnt],
                                  lambda res, check=check: check(res, mem=True),
                                  regs={"edx": src, "eflags": fl}, mem={d: bytes(le(dst, 2 << (w - 1)))})
            b.run()

    def test_shld16_long_counts(self):
        # 16-bit SHLD/SHRD with a count above 16 (undefined in the manuals): the 386 shifts dst:src:src (SHLD) or
        # src:src:dst (SHRD) as one 48-bit value, as the SingleStepTests 80386 vectors show.
        b = Batch(self, "386")
        for cnt in (17, 20, 25, 31):
            dst, src = 0x1234, 0xABCD
            want_l = ((((dst << 32) | (src << 16) | src) << cnt) >> 32) & 0xFFFF
            want_r = ((((src << 32) | (src << 16) | dst)) >> cnt) & 0xFFFF
            b.add([0x0F, 0xA4, modrm(3, 2, 3), cnt], lambda res, w=want_l: self.assertEqual(res["bx"], w),
                  regs={"ebx": dst, "edx": src})
            b.add([0x0F, 0xAC, modrm(3, 2, 3), cnt], lambda res, w=want_r: self.assertEqual(res["bx"], w),
                  regs={"ebx": dst, "edx": src})
        b.run()


@unittest.skipUnless(have_gcc(), "needs gcc")
class MulDiv386(unittest.TestCase):
    """MUL, IMUL, DIV, IDIV at 32 bits, divide errors as faults, and the two- and three-operand IMUL."""

    def test_mul_div32(self):
        for model in M386:
            rnd = random.Random(3386)
            b = Batch(self, model)
            mem = ivt(0)
            for op in (4, 5, 6, 7):
                cases = [(0, 0), (1, 1), (0xFFFFFFFF, 0xFFFFFFFF), (0x80000000, 0xFFFFFFFF), (0x7FFFFFFF, 2),
                         (1 << 63, 0xFFFFFFFF), ((1 << 63) - 1, 0x7FFFFFFF), (-(1 << 31) * 2 & ((1 << 64) - 1), 2)]
                cases += [(rnd.getrandbits(64), rnd.getrandbits(32)) for _ in range(120)]
                cases += [(rnd.getrandbits(32), rnd.getrandbits(32) or 1) for _ in range(80)]
                for acc, src in cases:
                    eax, edx = acc & 0xFFFFFFFF, acc >> 32
                    code = [0x66, 0xF7, modrm(3, op, 3)]     # mul/imul/div/idiv ebx
                    what = "%s grp3/%d acc=%x src=%x" % (model, op, acc, src)
                    if op in (4, 5):
                        (lo, hi), f, d = ref_mul(op, eax, src, 2)

                        def check(res, lo=lo, hi=hi, f=f, d=d, what=what):
                            self.assertEqual((res["eax"], res["edx"]), (lo, hi), what)
                            flags_ok(self, res, f, d, what)
                    else:
                        q = ref_div(op, acc, src, 2, model)

                        def check(res, q=q, what=what):
                            if q is None:
                                fault_at(self, res, 0, what)
                                return
                            no_fault(self, res, what)
                            self.assertEqual((res["eax"], res["edx"]), q, what)
                    b.add(code, check, regs={"eax": eax, "edx": edx, "ebx": src, "eflags": F386}, mem=mem)
            b.run()

    def test_imul_forms(self):
        for model in M386:
            rnd = random.Random(33)
            b = Batch(self, model)
            for w in (1, 2):
                n = 8 << w
                pre = [0x66] if w == 2 else []
                for _ in range(120):
                    x, y = rnd.getrandbits(n), rnd.getrandbits(n)
                    r, f = ref_imul_trunc(x, y, w)
                    what = "%s imul w%d %x*%x" % (model, w, x, y)

                    def check(res, r=r, f=f, w=w, what=what):
                        self.assertEqual(reg_of(res, w, "ecx"), r, what)
                        flags_ok(self, res, f, CF | OF, what)
                    b.add(pre + [0x0F, 0xAF, modrm(3, 1, 3)], check, regs={"ecx": x, "ebx": y})     # imul cx, bx
                    b.add(pre + [0x69, modrm(3, 1, 3)] + le(y, n // 8), check, regs={"ebx": x})    # imul cx, bx, imm
                    y8 = y & 0xFF
                    r8, f8 = ref_imul_trunc(x, y8 | (((1 << n) - 256) if y8 & 0x80 else 0), w)
                    b.add(pre + [0x6B, modrm(3, 1, 3), y8], lambda res, r=r8, f=f8, w=w, what=what: (
                        self.assertEqual(reg_of(res, w, "ecx"), r, what + " imm8"), flags_ok(self, res, f, CF | OF, what)),
                        regs={"ebx": x})                                                          # imul cx, bx, imm8
            b.run()

    def test_8_16_bit_on_386(self):
        # The 8- and 16-bit forms keep the 286's results: the most negative quotient is allowed, the divide error
        # points at the divide.
        b = Batch(self, "386")
        b.add([0xF6, modrm(3, 7, 3)], lambda res: (no_fault(self, res, "idiv -128"), self.assertEqual(res["al"], 0x80)),
              regs={"ax": (-256) & 0xFFFF, "bx": 2}, mem=ivt(0))
        b.add([0xF7, modrm(3, 6, 3)], lambda res: fault_at(self, res, 0, "div by 0"), regs={"ax": 5, "bx": 0}, mem=ivt(0))
        b.add([0xF3, 0xF6, modrm(3, 7, 3)], lambda res: self.assertEqual(res["al"], 50), regs={"ax": 100, "bx": 2})
        b.run()


@unittest.skipUnless(have_gcc(), "needs gcc")
class Bits386(unittest.TestCase):
    """BT/BTS/BTR/BTC (register, immediate, memory bit strings), BSF/BSR, SETcc, and the 486's BSWAP, XADD, CMPXCHG."""

    def test_bt_register_and_immediate(self):
        for model in M386:
            rnd = random.Random(4386)
            b = Batch(self, model)
            for w in (1, 2):
                n = 8 << w
                pre = [0x66] if w == 2 else []
                for kind, opc in enumerate((0xA3, 0xAB, 0xB3, 0xBB)):
                    for _ in range(30):
                        v, bit = rnd.getrandbits(n), rnd.getrandbits(n)
                        new, cf = ref_bit(kind, v, bit, w)
                        what = "%s bt%d w%d %x bit %d" % (model, kind, w, v, bit)
                        # bt* bx, dx (0F op /r, modrm 11 010 011): the bit number modulo the width
                        b.add(pre + [0x0F, opc, modrm(3, 2, 3)], lambda res, new=new, cf=cf, w=w, what=what: (
                            self.assertEqual(reg_of(res, w, "ebx"), new, what), self.assertEqual(res["flags"] & CF, cf, what)),
                            regs={"ebx": v, "edx": bit})
                        # bt* bx, imm8 (0F BA /4+kind ib)
                        new2, cf2 = ref_bit(kind, v, bit & 0xFF, w)
                        b.add(pre + [0x0F, 0xBA, modrm(3, 4 + kind, 3), bit & 0xFF], lambda res, new=new2, cf=cf2, w=w, what=what: (
                            self.assertEqual(reg_of(res, w, "ebx"), new, what + " imm"), self.assertEqual(res["flags"] & CF, cf, what)),
                            regs={"ebx": v})
            b.run()

    def test_bt_memory_bit_string(self):
        base = DATA + 0x800
        for model in M386:
            b = Batch(self, model)
            for w in (1, 2):
                n = 8 << w
                pre = [0x66] if w == 2 else []
                for bitoff in (0, 5, n - 1, n, 2 * n + 3, 100, -1, -n, -n - 1, -100, 0x7FF, -0x800):
                    off, bit = ref_bitstring(base, bitoff & ((1 << n) - 1), w)
                    old = 0x5A5A5A5A & ((1 << n) - 1)
                    for kind, opc in enumerate((0xA3, 0xAB, 0xB3, 0xBB)):
                        new, cf = ref_bit(kind, old, bit, w)
                        what = "%s bt%d [mem] w%d bit offset %d" % (model, kind, w, bitoff)
                        # bt* [base], dx (0F op /r, modrm 00 010 110 disp16)
                        b.add(pre + [0x0F, opc, modrm(0, 2, 6)] + le(base, 2),
                              lambda res, off=off, new=new, cf=cf, w=w, what=what, old=old: (
                                  self.assertEqual(res.mem(lin(DS0, off), n // 8, old), new, what),
                                  self.assertEqual(res["flags"] & CF, cf, what)),
                              regs={"edx": bitoff & ((1 << n) - 1)}, mem={lin(DS0, off): bytes(le(old, n // 8))})
            b.run()

    def test_bsf_bsr(self):
        for model in M386:
            rnd = random.Random(5386)
            b = Batch(self, model)
            for w in (1, 2):
                n = 8 << w
                pre = [0x66] if w == 2 else []
                for v in [0, 1, 1 << (n - 1), (1 << n) - 1, 0x00100000 & ((1 << n) - 1) or 0x10] + [rnd.getrandbits(n) for _ in range(30)]:
                    for reverse in (0, 1):
                        idx, zf = ref_bsf(v, w, bool(reverse))
                        what = "%s bs%s w%d %x" % (model, "r" if reverse else "f", w, v)

                        def check(res, idx=idx, zf=zf, w=w, what=what):
                            self.assertEqual(res["flags"] & ZF, zf, what)
                            # a zero source leaves the destination as it was
                            want = (0x12345678 & ((1 << (8 << w)) - 1)) if idx is None else idx
                            self.assertEqual(reg_of(res, w, "ecx"), want, what)
                        # bsf/bsr cx, bx (0F BC/BD /r, modrm 11 001 011)
                        b.add(pre + [0x0F, 0xBC + reverse, modrm(3, 1, 3)], check, regs={"ecx": 0x12345678, "ebx": v})
            b.run()

    def test_setcc(self):
        d = lin(DS0, DATA)
        for model in M386:
            b = Batch(self, model)
            for cc in range(16):
                for fl in range(0, 64, 5):
                    flags = F386 | (CF if fl & 1 else 0) | (PF if fl & 2 else 0) | (ZF if fl & 4 else 0) | \
                        (SF if fl & 8 else 0) | (OF if fl & 16 else 0) | (AF if fl & 32 else 0)
                    want = int(ref_cond(cc, flags))
                    # setcc al (0F 90+cc, modrm 11 000 000) and setcc byte [DATA] (modrm 00 000 110)
                    b.add([0x0F, 0x90 + cc, 0xC0], lambda res, want=want, w=(cc, flags): self.assertEqual(res["al"], want, w),
                          regs={"eax": 0x5555, "eflags": flags})
                    b.add([0x0F, 0x90 + cc, 0x06] + le(DATA, 2), lambda res, want=want, w=(cc, flags): self.assertEqual(
                        res.writes[d], want, w), regs={"eflags": flags})
            b.run()

    def test_486_bswap_xadd_cmpxchg(self):
        rnd = random.Random(486)
        d = lin(DS0, DATA)
        b = Batch(self, "486")
        for r in range(8):
            v = rnd.getrandbits(32)
            # bswap r32 (0F C8+r)
            b.add([0x66, 0x0F, 0xC8 + r], lambda res, r=r, v=v: self.assertEqual(res[R32[r]], ref_bswap(v), "bswap %d" % r),
                  regs={R32[r]: v})
        for _ in range(20):
            x, y = rnd.getrandbits(32), rnd.getrandbits(32)
            r, f, defined = ref_alu(0, x, y, 0, 2)
            # xadd ebx, ecx (66 0F C1 /r, modrm 11 001 011): EBX = sum, ECX = old EBX, flags of the addition
            b.add([0x66, 0x0F, 0xC1, modrm(3, 1, 3)], lambda res, x=x, r=r, f=f, defined=defined: (
                self.assertEqual((res["ebx"], res["ecx"]), (r, x)), flags_ok(self, res, f, defined, "xadd")),
                regs={"ebx": x, "ecx": y})
            # xadd [DATA], cl (0F C0 /r)
            r8, f8, d8 = ref_alu(0, x & 0xFF, y & 0xFF, 0, 0)
            b.add([0x0F, 0xC0, modrm(0, 1, 6)] + le(DATA, 2), lambda res, x=x, r8=r8: (
                self.assertEqual(res.writes[d], r8), self.assertEqual(res["cl"], x & 0xFF)),
                regs={"ecx": y}, mem={d: x & 0xFF})
            for equal in (0, 1):
                acc = x if equal else x ^ 0x100
                rr, ff, dd = ref_alu(7, acc, x, 0, 2)
                # cmpxchg [DATA], ecx (66 0F B1 /r): EAX == [DATA]: ZF, [DATA] = ECX; else EAX = [DATA]
                b.add([0x66, 0x0F, 0xB1, modrm(0, 1, 6)] + le(DATA, 2), lambda res, x=x, y=y, acc=acc, equal=equal, ff=ff, dd=dd: (
                    self.assertEqual(res.mem(d, 4, x), y if equal else x),
                    self.assertEqual(res["eax"], acc if equal else x), flags_ok(self, res, ff, dd, "cmpxchg")),
                    regs={"eax": acc, "ecx": y}, mem={d: bytes(le(x, 4))})
        # cmpxchg bl, cl (0F B0 /r): not equal, so AL takes BL
        b.add([0x0F, 0xB0, modrm(3, 1, 3)], lambda res: (self.assertEqual(res["al"], 0x22), self.assertEqual(res["bl"], 0x22)),
              regs={"eax": 0x11, "ebx": 0x22, "ecx": 0x33})
        b.run()
        b = Batch(self, "386")                          # none of them exists on the 386
        for code in ([0x0F, 0xC8], [0x0F, 0xC1, 0xCB], [0x0F, 0xB1, 0xCB], [0x0F, 0x08], [0x0F, 0x09], [0x0F, 0x01, 0x3F]):
            b.add(code, lambda res, code=code: fault_at(self, res, 6, "386 " + bytes(code).hex()), mem=ivt(6))
        b.run()


@unittest.skipUnless(have_gcc(), "needs gcc")
class Moves386(unittest.TestCase):
    """MOVZX/MOVSX, 32-bit MOV/XCHG/LEA/PUSH/POP, 32-bit direct offsets."""

    def test_movzx_movsx(self):
        d = lin(DS0, DATA)
        for model in M386:
            b = Batch(self, model)
            for v8 in (0, 0x7F, 0x80, 0xFF):
                sx = v8 | (0xFFFFFF00 if v8 & 0x80 else 0)
                b.add([0x66, 0x0F, 0xB6, modrm(3, 1, 3)], lambda res, v=v8: self.assertEqual(res["ecx"], v), regs={"ebx": v8, "ecx": 0xDEADBEEF})
                b.add([0x66, 0x0F, 0xBE, modrm(3, 1, 3)], lambda res, v=sx: self.assertEqual(res["ecx"], v), regs={"ebx": v8})
                b.add([0x0F, 0xBE, modrm(3, 1, 3)], lambda res, v=sx: self.assertEqual(res["ecx"], 0xDEAD0000 | (v & 0xFFFF)),
                      regs={"ebx": v8, "ecx": 0xDEADBEEF})                          # movsx cx, bl: the upper half stays
                b.add([0x66, 0x0F, 0xB6, modrm(0, 0, 6)] + le(DATA, 2), lambda res, v=v8: self.assertEqual(res["eax"], v),
                      mem={d: v8})                                                   # movzx eax, byte [DATA]
            for v16 in (0, 0x7FFF, 0x8000, 0xFFFF):
                sx = v16 | (0xFFFF0000 if v16 & 0x8000 else 0)
                b.add([0x66, 0x0F, 0xB7, modrm(3, 1, 3)], lambda res, v=v16: self.assertEqual(res["ecx"], v), regs={"ebx": v16, "ecx": 0xDEADBEEF})
                b.add([0x66, 0x0F, 0xBF, modrm(3, 1, 3)], lambda res, v=sx: self.assertEqual(res["ecx"], v), regs={"ebx": v16})
            b.run()

    def test_moves32(self):
        d, ss = lin(DS0, DATA), SS0
        for model in M386:
            b = Batch(self, model, defaults={"sp": 0x1000})
            b.add([0x66, 0xB9, 0x78, 0x56, 0x34, 0x12], lambda res: self.assertEqual(res["ecx"], 0x12345678))     # mov ecx, imm32
            b.add([0x66, 0x89, 0x1E] + le(DATA, 2), lambda res: self.assertEqual(res.mem(d, 4), 0xCAFEF00D),
                  regs={"ebx": 0xCAFEF00D})                                                                  # mov [DATA], ebx
            b.add([0x66, 0x8B, 0x0E] + le(DATA, 2), lambda res: self.assertEqual(res["ecx"], 0x01020304),
                  mem={d: b"\x04\x03\x02\x01"})                                                               # mov ecx, [DATA]
            b.add([0x66, 0xC7, 0x06] + le(DATA, 2) + le(0x89ABCDEF, 4), lambda res: self.assertEqual(res.mem(d, 4), 0x89ABCDEF))
            b.add([0x67, 0x66, 0xA1] + le(DATA, 4), lambda res: self.assertEqual(res["eax"], 0x0A0B0C0D),
                  mem={d: b"\x0D\x0C\x0B\x0A"})                                                               # mov eax, [dword DATA]
            b.add([0x67, 0x66, 0xA3] + le(DATA, 4), lambda res: self.assertEqual(res.mem(d, 4), 0x13579BDF),
                  regs={"eax": 0x13579BDF})                                                                  # mov [dword DATA], eax
            b.add([0x66, 0x87, 0xD9], lambda res: self.assertEqual((res["ebx"], res["ecx"]), (2, 1)),
                  regs={"ebx": 1, "ecx": 2})                                                                 # xchg ebx, ecx
            b.add([0x66, 0x92], lambda res: self.assertEqual((res["eax"], res["edx"]), (0xBBBB0002, 0xAAAA0001)),
                  regs={"eax": 0xAAAA0001, "edx": 0xBBBB0002})                                               # xchg eax, edx
            # lea ecx, [eax+ebx*2+10h] (66 67 8D 4C 58 10): no memory access, all 32 bits
            b.add([0x66, 0x67, 0x8D, 0x4C, 0x58, 0x10], lambda res: self.assertEqual(res["ecx"], 0x40000000 + 0x20000000 * 2 + 0x10),
                  regs={"eax": 0x40000000, "ebx": 0x20000000})
            # lea cx, [eax+ebx*2+10h] (67 8D 4C 58 10): truncated to 16 bits
            b.add([0x67, 0x8D, 0x4C, 0x58, 0x10], lambda res: self.assertEqual(res["ecx"], 0xFFFF0000 | 0x2235),
                  regs={"eax": 0x12341225, "ebx": 0x00000800, "ecx": 0xFFFFFFFF})
            # lea ecx, [bx+si] (66 8D 08): a 16-bit address, zero-extended
            b.add([0x66, 0x8D, 0x08], lambda res: self.assertEqual(res["ecx"], 0x0000FFFF & (0xF000 + 0x1234)),
                  regs={"ebx": 0xF000, "esi": 0x1234, "ecx": 0xFFFFFFFF})
            # push ebx (66 53) / pop ecx (66 59): four bytes on the stack
            b.add([0x66, 0x53], lambda res: (self.assertEqual(res["sp"], 0x0FFC), self.assertEqual(res.mem(lin(ss, 0xFFC), 4), 0x11223344)),
                  regs={"ebx": 0x11223344})
            b.add([0x66, 0x59], lambda res: (self.assertEqual(res["sp"], 0x1004), self.assertEqual(res["ecx"], 0x55667788)),
                  mem={lin(ss, 0x1000): b"\x88\x77\x66\x55"})
            # push imm32 (66 68), push imm8 sign-extended to 32 bits (66 6A)
            b.add([0x66, 0x68] + le(0x0BADF00D, 4), lambda res: self.assertEqual(res.mem(lin(ss, 0xFFC), 4), 0x0BADF00D))
            b.add([0x66, 0x6A, 0xFE], lambda res: self.assertEqual(res.mem(lin(ss, 0xFFC), 4), 0xFFFFFFFE))
            # push esp pushes the value before the push
            b.add([0x66, 0x54], lambda res: self.assertEqual(res.mem(lin(ss, 0xFFC), 4), 0x12341000), regs={"esp": 0x12341000})
            b.run()


@unittest.skipUnless(have_gcc(), "needs gcc")
class Addressing386(unittest.TestCase):
    """32-bit ModR/M and SIB addressing, segment defaults and overrides, and the real-mode limit faults."""

    VALS = [0x0100, 0x0010, 0x0020, 0x1000, 0x0800, 0x2000, 0x0300, 0x0040]     # EAX..EDI for the address tests

    def ea_of(self, model, mod, rm, index=4, scale=0, base=None, disp=0):
        """(offset, segment) a 32-bit ModR/M (+ SIB) form addresses, from the manual's tables."""
        v = self.VALS
        if rm != 4:
            if mod == 0 and rm == 5:
                return disp & 0xFFFFFFFF, DS0
            return (v[rm] + disp) & 0xFFFFFFFF, SS0 if rm == 5 else DS0
        if base == 5 and mod == 0:
            ea, seg = disp, DS0
        else:
            bv = v[base]
            if index == 4 and scale and model == "386":
                bv <<= scale                            # the 386 scales the base when there is no index (see x86cpu32.c)
            ea, seg = bv + disp, SS0 if base in (4, 5) else DS0
        if index != 4:
            ea += v[index] << scale
        return ea & 0xFFFFFFFF, seg

    def test_modrm32(self):
        for model in M386:
            b = Batch(self, model)
            regs = {R32[i]: self.VALS[i] for i in range(8)}
            n = 0
            for mod in (0, 1, 2):
                dispv = {0: 0, 1: -0x10, 2: 0x1234}[mod]
                dbytes = {0: [], 1: [dispv & 0xFF], 2: le(dispv, 4)}[mod]
                for rm in range(8):
                    if rm == 4:
                        for base in range(8):
                            for index in range(8):
                                for scale in range(4):
                                    disp, db = (0x2345, le(0x2345, 4)) if (mod == 0 and base == 5) else (dispv, dbytes)
                                    ea, seg = self.ea_of(model, mod, 4, index, scale, base, disp)
                                    if ea > 0xFFFC:
                                        continue
                                    n += 1
                                    val = (0x01000193 * n) & 0xFFFFFFFF
                                    code = [0x66, 0x67, 0x8B, modrm(mod, 0, 4), sib(scale, index, base)] + db
                                    b.add(code, lambda res, val=val, w=(model, mod, base, index, scale): self.assertEqual(res["eax"], val, w),
                                          regs=regs, mem={lin(seg, ea): bytes(le(val, 4))})
                    else:
                        disp, db = (0x2345, le(0x2345, 4)) if (mod == 0 and rm == 5) else (dispv, dbytes)
                        ea, seg = self.ea_of(model, mod, rm, disp=disp)
                        n += 1
                        val = (0x01000193 * n) & 0xFFFFFFFF
                        b.add([0x66, 0x67, 0x8B, modrm(mod, 0, rm)] + db, lambda res, val=val, w=(model, mod, rm): self.assertEqual(res["eax"], val, w),
                              regs=regs, mem={lin(seg, ea): bytes(le(val, 4))})
            b.run()

    def test_overrides_fs_gs(self):
        for model in M386:
            b = Batch(self, model)
            # mov al, fs:[bx] (64 8A 07) and gs:[bx] (65 8A 07); mov eax, fs:[ebp] (64 66 67 8B 45 00): FS wins over SS
            b.add([0x64, 0x8A, 0x07], lambda res: self.assertEqual(res["al"], 0x61), regs={"fs": 0x6000, "ebx": 0x10},
                  mem={lin(0x6000, 0x10): 0x61})
            b.add([0x65, 0x8A, 0x07], lambda res: self.assertEqual(res["al"], 0x71), regs={"gs": 0x7000, "ebx": 0x10},
                  mem={lin(0x7000, 0x10): 0x71})
            b.add([0x64, 0x66, 0x67, 0x8B, 0x45, 0x00], lambda res: self.assertEqual(res["eax"], 0x66667777),
                  regs={"fs": 0x6000, "ebp": 0x20}, mem={lin(0x6000, 0x20): b"\x77\x77\x66\x66"})
            b.run()

    def test_limit_faults(self):
        h = {**ivt(12), **ivt(13)}
        for model in M386:
            b = Batch(self, model)
            # mov ax, [FFFFh] (A1 FF FF): a word at the limit is a #GP, it does not wrap as on the 8086
            b.add([0xA1, 0xFF, 0xFF], lambda res: fault_at(self, res, 13, "word at FFFF"), mem=h)
            # mov al, [FFFFh] (A0 FF FF) is fine
            b.add([0xA0, 0xFF, 0xFF], lambda res: (no_fault(self, res, "byte at FFFF"), self.assertEqual(res["al"], 0x42)),
                  mem={**h, lin(DS0, 0xFFFF): 0x42})
            # mov eax, [FFFDh] (66 A1 FD FF): a doubleword across the limit
            b.add([0x66, 0xA1, 0xFD, 0xFF], lambda res: fault_at(self, res, 13, "dword at FFFD"), mem=h)
            # mov ax, [bp+0] with BP = FFFFh (8B 46 00): in the stack segment it is a #SS
            b.add([0x8B, 0x46, 0x00], lambda res: fault_at(self, res, 12, "word at SS:FFFF"), regs={"bp": 0xFFFF}, mem=h)
            # mov al, [ebx] with EBX = 10000h (67 8A 03): an offset beyond the limit
            b.add([0x67, 0x8A, 0x03], lambda res: fault_at(self, res, 13, "offset 10000h"), regs={"ebx": 0x10000}, mem=h)
            # mov al, [ebp+0] with EBP = 10000h (67 8A 45 00): #SS
            b.add([0x67, 0x8A, 0x45, 0x00], lambda res: fault_at(self, res, 12, "SS offset 10000h"), regs={"ebp": 0x10000}, mem=h)
            # movsd with SI = FFFEh (66 A5): #GP, with SI and DI as they were
            b.add([0x66, 0xA5], lambda res: (fault_at(self, res, 13, "movsd"), self.assertEqual((res["si"], res["di"]), (0xFFFE, 0x10))),
                  regs={"si": 0xFFFE, "di": 0x10}, mem=h)
            # add [FFFFh], ax (01 06 FF FF): nothing is written, the flags are as before
            b.add([0x01, 0x06, 0xFF, 0xFF], lambda res: (fault_at(self, res, 13, "add [FFFF]"),
                                                          self.assertEqual(res.mem(lin(SS0, res["sp"] + 4), 2), 0x08D7)),
                  regs={"ax": 1, "eflags": 0x08D7}, mem=h)
            # push ax with SP = 1: no room for the exception frame either, the CPU shuts down
            b.add([0x50], lambda res: self.assertEqual(res.stop, 4), regs={"sp": 1}, mem=h)
            b.run()

    def test_instruction_ending_at_ffff(self):
        # A NOP at CS:FFFFh completes; EIP is then 10000h and the next fetch takes #GP, pushing IP 0. The 8086 wraps.
        for model in M386:
            mem = {**ivt(13), lin(DEFAULT["cs"], 0xFFFF): 0x90}
            r = run_script(["model " + model] + case_lines(None, regs={"ip": 0xFFFF}, mem=mem, steps=2))[0]
            fault_at(self, r, 13, model + " fetch past FFFF", ip=0)
        mem = {lin(DEFAULT["cs"], 0xFFFF): 0x90, lin(DEFAULT["cs"], 0): 0xF4}
        r = run_script(["model 8086"] + case_lines(None, regs={"ip": 0xFFFF}, mem=mem, steps=2))[0]
        self.assertEqual((r["cs"], r["ip"], r.halted), (DEFAULT["cs"], 1, 1))


@unittest.skipUnless(have_gcc(), "needs gcc")
class Strings386(unittest.TestCase):
    """Doubleword string instructions, and the address size choosing SI/DI/CX or ESI/EDI/ECX."""

    def test_string32(self):
        src = bytes(range(0x40, 0x80))
        for model in M386:
            b = Batch(self, model)
            # rep movsd, CX = 4 (F3 66 A5): 16 bytes
            b.add([0xF3, 0x66, 0xA5], lambda res: (
                self.assertEqual(bytes(res.writes[lin(ES0, 0x100 + i)] for i in range(16)), src[:16]),
                self.assertEqual((res["cx"], res["si"], res["di"]), (0, 0x110, 0x110))),
                regs={"cx": 4, "si": 0x100, "di": 0x100}, mem={lin(DS0, 0x100): src})
            # backwards (DF = 1): rep movsd from SI = 104h, CX = 2: dwords at 104h, then 100h
            b.add([0xF3, 0x66, 0xA5], lambda res: (
                self.assertEqual(bytes(res.writes[lin(ES0, 0x200 + i)] for i in range(8)), src[:8]),
                self.assertEqual((res["si"], res["di"]), (0xFC, 0x1FC))),
                regs={"cx": 2, "si": 0x104, "di": 0x204, "eflags": F386 | DF}, mem={lin(DS0, 0x100): src})
            # a 16-bit address size counts in CX alone: ECX = 10002h copies two bytes and leaves 10000h
            b.add([0xF3, 0xA4], lambda res: self.assertEqual(res["ecx"], 0x10000),
                  regs={"ecx": 0x10002, "si": 0x100, "di": 0x100}, mem={lin(DS0, 0x100): src})
            # a 32-bit address size (67 F3 A4) uses ECX, ESI, EDI
            b.add([0x67, 0xF3, 0xA4], lambda res: self.assertEqual((res["ecx"], res["esi"], res["edi"]), (0, 0x103, 0x303)),
                  regs={"ecx": 3, "esi": 0x100, "edi": 0x300}, mem={lin(DS0, 0x100): src})
            # ... and an ESI beyond the limit faults: 67 A4 with ESI = 10000h
            b.add([0x67, 0xA4], lambda res: fault_at(self, res, 13, "movsb [esi=10000h]"), regs={"esi": 0x10000}, mem=ivt(13))
            # stosd, lodsd, scasd, cmpsd
            b.add([0x66, 0xAB], lambda res: (self.assertEqual(res.mem(lin(ES0, 0x40), 4), 0x87654321), self.assertEqual(res["di"], 0x44)),
                  regs={"eax": 0x87654321, "di": 0x40})
            b.add([0x66, 0xAD], lambda res: (self.assertEqual(res["eax"], 0x47464544), self.assertEqual(res["si"], 0x108)),
                  regs={"si": 0x104}, mem={lin(DS0, 0x100): src})
            b.add([0x66, 0xAF], lambda res: (self.assertTrue(res["flags"] & ZF), self.assertEqual(res["di"], 0x34)),
                  regs={"eax": 0xDDCCBBAA, "di": 0x30}, mem={lin(ES0, 0x30): b"\xAA\xBB\xCC\xDD"})
            b.add([0xF3, 0x66, 0xA7], lambda res: (self.assertEqual(res["cx"], 1), self.assertFalse(res["flags"] & ZF)),
                  regs={"cx": 3, "si": 0, "di": 0}, mem={lin(DS0, 0): b"abcdefgh", lin(ES0, 0): b"abcdefgX"})
            # insd from port 60h (66 6D) and outsd (66 6F): two word accesses each
            b.add([0x66, 0x6D], lambda res: self.assertEqual(res.mem(lin(ES0, 0x20), 4), 0x56781234),
                  regs={"di": 0x20, "dx": 0x60}, ports={0x60: 0x1234, 0x62: 0x5678})
            b.add([0x66, 0x6F], lambda res: self.assertEqual(res.outs, [(0x60, 0x4140, 2), (0x62, 0x4342, 2)]),
                  regs={"si": 0x100, "dx": 0x60}, mem={lin(DS0, 0x100): src})
            # in eax, dx / out dx, eax (66 ED / 66 EF)
            b.add([0x66, 0xED], lambda res: self.assertEqual(res["eax"], 0x00AB00CD), regs={"dx": 0x70}, ports={0x70: 0xCD, 0x72: 0xAB})
            b.add([0x66, 0xEF], lambda res: self.assertEqual(res.outs, [(0x70, 0x5678, 2), (0x72, 0x1234, 2)]),
                  regs={"eax": 0x12345678, "dx": 0x70})
            b.run()


@unittest.skipUnless(have_gcc(), "needs gcc")
class Control386(unittest.TestCase):
    """Near Jcc, 32-bit jumps, calls and returns (EIP beyond FFFFh faults), LOOP/JECXZ, IRETD, ENTER/LEAVE, PUSHAD."""

    SP = 0x1000

    def test_jcc_near(self):
        for model in M386:
            b = Batch(self, model)
            for cc in range(16):
                for fl in range(0, 64, 3):
                    flags = F386 | (CF if fl & 1 else 0) | (PF if fl & 2 else 0) | (ZF if fl & 4 else 0) | \
                        (SF if fl & 8 else 0) | (OF if fl & 16 else 0) | (AF if fl & 32 else 0)
                    taken = ref_cond(cc, flags)
                    # jcc near +1000h (0F 80+cc cw)
                    b.add([0x0F, 0x80 + cc, 0x00, 0x10], lambda res, t=taken, w=(cc, flags): self.assertEqual(
                        res["ip"], 0x1104 if t else 0x104, w), regs={"eflags": flags})
                    # jcc near dword -80h (66 0F 80+cc cd)
                    b.add([0x66, 0x0F, 0x80 + cc] + le(-0x80, 4), lambda res, t=taken, w=(cc, flags): self.assertEqual(
                        res["ip"], 0x87 if t else 0x107, w), regs={"eflags": flags})
            b.run()

    def test_eip_beyond_limit(self):
        h = ivt(13)
        ss, sp = SS0, self.SP
        for model in M386:
            b = Batch(self, model, defaults={"sp": sp})
            b.add([0x66, 0xE9] + le(0x10000, 4), lambda res: fault_at(self, res, 13, "jmp dword"), mem=h)
            b.add([0x66, 0xE8] + le(0x10000, 4), lambda res: (fault_at(self, res, 13, "call dword"),
                                                               self.assertEqual(res["sp"], sp - 6)), mem=h)
            b.add([0x66, 0xC3], lambda res: fault_at(self, res, 13, "ret dword"), mem={**h, lin(ss, sp): b"\x00\x00\x01\x00"})
            b.add([0x66, 0xEA] + le(0x10000, 4) + le(0x1234, 2), lambda res: fault_at(self, res, 13, "jmp far dword"), mem=h)
            b.add([0x66, 0x0F, 0x84] + le(0xFF00, 4), lambda res: fault_at(self, res, 13, "jz dword"), regs={"eflags": F386 | ZF}, mem=h)
            # with a 16-bit operand size the target wraps instead: jmp near +FF00h from 103h lands at 0003h
            b.add([0xE9, 0x00, 0xFF], lambda res: (no_fault(self, res, "jmp wrap"), self.assertEqual(res["ip"], 0x0003)), mem=h)
            # a short jump with a 32-bit operand size inside the segment is fine
            b.add([0x66, 0xEB, 0x10], lambda res: self.assertEqual(res["ip"], 0x113), mem=h)
            b.run()

    def test_call_ret_32(self):
        ss, sp = SS0, self.SP
        for model in M386:
            b = Batch(self, model, defaults={"sp": sp})
            # call near dword +100h (66 E8 cd): pushes the 32-bit return EIP
            b.add([0x66, 0xE8] + le(0x100, 4), lambda res: (self.assertEqual((res["ip"], res["sp"]), (0x206, sp - 4)),
                                                             self.assertEqual(res.mem(lin(ss, sp - 4), 4), 0x106)))
            # ret / ret 8 (66 C3 / 66 C2 08 00): pop 4 bytes
            b.add([0x66, 0xC3], lambda res: self.assertEqual((res["ip"], res["sp"]), (0x0789, sp + 4)), mem={lin(ss, sp): b"\x89\x07\x00\x00"})
            b.add([0x66, 0xC2, 0x08, 0x00], lambda res: self.assertEqual((res["ip"], res["sp"]), (0x0789, sp + 12)),
                  mem={lin(ss, sp): b"\x89\x07\x00\x00"})
            # call far ptr16:32 (66 9A): CS and EIP pushed as doublewords
            b.add([0x66, 0x9A] + le(0x5678, 4) + le(0x1234, 2), lambda res: (
                self.assertEqual((res["cs"], res["ip"], res["sp"]), (0x1234, 0x5678, sp - 8)),
                self.assertEqual(res.mem(lin(ss, sp - 8), 8), 0x0000_1000_0000_0108)))
            # retf (66 CB): EIP then CS from doubleword slots
            b.add([0x66, 0xCB], lambda res: self.assertEqual((res["cs"], res["ip"], res["sp"]), (0x0BCD, 0x0789, sp + 8)),
                  mem={lin(ss, sp): b"\x89\x07\x00\x00\xCD\x0B\x00\x00"})
            # iretd (66 CF): EIP, CS, EFLAGS
            b.add([0x66, 0xCF], lambda res: self.assertEqual((res["ip"], res["cs"], res["flags"], res["sp"]), (0x1234, 0x5678, 0x02D7, sp + 12)),
                  mem={lin(ss, sp): b"\x34\x12\x00\x00\x78\x56\x00\x00\xD7\x02\x00\x00"})
            # call ebx (66 FF D3), jmp ebx (66 FF E3); call far [DATA] with a 16:32 pointer (66 FF 1E)
            b.add([0x66, 0xFF, 0xD3], lambda res: (self.assertEqual(res["ip"], 0x4321), self.assertEqual(res.mem(lin(ss, sp - 4), 4), 0x103)),
                  regs={"ebx": 0x4321})
            b.add([0x66, 0xFF, 0x1E] + le(DATA, 2), lambda res: self.assertEqual((res["cs"], res["ip"]), (0x2222, 0x1111)),
                  mem={lin(DS0, DATA): b"\x11\x11\x00\x00\x22\x22"})
            b.run()

    def test_loop_jecxz(self):
        for model in M386:
            b = Batch(self, model)
            # loop with a 32-bit address size counts ECX (67 E2): 10000h -> FFFFh, taken
            b.add([0x67, 0xE2, 0x10], lambda res: self.assertEqual((res["ecx"], res["ip"]), (0xFFFF, 0x113)), regs={"ecx": 0x10000})
            # loop with a 16-bit address size counts CX: ECX = 10001h -> 10000h, CX = 0, not taken
            b.add([0xE2, 0x10], lambda res: self.assertEqual((res["ecx"], res["ip"]), (0x10000, 0x102)), regs={"ecx": 0x10001})
            # jecxz (67 E3) sees ECX = 10000h as nonzero; jcxz (E3) sees CX = 0
            b.add([0x67, 0xE3, 0x10], lambda res: self.assertEqual(res["ip"], 0x103), regs={"ecx": 0x10000})
            b.add([0xE3, 0x10], lambda res: self.assertEqual(res["ip"], 0x112), regs={"ecx": 0x10000})
            b.run()

    def test_pushad_popad_enter_leave(self):
        ss, sp = SS0, self.SP
        vals = {R32[i]: 0x11111111 * (i + 1) for i in range(8)}
        vals["esp"] = sp
        for model in M386:
            b = Batch(self, model)
            # pushad (66 60): EAX ECX EDX EBX ESP(before) EBP ESI EDI, 32 bytes
            b.add([0x66, 0x60], lambda res: (self.assertEqual(res["sp"], sp - 32),
                                             self.assertEqual([res.mem(lin(ss, sp - 4 - 4 * i), 4) for i in range(8)],
                                                              [vals[k] for k in R32])), regs=vals)
            # popad (66 61): the saved ESP is skipped, except that its upper half is loaded (a 386 effect on a 16-bit stack)
            img = b"".join(bytes(le(0x01010101 * (8 - i), 4)) for i in range(8))     # EDI first
            b.add([0x66, 0x61], lambda res: (
                self.assertEqual([res[k] for k in ("edi", "esi", "ebp", "ebx", "edx", "ecx", "eax")],
                                 [0x08080808, 0x07070707, 0x06060606, 0x04040404, 0x03030303, 0x02020202, 0x01010101]),
                self.assertEqual(res["esp"], 0x05050000 | (sp + 32))), regs={"esp": sp}, mem={lin(ss, sp): img})
            # pushfd / popfd (66 9C / 66 9D)
            b.add([0x66, 0x9C], lambda res: self.assertEqual(res.mem(lin(ss, sp - 4), 4), 0x0ED7), regs={"esp": sp, "eflags": 0x0ED7})
            # enter 8, 0 with a 32-bit operand size (66 C8 08 00 00): push EBP, EBP = frame (zero-extended), SP -= 8
            b.add([0x66, 0xC8, 0x08, 0x00, 0x00], lambda res: (
                self.assertEqual((res["ebp"], res["sp"]), (sp - 4, sp - 12)), self.assertEqual(res.mem(lin(ss, sp - 4), 4), 0xABCD1234)),
                regs={"esp": sp, "ebp": 0xABCD1234})
            # enter 4, 2 (66 C8 04 00 02): one outer frame pointer copied
            b.add([0x66, 0xC8, 0x04, 0x00, 0x02], lambda res: (
                self.assertEqual((res["ebp"], res["sp"]), (sp - 4, sp - 16)),
                self.assertEqual([res.mem(lin(ss, sp - 4 * i), 4) for i in (1, 2, 3)], [0x00000800, 0x99999999, sp - 4])),
                regs={"esp": sp, "ebp": 0x800}, mem={lin(ss, 0x800 - 4): b"\x99\x99\x99\x99"})
            # leave (66 C9): SP = BP, pop EBP
            b.add([0x66, 0xC9], lambda res: self.assertEqual((res["ebp"], res["sp"]), (0xFEEDFACE, 0x0804)),
                  regs={"esp": sp, "ebp": 0x800}, mem={lin(ss, 0x800): b"\xCE\xFA\xED\xFE"})
            b.run()


@unittest.skipUnless(have_gcc(), "needs gcc")
class Segments386(unittest.TestCase):
    """FS and GS: MOV, PUSH/POP (32-bit forms move SP by 4 but touch one word), LFS/LGS/LSS; the invalid forms."""

    def test_fs_gs(self):
        ss, sp, d = SS0, 0x1000, lin(DS0, DATA)
        for model in M386:
            b = Batch(self, model, defaults={"sp": sp})
            b.add([0x8E, 0xE0], lambda res: self.assertEqual(res["fs"], 0x1357), regs={"eax": 0x1357})        # mov fs, ax
            b.add([0x8E, 0xE8], lambda res: self.assertEqual(res["gs"], 0x2468), regs={"eax": 0x2468})        # mov gs, ax
            b.add([0x8C, 0xE0], lambda res: self.assertEqual(res["eax"], 0xFFFF6000), regs={"eax": 0xFFFFFFFF, "fs": 0x6000})  # mov ax, fs
            b.add([0x66, 0x8C, 0xE8], lambda res: self.assertEqual(res["eax"], 0x7000), regs={"eax": 0xFFFFFFFF, "gs": 0x7000})  # mov eax, gs
            b.add([0x0F, 0xA0], lambda res: (self.assertEqual(res["sp"], sp - 2), self.assertEqual(res.mem(lin(ss, sp - 2), 2), 0x6000)),
                  regs={"fs": 0x6000})                                                                         # push fs
            b.add([0x66, 0x0F, 0xA8], lambda res: (self.assertEqual(res["sp"], sp - 4), self.assertEqual(res.mem(lin(ss, sp - 4), 2), 0x7000),
                                                   self.assertNotIn(lin(ss, sp - 2), res.writes)), regs={"gs": 0x7000})  # push gs (32-bit)
            b.add([0x0F, 0xA9], lambda res: (self.assertEqual(res["gs"], 0xBEEF), self.assertEqual(res["sp"], sp + 2)),
                  mem={lin(ss, sp): b"\xEF\xBE"})                                                              # pop gs
            b.add([0x66, 0x0F, 0xA1], lambda res: (self.assertEqual(res["fs"], 0xBEEF), self.assertEqual(res["sp"], sp + 4)),
                  mem={lin(ss, sp): b"\xEF\xBE\x55\x55"})                                                      # pop fs (32-bit)
            b.add([0x0F, 0xB4, 0x1E] + le(DATA, 2), lambda res: self.assertEqual((res["bx"], res["fs"]), (0x1234, 0xABCD)),
                  mem={d: b"\x34\x12\xCD\xAB"})                                                                # lfs bx, [DATA]
            b.add([0x66, 0x0F, 0xB5, 0x1E] + le(DATA, 2), lambda res: self.assertEqual((res["ebx"], res["gs"]), (0x87654321, 0x0F0F)),
                  mem={d: b"\x21\x43\x65\x87\x0F\x0F"})                                                        # lgs ebx, [DATA]
            b.add([0x0F, 0xB2, 0x26] + le(DATA, 2), lambda res: self.assertEqual((res["sp"], res["ss"]), (0x8000, 0x3456)),
                  mem={d: b"\x00\x80\x56\x34"})                                                                # lss sp, [DATA]
            for code in ([0x8E, 0xC8], [0x8E, 0xF0], [0x8C, 0xF8], [0x0F, 0xB4, 0xC0]):   # mov cs, ax; reg 6/7; lfs ax, ax
                b.add(code, lambda res, code=code: fault_at(self, res, 6, bytes(code).hex()), mem=ivt(6))
            b.run()


@unittest.skipUnless(have_gcc(), "needs gcc")
class System386(unittest.TestCase):
    """Control, debug and test registers, descriptor-table registers and the IDTR's effect, MSW, the FPU's absence."""

    def test_control_registers(self):
        for model, cr0 in (("386", 0x7FFFFFE0), ("486", 0x60000010)):
            b = Batch(self, model)
            b.add([0x0F, 0x20, 0xC0], lambda res, cr0=cr0: self.assertEqual(res["eax"], cr0, model))           # mov eax, cr0
            b.add([0x0F, 0x22, 0xC0], lambda res: self.assertEqual((res.stop, res["ip"]), (1, 0x103)), regs={"eax": 1})  # PE: stop
            b.add([0x0F, 0x22, 0xC0], lambda res: fault_at(self, res, 13, "PG without PE"), regs={"eax": 0x80000000}, mem=ivt(13))
            b.add([0x0F, 0x22, 0xD0, 0x0F, 0x20, 0xD3], lambda res: self.assertEqual(res["ebx"], 0x12345678),
                  regs={"eax": 0x12345678}, steps=2)                                                          # cr2 round trip
            b.add([0x0F, 0x22, 0xD8, 0x0F, 0x20, 0xDB], lambda res: self.assertEqual(res["ebx"], 0x12345000 | (0x18 if model == "486" else 0)),
                  regs={"eax": 0x12345FFF}, steps=2)                                                          # cr3: PCD/PWT on the 486
            for code in ([0x0F, 0x20, 0xC8], [0x0F, 0x20, 0xE0], [0x0F, 0x22, 0xE8]):                       # cr1, cr4, cr5
                b.add(code, lambda res, code=code: fault_at(self, res, 6, model + " " + bytes(code).hex()), mem=ivt(6))
            b.run()
        b = Batch(self, "486")
        # the 486's CR0 bits: CD, NW, AM, WP, NE, MP, EM, TS read back (ET always set); NW without CD is a #GP
        b.add([0x0F, 0x22, 0xC0, 0x0F, 0x20, 0xC3], lambda res: self.assertEqual(res["ebx"], 0x6005003E), regs={"eax": 0x6005002E}, steps=2)
        b.add([0x0F, 0x22, 0xC0], lambda res: fault_at(self, res, 13, "NW without CD"), regs={"eax": 0x20000000}, mem=ivt(13))
        b.run()
        b = Batch(self, "386")
        # the 386 keeps MP, EM, TS, ET (its bits 5-30 read as ones)
        b.add([0x0F, 0x22, 0xC0, 0x0F, 0x20, 0xC3], lambda res: self.assertEqual(res["ebx"], 0x7FFFFFFE), regs={"eax": 0x0005003E}, steps=2)
        b.run()

    def test_debug_and_test_registers(self):
        for model in M386:
            b = Batch(self, model)
            b.add([0x0F, 0x23, 0xC0, 0x0F, 0x21, 0xC3], lambda res: self.assertEqual(res["ebx"], 0xCAFE0001), regs={"eax": 0xCAFE0001}, steps=2)  # dr0
            b.add([0x0F, 0x21, 0xE3], lambda res: self.assertEqual(res["ebx"], 0xFFFF0FF0))                  # mov ebx, dr4 (= dr6)
            b.add([0x0F, 0x26, 0xF0, 0x0F, 0x24, 0xF3], lambda res: self.assertEqual(res["ebx"], 0x0BADBEEF), regs={"eax": 0x0BADBEEF}, steps=2)  # tr6
            if model == "386":
                b.add([0x0F, 0x24, 0xDB], lambda res: fault_at(self, res, 6, "tr3 on a 386"), mem=ivt(6))
            else:
                b.add([0x0F, 0x26, 0xD8, 0x0F, 0x24, 0xDB], lambda res: self.assertEqual(res["ebx"], 7), regs={"eax": 7}, steps=2)  # tr3
            b.run()

    def test_descriptor_table_registers(self):
        d = lin(DS0, DATA)
        for model in M386:
            # sgdt [DATA] after reset: limit FFFFh, base 0, the top byte 0 (a 286 stores FFh there)
            r = run_script(["model " + model] + case_lines([0x0F, 0x01, 0x06] + le(DATA, 2)))[0]
            self.assertEqual(r.mem(d, 6), 0x00000000FFFF)
            # lgdt [DATA] with a 16-bit operand size keeps 24 bits of the base; with 66h all 32
            for pre, want in (([], 0x00563412), ([0x66], 0x78563412)):
                lines = ["model " + model] + case_lines(pre + [0x0F, 0x01, 0x16] + le(DATA, 2) + [0x66, 0x0F, 0x01, 0x06] + le(DATA + 8, 2),
                                                         mem={d: b"\x27\x00\x12\x34\x56\x78"}, steps=2)
                r = run_script(lines)[0]
                self.assertEqual(r.mem(d + 8, 6), want << 16 | 0x27, "lgdt with prefix %s" % pre)
            # lidt moves the real-mode vector table: int 21h reads its vector at 6000h + 84h
            idt = {lin(DS0, DATA): b"\xFF\x03\x00\x60\x00\x00", 0x6000 + 0x21 * 4: bytes([0x10, 0x02, 0x00, 0x50]),
                   lin(HANDLER_SEG, 0x210): b"\xF4"}
            r = run_script(["model " + model] + case_lines([0x0F, 0x01, 0x1E] + le(DATA, 2) + [0xCD, 0x21], mem=idt, steps=2))[0]
            self.assertEqual((r["cs"], r["ip"]), (HANDLER_SEG, 0x210), model + " lidt")
            # an IDT limit of 0: INT 3 has no vector, no #GP and no double fault either, so the CPU shuts down
            r = run_script(["model " + model] + case_lines([0xCC], regs={"idt": (0, 0)}))[0]
            self.assertEqual(r.stop, 4, model + " shutdown")
            # a limit that holds the #GP vector but not 21h: #GP instead, pointing at the INT
            r = run_script(["model " + model] + case_lines([0xCD, 0x21], regs={"idt": (0, 13 * 4 + 3)}, mem=ivt(13)))[0]
            fault_at(self, r, 13, model + " vector beyond the IDT limit")

    def test_msw(self):
        for model, msw in (("386", 0xFFE0), ("486", 0x0010)):
            b = Batch(self, model)
            b.add([0x0F, 0x01, 0xE0], lambda res, msw=msw: self.assertEqual(res["ax"], msw))                       # smsw ax
            b.add([0x0F, 0x01, 0xF0], lambda res: self.assertEqual(res.stop, 1), regs={"eax": 1})                   # lmsw ax with PE
            b.add([0x0F, 0x01, 0xF0, 0x0F, 0x20, 0xC3], lambda res: self.assertEqual(res["ebx"] & 0xE, 0xE), regs={"eax": 0xE}, steps=2)
            b.add([0x0F, 0x06, 0x0F, 0x20, 0xC3], lambda res: self.assertEqual(res["ebx"] & 8, 0), regs={"cr0": 8}, steps=2)  # clts
            b.run()

    def test_486_only_and_never(self):
        b = Batch(self, "486")
        for code in ([0x0F, 0x08], [0x0F, 0x09], [0x0F, 0x01, 0x3F]):                   # invd, wbinvd, invlpg [bx]
            b.add(code, lambda res, code=code: (no_fault(self, res, bytes(code).hex()), self.assertEqual(res["ip"], 0x100 + len(code))),
                  mem=ivt(6))
        b.add([0x0F, 0x01, 0xF8], lambda res: fault_at(self, res, 6, "invlpg with a register"), mem=ivt(6))
        b.run()
        for model in M386:
            b = Batch(self, model)
            for code in ([0x0F, 0xA2], [0x0F, 0x0B], [0x0F, 0x31], [0x0F, 0x40, 0xC0], [0x0F, 0xAA], [0x0F, 0x07]):
                if code == [0x0F, 0x07]:                # LOADALL: not modelled, the CPU stops on it
                    b.add(code, lambda res: self.assertEqual(res.stop, 3), mem=ivt(6))
                    continue
                # cpuid, ud2, rdtsc, cmovo, rsm: not on these chips
                b.add(code, lambda res, code=code: fault_at(self, res, 6, model + " " + bytes(code).hex()), mem=ivt(6))
            b.run()

    def test_no_fpu(self):
        d = lin(DS0, DATA)
        for model in M386:
            b = Batch(self, model)
            b.add([0xDB, 0xE3], lambda res: self.assertEqual(res["ip"], 0x102))                                   # fninit: nothing
            b.add([0xDF, 0xE0], lambda res: self.assertEqual(res["ax"], 0xFFFF), regs={"eax": 0})                  # fnstsw ax
            b.add([0xD9, 0x3E] + le(DATA, 2), lambda res: self.assertEqual(res.mem(d, 2), 0xFFFF))               # fnstcw [DATA]
            b.add([0xDD, 0x3E] + le(DATA, 2), lambda res: self.assertEqual(res.mem(d, 2), 0xFFFF))               # fnstsw [DATA]
            b.add([0xD9, 0xE8], lambda res: fault_at(self, res, 7, "EM set"), regs={"cr0": 4}, mem=ivt(7))         # fld1 with EM: #NM
            b.add([0x9B], lambda res: fault_at(self, res, 7, "wait with MP+TS"), regs={"cr0": 0xA}, mem=ivt(7))
            b.add([0x9B], lambda res: no_fault(self, res, "wait with TS only"), regs={"cr0": 0x8}, mem=ivt(7))
            b.run()


@unittest.skipUnless(have_gcc(), "needs gcc")
class Faults386(unittest.TestCase):
    """LOCK rules, invalid opcodes, the instruction length limit, divide error, BOUND, ICEBP."""

    def test_lock_prefix(self):
        h = ivt(6)
        good = [[0xF0, 0x01, 0x06] + le(DATA, 2), [0xF0, 0x0F, 0xAB, 0x06] + le(DATA, 2), [0xF0, 0x87, 0x06] + le(DATA, 2),
                [0xF0, 0xFF, 0x06] + le(DATA, 2), [0xF0, 0xF6, 0x16] + le(DATA, 2), [0xF0, 0x66, 0x83, 0x2E] + le(DATA, 2) + [1],
                [0xF0, 0x0F, 0xBA, 0x36] + le(DATA, 2) + [3]]
        bad = [[0xF0, 0x01, 0xD8], [0xF0, 0x89, 0x06] + le(DATA, 2), [0xF0, 0x39, 0x06] + le(DATA, 2),
               [0xF0, 0x0F, 0xA3, 0x06] + le(DATA, 2), [0xF0, 0x90], [0xF0, 0x85, 0x06] + le(DATA, 2), [0xF0, 0x40],
               [0xF0, 0x87, 0xD8], [0xF0, 0x80, 0x3E] + le(DATA, 2) + [1], [0xF0, 0x0F, 0xBA, 0x26] + le(DATA, 2) + [3]]
        for model in M386:
            b = Batch(self, model)
            for code in good:
                b.add(code, lambda res, code=code: (no_fault(self, res, bytes(code).hex()), self.assertEqual(res["ip"], 0x100 + len(code))), mem=h)
            for code in bad:
                b.add(code, lambda res, code=code: fault_at(self, res, 6, "lock " + bytes(code).hex()), mem=h)
            b.run()

    def test_invalid_opcodes(self):
        codes = ([0x63, 0xC0], [0xC6, 0xC8, 0x00], [0xC7, 0xC8, 0x00, 0x00], [0x8F, 0xC8], [0xFE, 0xD0], [0xFF, 0xF8],
                 [0xFF, 0xD8], [0x0F, 0x00, 0xC0], [0x0F, 0xBA, 0xC0, 0x01], [0x0F, 0xA6, 0xC0], [0x0F, 0xFF], [0x8D, 0xC0],
                 [0xC4, 0xC0], [0x62, 0xC0], [0x0F, 0xB2, 0xC0], [0x0F, 0x02, 0xC0], [0x0F, 0x03, 0xC0], [0x0F, 0x01, 0xE8],
                 [0x0F, 0x01, 0xD0])
        for model in M386:
            b = Batch(self, model)
            for code in codes:
                b.add(code, lambda res, code=code: fault_at(self, res, 6, model + " " + bytes(code).hex()), mem=ivt(6))
            b.run()

    def test_instruction_length(self):
        for model in M386:
            b = Batch(self, model)
            b.add([0x26] * 14 + [0x90], lambda res: (no_fault(self, res, "15 bytes"), self.assertEqual(res["ip"], 0x10F)), mem=ivt(13))
            b.add([0x26] * 15 + [0x90], lambda res: fault_at(self, res, 13, "16 bytes"), mem=ivt(13))
            b.run()

    def test_divide_bound_icebp(self):
        d = lin(DS0, DATA)
        for model in M386:
            b = Batch(self, model, defaults={"sp": 0x1000})
            b.add([0x66, 0xF7, 0xF3], lambda res: fault_at(self, res, 0, "div ebx, 0"), regs={"eax": 5}, mem=ivt(0))
            # bound eax, [DATA] with doubleword bounds 0..10 (66 62 06): 11 is out (#BR, a fault), 5 is in
            bounds = {d: b"\x00\x00\x00\x00\x0A\x00\x00\x00"}
            b.add([0x66, 0x62, 0x06] + le(DATA, 2), lambda res: fault_at(self, res, 5, "bound 11"), regs={"eax": 11}, mem={**ivt(5), **bounds})
            b.add([0x66, 0x62, 0x06] + le(DATA, 2), lambda res: no_fault(self, res, "bound 5"), regs={"eax": 5}, mem={**ivt(5), **bounds})
            # icebp (F1): INT 1, a trap (the IP after it is pushed)
            b.add([0xF1], lambda res: fault_at(self, res, 1, "icebp", ip=0x101), mem=ivt(1))
            # int 21h with an operand-size prefix still pushes a 16-bit frame in real mode
            b.add([0x66, 0xCD, 0x21], lambda res: (self.assertEqual((res["cs"], res["ip"], res["sp"]), (HANDLER_SEG, 0x210, 0x1000 - 6)),
                                                   self.assertEqual(res.mem(lin(SS0, 0x1000 - 6), 2), 0x103)), mem=ivt(0x21))
            b.run()


@unittest.skipUnless(have_gcc(), "needs gcc")
class Flags386(unittest.TestCase):
    """What FLAGS and EFLAGS can hold in real mode: IOPL and NT yes, bit 15 no; AC on the 486 only; ID never."""

    def test_flags_images(self):
        ss, sp = SS0, 0x1000
        for model, ac in (("386", 0), ("486", AC)):
            b = Batch(self, model, defaults={"sp": sp})
            # popf of FEFFh (all but TF, which would trap), then pushf (9D 9C): IOPL and NT stick, bit 15 does not
            b.add([0x9D, 0x9C], lambda res: (self.assertEqual(res["flags"], 0x7ED7), self.assertEqual(res.mem(lin(ss, sp), 2), 0x7ED7)),
                  mem={lin(ss, sp): b"\xFF\xFE"}, steps=2)
            b.add([0x9D], lambda res: self.assertEqual(res["flags"], 0x0002), mem={lin(ss, sp): b"\x00\x00"})
            # popfd of FFFFFEFFh (66 9D): only AC (486) above bit 15
            b.add([0x66, 0x9D, 0x66, 0x9C], lambda res, ac=ac: (self.assertEqual(res["eflags"], 0x7ED7 | ac),
                                                                self.assertEqual(res.mem(lin(ss, sp), 4), 0x7ED7 | ac)),
                  mem={lin(ss, sp): b"\xFF\xFE\xFF\xFF"}, steps=2)
            # the ID flag (bit 21) cannot be set on either: no CPUID
            b.add([0x66, 0x9D], lambda res: self.assertEqual(res["eflags"] & 0x200000, 0), mem={lin(ss, sp): b"\x02\x00\x20\x00"})
            # sahf/lahf as on the 8086
            b.add([0x9E, 0x9F], lambda res: (self.assertEqual(res["flags"], 0x00D7), self.assertEqual(res["ah"], 0xD7)),
                  regs={"eax": 0xFF00, "eflags": F386}, steps=2)
            b.run()
        # an interrupt clears the 486's AC and pushes the 16-bit FLAGS
        b = Batch(self, "486", defaults={"sp": sp})
        b.add([0xCD, 0x21], lambda res: (self.assertEqual(res["eflags"] & AC, 0), self.assertEqual(res.mem(lin(ss, sp - 2), 2), 0x0202)),
              regs={"eflags": AC | 0x0202}, mem=ivt(0x21))
        b.run()


@unittest.skipUnless(have_gcc(), "needs gcc")
class Hardware386(unittest.TestCase):
    """What the manuals do not say and the SingleStepTests 80386 vectors (a real 386EX) do: one case each, so these hold
    without the opt-in vector suite (tests/host/x86sst.py --cpu 386)."""

    def test_far_pointer_second_part_wraps(self):
        # lss di, [ss:bp+di] with BP+DI = FFFEh (36 0F B2 3B): the offset from SS:FFFEh, the selector from SS:0000h
        b = Batch(self, "386")
        b.add([0x0F, 0xB2, 0x3B], lambda res: (no_fault(self, res, "lss at FFFE"), self.assertEqual((res["di"], res["ss"]), (0x1234, 0x5678))),
              regs={"bp": 0xFFFE, "di": 0}, mem={**ivt(12), **ivt(13), lin(SS0, 0xFFFE): b"\x34\x12", lin(SS0, 0): b"\x78\x56"})
        b.run()

    def test_pushad_faults_part_way(self):
        # pushad with SP = 000Eh: the block starts at FFEEh; EDI, ESI, EBP and ESP fit below the wrap and are written,
        # EBX would cross it: #SS, with SP as it was
        regs = {"eax": 0xAAAAAAAA, "ecx": 0xCCCCCCCC, "edx": 0xDDDDDDDD, "ebx": 0xBBBBBBBB, "esp": 0x000E, "ebp": 0x99999999,
                "esi": 0x55555555, "edi": 0x77777777}
        b = Batch(self, "386")
        b.add([0x66, 0x60], lambda res: (
            self.assertEqual((res["cs"], res["ip"]), (HANDLER_SEG, 0xC0)),
            self.assertEqual([res.mem(lin(SS0, 0xFFEE + 4 * i), 4) for i in range(4)], [0x77777777, 0x55555555, 0x99999999, 0x000E]),
            self.assertNotIn(lin(SS0, 0xFFFE), res.writes), self.assertEqual(res["sp"], 0x000E - 6)), regs=regs, mem=ivt(12))
        b.run()

    def test_vector_read_before_the_frame(self):
        # SS = 0000h, SP = 0018h: INT 5's frame lands on the vector table entries of INT 4 and INT 5 (0010h-0017h)
        mem = {5 * 4: bytes([0x50, 0x00, HANDLER_SEG & 0xFF, HANDLER_SEG >> 8]), lin(HANDLER_SEG, 0x50): b"\xF4"}
        b = Batch(self, "386")
        b.add([0xCD, 0x05], lambda res: self.assertEqual((res["cs"], res["ip"]), (HANDLER_SEG, 0x50)), regs={"ss": 0, "sp": 0x18}, mem=mem)
        b.run()

    def test_rep_runs_from_its_decoded_form(self):
        # rep movsb at 1000:0100 copying a HLT over its own opcode byte (DI = 102h, the A4): the copy still finishes
        code = [0xF3, 0xA4]
        src = lin(DEFAULT["cs"], 0x200)
        r = run_script(["model 386"] + case_lines(code, regs={"cx": 4, "si": 0x200, "di": 0x101, "ds": DEFAULT["cs"],
                                                                "es": DEFAULT["cs"]}, mem={src: b"\xF4\xF4\xF4\xF4"}))[0]
        self.assertEqual((r["cx"], r["si"], r["di"]), (0, 0x204, 0x105))

    def test_shift_flags(self):
        b = Batch(self, "386")
        # shl bl, cl with CL = 16 and 24: CF as for a shift by 8 (bit 0), OF = CF; CL = 9: CF = 0
        for cnt, cf in ((16, 1), (24, 1), (9, 0), (8, 1)):
            b.add([0xD2, 0xE3], lambda res, cf=cf, cnt=cnt: (self.assertEqual(res["flags"] & CF, cf, cnt),
                                                              self.assertEqual(bool(res["flags"] & OF), bool(cf), cnt)),
                  regs={"ebx": 0x81, "ecx": cnt, "eflags": F386})
        # shr bl, cl with CL = 16: CF = bit 7
        b.add([0xD2, 0xEB], lambda res: self.assertEqual(res["flags"] & CF, 1), regs={"ebx": 0x81, "ecx": 16, "eflags": F386})
        # SHL, SHR, SAR, SHLD and SHRD set AF; logic instructions clear it
        for code in ([0xD0, 0xE3], [0xD0, 0xEB], [0xD0, 0xFB], [0x0F, 0xA4, 0xD3, 3], [0x0F, 0xAC, 0xD3, 3]):
            b.add(code, lambda res, code=code: self.assertTrue(res["flags"] & AF, bytes(code).hex()), regs={"ebx": 0x1234, "edx": 1, "eflags": F386})
        b.add([0x09, 0xD8], lambda res: self.assertFalse(res["flags"] & AF), regs={"eflags": F386 | AF})
        # SHRD by 3: OF is the XOR of the result's top two bits (here bit 15 = 1, bit 14 = 0)
        b.add([0x0F, 0xAC, 0xD3, 3], lambda res: (self.assertEqual(res["bx"], 0x8000 | (0x0010 >> 3)), self.assertTrue(res["flags"] & OF)),
              regs={"ebx": 0x0010, "edx": 0x0004, "eflags": F386})
        # aaa with AL = 7Ah (adjusted): SF/ZF/PF from AL + 6 = 80h, OF = its signed overflow
        b.add([0x37], lambda res: (self.assertEqual(res["ax"], 0x0100), self.assertEqual(res["flags"] & (SF | ZF | OF), SF | OF)),
              regs={"eax": 0x007A, "eflags": F386})
        # daa from 7Ah: AL = 80h, OF set (a signed overflow of the adjustment)
        b.add([0x27], lambda res: (self.assertEqual(res["al"], 0x80), self.assertTrue(res["flags"] & OF)), regs={"eax": 0x7A, "eflags": F386})
        # idiv clears OF
        b.add([0xF6, 0xFB], lambda res: self.assertFalse(res["flags"] & OF), regs={"eax": 100, "ebx": 3, "eflags": F386 | OF})
        b.run()


@unittest.skipUnless(have_gcc(), "needs gcc")
class Cycles386(unittest.TestCase):
    """The 386 and 486 clock counts (data sheet values for the common case; the 486 is faster, but pays a clock per
    prefix byte where the 386 pays none)."""

    def test_counts(self):
        table = [  # (code, 386, 486)
            ([0x01, 0xD8], 2, 1),                                       # add ax, bx
            ([0x66, 0x01, 0xD8], 2, 1 + 1),                             # add eax, ebx
            ([0x66, 0xA1, 0x00, 0x02], 4, 1 + 1),                       # mov eax, [0200]
            ([0x66, 0x01, 0x07], 7, 3 + 1),                             # add [bx], eax
            ([0xEB, 0x00], 10, 3),                                      # jmp short
            ([0xF3, 0x66, 0xA5], 5 + 4 * 4, 2 + 7 + 3 * 4),             # rep movsd, CX = 4
            ([0xCD, 0x10], 37, 30),                                     # int 10h
            ([0x66, 0xF7, 0xF3], 38, 1 + 40),                           # div ebx
        ]
        for model in M386:
            b = Batch(self, model)
            for code, c386, c486 in table:
                want = c386 if model == "386" else c486
                b.add(code, lambda res, want=want, code=code: self.assertEqual(res.cycles, want, model + " " + bytes(code).hex()),
                      regs={"cx": 4, "bx": 0x10, "si": 0x20, "eax": 1, "edx": 0, "ebx": 3}, mem=ivt(0x10))
            b.run()


if __name__ == "__main__":
    unittest.main()
