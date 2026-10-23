"""Test picture data shared by the host-side libimg test (tests/host/imgtest.py) and the guest test (tests/case_media.py).

PNG files are written here by hand (zlib + struct) because Pillow cannot make every variant the format allows: 16-bit colour,
1/2/4-bit palettes, Adam7 interlacing, tRNS colour keys, every filter type, IDAT split over several chunks. expect_png()
works the expected pixels out from the PNG specification, independently of the decoder under test.
"""
import random
import struct
import zlib

from PIL import Image


def png_chunk(t, data):
    return struct.pack(">I", len(data)) + t + data + struct.pack(">I", zlib.crc32(t + data) & 0xFFFFFFFF)


def pack_row(samples, depth, channels):
    """samples: flat list for one row; returns the raw row bytes."""
    if depth == 8:
        return bytes(samples)
    if depth == 16:
        return b"".join(struct.pack(">H", s) for s in samples)
    bits = "".join(format(s, f"0{depth}b") for s in samples)
    bits += "0" * (-len(bits) % 8)
    return bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))


def filter_row(ftype, row, prev, bpp):
    out = bytearray()
    for i, v in enumerate(row):
        a = row[i - bpp] if i >= bpp else 0
        b = prev[i]
        c = prev[i - bpp] if i >= bpp else 0
        if ftype == 0:
            p = 0
        elif ftype == 1:
            p = a
        elif ftype == 2:
            p = b
        elif ftype == 3:
            p = (a + b) // 2
        else:
            pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
            p = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
        out.append((v - p) & 255)
    return bytes([ftype]) + bytes(out)


ADAM7 = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]


def make_png(w, h, ctype, depth, pix, interlace=0, plte=None, trns=None, rnd=None, bad_crc=False):
    """pix[y][x] = tuple of samples (channels per colour type). Returns the file bytes."""
    rnd = rnd or random.Random(1)
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    bpp = max(1, channels * depth // 8)
    raw = bytearray()

    def emit(rows):
        prev = bytes(len(pack_row([s for px in rows[0] for s in px], depth, channels))) if rows else b""
        for row in rows:
            r = pack_row([s for px in row for s in px], depth, channels)
            raw.extend(filter_row(rnd.randrange(5), r, prev, bpp))
            prev = r

    if not interlace:
        emit([pix[y] for y in range(h)])
    else:
        for xs, ys, dx, dy in ADAM7:
            sub = [[pix[y][x] for x in range(xs, w, dx)] for y in range(ys, h, dy)]
            if sub and sub[0]:
                emit(sub)
    comp = zlib.compress(bytes(raw), 6)
    # split the compressed data over several IDAT chunks to exercise the chunk boundaries
    cuts = sorted(rnd.sample(range(1, max(2, len(comp))), min(3, max(0, len(comp) - 1)))) if len(comp) > 8 else []
    parts, last = [], 0
    for c in cuts + [len(comp)]:
        parts.append(comp[last:c])
        last = c
    data = b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ctype, 0, 0, interlace))
    if plte:
        data += png_chunk(b"PLTE", bytes(c for rgb in plte for c in rgb))
    if trns is not None:
        data += png_chunk(b"tRNS", trns)
    data += png_chunk(b"gAMA", struct.pack(">I", 45455))
    for p in parts:
        data += png_chunk(b"IDAT", p)
    data += png_chunk(b"IEND", b"")
    if bad_crc:                                            # damage the CRC of the first IDAT chunk
        k = data.index(b"IDAT")
        n = struct.unpack(">I", data[k - 4:k])[0]
        c = k + 4 + n
        data = data[:c] + bytes([data[c] ^ 1]) + data[c + 1:]
    return data


def expect_png(w, h, ctype, depth, pix, plte=None, trns=None):
    """Reference decoding of the pixel model to RGBA bytes, written from the PNG specification."""
    out = bytearray()
    maxv = (1 << depth) - 1
    def hi(v):
        return v >> 8 if depth == 16 else v
    for y in range(h):
        for x in range(w):
            px = pix[y][x]
            if ctype == 0:
                g = px[0] * 255 // maxv if depth < 8 else hi(px[0])
                a = 0 if (trns is not None and px[0] == struct.unpack(">H", trns)[0]) else 255
                out += bytes((g, g, g, a))
            elif ctype == 2:
                a = 255
                if trns is not None and tuple(px) == struct.unpack(">HHH", trns):
                    a = 0
                out += bytes((hi(px[0]), hi(px[1]), hi(px[2]), a))
            elif ctype == 3:
                r, g, b = plte[px[0]]
                a = trns[px[0]] if trns is not None and px[0] < len(trns) else 255
                out += bytes((r, g, b, a))
            elif ctype == 4:
                g = hi(px[0])
                out += bytes((g, g, g, hi(px[1])))
            else:
                out += bytes((hi(px[0]), hi(px[1]), hi(px[2]), hi(px[3])))
    return bytes(out)


def photo(w, h, seed=3):
    """A smooth image with some texture, so JPEG chroma subsampling has something to do."""
    rnd = random.Random(seed)
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = ((x * 255 // max(1, w - 1) + rnd.randint(-6, 6)) & 255, (y * 255 // max(1, h - 1) + rnd.randint(-6, 6)) & 255,
                        (128 + 100 * ((x // 9 + y // 7) % 2) + rnd.randint(-8, 8)) & 255)
    return im


