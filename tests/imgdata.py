"""Test picture data shared by the host-side libimg test (tests/host/imgtest.py) and the guest test (tests/case_media.py).

PNG files are written here by hand (zlib + struct) because Pillow cannot make every variant the format allows: 16-bit colour,
1/2/4-bit palettes, Adam7 interlacing, tRNS colour keys, every filter type, IDAT split over several chunks. expect_png()
works the expected pixels out from the PNG specification, independently of the decoder under test.
"""
import io
import random
import struct
import zlib

from PIL import Image, ImageOps

from jpegenc import encode_jpeg


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


# ---------------------------------------------------------------------------------------------- the picture corpus

def clear_transparent(rgba):
    out = bytearray(rgba)
    for i in range(0, len(out), 4):
        if out[i + 3] == 0:
            out[i] = out[i + 1] = out[i + 2] = 0
    return bytes(out)


# ---------------------------------------------------------------------------------------------- the picture corpus

def lzw_gif(indices, min_size):
    """A deliberately simple LZW encoder: a clear code often enough that the code size never has to grow."""
    clear, eoi = 1 << min_size, (1 << min_size) + 1
    size = min_size + 1
    group = (1 << size) - (clear + 2)
    bits, nbits, out = 0, 0, bytearray()

    def put(code):
        nonlocal bits, nbits
        bits |= code << nbits
        nbits += size
        while nbits >= 8:
            out.append(bits & 255)
            bits >>= 8
            nbits -= 8

    put(clear)
    for i, v in enumerate(indices):
        if i and i % group == 0:
            put(clear)
        put(v)
    put(eoi)
    if nbits:
        out.append(bits & 255)
    blocks = bytearray()
    for i in range(0, len(out), 255):
        chunk = out[i:i + 255]
        blocks.append(len(chunk))
        blocks += chunk
    blocks.append(0)
    return bytes([min_size]) + bytes(blocks)


def make_gif(w, h, rows, palette, interlace=False, transparent=None):
    """rows[y][x] = palette index (display order)."""
    n = 2
    while n < len(palette):
        n *= 2
    min_size = max(2, n.bit_length() - 1)
    pal = list(palette) + [(0, 0, 0)] * (n - len(palette))
    order = list(range(h))
    if interlace:
        order = list(range(0, h, 8)) + list(range(4, h, 8)) + list(range(2, h, 4)) + list(range(1, h, 2))
    data = b"GIF89a" + struct.pack("<HHBBB", w, h, 0x80 | (min_size - 1), 0, 0) + bytes(c for rgb in pal for c in rgb)
    if transparent is not None:
        data += b"\x21\xf9\x04\x01\x00\x00" + bytes([transparent]) + b"\x00"
    data += b"\x2c" + struct.pack("<HHHHB", 0, 0, w, h, 0x40 if interlace else 0)
    data += lzw_gif([rows[y][x] for y in order for x in range(w)], min_size) + b"\x3b"
    return data


def pnm_variants():
    out = {}
    px = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (10, 20, 30), (40, 50, 60), (70, 80, 90)]
    out["p6.ppm"] = (b"P6\n# a comment\n3 2\n255\n" + bytes(c for p in px for c in p), px, 3, 2)
    out["p3.ppm"] = (b"P3\n3 2\n255\n" + b"\n".join(" ".join(map(str, p)).encode() for p in px) + b"\n", px, 3, 2)
    gray = [0, 64, 128, 192, 255, 7]
    out["p5.pgm"] = (b"P5\n3 2\n255\n" + bytes(gray), [(g, g, g) for g in gray], 3, 2)
    out["p2.pgm"] = (b"P2\n3 2\n255\n0 64 128\n192 255 7\n", [(g, g, g) for g in gray], 3, 2)
    bw = [1, 0, 1, 0, 1, 0]
    out["p1.pbm"] = (b"P1\n3 2\n1 0 1\n0 1 0\n", [(0, 0, 0) if v else (255, 255, 255) for v in bw], 3, 2)
    out["p4.pbm"] = (b"P4\n3 2\n" + bytes([0b10100000, 0b01000000]), [(0, 0, 0) if v else (255, 255, 255) for v in bw], 3, 2)
    wide = [(0xFFFF, 0x8000, 0x0100), (0, 0x1234, 0xFF00)]
    out["wide.ppm"] = (b"P6\n2 1\n65535\n" + b"".join(struct.pack(">HHH", *p) for p in wide), [(p[0] >> 8, p[1] >> 8, p[2] >> 8) for p in wide], 2, 1)
    return out


def build_corpus():
    """Returns (files, expect): files maps 'media/img/NAME' to bytes, expect maps NAME to (format, w, h, alpha or None, frames, crc)."""
    rnd = random.Random(21)
    files, expect = {}, {}

    def add(name, data, fmt, w, h, alpha, rgba, frames=1):
        files["media/img/" + name] = data
        expect[name] = (fmt, w, h, alpha, frames, zlib.crc32(rgba))

    def pillow(name, data, fmt, alpha=False, tweak=None):
        im = Image.open(io.BytesIO(data))
        rgba = im.convert("RGBA").tobytes()
        if tweak:
            rgba = tweak(rgba)
        add(name, data, fmt, im.size[0], im.size[1], alpha, rgba)

    # --- PNG: every colour type and depth, plain and Adam7, with tRNS where it applies (expected pixels from the specification)
    for ctype, depth in [(0, 1), (0, 2), (0, 4), (0, 8), (0, 16), (2, 8), (2, 16), (3, 1), (3, 2), (3, 4), (3, 8), (4, 8), (4, 16), (6, 8), (6, 16)]:
        channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
        for interlace in (0, 1):
            for variant in ("plain", "trns"):
                if variant == "trns" and (ctype in (4, 6) or (interlace and ctype in (2, 3) and depth != 8)):
                    continue
                w, h = (11, 6) if not interlace else (13, 9)
                maxv = (1 << depth) - 1
                pix = [[tuple(rnd.randint(0, maxv) for _ in range(channels)) for _ in range(w)] for _ in range(h)]
                plte = trns = None
                if ctype == 3:
                    n = min(1 << depth, 256) if variant == "plain" else min(1 << depth, 200)
                    plte = [tuple(rnd.randrange(256) for _ in range(3)) for _ in range(n)]
                    pix = [[(p[0] % n,) for p in row] for row in pix]
                    if variant == "trns":
                        trns = bytes(rnd.randrange(256) for _ in range(rnd.randint(1, n)))
                elif variant == "trns":
                    key = pix[h // 2][w // 2]
                    trns = struct.pack(">H", key[0]) if ctype == 0 else struct.pack(">HHH", *key)
                data = make_png(w, h, ctype, depth, pix, interlace, plte, trns, rnd)
                rgba = expect_png(w, h, ctype, depth, pix, plte, trns)
                add(f"png-c{ctype}d{depth:02d}-{'i' if interlace else 'p'}-{variant}.png", data, "PNG", w, h,
                    ctype in (4, 6) or trns is not None, rgba)
    # --- JPEG (Pillow): sampling variants, progressive, grayscale, EXIF orientation
    base = photo(33, 21, seed=5)
    for sub in (0, 1, 2):
        for prog in (False, True):
            b = io.BytesIO()
            base.save(b, "JPEG", quality=88, subsampling=sub, progressive=prog, optimize=prog)
            pillow(f"jpeg-rgb-s{sub}-{'prog' if prog else 'base'}.jpg", b.getvalue(), "JPEG")
    for prog in (False, True):
        b = io.BytesIO()
        base.convert("L").save(b, "JPEG", quality=85, progressive=prog)
        pillow(f"jpeg-gray-{'prog' if prog else 'base'}.jpg", b.getvalue(), "JPEG")
    b = io.BytesIO()
    photo(70, 50, seed=8).save(b, "JPEG", quality=60, subsampling=2)
    pillow("jpeg-70x50-q60.jpg", b.getvalue(), "JPEG")
    for name, kw in (("blocks2", dict(restart_marker_blocks=2)), ("rows1-420", dict(restart_marker_rows=1, subsampling=2)),
                     ("prog-blocks3", dict(restart_marker_blocks=3, progressive=True))):
        b = io.BytesIO()
        photo(70, 50, seed=9).save(b, "JPEG", quality=80, **kw)
        pillow(f"jpeg-restart-{name}.jpg", b.getvalue(), "JPEG")
    # sampling layouts Pillow cannot write, made with tests/jpegenc.py: 4:4:0, 4:1:1, 4:1:0, chroma that is only half as tall, mixed
    odd = photo(37, 29, seed=4)
    for name, samp, rst in (("s440", [(1, 2), (1, 1), (1, 1)], 0), ("s411", [(4, 1), (1, 1), (1, 1)], 0), ("s410", [(4, 2), (1, 1), (1, 1)], 0),
                            ("chroma-h1v2", [(2, 2), (2, 1), (2, 1)], 0), ("mixed", [(2, 2), (1, 2), (2, 1)], 0), ("s440-restart3", [(1, 2), (1, 1), (1, 1)], 3)):
        pillow(f"jpeg-hand-{name}.jpg", encode_jpeg(odd, samp, rst), "JPEG")
    # tiny pictures: the chroma plane is two columns wide or less (no fancy upsampling), one column, one row
    for w, h, sub in ((3, 17, 2), (3, 17, 1), (1, 9, 2), (5, 9, 2), (2, 2, 2), (17, 1, 2)):
        b = io.BytesIO()
        photo(w, h, seed=6).save(b, "JPEG", quality=90, subsampling=sub)
        pillow(f"jpeg-tiny-{w}x{h}-s{sub}.jpg", b.getvalue(), "JPEG")
    for orient in (3, 6, 8):
        b = io.BytesIO()
        ex = Image.Exif()
        ex[0x0112] = orient
        base.save(b, "JPEG", quality=90, exif=ex)
        data = b.getvalue()
        rot = ImageOps.exif_transpose(Image.open(io.BytesIO(data))).convert("RGBA")
        add(f"jpeg-exif{orient}.jpg", data, "JPEG", rot.size[0], rot.size[1], False, rot.tobytes())
    # --- GIF
    pal = [(0, 0, 0), (255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0), (255, 255, 255)]
    rows = [[(x * 3 + y * 5) % 6 for x in range(17)] for y in range(13)]
    for interlace in (False, True):
        for transp in (None, 2):
            data = make_gif(17, 13, rows, pal, interlace, transp)
            rgba = bytearray()
            for row in rows:
                for v in row:
                    rgba += bytes((*pal[v], 0 if v == transp else 255))
            add(f"gif-hand-{'i' if interlace else 'p'}{'-t' if transp is not None else ''}.gif", data, "GIF", 17, 13, transp is not None, clear_transparent(bytes(rgba)))
    im = Image.new("P", (16, 12))
    im.putpalette([c for i in range(256) for c in (i, (i * 5) % 256, (i * 11) % 256)])
    for y in range(12):
        for x in range(16):
            im.putpixel((x, y), (x * y) % 40)
    b = io.BytesIO()
    im.save(b, "GIF")
    pillow("gif-pillow.gif", b.getvalue(), "GIF", None, clear_transparent)
    frames = [Image.new("RGB", (24, 16), c) for c in ((255, 0, 0), (0, 255, 0), (0, 0, 255))]
    b = io.BytesIO()
    frames[0].save(b, "GIF", save_all=True, append_images=frames[1:], duration=[120, 240, 360], loop=0)
    data = b.getvalue()
    first = Image.open(io.BytesIO(data)).convert("RGBA").tobytes()
    files["media/img/gif-anim.gif"] = data
    expect["gif-anim.gif"] = ("GIF", 24, 16, None, 3, zlib.crc32(clear_transparent(first)))
    # --- BMP
    small = photo(19, 7, seed=2)
    for mode in ("RGB", "P", "1", "L", "RGBA"):
        im = small.convert(mode) if mode != "RGBA" else small.convert("RGBA")
        if mode == "P":
            im = small.convert("P", palette=Image.ADAPTIVE, colors=14)
        b = io.BytesIO()
        im.save(b, "BMP")
        data = b.getvalue()
        loaded = Image.open(io.BytesIO(data)).convert("RGBA").tobytes()
        add(f"bmp-{mode}.bmp", data, "BMP", 19, 7, False, loaded)
    bpal = [(0, 0, 0), (255, 0, 0), (0, 255, 0), (0, 0, 255)]
    rle = bytes([4, 1, 0, 0, 0, 3, 0, 1, 2, 0, 1, 3, 0, 1])
    data = struct.pack("<2sIHHI", b"BM", 14 + 40 + 16 + len(rle), 0, 0, 14 + 40 + 16) + struct.pack("<IiiHHIIiiII", 40, 4, 2, 1, 8, 1, len(rle), 2835, 2835, 4, 0)
    data += b"".join(bytes((b_, g, r, 0)) for r, g, b_ in bpal) + rle
    add("bmp-rle8.bmp", data, "BMP", 4, 2, False, b"".join(bytes((*c, 255)) for c in [bpal[0], bpal[1], bpal[2], bpal[3]] + [bpal[1]] * 4))
    rle4 = bytes([4, 0x12, 0, 0, 0, 3, 0x12, 0x30, 1, 0x30, 0, 1])      # bottom row: run 1,2,1,2; top row: absolute 1,2,3 then a run of one pixel (3)
    data = struct.pack("<2sIHHI", b"BM", 14 + 40 + 64 + len(rle4), 0, 0, 14 + 40 + 64) + struct.pack("<IiiHHIIiiII", 40, 4, 2, 1, 4, 2, len(rle4), 2835, 2835, 16, 0)
    data += b"".join(bytes((b_, g, r, 0)) for r, g, b_ in bpal + [(0, 0, 0)] * 12) + rle4
    add("bmp-rle4.bmp", data, "BMP", 4, 2, False, b"".join(bytes((*c, 255)) for c in [bpal[1], bpal[2], bpal[3], bpal[3]] + [bpal[1], bpal[2], bpal[1], bpal[2]]))
    vals = [[0xF800, 0x07E0, 0x001F], [0xFFFF, 0x0000, 0x8410]]
    body = b"".join(b"".join(struct.pack("<H", v) for v in r) + b"\0\0" for r in vals)
    data = struct.pack("<2sIHHI", b"BM", 14 + 40 + 12 + len(body), 0, 0, 14 + 40 + 12) + struct.pack("<IiiHHIIiiII", 40, 3, -2, 1, 16, 3, len(body), 2835, 2835, 0, 0) + struct.pack("<III", 0xF800, 0x07E0, 0x001F) + body
    add("bmp-565-topdown.bmp", data, "BMP", 3, 2, False,
        b"".join(bytes((((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31, 255)) for r in vals for v in r))
    # --- PNM
    for name, (data, px, w, h) in pnm_variants().items():
        add("pnm-" + name, data, "PNM", w, h, False, b"".join(bytes((*p, 255)) for p in px))
    # --- TGA
    t = photo(12, 9, seed=6)
    for mode, extra in (("RGB", {}), ("RGBA", {}), ("L", {}), ("RGB", {"compression": "tga_rle"}), ("P", {})):
        im = t.convert(mode) if mode != "P" else t.convert("P", palette=Image.ADAPTIVE, colors=20)
        if "compression" in extra:
            for i in range(0, 12 * 9, 3):
                im.putpixel((i % 12, i // 12), (10, 20, 30))
        b = io.BytesIO()
        im.save(b, "TGA", **extra)
        data = b.getvalue()
        loaded = Image.open(io.BytesIO(data)).convert("RGBA").tobytes()
        add(f"tga-{mode}{'-rle' if extra else ''}.tga", data, "TGA", 12, 9, mode == "RGBA", loaded)
    return files, expect


def mutate(rnd, data):
    b = bytearray(data)
    mode = rnd.choice(("flip", "flip", "trunc", "junk", "zero", "insert"))
    if mode == "flip":
        for _ in range(rnd.randint(1, 5)):
            b[rnd.randrange(len(b))] ^= 1 << rnd.randrange(8)
    elif mode == "trunc":
        del b[rnd.randrange(1, len(b)):]
    elif mode == "junk":
        k = rnd.randrange(len(b))
        b[k:k + rnd.randint(1, 16)] = bytes(rnd.getrandbits(8) for _ in range(rnd.randint(1, 16)))
    elif mode == "zero":
        k = rnd.randrange(len(b))
        b[k:k + rnd.randint(1, 12)] = bytes(rnd.randint(1, 12))
    else:
        k = rnd.randrange(len(b))
        b[k:k] = bytes(rnd.getrandbits(8) for _ in range(rnd.randint(1, 6)))
    return bytes(b)


