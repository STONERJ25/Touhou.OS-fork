"""A tiny baseline JPEG encoder with any sampling factors, for test pictures Pillow cannot write (4:4:0, 4:1:1, ...).

The tables are the simplest legal ones (every AC code is 8 bits long, every DC code 4 bits long), the quantiser is a gentle ramp, and
the pixels go through a plain floating point DCT.  The only job of the file is to be a valid JPEG with the requested layout: the
expected pixels come from Pillow (libjpeg-turbo) decoding the very same file.
"""
import math
import struct

ZIGZAG = [0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
          35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63]

_COS = [[math.cos((2 * x + 1) * u * math.pi / 16) for x in range(8)] for u in range(8)]
_ALPHA = [math.sqrt(0.5)] + [1.0] * 7

AC_SYMBOLS = [0x00, 0xF0] + [(r << 4) | s for r in range(16) for s in range(1, 11)]       # 162 symbols
DC_SYMBOLS = list(range(12))
AC_CODE = {sym: (i, 8) for i, sym in enumerate(AC_SYMBOLS)}                              # canonical codes, all 8 bits long
DC_CODE = {sym: (i, 4) for i, sym in enumerate(DC_SYMBOLS)}                              # all 4 bits long


def _dct(block, quant):
    """block: 64 samples (row-major, 0..255). Returns 64 quantised coefficients in zigzag order."""
    shifted = [v - 128 for v in block]
    out = [0] * 64
    for v in range(8):
        for u in range(8):
            s = 0.0
            for y in range(8):
                cy = _COS[v][y]
                row = y * 8
                for x in range(8):
                    s += shifted[row + x] * _COS[u][x] * cy
            f = 0.25 * _ALPHA[u] * _ALPHA[v] * s
            out[v * 8 + u] = int(round(f / quant[ZIGZAG.index(v * 8 + u)]))
    return [out[ZIGZAG[k]] for k in range(64)]


class _Bits:
    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def put(self, code, size):
        self.acc = (self.acc << size) | code
        self.n += size
        while self.n >= 8:
            self.n -= 8
            byte = (self.acc >> self.n) & 255
            self.out.append(byte)
            if byte == 255:
                self.out.append(0)
        self.acc &= (1 << self.n) - 1

    def finish(self):
        if self.n:
            self.put((1 << (8 - self.n)) - 1, 8 - self.n)
        return bytes(self.out)


def _category(v):
    return abs(v).bit_length()


def _put_value(bits, v, size):
    if size:
        bits.put(v if v > 0 else v + (1 << size) - 1, size)


def _encode_block(bits, coef, prev_dc):
    diff = coef[0] - prev_dc
    size = _category(diff)
    bits.put(*DC_CODE[size])
    _put_value(bits, diff, size)
    run = 0
    last = max((i for i in range(1, 64) if coef[i]), default=0)
    for k in range(1, last + 1):
        v = coef[k]
        if v == 0:
            run += 1
            continue
        while run > 15:
            bits.put(*AC_CODE[0xF0])
            run -= 16
        size = _category(v)
        bits.put(*AC_CODE[(run << 4) | size])
        _put_value(bits, v, size)
        run = 0
    if last < 63:
        bits.put(*AC_CODE[0x00])
    return coef[0]


def _rgb_to_ycc(rgb):
    r, g, b = rgb
    y = 0.299 * r + 0.587 * g + 0.114 * b
    cb = -0.168736 * r - 0.331264 * g + 0.5 * b + 128
    cr = 0.5 * r - 0.418688 * g - 0.081312 * b + 128
    return [min(255, max(0, int(round(v)))) for v in (y, cb, cr)]


def _segment(marker, payload):
    return struct.pack(">BBH", 0xFF, marker, len(payload) + 2) + payload


def encode_jpeg(image, sampling, restart_mcus=0):
    """image: a Pillow RGB or L image. sampling: [(h, v)] per component (1 entry for gray, 3 for colour). Returns the file bytes."""
    w, h = image.size
    gray = image.mode == "L"
    ncomp = 1 if gray else 3
    assert len(sampling) == ncomp
    hmax = max(s[0] for s in sampling)
    vmax = max(s[1] for s in sampling)
    mcus_x = -(-w // (8 * hmax))
    mcus_y = -(-h // (8 * vmax))
    pw, ph = mcus_x * 8 * hmax, mcus_y * 8 * vmax
    px = image.load()
    planes = [[[0] * pw for _ in range(ph)] for _ in range(ncomp)]
    for y in range(ph):
        for x in range(pw):
            p = px[min(x, w - 1), min(y, h - 1)]
            vals = [p] if gray else _rgb_to_ycc(p)
            for c in range(ncomp):
                planes[c][y][x] = vals[c]
    quant = [[2 + k // 6 + (c and 2) for k in range(64)] for c in range(2)]               # in zigzag order
    comps = []
    for c, (ch, cv) in enumerate(sampling):
        fx, fy = hmax // ch, vmax // cv
        cw, chh = mcus_x * 8 * ch, mcus_y * 8 * cv
        plane = [[0] * cw for _ in range(chh)]
        for y in range(chh):
            for x in range(cw):
                s = sum(planes[c][y * fy + dy][x * fx + dx] for dy in range(fy) for dx in range(fx))
                plane[y][x] = (s + (fx * fy) // 2) // (fx * fy)
        comps.append((ch, cv, plane, quant[1 if c else 0]))
    bits = _Bits()
    prev = [0] * ncomp
    out = bytearray()
    mcu = 0
    for my in range(mcus_y):
        for mx in range(mcus_x):
            if restart_mcus and mcu and mcu % restart_mcus == 0:
                out += bits.finish()
                out += bytes((0xFF, 0xD0 + ((mcu // restart_mcus - 1) & 7)))
                bits = _Bits()
                prev = [0] * ncomp
            for c, (ch, cv, plane, q) in enumerate(comps):
                for by in range(cv):
                    for bx in range(ch):
                        x0, y0 = (mx * ch + bx) * 8, (my * cv + by) * 8
                        block = [plane[y0 + j][x0 + i] for j in range(8) for i in range(8)]
                        prev[c] = _encode_block(bits, _dct(block, q), prev[c])
            mcu += 1
    out += bits.finish()
    data = bytearray(b"\xff\xd8")
    data += _segment(0xE0, b"JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00")
    for t in range(1 if gray else 2):
        data += _segment(0xDB, bytes([t]) + bytes(quant[t]))
    data += _segment(0xC0, struct.pack(">BHHB", 8, h, w, ncomp) + b"".join(bytes((c + 1, (sh << 4) | sv, 1 if c else 0)) for c, (sh, sv) in enumerate(sampling)))
    for tc, symbols, length in ((0, DC_SYMBOLS, 4), (1, AC_SYMBOLS, 8)):
        counts = [0] * 16
        counts[length - 1] = len(symbols)
        for th in range(1 if gray else 2):
            data += _segment(0xC4, bytes([(tc << 4) | th]) + bytes(counts) + bytes(symbols))
    if restart_mcus:
        data += _segment(0xDD, struct.pack(">H", restart_mcus))
    data += _segment(0xDA, bytes([ncomp]) + b"".join(bytes((c + 1, 0x00 if not c else 0x11)) for c in range(ncomp)) + bytes((0, 63, 0)))
    data += out + b"\xff\xd9"
    return bytes(data)
