"""Host tests of libaudio's file readers (user/lib/audio/src/afile.c, wavext.c, aiff.c, au.c; flac.c has its own tests in test_flac.py), without QEMU.

  python tools/hosttest.py discover -s tests/host -p "test_audio_formats.py"        (on Windows this runs in WSL's Ubuntu: it needs gcc and Python 3.12)
  python tools/hosttest.py discover -s tests/host -p "test_audio_formats.py" -k Wav (one group)

The readers are built for the host by tests/host/audiotool.c with AddressSanitizer and UndefinedBehaviorSanitizer (build/host-audio/audiotool) and checked against
files that tests/host/audiogen.py makes: for each file the generator says which samples a reader must give back, and the C reader must give exactly those, bit
for bit, in the format the sound device takes. Three things are independent of the C code and of this reading of the specifications:

  * Python's own modules, which read the generated files (wave, aifc, sunau, scipy.io.wavfile) or compute the codecs (audioop: G.711, IMA ADPCM), so that
    the generator's model of each format is checked against another implementation (the class Independent; these need Python 3.12, which still has audioop);
  * the generator is a second, separate implementation of every format (an encoder), so the C reader and the Python writer would have to share a mistake;
  * every number a test checks comes from the generator's model of the samples, never from running the reader.

Not independent: Microsoft ADPCM (nothing in Python decodes it: the model and the C code come from one reading of the specification), the 80-bit AIFF rate (checked
only against the generator), and the choice of downmix gains (a design decision, checked against a model of the same formulas).
"""
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
import audiogen as ag  # noqa: E402
import flacenc as fe  # noqa: E402

TOOL = ROOT / "build" / "host-audio" / "audiotool"
WORK = ROOT / "build" / "audio-tests"
SRC = ROOT / "user/lib/audio/src"
SOURCES = [ROOT / "tests/host/audiotool.c", SRC / "afile.c", SRC / "wavext.c", SRC / "aiff.c", SRC / "au.c", SRC / "flac.c"]
HEADERS = [ROOT / "user/lib/audio/include/afile.h", ROOT / "user/lib/audio/include/flac.h", SRC / "afile_priv.h", ROOT / "shared/audio_abi.h"]
HAVE_GCC = shutil.which("gcc") is not None
ENV = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=0:exitcode=77", UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1")


def build_tool():
    newest = max(p.stat().st_mtime for p in SOURCES + HEADERS)
    if TOOL.exists() and TOOL.stat().st_mtime >= newest:
        return
    TOOL.parent.mkdir(parents=True, exist_ok=True)
    cmd = ["gcc", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
           "-I", str(ROOT / "user/lib/audio/include"), "-I", str(SRC), "-I", str(ROOT / "shared"), *map(str, SOURCES), "-o", str(TOOL)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("cannot build tests/host/audiotool.c:\n" + r.stdout + r.stderr)


def tool(*args, timeout=300):
    r = subprocess.run([str(TOOL), *map(str, args)], capture_output=True, timeout=timeout, env=ENV)
    err = r.stderr.decode("utf-8", "replace")
    if r.returncode < 0 or r.returncode >= 77 or "runtime error" in err or "AddressSanitizer" in err or "LeakSanitizer" in err:
        raise AssertionError(f"audiotool {' '.join(map(str, args))}: status {r.returncode}\n{err[-3000:]}")
    return r.returncode, r.stdout.decode("utf-8", "replace")


def kv_lines(text):
    d = {}
    for line in text.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            d[k] = v
    return d


def kv_words(text):
    d = {}
    for word in text.split():
        if "=" in word:
            k, v = word.split("=", 1)
            d[k] = v
    return d


def write(name, data):
    WORK.mkdir(parents=True, exist_ok=True)
    p = WORK / name
    p.write_bytes(data if isinstance(data, bytes) else data.data)
    return p


def info_of(path):
    status, text = tool("info", path)
    return kv_lines(text), status


def pcm_of(path, down=False):
    out = WORK / "out.raw"
    if out.exists():
        out.unlink()
    args = ["pcm"] + (["--down"] if down else []) + [path, out]
    status, text = tool(*args)
    summary = kv_words([ln for ln in text.splitlines() if ln.startswith("frames=")][0]) if "frames=" in text else {}
    return (out.read_bytes() if out.exists() else b""), summary, text


def read_wav(data):
    """A small WAV reader for what afile_render_wav writes (and for plain files): (rate, container bits, valid bits, mask, [samples per channel as ints])."""
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE", data[:12]
    pos, fmt, body = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack_from("<I", data, pos + 4)[0]
        if cid == b"fmt ":
            fmt = data[pos + 8:pos + 8 + size]
        elif cid == b"data":
            body = data[pos + 8:pos + 8 + size]
            assert pos + 8 + size <= len(data), "the data chunk is longer than the file"
            break
        pos += 8 + size + (size & 1)
    tag, ch, rate, _byterate, block, bits = struct.unpack_from("<HHIIHH", fmt)
    valid, mask = bits, 0
    if tag == 0xFFFE:
        valid, mask, sub = struct.unpack_from("<HIH", fmt, 18)
        assert sub == 1, "the render is PCM"
    else:
        assert tag == 1
    assert block == ch * bits // 8
    cb = bits // 8
    n = len(body) // block
    chans = [[] for _ in range(ch)]
    for i in range(n):
        for c in range(ch):
            raw = body[i * block + c * cb:i * block + (c + 1) * cb]
            if cb == 1:
                v = raw[0] - 128
            else:
                v = int.from_bytes(raw, "little", signed=True)
            chans[c].append(v >> (bits - valid))
    return rate, bits, valid, mask, chans


FORMAT_CODE = {8: 1, 16: 2, 24: 3, 32: 4}


@unittest.skipUnless(HAVE_GCC, "needs gcc (run it through tools/hosttest.py)")
class AudioCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build_tool()

    def check(self, a, name="f", msg="", seek=True, render=True, delivered_check=True):
        """The reader reports what the generator says and delivers exactly the samples the generator predicts; the same goes for a render to WAV and for seeking."""
        p = write(name, a)
        i, status = info_of(p)
        self.assertEqual(status, 0, f"{msg}: {i}")
        self.assertEqual((i["format"], i["codec"]), (a.fmt, a.codec), msg)
        self.assertEqual((int(i["rate"]), int(i["channels"]), int(i["bits"]), int(i["frames"])), (a.rate, a.channels, a.bits, a.frames), f"{msg}: {i}")
        self.assertEqual(int(i["channel_mask"], 16), a.mask, msg)
        for k in ("title", "artist", "album", "track", "date"):
            self.assertEqual(i[k], a.tags.get(k, ""), f"{msg}: tag {k}")
        c = ag.container_of(a.bits)
        self.assertEqual((int(i["out_format"]), int(i["out_channels"]), int(i["out_bytes"]), int(i["out_shift"])), (FORMAT_CODE[c], a.channels, a.channels * c // 8, c - a.bits), msg)
        if delivered_check:
            got, summary, text = pcm_of(p)
            want = ag.delivered(a)
            self.assertEqual(summary.get("damage"), "0", f"{msg}: {text}")
            self.assertEqual(len(got), len(want), f"{msg}: delivered {len(got)} bytes, expected {len(want)}")
            if got != want:
                x = next(k for k in range(len(want)) if got[k] != want[k])
                self.fail(f"{msg}: the delivered samples differ, the first at byte {x} (frame {x // (a.channels * c // 8)}): got {got[x]:#x}, expected {want[x]:#x}")
        if render and a.frames:
            out = WORK / "render.wav"
            status, text = tool("wav", p, out)
            self.assertEqual(status, 0, f"{msg}: {text}")
            rate, cbits, valid, mask, chans = read_wav(out.read_bytes())
            self.assertEqual((rate, cbits, valid), (a.rate, c, a.bits), msg)
            self.assertEqual(mask, a.mask if (a.channels > 2 or c > 16 or a.bits != c) else 0, msg)         # only the extensible header carries a mask
            self.assertEqual(chans, a.chans, f"{msg}: the rendered WAV differs from the samples")
            self.assertEqual(out.read_bytes(), ag.render_wav(a), f"{msg}: the rendered WAV is not byte for byte what the model says")
            if a.frames > 20:                                                                          # a part of it, as `play -s -t -w` renders
                status, text = tool("wav", p, out, a.frames // 3, a.frames // 4)
                self.assertEqual(status, 0, f"{msg}: {text}")
                self.assertEqual(out.read_bytes(), ag.render_wav(a, a.frames // 3, a.frames // 4), f"{msg}: a rendered part")
        if seek and a.frames > 10:
            self.check_seek(a, p, msg)

    def check_seek(self, a, p, msg=""):
        n, c = a.frames, ag.container_of(a.bits)
        fb = a.channels * c // 8
        want_all = ag.delivered(a)
        rnd = random.Random(5)
        for t in sorted({0, 1, n // 2, n - 1, n, n + 5, 7, n // 3} | {rnd.randrange(n) for _ in range(6)}):
            out = WORK / "seek.raw"
            status, text = tool("seek", p, t, 300, out)
            self.assertEqual(status, 0, text)
            t_eff = min(t, n)
            self.assertEqual(out.read_bytes(), want_all[t_eff * fb:(t_eff + 300) * fb], f"{msg}: after a seek to frame {t}")
            self.assertIn(f"seek=0 tell={t_eff}", text, msg)


# ------------------------------------------------------------------------------------------ the generator against Python

class Independent(unittest.TestCase):
    """The generator's models of the formats against Python's own implementations."""

    def test_g711_against_audioop(self):
        try:
            import audioop
        except ImportError:
            self.skipTest("audioop is gone (Python 3.13)")
        for b in range(256):
            self.assertEqual(ag.ulaw_decode(b), struct.unpack("<h", audioop.ulaw2lin(bytes([b]), 2))[0], f"mu-law {b:#x}")
            self.assertEqual(ag.alaw_decode(b), struct.unpack("<h", audioop.alaw2lin(bytes([b]), 2))[0], f"A-law {b:#x}")

    def test_ima_adpcm_against_audioop(self):
        try:
            import audioop
        except ImportError:
            self.skipTest("audioop is gone (Python 3.13)")
        rnd = random.Random(1)
        for trial in range(20):
            pred, index = rnd.randint(-30000, 30000), rnd.randint(0, 88)
            nibbles = [rnd.randrange(16) for _ in range(300)]
            ours, p, ix = [], pred, index
            for nib in nibbles:
                p, ix = ag.ima_step(nib, p, ix)
                ours.append(p)
            data = bytes((nibbles[i] << 4) | nibbles[i + 1] for i in range(0, len(nibbles), 2))    # audioop takes the high nibble first
            theirs, state = audioop.adpcm2lin(data, 2, (pred, index))
            self.assertEqual(list(struct.unpack(f"<{len(ours)}h", theirs)), ours, f"trial {trial}")
            self.assertEqual(state, (p, ix))

    def test_wave_module_reads_the_pcm_files(self):
        import wave
        for bits in (8, 16, 24, 32):
            a = ag.wav_pcm([ag.ints(500, bits, 1), ag.ints(500, bits, 2)], 22050, bits)
            p = write("ind.wav", a)
            with wave.open(str(p)) as w:
                self.assertEqual((w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()), (2, bits // 8, 22050, 500))
                raw = w.readframes(500)
            self.assertEqual(raw, ag.pcm_bytes(a.chans, bits // 8, bits, unsigned8=(bits == 8)), f"{bits} bits")

    def test_aifc_module_reads_the_aiff_files(self):
        import aifc
        a = ag.aiff([ag.ints(400, 16, 1), ag.ints(400, 16, 2)], 44100, 16, tags={"title": "T"})
        with aifc.open(str(write("ind.aif", a))) as f:
            self.assertEqual((f.getnchannels(), f.getsampwidth(), f.getframerate(), f.getnframes(), f.getcomptype()), (2, 2, 44100, 400, b"NONE"))
            self.assertEqual(f.readframes(400), ag.pcm_bytes(a.chans, 2, 16, big=True))
        for kind in ("ulaw", "alaw"):
            codes = [ag.codes(300, 1)]
            a = ag.aiff(codes, 8000, 16, comp=kind.upper())
            with aifc.open(str(write("ind.aifc", a))) as f:
                self.assertEqual((f.getnchannels(), f.getframerate(), f.getnframes()), (1, 8000, 300))
                got = f.readframes(300)                                                              # aifc expands the companded data to 16 bits (little endian)
            self.assertEqual(list(struct.unpack("<300h", got)), a.chans[0], kind)

    def test_sunau_module_reads_the_au_files(self):
        import sunau
        a = ag.au([ag.ints(300, 16, 1), ag.ints(300, 16, 2)], 16000, 3)
        with sunau.open(str(write("ind.au", a))) as f:
            self.assertEqual((f.getnchannels(), f.getsampwidth(), f.getframerate(), f.getnframes()), (2, 2, 16000, 300))
            self.assertEqual(f.readframes(300), ag.pcm_bytes(a.chans, 2, 16, big=True))
        a = ag.au([ag.codes(200, 3)], 8000, 1)
        with sunau.open(str(write("ind2.au", a))) as f:
            got = f.readframes(200)                                                                  # sunau expands mu-law to 16 bits
        self.assertEqual(list(struct.unpack("<200h", got)), a.chans[0])

    def test_scipy_reads_the_float_files(self):
        try:
            from scipy.io import wavfile
        except ImportError:
            self.skipTest("no scipy")
        rnd = random.Random(2)
        for bits in (32, 64):
            x = [[rnd.uniform(-1, 1) for _ in range(200)], [rnd.uniform(-1, 1) for _ in range(200)]]
            for ext in (False, True):
                a = ag.wav_float(x, 44100, bits, extensible=ext, mask=3 if ext else 0)
                rate, data = wavfile.read(str(write("ind_f.wav", a)))
                self.assertEqual(rate, 44100)
                self.assertEqual(data.shape, (200, 2))
                ref = np.array(x, dtype=np.float32 if bits == 32 else np.float64).T
                self.assertTrue(np.array_equal(data, ref), f"float{bits} extensible={ext}")


# ----------------------------------------------------------------------------------------------------------------- WAVE

class Wav(AudioCase):
    def test_pcm(self):
        for bits in (8, 16, 24, 32):
            for ch in (1, 2):
                a = ag.wav_pcm([ag.ints(1200, bits, 10 + c) for c in range(ch)], 44100, bits)
                self.check(a, "pcm", f"{bits}-bit, {ch} channels")
        for rate in (4000, 8000, 11025, 22050, 48000, 96000, 192000, 7, 1000000):
            self.check(ag.wav_pcm([ag.ints(300, 16, 1)], rate, 16), "rate", f"{rate} Hz")

    def test_pcm_extremes(self):
        for bits in (8, 16, 24, 32):
            top = (1 << (bits - 1)) - 1
            x = [top, -top - 1, 0, 1, -1, top, top, -top - 1, -top - 1] * 20
            self.check(ag.wav_pcm([x], 8000, bits), "ext", f"{bits}-bit extremes")

    def test_valid_bits_in_wider_containers(self):
        """12 bits in 16, 20 in 24, 24 in 32 (the samples left-justified in the container): the readers give the significant bits back."""
        for valid, container in ((12, 16), (20, 24), (24, 32), (9, 16), (18, 24), (4, 8), (1, 8)):
            chans = [[v >> 0 for v in ag.ints(900, valid, 3 + c)] for c in range(2)]
            for ext in (True,):
                a = ag.wav_pcm(chans, 44100, valid, container=container, extensible=ext, mask=0x3)
                self.check(a, "valid", f"{valid} bits in {container}, extensible")
        a = ag.wav_pcm([ag.ints(900, 12, 3)], 44100, 12, container=16)                          # plain PCM: wBitsPerSample 12, two bytes a sample
        self.check(a, "valid2", "12 bits in a plain PCM header")

    def test_extensible_masks_and_channel_counts(self):
        for ch, mask in ((1, 0x4), (2, 0x3), (3, 0x7), (4, 0x33), (5, 0x37), (6, 0x3F), (7, 0x70F), (8, 0x63F), (2, 0x600), (6, 0x60F), (4, 0)):
            a = ag.wav_pcm([ag.ints(500, 16, c + 1) for c in range(ch)], 48000, 16, extensible=True, mask=mask)
            self.check(a, "mask", f"{ch} channels, mask {mask:#x}")
        for ch in (9, 12, 16, 32):
            self.check(ag.wav_pcm([ag.ints(100, 16, c + 1) for c in range(ch)], 48000, 16), "wide", f"{ch} channels")

    def test_a_mask_that_disagrees_with_the_channels_is_ignored(self):
        a = ag.wav_pcm([ag.ints(300, 16, 1), ag.ints(300, 16, 2)], 44100, 16, extensible=True, mask=0x7)
        a.mask = 0
        self.check(a, "badmask", "a three speaker mask on two channels")

    def test_float(self):
        rnd = random.Random(3)
        for bits in (32, 64):
            for ch in (1, 2, 6):
                x = [[rnd.uniform(-1.2, 1.2) for _ in range(400)] for _ in range(ch)]
                self.check(ag.wav_float(x, 44100, bits), "flt", f"float{bits}, {ch} channels")
            self.check(ag.wav_float([[rnd.uniform(-1, 1) for _ in range(300)]] * 2, 48000, bits, extensible=True, mask=3), "fltx", f"float{bits} extensible")
        special = [0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 2.0, -2.0, 1e-30, -1e-30, 0.999999999, -0.999999999, float("inf"), float("-inf"), float("nan"), 1 / 3, 4.656612873077393e-10, 3e38]
        self.check(ag.wav_float([special * 3], 8000, 64), "fltsp", "float64 special values")
        self.check(ag.wav_float([special * 3], 8000, 32), "fltsp2", "float32 special values")

    def test_g711(self):
        for kind in ("alaw", "ulaw"):
            for ch in (1, 2):
                a = ag.wav_g711([ag.codes(700, c + 1) for c in range(ch)], 8000, kind)
                self.check(a, "g711", f"{kind}, {ch} channels")
            self.check(ag.wav_g711([list(range(256)) * 2], 8000, kind), "g711all", f"{kind}, every code")

    def test_ima_adpcm(self):
        for ch, block, n in ((1, 256, 2000), (2, 512, 3001), (1, 1024, 2047), (2, 256, 700), (4, 512, 1500), (1, 64, 300), (3, 128, 500)):
            x = [ag.ints(n, 16, 20 + c) for c in range(ch)]
            self.check(ag.wav_ima(x, 22050, block), "ima", f"IMA ADPCM, {ch} channels, {block}-byte blocks, {n} frames")
        a = ag.wav_ima([ag.ints(1000, 16, 1)], 22050, 256, fact=False)
        self.check(a, "ima2", "IMA ADPCM without a fact chunk (the last block is full)")

    def test_ima_adpcm_against_audioop_in_a_file(self):
        """A whole generated IMA file decoded by audioop block by block (the independent decoder) against the C reader."""
        try:
            import audioop
        except ImportError:
            self.skipTest("audioop is gone (Python 3.13)")
        a = ag.wav_ima([ag.ints(1500, 16, 4)], 22050, 256)
        body = a.data[a.data.index(b"data") + 8:]
        out = []
        for off in range(0, len(body) - len(body) % 256, 256):
            blk = body[off:off + 256]
            pred, index, _ = struct.unpack("<hBB", blk[:4])
            data = bytes(((b & 15) << 4) | (b >> 4) for b in blk[4:])                           # WAVE has the earlier sample in the low nibble
            dec, _state = audioop.adpcm2lin(data, 2, (pred, index))
            out += [pred] + list(struct.unpack(f"<{len(dec) // 2}h", dec))
        got, summary, text = pcm_of(write("imaref.wav", a))
        self.assertEqual(list(struct.unpack(f"<{len(got) // 2}h", got)), out[:1500])

    def test_ms_adpcm(self):
        for ch, block, n in ((1, 256, 2000), (2, 512, 2500), (1, 512, 700), (2, 256, 1000), (1, 128, 300)):
            x = [ag.ints(n, 16, 30 + c) for c in range(ch)]
            self.check(ag.wav_msadpcm(x, 22050, block), "ms", f"Microsoft ADPCM, {ch} channels, {block}-byte blocks, {n} frames")
        self.check(ag.wav_msadpcm([ag.ints(1000, 16, 1)], 22050, 256, coefs=[(256, 0), (0, 0), (384, -128)]), "ms2", "Microsoft ADPCM with its own three coefficient pairs")
        self.check(ag.wav_msadpcm([ag.ints(1000, 16, 1)], 22050, 256, fact=False), "ms3", "Microsoft ADPCM without a fact chunk")

    def test_rf64_and_bw64(self):
        for kind in (b"RF64", b"BW64"):
            a = ag.wav_pcm([ag.ints(800, 16, 1), ag.ints(800, 16, 2)], 44100, 16, kind=kind, ds64=True)
            self.check(a, "rf64", kind.decode())
            a = ag.wav_pcm([ag.ints(300, 24, 1)] * 6, 48000, 24, extensible=True, mask=0x3F, kind=kind, ds64=True)
            self.check(a, "rf64b", f"{kind.decode()} 6 channels 24 bits")
        a = ag.wav_pcm([ag.ints(800, 16, 1)], 44100, 16, kind=b"RF64", ds64=True)                 # without a usable ds64 the 0xFFFFFFFF is a stream: the data runs to the end
        self.check(a, "rf64c", "RF64")

    def test_data_chunk_sizes(self):
        """A data chunk of size 0 or 0xFFFFFFFF is a stream that was being written; one that promises more than the file holds was cut short."""
        for mode in ("exact", "zero", "ffff", "larger"):
            a = ag.wav_pcm([ag.ints(600, 16, 1), ag.ints(600, 16, 2)], 44100, 16, data_size=mode)
            self.check(a, "dsize", f"data size {mode}")
        a = ag.wav_pcm([ag.ints(600, 16, 1)], 44100, 16, data_size="larger")
        cut = a.data[:-37]                                                                           # a partial frame at the end is dropped
        p = write("cutwav.wav", cut)
        got, summary, text = pcm_of(p)
        self.assertEqual(int(summary["frames"]), (600 * 2 - 37) // 2)
        self.assertEqual(got, ag.delivered(a)[:len(got)])

    def test_odd_sized_chunks_and_chunks_after_the_data(self):
        junk = ag._chunk(b"JUNK", b"abcde")                                                          # five bytes and a pad byte
        extra = ag._chunk(b"cue ", bytes(11))
        a = ag.wav_pcm([ag.ints(501, 8, 1)], 11025, 8, before_data=junk + extra, after_data=ag._chunk(b"id3 ", b"xyz"), list_info={"INAM": "After the data"})
        self.check(a, "odd", "odd chunks before and after an odd-sized data chunk")
        self.assertEqual(a.frames, 501)

    def test_list_info_tags(self):
        tags = {"INAM": "Shrine Steps", "IART": "Reimu", "IPRD": "Moon", "ITRK": "3", "ICRD": "2026-11-17"}
        self.check(ag.wav_pcm([ag.ints(300, 16, 1)], 44100, 16, list_info=tags), "tags", "LIST/INFO")
        self.check(ag.wav_pcm([ag.ints(300, 16, 1)], 44100, 16, list_info={"INAM": "東方 ~ Moon".encode(), "IART": b"Caf\xe9 \x01Bar"}), "tags2", "UTF-8 and Latin-1 tags")
        a = ag.wav_pcm([ag.ints(300, 16, 1)], 44100, 16, list_info={"INAM": b"Caf\xe9"})
        self.assertEqual(a.tags["title"], "Café")
        self.check(a, "tags3", "a Latin-1 tag is converted to UTF-8")
        a = ag.wav_pcm([ag.ints(300, 16, 1)], 44100, 16, list_info={"IPRT": "7", "INAM": "x" * 400})
        self.assertEqual(a.tags, {"track": "7", "title": "x" * 127})
        self.check(a, "tags4", "a long title is cut")

    def test_fmt_chunk_with_a_zero_cbsize(self):
        self.check(ag.wav_pcm([ag.ints(300, 16, 1)], 44100, 16, fmt_bytes=18), "fmt18", "an 18-byte fmt chunk")

    def test_unsupported_formats_are_named(self):
        a = ag.wav_pcm([ag.ints(300, 16, 1)], 44100, 16)
        for tag, word in ((0x55, "MP3"), (0x161, "Windows Media"), (0x2000, "AC-3"), (0x1234, "unknown")):
            data = bytearray(a.data)
            at = data.index(b"fmt ") + 8
            data[at:at + 2] = struct.pack("<H", tag)
            i, status = info_of(write("unsup.wav", bytes(data)))
            self.assertEqual(status, 1)
            self.assertIn(word, i["error"], i)
            self.assertIn(f"{tag:04X}", i["error"])

    def test_broken_headers(self):
        a = ag.wav_pcm([ag.ints(300, 16, 1)], 44100, 16)
        cases = {}
        d = bytearray(a.data); d[d.index(b"fmt ") + 10:d.index(b"fmt ") + 12] = b"\0\0"; cases["no channels"] = d                          # nChannels = 0
        d = bytearray(a.data); d[d.index(b"fmt ") + 12:d.index(b"fmt ") + 16] = b"\0\0\0\0"; cases["rate 0"] = d
        d = bytearray(a.data); d[d.index(b"fmt ") + 20:d.index(b"fmt ") + 22] = b"\x05\0"; cases["block align 5"] = d
        d = bytearray(a.data); d[d.index(b"fmt "):d.index(b"fmt ") + 4] = b"fmX "; cases["no fmt"] = d
        d = bytearray(a.data); d[d.index(b"data"):d.index(b"data") + 4] = b"dat "; cases["no data"] = d
        cases["empty after RIFF"] = bytearray(a.data[:12])
        cases["header only"] = bytearray(a.data[:20])
        for name, data in cases.items():
            i, status = info_of(write("broken.wav", bytes(data)))
            self.assertEqual(status, 1, f"{name}: accepted: {i}")
            self.assertTrue(i.get("error"), name)


# ------------------------------------------------------------------------------------------------------------------ AIFF

class Aiff(AudioCase):
    def test_pcm_sizes(self):
        for bits in (8, 16, 24, 32, 12, 20, 9, 1, 4, 31):
            for ch in (1, 2):
                a = ag.aiff([ag.ints(900, bits, c + 1) for c in range(ch)], 44100, bits)
                self.check(a, "aiff", f"AIFF {bits}-bit, {ch} channels")
        self.check(ag.aiff([ag.ints(900, 16, 1)] * 6, 48000, 16), "aiff6", "AIFF, 6 channels")

    def test_sample_rates(self):
        for rate in (1, 7, 8000, 11025, 22050, 32000, 44100, 48000, 88200, 96000, 192000, 1000000):
            self.check(ag.aiff([ag.ints(200, 16, 1)], rate, 16), "rate", f"{rate} Hz")

    def test_aiff_c_compression_types(self):
        for kind in ("NONE", "twos", "sowt"):
            for bits in (16, 24, 32, 8):
                self.check(ag.aiff([ag.ints(500, bits, 1), ag.ints(500, bits, 2)], 44100, bits, comp=kind), "aifc", f"AIFF-C {kind} {bits}-bit")
        self.check(ag.aiff([ag.ints(500, 24, 1)], 44100, 24, comp="in24"), "aifc2", "AIFF-C in24")
        self.check(ag.aiff([ag.ints(500, 32, 1)], 44100, 32, comp="in32"), "aifc3", "AIFF-C in32")
        self.check(ag.aiff([ag.ints(500, 8, 1)], 44100, 8, comp="raw "), "aifc4", "AIFF-C raw (unsigned 8-bit)")
        rnd = random.Random(1)
        for kind in ("fl32", "FL32", "fl64", "FL64"):
            x = [[rnd.uniform(-1.1, 1.1) for _ in range(300)] for _ in range(2)]
            self.check(ag.aiff(x, 44100, 32, comp=kind), "aifc5", f"AIFF-C {kind}")
        for kind in ("ulaw", "ULAW", "alaw", "ALAW"):
            self.check(ag.aiff([ag.codes(400, 1), ag.codes(400, 2)], 8000, 16, comp=kind), "aifc6", f"AIFF-C {kind}")

    def test_tags_padding_and_offsets(self):
        self.check(ag.aiff([ag.ints(301, 8, 1)], 11025, 8, tags={"title": "Odd Length", "artist": "Reimu"}), "aiffn", "odd chunk sizes and NAME / AUTH")
        self.check(ag.aiff([ag.ints(300, 16, 1)], 44100, 16, ssnd_offset=6, extra=ag._iff(b"ANNO", b"a comment")), "aiffo", "an SSND offset and an ANNO chunk")
        a = ag.aiff([ag.ints(300, 16, 1)], 44100, 16, declared_frames=200)
        self.check(a, "aiffd", "the header counts fewer frames than the data holds")
        a = ag.aiff([ag.ints(300, 16, 1)], 44100, 16, declared_frames=500)
        a.frames = 300
        a.chans = [a.chans[0]]
        self.check(a, "aiffe", "the header counts more frames than the data holds")

    def test_unsupported_compression_is_named(self):
        a = ag.aiff([ag.ints(100, 16, 1)], 44100, 16, comp="NONE")
        data = bytearray(a.data)
        at = data.index(b"NONE")
        data[at:at + 4] = b"ima4"
        i, status = info_of(write("ima4.aifc", bytes(data)))
        self.assertEqual(status, 1)
        self.assertIn("ima4", i["error"])

    def test_broken_files(self):
        a = ag.aiff([ag.ints(100, 16, 1)], 44100, 16)
        for name, mod in (("no COMM", lambda d: d.replace(b"COMM", b"COMX")), ("no SSND", lambda d: d.replace(b"SSND", b"SSNX")),
                          ("channels 0", lambda d: d[:d.index(b"COMM") + 8] + b"\0\0" + d[d.index(b"COMM") + 10:]),
                          ("rate 0", lambda d: d[:d.index(b"COMM") + 16] + bytes(10) + d[d.index(b"COMM") + 26:]),
                          ("size 0", lambda d: d[:d.index(b"COMM") + 14] + b"\0\0" + d[d.index(b"COMM") + 16:])):
            i, status = info_of(write("broken.aif", mod(a.data)))
            self.assertEqual(status, 1, f"{name}: accepted: {i}")


# --------------------------------------------------------------------------------------------------------------------- AU

class Au(AudioCase):
    def test_encodings(self):
        for enc in (2, 3, 4, 5):
            for ch in (1, 2):
                bits = {2: 8, 3: 16, 4: 24, 5: 32}[enc]
                self.check(ag.au([ag.ints(800, bits, c + 1) for c in range(ch)], 8000, enc), "au", f"AU encoding {enc}, {ch} channels")
        rnd = random.Random(1)
        for enc in (6, 7):
            x = [[rnd.uniform(-1.1, 1.1) for _ in range(300)] for _ in range(2)]
            self.check(ag.au(x, 44100, enc), "auf", f"AU float encoding {enc}")
        for enc in (1, 27):
            self.check(ag.au([ag.codes(500, 1), ag.codes(500, 2)], 8000, enc), "aug", f"AU G.711 encoding {enc}")

    def test_annotation_and_unknown_size(self):
        self.check(ag.au([ag.ints(300, 16, 1)], 16000, 3, annotation=b"made for a test\0"), "aua", "an annotation")
        self.check(ag.au([ag.ints(300, 16, 1)], 16000, 3, data_size="unknown"), "aus", "data size 0xFFFFFFFF")
        self.check(ag.au([ag.ints(300, 16, 1)], 16000, 3, data_size="larger"), "aul", "data size past the end of the file")

    def test_broken_files(self):
        a = ag.au([ag.ints(100, 16, 1)], 8000, 3)
        for name, off, val in (("offset 0", 4, 0), ("encoding 99", 12, 99), ("rate 0", 16, 0), ("channels 0", 20, 0), ("channels 99", 20, 99), ("offset past the end", 4, 100000)):
            d = bytearray(a.data)
            d[off:off + 4] = struct.pack(">I", val)
            i, status = info_of(write("broken.au", bytes(d)))
            self.assertEqual(status, 1, f"{name}: accepted: {i}")


# ------------------------------------------------------------------------------------------------------------- detection

class Detection(AudioCase):
    def test_by_magic_bytes_not_by_name(self):
        flac = fe.encode([fe.music(1000, 44100, 16, 1)], 44100, 16)
        files = {"FLAC": flac.data, "WAV": ag.wav_pcm([ag.ints(100, 16, 1)], 8000, 16).data, "AIFF": ag.aiff([ag.ints(100, 16, 1)], 8000, 16).data,
                 "AU": ag.au([ag.ints(100, 16, 1)], 8000, 3).data}
        for kind, data in files.items():
            for name in ("a.mp3", "a.txt", "noextension", "a.wav", "a.flac", "A.AIF", "a.au", "a.mod", "a.tms"):
                p = write(name, data)
                status, text = tool("probe", p)
                self.assertEqual(text.strip(), kind, f"{kind} named {name}")
                i, st = info_of(p)
                self.assertEqual(st, 0, f"{kind} named {name}: {i}")
                self.assertEqual(i["format"].split("-")[0] if kind != "FLAC" else i["format"], kind if kind != "WAV" else i["format"], name)

    def test_other_files_are_not_ours(self):
        rnd = random.Random(1)
        for name, data in (("empty", b""), ("text", b"hello world\n" * 20), ("junk", bytes(rnd.getrandbits(8) for _ in range(5000))), ("riff but not wave", b"RIFF\0\0\0\0AVI LIST"),
                           ("short flac", b"fLa"), ("form but not aiff", b"FORM\0\0\0\0ILBM"), ("mp3 with id3", b"ID3\x03\0\0\0\0\0\x20" + bytes(100) + b"\xff\xfb\x90\x00"),
                           ("png", b"\x89PNG\r\n\x1a\n" + bytes(100))):
            p = write("other.bin", data)
            status, text = tool("probe", p)
            self.assertEqual(text.strip(), "", name)
            i, st = info_of(p)
            self.assertEqual(st, 1, name)

    def test_flac_after_an_id3v2_tag(self):
        flac = fe.encode([fe.music(1000, 44100, 16, 1)], 44100, 16)
        payload = bytes(300)
        tag = b"ID3\x04\x00\x00" + bytes([0, 0, 2, 0x2C]) + payload
        p = write("tagged.flac", tag + flac.data)
        status, text = tool("probe", p)
        self.assertEqual(text.strip(), "FLAC")
        i, st = info_of(p)
        self.assertEqual((st, i["format"], i["frames"]), (0, "FLAC", "1000"))


# ---------------------------------------------------------------------------------------------------------------- FLAC

class FlacFront(AudioCase):
    """What the front adds to the FLAC decoder: the delivered format, the tags, the render to WAV."""

    def flac(self, chans, bps, rate=44100, **kw):
        s = fe.encode(chans, rate, bps, **kw)
        return ag.Audio(s.data, [list(c) for c in chans], rate, bps, "FLAC", "FLAC", self.mask(len(chans)), {})

    @staticmethod
    def mask(ch):
        return {1: 0x4, 2: 0x3, 3: 0x7, 4: 0x33, 5: 0x37, 6: 0x3F, 7: 0x70F, 8: 0x63F}[ch]

    def test_depths_and_channel_counts(self):
        for bps in (4, 7, 8, 12, 16, 20, 24, 32):
            for ch in (1, 2):
                a = self.flac([fe.music(1500, 44100, bps, 5 + c) for c in range(ch)], bps, blocksize=700, stereo="auto")
                self.check(a, "ff", f"FLAC {bps}-bit, {ch} channels")
        for ch in (3, 6, 8):
            a = self.flac([fe.music(900, 48000, 16, c + 1) for c in range(ch)], 16, 48000, blocksize=400)
            self.check(a, "ff2", f"FLAC {ch} channels")

    def test_tags_and_mask(self):
        a = self.flac([fe.music(500, 44100, 16, 1)], 16, comments=["TITLE=Shrine", "ARTIST=Reimu", "ALBUM=Moon", "TRACKNUMBER=2", "DATE=2026"])
        a.tags = {"title": "Shrine", "artist": "Reimu", "album": "Moon", "track": "2", "date": "2026"}
        self.check(a, "fft", "FLAC tags")
        a = self.flac([fe.music(500, 44100, 16, c + 1) for c in range(6)], 16, comments=["WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x60F"])
        a.mask = 0x60F
        self.check(a, "ffm", "FLAC channel mask from the tag")

    def test_damage_is_reported_and_skipped(self):
        n = 6000
        s = fe.encode([fe.music(n, 44100, 16, 1), fe.music(n, 44100, 16, 2)], 44100, 16, blocksize=1000, stereo="auto")
        data = bytearray(s.data)
        f = s.frames[2]
        data[f.start + f.header_len + 5] ^= 0x40
        p = write("dmg.flac", bytes(data))
        got, summary, text = pcm_of(p)
        self.assertEqual(summary["damage"], "1", text)
        self.assertIn("damage:", text)
        self.assertEqual(int(summary["concealed"]), 1000)
        want = bytearray(ag.delivered(ag.Audio(b"", [list(c) for c in s.pcm], 44100, 16)))
        want[2000 * 4:3000 * 4] = bytes(1000 * 4)
        self.assertEqual(got, bytes(want))


# ------------------------------------------------------------------------------------------------------------ downmixing

class Downmix(AudioCase):
    def test_more_than_two_channels_to_stereo(self):
        for ch, mask in ((3, 0x7), (4, 0x33), (5, 0x37), (6, 0x3F), (7, 0x70F), (8, 0x63F), (6, 0), (4, 0x603), (3, 0x1000), (9, 0), (12, 0)):
            for bits in (16, 24):
                a = ag.wav_pcm([ag.ints(700, bits, c + 1) for c in range(ch)], 48000, bits, extensible=bool(mask), mask=mask)
                p = write("down.wav", a)
                got, summary, text = pcm_of(p, down=True)
                want = ag.delivered(ag.Audio(b"", ag.downmixed(a), 48000, bits))
                self.assertEqual(got, want, f"{ch} channels, mask {mask:#x}, {bits} bits")
                i, _ = info_of(p)
        a = ag.wav_pcm([ag.ints(100, 16, 1)] * 2, 48000, 16)                                                  # stereo and mono are left alone
        got, summary, text = pcm_of(write("down2.wav", a), down=True)
        self.assertEqual(got, ag.delivered(a))

    def test_loud_sources_do_not_clip(self):
        top = 32767
        for ch in (3, 6, 8):
            x = [[top, -top - 1] * 50 for _ in range(ch)]
            a = ag.wav_pcm(x, 48000, 16, extensible=True, mask={3: 0x7, 6: 0x3F, 8: 0x63F}[ch])
            got, summary, text = pcm_of(write("loud.wav", a), down=True)
            vals = np.frombuffer(got, dtype="<i2")
            self.assertLessEqual(int(np.abs(vals.astype(np.int32)).max()), 32768, ch)
            self.assertEqual(list(vals), list(np.frombuffer(ag.delivered(ag.Audio(b"", ag.downmixed(a), 48000, 16)), dtype="<i2")))


# -------------------------------------------------------------------------------------------------------- truncation, fuzz

class Damage(AudioCase):
    def test_truncated_files_play_what_is_there(self):
        for a, name in ((ag.wav_pcm([ag.ints(1000, 16, 1), ag.ints(1000, 16, 2)], 44100, 16), "wav"), (ag.aiff([ag.ints(1000, 16, 1)], 44100, 16), "aiff"),
                        (ag.au([ag.ints(1000, 24, 1)], 44100, 4), "au"), (ag.wav_ima([ag.ints(1500, 16, 1)], 22050, 256), "ima"),
                        (ag.wav_msadpcm([ag.ints(1500, 16, 1)], 22050, 256), "ms")):
            for cut in (len(a.data) // 3, len(a.data) // 2 + 1, len(a.data) - 1):
                p = write("trunc." + name, a.data[:cut])
                i, status = info_of(p)
                if status:
                    continue                                                                               # cut inside the header: refused, fine
                got, summary, text = pcm_of(p)
                want = ag.delivered(a)
                self.assertEqual(want[:len(got)], got, f"{name} cut at {cut}: what is delivered is a prefix of the whole")
                self.assertGreater(len(got), 0)

    def test_a_damaged_adpcm_block_is_silence_and_reported(self):
        a = ag.wav_msadpcm([ag.ints(1500, 16, 1)], 22050, 256)
        data = bytearray(a.data)
        at = data.index(b"data") + 8 + 256 * 2                                                           # the predictor index of the third block
        data[at] = 99
        got, summary, text = pcm_of(write("badblk.wav", bytes(data)))
        self.assertEqual(summary["damage"], "1", text)
        spb = (256 - 7) * 2 + 2
        want = bytearray(ag.delivered(a))
        want[2 * spb * 2:3 * spb * 2] = bytes(spb * 2)
        self.assertEqual(got, bytes(want))

    def test_mutation_fuzz(self):
        """Random changes of generated files of every kind: opened, read to the end, seeked in. The sanitizers abort on any fault and an alarm on a hang."""
        rnd = random.Random(3)
        files = []
        corpus = [ag.wav_pcm([ag.ints(900, 16, 1), ag.ints(900, 16, 2)], 44100, 16), ag.wav_pcm([ag.ints(300, 24, 1)] * 6, 48000, 24, extensible=True, mask=0x3F),
                  ag.wav_float([[rnd.uniform(-1, 1) for _ in range(300)]] * 2, 44100, 32), ag.wav_g711([ag.codes(500, 1)], 8000, "ulaw"),
                  ag.wav_ima([ag.ints(900, 16, 1), ag.ints(900, 16, 2)], 22050, 256, list_info={"INAM": "t"}), ag.wav_msadpcm([ag.ints(900, 16, 1)], 22050, 256),
                  ag.wav_pcm([ag.ints(300, 16, 1)], 44100, 16, kind=b"RF64", ds64=True), ag.aiff([ag.ints(500, 16, 1)] * 2, 44100, 16, tags={"title": "x"}),
                  ag.aiff([ag.codes(300, 1)], 8000, 16, comp="ULAW"), ag.aiff([ag.ints(300, 16, 1)], 44100, 16, comp="sowt"), ag.au([ag.ints(500, 16, 1)], 8000, 3),
                  ag.au([ag.codes(300, 1)], 8000, 27)]
        corpus.append(ag.Audio(fe.encode([fe.music(3000, 44100, 16, 1)] * 2, 44100, 16, blocksize=700, stereo="auto").data, [[0]], 44100, 16))
        for i, a in enumerate(corpus):
            files.append(write(f"fuzz{i}.bin", a))
        count = int(os.environ.get("TOUHOU_AUDIO_FUZZ", "20000"))
        for seed in (1, 2):
            status, text = tool("fuzz", seed, count, *files, timeout=1800)
            self.assertEqual(status, 0, text)
            r = kv_words(text.replace("fuzz ", ""))
            self.assertEqual(r["iterations"], str(count))
            self.assertGreater(int(r["opened"]), count // 10, text)


# ------------------------------------------------------------------------------------------------ the Jukebox's folder scan

JB_SRC = ROOT / "user/bin/jukebox/playlist.c"
JB_TOOL = ROOT / "build" / "host-audio" / "jukebox_scan"
JB_PRELUDE = r'''
#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
struct { int ntr; } J;                  /* the Jukebox's list has more in it; the scan only counts */
int playlist_add(const char *path, bool shipped, bool named) { (void)named; printf("%s %s\n", shipped ? "S" : "U", path); J.ntr++; return J.ntr - 1; }
'''
JB_MAIN = r'''
int main(int argc, char **argv) { (void)argc; playlist_add_directory(argv[1], false); fprintf(stderr, "tracks=%d\n", J.ntr); return 0; }
'''


def jukebox_scan_constants():
    src = JB_SRC.read_text(encoding="utf-8").replace("\r\n", "\n")
    return {k: int(re.search(rf"#define {k}\s+(\d+)", src).group(1)) for k in ("SCAN_ENTRIES", "SCAN_DEPTH", "SCAN_TRACKS")}


def build_jukebox_scan():
    """The folder scan of the Jukebox (has_suffix, is_music_name, by_name, add_directory, playlist_add_directory) is cut out of playlist.c itself and put in front of a stub
    playlist_add that prints the path: the real code, on the host's own readdir and stat, with the sanitizers on."""
    if JB_TOOL.exists() and JB_TOOL.stat().st_mtime >= JB_SRC.stat().st_mtime:
        return
    src = JB_SRC.read_text(encoding="utf-8").replace("\r\n", "\n")
    a, b = src.index("static bool has_suffix("), src.index("static void base_title(")
    c, d = src.index("static int by_name("), src.index("void playlist_scan(void)")
    WORK.mkdir(parents=True, exist_ok=True)
    JB_TOOL.parent.mkdir(parents=True, exist_ok=True)
    cfile = WORK / "jukebox_scan.c"
    cfile.write_text(JB_PRELUDE + src[a:b] + "\n" + src[c:d] + JB_MAIN, encoding="utf-8", newline="\n")
    r = subprocess.run(["gcc", "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-Wall", "-Wextra", "-Werror", str(cfile), "-o", str(JB_TOOL)],
                       capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("cannot build the Jukebox scan harness:\n" + r.stdout + r.stderr)


@unittest.skipUnless(HAVE_GCC, "needs gcc (run it through tools/hosttest.py)")
class JukeboxScan(unittest.TestCase):
    """What the Jukebox puts in its list from a folder (user/bin/jukebox/playlist.c): each folder sorted by name, music in the folders inside it down to SCAN_DEPTH levels,
    hidden entries and other files left out, a cap on the entries read from one folder and on the tracks in all, and no endless walk through a loop of symbolic links."""

    @classmethod
    def setUpClass(cls):
        build_jukebox_scan()
        cls.k = jukebox_scan_constants()

    def scan(self, root):
        r = subprocess.run([str(JB_TOOL), str(root)], capture_output=True, env=ENV, timeout=300)
        err = r.stderr.decode("utf-8", "replace")
        self.assertEqual(r.returncode, 0, err[-2000:])
        self.assertNotIn("Sanitizer", err)
        self.assertNotIn("runtime error", err)
        return [ln.split(" ", 1)[1] for ln in r.stdout.decode("utf-8").splitlines()], int(re.search(r"tracks=(\d+)", err).group(1))

    @staticmethod
    def touch(root, *names):
        for n in names:
            p = Path(root) / n
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(b"")

    def test_each_folder_is_sorted_and_albums_are_found(self):
        self.assertEqual(self.k["SCAN_DEPTH"], 3, "docs/AUDIO.md and jukebox.pw say three levels")
        with tempfile.TemporaryDirectory() as t:
            self.touch(t, "Album One/02.flac", "Album One/01.flac", "Album Two/Disc 1/a.wav", "Album Two/Disc 2/deep.wav", "Zed/L1/L2/ok3.flac", "Zed/L1/L2/L3/too-deep.flac",
                       ".hiddendir/x.flac", ".hidden.flac", "a-top.flac", "B-top.wav", "D.FLAC", "c.TXT", "e.mod", "f.tms", "g.AIFF", "h.au", "i.aifc", "j.fla", "Zed/note.md",
                       "Zed/L1/z.snd", "k.wave", "l.aif", "m.mp3", "noext")
            (Path(t) / "empty").mkdir()
            got, n = self.scan(t)
            names = ["Album One/01.flac", "Album One/02.flac", "Album Two/Disc 1/a.wav", "Album Two/Disc 2/deep.wav", "B-top.wav", "D.FLAC", "Zed/L1/L2/ok3.flac", "Zed/L1/z.snd",
                     "a-top.flac", "e.mod", "f.tms", "g.AIFF", "h.au", "i.aifc", "j.fla", "k.wave", "l.aif"]         # byte order, a folder where its name falls; the music files only
            self.assertEqual(got, [f"{t}/{x}" for x in names])
            self.assertEqual(n, len(names))

    def test_folders_inside_folders_stop_at_the_depth_limit(self):
        with tempfile.TemporaryDirectory() as t:
            chain = ["a", "b", "c", "d", "e", "f"]
            for level in range(len(chain) + 1):
                self.touch(t, "/".join(chain[:level] + [f"lvl{level}.flac"]))
            got, n = self.scan(t)
            self.assertCountEqual([x[len(t) + 1:] for x in got], ["/".join(chain[:level] + [f"lvl{level}.flac"]) for level in range(self.k["SCAN_DEPTH"] + 1)])   # (a folder's name sorts before lvl*.flac, so the deepest comes first)

    def test_the_caps(self):
        e, tr = self.k["SCAN_ENTRIES"], self.k["SCAN_TRACKS"]
        with tempfile.TemporaryDirectory() as t:
            self.touch(t, *[f"big/f{i:04d}.flac" for i in range(e + 80)], *[f"big2/g{i:04d}.flac" for i in range(tr)])
            got, n = self.scan(t)
            self.assertEqual(n, tr, "the list stops at SCAN_TRACKS")
            self.assertEqual(sum("/big/" in x for x in got), e, "no more than SCAN_ENTRIES entries are read from one folder")
            self.assertEqual(sum("/big2/" in x for x in got), tr - e)

    def test_a_loop_of_symbolic_links_ends(self):
        with tempfile.TemporaryDirectory() as t:
            self.touch(t, "a.flac", "sub/b.flac")
            os.symlink("..", Path(t) / "sub" / "up")
            got, n = self.scan(t)
            self.assertEqual([x[len(t) + 1:] for x in got], ["a.flac", "sub/b.flac", "sub/up/a.flac", "sub/up/sub/b.flac"])

    def test_a_missing_or_empty_folder_gives_nothing(self):
        with tempfile.TemporaryDirectory() as t:
            self.assertEqual(self.scan(Path(t) / "does-not-exist"), ([], 0))
            self.assertEqual(self.scan(t), ([], 0))


if __name__ == "__main__":
    unittest.main()
