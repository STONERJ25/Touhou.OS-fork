"""flacenc.py: a FLAC encoder in Python, written from RFC 9639 ("Free Lossless Audio Codec (FLAC)"), that makes the test streams for the decoder
(user/lib/audio/src/flac.c, tests/host/test_flac.py).

The encoder is the oracle's other half. The input PCM is what the decoder must give back, bit for bit, and the MD5 this file writes into STREAMINFO
(Python's hashlib, over the PCM) is what flac_verify must find. It is not made to compress well (though `auto` tries a handful of predictors and
keeps the smallest); it is made to reach every path of the format on purpose:

  * the four channel assignments (independent, left/side, right/side, mid/side), 1 to 8 channels, 4 to 32 bits per sample (the side channel of a
    32-bit stereo stream needs 33 bits), every sample rate code (the table, the 8-bit kHz, 16-bit Hz and 16-bit tens-of-Hz forms, and "from STREAMINFO"),
    every block size code (the table, the 8-bit and 16-bit forms), fixed and variable block size streams;
  * CONSTANT, VERBATIM, FIXED (order 0 to 4) and LPC (order 1 to 32, any precision 1 to 15 and any shift 0 to 15, or chosen by Levinson-Durbin),
    wasted bits, the Rice residual (both methods, any partition order, any parameter, escape-coded partitions of any width);
  * metadata: STREAMINFO (with fields that lie, when a test asks), SEEKTABLE (with placeholders), VORBIS_COMMENT, PADDING, APPLICATION, CUESHEET, a big
    PICTURE, an ID3v2 tag in front.

Everything is exact integer arithmetic on Python ints (numpy, when present, only speeds up sums that cannot overflow 64 bits).

    s = encode([left, right], rate=44100, bps=16, blocksize=4096, stereo="auto")
    s.data                    the whole file
    s.frames                  [Frame(start, end, first_sample, nsamples, header_len)]: where each frame is in s.data (to damage one on purpose)
"""
import hashlib
import math
import random
import struct
from dataclasses import dataclass, field

try:
    import numpy as np
except ImportError:                                    # pragma: no cover - the fallbacks below give the same bytes, only slower
    np = None


# ------------------------------------------------------------------------------------------------------------ checksums

def _tables():
    t8, t16 = [], []
    for i in range(256):
        c = i
        for _ in range(8):
            c = ((c << 1) ^ 0x07) & 0xFF if c & 0x80 else (c << 1) & 0xFF
        t8.append(c)
        c = i << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x8005) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
        t16.append(c)
    return t8, t16


_T8, _T16 = _tables()


def crc8(data, crc=0):
    """CRC-8, polynomial x^8 + x^2 + x + 1, start value 0 (the frame header checksum). crc8(b"123456789") == 0xF4 (CRC-8/SMBUS)."""
    for b in data:
        crc = _T8[crc ^ b]
    return crc


def crc16(data, crc=0):
    """CRC-16, polynomial 0x8005, start value 0, no reflection (the frame checksum). crc16(b"123456789") == 0xFEE8 (CRC-16/UMTS)."""
    for b in data:
        crc = ((crc << 8) & 0xFFFF) ^ _T16[(crc >> 8) ^ b]
    return crc


# -------------------------------------------------------------------------------------------------------------- bit writer

class BitWriter:
    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.nbits = 0

    def write(self, value, bits):
        if bits == 0:
            return
        assert 0 <= value < (1 << bits), (value, bits)
        self.acc = (self.acc << bits) | value
        self.nbits += bits
        if self.nbits >= 4096:
            self._flush()

    def _flush(self):
        nbytes = self.nbits >> 3
        if nbytes:
            rem = self.nbits & 7
            self.out += (self.acc >> rem).to_bytes(nbytes, "big")
            self.acc &= (1 << rem) - 1
            self.nbits = rem

    def write_signed(self, value, bits):
        assert -(1 << (bits - 1)) <= value < (1 << (bits - 1)), (value, bits)
        self.write(value & ((1 << bits) - 1), bits)

    def write_unary(self, q):
        """q zero bits and then a one bit."""
        self.write(1, q + 1)

    def align(self):
        self.write(0, (-self.nbits) % 8)

    def bit_length(self):
        return len(self.out) * 8 + self.nbits

    def getvalue(self):
        assert self.nbits % 8 == 0, "not byte aligned"
        self._flush()
        return bytes(self.out)


def coded_number(n):
    """The UTF-8 style number of a frame header (frame number or sample number): 1 byte up to 7 bits, up to 7 bytes for 36 bits."""
    if n < 0x80:
        return bytes([n])
    for length, limit in ((2, 1 << 11), (3, 1 << 16), (4, 1 << 21), (5, 1 << 26), (6, 1 << 31), (7, 1 << 36)):
        if n < limit:
            break
    else:
        raise ValueError(f"{n} does not fit in a frame header number")
    lead = (0xFF << (8 - length)) & 0xFF
    out = bytearray([lead | (n >> (6 * (length - 1)))])
    for i in range(length - 2, -1, -1):
        out.append(0x80 | ((n >> (6 * i)) & 0x3F))
    return bytes(out)


# ------------------------------------------------------------------------------------------------------------- predictors

def _diff(x, order):
    """The residual of the FIXED predictor of the given order: the order-th difference of x (x[order:] and on)."""
    if np is not None and len(x) > 64:
        return [int(v) for v in np.diff(np.array(x, dtype=np.int64), n=order)] if order else list(x)
    r = list(x)
    for _ in range(order):
        r = [r[i] - r[i - 1] for i in range(1, len(r))]
    return r


def lpc_residual(x, q, shift):
    """x[i] - ((sum q[j] * x[i-1-j]) >> shift) for i >= len(q), exactly as the decoder reconstructs it."""
    order = len(q)
    n = len(x)
    if np is not None and n > 64:
        a = np.array(x, dtype=np.int64)
        acc = np.zeros(n - order, dtype=np.int64)
        for j in range(order):
            acc += int(q[j]) * a[order - 1 - j:n - 1 - j]
        return [int(v) for v in (a[order:] - (acc >> shift))]
    out = []
    for i in range(order, n):
        s = 0
        for j in range(order):
            s += q[j] * x[i - 1 - j]
        out.append(x[i] - (s >> shift))
    return out


def levinson(autoc, max_order):
    """Levinson-Durbin: returns [coefficients of the best predictor of order 1, 2, ..., max_order] (floats; x[n] ~ sum a[j] * x[n-1-j])."""
    out = []
    a = []
    err = autoc[0]
    if err <= 0:
        return [[0.0] * k for k in range(1, max_order + 1)]
    for m in range(1, max_order + 1):
        acc = autoc[m] - sum(a[j] * autoc[m - 1 - j] for j in range(m - 1))
        k = acc / err if err > 0 else 0.0
        new = [a[j] - k * a[m - 2 - j] for j in range(m - 1)] + [k]
        a = new
        err *= (1.0 - k * k)
        if err <= 0:
            err = 1e-12
        out.append(list(a))
    return out


def autocorrelation(x, max_lag):
    n = len(x)
    if n == 0:
        return [0.0] * (max_lag + 1)
    w = [1.0 - ((2.0 * i - (n - 1)) / max(n - 1, 1)) ** 2 for i in range(n)]            # a Welch window
    if np is not None and n > 64:
        a = np.array(x, dtype=np.float64) * np.array(w)
        return [float(np.dot(a[:n - lag], a[lag:])) for lag in range(max_lag + 1)]
    xs = [x[i] * w[i] for i in range(n)]
    return [sum(xs[i] * xs[i + lag] for i in range(n - lag)) for lag in range(max_lag + 1)]


def quantize(coefs, precision, shift=None):
    """Integer coefficients of `precision` bits (signed) and the right shift for a predictor given as floats. When `shift` is None it is the
    largest (at most 15) that keeps every coefficient in range; a given shift is used as it is and coefficients that do not fit are clipped."""
    lim = (1 << (precision - 1)) - 1
    cmax = max((abs(c) for c in coefs), default=0.0)
    if shift is None:
        shift = 15
        while shift > 0 and cmax * (1 << shift) > lim:
            shift -= 1
    q, err = [], 0.0
    for c in coefs:                                      # error feedback keeps the sum of the rounding errors small
        v = c * (1 << shift) + err
        r = int(round(v))
        r = max(-lim - 1, min(lim, r))
        err = v - r
        q.append(r)
    return q, shift


# -------------------------------------------------------------------------------------------------------------- residual

def _fold(r):
    return (r << 1) if r >= 0 else (((-r) << 1) - 1)


def _best_k(us, kmax):
    """The Rice parameter in 0..kmax that gives the fewest bits for these folded values, and that number of bits."""
    if not us:
        return 0, 0
    mean = sum(us) / len(us)
    k0 = max(0, min(kmax, int(math.log2(mean)) if mean >= 1 else 0))
    best = None
    for k in range(max(0, k0 - 2), min(kmax, k0 + 2) + 1):
        bits = len(us) * (k + 1) + sum(u >> k for u in us)
        if best is None or bits < best[1]:
            best = (k, bits)
    return best


def _signed_width(values):
    """The fewest bits that hold every value as two's complement (0 if they are all zero)."""
    w = 0
    for v in values:
        b = (v.bit_length() + 1) if v >= 0 else ((-v - 1).bit_length() + 1)
        if v == 0:
            b = 0
        w = max(w, b)
    return w


def encode_residual(bw, res, n, order, method=0, porder="best", escape="auto", k=None):
    """Writes the coded residual of a block of n samples (res holds the n - order residual values). Returns nothing; raises ValueError if the
    stream would be invalid (a residual that does not fit 32 bits, a partition order the block size does not allow)."""
    for r in res:
        if not -(1 << 31) <= r < (1 << 31):
            raise ValueError("residual does not fit in 32 bits")
    kmax = 14 if method == 0 else 30
    esc_code = 15 if method == 0 else 31
    pbits = 4 if method == 0 else 5
    allowed = [po for po in range(16) if n % (1 << po) == 0 and (n >> po) >= order]
    if porder == "best":
        choices = []
        for po in allowed:
            choices.append((_residual_bits(res, n, order, po, method, escape, k, kmax), po))
        porder = min(choices)[1]
    elif porder not in allowed:
        raise ValueError(f"partition order {porder} is not allowed for a block of {n} with predictor order {order}")
    bw.write(method, 2)
    bw.write(porder, 4)
    parts = 1 << porder
    psize = n >> porder
    at = 0
    for p in range(parts):
        count = psize - order if p == 0 else psize
        chunk = res[at:at + count]
        at += count
        us = [_fold(r) for r in chunk]
        kk, cost = _best_k(us, kmax) if k is None else (k, len(us) * (k + 1) + sum(u >> k for u in us))
        width = _signed_width(chunk)
        esc_cost = 5 + len(chunk) * width
        use_escape = (escape == "force" and width <= 31) or (escape == "auto" and k is None and esc_cost < cost and width <= 31)
        if use_escape:
            bw.write(esc_code, pbits)
            bw.write(width, 5)
            for r in chunk:
                if width:
                    bw.write_signed(r, width)
        else:
            if kk > kmax:
                raise ValueError("Rice parameter out of range")
            bw.write(kk, pbits)
            mask = (1 << kk) - 1
            for u in us:
                q = u >> kk
                bw.write((1 << kk) | (u & mask), q + 1 + kk)
    assert at == n - order


def _residual_bits(res, n, order, porder, method, escape, k, kmax):
    parts = 1 << porder
    psize = n >> porder
    at, total = 0, 6
    pbits = 4 if method == 0 else 5
    for p in range(parts):
        count = psize - order if p == 0 else psize
        chunk = res[at:at + count]
        at += count
        us = [_fold(r) for r in chunk]
        kk, cost = _best_k(us, kmax) if k is None else (k, len(us) * (k + 1) + sum(u >> k for u in us))
        width = _signed_width(chunk)
        esc_cost = 5 + len(chunk) * width
        if (escape == "force" and width <= 31) or (escape == "auto" and k is None and esc_cost < cost and width <= 31):
            total += pbits + esc_cost
        else:
            total += pbits + cost
    return total


# ------------------------------------------------------------------------------------------------------------ subframes

def _trailing_zeros(x):
    acc = 0
    for v in x:
        acc |= v
    if acc == 0:
        return 0
    return (acc & -acc).bit_length() - 1


def _fits(x, bits):
    lo, hi = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
    return all(lo <= v <= hi for v in x)


def encode_subframe(bw, x, bps, spec):
    """Writes one subframe of the samples x (each a signed `bps`-bit value) as the spec says. spec keys: kind (auto, constant, verbatim, fixed, lpc),
    order, precision, shift, method (Rice method 0 or 1), porder ('best' or a number), escape (auto, never, force), k (a Rice parameter to force),
    wasted ('auto' or a number), coefs (explicit integer LPC coefficients)."""
    spec = dict(spec)
    kind = spec.get("kind", "auto")
    wasted = spec.get("wasted", "auto")
    if wasted == "auto":
        wasted = _trailing_zeros(x)
        if wasted >= bps:
            wasted = 0
    xs = [v >> wasted for v in x] if wasted else list(x)
    if wasted and any((v >> wasted) << wasted != v for v in x):
        raise ValueError("samples are not all multiples of 2**wasted")
    sbps = bps - wasted
    assert _fits(xs, sbps), "sample out of range for its bit depth"
    if kind == "auto":
        return _encode_best(bw, xs, bps, sbps, wasted, spec)
    return _write_subframe(bw, xs, bps, sbps, wasted, kind, spec)


def _header(bw, type_code, wasted):
    bw.write(0, 1)
    bw.write(type_code, 6)
    if wasted:
        bw.write(1, 1)
        bw.write_unary(wasted - 1)
    else:
        bw.write(0, 1)


def _write_subframe(bw, xs, bps, sbps, wasted, kind, spec):
    n = len(xs)
    if kind == "constant":
        if any(v != xs[0] for v in xs):
            raise ValueError("a CONSTANT subframe needs equal samples")
        _header(bw, 0, wasted)
        bw.write_signed(xs[0], sbps)
        return
    if kind == "verbatim":
        _header(bw, 1, wasted)
        for v in xs:
            bw.write_signed(v, sbps)
        return
    method = spec.get("method", 0)
    porder = spec.get("porder", "best")
    escape = spec.get("escape", "auto")
    k = spec.get("k")
    if kind == "fixed":
        order = spec.get("order", 2)
        if order > 4 or order > n:
            raise ValueError("fixed order")
        res = _diff(xs, order)[:n - order] if order else list(xs)
        _header(bw, 8 + order, wasted)
        for v in xs[:order]:
            bw.write_signed(v, sbps)
        encode_residual(bw, res, n, order, method, porder, escape, k)
        return
    if kind == "lpc":
        order = spec.get("order", 8)
        if not 1 <= order <= 32 or order > n:
            raise ValueError("lpc order")
        precision = spec.get("precision", 12)
        if "coefs" in spec:
            q, shift = list(spec["coefs"]), spec.get("shift", 0)
        else:
            ac = autocorrelation(xs, order)
            a = levinson(ac, order)[order - 1]
            q, shift = quantize(a, precision, spec.get("shift"))
        lim = 1 << (precision - 1)
        assert all(-lim <= c < lim for c in q), "coefficient out of range for its precision"
        res = lpc_residual(xs, q, shift)
        _header(bw, 31 + order, wasted)
        for v in xs[:order]:
            bw.write_signed(v, sbps)
        bw.write(precision - 1, 4)
        bw.write_signed(shift, 5)
        for c in q:
            bw.write_signed(c, precision)
        encode_residual(bw, res, n, order, method, porder, escape, k)
        return
    raise ValueError(f"unknown subframe kind {kind}")


def _encode_best(bw, xs, bps, sbps, wasted, spec):
    """Tries CONSTANT (if it applies), FIXED 0 to 4 and a few LPC orders, and writes the smallest. The candidates are compared with a quick
    partition order; the winner is written with the full search (or with what the spec says)."""
    n = len(xs)
    cands = []
    if n and all(v == xs[0] for v in xs):
        cands.append(("constant", {}))
    cands.append(("verbatim", {}))
    for order in range(0, min(4, n - 1) + 1):
        cands.append(("fixed", {"order": order}))
    if n > 64:
        for order in (3, 8, 12, 24):
            if order < n:
                cands.append(("lpc", {"order": order, "precision": 13}))
    best = None
    quick = {"porder": 4} if "porder" not in spec else {}
    for kind, extra in cands:
        s = dict(spec)
        s.update(extra)
        trial = BitWriter()
        try:
            try:
                _write_subframe(trial, xs, bps, sbps, wasted, kind, dict(s, **quick))
            except ValueError:
                if not quick:
                    raise
                trial = BitWriter()
                _write_subframe(trial, xs, bps, sbps, wasted, kind, s)          # a partition order of 4 does not fit this block: the full search
        except (ValueError, AssertionError):
            continue
        size = trial.bit_length()
        if best is None or size < best[0]:
            best = (size, kind, s)
    _write_subframe(bw, xs, bps, sbps, wasted, best[1], best[2])


# ----------------------------------------------------------------------------------------------------- stream and frames

RATE_CODES = {88200: 1, 176400: 2, 192000: 3, 8000: 4, 16000: 5, 22050: 6, 24000: 7, 32000: 8, 44100: 9, 48000: 10, 96000: 11}
BLOCK_CODES = {192: 1, 576: 2, 1152: 3, 2304: 4, 4608: 5, 256: 8, 512: 9, 1024: 10, 2048: 11, 4096: 12, 8192: 13, 16384: 14, 32768: 15}
BPS_CODES = {8: 1, 12: 2, 16: 4, 20: 5, 24: 6, 32: 7}
STEREO_CODES = {"left_side": 8, "right_side": 9, "mid_side": 10}


@dataclass
class Frame:
    start: int
    end: int
    first_sample: int
    nsamples: int
    header_len: int


@dataclass
class Stream:
    data: bytes
    frames: list = field(default_factory=list)
    pcm: list = field(default_factory=list)            # the input, one list of ints per channel
    rate: int = 0
    bps: int = 0
    channels: int = 0
    md5: bytes = b""
    first_frame_offset: int = 0
    streaminfo_offset: int = 0                         # where the 34 bytes of STREAMINFO start in `data`
    total_samples: int = 0

    def interleaved(self):
        """The expected output of the decoder: int32 little endian, interleaved."""
        n = self.total_samples
        out = bytearray()
        for i in range(n):
            for ch in self.pcm:
                out += struct.pack("<i", ch[i])
        return bytes(out)

    def interleaved_np(self):
        return np.array(self.pcm, dtype="<i4").T.reshape(-1).tobytes()


def pcm_md5(pcm, bps):
    """The MD5 STREAMINFO carries: samples interleaved, each as a little-endian signed integer of ceil(bps / 8) bytes."""
    nb = (bps + 7) // 8
    n = len(pcm[0]) if pcm else 0
    if np is not None and n:
        a = np.array(pcm, dtype="<i4").T.reshape(-1)
        return hashlib.md5(a.view(np.uint8).reshape(-1, 4)[:, :nb].tobytes()).digest()
    h = hashlib.md5()
    for i in range(n):
        for ch in pcm:
            h.update((ch[i] & ((1 << (8 * nb)) - 1)).to_bytes(nb, "little"))
    return h.digest()


def streaminfo_block(min_block, max_block, min_frame, max_frame, rate, channels, bps, total, md5):
    v = (rate << 44) | ((channels - 1) << 41) | ((bps - 1) << 36) | total
    return struct.pack(">HH", min_block, max_block) + min_frame.to_bytes(3, "big") + max_frame.to_bytes(3, "big") + v.to_bytes(8, "big") + md5


def metadata_header(block_type, length, last):
    return bytes([(0x80 if last else 0) | block_type]) + length.to_bytes(3, "big")


def vorbis_comment_block(vendor, comments):
    """VORBIS_COMMENT: all lengths little endian (the one little endian block of the format)."""
    v = vendor.encode()
    out = struct.pack("<I", len(v)) + v + struct.pack("<I", len(comments))
    for c in comments:
        b = c if isinstance(c, bytes) else c.encode()
        out += struct.pack("<I", len(b)) + b
    return out


def picture_block(size, mime="image/png", desc="cover", seed=1):
    """PICTURE: all fields big endian; `size` bytes of (pseudo-random) picture data."""
    rnd = random.Random(seed)
    data = bytes(rnd.getrandbits(8) for _ in range(min(size, 1 << 16))) * (size // (1 << 16) + 1)
    data = data[:size]
    m, d = mime.encode(), desc.encode()
    return struct.pack(">II", 3, len(m)) + m + struct.pack(">I", len(d)) + d + struct.pack(">IIIII", 600, 600, 24, 0, len(data)) + data


def seektable_block(points):
    """points: [(sample, offset, nsamples)]; a placeholder is (0xFFFFFFFFFFFFFFFF, 0, 0)."""
    return b"".join(struct.pack(">QQH", s, o, n) for s, o, n in points)


def encode(pcm, rate, bps, blocksize=4096, variable=False, stereo="indep", subframe=None, frame_bps="explicit", frame_rate="explicit",
           blocksize_code="auto", md5=True, total_samples=None, seek_every=None, seek_placeholders=0, vendor="flacenc.py", comments=None,
           picture_bytes=None, padding=None, application=False, cuesheet=False, streaminfo=None, id3v2=None, first_number=0, extra_blocks=None,
           min_max_frame=None, trailer=b""):
    """Encodes pcm (a list of channels, each a list of ints in the range of `bps` bits) into a FLAC stream.

    blocksize      an int (the last frame is shorter), or a list of block sizes (variable block size streams; then `variable` is implied)
    stereo         'indep', 'left_side', 'right_side', 'mid_side', 'auto' (the smallest), or a function of the frame number returning one of those
    subframe       None (auto: the smallest of constant, verbatim, fixed and LPC), a spec dict (see encode_subframe), or a function
                   (frame_number, channel, samples, bits) -> spec dict
    frame_bps      'explicit' (the 3-bit code when there is one) or 'streaminfo' (code 0)
    frame_rate     'explicit' (a table code when there is one, else the 8-bit kHz, 16-bit Hz or 16-bit tens-of-Hz form), 'streaminfo',
                   'khz' / 'hz' / 'tens' (force that extension form)
    blocksize_code 'auto' or 'ext8' / 'ext16' (force the 8-bit or 16-bit "blocksize - 1" form)
    md5            True (computed), False (all zero: not computed) or 16 bytes
    total_samples  None (the real count), or the number STREAMINFO should claim (0 = unknown)
    seek_every     put a SEEKTABLE point at the first frame at or after every multiple of this many samples; seek_placeholders adds placeholder points
    streaminfo     a dict overriding STREAMINFO fields (min_block, max_block, min_frame, max_frame, rate, channels, bps, total, md5) so that they lie
    id3v2          bytes put in front of the "fLaC" marker; trailer: bytes appended after the last frame
    first_number   the frame number (or sample number) of the first frame: a stream cut out of a longer one
    extra_blocks   [(type, payload)] more metadata blocks, in order, after the standard ones
    """
    channels = len(pcm)
    n_total = len(pcm[0]) if channels else 0
    assert all(len(c) == n_total for c in pcm)
    lo, hi = -(1 << (bps - 1)), (1 << (bps - 1)) - 1
    assert all(lo <= v <= hi for c in pcm for v in c), "a sample is out of range for its bit depth"
    if isinstance(blocksize, list):
        sizes = list(blocksize)
        variable = True
        assert sum(sizes) == n_total, "block sizes must add up to the number of samples"
    else:
        sizes = [blocksize] * (n_total // blocksize) + ([n_total % blocksize] if n_total % blocksize else [])

    frames, body = [], bytearray()
    pos = 0
    for index, size in enumerate(sizes):
        block = [c[pos:pos + size] for c in pcm]
        number = first_number + (pos if variable else index)
        mode = stereo(index) if callable(stereo) else stereo
        frame, hlen = _encode_frame(block, number, variable, rate, bps, mode, subframe, frame_bps, frame_rate, blocksize_code, index)
        frames.append(Frame(len(body), len(body) + len(frame), pos, size, hlen))
        body += frame
        pos += size

    digest = pcm_md5(pcm, bps) if md5 is True else (bytes(16) if md5 is False else bytes(md5))
    real_sizes = [f.end - f.start for f in frames]
    info = dict(min_block=min(sizes, default=0), max_block=max(sizes, default=0),
                min_frame=min(real_sizes, default=0), max_frame=max(real_sizes, default=0), rate=rate, channels=channels, bps=bps,
                total=n_total if total_samples is None else total_samples, md5=digest)
    if len(sizes) > 1 and not variable:
        info["min_block"] = info["max_block"]                      # the last frame of a fixed block size stream does not count
    if min_max_frame is not None:
        info["min_frame"], info["max_frame"] = min_max_frame
    if streaminfo:
        info.update(streaminfo)

    blocks = [(0, streaminfo_block(info["min_block"], info["max_block"], info["min_frame"], info["max_frame"], info["rate"], info["channels"],
                                   info["bps"], info["total"], info["md5"]))]
    if comments is not None:
        blocks.append((4, vorbis_comment_block(vendor, comments)))
    if padding is not None:
        blocks.append((1, bytes(padding)))
    if application:
        blocks.append((2, b"test" + bytes(range(60))))
    if cuesheet:
        blocks.append((5, bytes(random.Random(5).getrandbits(8) for _ in range(396))))
    if picture_bytes is not None:
        blocks.append((6, picture_block(picture_bytes)))
    for t, payload in (extra_blocks or []):
        blocks.append((t, payload))
    seek_index = None
    if seek_every:
        pts, next_mark = [], 0
        for f in frames:
            if f.first_sample >= next_mark:
                pts.append(((first_number if variable else 0) + f.first_sample, f.start, f.nsamples))
                next_mark = (f.first_sample // seek_every + 1) * seek_every
        pts += [(0xFFFFFFFFFFFFFFFF, 0, 0)] * seek_placeholders
        seek_index = len(blocks)
        blocks.insert(seek_index, (3, seektable_block(pts)))

    head = bytearray(id3v2 or b"")
    head += b"fLaC"
    streaminfo_offset = None
    for i, (t, payload) in enumerate(blocks):
        head += metadata_header(t, len(payload), i == len(blocks) - 1)
        if t == 0 and streaminfo_offset is None:
            streaminfo_offset = len(head)
        head += payload
    first = len(head)
    out = bytes(head) + bytes(body) + trailer
    for f in frames:
        f.start += first
        f.end += first
    return Stream(out, frames, [list(c) for c in pcm], rate, bps, channels, digest, first, streaminfo_offset, n_total)


def _encode_frame(block, number, variable, rate, bps, mode, subframe, frame_bps, frame_rate, blocksize_code, index):
    channels, n = len(block), len(block[0])
    if channels != 2 and mode != "indep":
        mode = "indep"
    if mode == "auto":
        mode = _best_stereo(block, bps, subframe, index)
    # the header
    if blocksize_code == "ext8" or (blocksize_code == "auto" and n not in BLOCK_CODES and n <= 256):
        bs_code, bs_tail = 6, bytes([n - 1])
    elif blocksize_code == "ext16" or n not in BLOCK_CODES:
        bs_code, bs_tail = 7, struct.pack(">H", n - 1)
    else:
        bs_code, bs_tail = BLOCK_CODES[n], b""
    if blocksize_code == "ext8" and n > 256:
        raise ValueError("ext8 holds at most 256 samples")
    sr_code, sr_tail = 0, b""
    if frame_rate != "streaminfo":
        if frame_rate == "explicit" and rate in RATE_CODES:
            sr_code = RATE_CODES[rate]
        elif frame_rate in ("explicit", "khz") and rate % 1000 == 0 and rate // 1000 <= 255:
            sr_code, sr_tail = 12, bytes([rate // 1000])
        elif frame_rate in ("explicit", "hz") and rate <= 65535:
            sr_code, sr_tail = 13, struct.pack(">H", rate)
        elif frame_rate in ("explicit", "tens") and rate % 10 == 0 and rate // 10 <= 65535:
            sr_code, sr_tail = 14, struct.pack(">H", rate // 10)
        elif frame_rate != "explicit":
            raise ValueError(f"{rate} Hz cannot be coded as {frame_rate}")
    sz_code = BPS_CODES.get(bps, 0) if frame_bps == "explicit" else 0
    ch_code = STEREO_CODES[mode] if mode in STEREO_CODES else channels - 1
    hdr = bytearray([0xFF, 0xF9 if variable else 0xF8, (bs_code << 4) | sr_code, (ch_code << 4) | (sz_code << 1)])
    hdr += coded_number(number) + bs_tail + sr_tail
    hdr.append(crc8(hdr))

    # the subframes
    chans = list(block)
    bits = [bps] * channels
    if mode == "left_side":
        chans = [block[0], [l - r for l, r in zip(block[0], block[1])]]
        bits = [bps, bps + 1]
    elif mode == "right_side":
        chans = [[l - r for l, r in zip(block[0], block[1])], block[1]]
        bits = [bps + 1, bps]
    elif mode == "mid_side":
        chans = [[(l + r) >> 1 for l, r in zip(block[0], block[1])], [l - r for l, r in zip(block[0], block[1])]]
        bits = [bps, bps + 1]
    bw = BitWriter()
    for c in range(channels):
        spec = subframe(index, c, chans[c], bits[c]) if callable(subframe) else (subframe or {})
        encode_subframe(bw, chans[c], bits[c], spec)
    bw.align()
    frame = bytes(hdr) + bw.getvalue()
    return frame + struct.pack(">H", crc16(frame)), len(hdr)


def _best_stereo(block, bps, subframe, index):
    """The channel assignment that gives the smallest frame, judged with a quick predictor (fixed order 2) so that the choice is cheap."""
    probe = {"kind": "fixed", "order": 2, "porder": 4}
    best = None
    for mode in ("indep", "left_side", "right_side", "mid_side"):
        try:
            size = len(_encode_frame(block, 0, False, 44100, bps, mode, probe, "explicit", "explicit", "auto", index)[0])
        except (ValueError, AssertionError):
            try:
                size = len(_encode_frame(block, 0, False, 44100, bps, mode, {"kind": "verbatim"}, "explicit", "explicit", "auto", index)[0])
            except (ValueError, AssertionError):
                continue
        if best is None or size < best[0]:
            best = (size, mode)
    return best[1]


# ---------------------------------------------------------------------------------------------------------------- signals

def tone(n, rate, freq, amp, bps, phase=0.0):
    """A sine wave: n samples at `rate`, amplitude `amp` (0..1 of full scale) quantized to `bps` bits."""
    top = (1 << (bps - 1)) - 1
    return [max(-top - 1, min(top, int(round(amp * top * math.sin(2 * math.pi * freq * i / rate + phase))))) for i in range(n)]


def noise(n, bps, seed=1, amp=1.0):
    """Uniform noise over the full range of `bps` bits (scaled by amp)."""
    rnd = random.Random(seed)
    top = int(((1 << (bps - 1)) - 1) * amp)
    return [rnd.randint(-top - 1 if amp >= 1.0 else -top, top) for _ in range(n)]


def music(n, rate, bps, seed=1, voices=3):
    """Something that predicts like music does: a few decaying partials plus a little noise."""
    rnd = random.Random(seed)
    top = (1 << (bps - 1)) - 1
    parts = [(rnd.uniform(110, 1800), rnd.uniform(0.1, 0.3), rnd.uniform(0, 6.28)) for _ in range(voices)]
    out = []
    for i in range(n):
        v = sum(a * math.sin(2 * math.pi * f * i / rate + p) for f, a, p in parts) * (0.6 + 0.4 * math.sin(2 * math.pi * 3 * i / rate))
        v += rnd.uniform(-0.002, 0.002)
        out.append(max(-top - 1, min(top, int(round(v * top)))))
    return out
