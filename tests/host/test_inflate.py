"""Host tests of the kernel's gunzip (kernel/lib/inflate.c, the code that unpacks a compressed initrd.img at boot), without QEMU.

  python tools/hosttest.py discover -s tests/host -p "test_inflate.py" -v     (on Windows this runs in WSL's Ubuntu: it needs gcc)
  python tools/hosttest.py discover -s tests/host -p "test_inflate.py" -k Fuzz

The very source file the kernel is built from is compiled for the host by tests/host/inflatetool.c with AddressSanitizer and UBSan (build/host-inflate/inflatetool);
every input is handed over in a heap block of exactly its size, so a read past the end of it is an error too. The oracle is Python's zlib: it compresses (every level,
every strategy, flushes that make many small blocks), it decodes what the C code is given, and it is run side by side with the C code on damaged streams: a damaged
stream is either refused by both or decoded to the same bytes by both. A second, independent oracle is the little deflate encoder below (BitWriter, canonical codes,
random Huffman code lengths up to 15 bits, the repeat codes 16, 17 and 18, stored/fixed/dynamic blocks mixed in one stream) which knows what its output must be;
it makes streams that zlib itself would never write, and so reaches the paths that only exotic code sets use (codes longer than the 10-bit lookup table).

What is checked, in the order of the classes below: round trips on every block type and window size; the hand-built streams; the exact refusal of malformed
streams (and the reason given); every truncation of small streams; the trailer and header rules; bits flipped one at a time and at random (never a crash, and either
an error or the right bytes); a decompression bomb against the output limit; a sink that fails; and the real initrd, if a build has left one in build/.
"""
import gzip
import os
import random
import re
import shutil
import struct
import subprocess
import time
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
OUT = ROOT / "build" / "host-inflate"
TOOL = OUT / "inflatetool"
SOURCES = [HERE / "inflatetool.c", ROOT / "kernel" / "lib" / "inflate.c"]
HEADERS = [ROOT / "kernel" / "include" / "inflate.h"]

OK, E_FORMAT, E_HCRC, E_DATA, E_TRUNC, E_CRC, E_SIZE, E_TRAILING, E_LIMIT, E_SINK, E_ARG = 0, -1, -2, -3, -4, -5, -6, -7, -8, -9, -10
STATE_BYTES, HISTORY = 16384, 32768
MIN_WORK = STATE_BYTES + HISTORY + 1024
WORKS = [MIN_WORK, MIN_WORK + 24, 65536, 131072, 1 << 20]       # the smallest work area, an odd one, and bigger ones: the result must not depend on it

ENV = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=0:exitcode=77", UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1")
HAVE_GCC = shutil.which("gcc") is not None
_built = False


def build_tool():
    """Compiles the decoder for the host with the sanitizers (once per run, and again when a source is newer than the binary)."""
    global _built
    if _built:
        return
    if not HAVE_GCC:
        raise unittest.SkipTest("no gcc (on Windows run through tools/hosttest.py, which uses WSL)")
    newest = max(p.stat().st_mtime for p in SOURCES + HEADERS)
    if not TOOL.exists() or TOOL.stat().st_mtime < newest:
        OUT.mkdir(parents=True, exist_ok=True)
        cmd = ["gcc", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-std=gnu17", "-Wall", "-Wextra", "-Werror", "-Wshadow",
               "-Wno-unused-parameter", "-iquote", str(ROOT / "kernel" / "include"), *map(str, SOURCES), "-o", str(TOOL)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError("cannot build tests/host/inflatetool.c:\n" + r.stdout + r.stderr)
    _built = True


RESULT = re.compile(r"rc=(-?\d+) in=(\d+) out=(\d+) crc=([0-9a-f]{8}) adler=([0-9a-f]{8}) calls=(\d+) why=(.*)")


class Res:
    def __init__(self, line):
        m = RESULT.fullmatch(line)
        assert m, f"unexpected line from inflatetool: {line!r}"
        self.rc, self.consumed, self.out, self.calls = int(m[1]), int(m[2]), int(m[3]), int(m[6])
        self.crc, self.adler, self.why = int(m[4], 16), int(m[5], 16), m[7]

    def __repr__(self):
        return f"Res(rc={self.rc}, in={self.consumed}, out={self.out}, calls={self.calls}, why={self.why!r})"


def _check_run(p, what):
    err = p.stderr.decode("utf-8", "replace")
    if p.returncode != 0 or "runtime error" in err or "AddressSanitizer" in err or "LeakSanitizer" in err:
        raise AssertionError(f"inflatetool {what}: status {p.returncode}\n{err[-3000:]}")


def batch(records, work=None, limit=None, timeout=600):
    """Decodes each record (bytes) in one run of the tool; returns a list of Res. A sanitizer report, a crash or an abort is a failure on its own."""
    build_tool()
    args = [str(TOOL), "batch"] + (["-w", str(work)] if work else []) + (["-l", str(limit)] if limit is not None else [])
    payload = b"".join(struct.pack("<I", len(r)) + r for r in records)
    p = subprocess.run(args, input=payload, capture_output=True, env=ENV, timeout=timeout)
    lines = p.stdout.decode().splitlines()
    if p.returncode != 0:
        OUT.mkdir(parents=True, exist_ok=True)
        bad = records[len(lines)] if len(lines) < len(records) else b""
        (OUT / "crash.gz").write_bytes(bad)
        _check_run(p, f"batch (record {len(lines)} of {len(records)} saved in {OUT / 'crash.gz'})")
    assert len(lines) == len(records), (len(lines), len(records))
    return [Res(l) for l in lines]


def run_file(path, work=None, limit=None, fail_at=None, out=None):
    build_tool()
    args = [str(TOOL), "run"] + (["-w", str(work)] if work else []) + (["-l", str(limit)] if limit is not None else []) + \
           (["-f", str(fail_at)] if fail_at else []) + (["-o", str(out)] if out else []) + [str(path)]
    p = subprocess.run(args, capture_output=True, env=ENV, timeout=600)
    _check_run(p, "run")
    return Res(p.stdout.decode().strip())


# ------------------------------------------------------------------------------------------------ making streams

def gz_header(flg=0, mtime=0, xfl=0, os_=255, extra=b"", name=b"", comment=b"", bad_hcrc=False):
    h = bytearray(b"\x1f\x8b\x08" + bytes([flg]) + struct.pack("<I", mtime) + bytes([xfl, os_]))
    if flg & 4:
        h += struct.pack("<H", len(extra)) + extra
    if flg & 8:
        h += name + b"\0"
    if flg & 16:
        h += comment + b"\0"
    if flg & 2:
        c = zlib.crc32(bytes(h)) & 0xFFFF
        h += struct.pack("<H", c ^ 1 if bad_hcrc else c)
    return bytes(h)


def gz_trailer(data):
    return struct.pack("<II", zlib.crc32(data), len(data) & 0xFFFFFFFF)


def raw_deflate(data, level=6, strategy=zlib.Z_DEFAULT_STRATEGY, flush_every=0, flush_mode=zlib.Z_SYNC_FLUSH, seed=0, memlevel=8):
    c = zlib.compressobj(level, zlib.DEFLATED, -15, memlevel, strategy)
    if not flush_every:
        return c.compress(data) + c.flush()
    rnd = random.Random(seed)
    out, pos = [], 0
    while pos < len(data):
        n = rnd.randint(1, flush_every)
        out.append(c.compress(data[pos:pos + n]))
        out.append(c.flush(flush_mode))
        pos += n
    out.append(c.flush())
    return b"".join(out)


def member(data, raw=None, **hdr):
    return gz_header(**hdr) + (raw if raw is not None else raw_deflate(data)) + gz_trailer(data)


def expect_ok(tc, res, data, what=""):
    tc.assertEqual(res.rc, OK, f"{what}: {res!r}")
    tc.assertEqual((res.out, res.crc, res.adler), (len(data), zlib.crc32(data), zlib.adler32(data)), f"{what}: the bytes are not the right ones")


def samples():
    rnd = random.Random(20261103)
    yield "empty", b""
    yield "one byte", b"x"
    yield "three bytes", b"abc"
    yield "short text", b"hello hello hello hello gensokyo"
    yield "zeros 1 MB", bytes(1 << 20)
    yield "random 100 KB", rnd.randbytes(100_000)
    yield "ramp", bytes(range(256)) * 400
    words = [b"gensokyo", b"reimu", b"marisa", b"touhou", b"kappa", b"fairy", b"shrine", b"\n", b" ", b"sakuya", b"patchouli", b"the", b"of", b"0123456789"]
    yield "words 300 KB", b"".join(rnd.choice(words) for _ in range(40_000))
    block = rnd.randbytes(HISTORY)
    yield "repeats at the farthest distance", block * 5 + block[:777]            # matches of length 258 at distance 32768, over and over
    yield "repeats one short of it", (block[:HISTORY - 1]) * 6
    # a mix of fresh bytes, runs and copies from random earlier places: all lengths, distances and run kinds
    out = bytearray()
    while len(out) < 400_000:
        k = rnd.random()
        if k < 0.3 or len(out) < 10:
            out += rnd.randbytes(rnd.randint(1, 40))
        elif k < 0.5:
            out += bytes([rnd.randrange(256)]) * rnd.randint(2, 600)
        else:
            d = rnd.randint(1, min(len(out), HISTORY + 50))
            n = rnd.randint(3, 300)
            for _ in range(n):
                out.append(out[-d])
    yield "lz mix 400 KB", bytes(out)
    src = b"".join(p.read_bytes() for p in sorted((ROOT / "kernel").rglob("*.c"))[:80])
    yield "kernel sources", src


METHODS = [("level %d" % l, dict(level=l)) for l in range(0, 10)] + \
          [("%s level %d" % (n, l), dict(level=l, strategy=s)) for n, s in (("filtered", zlib.Z_FILTERED), ("huffman only", zlib.Z_HUFFMAN_ONLY), ("rle", zlib.Z_RLE), ("fixed", zlib.Z_FIXED))
           for l in (1, 6, 9)] + \
          [("sync flushes", dict(level=6, flush_every=3000, flush_mode=zlib.Z_SYNC_FLUSH)), ("full flushes", dict(level=9, flush_every=700, flush_mode=zlib.Z_FULL_FLUSH, seed=3)),
           ("tiny blocks", dict(level=1, flush_every=40, flush_mode=zlib.Z_SYNC_FLUSH, seed=5, memlevel=1))]


class RoundTrip(unittest.TestCase):
    def test_zlib_streams_of_every_kind_at_every_window_size(self):
        """Every sample, compressed every way zlib can (all levels, all strategies, many small blocks), decodes to the same bytes at every work-area size."""
        build_tool()
        cases = []
        for sname, data in samples():
            for mname, kw in METHODS:
                cases.append((f"{sname} / {mname}", data, member(data, raw=raw_deflate(data, **kw))))
        for work in WORKS:
            results = batch([c[2] for c in cases], work=work)
            for (what, data, _), res in zip(cases, results):
                expect_ok(self, res, data, f"{what} (work area {work})")

    def test_block_types_really_occur(self):
        """Guards the test above: the streams do hold stored, fixed and dynamic blocks (read from the first block's header bits, which is what Python can tell)."""
        data = b"hello gensokyo and reimu " * 40
        kinds = set()
        for name, kw in (("stored", dict(level=0)), ("fixed", dict(level=9, strategy=zlib.Z_FIXED)), ("dynamic", dict(level=9, strategy=zlib.Z_HUFFMAN_ONLY))):
            raw = raw_deflate(data, **kw)
            kinds.add((name, (raw[0] >> 1) & 3))
        self.assertEqual(sorted(kinds), [("dynamic", 2), ("fixed", 1), ("stored", 0)])

    def test_a_piece_of_output_is_never_empty_and_stays_in_the_window(self):
        """The sink's contract (the tool aborts on a violation): non-empty pieces inside the window, at most `limit` bytes; the number of pieces follows the window size."""
        data = bytes(random.Random(1).getrandbits(8) for _ in range(300_000))
        small, big = (batch([member(data)], work=w)[0] for w in (MIN_WORK, 1 << 20))
        expect_ok(self, small, data)
        expect_ok(self, big, data)
        self.assertGreater(small.calls, big.calls)
        self.assertEqual(big.calls, 1)                       # (300 KB fits in a 1 MiB work area: one piece, at the end)


# ------------------------------------------------------------------------------------------------ a deflate encoder of our own

class BitWriter:
    def __init__(self):
        self.out, self.acc, self.n = bytearray(), 0, 0

    def bits(self, value, nbits):                            # fields are stored least significant bit first
        self.acc |= (value & ((1 << nbits) - 1)) << self.n
        self.n += nbits
        while self.n >= 8:
            self.out.append(self.acc & 0xFF)
            self.acc >>= 8
            self.n -= 8

    def huff(self, code, length):                            # Huffman codes go out with their most significant bit first
        r = 0
        for i in range(length):
            r |= ((code >> i) & 1) << (length - 1 - i)
        self.bits(r, length)

    def align(self):
        if self.n:
            self.bits(0, 8 - self.n)

    def tobytes(self):
        self.align()
        return bytes(self.out)


def canonical_codes(lengths):
    """RFC 1951 3.2.2."""
    bl_count = [0] * 16
    for l in lengths:
        bl_count[l] += 1
    bl_count[0] = 0
    code, next_code = 0, [0] * 17
    for bits in range(1, 16):
        code = (code + bl_count[bits - 1]) << 1
        next_code[bits] = code
    codes = []
    for l in lengths:
        if l:
            codes.append(next_code[l])
            next_code[l] += 1
        else:
            codes.append(0)
    return codes


def random_lengths(rnd, nsyms, nused, maxlen=15, skew=0.0, must=()):
    """Code lengths for an alphabet of nsyms symbols: a random COMPLETE prefix code over nused of them (the `must` symbols among them), no code longer than maxlen.
    skew near 1 grows a long chain (lengths 1, 2, 3 ... : the deepest codes there can be); near 0 a bushy tree."""
    assert 2 <= nused <= nsyms and nused <= 1 << maxlen
    leaves = [1, 1]                                          # (the depths of the leaves of the tree: the root has been split once)
    while len(leaves) < nused:
        deep = [i for i, d in enumerate(leaves) if d < maxlen]
        if rnd.random() < skew:
            i = max(deep, key=lambda i: leaves[i])
        else:
            i = rnd.choice(deep)
        d = leaves.pop(i)
        leaves += [d + 1, d + 1]
    rnd.shuffle(leaves)
    chosen = list(must) + rnd.sample([s for s in range(nsyms) if s not in must], nused - len(must))
    lens = [0] * nsyms
    for s, d in zip(chosen, leaves):
        lens[s] = d
    return lens


FIXED_LIT = [8] * 144 + [9] * 112 + [7] * 24 + [8] * 8
FIXED_DIST = [5] * 32
CLORDER = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15]
LBASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258]
LEXT = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0]
DBASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577]
DEXT = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13]


def rle_lengths(rnd, lens):
    """The code lengths as a list of (symbol, extra value, extra bits), using the repeat codes 16, 17 and 18 where the dice say so."""
    out, i = [], 0
    while i < len(lens):
        run = 1
        while i + run < len(lens) and lens[i + run] == lens[i]:
            run += 1
        v = lens[i]
        if v == 0 and run >= 11 and rnd.random() < 0.8:
            n = min(run, 138)
            out.append((18, n - 11, 7))
        elif v == 0 and run >= 3 and rnd.random() < 0.8:
            n = min(run, 10)
            out.append((17, n - 3, 3))
        elif v != 0 and run >= 4 and rnd.random() < 0.8:
            out.append((v, 0, 0))                           # (the first one is written out, then up to six repeats of it)
            n = min(run - 1, 6)
            out.append((16, n - 3, 2))
            n += 1
        else:
            out.append((v, 0, 0))
            n = 1
        i += n
    return out


class Encoder:
    """Writes a DEFLATE stream block by block and knows what it decodes to. Matches reach back into earlier blocks, as they do in real streams."""

    def __init__(self, rnd):
        self.rnd, self.w, self.data = rnd, BitWriter(), bytearray()

    def tokens(self, lit_lens, dist_lens, count):
        """count random symbols (literals and matches) that the given code lengths can express; returns the list and appends their effect to self.data."""
        rnd = self.rnd
        lits = [s for s in range(256) if lit_lens[s]]
        lsyms = [s for s in range(257, 286) if s < len(lit_lens) and lit_lens[s]]
        dsyms = [s for s in range(30) if s < len(dist_lens) and dist_lens[s]]
        toks = []
        for _ in range(count):
            if lsyms and dsyms and self.data and rnd.random() < 0.45:
                ls, ds = rnd.choice(lsyms), rnd.choice(dsyms)
                li, di = ls - 257, ds
                length = LBASE[li] + (rnd.randrange(1 << LEXT[li]) if LEXT[li] else 0)
                if li == 28:
                    length = 258
                dist = DBASE[di] + (rnd.randrange(1 << DEXT[di]) if DEXT[di] else 0)
                if dist > len(self.data):
                    continue
                toks.append(("m", ls, li, length, ds, dist))
                for _ in range(length):
                    self.data.append(self.data[-dist])
            elif lits:
                s = rnd.choice(lits)
                toks.append(("l", s))
                self.data.append(s)
        return toks

    def emit(self, toks, lit_lens, dist_lens):
        lc, dc, w = canonical_codes(lit_lens), canonical_codes(dist_lens), self.w
        for t in toks:
            if t[0] == "l":
                w.huff(lc[t[1]], lit_lens[t[1]])
            else:
                _, ls, li, length, ds, dist = t
                w.huff(lc[ls], lit_lens[ls])
                if LEXT[li]:
                    w.bits(length - LBASE[li], LEXT[li])
                w.huff(dc[ds], dist_lens[ds])
                if DEXT[ds]:
                    w.bits(dist - DBASE[ds], DEXT[ds])
        w.huff(lc[256], lit_lens[256])

    def stored(self, last, n):
        w = self.w
        w.bits(1 if last else 0, 1)
        w.bits(0, 2)
        w.align()
        chunk = self.rnd.randbytes(n)
        w.bits(n, 16)
        w.bits(n ^ 0xFFFF, 16)
        for b in chunk:
            w.bits(b, 8)
        self.data += chunk

    def fixed(self, last, count):
        w = self.w
        w.bits(1 if last else 0, 1)
        w.bits(1, 2)
        lit = FIXED_LIT[:]
        toks = self.tokens(lit, FIXED_DIST, count)
        self.emit(toks, lit, FIXED_DIST)

    def dynamic(self, last, count, skew=0.0, nlit=None, ndist=None, deep_only=False, no_dist=False):
        rnd, w = self.rnd, self.w
        nlit = nlit if nlit is not None else rnd.randint(2, 286)
        # literal/length alphabet: must have the end-of-block symbol, and (to have matches at all) some length symbols
        must = [256]
        lit_lens = random_lengths(rnd, 286, max(2, nlit), skew=skew, must=must)
        if no_dist:
            dist_lens = [0]
        else:
            nd = ndist if ndist is not None else rnd.randint(1, 30)
            if nd == 1:
                dist_lens = [0] * rnd.randint(1, 30)
                dist_lens[rnd.randrange(len(dist_lens))] = 1    # the one code of length 1 that is allowed to leave a gap
            else:
                dist_lens = random_lengths(rnd, 30, nd, skew=skew)
        while len(lit_lens) > 257 and lit_lens[-1] == 0:
            lit_lens.pop()
        while len(dist_lens) > 1 and dist_lens[-1] == 0:
            dist_lens.pop()
        hlit, hdist = len(lit_lens), len(dist_lens)
        seq = rle_lengths(rnd, lit_lens + dist_lens)
        used = sorted({s for s, _, _ in seq})
        cl_lens = random_lengths(rnd, 19, max(2, len(used)), maxlen=7, must=used)
        cl_codes = canonical_codes(cl_lens)
        hclen = 19
        while hclen > 4 and cl_lens[CLORDER[hclen - 1]] == 0:
            hclen -= 1
        w.bits(1 if last else 0, 1)
        w.bits(2, 2)
        w.bits(hlit - 257, 5)
        w.bits(hdist - 1, 5)
        w.bits(hclen - 4, 4)
        for i in range(hclen):
            w.bits(cl_lens[CLORDER[i]], 3)
        for s, ev, eb in seq:
            w.huff(cl_codes[s], cl_lens[s])
            if eb:
                w.bits(ev, eb)
        toks = self.tokens(lit_lens, dist_lens if not no_dist else [0], count)
        self.emit(toks, lit_lens, dist_lens if not no_dist else [0])

    def finish(self):
        return self.w.tobytes(), bytes(self.data)


def hand_built(seed, blocks=6):
    rnd = random.Random(seed)
    e = Encoder(rnd)
    for b in range(blocks):
        last = b == blocks - 1
        kind = rnd.choice(["stored", "fixed", "dynamic", "dynamic", "dynamic", "deep", "nodist"])
        count = rnd.choice([0, 1, 5, 60, 400, 3000])
        if kind == "stored":
            e.stored(last, rnd.choice([0, 1, 100, 5000]))
        elif kind == "fixed":
            e.fixed(last, count)
        elif kind == "dynamic":
            e.dynamic(last, count, skew=rnd.choice([0.0, 0.3, 0.7]))
        elif kind == "deep":
            e.dynamic(last, count, skew=1.0, nlit=rnd.choice([16, 40, 200, 286]), ndist=rnd.choice([2, 12, 30]))
        else:
            e.dynamic(last, count, no_dist=True, nlit=rnd.choice([2, 30, 150]))
    return e.finish()


class HandBuilt(unittest.TestCase):
    def test_streams_from_our_own_encoder(self):
        """Dynamic blocks with random code sets (up to 15-bit codes, the repeat codes, lone distance codes, no distance codes at all), fixed and stored blocks, mixed
        in one stream: the encoder knows the output, and zlib agrees with the encoder (that checks the encoder), and the C code agrees with both."""
        cases = []
        for seed in range(300):
            raw, data = hand_built(seed)
            self.assertEqual(zlib.decompress(raw, -15), data, f"the test's own encoder is wrong (seed {seed})")
            cases.append((seed, data, member(data, raw=raw)))
        for work in (MIN_WORK, 1 << 18):
            for (seed, data, _), res in zip(cases, batch([c[2] for c in cases], work=work)):
                expect_ok(self, res, data, f"hand-built stream, seed {seed}, work area {work}")

    def test_long_codes_are_decoded(self):
        """Codes of 11 to 15 bits leave the lookup table for the canonical decode: streams made of nothing but a caterpillar code set must still decode."""
        cases = []
        for seed in range(60):
            rnd = random.Random(1000 + seed)
            e = Encoder(rnd)
            e.dynamic(True, 5000, skew=1.0, nlit=rnd.choice([20, 100, 286]), ndist=rnd.choice([10, 30]))
            raw, data = e.finish()
            cases.append((seed, data, member(data, raw=raw)))
        for (seed, data, _), res in zip(cases, batch([c[2] for c in cases])):
            expect_ok(self, res, data, f"caterpillar code set, seed {seed}")


# ------------------------------------------------------------------------------------------------ malformed streams, and the reason given

def dyn_header(w, last, hlit, hdist, cl_lens, seq, cl_codes=None):
    """Writes the start of a dynamic block with exactly these fields (nothing is checked here: this makes the bad ones)."""
    cl_codes = cl_codes or canonical_codes(cl_lens)
    hclen = 19
    while hclen > 4 and cl_lens[CLORDER[hclen - 1]] == 0:
        hclen -= 1
    w.bits(1 if last else 0, 1)
    w.bits(2, 2)
    w.bits(hlit - 257, 5)
    w.bits(hdist - 1, 5)
    w.bits(hclen - 4, 4)
    for i in range(hclen):
        w.bits(cl_lens[CLORDER[i]], 3)
    for s, ev, eb in seq:
        w.huff(cl_codes[s], cl_lens[s])
        if eb:
            w.bits(ev, eb)


def plain_dyn(lit_lens, dist_lens, last=True, hlit=None, hdist=None):
    """The header of a dynamic block for exactly these code lengths (repeat codes used the plain way), and a writer positioned after it. Nothing is checked here:
    this is how the bad ones are made."""
    lens = lit_lens + dist_lens
    seq, i = [], 0
    while i < len(lens):
        v, run = lens[i], 1
        while i + run < len(lens) and lens[i + run] == v:
            run += 1
        if v == 0 and run >= 11:
            n = min(run, 138)
            seq.append((18, n - 11, 7))
        elif v == 0 and run >= 3:
            n = min(run, 10)
            seq.append((17, n - 3, 3))
        else:
            seq.append((v, 0, 0))
            n = 1
        i += n
    used = sorted({s for s, _, _ in seq})
    cl = random_lengths(random.Random(0), 19, max(2, len(used)), maxlen=7, must=used)
    w = BitWriter()
    dyn_header(w, last, hlit or len(lit_lens), hdist or len(dist_lens), cl, seq)
    return w


def lit_lens_with(**kw):
    """A literal/length alphabet of 257 symbols: lit_lens_with(a=2, eob=2) gives the symbol 97 and the end-of-block symbol those lengths."""
    lens = [0] * 257
    if "a" in kw:
        lens[97] = kw["a"]
    lens[256] = kw.get("eob", 0)
    return lens


def bad_streams():
    """(name, raw deflate bytes, regex that the reason must match). Each is something zlib refuses too (the tests check it)."""
    out = []

    def add(name, why, build):
        w = BitWriter()
        made = build(w)
        if isinstance(made, BitWriter):                     # (plain_dyn makes a writer of its own)
            w = made
        w.bits(0, 40)                                       # so that running out of input is not the first thing the decoder meets
        out.append((name, w.tobytes(), why))

    add("block type 3", r"invalid block type", lambda w: (w.bits(1, 1), w.bits(3, 2)))
    add("stored length check", r"stored block", lambda w: (w.bits(1, 1), w.bits(0, 2), w.align(), w.bits(5, 16), w.bits(5, 16)))
    # fixed block: 7-bit codes 0000000.. are the symbols 256.., so 0000001 is the length symbol 257 (length 3); 8-bit codes from 11000000 are 280..287
    add("a match at the very start", r"before the start", lambda w: (w.bits(1, 1), w.bits(1, 2), w.huff(0b0000001, 7), w.huff(0, 5)))
    add("symbol 286 in a fixed block", r"literal/length code \(286", lambda w: (w.bits(1, 1), w.bits(1, 2), w.huff(0b11000110, 8)))
    add("symbol 287 in a fixed block", r"literal/length code \(286", lambda w: (w.bits(1, 1), w.bits(1, 2), w.huff(0b11000111, 8)))
    add("distance symbol 30 in a fixed block", r"distance code \(30", lambda w: (w.bits(1, 1), w.bits(1, 2), w.huff(0x30 + 97, 8), w.huff(0b0000001, 7), w.huff(30, 5)))
    add("distance symbol 31 in a fixed block", r"distance code \(30", lambda w: (w.bits(1, 1), w.bits(1, 2), w.huff(0x30 + 97, 8), w.huff(0b0000001, 7), w.huff(31, 5)))
    add("HLIT 287", r"too many length or distance symbols", lambda w: (w.bits(1, 1), w.bits(2, 2), w.bits(30, 5), w.bits(0, 5), w.bits(0, 4)))
    add("HDIST 31", r"too many length or distance symbols", lambda w: (w.bits(1, 1), w.bits(2, 2), w.bits(0, 5), w.bits(30, 5), w.bits(0, 4)))

    def cl_header(cl_lens, hlit=257, hdist=1):             # a header with a code-length code of our own choosing, followed by 258 zeros
        return lambda w: dyn_header(w, True, hlit, hdist, cl_lens, [(0, 0, 0)] * (hlit + hdist))
    over, gap, none = [0] * 19, [0] * 19, [0] * 19
    over[0] = over[1] = over[2] = 1                          # three codes of one bit: over-subscribed
    gap[0] = gap[1] = 2                                      # two codes of two bits: a quarter of the code space unused
    add("code-length code over-subscribed", r"invalid code lengths set", cl_header(over))
    add("code-length code incomplete", r"invalid code lengths set", cl_header(gap))
    add("code-length code empty", r"invalid code lengths set|end-of-block", lambda w: dyn_header(w, True, 257, 1, none, [], cl_codes=[0] * 19))

    cl_full = [0] * 19                                       # complete: lengths 2 2 2 3 4 4 for the symbols 0, 1, 8, 16, 17, 18
    for sym, ln in ((0, 2), (1, 2), (8, 2), (16, 3), (17, 4), (18, 4)):
        cl_full[sym] = ln
    add("repeat 16 with nothing before it", r"bit length repeat", lambda w: dyn_header(w, True, 257, 1, cl_full, [(16, 0, 2)]))
    add("repeat runs past the end", r"bit length repeat", lambda w: dyn_header(w, True, 257, 1, cl_full, [(0, 0, 0)] * 250 + [(18, 127, 7)]))
    add("no end-of-block code", r"end-of-block", lambda w: plain_dyn(lit_lens_with(a=1), [1]))
    add("literal/length set over-subscribed", r"literal/lengths set", lambda w: plain_dyn(lit_lens_with(a=1, eob=1)[:97] + [1, 1, 1] + [0] * 156 + [1], [0]))
    add("literal/length set incomplete", r"literal/lengths set", lambda w: plain_dyn([2, 2] + [0] * 254 + [2], [0]))
    add("the lone literal/length code is two bits long", r"literal/lengths set", lambda w: plain_dyn(lit_lens_with(eob=2), [0]))
    add("distance set over-subscribed", r"distances set", lambda w: plain_dyn(lit_lens_with(a=1, eob=1), [1, 1, 1]))
    add("distance set incomplete", r"distances set", lambda w: plain_dyn(lit_lens_with(a=1, eob=1), [2, 2, 2]))
    add("the lone distance code is two bits long", r"distances set", lambda w: plain_dyn(lit_lens_with(a=1, eob=1), [2]))
    return out


def legal_edge_streams():
    """(name, raw deflate bytes, expected output): streams that use the corners DEFLATE allows."""
    out = []

    def empty_stored(w, last):
        w.bits(1 if last else 0, 1)
        w.bits(0, 2)
        w.align()
        w.bits(0, 16)
        w.bits(0xFFFF, 16)

    w = BitWriter()
    empty_stored(w, False)
    empty_stored(w, False)
    empty_stored(w, True)
    out.append(("three empty stored blocks", w.tobytes(), b""))
    w = BitWriter()
    for last in (False, False, True):
        w.bits(1 if last else 0, 1)
        w.bits(1, 2)
        w.huff(0, 7)                                         # a fixed block of nothing but the end-of-block code
    out.append(("three fixed blocks of only end-of-block", w.tobytes(), b""))
    w = plain_dyn(lit_lens_with(eob=1), [0])                 # a dynamic block whose only literal/length code is end-of-block (one bit: the allowed gap), and no distance codes
    w.huff(0, 1)
    out.append(("a literal/length code of end-of-block alone", w.tobytes(), b""))
    w = plain_dyn(lit_lens_with(a=1, eob=1), [0])            # two codes of one bit ('a' is 0, end-of-block is 1) and no distance codes
    for bit in (0, 0, 1):
        w.huff(bit, 1)
    out.append(("literals only, an empty distance code", w.tobytes(), b"aa"))
    w = plain_dyn(lit_lens_with(a=1, eob=1), [1])            # the lone distance code of length 1 (the other half of its code space is unused), never needed
    for bit in (0, 0, 1):
        w.huff(bit, 1)
    out.append(("a lone distance code of length 1", w.tobytes(), b"aa"))
    lens = [0] * 258
    lens[97], lens[98], lens[256], lens[257] = 2, 2, 2, 2   # a, b, end-of-block and the length symbol 257 (length 3): a complete code of four 2-bit codes ...
    w = plain_dyn(lens, [1, 1])                              # ... and two distance codes of one bit each: distance 1 is '0', distance 2 is '1'
    for code, n in ((0, 2), (1, 2), (3, 2), (1, 1), (2, 2)):  # 'a' 'b' then length 3 (symbol 257 is the code 11) at distance 2, then end-of-block (code 10)
        w.huff(code, n)
    out.append(("two distance codes of one bit", w.tobytes(), b"ababa"))
    return out


class Malformed(unittest.TestCase):
    def test_refused_with_the_right_reason_and_zlib_agrees(self):
        """Each hand-made bad stream: the C code says GZ_E_DATA with the expected reason, and zlib refuses it as well."""
        bad = bad_streams()
        results = batch([member(b"", raw=raw) for _, raw, _ in bad])
        for (name, raw, why), res in zip(bad, results):
            with self.assertRaises(zlib.error, msg=f"{name}: zlib accepts the stream, so the test case is wrong"):
                zlib.decompress(raw, -15)
            self.assertEqual(res.rc, E_DATA, f"{name}: {res!r}")
            self.assertRegex(res.why, why, f"{name}: {res!r}")

    def test_what_deflate_allows_is_accepted(self):
        """The edge cases that are legal: a block of only the end-of-block code, a lone distance code of length 1, no distance codes at all, a distance tree with one used
        code of length 1 and others unused, empty stored blocks in a row."""
        cases = [(name, raw, want) for name, raw, want in legal_edge_streams()]
        for seed in (1, 2, 3):                               # dynamic blocks without distance codes / with the single distance code
            rnd = random.Random(seed)
            e = Encoder(rnd)
            e.dynamic(False, 200, no_dist=True, nlit=8)
            e.dynamic(False, 200, ndist=1, nlit=30)
            e.dynamic(True, 0, nlit=2, no_dist=True)
            raw, data = e.finish()
            cases.append((f"encoder, seed {seed}", raw, data))
        for (name, raw, want), res in zip(cases, batch([member(want, raw=raw) for _, raw, want in cases])):
            self.assertEqual(zlib.decompress(raw, -15), want, f"{name}: zlib does not read this stream as the test expects, so the test case is wrong")
            expect_ok(self, res, want, name)


# ------------------------------------------------------------------------------------------------ truncation, header and trailer

def small_streams():
    rnd = random.Random(77)
    text = b"hello hello hello hello gensokyo " * 12
    yield "dynamic", member(text, raw=raw_deflate(text, 9))
    yield "fixed", member(b"hello hello", raw=raw_deflate(b"hello hello", 9, zlib.Z_FIXED))
    blob = rnd.randbytes(150)
    yield "stored", member(blob, raw=raw_deflate(blob, 0))
    yield "empty", member(b"")
    yield "with every optional header field", member(text, raw=raw_deflate(text), flg=2 | 4 | 8 | 16, extra=b"xy", name=b"initrd.tar", comment=b"c")
    raw, data = hand_built(5, blocks=4)
    yield "hand built", member(data, raw=raw)


class Truncation(unittest.TestCase):
    def test_every_prefix_of_a_small_stream_is_refused(self):
        """Cut a complete stream anywhere: the result is never a success and never a crash; a cut inside the header is a format error, a cut anywhere later GZ_E_TRUNC."""
        for name, stream in small_streams():
            res = batch([stream])[0]
            self.assertEqual(res.rc, OK, f"{name}: the complete stream: {res!r}")
            prefixes = [stream[:n] for n in range(len(stream))]
            for n, r in enumerate(batch(prefixes)):
                self.assertIn(r.rc, (E_FORMAT, E_TRUNC), f"{name} cut to {n} of {len(stream)} bytes: {r!r}")

    def test_cuts_in_a_big_stream(self):
        data = b"".join(random.Random(3).choice([b"alpha ", b"beta ", b"gamma\n", bytes(range(40))]) for _ in range(30000))
        stream = member(data)
        cuts = sorted(random.Random(4).sample(range(len(stream)), 120)) + [len(stream) - 1, len(stream) - 8, len(stream) - 9]
        for n, r in zip(cuts, batch([stream[:n] for n in cuts])):
            self.assertIn(r.rc, (E_TRUNC, E_FORMAT), f"cut at {n} of {len(stream)}: {r!r}")


class HeaderAndTrailer(unittest.TestCase):
    DATA = b"hello hello hello hello gensokyo " * 20

    def test_every_combination_of_header_flags(self):
        """FTEXT, FHCRC, FEXTRA, FNAME, FCOMMENT in all 32 combinations, with extra fields of several sizes, odd times and OS bytes: all decode."""
        cases = []
        for flg in range(32):
            for extra in (b"", b"\0", bytes(range(200)), b"\x1f\x8b" * 300):
                if not flg & 4 and extra != b"":
                    continue
                cases.append(member(self.DATA, flg=flg, extra=extra, name=b"a/b c.tar" * (1 + flg % 3), comment=b"made by hand", mtime=0x12345678 if flg & 1 else 0, xfl=flg, os_=flg * 7 % 256))
        for res in batch(cases):
            expect_ok(self, res, self.DATA)

    def test_header_faults(self):
        good = member(self.DATA)
        cases = {"empty": (b"", E_FORMAT), "magic 1": (b"\x1e" + good[1:], E_FORMAT), "magic 2": (good[:1] + b"\x8a" + good[2:], E_FORMAT),
                 "reserved flag 5": (good[:3] + b"\x20" + good[4:], E_FORMAT), "reserved flag 6": (good[:3] + b"\x40" + good[4:], E_FORMAT),
                 "reserved flag 7": (good[:3] + b"\x80" + good[4:], E_FORMAT), "FHCRC wrong": (member(self.DATA, flg=2, bad_hcrc=True), E_HCRC),
                 "name never ends": (good[:3] + b"\x08" + good[4:10] + b"initrd.tar", E_FORMAT),
                 "FEXTRA longer than the input": (good[:3] + b"\x04" + good[4:10] + struct_pack_h(60000) + b"abc", E_FORMAT),
                 "FEXTRA cut after one length byte": (good[:3] + b"\x04" + good[4:10] + b"\x01", E_FORMAT)}
        for m in range(256):
            if m != 8:
                cases[f"method {m}"] = (good[:2] + bytes([m]) + good[3:], E_FORMAT)
        names = list(cases)
        for name, res in zip(names, batch([cases[n][0] for n in names])):
            self.assertEqual(res.rc, cases[name][1], f"{name}: {res!r}")

    def test_trailer_faults(self):
        raw = raw_deflate(self.DATA)
        h = gz_header()
        crc, n = zlib.crc32(self.DATA), len(self.DATA)
        cases = {"good": (h + raw + struct_pack_ii(crc, n), OK), "crc off by one": (h + raw + struct_pack_ii(crc ^ 1, n), E_CRC),
                 "crc high bit": (h + raw + struct_pack_ii(crc ^ 0x80000000, n), E_CRC), "size off by one": (h + raw + struct_pack_ii(crc, n + 1), E_SIZE),
                 "size high bit": (h + raw + struct_pack_ii(crc, n ^ 0x80000000), E_SIZE),
                 "one byte after": (h + raw + struct_pack_ii(crc, n) + b"\0", E_TRAILING), "seven bytes after": (h + raw + struct_pack_ii(crc, n) + b"\0" * 7, E_TRAILING),
                 "garbage after": (h + raw + struct_pack_ii(crc, n) + os.urandom(100), E_TRAILING),
                 "a second member": (member(self.DATA) + member(self.DATA), E_TRAILING), "a gzip member and then zero padding": (member(self.DATA) + bytes(512), E_TRAILING),
                 "no trailer": (h + raw, E_TRUNC), "half a trailer": (h + raw + struct_pack_ii(crc, n)[:4], E_TRUNC), "seven bytes of it": (h + raw + struct_pack_ii(crc, n)[:7], E_TRUNC)}
        names = list(cases)
        for name, res in zip(names, batch([cases[n][0] for n in names])):
            self.assertEqual(res.rc, cases[name][1], f"{name}: {res!r}")

    def test_every_bit_of_the_trailer_matters(self):
        raw = raw_deflate(self.DATA)
        t = bytearray(gz_trailer(self.DATA))
        cases, want = [], []
        for bit in range(64):
            m = bytearray(t)
            m[bit // 8] ^= 1 << (bit % 8)
            cases.append(gz_header() + raw + bytes(m))
            want.append(E_CRC if bit < 32 else E_SIZE)
        for bit, (res, w) in enumerate(zip(batch(cases), want)):
            self.assertEqual(res.rc, w, f"trailer bit {bit}: {res!r}")

    def test_arguments(self):
        build_tool()
        stream = member(self.DATA)
        small = batch([stream], work=MIN_WORK - 8)[0]
        self.assertEqual(small.rc, E_ARG)
        self.assertEqual(small.out, 0)
        expect_ok(self, batch([stream], work=MIN_WORK)[0], self.DATA)


def struct_pack_h(v):
    return struct.pack("<H", v)


def struct_pack_ii(a, b):
    return struct.pack("<II", a & 0xFFFFFFFF, b & 0xFFFFFFFF)


# ------------------------------------------------------------------------------------------------ damage

def zlib_verdict(raw):
    """What zlib makes of a raw DEFLATE stream: (output, the part of raw that the stream used), or None when it refuses it."""
    d = zlib.decompressobj(-15)
    try:
        out = d.decompress(raw) + d.flush()
    except zlib.error:
        return None
    if not d.eof:
        return None
    return out, raw[:len(raw) - len(d.unused_data)]


class Fuzz(unittest.TestCase):
    def agree(self, mutated_raws, what, hdr=None):
        """Wraps each damaged raw stream in a gzip member whose trailer is right for whatever zlib decodes it to; the C code must then decode exactly the same bytes when zlib
        accepts the stream, and must refuse it when zlib does not (any error: a success on a stream zlib refuses would be a bug). Returns the number zlib accepted."""
        hdr = hdr or gz_header()
        wrapped, verdicts = [], []
        for raw in mutated_raws:
            v = zlib_verdict(raw)
            verdicts.append(v)
            wrapped.append(hdr + (v[1] + gz_trailer(v[0]) if v else raw + gz_trailer(b"")))
        accepted = 0
        for raw, v, res in zip(mutated_raws, verdicts, batch(wrapped)):
            if v:
                accepted += 1
                expect_ok(self, res, v[0], f"{what}: zlib accepts this stream ({raw[:24].hex()}...)")
            else:
                self.assertNotEqual(res.rc, OK, f"{what}: zlib refuses this stream and the C code accepts it ({raw[:24].hex()}...): {res!r}")
        return accepted

    def test_every_bit_flipped_in_small_streams(self):
        """Flip each bit of a handful of small deflate streams, one at a time: same verdict as zlib, same bytes when accepted, no crash."""
        rnd = random.Random(11)
        text = b"gensokyo reimu marisa gensokyo reimu " * 6
        raws = [raw_deflate(text, 9), raw_deflate(text, 1, zlib.Z_FIXED), raw_deflate(rnd.randbytes(80), 0), raw_deflate(bytes(range(100)) * 3, 6, zlib.Z_HUFFMAN_ONLY),
                hand_built(21, 3)[0], hand_built(22, 3)[0]]
        total = accepted = 0
        for raw in raws:
            muts = []
            for bit in range(len(raw) * 8):
                m = bytearray(raw)
                m[bit // 8] ^= 1 << (bit % 8)
                muts.append(bytes(m))
            total += len(muts)
            accepted += self.agree(muts, f"bit flips of a {len(raw)}-byte stream")
        self.assertGreater(total, 3000)
        self.assertGreater(accepted, 0)                      # (some flips leave a valid stream; if none did, the check above would test nothing about acceptance)

    def test_random_damage_to_headers_of_dynamic_blocks(self):
        """The dynamic block header is where the tables get built: damage it by random bytes and compare with zlib thousands of times."""
        rnd = random.Random(12)
        base = [hand_built(s, 2)[0] for s in range(30, 50)] + [raw_deflate(b"gensokyo " * 100, 9), raw_deflate(bytes(rnd.getrandbits(3) for _ in range(5000)), 9)]
        muts = []
        for raw in base:
            for _ in range(250):
                m = bytearray(raw)
                for _ in range(rnd.choice([1, 1, 2, 3])):
                    i = rnd.randrange(min(len(m), 40))        # (the first 40 bytes hold the block header)
                    m[i] = rnd.choice([m[i] ^ (1 << rnd.randrange(8)), rnd.randrange(256), 0, 0xFF])
                muts.append(bytes(m))
        self.agree(muts, "damaged block headers")

    def test_random_damage_anywhere_and_truncation_plus_damage(self):
        rnd = random.Random(13)
        data = b"".join(rnd.choice([b"alpha ", b"beta ", b"gamma\n", rnd.randbytes(5)]) for _ in range(4000))
        raw = raw_deflate(data, 6)
        muts = []
        for _ in range(1500):
            m = bytearray(raw)
            for _ in range(rnd.randint(1, 6)):
                m[rnd.randrange(len(m))] ^= 1 << rnd.randrange(8)
            muts.append(bytes(m[:rnd.randrange(len(m) // 2, len(m) + 1)]) if rnd.random() < 0.3 else bytes(m))
        self.agree(muts, "random damage")

    def test_damaged_streams_with_the_original_trailer(self):
        """The real situation: a stream damaged in transit, the trailer as the original had it. Either the damage is found (any error) or, if some flipped bit changes
        nothing, the output is the original data. Never a different output."""
        rnd = random.Random(14)
        data = b"".join(rnd.choice([b"alpha ", b"beta ", b"gamma\n", rnd.randbytes(5)]) for _ in range(3000))
        good = member(data)
        muts = []
        for _ in range(3000):
            m = bytearray(good)
            for _ in range(rnd.randint(1, 3)):
                m[rnd.randrange(len(m))] ^= 1 << rnd.randrange(8)
            muts.append(bytes(m))
        refused = 0
        for m, res in zip(muts, batch(muts)):
            if res.rc != OK:
                refused += 1
                self.assertIn(res.rc, (E_FORMAT, E_HCRC, E_DATA, E_TRUNC, E_CRC, E_SIZE, E_TRAILING), repr(res))
            else:
                expect_ok(self, res, data, "an accepted damaged stream")
        self.assertGreater(refused, 2900)


# ------------------------------------------------------------------------------------------------ limits and sinks

class Limits(unittest.TestCase):
    def test_the_output_limit_is_exact(self):
        data = os.urandom(70_000)
        stream = member(data)
        for lim, want in ((len(data), OK), (len(data) + 1, OK), (len(data) - 1, E_LIMIT), (0, E_LIMIT), (1, E_LIMIT), (65536, E_LIMIT)):
            res = batch([stream], limit=lim)[0]
            self.assertEqual(res.rc, want, f"limit {lim}: {res!r}")
            self.assertLessEqual(res.out, lim)                   # (the tool aborts if the sink gets more than the limit; this is the same fact from the outside)
        res = batch([member(b"")], limit=0)[0]
        self.assertEqual(res.rc, OK)

    def test_a_decompression_bomb_stops_at_the_limit(self):
        """512 MiB of zeros in about half a megabyte: with a limit of 8 MiB the decoder stops at once, it does not produce, hold or deliver more."""
        c = zlib.compressobj(9, zlib.DEFLATED, -15)
        chunk = bytes(1 << 20)
        raw = b"".join(c.compress(chunk) for _ in range(512)) + c.flush()
        crc = 0
        for _ in range(512):
            crc = zlib.crc32(chunk, crc)
        stream = gz_header() + raw + struct.pack("<II", crc, (512 << 20) & 0xFFFFFFFF)
        self.assertLess(len(stream), 1 << 20)
        t = time.time()
        res = batch([stream], limit=8 << 20)[0]
        self.assertEqual(res.rc, E_LIMIT, repr(res))
        self.assertLessEqual(res.out, 8 << 20)
        self.assertLess(time.time() - t, 20)
        self.assertLess(res.consumed, 100_000)                   # it never read the rest of the input
        # the very same stream is fine when the limit allows it (and its trailer is right: the size field is the size mod 2^32)
        res = batch([stream], limit=512 << 20)[0]
        self.assertEqual(res.rc, OK, repr(res))
        self.assertEqual((res.out, res.crc), (512 << 20, crc))

    def test_a_sink_that_fails(self):
        data = os.urandom(300_000)
        path = OUT / "sinkfail.gz"
        OUT.mkdir(parents=True, exist_ok=True)
        path.write_bytes(member(data))
        for n in (1, 2, 3):
            res = run_file(path, work=MIN_WORK, fail_at=n)
            self.assertEqual(res.rc, E_SINK, repr(res))
            self.assertEqual(res.calls, n)                       # it stopped at the failure: no further call
            self.assertLess(res.out, len(data))
        expect_ok(self, run_file(path, work=MIN_WORK), data)
        res = run_file(path, work=1 << 20, fail_at=1)            # one piece only, and it fails
        self.assertEqual((res.rc, res.out), (E_SINK, 0))


class RealInitrd(unittest.TestCase):
    def find_tar(self):
        for p in (ROOT / "build" / "initrd.tar", ROOT / "build" / "esp" / "initrd.img", ROOT / "build" / "initrd.img"):
            if p.is_file():
                d = p.read_bytes()
                if d[:2] == b"\x1f\x8b":
                    d = gzip.decompress(d)
                if len(d) > 1 << 20 and d[257:262] == b"ustar":
                    return p, d
        self.skipTest("no build/initrd.tar (python tools/build.py makes it)")

    def test_the_initrd_of_this_build(self):
        """The real thing: this build's initrd tar, compressed at levels 1 and 9, decodes byte for byte at the kernel's work-area size and at a smaller one."""
        p, tar = self.find_tar()
        OUT.mkdir(parents=True, exist_ok=True)
        for level, works in ((9, (131072, 70000)), (1, (131072,))):               # (the smallest window is tried on every sample by the class above; on 17 MB it takes 20 s)
            gz = OUT / f"initrd-{level}.gz"
            gz.write_bytes(gzip.compress(tar, level, mtime=0))
            for work in works:
                back = OUT / "initrd.back"
                back.unlink(missing_ok=True)
                t = time.time()
                res = run_file(gz, work=work, out=back)
                took = time.time() - t
                expect_ok(self, res, tar, f"{p.name} at level {level}, work area {work}")
                self.assertEqual(back.read_bytes(), tar)
                print(f"\n  {p.name}: {len(tar)} bytes, gzip level {level} -> {gz.stat().st_size} bytes ({100 * gz.stat().st_size / len(tar):.1f}%), "
                      f"work {work // 1024} KiB: {res.calls} pieces, {took:.2f} s under the sanitizers", end="")


if __name__ == "__main__":
    unittest.main()
