#!/usr/bin/env python3
"""Host tests for the YM2203/YM2608 model (user/lib/pc98/src/opna.c), through tests/host/opnatool.c.

The expected values come from the chips' documented behaviour (timer periods, the SSG's formulas, the ADPCM-B
procedure) and from opnamodel.py, a second implementation of the FM and SSG parts written in Python from the same
descriptions, deliberately without looking at the C code's structure: it computes its own tables with Python's math
module and runs sample by sample in the most literal way. Every register script and every sample is made up here;
no game data, no ROM (docs/PC98_SOUND.md).

    python tools/hosttest.py discover -s tests/host -p "test_opna.py" -v
"""
import math
import os
import struct
import subprocess
import sys
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import x86ref  # noqa: E402
import opnamodel as M  # noqa: E402

HERE = Path(__file__).resolve().parent
OUT = x86ref.REPO / "build" / "opna"
SCRIPTS = HERE / "opna"


def tool():
    return x86ref.build("opnatool", [HERE / "opnatool.c"])


def run(lines):
    r = subprocess.run([str(tool())], input="\n".join(lines) + "\n", capture_output=True, text=True, errors="replace",
                       env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
    if r.returncode != 0:
        raise RuntimeError("opnatool failed (%d): %s" % (r.returncode, r.stderr[-3000:]))
    return r.stdout.splitlines()


def answers(out, tag):
    return [l.split()[1:] for l in out if l.startswith(tag + " ")]


def statuses(out, mask=0x7F):
    """Every 'S v' answer as an int, BUSY masked off unless asked for."""
    return [int(a[0], 16) & mask for a in answers(out, "S")]


def state(out):
    line = [l for l in out if l.startswith("Z ")][-1]
    return {k: v for k, v in (kv.split("=") for kv in line[2:].split())}


def frames(out):
    return [(int(a[0]), int(a[1])) for a in answers(out, "F")]


def w(bank, reg, val):
    return "w %x %x %x" % (bank, reg, val)


# ------------------------------------------------------------------ timers, flags, interrupt line, busy

@unittest.skipUnless(x86ref.have_gcc(), "needs gcc")
class Timers(unittest.TestCase):
    def test_timer_a_exact_count(self):
        # NA = 1000: 24 samples of 144 master clocks on the YM2608
        out = run(["init 2608 7987200", w(0, 0x24, 1000 >> 2), w(0, 0x25, 1000 & 3), w(0, 0x27, 0x05),
                   "adv 3455", "st 0", "irq", "adv 1", "st 0", "irq",
                   w(0, 0x27, 0x15), "st 0", "irq",          # reset the flag, keep it running
                   "adv 3455", "st 0", "adv 1", "st 0", "irq"])
        self.assertEqual(statuses(out), [0, 1, 0, 0, 1])
        self.assertEqual([a[0] for a in answers(out, "I")], ["0", "1", "0", "1"])
        self.assertEqual(answers(out, "I")[-1][1], "2", "two rising edges")

    def test_timer_b_exact_count(self):
        # NB = 250: 6 ticks of 16 samples
        out = run(["init 2608 7987200", w(0, 0x26, 250), w(0, 0x27, 0x0A),
                   "adv %d" % (96 * 144 - 1), "st 0", "adv 1", "st 0", "irq"])
        self.assertEqual(statuses(out), [0, 2])
        self.assertEqual(answers(out, "I")[0][0], "1")

    def test_ym2203_sample_is_72_clocks(self):
        out = run(["init 2203 3993600", w(0, 0x24, 0xFF), w(0, 0x25, 3), w(0, 0x27, 0x05),   # NA = 1023: 1 sample
                   "adv 71", "st 0", "adv 1", "st 0", w(0, 0x27, 0x15), "adv 71", "st 0", "adv 1", "st 0",
                   w(0, 0x24, 0), w(0, 0x25, 0), w(0, 0x27, 0x15),        # NA = 0: 1024 samples from the next reload
                   "adv 72", "st 0", w(0, 0x27, 0x15), "adv %d" % (1024 * 72 - 1), "st 0", "adv 1", "st 0", "state"])
        self.assertEqual(statuses(out), [0, 1, 0, 1, 1, 0, 1])
        self.assertEqual(state(out)["sample_clocks"], "72")

    def test_enable_and_load_bits(self):
        out = run(["init 2608 7987200", w(0, 0x24, 0xFF), w(0, 0x25, 3),
                   w(0, 0x27, 0x01), "adv 1000", "st 0", "irq",           # loaded, not enabled: counts, no flag
                   w(0, 0x27, 0x04), "adv 1000", "st 0", "state",          # enabled, stopped: nothing counts
                   w(0, 0x27, 0x05), "adv 144", "st 0", "irq"])
        self.assertEqual(statuses(out), [0, 0, 1])
        self.assertEqual(answers(out, "I")[0][0], "0")
        self.assertEqual(answers(out, "I")[1][0], "1")

    def test_new_value_waits_for_the_reload_and_load_does_not_restart(self):
        # NA = 1000 (24 samples) running; at sample 10 NA becomes 1020 (4 samples) and LOAD is written again
        out = run(["init 2608 7987200", w(0, 0x24, 1000 >> 2), w(0, 0x25, 0), w(0, 0x27, 0x05),
                   "adv %d" % (10 * 144), w(0, 0x24, 1020 >> 2), w(0, 0x25, 0), w(0, 0x27, 0x05),
                   "adv %d" % (14 * 144 - 1), "st 0", "adv 1", "st 0", w(0, 0x27, 0x15),
                   "adv %d" % (4 * 144 - 1), "st 0", "adv 1", "st 0"])
        self.assertEqual(statuses(out), [0, 1, 0, 1])

    def test_ym2608_irq_enable_and_flag_mask(self):
        out = run(["init 2608 7987200", w(0, 0x29, 0x80), w(0, 0x24, 0xFF), w(0, 0x25, 3), w(0, 0x27, 0x05),
                   "adv 144", "st 0", "st 1", "irq",                        # flag set, IRQ not enabled
                   w(0, 0x29, 0x81), "irq",                                 # enabling raises the line
                   w(1, 0x10, 0x80), "st 0", "irq",                         # IRQ RESET clears the flags
                   w(1, 0x10, 0x01), "adv 144", "st 0", "st 1", "irq",      # timer A masked: never set
                   w(1, 0x10, 0x00), "adv 144", "st 0", "irq", "state"])
        self.assertEqual(statuses(out), [1, 1, 0, 0, 0, 1])
        self.assertEqual([a[0] for a in answers(out, "I")], ["0", "1", "0", "0", "1"])
        self.assertEqual(state(out)["six"], "1")

    def test_busy_after_data_writes(self):
        out = run(["init 2608 7987200", "st 0", w(0, 0xB0, 0x07), "st 0", "st 1", "adv 82", "st 0", "adv 1", "st 0",
                   w(0, 0x00, 0x12), "st 0",                                # SSG writes: no wait
                   "a 0 b0", "st 0"])                                       # nor do address writes
        self.assertEqual(statuses(out, 0xFF), [0, 0x80, 0x80, 0x80, 0, 0, 0])

    def test_host_clock_and_next_event(self):
        # 24 samples = 3456 master clocks = 3461.5 cycles of an 8 MHz CPU
        out = run(["init 2608 7987200", "host 8000000", "next", w(0, 0x24, 1000 >> 2), w(0, 0x25, 0),
                   w(0, 0x27, 0x05), "next", "adv 3461", "st 0", "adv 1", "st 0", "next",
                   w(0, 0x26, 0xFF), w(0, 0x27, 0x1F), "next"])
        n = [int(a[0]) for a in answers(out, "N")]
        self.assertEqual(n[0], -1, "nothing armed")
        self.assertEqual(n[1], 3462)
        self.assertEqual(statuses(out), [0, 1])
        self.assertEqual(n[2], -1, "flag A already up: no new edge from A")
        # after 1Fh (timer A flag cleared, B loaded with NB = 255: 16 samples = 2304 clocks) B comes first; 2686400
        # is what the 8 MHz -> 7.9872 MHz conversion carried over from the 3462 cycles so far
        self.assertEqual(n[3], -(-(2304 * 8000000 - 2686400) // 7987200))

    def test_prescaler(self):
        # the address alone selects: 2Eh after reset = 1/3, 2Fh = 1/2, 2Dh = 1/6 again, 2Eh = 1/3
        out = run(["init 2203 3993600", "a 0 2e", "state", "a 0 2f", "state", "a 0 2d", "state", "a 0 2e", "state",
                   w(0, 0x24, 1000 >> 2), w(0, 0x25, 0), w(0, 0x27, 0x05), "adv %d" % (24 * 36 - 1), "st 0", "adv 1",
                   "st 0", "init 2608 7987200", "a 0 2f", "state"])
        sc = [(s["sample_clocks"], s["ssg_clocks"]) for s in (state([l]) for l in out if l.startswith("Z "))]
        self.assertEqual(sc, [("36", "16"), ("24", "8"), ("72", "32"), ("36", "16"), ("48", "16")])
        self.assertEqual(statuses(out), [0, 1])

    def test_irq_callback_edges_through_reset(self):
        lines = ["init 2203 3993600", w(0, 0x24, 0xFF), w(0, 0x25, 3), w(0, 0x27, 0x05)]
        for _ in range(5):
            lines += ["adv 72", w(0, 0x27, 0x15)]
        lines.append("irq")
        out = run(lines)
        self.assertEqual(answers(out, "I")[0], ["0", "5"])


# ------------------------------------------------------------------ registers, ID, board decoding

@unittest.skipUnless(x86ref.have_gcc(), "needs gcc")
class Registers(unittest.TestCase):
    def test_power_on_state_and_size(self):
        out = run(["init 2608 7987200", "st 0", "st 1", "irq", "state"])
        self.assertEqual(statuses(out, 0xFF), [0, 0])
        z = state(out)
        self.assertEqual((z["irq_enable"], z["sample_clocks"], z["ssg_clocks"], z["six"]), ("1f", "144", "64", "0"))
        size = int(z["size"])
        print("\nopna: sizeof(Opna) = %d bytes" % size, file=sys.stderr)
        self.assertLess(size, 300 * 1024, "the ADPCM RAM (256 KiB) and the FIFO (16 KiB) are most of it")

    def test_ssg_registers_read_back_whole_bytes(self):
        out = run(["init 2203 3993600", w(0, 0x0B, 0xAA), "rd 0", w(0, 0x07, 0xBF), "rd 0", w(0, 0x01, 0xFF), "rd 0",
                   "a 0 0e", "rd 0", w(0, 0x07, 0x40), w(0, 0x0E, 0x5A), "rd 0"])
        self.assertEqual([a[0] for a in answers(out, "D")], ["aa", "bf", "ff", "ff", "5a"])

    def test_id_register(self):
        out = run(["init 2608 7987200", "a 0 ff", "rd 0", "init 2203 3993600", "a 0 ff", "rd 0", "st 1", "rd 1"])
        self.assertEqual([a[0] for a in answers(out, "D")], ["1", "0", "ff"])
        self.assertEqual(statuses(out, 0xFF), [0xFF])

    def test_board_26k(self):
        out = run(["board 26k 12", "bin 188", "bout 188 b", "bout 18a aa", "bin 18a", "bin 18c", "bin 18e", "bin a460",
                   "bout 188 e", "bin 18a", "board 26k 3", "bout 188 e", "bin 18a"])
        b = [a[0] for a in answers(out, "B")]
        self.assertEqual(b, ["0", "aa", "-1", "-1", "-1", "ff", "3f"])

    def test_board_86_second_bank_and_irq_jumper(self):
        out = run(["board 86 10", "bin a460", "bout 188 ff", "bin 18a",
                   "bin 18c", "bin 18e",                                    # OPNA mode off: the second bank is not there
                   "bout a460 41", "bin a460", "bin 18c",                   # on
                   "bout 18c 10", "bout 18e 80", "bin 18c",
                   "bout 188 e", "bin 18a", "state"])
        b = [a[0] for a in answers(out, "B")]
        self.assertEqual(b[:4], ["40", "1", "ff", "ff"])
        self.assertEqual(b[4], "41")
        self.assertEqual(int(b[5], 16) & 0x7F, 0)
        self.assertEqual(b[7], "bf", "port A bits 7-6 = 10: INT41 (IRQ 10)")
        self.assertEqual(state(out)["clock"], "7987200")


@unittest.skipUnless(x86ref.have_gcc(), "needs gcc")
class Pcm86(unittest.TestCase):
    """The -86 board's PCM FIFO ports A466h-A46Ch (docs/PC98_SOUND.md, "The -86 board's PCM FIFO"): registers that read back, a data
    port that takes bytes, and an interrupt flag (A468h bit 4) that never rises. PMD86's interrupt entry loops for as long as
    that bit reads as set, with interrupts off: an unhandled port answers FFh, and TH02 to TH05 never got past their sound driver."""

    def test_registers_read_back_and_the_flag_reads_clear(self):
        out = run(["board 86 12", "bin a466", "bin a468", "bin a46a", "bin a46c",
                   "bout a466 a3", "bout a46a f2", "bout a468 7f", "bin a466", "bin a46a", "bin a468",
                   "bout a468 ff", "bin a468",                              # bit 4 written as 1 still reads 0: no interrupt is pending
                   "bout a46c 55", "bout a46c 66", "bin a46c", "state", "adv 3000000", "irq"])
        b = [a[0] for a in answers(out, "B")]
        self.assertEqual(b[:4], ["0", "0", "0", "0"], "power-on state")
        self.assertEqual(b[4:7], ["a3", "f2", "6f"], "volume and mode read back as written, the control byte without bit 4 (7Fh written)")
        self.assertEqual(b[7], "ef", "A468h reads with bit 4 clear whatever was written: PMD86 serves the FIFO while it is set")
        self.assertEqual(b[8], "0", "the data port reads 0")
        z = state(out)
        self.assertEqual((z["pcm_vol"], z["pcm_mode"], z["pcm_ctrl"], z["pcm_bytes"]), ("a3", "f2", "ff", "2"))
        self.assertEqual(answers(out, "I")[-1][0], "0", "no interrupt, however long the FIFO is 'playing'")

    def test_only_the_86_has_them(self):
        out = run(["board 26k 12", "bin a466", "bout a468 10", "bin a468", "board 86 12", "bin a468"])
        self.assertEqual([a[0] for a in answers(out, "B")], ["-1", "-1", "0"])


# ------------------------------------------------------------------ tables

@unittest.skipUnless(x86ref.have_gcc(), "needs gcc")
class Tables(unittest.TestCase):
    def table(self, name):
        out = run(["init 2608 7987200", "table " + name])
        return [int(v) for v in out[0].split()[2:]]

    def test_logsin_and_exponent(self):
        self.assertEqual(self.table("logsin"), M.LOGSIN)
        self.assertEqual(self.table("exptab"), M.EXP)
        # spot values straight from the definitions
        self.assertEqual(M.LOGSIN[0], 2137)
        self.assertEqual(M.LOGSIN[255], 0)
        self.assertEqual((M.EXP[0], M.EXP[255]), (2042, 1024))

    def test_ssg_rhythm_pm(self):
        self.assertEqual(self.table("ssgdac"), M.SSG_DAC)
        self.assertEqual(self.table("rhythm"), M.RHYTHM_GAIN)
        pm = self.table("pm")
        self.assertEqual([pm[i * 8:(i + 1) * 8] for i in range(8)], M.PM_RATIO)

    def test_operator_lookup(self):
        cases = [(0, 0, 0), (0x40000, 0, 0), (0x7FC00, 0, 0), (0x80000, 0, 0), (0xC0000, 0, 0), (0x12345, 0, 100),
                 (0x40000, 0, 1023), (0, 256, 0), (0, -256, 0), (0x55555, -77, 37), (0, 0, 640)]
        out = run(["init 2608 7987200"] + ["opout %x %d %x" % c for c in cases])
        got = [int(a[0]) for a in answers(out, "V")]
        self.assertEqual(got, [M.op_lookup(p, m, a) for p, m, a in cases])
        self.assertEqual(got[1], 8168, "the peak of the sine at full level")
        self.assertEqual(got[4], -8168)
        self.assertEqual(got[6], 0, "full attenuation")

    def test_phase_increments(self):
        cases = []
        for fnum, block in ((0x26A, 4), (0x410, 4), (0x7FF, 7), (0x001, 0), (0x080, 0), (0x4D4, 2)):
            for dt in range(8):
                for mul in (0, 1, 2, 7, 15):
                    for pm in (0, 3000, -3000):
                        cases.append((fnum, block, dt, mul, pm))
        out = run(["init 2608 7987200"] + ["inc %x %x %x %x %d" % c for c in cases])
        got = [int(a[0]) for a in answers(out, "C")]
        self.assertEqual(got, [M.phase_inc(*c) for c in cases])
        # A4 at block 4, F-number 1040, MUL 1: 2^20 phase units per cycle at 55466.67 Hz
        hz = M.phase_inc(1040, 4, 0, 1, 0) * (7987200 / 144) / 2 ** 20
        self.assertAlmostEqual(hz, 440.0, delta=0.5)
        # the detune adder wraps around (17 bits) when a negative detune meets a tiny F-number
        self.assertEqual(M.phase_inc(1, 0, 7, 1, 0), 0x1FFFF - 1)


# ------------------------------------------------------------------ SSG

def ssg_lines(chip, regs, n, dc=0):
    clock = 3993600 if chip == 2203 else 7987200
    lines = ["init %d %d" % (chip, clock), "dc %d" % dc]
    lines += [w(0, r, v) for r, v in regs]
    lines.append("ssgframes %d" % n)
    return lines


@unittest.skipUnless(x86ref.have_gcc(), "needs gcc")
class Ssg(unittest.TestCase):
    def check(self, chip, regs, n=3000):
        out = run(ssg_lines(chip, regs, n))
        got = [int(a[0]) for a in answers(out, "G")]
        want = M.ssg_frames(chip, dict(regs), n)
        self.assertEqual(got, want)
        return got

    def test_square_wave_pitch_and_level(self):
        for chip in (2203, 2608):
            with self.subTest(chip=chip):
                got = self.check(chip, [(0, 0x00), (1, 0x01), (7, 0x3E), (8, 0x0F)], 4000)   # TP 256, tone A, vol 15
                # tone = phiM / (64 TP) on the YM2203, phiM / (128 TP) on the YM2608: 243.75 Hz either way;
                # the square spends 256 SSG ticks (113.8 samples of 2.25 ticks) at each level
                rises = [i for i in range(1, len(got)) if got[i] and not got[i - 1]]
                period = (rises[-1] - rises[0]) / (len(rises) - 1)
                self.assertAlmostEqual(period, (7987200 / 144) / 243.75, delta=0.1)
                self.assertEqual(max(got), 8191)
                self.assertEqual(min(got), 0)

    def test_fixed_volumes(self):
        for v in (0, 1, 7, 15):
            with self.subTest(volume=v):
                got = self.check(2203, [(7, 0x3F), (8, v), (9, v), (10, 0)], 50)    # tone and noise off: DC level
                self.assertEqual(got[-1], 2 * (M.SSG_DAC[2 * v + 1] if v else 0))

    def test_noise(self):
        for np_ in (1, 5, 31):
            with self.subTest(np=np_):
                got = self.check(2608, [(6, np_), (7, 0x37), (8, 0x0C)], 3000)
                self.assertGreater(len(set(got)), 2, "noise has more than two levels after box filtering")

    def test_tone_and_noise_and_three_channels(self):
        self.check(2203, [(0, 0x55), (2, 0x99), (3, 0x01), (4, 0x30), (6, 3), (7, 0x31), (8, 0x0F), (9, 0x0A),
                          (10, 0x06)], 4000)

    def test_envelope_shapes(self):
        for shape in range(16):
            with self.subTest(shape=shape):
                self.check(2203, [(7, 0x3F), (8, 0x10), (11, 3), (12, 0), (13, shape)], 300)

    def test_envelope_slow_and_tone(self):
        self.check(2608, [(0, 0x20), (7, 0x3E), (8, 0x10), (11, 0x10), (12, 0), (13, 0x0E)], 3000)

    def test_dc_blocker_centres_the_square(self):
        out = run(ssg_lines(2203, [(0, 0x40), (7, 0x3E), (8, 0x0F)], 1, dc=1) + ["mix 0 256 0 0", "frames 20000"])
        f = [l for l, r in frames(out)]
        tail = f[-5000:]
        self.assertLess(abs(sum(tail) / len(tail)), 300, "no DC left after the coupling high-pass")
        self.assertGreater(max(tail), 3000)
        self.assertLess(min(tail), -3000)


# ------------------------------------------------------------------ FM

CLOCK = {2203: 3993600, 2608: 7987200}


def run_program(chip, program, extra=()):
    """program: ("w", bank, reg, val) writes and ("n", count) stretches of samples. Returns (C frames, model frames)."""
    lines = ["init %d %d" % (chip, CLOCK[chip]), "mix 256 0 0 0", *extra]
    model = M.Fm(chip)
    want = []
    for item in program:
        if item[0] == "w":
            _, b, r, v = item
            lines.append(w(b, r, v))
            model.write(b, r, v)
        else:
            lines.append("frames %d" % item[1])
            want += [model.sample() for _ in range(item[1])]
    return frames(run(lines)), want


def patch(rnd, c, alg, fb, loud=True):
    """Random but sensible register values for channel c (0-5): every operator field, the algorithm and feedback,
    a frequency; then the key-on. Carriers get a fast attack so the channel is heard."""
    bank, cc = (1, c - 3) if c >= 3 else (0, c)
    prog = []
    for idx, off in enumerate((0, 8, 4, 12)):           # S1, S2, S3, S4
        r = cc + off
        carrier = idx in M.CARRIERS[alg]
        prog.append(("w", bank, 0x30 + r, rnd.randrange(128)))
        prog.append(("w", bank, 0x40 + r, rnd.randrange(0, 24) if carrier and loud else rnd.randrange(0, 80)))
        prog.append(("w", bank, 0x50 + r, rnd.randrange(4) << 6 | (rnd.randrange(18, 32) if carrier else rnd.randrange(32))))
        prog.append(("w", bank, 0x60 + r, rnd.randrange(256)))
        prog.append(("w", bank, 0x70 + r, rnd.randrange(32)))
        prog.append(("w", bank, 0x80 + r, rnd.randrange(256)))
    prog.append(("w", bank, 0xB0 + cc, fb << 3 | alg))
    fnum, block = rnd.randrange(0x200, 0x800), rnd.randrange(1, 7)
    prog.append(("w", bank, 0xA4 + cc, block << 3 | fnum >> 8))
    prog.append(("w", bank, 0xA0 + cc, fnum & 0xFF))
    prog.append(("w", 0, 0x28, 0xF0 | cc | (4 if bank else 0)))
    return prog


def key_off(c):
    return ("w", 0, 0x28, (c % 3) | (4 if c >= 3 else 0))


def zero_crossings(xs):
    return sum(1 for a, b in zip(xs, xs[1:]) if a < 0 <= b)


@unittest.skipUnless(x86ref.have_gcc(), "needs gcc")
class Fm(unittest.TestCase):
    def test_pure_sine(self):
        # algorithm 7, S4 alone at full level: a 440 Hz sine with the chip's peak value
        prog = [("w", 0, 0xB2, 0x07), ("w", 0, 0x5E, 0x1F), ("w", 0, 0x3E, 0x01), ("w", 0, 0x4E, 0x00)]
        for off in (0x42, 0x46, 0x4A):
            prog.append(("w", 0, off, 0x7F))            # the other three silent
        prog += [("w", 0, 0xA6, 4 << 3 | 0x04), ("w", 0, 0xA2, 0x10), ("w", 0, 0x28, 0xF2), ("n", 5546)]
        for chip in (2203, 2608):
            with self.subTest(chip=chip):
                got, want = run_program(chip, prog)
                self.assertEqual(got, want)
                left = [l for l, r in got]
                self.assertEqual(max(left), 8168)
                self.assertEqual(min(left), -8168)
                self.assertAlmostEqual(zero_crossings(left) / (5546 / 55466.67), 440, delta=11)

    def test_algorithms_and_feedback(self):
        import random
        rnd = random.Random(2608)
        for chip in (2203, 2608):
            for alg in range(8):
                fb = rnd.randrange(8) if alg % 2 else 7 - alg
                with self.subTest(chip=chip, alg=alg, fb=fb):
                    prog = patch(rnd, 1, alg, fb) + [("n", 900), key_off(1), ("n", 300)]
                    got, want = run_program(chip, prog)
                    self.assertEqual(got, want)
                    self.assertTrue(any(l for l, r in got), "heard something")

    def test_all_channels_together_and_changes_while_playing(self):
        import random
        rnd = random.Random(86)
        prog = [("w", 0, 0x29, 0x80)]
        for c in range(6):
            prog += patch(rnd, c, rnd.randrange(8), rnd.randrange(8), loud=False)
        prog += [("n", 500),
                 ("w", 0, 0xA5, 0x22), ("w", 0, 0xA1, 0x80),            # a new note on channel 2 while it plays
                 ("w", 1, 0x40, 0x10), ("w", 1, 0x4C, 0x05),            # levels change on channel 4
                 ("w", 1, 0xB6, 0x80),                                   # channel 6 left only
                 ("n", 400), key_off(0), key_off(4), ("n", 300),
                 ("w", 0, 0x28, 0x31), ("n", 200)]                       # S1 and S2 of channel 2 again
        got, want = run_program(2608, prog)
        self.assertEqual(got, want)
        self.assertNotEqual([l for l, r in got], [r for l, r in got])

    def test_envelope_trace(self):
        # one operator through attack, decay, sustain and release, with key scaling
        for ks, ar, dr, sr, sl, rr, block in ((0, 14, 9, 3, 6, 5, 4), (3, 21, 25, 12, 2, 9, 7), (1, 31, 0, 0, 0, 15, 2),
                                              (2, 8, 31, 31, 15, 1, 5)):
            with self.subTest(ar=ar, ks=ks):
                regs = [(0, 0xB0, 0x07), (0, 0x30, 0x01), (0, 0x40, 0), (0, 0x50, ks << 6 | ar), (0, 0x60, dr),
                        (0, 0x70, sr), (0, 0x80, sl << 4 | rr), (0, 0xA4, block << 3 | 0x03), (0, 0xA0, 0x40),
                        (0, 0x28, 0x10)]
                lines = ["init 2608 7987200"] + [w(*r) for r in regs] + ["trace 3000 0 0", w(0, 0x28, 0x00), "trace 3000 0 0"]
                got = [(int(a[0]), int(a[1])) for a in answers(run(lines), "T")]
                model = M.Fm(2608)
                for r in regs:
                    model.write(*r)
                want = []
                for k in range(6000):
                    if k == 3000:
                        model.write(0, 0x28, 0x00)
                    model.sample()
                    op = model.ch[0].ops[0]
                    want.append((op.att, op.state))
                self.assertEqual(got, want)
                if ar == 31:
                    self.assertEqual(got[0], (0, M.DECAY), "attack rate 62-63: no attack at all")
                self.assertEqual(got[-1][1], M.RELEASE)

    def test_decay_speed_against_the_counter(self):
        # the fastest decay (rate 63) moves 8 steps per envelope event, one event per 3 samples: 0 -> 1023 in 128
        lines = ["init 2608 7987200", w(0, 0xB0, 7), w(0, 0x50, 0xDF), w(0, 0x60, 0x1F), w(0, 0x70, 0x1F),
                 w(0, 0x80, 0xF0), w(0, 0xA4, 0x38), w(0, 0xA0, 0), w(0, 0x28, 0x10), "trace 400 0 0"]
        att = [int(a[0]) for a in answers(run(lines), "T")]
        first_full = att.index(1023)
        self.assertEqual(first_full, 3 * 128 - 1)

    def test_lfo_am_and_pm(self):
        import random
        rnd = random.Random(55466)
        for lfo in (0x08, 0x0B, 0x0F):
            with self.subTest(lfo=lfo):
                prog = [("w", 0, 0x22, lfo)] + patch(rnd, 0, 4, 3)
                prog += [("w", 0, 0x6C, 0x85), ("w", 0, 0x64, 0x80 | 3), ("w", 0, 0xB4, 0xC0 | 3 << 4 | 7), ("n", 3000)]
                got, want = run_program(2608, prog)
                self.assertEqual(got, want)

    def test_lfo_off_and_ym2203_ignore_it(self):
        import random
        rnd = random.Random(7)
        base = patch(rnd, 0, 7, 0)
        with_lfo = [("w", 0, 0x22, 0x0F), ("w", 0, 0xB4, 0xF7)] + base + [("n", 800)]
        got_2203, _ = run_program(2203, with_lfo)
        plain, _ = run_program(2203, base + [("n", 800)])
        self.assertEqual(got_2203, plain, "the YM2203 has no LFO and no B4h")

    def test_channel3_special_mode(self):
        # 27h bits 7-6 = 01: S1, S2, S3 of channel 3 take their own frequencies (A9h/ADh, AAh/AEh, A8h/ACh)
        prog = [("w", 0, 0x27, 0x40), ("w", 0, 0xB2, 0x07)]
        for off in (0x32, 0x36, 0x3A, 0x3E):
            prog.append(("w", 0, off, 0x01))
        for off in (0x42, 0x46, 0x4A, 0x4E):
            prog.append(("w", 0, off, 0x10))
        for off in (0x52, 0x56, 0x5A, 0x5E):
            prog.append(("w", 0, off, 0x1F))
        prog += [("w", 0, 0xAD, 0x22), ("w", 0, 0xA9, 0x6A),      # S1: C4
                 ("w", 0, 0xAE, 0x23), ("w", 0, 0xAA, 0x0B),      # S2: E4
                 ("w", 0, 0xAC, 0x23), ("w", 0, 0xA8, 0x9E),      # S3: G4
                 ("w", 0, 0xA6, 0x2C), ("w", 0, 0xA2, 0xD4),      # S4: C5
                 ("w", 0, 0x28, 0xF2), ("n", 2000),
                 ("w", 0, 0x27, 0x00), ("n", 500)]                 # back to normal: all four at C5
        for chip in (2203, 2608):
            with self.subTest(chip=chip):
                got, want = run_program(chip, prog)
                self.assertEqual(got, want)

    def test_csm_keys_channel_3_with_timer_a(self):
        regs = [(0, 0xB2, 0x07), (0, 0x5E, 0x1F), (0, 0x8E, 0x0F), (0, 0x3E, 0x01), (0, 0x4E, 0), (0, 0xA6, 0x22),
                (0, 0xA2, 0x6A)]
        for off in (0x42, 0x46, 0x4A):
            regs.append((0, off, 0x7F))
        lines = ["init 2608 7987200", "mix 256 0 0 0"] + [w(*r) for r in regs]
        lines += ["frames 300", w(0, 0x24, 0xF0), w(0, 0x25, 0), w(0, 0x27, 0x85), "frames 3000"]
        f = [l for l, r in frames(run(lines))]
        self.assertEqual(set(f[:300]), {0}, "nothing keyed yet")
        # NA = 3C0h: 64 samples per overflow; each overflow keys channel 3 on for one sample, which restarts the
        # attack and the phase: a sound that is cut short and restarted every 64 samples
        heard = [i for i, v in enumerate(f[300:]) if v]
        self.assertTrue(heard and heard[0] < 70, heard[:3])

    def test_pan_and_six_channels(self):
        import random
        rnd = random.Random(1)
        prog = patch(rnd, 3, 7, 0)                     # channel 4 while 29h says three channels: silent
        got, _ = run_program(2608, prog + [("n", 300)])
        self.assertEqual(set(got), {(0, 0)})
        rnd = random.Random(1)
        prog = [("w", 0, 0x29, 0x80)] + patch(rnd, 3, 7, 0) + [("w", 1, 0xB4, 0x80), ("n", 300)]
        got, want = run_program(2608, prog)
        self.assertEqual(got, want)
        self.assertTrue(any(l for l, r in got))
        self.assertEqual({r for l, r in got}, {0}, "left only")

    def test_ssg_type_envelope_shapes(self):
        """The approximate SSG-type envelope (90h-9Eh): the shapes of the SSG's envelope, on an FM operator. S4 alone
        (algorithm 7) at 1760 Hz, fast attack, decay rate 20: with the x4 of this mode a ramp of 200h (48 dB) takes
        1536 samples, so 2 dB per block of 64 samples; a restart jumps by about 45 dB."""
        def envelope(shape):
            lines = ["init 2608 7987200", "mix 256 0 0 0", w(0, 0xB0, 7), w(0, 0x3C, 1), w(0, 0x4C, 0), w(0, 0x5C, 0x1F),
                     w(0, 0x6C, 20), w(0, 0x7C, 20), w(0, 0x8C, 0xFF), w(0, 0x9C, shape)] + \
                    [w(0, r, 0x7F) for r in (0x40, 0x44, 0x48)] + [w(0, 0xA4, 0x34), w(0, 0xA0, 0x10), w(0, 0x28, 0x80),
                                                                     "frames 22016"]
            f = [abs(l) for l, r in frames(run(lines))]
            peaks = [max(f[i:i + 64]) for i in range(0, 22016, 64)]
            return [20 * math.log10(p / 8168) if p else -100.0 for p in peaks]     # dB per block of 64 samples
        rises = lambda db, by: sum(1 for a, b in zip(db, db[1:]) if b > a + by)
        plain = envelope(0x00)                 # off: one long decay
        self.assertGreater(plain[0], -1)
        self.assertEqual(rises(plain, 1), 0, "a plain decay only goes down")
        saw = envelope(0x08)                   # \\\\ : decays again and again, jumping back up
        self.assertGreaterEqual(rises(saw, 30), 10)
        once = envelope(0x09)                  # \___ : one decay, then silence
        self.assertEqual(max(once[-100:]), -100.0)
        held = envelope(0x0B)                  # \--- : one decay, then held at full level
        self.assertGreater(min(held[-100:]), -1)
        tri = envelope(0x0A)                   # \/\/ : down and back up, turning round without jumps
        self.assertEqual(rises(tri, 6), 0)
        turns = sum(1 for a, b, c in zip(tri, tri[1:], tri[2:]) if b <= a and b < c and b < -30)
        self.assertGreaterEqual(turns, 5)
        rise = envelope(0x0D)                  # /--- : rises once, then held
        self.assertLess(rise[0], -30)
        self.assertGreater(min(rise[-100:]), -1)
        self.assertEqual(rises(rise, 6), 0)

    def test_release_ends_in_silence(self):
        import random
        rnd = random.Random(3)
        prog = patch(rnd, 0, 7, 0) + [("w", 0, 0x80, 0x0F), ("w", 0, 0x84, 0x0F), ("w", 0, 0x88, 0x0F),
                                      ("w", 0, 0x8C, 0x0F), ("n", 400), key_off(0), ("n", 1500)]
        got, want = run_program(2203, prog)
        self.assertEqual(got, want)
        self.assertEqual(set(got[-200:]), {(0, 0)})

    def test_regression_hash(self):
        # a fixed patch of every feature at once; the hash guards against silent changes (update it only together
        # with a reason in the commit message)
        import random
        rnd = random.Random(1997)
        prog = [("w", 0, 0x29, 0x80), ("w", 0, 0x22, 0x0C)]
        for c in range(6):
            prog += patch(rnd, c, c + 1 if c < 6 else 0, c)
        prog += [("w", 0, 0xB4, 0xF5), ("n", 4096)]
        got, want = run_program(2608, prog)
        self.assertEqual(got, want)
        crc = zlib.crc32(b"".join(struct.pack("<hh", l, r) for l, r in got))
        self.assertEqual("%08x" % crc, REGRESSION_CRC)


REGRESSION_CRC = "fcbdd801"


# ------------------------------------------------------------------ rhythm

@unittest.skipUnless(x86ref.have_gcc(), "needs gcc")
class Rhythm(unittest.TestCase):
    def test_one_drum_levels_and_dump(self):
        pcm = M.synthetic_drum(2000, 7)
        base = ["init 2608 7987200", "mix 0 0 256 0", "rhythmpcm 0 22050 2000 7"]
        for tl, il in ((0x3F, 0x1F), (0x30, 0x10), (0x00, 0x00)):
            with self.subTest(tl=tl, il=il):
                out = run(base + [w(0, 0x11, tl), w(0, 0x18, 0xC0 | il), w(0, 0x10, 0x01), "frames 6000"])
                want = M.rhythm_play(pcm, 22050, 7987200, 144, tl, il, 6000)
                self.assertEqual(frames(out), [(v, v) for v in want])
        # left only, then a dump half-way, then a key-on that starts again from the top
        out = run(base + [w(0, 0x11, 0x3F), w(0, 0x18, 0x9F), w(0, 0x10, 0x01), "frames 1000", w(0, 0x10, 0x81),
                          "frames 100", w(0, 0x10, 0x01), "frames 50"])
        f = frames(out)
        want = M.rhythm_play(pcm, 22050, 7987200, 144, 0x3F, 0x1F, 1000)
        self.assertEqual(f[:1000], [(v, 0) for v in want])
        self.assertEqual(set(f[1000:1100]), {(0, 0)})
        self.assertEqual(f[1100:], [(v, 0) for v in want[:50]])

    def test_six_drums_mixed(self):
        lines = ["init 2608 7987200", "mix 0 0 256 0", w(0, 0x11, 0x30)]
        pcms = []
        for i in range(6):
            lines.append("rhythmpcm %d %d %d %d" % (i, 16000 + 4000 * i, 500 + 300 * i, 3 + i))
            pcms.append(M.synthetic_drum(500 + 300 * i, 3 + i))
            lines.append(w(0, 0x18 + i, (0xC0, 0x80, 0x40, 0xC0, 0x80, 0x40)[i] | (31 - 3 * i)))
        lines += [w(0, 0x10, 0x3F), "frames 3000"]
        got = frames(run(lines))
        left = [0] * 3000
        right = [0] * 3000
        for i in range(6):
            v = M.rhythm_play(pcms[i], 16000 + 4000 * i, 7987200, 144, 0x30, 31 - 3 * i, 3000)
            for k in range(3000):
                if i in (0, 1, 3, 4):
                    left[k] += v[k]
                if i in (0, 2, 3, 5):
                    right[k] += v[k]
        self.assertEqual(got, list(zip(left, right)))

    def test_no_samples_no_sound(self):
        out = run(["init 2608 7987200", "mix 0 0 256 0", w(0, 0x11, 0x3F), w(0, 0x18, 0xDF), w(0, 0x10, 0x3F),
                   "frames 100"])
        self.assertEqual(set(frames(out)), {(0, 0)})


# ------------------------------------------------------------------ ADPCM-B

def adpcm_setup(mode, start, stop, mask=0x00):
    """The manual's preamble for a CPU memory access: reset, flag mask, flag reset, mode, 8-bit RAM, limit, addresses."""
    return [w(1, 0x00, 0x01), w(1, 0x10, mask), w(1, 0x10, 0x80), w(1, 0x00, mode), w(1, 0x01, 0x02),
            w(1, 0x0C, 0xFF), w(1, 0x0D, 0xFF), w(1, 0x02, start & 0xFF), w(1, 0x03, start >> 8),
            w(1, 0x04, stop & 0xFF), w(1, 0x05, stop >> 8)]


@unittest.skipUnless(x86ref.have_gcc(), "needs gcc")
class Adpcm(unittest.TestCase):
    def test_memory_write_and_read_back_with_brdy(self):
        data = bytes((i * 37 + 11) & 0xFF for i in range(64))
        lines = ["init 2608 7987200"] + adpcm_setup(0x60, 0x1000, 0x1001, mask=0x13)    # BRDY and EOS unmasked
        for b in data:
            lines += ["a 1 8", "d 1 %x" % b, "st 1", "adv 40", "st 1", w(1, 0x10, 0x80)]
        lines += [w(1, 0x00, 0x00), "ram 20000 40", "state"]
        out = run(lines)
        st = statuses(out)
        self.assertEqual(len(st), 128)
        self.assertTrue(all(s & M_BRDY == 0 for s in st[0::2]), "BRDY drops while a byte is written")
        self.assertTrue(all(s & M_BRDY for s in st[1::2]), "and comes back")
        self.assertFalse(any(s & M_EOS for s in st[:126]))
        self.assertTrue(st[126] & st[127] & M_EOS, "the 64th byte reached the stop address")
        ram = bytes(int(v, 16) for v in answers(out, "R")[0])
        self.assertEqual(ram, data, "start address 1000h in 32-byte units = byte 20000h")
        self.assertEqual(state(out)["mem_writes"], "64")
        self.assertEqual(state(out)["early"], "0")

        # read back: two dummy reads, then a byte per BRDY; the flag reset after each read does not hide BRDY
        lines = lines[:-3] + [w(1, 0x00, 0x00)] + adpcm_setup(0x20, 0x1000, 0x1001)
        lines += ["a 1 8", "rd 1", "adv 40", w(1, 0x10, 0x80), "a 1 8", "rd 1", "adv 40", w(1, 0x10, 0x80)]
        for _ in data:
            lines += ["a 1 8", "st 1", "rd 1", "adv 40", w(1, 0x10, 0x80)]
        out = run(lines)
        got = bytes(int(a[0], 16) for a in answers(out, "D")[2:])
        self.assertEqual(got, data)
        self.assertTrue(all(s & M_BRDY for s in statuses(out)[-64:]))

    def test_top_of_ram_and_mirroring(self):
        # the drivers' RAM check: start unit 1FFFh (the last 32 bytes of 256 KiB with 8-bit RAM), stop FFFFh (beyond the
        # RAM): no EOS at the end of the RAM, the chip's counter runs on and the next byte lands at the RAM's start
        data = bytes(range(0x40, 0x80))
        lines = ["init 2608 7987200"] + adpcm_setup(0x60, 0x1FFF, 0xFFFF)
        for b in data:
            lines += ["a 1 8", "d 1 %x" % b, "adv 40", "st 1", w(1, 0x10, 0x80)]
        lines += ["ram 3ffe0 20", "ram 0 20", "state"]
        out = run(lines)
        self.assertFalse(any(s & M_EOS for s in statuses(out)), "the stop address is far beyond the RAM")
        rams = answers(out, "R")
        self.assertEqual(bytes(int(v, 16) for v in rams[0]), data[:32])
        self.assertEqual(bytes(int(v, 16) for v in rams[1]), data[32:], "256 KiB repeat in the 2 MiB address space")
        self.assertEqual(state(out)["adpcm_addr"], "40020")

    def test_playback_matches_the_decoder(self):
        wave = [int(12000 * math.sin(2 * math.pi * k / 61) * (1 - k / 3000)) for k in range(2048)]
        data = M.adpcm_encode(wave)                      # 1024 bytes = 32 units of 32 bytes
        self.assertEqual(len(data), 1024)
        for dn, level, pan in ((0x8000, 0xFF, 0xC0), (0xC5A3, 0x80, 0x80), (0x2000, 0x40, 0x40)):
            with self.subTest(delta_n=dn):
                n = int(2048 * 65536 / dn) + 400
                lines = ["init 2608 7987200", "mix 0 0 0 256", "poke 0 " + " ".join("%x" % b for b in data),
                         w(1, 0x01, pan | 0x02), w(1, 0x02, 0), w(1, 0x03, 0), w(1, 0x04, 31), w(1, 0x05, 0),
                         w(1, 0x0C, 0xFF), w(1, 0x0D, 0xFF), w(1, 0x09, dn & 0xFF), w(1, 0x0A, dn >> 8),
                         w(1, 0x0B, level), w(1, 0x00, 0xA0), "st 1", "frames %d" % n, "st 1"]
                out = run(lines)
                want = M.adpcm_play(data, dn, level, n, left=bool(pan & 0x80), right=bool(pan & 0x40))
                self.assertEqual(frames(out), want)
                st = statuses(out)
                self.assertTrue(st[0] & M_PCMBSY, "playing")
                self.assertFalse(st[1] & M_PCMBSY, "finished")
                self.assertTrue(st[1] & M_EOS)

    def test_decoder_follows_the_waveform(self):
        wave = [int(9000 * math.sin(2 * math.pi * k / 40)) for k in range(4096)]
        data = M.adpcm_encode(wave)
        x, step, dec = 0, 127, []
        for b in data:
            for nib in (b >> 4, b & 15):
                x, step = M.adpcm_step(x, step, nib)
                dec.append(x)
        err = max(abs(a - b) for a, b in zip(dec[200:], wave[200:]))
        self.assertLess(err, 1500, "the codec tracks a sine within a few per cent")

    def test_repeat_and_spoff_and_irq(self):
        data = M.adpcm_encode([int(8000 * math.sin(k / 5)) for k in range(64)])        # 32 bytes = one unit
        lines = ["init 2608 7987200", "mix 0 0 0 256", "poke 0 " + " ".join("%x" % b for b in data),
                 w(0, 0x29, 0x84),                                                      # IRQ on EOS only
                 w(1, 0x01, 0xC2), w(1, 0x04, 0), w(1, 0x05, 0), w(1, 0x0C, 0xFF), w(1, 0x0D, 0xFF),
                 w(1, 0x09, 0x00), w(1, 0x0A, 0x80), w(1, 0x0B, 0xFF), w(1, 0x00, 0xB0),  # START + MEMDATA + REPEAT
                 "frames 127", "irq", "frames 2", "irq", "st 1", w(1, 0x10, 0x80), "irq", "frames 300", "st 1",
                 w(1, 0x00, 0xB8), "frames 50"]                                          # SPOFF: silent
        out = run(lines)
        f = frames(out)
        want = M.adpcm_play(data, 0x8000, 0xFF, 429, repeat=True)
        self.assertEqual(f[:429], want)
        self.assertEqual([a[0] for a in answers(out, "I")], ["0", "1", "0"])
        st = statuses(out)
        self.assertTrue(st[0] & M_EOS and st[0] & M_PCMBSY)
        self.assertTrue(st[1] & M_PCMBSY, "still playing: it repeats")
        self.assertEqual(set(f[429:]), {(0, 0)})


M_EOS, M_BRDY, M_PCMBSY = 0x04, 0x08, 0x20


# ------------------------------------------------------------------ resampler

def goertzel(xs, freq, rate):
    k = 2 * math.cos(2 * math.pi * freq / rate)
    s1 = s2 = 0.0
    for x in xs:
        s1, s2 = x + k * s1 - s2, s1
    return math.sqrt(max(0.0, s1 * s1 + s2 * s2 - k * s1 * s2)) * 2 / len(xs)


def tone_program(fnum, block, mul):
    return [w(0, 0xB0, 0x07), w(0, 0x50, 0x1F), w(0, 0x30, mul), w(0, 0x40, 0)] + \
           [w(0, r, 0x7F) for r in (0x44, 0x48, 0x4C)] + [w(0, 0xA4, block << 3 | fnum >> 8), w(0, 0xA0, fnum & 0xFF),
                                                           w(0, 0x28, 0x10)]


@unittest.skipUnless(x86ref.have_gcc(), "needs gcc")
class Resampler(unittest.TestCase):
    def setUp(self):
        OUT.mkdir(parents=True, exist_ok=True)

    def test_tone_keeps_pitch_and_level(self):
        for rate in (48000, 44100):
            with self.subTest(rate=rate):
                # S1 alone (algorithm 7): F-number 1040 block 4 = 440 Hz
                out = run(["init 2608 7987200", "mix 256 0 0 0"] + tone_program(1040, 4, 1) +
                          ["rs 7987200 144 %d" % rate, "rsrender %d %s" % (rate // 2, OUT / ("tone%d.raw" % rate))])
                raw = (OUT / ("tone%d.raw" % rate)).read_bytes()
                left = [v for v, in struct.iter_unpack("<h", raw)][0::2][rate // 10:]
                hz = M.phase_inc(1040, 4, 0, 1) * (7987200 / 144) / 2 ** 20
                amp = goertzel(left, hz, rate)
                self.assertAlmostEqual(amp, 8168, delta=8168 * 0.01)
                self.assertAlmostEqual(zero_crossings(left) / (len(left) / rate), hz, delta=5)

    def test_tones_above_the_new_nyquist_are_removed(self):
        # MUL 4, F-number 1994, block 7: 27.0 kHz, legal at 55.5 kHz, an alias at 48 kHz
        out = run(["init 2608 7987200", "mix 256 0 0 0", "dc 0"] + tone_program(1994, 7, 4) +
                  ["rs 7987200 144 48000", "rsrender 24000 %s" % (OUT / "alias.raw")])
        raw = (OUT / "alias.raw").read_bytes()
        left = [v for v, in struct.iter_unpack("<h", raw)][0::2][4800:]
        peak = max(abs(v) for v in left)
        self.assertLess(peak, 8168 * 0.01, "at least 40 dB down")


# ------------------------------------------------------------------ register scripts rendered to WAV

def read_wav(path):
    import wave
    with wave.open(str(path), "rb") as f:
        rate, ch, n = f.getframerate(), f.getnchannels(), f.getnframes()
        data = f.readframes(n)
    vals = [v for v, in struct.iter_unpack("<h", data)]
    return rate, list(zip(vals[0::ch], vals[1::ch])) if ch == 2 else [(v, v) for v in vals]


def write_mono_wav(path, samples, rate):
    import wave
    with wave.open(str(path), "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(rate)
        f.writeframes(b"".join(struct.pack("<h", s) for s in samples))


NOTES = {"C4": 261.63, "D4": 293.66, "E4": 329.63, "F4": 349.23, "G4": 392.0, "A4": 440.0, "B4": 493.88, "C5": 523.25}


@unittest.skipUnless(x86ref.have_gcc(), "needs gcc")
class Scripts(unittest.TestCase):
    """The three listening scripts in tests/host/opna, rendered to build/opna/*.wav at 48 kHz."""

    def render(self, name, *extra):
        OUT.mkdir(parents=True, exist_ok=True)
        wav = OUT / (name + ".wav")
        r = subprocess.run([str(tool()), "--script", str(SCRIPTS / (name + ".txt")), "--out", str(wav), "--rate", "48000",
                            *extra], capture_output=True, text=True, env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
        self.assertEqual(r.returncode, 0, r.stderr)
        rate, f = read_wav(wav)
        self.assertEqual(rate, 48000)
        peak = max(max(abs(l), abs(r)) for l, r in f)
        self.assertLess(peak, 32767, "no clipping")
        self.assertGreater(peak, 2000)
        return f

    def window(self, f, start_ms, ms, side=0):
        a = int(start_ms * 48)
        return [s[side] for s in f[a:a + int(ms * 48)]]

    def test_fm_scale(self):
        f = self.render("fm_scale")
        self.assertAlmostEqual(len(f) / 48000, 8.3, delta=0.01)
        order = ["C4", "D4", "E4", "F4", "G4", "A4", "B4", "C5", "B4", "A4", "G4", "F4", "E4", "D4", "C4"]
        heard = []
        for k in range(15):
            x = self.window(f, 50 + 400 * k + 40, 250)
            heard.append(max(NOTES, key=lambda n: goertzel(x, NOTES[n] * 1.0004, 48000)))
        self.assertEqual(heard, order)
        # the chord: C4 in the middle, E4 on the left, G4 on the right
        left, right = self.window(f, 6250, 800, 0), self.window(f, 6250, 800, 1)
        mag = lambda x, n: goertzel(x, NOTES[n], 48000)
        for n, side in (("C4", left), ("C4", right), ("E4", left), ("G4", right)):
            self.assertGreater(mag(side, n), 10 * mag(side, "D4"), n)
        self.assertLess(mag(right, "E4"), mag(left, "E4") / 10)
        self.assertLess(mag(left, "G4"), mag(right, "G4") / 10)

    def test_ssg_chords(self):
        f = self.render("ssg_chords")
        self.assertAlmostEqual(len(f) / 48000, 4.3, delta=0.01)
        ssg_hz = lambda tp: 3993600 / (64 * tp)
        chords = [(0, (0xEF, 0xBD, 0x9F), 0xB3), (700, (0xB3, 0x8E, 0x77), 0x9F), (1400, (0x9F, 0x7E, 0x6A), 0xEF)]
        for start, tones, absent in chords:
            x = self.window(f, start + 100, 500)
            quiet = goertzel(x, ssg_hz(absent), 48000)
            for tp in tones:
                self.assertGreater(goertzel(x, ssg_hz(tp), 48000), 5 * quiet, (start, hex(tp)))
        # the envelope's fade: the last second gets quieter and quieter
        level = lambda ms: max(abs(s) for s in self.window(f, ms, 100))
        self.assertGreater(level(2900), 2 * level(3600))

    def test_rhythm_with_and_without_drums(self):
        bare = self.render("rhythm")
        self.assertAlmostEqual(len(bare) / 48000, 5.6, delta=0.01)
        bass = self.window(bare, 300, 600)
        self.assertGreater(goertzel(bass, 65.4, 48000), 500, "the FM bass on C2")
        # with drums: synthetic ones made here (noise bursts and pitched decays), never the chip's ROM sounds
        import random
        rnd = random.Random(2608)
        drums = Path(OUT / "synthdrums")
        drums.mkdir(parents=True, exist_ok=True)
        for name, n, hz in (("BD", 3000, 60), ("SD", 3000, 0), ("TOP", 12000, 0), ("HH", 1500, 0), ("TOM", 4000, 150),
                            ("RIM", 800, 900)):
            s = []
            for i in range(n):
                env = 1 - i / n
                v = math.sin(2 * math.pi * hz * i / 44100) if hz else rnd.uniform(-1, 1)
                s.append(int(20000 * env * env * v))
            write_mono_wav(drums / ("2608_%s.WAV" % name), s, 44100)
        full = self.render("rhythm", "--rhythm", str(drums))
        left = [a[0] - b[0] for a, b in zip(full, bare)]           # what the drums added, per side
        right = [a[1] - b[1] for a, b in zip(full, bare)]
        peak = lambda d, ms: max(abs(v) for v in d[int(ms * 48):int(ms * 48) + 480])
        self.assertGreater(peak(left, 20), 1000, "bass drum and cymbal on the first step")
        self.assertGreater(peak(left, 520), 1000, "the snare on the third")
        # the second step is the hi-hat alone, panned right: heard on the right; on the left only the tails of the
        # first step's drums (the cymbal is long)
        self.assertGreater(peak(right, 270), 1000)
        self.assertGreater(peak(right, 270), 3 * peak(left, 270) if peak(left, 270) else 0)


def owner_rhythm_dir():
    """The owner's private copies of the six rhythm sounds (assets/local, git-ignored), if this checkout or one of its
    parents (a worktree sits inside the main checkout) has them. Only ever read, never copied into the tree."""
    for root in (x86ref.REPO, *x86ref.REPO.parents):
        local = root / "assets" / "local"
        if not local.is_dir():
            continue
        for cand in [local, *local.glob("*"), *local.glob("*/DOSBox-X")]:
            if (cand / "2608_BD.WAV").is_file():
                return cand
    return None


@unittest.skipUnless(x86ref.have_gcc(), "needs gcc")
@unittest.skipUnless(owner_rhythm_dir(), "the owner's rhythm samples are not on this machine")
class OwnerRhythmSamples(unittest.TestCase):
    def test_render_with_the_rom_sounds(self):
        """Leaves build/opna/rhythm_rom.wav for listening (git-ignored, like everything derived from assets/local)."""
        OUT.mkdir(parents=True, exist_ok=True)
        wav = OUT / "rhythm_rom.wav"
        r = subprocess.run([str(tool()), "--script", str(SCRIPTS / "rhythm.txt"), "--out", str(wav), "--rate", "48000",
                            "--rhythm", str(owner_rhythm_dir())], capture_output=True, text=True,
                           env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stderr, "", "all six samples read")
        rate, f = read_wav(wav)
        peak = max(max(abs(a), abs(b)) for a, b in f)
        self.assertLess(peak, 32767)
        self.assertGreater(peak, 4000)


# ------------------------------------------------------------------ a driver-like program on the emulated PC-98

@unittest.skipUnless(x86ref.have_gcc(), "needs gcc")
class Machine(unittest.TestCase):
    """tests/host/opna/fmboard.asm on the x86 core and the PC-98 skeleton, the board on the machine's host port hooks
    and its interrupt line on the 8259s: detection, the interrupt jumper, timer B interrupts with the flag reset in
    the handler, a note keyed on meanwhile."""

    def binary(self):
        nasm = x86ref.find_nasm()
        if not nasm:
            self.skipTest("needs nasm")
        OUT.mkdir(parents=True, exist_ok=True)
        out = OUT / "fmboard.bin"
        subprocess.run([nasm, "-f", "bin", "-o", str(out), str(SCRIPTS / "fmboard.asm")], check=True)
        return out

    def test_detect_and_take_timer_interrupts(self):
        prog = self.binary()
        # board, jumper IRQ, ID register, second bank status with OPNA mode off/on, vector
        cases = [("86", 12, "01", "FF", "00", "14"), ("86", 3, "01", "FF", "00", "0B"),
                 ("26k", 13, "00", "00", "00", "15"), ("26k", 10, "00", "00", "00", "12")]
        for board, irq, chipid, off, on, vec in cases:
            with self.subTest(board=board, irq=irq):
                out = run(["pc98 v30 10 %s %d" % (board, irq), "pc98boot %s" % prog, "pc98run 20000000", "pc98text"])
                self.assertEqual(answers(out, "B")[0], ["0"])
                used, dead, edges, nframes, crc, peak = answers(out, "M")[0]
                scr = [l[1:].rstrip() for l in out if l.startswith("|")]
                self.assertEqual(scr[0], "FM board test, no ROM inside")
                self.assertEqual(scr[1], "register FFh: " + chipid)
                self.assertEqual(scr[2], "second bank status, OPNA mode off: %s, on: %s" % (off, on))
                self.assertEqual(scr[3], "interrupt: IRQ %02X, vector %s" % (irq, vec))
                self.assertEqual(scr[4], "timer B interrupts: 08")
                self.assertEqual(scr[5], "Halting.")
                self.assertEqual(dead, "1")
                self.assertEqual(edges, "8", "one rising edge per interrupt")
                # eight periods of 56 x 16 samples at 55466.67 Hz on a 10 MHz CPU, plus the set-up
                period = 56 * 16 * 144 / 7987200 * 10e6
                self.assertGreater(int(used), 8 * period)
                self.assertLess(int(used), 8 * period + 20000)
                self.assertEqual(int(peak), 8168, "the note was heard at the chip's full level")
                self.assertAlmostEqual(int(nframes), int(used) / 10e6 * 55466.67, delta=2)


@unittest.skipUnless(x86ref.have_gcc(), "needs gcc")
class Speed(unittest.TestCase):
    def test_faster_than_real_time(self):
        """Six FM channels with the LFO and three SSG tones, optimised build without sanitizers. Prints the factor."""
        exe = x86ref.build("opnatool-fast", [HERE / "opnatool.c"], sanitize=False)
        r = subprocess.run([str(exe)], input="bench 60\n", capture_output=True, text=True)
        audio, cpu = (float(v) for v in r.stdout.split()[1:3])
        print("\nopna: %.0f s of audio in %.2f s of CPU (%.0fx real time)" % (audio, cpu, audio / max(cpu, 1e-6)),
              file=sys.stderr)
        self.assertLess(cpu, audio / 4, "well under real time even on a slow machine")


if __name__ == "__main__":
    unittest.main()
