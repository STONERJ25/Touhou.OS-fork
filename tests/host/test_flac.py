"""Host tests of the FLAC decoder (user/lib/audio/src/flac.c), without QEMU.

  python tools/hosttest.py discover -s tests/host -p "test_flac.py"          (on Windows this runs in WSL's Ubuntu: it needs gcc)
  python tools/hosttest.py discover -s tests/host -p "test_flac.py" -k Damage (one group)

The decoder is built for the host by tests/host/flactool.c with AddressSanitizer and UndefinedBehaviorSanitizer (build/host-flac/flactool). The streams
come from tests/host/flacenc.py, an encoder written from RFC 9639 that can be told to take every path of the format; the PCM that goes into the encoder is
the oracle (the decoder must give it back bit for bit) and so is the MD5 the encoder writes into STREAMINFO (hashlib, not the decoder's own MD5: flac_verify
must agree with it). Where the two halves share a reading of the specification a mistake would not show; what is independent: the CRC check values from the
CRC catalogue, MD5 against hashlib, every number the PCM predicts.

Damage is tested as a property: whatever is not damaged must come back exactly, the damaged frame must be reported once and replaced by silence of the same
length (so that the position stays right), and nothing may crash (the sanitizers abort the tool). A mutation fuzz (thousands of random changes of valid streams)
ends in an error or a clean decode, never a fault or a hang.
"""
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import unittest
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
import flacenc as fe  # noqa: E402

TOOL = ROOT / "build" / "host-flac" / "flactool"
WORK = ROOT / "build" / "flac-tests"
SOURCES = [ROOT / "tests/host/flactool.c", ROOT / "user/lib/audio/src/flac.c"]
HEADERS = [ROOT / "user/lib/audio/include/flac.h"]
HAVE_GCC = shutil.which("gcc") is not None
ENV = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=0:exitcode=77", UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1")

E_SYNC, E_HEADER, E_CRC, E_FRAME, E_TRUNCATED, E_BADMETA, E_NOTFLAC, E_MD5, E_LENGTH = -7, -8, -9, -10, -11, -4, -3, -12, -13


def build_tool():
    """Compiles the decoder for the host with the sanitizers (again when a source is newer than the binary)."""
    newest = max(p.stat().st_mtime for p in SOURCES + HEADERS)
    if TOOL.exists() and TOOL.stat().st_mtime >= newest:
        return
    TOOL.parent.mkdir(parents=True, exist_ok=True)
    cmd = ["gcc", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
           "-I", str(ROOT / "user/lib/audio/include"), *map(str, SOURCES), "-o", str(TOOL)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("cannot build tests/host/flactool.c:\n" + r.stdout + r.stderr)


def tool(*args, timeout=300):
    """Runs flactool. Returns (status, stdout text). A sanitizer report, a signal or a hang is a failure of the test."""
    r = subprocess.run([str(TOOL), *map(str, args)], capture_output=True, timeout=timeout, env=ENV)
    err = r.stderr.decode("utf-8", "replace")
    if r.returncode < 0 or r.returncode >= 77 or "runtime error" in err or "AddressSanitizer" in err or "LeakSanitizer" in err:
        raise AssertionError(f"flactool {' '.join(map(str, args))}: status {r.returncode}\n{err[-3000:]}")
    return r.returncode, r.stdout.decode("utf-8", "replace")


def kv_lines(text):
    """key=value, one per line (the output of `info`: values may hold spaces)."""
    d = {}
    for line in text.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            d[k] = v
    return d


def kv_words(text):
    """key=value words (the summary lines of the other commands; a trailing message=... is taken whole)."""
    msg = ""
    if " message=" in text:
        text, msg = text.split(" message=", 1)
    d = {}
    for word in text.split():
        if "=" in word:
            k, v = word.split("=", 1)
            d[k] = v
    if msg:
        d["message"] = msg.strip()
    return d


class Decoded:
    def __init__(self, status, text, pcm, channels):
        self.status = status
        self.text = text
        self.events = []
        for line in text.splitlines():
            m = re.match(r"error at=(\d+) code=(-?\d+) message=(.*)", line)
            if m:
                self.events.append({"at": m.group(1), "code": m.group(2), "message": m.group(3)})
        self.summary = kv_words([line for line in text.splitlines() if line.startswith("frames=")][0]) if "frames=" in text else {}
        self.pcm = pcm
        self.channels = channels

    @property
    def codes(self):
        return [int(e["code"]) for e in self.events]


def write(name, data):
    WORK.mkdir(parents=True, exist_ok=True)
    p = WORK / name
    p.write_bytes(data)
    return p


def decode(data, name="s", channels=None):
    """Decodes a stream with flactool. channels: for splitting the output (read from info when not given)."""
    p = write(name + ".flac", data)
    out = WORK / (name + ".raw")
    if out.exists():
        out.unlink()
    status, text = tool("decode", p, out)
    pcm = np.frombuffer(out.read_bytes(), dtype="<i4") if out.exists() else np.zeros(0, dtype="<i4")
    if channels is None and "open error" not in text:
        channels = int(kv_lines(tool("info", p)[1])["channels"])
    return Decoded(status, text, pcm, channels or 1)


def expect_pcm(s):
    return np.array(s.pcm, dtype="<i4").T.reshape(-1) if s.pcm and len(s.pcm[0]) else np.zeros(0, dtype="<i4")


def info(data, name="i"):
    p = write(name + ".flac", data)
    status, text = tool("info", p)
    return kv_lines(text)


def verify(data, name="v"):
    p = write(name + ".flac", data)
    status, text = tool("verify", p)
    return status, kv_words(text)


@unittest.skipUnless(HAVE_GCC, "needs gcc (run it through tools/hosttest.py)")
class FlacCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build_tool()

    def check(self, s, name="s", verify_too=True, msg=""):
        """The stream decodes without any error to exactly the PCM that made it, and flac_verify agrees (the MD5 is hashlib's)."""
        d = decode(s.data, name)
        self.assertEqual(d.status, 0, d.text)
        self.assertEqual(d.events, [], f"{msg} {d.text}")
        want = expect_pcm(s)
        self.assertEqual(len(d.pcm), len(want), f"{msg}: decoded {len(d.pcm)} values, expected {len(want)}")
        if not np.array_equal(d.pcm, want):
            bad = np.nonzero(d.pcm != want)[0]
            self.fail(f"{msg}: {len(bad)} samples differ, the first at interleaved index {bad[0]}: got {d.pcm[bad[0]]}, expected {want[bad[0]]}")
        if verify_too:
            status, v = verify(s.data, name)
            self.assertEqual(status, 0, f"{msg} verify: {v}")
            self.assertEqual(v["md5_ok"], "1" if s.md5 != bytes(16) else "0")
        return d


# ---------------------------------------------------------------------------------------------------------- primitives

class Primitives(FlacCase):
    def test_crc_check_values(self):
        """The CRC-8 and CRC-16 of the encoder are the standard ones (the check values of the CRC catalogue); the decoder's frames pass through them."""
        self.assertEqual(fe.crc8(b"123456789"), 0xF4)                 # CRC-8/SMBUS
        self.assertEqual(fe.crc16(b"123456789"), 0xFEE8)              # CRC-16/UMTS
        self.assertEqual(fe.crc8(b""), 0)
        self.assertEqual(fe.crc16(b""), 0)

    def test_md5_against_hashlib(self):
        import hashlib
        rnd = random.Random(7)
        for n in (0, 1, 3, 55, 56, 57, 63, 64, 65, 119, 120, 121, 127, 128, 129, 1000, 4096, 100003):
            data = bytes(rnd.getrandbits(8) for _ in range(n))
            p = write("md5.bin", data)
            status, text = tool("md5", p)
            self.assertEqual(text.strip(), hashlib.md5(data).hexdigest(), f"{n} bytes")

    def test_coded_numbers(self):
        """The UTF-8 style frame/sample numbers of every length (1 to 7 bytes) at and around the boundaries, in a stream that starts late (a cut stream)."""
        for n in (0, 1, 0x7F, 0x80, 0x7FF, 0x800, 0xFFFF, 0x10000, 0x1FFFFF, 0x200000, 0x3FFFFFF, 0x4000000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, (1 << 36) - 5000):
            pcm = [fe.tone(3000, 44100, 440, 0.5, 16)]
            s = fe.encode(pcm, 44100, 16, blocksize=1000, variable=True, first_number=n)
            self.assertEqual(len(fe.coded_number(n)), 1 if n < 0x80 else 2 if n < 0x800 else 3 if n < 0x10000 else 4 if n < 0x200000 else 5 if n < 0x4000000 else
                             6 if n < 0x80000000 else 7)
            self.check(s, "num", msg=f"first sample number {n:#x}")
        for n in (0, 1, 0x7F, 0x80, 0x7FF, 0x800, 0xFFFF, 0x10000, 0x7FFFFFF0):                    # frame numbers of a fixed block size stream: 31 bits at most
            s = fe.encode([fe.tone(3000, 44100, 440, 0.5, 16)], 44100, 16, blocksize=1000, first_number=n)
            self.check(s, "num2", msg=f"first frame number {n:#x}")


# ----------------------------------------------------------------------------------------- the format: header, channels

class Format(FlacCase):
    def test_bit_depths_and_channel_assignments(self):
        """4 to 32 bits (the usual ones and odd ones), all four channel assignments, music and noise."""
        for bps in (4, 5, 8, 9, 12, 13, 16, 17, 20, 23, 24, 27, 31, 32):
            n = 2200
            for mode in ("indep", "left_side", "right_side", "mid_side", "auto"):
                s = fe.encode([fe.music(n, 44100, bps, 1), fe.music(n, 44100, bps, 2)], 44100, bps, blocksize=1024, stereo=mode)
                self.check(s, "bps", msg=f"{bps} bits, music, {mode}")
        for bps in (4, 8, 16, 24, 32):
            n = 1500
            for mode in ("indep", "left_side", "mid_side"):
                s = fe.encode([fe.noise(n, bps, 3), fe.noise(n, bps, 4)], 44100, bps, blocksize=777, stereo=mode)
                self.check(s, "bps2", msg=f"{bps} bits, noise, {mode}")
                s = fe.encode([fe.tone(n, 44100, 300, 0.8, bps), fe.tone(n, 44100, 300, 0.78, bps)], 44100, bps, blocksize=777, stereo=mode)
                self.check(s, "bps3", msg=f"{bps} bits, sine, {mode}")

    def test_16_bit_output(self):
        """flac_read with FLAC_READ_S16 scales to 16 bits: shifted down (truncating) from more, up from fewer."""
        for bps in (4, 8, 12, 16, 20, 24, 32):
            s = fe.encode([fe.music(1500, 44100, bps, 1), fe.music(1500, 44100, bps, 2)], 44100, bps, blocksize=700, stereo="auto")
            p = write("s16.flac", s.data)
            out = WORK / "s16.raw"
            status, text = tool("decode", "--s16", p, out)
            self.assertEqual(status, 0, text)
            got = np.frombuffer(out.read_bytes(), dtype="<i2").astype(np.int64)
            want = expect_pcm(s).astype(np.int64)
            want = (want >> (bps - 16)) if bps >= 16 else (want << (16 - bps))
            self.assertTrue(np.array_equal(got, want), f"{bps} bits to 16")

    def test_frame_bit_depth_from_streaminfo(self):
        """A frame header may leave the bit depth to STREAMINFO (code 0) even for 16 bits."""
        s = fe.encode([fe.music(3000, 44100, 16, 1)], 44100, 16, blocksize=1000, frame_bps="streaminfo")
        self.check(s)

    def test_channel_counts(self):
        for ch in range(1, 9):
            for bps in (8, 16, 24):
                pcm = [fe.music(2500, 48000, bps, 10 + c, voices=2) for c in range(ch)]
                s = fe.encode(pcm, 48000, bps, blocksize=900)
                d = self.check(s, "chan", msg=f"{ch} channels, {bps} bits")
                self.assertEqual(d.channels, ch)

    def test_sample_rates(self):
        """Every rate code of the table, the three extension forms, and 'from STREAMINFO'."""
        for rate in (8000, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000, 176400, 192000):       # the table
            self.check(fe.encode([fe.music(2000, rate, 16, 1)], rate, 16, blocksize=700), "rate", msg=f"{rate}")
        for rate, form in ((11025, "explicit"), (12000, "explicit"), (100000, "explicit"), (255000, "explicit"), (352800, "explicit"), (4000, "hz"), (65535, "hz"),
                           (123450, "tens"), (655350, "tens"), (1, "hz"), (3000, "khz"), (88000, "khz")):
            self.check(fe.encode([fe.music(2000, 44100, 16, 1)], rate, 16, blocksize=700, frame_rate=form), "rate2", msg=f"{rate} as {form}")
        for rate in (44100, 11025, 700001):
            self.check(fe.encode([fe.music(2000, 44100, 16, 1)], rate, 16, blocksize=700, frame_rate="streaminfo"), "rate3", msg=f"{rate} from STREAMINFO")

    def test_block_sizes(self):
        """The table sizes, the 8-bit and 16-bit forms (also for sizes the table has), the smallest and the biggest."""
        sizes = [16, 17, 100, 191, 192, 255, 256, 257, 576, 1000, 1152, 2304, 4096, 4608, 8192, 16384, 32768, 65535]
        for bs in sizes:
            n = bs * 2 + 10 if bs <= 4608 else bs + 3
            self.check(fe.encode([fe.music(n, 44100, 16, bs)], 44100, 16, blocksize=bs), "bs", msg=f"block size {bs}")
        for bs in (16, 100, 192, 256):
            self.check(fe.encode([fe.music(bs * 3 + 1, 44100, 16, 1)], 44100, 16, blocksize=bs, blocksize_code="ext8"), "bs8", msg=f"{bs} as 8-bit")
        for bs in (16, 100, 4096, 20000):
            self.check(fe.encode([fe.music(bs * 2 + 1, 44100, 16, 1)], 44100, 16, blocksize=bs, blocksize_code="ext16"), "bs16", msg=f"{bs} as 16-bit")
        for bs in (1, 2, 5, 15):                                                                          # below what a stream may have except at its end: still decodable
            self.check(fe.encode([fe.music(bs * 4 + 1, 44100, 16, 1)], 44100, 16, blocksize=bs), "bssmall", msg=f"block size {bs}")

    def test_variable_block_size(self):
        rnd = random.Random(3)
        sizes, total = [], 0
        while total < 30000:
            sizes.append(rnd.choice([16, 100, 192, 576, 1000, 1152, 2048, 4096, 4608, 7000, 200, 333]))
            total += sizes[-1]
        pcm = [fe.music(total, 44100, 16, 5), fe.music(total, 44100, 16, 6)]
        s = fe.encode(pcm, 44100, 16, blocksize=sizes, stereo="auto")
        self.check(s, "var")
        i = info(s.data)
        self.assertEqual(int(i["min_block"]), min(sizes))
        self.assertEqual(int(i["max_block"]), max(sizes))

    def test_streaminfo_fields(self):
        s = fe.encode([fe.music(10000, 44100, 24, 1)] * 2, 44100, 24, blocksize=4096, stereo="auto")
        i = info(s.data)
        self.assertEqual((i["rate"], i["channels"], i["bits"], i["total_samples"]), ("44100", "2", "24", "10000"))
        self.assertEqual(i["md5"], s.md5.hex())
        self.assertEqual((i["min_block"], i["max_block"]), ("4096", "4096"))
        self.assertEqual(int(i["first_frame_offset"]), s.first_frame_offset)
        self.assertEqual(i["channel_mask"], "0x3")
        for ch, mask in ((1, "0x4"), (3, "0x7"), (4, "0x33"), (5, "0x37"), (6, "0x3f"), (7, "0x70f"), (8, "0x63f")):
            s = fe.encode([fe.tone(100, 44100, 440, 0.3, 16)] * ch, 44100, 16)
            self.assertEqual(info(s.data)["channel_mask"], mask, f"{ch} channels")


# -------------------------------------------------------------------------------------------------------------- subframes

class Subframes(FlacCase):
    def test_fixed_orders(self):
        pcm = [fe.music(5000, 44100, 16, 1)]
        for order in range(5):
            s = fe.encode(pcm, 44100, 16, blocksize=1000, subframe={"kind": "fixed", "order": order})
            self.check(s, "fixed", msg=f"fixed order {order}")

    def test_lpc_orders_precisions_and_shifts(self):
        pcm = [fe.music(6000, 44100, 16, 3, voices=4)]
        for order in range(1, 33):
            s = fe.encode(pcm, 44100, 16, blocksize=2000, subframe={"kind": "lpc", "order": order})
            self.check(s, "lpc", msg=f"lpc order {order}")
        for precision in range(1, 16):
            for shift in sorted({0, 1, 5, 9, 12, 15}):
                for order in (1, 3, 12):
                    s = fe.encode(pcm, 44100, 16, blocksize=2000, subframe={"kind": "lpc", "order": order, "precision": precision, "shift": shift})
                    self.check(s, "lpcps", verify_too=False, msg=f"lpc order {order}, precision {precision}, shift {shift}")
        s = fe.encode(pcm, 44100, 16, blocksize=2000, subframe={"kind": "lpc", "order": 32, "precision": 15, "shift": 15})
        self.check(s, "lpcmax")

    def test_lpc_coefficients_at_the_limits(self):
        """Explicit coefficients: the largest and the smallest values of every precision, in alternation, so that the sums are as big as they get."""
        rnd = random.Random(11)
        for precision in (2, 8, 15):
            lim = 1 << (precision - 1)
            for coefs in ([lim - 1] * 4, [-lim] * 4, [lim - 1, -lim] * 8, [-lim] + [0] * 31):
                x = [rnd.randint(-2000, 2000) for _ in range(900)]
                s = fe.encode([x], 44100, 16, blocksize=900, subframe={"kind": "lpc", "order": len(coefs), "precision": precision, "coefs": coefs, "shift": 14})
                self.check(s, "lpclim", msg=f"precision {precision}, coefficients {coefs[:3]}...")

    def test_constant_and_verbatim(self):
        s = fe.encode([[1234] * 3000, [-77] * 3000], 44100, 16, blocksize=1000, subframe={"kind": "constant"})
        self.check(s, "const")
        s = fe.encode([fe.noise(2000, 16, 1), fe.noise(2000, 16, 2)], 44100, 16, blocksize=500, subframe={"kind": "verbatim"})
        self.check(s, "verb")
        for bps in (4, 8, 24, 32):
            s = fe.encode([[(1 << (bps - 1)) - 1] * 700, [-(1 << (bps - 1))] * 700], 48000, bps, blocksize=350, subframe={"kind": "constant"})
            self.check(s, "constx", msg=f"{bps} bits")

    def test_wasted_bits(self):
        for w in range(1, 15):
            x = [v << w for v in fe.music(3000, 44100, 16 - w, 1)]
            for mode in ("auto", "forced"):
                spec = {} if mode == "auto" else {"wasted": w, "kind": "fixed", "order": 2}
                self.check(fe.encode([x, [v << w for v in fe.music(3000, 44100, 16 - w, 2)]], 44100, 16, blocksize=1000, stereo="auto", subframe=spec), "waste", msg=f"{w} wasted bits, {mode}")
        for bps in (8, 24, 32):                                                                               # the side channel keeps the bit
            x = [v << 3 for v in fe.music(2000, 44100, bps - 3, 1)]
            y = [v << 3 for v in fe.music(2000, 44100, bps - 3, 2)]
            self.check(fe.encode([x, y], 44100, bps, blocksize=1000, stereo="mid_side"), "waste2", msg=f"{bps} bits")
        s = fe.encode([[0] * 2000], 44100, 16, blocksize=1000, subframe={"kind": "verbatim", "wasted": 5})  # all zero with wasted bits: valid
        self.check(s, "waste0")

    def test_rice_partitions_and_methods(self):
        pcm = [fe.music(8192, 44100, 16, 1, voices=3)]
        for method in (0, 1):
            for porder in range(0, 13):
                for kind in ({"kind": "fixed", "order": 2}, {"kind": "lpc", "order": 12}):
                    if (8192 >> porder) < kind["order"]:
                        continue                                                              # the first partition would hold fewer samples than the predictor has warm-up samples
                    spec = dict(kind, method=method, porder=porder)
                    self.check(fe.encode(pcm, 44100, 16, blocksize=8192, subframe=spec), "rice", verify_too=False,
                               msg=f"method {method}, partition order {porder}, {kind}")
        for porder in (13, 14, 15):                                                      # every order the 4 bits can say, on a block that divides that far
            n = 1 << porder
            for method in (0, 1):
                s = fe.encode([fe.music(n, 44100, 16, 1)], 44100, 16, blocksize=n, subframe={"kind": "fixed", "order": 1, "method": method, "porder": porder})
                self.check(s, "rice2", verify_too=False, msg=f"method {method}, partition order {porder}")

    def test_rice_parameters_and_long_codes(self):
        """Forced Rice parameters from 0 (long unary runs for big residuals) to the largest of each method."""
        x = fe.music(2000, 44100, 16, 4)
        for method, ks in ((0, range(0, 15)), (1, list(range(0, 31)))):
            for k in ks:
                s = fe.encode([x], 44100, 16, blocksize=2000, subframe={"kind": "fixed", "order": 1, "method": method, "porder": 0, "k": k})
                self.check(s, "ricek", verify_too=False, msg=f"method {method}, parameter {k}")
        noisy = [fe.noise(300, 16, 5)]                                                  # residuals of 2**16: parameter 0 makes runs of tens of thousands of zero bits
        self.check(fe.encode(noisy, 44100, 16, blocksize=300, subframe={"kind": "fixed", "order": 0, "method": 0, "porder": 0, "k": 0}), "ricelong")
        wide = [[(-1) ** i * ((1 << 30) - 3 * i) for i in range(200)]]                  # 31-bit values: a quotient near 2**32 with parameter 0 is out of range for the decoder
        self.check(fe.encode(wide, 48000, 32, blocksize=200, subframe={"kind": "fixed", "order": 0, "method": 1, "porder": 0, "k": 20}), "ricewide")

    def test_escape_partitions(self):
        """Partitions coded with the escape: all-zero (0 bits), and every width up to 31 bits, with both methods."""
        rnd = random.Random(5)
        for method in (0, 1):
            for width in [0] + list(range(1, 32)):
                if width == 0:
                    x = [5] * 600
                else:
                    top = (1 << (width - 1)) - 1
                    x = [rnd.randint(-top - 1, top) for _ in range(600)]
                s = fe.encode([x], 48000, 32 if width > 16 else 16, blocksize=600, subframe={"kind": "fixed", "order": 0, "method": method, "porder": 2, "escape": "force"})
                self.check(s, "esc", verify_too=False, msg=f"method {method}, escape width {width}")
            s = fe.encode([fe.music(4096, 44100, 16, 8)], 44100, 16, blocksize=4096, subframe={"kind": "lpc", "order": 8, "method": method, "porder": 4, "escape": "auto"})
            self.check(s, "esc2", msg=f"method {method}, escape where cheaper")

    def test_silence_single_sample_and_empty(self):
        self.check(fe.encode([[0] * 10000] * 2, 44100, 16, blocksize=4096), "silent")
        self.check(fe.encode([[0] * 10000], 44100, 16, blocksize=4096, subframe={"kind": "verbatim"}), "silentv")
        self.check(fe.encode([[0] * 3000], 44100, 24, blocksize=4096, subframe={"kind": "fixed", "order": 4}), "silentf")
        for bps in (4, 16, 32):
            self.check(fe.encode([[-(1 << (bps - 1))]], 44100, bps), "one", msg=f"one sample of {bps} bits")
            self.check(fe.encode([[7], [-3]], 44100, 16, stereo="mid_side"), "one2")
        d = self.check(fe.encode([[], []], 44100, 16), "empty")
        self.assertEqual(len(d.pcm), 0)
        status, v = verify(fe.encode([[]], 44100, 16).data)
        self.assertEqual(status, 0, v)

    def test_full_scale_noise_and_extremes(self):
        for bps in (8, 16, 24, 32):
            top = (1 << (bps - 1)) - 1
            pcm = [fe.noise(3000, bps, 1), fe.noise(3000, bps, 2)]
            self.check(fe.encode(pcm, 44100, bps, blocksize=1000, stereo="auto"), "fs", msg=f"{bps} bits full scale noise")
            sq = [[top if (i // 3) % 2 else -top - 1 for i in range(3000)], [-top - 1 if (i // 5) % 2 else top for i in range(3000)]]
            for mode in ("indep", "left_side", "right_side", "mid_side"):
                self.check(fe.encode(sq, 44100, bps, blocksize=1000, stereo=mode), "ext", msg=f"{bps} bits square wave of full scale, {mode}")

    def test_32_bit_side_channels(self):
        """The side channel of 32-bit audio has 33 bits: left = -2**31 against right = 2**31 - 1 gives a side of -2**32 + 1 (and the other way round)."""
        lo, hi = -(1 << 31), (1 << 31) - 1
        left = [lo, hi, lo, hi, lo, lo, hi, hi, 0, -1, 1, lo, hi] * 40
        right = [hi, lo, lo, hi, hi, hi, lo, lo, 0, 1, -1, hi, lo] * 40
        for mode in ("left_side", "right_side", "mid_side", "indep", "auto"):
            for kind in ("auto", "verbatim"):
                s = fe.encode([left, right], 44100, 32, blocksize=260, stereo=mode, subframe={"kind": kind})
                self.check(s, "wide", msg=f"{mode}, {kind}")
        rnd = random.Random(2)
        l2 = [rnd.randint(lo, hi) for _ in range(2000)]
        r2 = [rnd.randint(lo, hi) for _ in range(2000)]
        for mode in ("left_side", "right_side", "mid_side"):
            self.check(fe.encode([l2, r2], 96000, 32, blocksize=1000, stereo=mode), "wide2", msg=f"random 32-bit, {mode}")
        sm = [v * 65536 for v in fe.music(3000, 44100, 16, 1)]                       # smooth 32-bit audio: the predictors work on 33-bit side channels
        sm2 = [v * 65536 + 9 for v in fe.music(3000, 44100, 16, 2)]
        for mode in ("left_side", "right_side", "mid_side"):
            self.check(fe.encode([sm, sm2], 44100, 32, blocksize=1000, stereo=mode), "wide3", msg=f"smooth 32-bit, {mode}")

    def test_a_sine_and_a_long_music_stream(self):
        self.check(fe.encode([fe.tone(44100, 44100, 1000, 0.9, 16)] * 2, 44100, 16, blocksize=4096, stereo="auto"), "sine")
        n = 120000
        self.check(fe.encode([fe.music(n, 44100, 16, 1), fe.music(n, 44100, 16, 2)], 44100, 16, blocksize=4096, stereo="auto"), "long")


# ------------------------------------------------------------------------------------------------------- metadata, seeking

class Metadata(FlacCase):
    def test_vorbis_comments(self):
        pcm = [fe.tone(2000, 44100, 440, 0.5, 16)]
        s = fe.encode(pcm, 44100, 16, comments=["TITLE=Shrine Steps", "artist=Reimu", "Album=Moon", "TRACKNUMBER=7", "DATE=2026", "GENRE=Game",
                                                "COMMENT=ignored", "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x4"], vendor="test vendor")
        i = info(s.data)
        self.assertEqual((i["title"], i["artist"], i["album"], i["tracknumber"], i["date"], i["genre"], i["vendor"]),
                         ("Shrine Steps", "Reimu", "Moon", "7", "2026", "Game", "test vendor"))
        self.check(s)
        jp = "東方紅魔郷 ～ the Embodiment".encode()
        i = info(fe.encode(pcm, 44100, 16, comments=[b"TITLE=" + jp, "ARTIST=\u00e9\u00e8"]).data)
        self.assertEqual(i["title"].encode(), jp)
        self.assertEqual(i["artist"], "\u00e9\u00e8")
        i = info(fe.encode(pcm, 44100, 16, comments=[b"TITLE=a\x01b\tc\x7fd\xffe\xc3", b"ARTIST=" + b"x" * 500, b"ALBUM=" + "é".encode() * 200]).data)
        self.assertEqual(i["title"], "a b c d?e?")                                         # control characters and bad UTF-8 are made harmless
        self.assertEqual(len(i["artist"]), 127)                                            # cut to fit
        self.assertEqual(i["album"], "é" * 63)                                             # ... at a character boundary
        i = info(fe.encode(pcm, 44100, 16, comments=[]).data)
        self.assertEqual(i["title"], "")
        i = info(fe.encode([fe.tone(100, 44100, 440, 0.3, 16)] * 6, 44100, 16, comments=["WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x60F"]).data)
        self.assertEqual(i["channel_mask"], "0x60f")                                       # a layout the tag gives (it must have as many bits as channels)
        i = info(fe.encode([fe.tone(100, 44100, 440, 0.3, 16)] * 6, 44100, 16, comments=["WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x3"]).data)
        self.assertEqual(i["channel_mask"], "0x3f")                                        # a wrong one is ignored

    def test_vorbis_comment_damage(self):
        """A comment block that lies about its own lengths gives fewer tags, never a fault."""
        for cut in range(0, 40):
            vc = fe.vorbis_comment_block("v", ["TITLE=Hello", "ARTIST=World"])[:cut]
            s = fe.encode([fe.tone(500, 44100, 440, 0.5, 16)], 44100, 16, extra_blocks=[(4, vc)])
            self.check(s, "vc", msg=f"comment block cut to {cut} bytes")
        huge = struct.pack("<I", 0xFFFFFFFF) + b"v" + struct.pack("<I", 0xFFFFFFFF) + struct.pack("<I", 0xFFFFFFFF) + b"TITLE=x"
        s = fe.encode([fe.tone(500, 44100, 440, 0.5, 16)], 44100, 16, extra_blocks=[(4, huge)])
        self.check(s, "vc2")

    def test_big_picture_and_other_blocks(self):
        pcm = [fe.music(9000, 44100, 16, 1), fe.music(9000, 44100, 16, 2)]
        for size in (1000, 300000, 5 << 20):
            s = fe.encode(pcm, 44100, 16, blocksize=4096, picture_bytes=size, padding=5000, application=True, cuesheet=True, comments=["TITLE=With a picture"])
            d = self.check(s, "pic", msg=f"picture of {size} bytes")
            i = info(s.data)
            self.assertEqual((i["pictures"], int(i["picture_bytes"]) >= size, i["title"]), ("1", True, "With a picture"))
        s = fe.encode(pcm, 44100, 16, blocksize=4096, extra_blocks=[(7, b"x" * 77), (100, b"y" * 3), (6, fe.picture_block(10)), (6, fe.picture_block(20))])
        self.check(s, "reserved")                                                           # reserved types and two pictures
        self.assertEqual(info(s.data)["pictures"], "2")

    def test_opening_reads_little(self):
        """A playlist opens hundreds of files to read their tags: flac_open reads a few KiB of the file, however big its picture and its audio are."""
        pcm = [fe.music(30000, 44100, 16, 1), fe.music(30000, 44100, 16, 2)]
        for kw in (dict(), dict(picture_bytes=3 << 20, comments=["TITLE=x"], padding=70000)):
            s = fe.encode(pcm, 44100, 16, blocksize=4096, stereo="auto", **kw)
            i = info(s.data, "open")
            self.assertLessEqual(int(i["open_bytes"]), 20000, f"{len(s.data)} bytes of file, {i['open_bytes']} read to open it")

    def test_id3v2_in_front(self):
        pcm = [fe.music(5000, 44100, 16, 1)]
        payload = b"TIT2" + bytes(40)
        tag = b"ID3\x04\x00\x00" + bytes([0, 0, 0, len(payload)]) + payload
        s = fe.encode(pcm, 44100, 16, id3v2=tag, blocksize=2048)
        self.check(s, "id3")
        self.assertEqual(info(s.data)["id3v2_bytes"], str(len(tag)))
        tag2 = b"ID3\x03\x00\x10" + bytes([0, 0, 0, len(payload)]) + payload + b"3DI" + bytes(7)         # with a footer
        self.check(fe.encode(pcm, 44100, 16, id3v2=tag2), "id3b")

    def test_trailing_bytes_after_the_last_frame(self):
        """A tag after the audio (an ID3v1 block, an APE tag) is not an error when the stream knows how long it is."""
        pcm = [fe.music(5000, 44100, 16, 1)]
        s = fe.encode(pcm, 44100, 16, blocksize=2048, trailer=b"TAG" + bytes(125))
        self.check(s, "trail")
        s = fe.encode(pcm, 44100, 16, blocksize=2048, trailer=b"APETAGEX" + bytes(200) + b"\xff\xf8" + b"junk")
        self.check(s, "trail2")
        s = fe.encode(pcm, 44100, 16, blocksize=2048, trailer=b"TAG" + bytes(125), total_samples=0)         # length unknown: the garbage is reported
        d = decode(s.data, "trail3")
        self.assertEqual(d.codes, [E_SYNC])
        self.assertTrue(np.array_equal(d.pcm, expect_pcm(s)))

    def test_audio_that_starts_beyond_4_gib(self):
        """64-bit offsets: a sparse file with 300 PADDING blocks of 16 MiB between STREAMINFO and the frames (the audio starts at 4.7 GiB): it opens, decodes, verifies and
        seeks (by its seek table and without) as if the padding were small. The file is made in the Linux file system (holes cost nothing there)."""
        import tempfile
        n = 20000
        s = fe.encode([fe.music(n, 44100, 16, 1), fe.music(n, 44100, 16, 2)], 44100, 16, blocksize=2048, stereo="auto")
        points = [(f.first_sample, f.start - s.first_frame_offset, f.nsamples) for f in s.frames]
        pad = 300
        d = tempfile.mkdtemp(dir="/tmp")
        try:
            for with_table in (True, False):
                path = Path(d) / f"big{int(with_table)}.flac"
                with open(path, "wb") as f:
                    f.write(b"fLaC" + fe.metadata_header(0, 34, False) + s.data[s.streaminfo_offset:s.streaminfo_offset + 34])
                    if with_table:
                        table = fe.seektable_block(points)
                        f.write(fe.metadata_header(3, len(table), False) + table)
                    for i in range(pad):
                        f.write(fe.metadata_header(1, 0xFFFFFF, i == pad - 1))
                        f.seek(0xFFFFFF, 1)                                              # a hole
                    f.write(s.data[s.first_frame_offset:])
                i = kv_lines(tool("info", path)[1])
                self.assertGreater(int(i["first_frame_offset"]), 1 << 32, i)
                out = WORK / "big.raw"
                status, text = tool("decode", path, out)
                self.assertEqual(status, 0, text)
                self.assertTrue(np.array_equal(np.frombuffer(out.read_bytes(), dtype="<i4"), expect_pcm(s)), "the decoded audio")
                status, text = tool("verify", path)
                self.assertEqual(status, 0, text)
                status, text = tool("seektest", path, 3, 80)
                self.assertEqual(status, 0, f"with_table={with_table}: {text}")
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_unknown_length_and_no_md5(self):
        pcm = [fe.music(9000, 44100, 16, 1)]
        s = fe.encode(pcm, 44100, 16, blocksize=4096, total_samples=0)
        d = self.check(s, "unk")
        s = fe.encode(pcm, 44100, 16, blocksize=4096, md5=False)
        self.check(s, "nomd5")
        status, v = verify(s.data)
        self.assertEqual((status, v["md5_present"], v["md5_ok"]), (0, "0", "0"))

    def test_a_stream_cut_out_of_a_longer_one(self):
        """The first frame is number 1000 (not 0): nothing is lost and no silence is put in front of it."""
        pcm = [fe.music(9000, 44100, 16, 1)]
        for variable in (False, True):
            s = fe.encode(pcm, 44100, 16, blocksize=3000, variable=variable, first_number=1000)
            d = self.check(s, "cut", msg=f"variable={variable}")
            self.assertEqual(d.summary["concealed"], "0")


class Seeking(FlacCase):
    def seek(self, data, name, ref, target, count=3000):
        p = write(name + ".flac", data)
        out = WORK / "seek.raw"
        status, text = tool("seek", p, target, count, out)
        self.assertEqual(status, 0, text)
        got = np.frombuffer(out.read_bytes(), dtype="<i4")
        return text, got

    def check_seeks(self, s, name, targets, msg=""):
        ref = expect_pcm(s)
        ch = s.channels
        total = s.total_samples
        for t in targets:
            text, got = self.seek(s.data, name, ref, t)
            t_eff = min(t, total)
            want = ref[t_eff * ch:(t_eff + 3000) * ch]
            self.assertTrue(np.array_equal(got, want), f"{msg}: seek to {t}: wrong samples (got {len(got)} values, wanted {len(want)}; {text.strip()})")
            self.assertIn(f"seek=0 tell={t_eff}", text, f"{msg}: seek to {t}")

    def targets(self, total, bs):
        t = {0, 1, 2, bs - 1, bs, bs + 1, 2 * bs - 1, 2 * bs, total // 2, total // 3, total - 1, total - 2, total, total + 1, total + 1000}
        rnd = random.Random(1)
        t |= {rnd.randrange(0, total) for _ in range(12)}
        return sorted(x for x in t if x >= 0)

    def test_with_a_seek_table(self):
        n = 60000
        pcm = [fe.music(n, 44100, 16, 1), fe.music(n, 44100, 16, 2)]
        for every, holes in ((4096, 0), (10000, 3), (1, 0)):
            s = fe.encode(pcm, 44100, 16, blocksize=4096, stereo="auto", seek_every=every, seek_placeholders=holes)
            self.assertGreater(int(info(s.data)["seek_points"]), 0)
            self.check_seeks(s, "seekt", self.targets(n, 4096), f"seek table every {every}, {holes} placeholders")

    def test_without_a_seek_table(self):
        n = 70000
        pcm = [fe.music(n, 44100, 16, 3), fe.music(n, 44100, 16, 4)]
        s = fe.encode(pcm, 44100, 16, blocksize=4096, stereo="auto")
        self.check_seeks(s, "seekn", self.targets(n, 4096), "fixed block size, no seek table")
        rnd = random.Random(2)
        sizes, tot = [], 0
        while tot < n:
            sizes.append(rnd.choice([1152, 4096, 576, 3000, 2048, 192]))
            tot += sizes[-1]
        pcm = [fe.music(tot, 44100, 16, 3), fe.music(tot, 44100, 16, 4)]
        s = fe.encode(pcm, 44100, 16, blocksize=sizes, stereo="auto")
        self.check_seeks(s, "seekv", self.targets(tot, 4096), "variable block size, no seek table")
        s = fe.encode(pcm[:1], 44100, 16, blocksize=2000, total_samples=0)                     # length unknown: seeking decodes from the start
        self.check_seeks(s, "seeku", [0, 5000, tot - 1], "length unknown")

    def test_a_seek_table_that_lies(self):
        n = 30000
        pcm = [fe.music(n, 44100, 16, 1)]
        for lie in ("offset", "sample", "random", "past"):
            s = fe.encode(pcm, 44100, 16, blocksize=2048, seek_every=2048)
            data = bytearray(s.data)
            at = 4 + 4 + 34                                                                      # fLaC, the STREAMINFO block, then the SEEKTABLE's header
            self.assertEqual(data[at] & 0x7F, 3)
            length = int.from_bytes(data[at + 1:at + 4], "big")
            rnd = random.Random(9)
            for k in range(length // 18):
                base = at + 4 + 18 * k
                if lie == "offset":
                    data[base + 8:base + 16] = (int.from_bytes(data[base + 8:base + 16], "big") + 7).to_bytes(8, "big")
                elif lie == "sample":
                    data[base:base + 8] = (int.from_bytes(data[base:base + 8], "big") + 3000).to_bytes(8, "big")
                elif lie == "random":
                    data[base:base + 18] = bytes(rnd.getrandbits(8) for _ in range(18))
                else:
                    data[base + 8:base + 16] = (1 << 40).to_bytes(8, "big")
            bad = fe.Stream(bytes(data), s.frames, s.pcm, s.rate, s.bps, s.channels, s.md5, s.first_frame_offset, s.streaminfo_offset, s.total_samples)
            self.check_seeks(bad, "seekl", [0, 1, 2047, 2048, 9999, 20000, n - 1], f"a seek table with lying {lie}")

    def test_seeking_back_and_forth_in_one_session(self):
        """Many seeks in one open stream, each followed by a read of a random length, every one checked against a full decode: the state machine of seeking and reading on."""
        n = 30000
        for name, kw in (("seek table", dict(blocksize=1024, seek_every=4096)), ("no seek table", dict(blocksize=1024)),
                         ("variable block size", dict(blocksize=[1000, 333, 4096, 192, 2000, 576] * 5))):
            n = sum(kw["blocksize"]) if isinstance(kw["blocksize"], list) else 30000
            s = fe.encode([fe.music(n, 44100, 16, 1), fe.music(n, 44100, 16, 2)], 44100, 16, stereo="auto", **kw)
            p = write("many.flac", s.data)
            status, text = tool("seektest", p, 7, 300)
            self.assertEqual(status, 0, f"{name}: {text}")

    def test_big_picture_does_not_slow_seeking(self):
        n = 50000
        pcm = [fe.music(n, 44100, 16, 1)]
        s = fe.encode(pcm, 44100, 16, blocksize=4096, picture_bytes=3 << 20, seek_every=4096)
        self.check_seeks(s, "seekp", [0, 20000, 49999], "after a 3 MiB picture")


# --------------------------------------------------------------------------------------------------------------- damage

def damage_stream(frames=12, bs=1024, tail=300, mode="auto", seed=1, **kw):
    n = bs * (frames - 1) + tail
    pcm = [fe.music(n, 44100, 16, seed), fe.music(n, 44100, 16, seed + 1)]
    return fe.encode(pcm, 44100, 16, blocksize=bs, stereo=mode, **kw)


class Damage(FlacCase):
    def assert_property(self, s, d, damaged, msg="", ends_early=False):
        """Everything outside the damaged frames is exact, the damaged frames are silent, one error per damaged stretch, the length is right."""
        ch = s.channels
        want = expect_pcm(s).copy()
        for k in damaged:
            f = s.frames[k]
            want[f.first_sample * ch:(f.first_sample + f.nsamples) * ch] = 0
        if ends_early:                                                                   # a lost last frame is lost, not replaced
            want = want[:min(f.first_sample for f in (s.frames[k] for k in damaged)) * ch]
        self.assertEqual(len(d.pcm), len(want), f"{msg}: {d.text}")
        diff = np.nonzero(d.pcm != want)[0]
        self.assertEqual(len(diff), 0, f"{msg}: {len(diff)} samples differ, the first at frame {diff[0] // ch if len(diff) else 0}")

    def test_a_flipped_bit_in_a_frame_body(self):
        s = damage_stream()
        rnd = random.Random(1)
        for k in (0, 1, 5, 10, 11):
            for trial in range(6):
                f = s.frames[k]
                data = bytearray(s.data)
                pos = rnd.randrange(f.start + f.header_len, f.end)                  # the subframes and the CRC-16
                data[pos] ^= 1 << rnd.randrange(8)
                d = decode(bytes(data), "flip")
                self.assertEqual(len(d.events), 1, f"frame {k}, byte {pos}: {d.text}")
                self.assertIn(d.codes[0], (E_CRC, E_FRAME), d.text)
                self.assert_property(s, d, [k], f"frame {k} byte {pos}", ends_early=(k == len(s.frames) - 1))
                self.assertEqual(int(d.summary["concealed"]), 0 if k == len(s.frames) - 1 else f_len(s, k))
                status, v = verify(bytes(data), "flipv")
                self.assertNotEqual(status, 0)

    def test_a_flipped_bit_in_a_frame_header(self):
        s = damage_stream()
        for k in (0, 3, 6, 10):
            f = s.frames[k]
            for byte in range(f.header_len):
                for bit in (0, 3, 7):
                    data = bytearray(s.data)
                    data[f.start + byte] ^= 1 << bit
                    d = decode(bytes(data), "flipH")
                    self.assertEqual(len(d.events), 1, f"frame {k} header byte {byte} bit {bit}: {d.text}")
                    self.assertIn(d.codes[0], (E_HEADER, E_SYNC), d.text)
                    self.assert_property(s, d, [k], f"frame {k} header byte {byte} bit {bit}")

    def test_a_damaged_last_frame_and_a_damaged_first_frame(self):
        s = damage_stream()
        last = len(s.frames) - 1
        data = bytearray(s.data)
        data[s.frames[last].start + s.frames[last].header_len + 3] ^= 0x10
        d = decode(bytes(data), "last")
        self.assertEqual(len(d.events), 1, d.text)
        self.assert_property(s, d, [last], "last frame", ends_early=True)
        data = bytearray(s.data)
        data[s.frames[0].start + 2] ^= 0x04
        d = decode(bytes(data), "first")
        self.assert_property(s, d, [0], "first frame")

    def test_several_damaged_frames(self):
        s = damage_stream(frames=20)
        rnd = random.Random(4)
        for trial in range(25):
            ks = sorted(rnd.sample(range(0, 19), rnd.randint(1, 5)))                  # not the last: a lost last frame is not concealed
            data = bytearray(s.data)
            for k in ks:
                f = s.frames[k]
                data[rnd.randrange(f.start, f.end)] ^= 1 << rnd.randrange(8)
            d = decode(bytes(data), "multi")
            self.assertGreaterEqual(len(d.events), 1)
            # frames damaged in the sync code or the CRC-8 are found again by position: all of them are silent, all the others exact
            self.assert_property(s, d, ks, f"frames {ks}")

    def test_garbage_between_frames(self):
        s = damage_stream()
        for size in (1, 2, 37, 500, 3000):
            rnd = random.Random(size)
            junk = bytes(rnd.getrandbits(8) & 0x7F for _ in range(size))               # no 0xFF: no false sync
            at = s.frames[4].start
            d = decode(s.data[:at] + junk + s.data[at:], "junk")
            self.assertEqual(d.codes, [E_SYNC], d.text)
            self.assertTrue(np.array_equal(d.pcm, expect_pcm(s)), f"{size} bytes of junk")
        junk = b"\xff\xf8\xff\xf9\xff\xff\xf8" * 40                                        # sync-like bytes whose headers do not check out
        d = decode(s.data[:at] + junk + s.data[at:], "junk2")
        self.assertEqual(len(d.events), 1, d.text)
        self.assertTrue(np.array_equal(d.pcm, expect_pcm(s)))

    def test_truncation(self):
        s = damage_stream()
        ch = s.channels
        for k in (1, 4, 11):
            f = s.frames[k]
            for cut in (f.start + 1, f.start + 3, f.start + f.header_len, f.start + f.header_len + 5, (f.start + f.end) // 2, f.end - 1):
                d = decode(s.data[:cut], "trunc")
                self.assertEqual(d.codes, [E_SYNC] if cut == f.start + 1 else [E_TRUNCATED], f"cut in frame {k} at {cut - f.start}: {d.text}")   # one lone byte is junk, not a frame
                self.assertTrue(np.array_equal(d.pcm, expect_pcm(s)[:f.first_sample * ch]), f"cut in frame {k}")
            d = decode(s.data[:f.start], "trunc2")                                        # between frames: a clean end
            self.assertEqual(d.events, [])
            self.assertEqual(len(d.pcm), f.first_sample * ch)
            status, v = verify(s.data[:f.start])
            self.assertNotEqual(status, 0)                                                # the length and the MD5 are wrong
        for cut in (0, 1, 3, 4, 5, 8, 20, 41, s.first_frame_offset - 1):
            self.assertEqual(info(s.data[:cut], "tm")["error"], str(E_NOTFLAC if cut < 4 else E_BADMETA), f"cut at {cut}")

    def test_lying_streaminfo(self):
        pcm = [fe.music(12000, 44100, 16, 1), fe.music(12000, 44100, 16, 2)]
        base = dict(blocksize=2048, stereo="auto")
        want = expect_pcm(fe.encode(pcm, 44100, 16, **base))
        cases = {"md5": dict(streaminfo={"md5": bytes(range(16))}), "total smaller": dict(total_samples=5000), "total larger": dict(total_samples=50000),
                 "total unknown": dict(total_samples=0), "block sizes": dict(streaminfo={"min_block": 16, "max_block": 16}),
                 "block sizes zero": dict(streaminfo={"min_block": 0, "max_block": 0}), "frame sizes tiny": dict(min_max_frame=(1, 2)),
                 "frame sizes huge": dict(min_max_frame=(0xFFFFFF, 0xFFFFFF)), "frame sizes zero": dict(min_max_frame=(0, 0)),
                 "max block 65535": dict(streaminfo={"max_block": 65535})}
        for name, kw in cases.items():
            s = fe.encode(pcm, 44100, 16, **base, **kw)
            d = decode(s.data, "lie")
            self.assertEqual(d.status, 0, name)
            self.assertEqual(d.events, [], f"{name}: {d.text}")                           # the frames are fine whatever the header claims
            self.assertTrue(np.array_equal(d.pcm, want), name)
            status, v = verify(s.data, "liev")
            expect = {"md5": E_MD5, "total smaller": E_LENGTH, "total larger": E_LENGTH}.get(name, 0)
            self.assertEqual(int(v["result"]), expect, f"{name}: {v}")
        for name, over in (("channels", {"channels": 3}), ("bits", {"bps": 24}), ("rate", {"rate": 48000}), ("rate zero", {"rate": 0}), ("bits 1", {"bps": 1}),
                           ("channels 8", {"channels": 8})):
            s = fe.encode(pcm, 44100, 16, **base, streaminfo=over)
            d = decode(s.data, "lie2")                                                    # a header that contradicts the frames: errors or a refusal, nothing worse
            self.assertTrue(d.status != 0 or d.events or "open error" in d.text, name)

    def test_absurd_values_in_the_metadata(self):
        s = fe.encode([fe.music(3000, 44100, 16, 1)], 44100, 16, comments=["TITLE=x"], seek_every=1000)
        base = s.data
        si = s.streaminfo_offset
        cases = {}
        cases["marker"] = b"fLaX" + base[4:]
        cases["first block not STREAMINFO"] = base[:4] + bytes([base[4] | 1]) + base[5:]
        cases["streaminfo too short"] = base[:5] + (33).to_bytes(3, "big") + base[8:]
        cases["streaminfo too long"] = base[:5] + (35).to_bytes(3, "big") + base[8:]
        cases["forbidden block type 127"] = base[:4] + bytes([0x7F]) + base[5:]
        d = bytearray(base)
        d[si + 17] = 0                                                                  # sample rate low bits ...
        d[si + 10:si + 14] = bytes(4)                                                   # ... sample rate zero
        cases["rate zero"] = bytes(d)
        for pat in (b"\xff" * 34, bytes(34)):
            d = bytearray(base)
            d[si:si + 34] = pat
            cases[f"streaminfo {pat[0]:#x}"] = bytes(d)
        d = bytearray(base)
        nxt = si + 34                                                                   # the header of the block after STREAMINFO
        d[nxt + 1:nxt + 4] = b"\xff\xff\xff"                                            # a block of 16 MiB in a small file
        cases["huge block length"] = bytes(d)
        d = bytearray(base)
        d[nxt] |= 0x80                                                                  # the last-block flag early: the audio is parsed from the middle of a block
        cases["last flag early"] = bytes(d)
        d = bytearray(base)
        d[nxt] &= 0x7F
        d[nxt] = (d[nxt] & 0x80) | 0x7F
        cases["type 127 later"] = bytes(d)
        for name, data in cases.items():
            r = decode(data, "abs")
            ok = r.status != 0 or "open error" in r.text or r.events or len(r.pcm) != 3000
            self.assertTrue(ok, f"{name} was accepted as a good stream: {r.text}")
        for name in ("marker", "first block not STREAMINFO", "streaminfo too short", "streaminfo too long", "forbidden block type 127", "rate zero", "huge block length"):
            i = info(cases[name], "absi")
            self.assertIn("error", i, name)

    def test_a_file_of_frame_like_headers_costs_a_bounded_amount(self):
        """Every eight bytes a frame header that passes its CRC-8 and promises two 32768-sample VERBATIM subframes (128 KiB of data to read before the CRC-16 can fail):
        if each of these cost a whole attempt the 2 MiB file would take minutes. The decoder gives up stepping one byte at a time inside a stretch that keeps failing."""
        import time
        header = bytes([0xFF, 0xF8, 0xF9, 0x18, 0x00])
        record = header + bytes([fe.crc8(header), 0x02, 0x55])
        si = fe.streaminfo_block(32768, 32768, 0, 0, 44100, 2, 16, 0, bytes(16))
        data = b"fLaC" + fe.metadata_header(0, 34, True) + si + record * ((2 << 20) // 8)
        t0 = time.time()
        d = decode(data, "bomb")
        took = time.time() - t0
        self.assertEqual(d.status, 0, d.text)
        self.assertTrue(d.events, "such a file is not audio: errors are expected")
        self.assertLess(took, 8, f"{took:.1f} s: decoding the file costs too much (0.2 s with the limit, 16 s without it, for 2 MiB)")

    def test_tiny_and_empty_inputs(self):
        for data in (b"", b"f", b"fLaC", b"fLaC\x00", b"fLaC\x80\x00\x00\x22", b"\x00" * 100, b"fLaC" + bytes(40)):
            self.assertIn("error", info(data, "tiny"), data)
            d = decode(data, "tiny2")
            self.assertTrue("open error" in d.text or d.status != 0, data)


def f_len(s, k):
    return s.frames[k].nsamples


# ------------------------------------------------------------------------------------------------------ hostile frames

class HostileFrames(FlacCase):
    """Frames built by hand to break one rule each, with correct CRCs so that it is the rule that stops the decoder and not the checksum."""

    def stream(self, subframes, channels=1, bps=16, n=64, ch_code=None, bs_code=6, sr_code=9, sz_code=4, extra=b"", reserved_bit=0):
        ch_code = channels - 1 if ch_code is None else ch_code
        hdr = bytearray([0xFF, 0xF8, (bs_code << 4) | sr_code, (ch_code << 4) | (sz_code << 1) | reserved_bit]) + fe.coded_number(0)
        if bs_code == 6:
            hdr.append(n - 1)
        hdr += extra
        hdr.append(fe.crc8(hdr))
        bw = fe.BitWriter()
        subframes(bw)
        bw.align()
        frame = bytes(hdr) + bw.getvalue()
        frame += struct.pack(">H", fe.crc16(frame))
        si = fe.streaminfo_block(n, n, 0, 0, 44100, channels, bps, n, bytes(16))
        return b"fLaC" + fe.metadata_header(0, 34, True) + si + frame

    def rejects(self, data, name, codes):
        """Exactly one damaged spot is reported, with one of these codes, and no sample comes out."""
        d = decode(data, "hf")
        self.assertEqual(d.status, 0, f"{name}: {d.text}")
        self.assertEqual(len(d.events), 1, f"{name}: {d.text}")
        self.assertIn(d.codes[0], codes, f"{name}: {d.text}")
        self.assertEqual(len(d.pcm), 0, name)

    @staticmethod
    def constant(bw):
        fe._header(bw, 0, 0)
        bw.write_signed(5, 16)

    def test_the_good_baseline(self):
        def zeros(bw):                                                                   # FIXED order 0, one partition, escape with 0 bits: 64 zero samples
            fe._header(bw, 8, 0)
            bw.write(0, 2); bw.write(0, 4); bw.write(15, 4); bw.write(0, 5)
        d = decode(self.stream(zeros), "hf0")
        self.assertEqual(d.events, [], d.text)
        self.assertEqual(len(d.pcm), 64)
        d = decode(self.stream(self.constant), "hf1")
        self.assertEqual((d.events, list(d.pcm[:3]), len(d.pcm)), ([], [5, 5, 5], 64))

    def test_header_rules(self):
        for name, kw in (("block size code 0", dict(bs_code=0)), ("sample rate code 15", dict(sr_code=15)), ("channel code 11", dict(ch_code=11)),
                         ("channel code 15", dict(ch_code=15)), ("sample size code 3", dict(sz_code=3)), ("sample size differs from STREAMINFO", dict(sz_code=6)),
                         ("rate differs from STREAMINFO", dict(sr_code=10)), ("channel count differs", dict(ch_code=1)), ("stereo mode for one channel", dict(ch_code=9)),
                         ("reserved bit", dict(reserved_bit=1)), ("16-bit block size that is not there", dict(bs_code=7))):
            self.rejects(self.stream(self.constant, **kw), name, (E_HEADER, E_SYNC, E_FRAME, E_TRUNCATED))
        data = bytearray(self.stream(self.constant))
        at = data.find(b"\xff\xf8")
        data[at + 1] = 0xFA                                                               # the reserved bit of the sync code itself
        self.rejects(bytes(data), "sync code with the reserved bit", (E_SYNC,))
        data = bytearray(self.stream(self.constant))
        data[at + 3] ^= 0x10
        self.rejects(bytes(data), "header changed without fixing its CRC-8", (E_HEADER,))

    def test_subframe_rules(self):
        cases = {}

        def case(f):
            cases[f.__name__] = f
            return f

        @case
        def padding_bit_set(bw): bw.write(1, 1); bw.write(0, 6); bw.write(0, 1); bw.write_signed(1, 16)

        @case
        def reserved_type_2(bw): bw.write(0, 1); bw.write(2, 6); bw.write(0, 1)

        @case
        def reserved_type_16(bw): bw.write(0, 1); bw.write(16, 6); bw.write(0, 1)

        @case
        def reserved_type_31(bw): bw.write(0, 1); bw.write(31, 6); bw.write(0, 1)

        @case
        def fixed_order_5(bw):
            bw.write(0, 1); bw.write(8 + 5, 6); bw.write(0, 1)
            for _ in range(5): bw.write_signed(0, 16)
            bw.write(0, 2); bw.write(0, 4); bw.write(0, 4)

        @case
        def wasted_bits_as_many_as_the_sample(bw): bw.write(0, 1); bw.write(0, 6); bw.write(1, 1); bw.write_unary(15)

        @case
        def wasted_bits_more_than_the_sample(bw): bw.write(0, 1); bw.write(0, 6); bw.write(1, 1); bw.write_unary(40)

        @case
        def coding_method_2(bw): fe._header(bw, 8, 0); bw.write(2, 2); bw.write(0, 4)

        @case
        def partition_order_that_does_not_divide(bw): fe._header(bw, 8, 0); bw.write(0, 2); bw.write(7, 4)

        @case
        def first_partition_shorter_than_the_predictor(bw):          # 64 samples, order 8, partition order 4: 4 samples per partition
            fe._header(bw, 31 + 8, 0)
            for _ in range(8): bw.write_signed(0, 16)
            bw.write(11, 4); bw.write_signed(2, 5)
            for _ in range(8): bw.write_signed(0, 12)
            bw.write(0, 2); bw.write(4, 4)

        @case
        def lpc_precision_16(bw):
            fe._header(bw, 32, 0); bw.write_signed(0, 16); bw.write(15, 4); bw.write_signed(0, 5); bw.write_signed(0, 16)

        @case
        def lpc_negative_shift(bw):
            fe._header(bw, 32, 0); bw.write_signed(0, 16); bw.write(11, 4); bw.write_signed(-3, 5); bw.write_signed(1, 12)
            bw.write(0, 2); bw.write(0, 4); bw.write(15, 4); bw.write(0, 5)

        @case
        def rice_quotient_past_32_bits(bw):                          # method 1, parameter 30: a quotient above 3 does not fit in 32 bits
            fe._header(bw, 8, 0); bw.write(1, 2); bw.write(0, 4); bw.write(30, 5); bw.write_unary(4)

        @case
        def prediction_leaves_the_bit_depth(bw):                     # 32767 plus an escaped residual of 524287 does not fit in 16 bits
            fe._header(bw, 8 + 1, 0); bw.write_signed(32767, 16); bw.write(0, 2); bw.write(0, 4); bw.write(15, 4); bw.write(20, 5)
            for _ in range(63): bw.write_signed(0x7FFFF, 20)

        @case
        def not_enough_data(bw): bw.write(0, 1); bw.write(1, 6); bw.write(0, 1); bw.write_signed(1, 16)

        for name, f in cases.items():
            self.rejects(self.stream(f), name, (E_FRAME, E_TRUNCATED))
        self.rejects(self.stream(cases["lpc_precision_16"], n=16), "lpc precision, short block", (E_FRAME, E_TRUNCATED))

        def lpc_order_32_in_a_block_of_16(bw): fe._header(bw, 63, 0)
        self.rejects(self.stream(lpc_order_32_in_a_block_of_16, n=16), "lpc order above the block size", (E_FRAME, E_TRUNCATED))

    def test_a_decoded_sample_outside_its_bit_depth(self):
        """A stereo reconstruction that leaves the range of 16 bits is damage (it would have been a clip or a wrap)."""
        def left_side(bw):                                           # left 32767, side -32767: right = 65534
            fe._header(bw, 1, 0)
            for _ in range(64): bw.write_signed(32767, 16)
            fe._header(bw, 1, 0)
            for _ in range(64): bw.write_signed(-32767, 17)
        self.rejects(self.stream(left_side, channels=2, ch_code=8), "left/side out of range", (E_FRAME,))

        def mid_side(bw):                                            # mid 32767, side 3: left = (65535 + 3) >> 1 = 32769
            fe._header(bw, 1, 0)
            for _ in range(64): bw.write_signed(32767, 16)
            fe._header(bw, 1, 0)
            for _ in range(64): bw.write_signed(3, 17)
        self.rejects(self.stream(mid_side, channels=2, ch_code=10), "mid/side out of range", (E_FRAME,))


# -------------------------------------------------------------------------------------------------------------- fuzz

class Fuzz(FlacCase):
    def corpus(self):
        files = []
        n = 6000
        specs = [
            dict(pcm=[fe.music(n, 44100, 16, 1), fe.music(n, 44100, 16, 2)], bps=16, kw=dict(blocksize=1024, stereo="auto", seek_every=2048, comments=["TITLE=x", "ARTIST=y"])),
            dict(pcm=[fe.music(n, 48000, 24, 3)], bps=24, kw=dict(blocksize=2000, subframe={"kind": "lpc", "order": 24, "precision": 15})),
            dict(pcm=[fe.noise(n, 32, 4), fe.noise(n, 32, 5)], bps=32, kw=dict(blocksize=700, stereo="mid_side")),
            dict(pcm=[fe.music(n, 22050, 8, 6) for _ in range(6)], bps=8, kw=dict(blocksize=900)),
            dict(pcm=[[0] * n, [5] * n], bps=16, kw=dict(blocksize=1500)),
            dict(pcm=[fe.music(n, 44100, 12, 7), fe.music(n, 44100, 12, 8)], bps=12, kw=dict(blocksize=[500, 1000, 192, 2500, 1808], stereo="left_side")),
            dict(pcm=[fe.noise(n, 16, 9)], bps=16, kw=dict(blocksize=1200, subframe={"kind": "fixed", "order": 3, "method": 1, "porder": 4, "escape": "auto"})),
            dict(pcm=[[v << 2 for v in fe.music(n, 44100, 14, 10)], fe.music(n, 44100, 16, 11)], bps=16, kw=dict(blocksize=1000, stereo="right_side", picture_bytes=3000, padding=100)),
            dict(pcm=[fe.music(300, 44100, 16, 12)], bps=16, kw=dict(blocksize=100)),
            dict(pcm=[[]], bps=16, kw={}),
        ]
        for i, sp in enumerate(specs):
            s = fe.encode(sp["pcm"], 44100 if "rate" not in sp else sp["rate"], sp["bps"], **sp["kw"])
            files.append(write(f"fuzz{i}.flac", s.data))
        return files

    def test_mutation_fuzz(self):
        """Thousands of random changes (bit flips, bytes, truncations, deleted and duplicated chunks, extreme values) of valid streams: each is
        opened, read to the end, seeked in and verified. The sanitizers abort on any fault and an alarm on a hang; an error or a clean decode is the
        only acceptable end."""
        files = self.corpus()
        count = int(os.environ.get("TOUHOU_FLAC_FUZZ", "4000"))
        for seed in (1, 2):
            status, text = tool("fuzz", seed, count, *files, timeout=1800)
            self.assertEqual(status, 0, text)
            self.assertIn(f"iterations={count}", text)
            r = kv_words(text.replace("fuzz ", ""))
            self.assertGreater(int(r["opened"]), count // 10, text)                               # many mutations are survivable: the test is not trivially passing by refusing
            self.assertGreater(int(r["with_errors"]), 0, text)


if __name__ == "__main__":
    unittest.main()
