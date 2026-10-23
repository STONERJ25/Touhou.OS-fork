"""Media: archives (gzip, tar, zip on libz), pictures (libimg decoders and encoders, hostile files), the Gensokyo Gallery, the
wallpaper and screenshots.

The host makes the test files (Pillow, zlib, tarfile, zipfile, hand-written PNG/GIF/BMP/TGA writers in tests/imgdata.py),
puts them into a copy of the initrd (Qemu(esp=...)), the guest decodes or unpacks them, and the results are compared with
what Pillow or the format specification says. What the guest writes is pulled back out as base64 over the serial console
and read by Pillow/tarfile/zipfile on the host. The guest clock can be slow when the host is busy, so every wait is for a
result, never for a fixed time.
"""
import base64
import gzip
import io
import random
import re
import shutil
import struct
import tarfile
import time
import zipfile
import zlib

import harness
import test_shell
from guitools import Gui, near
from harness import Qemu, BUILD
from imgdata import make_png, expect_png, photo
from PIL import Image, ImageOps

MTIME = 1700000000                                   # 2023-11-14 22:13:20 UTC


def gui_test(fn):
    """A GUI test gets one more try on a fresh VM: the PS/2 mouse the rig drives can lose a byte when the guest is busy."""
    def run():
        try:
            return fn()
        except Exception as first:  # noqa: BLE001
            print(f"\n     ({fn.__name__} failed once, trying again on a fresh VM: {str(first)[:200]!r})", flush=True)
            return fn()
    run.__name__ = fn.__name__
    run.__doc__ = fn.__doc__
    return harness.test(run)


# ---------------------------------------------------------------------------------------------- helpers

def media_esp(files, name):
    """Copies build/esp to build/<name> with `files` ({path in the guest: bytes}) added to the initrd; returns the path."""
    src = BUILD / "esp"
    esp = BUILD / name
    (esp / "EFI" / "BOOT").mkdir(parents=True, exist_ok=True)
    shutil.copy2(src / "EFI" / "BOOT" / "BOOTX64.EFI", esp / "EFI" / "BOOT" / "BOOTX64.EFI")
    shutil.copy2(src / "kernel.elf", esp / "kernel.elf")
    with tarfile.open(src / "initrd.img") as old, tarfile.open(esp / "initrd.img", "w", format=tarfile.USTAR_FORMAT) as new:
        for m in old:
            new.addfile(m, old.extractfile(m) if m.isreg() else None)
        made = set()
        for path, data in sorted(files.items()):
            parts = path.split("/")
            for i in range(1, len(parts)):                      # directories first
                d = "/".join(parts[:i])
                if d not in made:
                    made.add(d)
                    ti = tarfile.TarInfo(d)
                    ti.type = tarfile.DIRTYPE
                    ti.mode = 0o755
                    ti.mtime = MTIME
                    new.addfile(ti)
            ti = tarfile.TarInfo(path)
            ti.size = len(data)
            ti.mode = 0o644
            ti.mtime = MTIME
            new.addfile(ti, io.BytesIO(data))
    return esp


_WATCHED = {}


def watch(g):
    """Remembers the Gui of the running test so that wait_for can save a screenshot when it gives up."""
    _WATCHED["g"] = g
    return g


def wait_for(fn, timeout=60, interval=0.5, what="the condition"):
    end = time.time() + timeout
    while True:
        v = fn()
        if v:
            return v
        if time.time() > end:
            g = _WATCHED.get("g")
            if g:
                try:
                    g.shot("timeout")
                except Exception:  # noqa: BLE001
                    pass
            raise AssertionError(f"timed out waiting for {what}")
        time.sleep(interval)


def fetch(vm, path, timeout=180):
    out, rc = test_shell.run(vm, f"base64 {path}", timeout=timeout)
    assert rc == 0, f"base64 {path} failed: {out[-200:]}"
    return base64.b64decode("".join(out.split()))


def area_reference(im, nw, nh):
    """The exact area average (fractional pixel coverage) of an RGB image, in pure Python: the definition libimg's IMG_AREA follows."""
    sw, sh = im.size
    px = im.load()
    out = bytearray()
    for j in range(nh):
        for i in range(nw):
            acc = [0, 0, 0]
            for y in range(sh):
                oy = min((y + 1) * nh, (j + 1) * sh) - max(y * nh, j * sh)
                if oy <= 0:
                    continue
                for x in range(sw):
                    ox = min((x + 1) * nw, (i + 1) * sw) - max(x * nw, i * sw)
                    if ox <= 0:
                        continue
                    for c in range(3):
                        acc[c] += px[x, y][c] * ox * oy
            total = sw * sh
            out += bytes((a + total // 2) // total for a in acc)
    return bytes(out)


def near_rgb(a, b, tol=2):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def crc_rgba(im):
    return zlib.crc32(im.convert("RGBA").tobytes())


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


LINE = re.compile(r"^(\S+): (\w+) (\d+)x(\d+) (alpha|opaque) (\d+) frames?, \d+ bytes, crc32 ([0-9a-f]{8})$", re.M)


# ---------------------------------------------------------------------------------------------- tests: pictures

@harness.test
def media_decoders():
    """imginfo -s decodes PNG (all colour types/depths, Adam7, tRNS), JPEG, GIF, BMP, PNM and TGA files in the guest; format, size, alpha, frame count and the pixel checksum agree with Pillow / the PNG specification."""
    files, expect = build_corpus()
    esp = media_esp(files, "esp-media-dec")
    with Qemu(cmdline="text", name="t-media-dec", esp=esp, mem="768M") as vm:
        vm.expect(test_shell.PROMPT, timeout=120)
        out, rc = test_shell.run(vm, "cd /media/img && imginfo -s *", timeout=300)
        got = {m.group(1): m.groups() for m in LINE.finditer(out)}
        missing = sorted(set(expect) - set(got))
        assert not missing, f"no result line for {missing}\n{out[-600:]}"
        bad, jpeg_retry = [], []
        for name, (fmt, w, h, alpha, frames, crc) in sorted(expect.items()):
            g = got[name]
            if (g[1], int(g[2]), int(g[3]), int(g[5])) != (fmt, w, h, frames):
                bad.append(f"{name}: {g[1:6]} but expected {(fmt, w, h, frames)}")
                continue
            if alpha is not None and (g[4] == "alpha") != alpha:
                bad.append(f"{name}: alpha flag {g[4]}, expected {'alpha' if alpha else 'opaque'}")
            if int(g[6], 16) != crc:
                (jpeg_retry if fmt == "JPEG" else bad).append(f"{name}: pixel checksum {g[6]}, expected {crc:08x}")
        # JPEG may differ from libjpeg by a rounding step: compare the pixels themselves, with a small tolerance
        for line in jpeg_retry:
            name = line.split(":")[0]
            test_shell.run(vm, f"convert /media/img/{name} /tmp/j.ppm", timeout=120)
            ours = Image.open(io.BytesIO(fetch(vm, "/tmp/j.ppm"))).convert("RGBA")
            ref = Image.open(io.BytesIO(files["media/img/" + name])).convert("RGBA")
            if name.startswith("jpeg-exif"):
                ref = ImageOps.exif_transpose(Image.open(io.BytesIO(files["media/img/" + name]))).convert("RGBA")
            worst = max(abs(a - b) for a, b in zip(ours.tobytes(), ref.tobytes()))
            if ours.size != ref.size or worst > 2:
                bad.append(f"{line} (and the pixels differ by up to {worst})")
        assert not bad, "\n".join(bad[:12])
        print(f"\n     {len(expect)} pictures decoded; {len(expect) - len(jpeg_retry)} checksums identical to the reference, {len(jpeg_retry)} JPEGs compared with a tolerance")


@harness.test
def media_encoders():
    """convert writes PNG (several levels), BMP and PPM, rotates, flips and scales; Pillow on the host reads every file and finds the right pixels."""
    src = photo(23, 17, seed=12).convert("RGBA")
    px = src.load()
    for y in range(17):
        for x in range(23):
            if (x + y) % 5 == 0:
                px[x, y] = (px[x, y][0], px[x, y][1], px[x, y][2], 90 + x)
    b = io.BytesIO()
    src.save(b, "PNG")
    opaque = photo(40, 30, seed=14)
    bo = io.BytesIO()
    opaque.save(bo, "PNG")
    esp = media_esp({"media/a.png": b.getvalue(), "media/o.png": bo.getvalue()}, "esp-media-enc")
    with Qemu(cmdline="text", name="t-media-enc", esp=esp, mem="768M") as vm:
        vm.expect(test_shell.PROMPT, timeout=120)
        cmds = [
            "convert /media/a.png /tmp/a1.png", "convert -level 0 /media/a.png /tmp/a0.png", "convert -level 9 /media/a.png /tmp/a9.png",
            "convert /media/a.png /tmp/a.bmp", "convert -background 00FF00 /media/a.png /tmp/a.ppm",
            "convert -rotate 90 /media/o.png /tmp/r90.png", "convert -rotate 270 /media/o.png /tmp/r270.png",
            "convert -flip /media/o.png /tmp/flip.png", "convert -flop /media/o.png /tmp/flop.png",
            "convert -fit 16x16 /media/o.png /tmp/fit.png", "convert -resize 25x10 /media/o.png /tmp/rs.png", "convert -scale 200 /media/o.png /tmp/up.png",
        ]
        for c in cmds:
            out, rc = test_shell.run(vm, c, timeout=120)
            assert rc == 0, f"{c} failed: {out}"
        want = src.tobytes()
        for name, mode in (("a1.png", None), ("a0.png", None), ("a9.png", None), ("a.bmp", None)):
            im = Image.open(io.BytesIO(fetch(vm, "/tmp/" + name)))
            im.load()
            assert im.convert("RGBA").tobytes() == want, f"{name}: the pixels written by convert differ from the source"
        ppm = Image.open(io.BytesIO(fetch(vm, "/tmp/a.ppm"))).convert("RGB")
        flat = Image.alpha_composite(Image.new("RGBA", src.size, (0, 255, 0, 255)), src).convert("RGB")
        worst = max(abs(a - c) for a, c in zip(ppm.tobytes(), flat.tobytes()))
        assert worst <= 1, f"PPM flattening over green is off by {worst}"
        sizes = {n: Image.open(io.BytesIO(fetch(vm, "/tmp/" + n))) for n in ("r90.png", "r270.png", "flip.png", "flop.png", "fit.png", "rs.png", "up.png")}
        o = opaque.convert("RGB")
        assert sizes["r90.png"].convert("RGB").tobytes() == o.rotate(-90, expand=True).tobytes(), "-rotate 90 is not a clockwise quarter turn"
        assert sizes["r270.png"].convert("RGB").tobytes() == o.rotate(90, expand=True).tobytes(), "-rotate 270"
        assert sizes["flip.png"].convert("RGB").tobytes() == o.transpose(Image.FLIP_TOP_BOTTOM).tobytes(), "-flip"
        assert sizes["flop.png"].convert("RGB").tobytes() == o.transpose(Image.FLIP_LEFT_RIGHT).tobytes(), "-flop"
        assert sizes["fit.png"].size == (16, 12), sizes["fit.png"].size
        assert sizes["rs.png"].size == (25, 10) and sizes["up.png"].size == (80, 60)
        ref = area_reference(o, 16, 12)
        worst = max(abs(a - c) for a, c in zip(sizes["fit.png"].convert("RGB").tobytes(), ref))
        assert worst <= 1, f"area-average shrinking (a 2.5 : 1 ratio, fractional edges) differs from the exact average by {worst}"
        out, rc = test_shell.run(vm, "ls -l /tmp/a0.png /tmp/a1.png /tmp/a9.png")
        sz = {m.group(2): int(m.group(1)) for m in re.finditer(r"\s(\d+)\s+\w+\s+\d+\s+\d+:\d+\s+(\S+)", out)}
        assert len(sz) == 3 and sz["/tmp/a9.png"] <= sz["/tmp/a1.png"] < sz["/tmp/a0.png"], f"compression levels should shrink the file: {sz}"


@harness.test
def media_hostile_files():
    """Truncated, corrupted and absurd picture files make imginfo report an error (status 1), never crash or hang the guest; nothing is logged as a fault."""
    files, _ = build_corpus()
    rnd = random.Random(31)
    names = sorted(files)
    out = {}
    for i in range(110):
        src = files[rnd.choice(names)]
        out[f"media/fuzz/f{i:03d}.bin"] = mutate(rnd, src)
    for i, name in enumerate(n for n in names if n.endswith((".png", ".jpg", ".gif", ".bmp"))):   # truncations at several points
        if i % 6 == 0:
            d = files[name]
            for cut in (1, len(d) // 3, len(d) - 1):
                out[f"media/fuzz/t{i:02d}-{cut}.bin"] = d[:cut]
    big = make_png(2, 2, 2, 8, [[(0, 0, 0)] * 2] * 2)
    out["media/fuzz/huge.png"] = big[:16] + struct.pack(">II", 0x7FFFFFFF, 0x7FFFFFFF) + big[24:]
    out["media/fuzz/empty.bin"] = b""
    out["media/fuzz/text.bin"] = b"this is not a picture\n" * 5
    esp = media_esp(out, "esp-media-fuzz")
    with Qemu(cmdline="text", name="t-media-fuzz", esp=esp, mem="768M") as vm:
        vm.expect(test_shell.PROMPT, timeout=120)
        faults_before, _ = test_shell.run(vm, "dmesg | grep -c -i -E 'boundary violation|incident|general protection'")
        text, rc = test_shell.run(vm, "for f in /media/fuzz/*; do imginfo -s $f > /dev/null 2>&1; echo \"RC $? $f\"; done", timeout=600)
        results = re.findall(r"^RC (\d+) (\S+)$", text, re.M)
        assert len(results) == len(out), f"{len(results)} results for {len(out)} files:\n{text[-500:]}"
        codes = {}
        for code, name in results:
            codes[int(code)] = codes.get(int(code), 0) + 1
            assert int(code) in (0, 1), f"imginfo ended with status {code} on {name}"
        faults_after, _ = test_shell.run(vm, "dmesg | grep -c -i -E 'boundary violation|incident|general protection'")
        assert faults_before.strip() == faults_after.strip(), "the kernel logged a fault while the pictures were decoded:\n" + test_shell.run(vm, "dmesg | tail -8")[0]
        assert codes.get(1, 0) > 20, f"expected many files to be refused: {codes}"
        print(f"\n     {len(out)} hostile files: {codes.get(0, 0)} decoded without complaint, {codes.get(1, 0)} refused, 0 crashes")


# ---------------------------------------------------------------------------------------------- tests: archives

@harness.test
def media_archives():
    """gzip, gunzip, zcat, tar and zip/unzip: archives made on the host unpack in the guest (modes, times, symlinks, long names), archives made in the guest are read by Python, and damaged archives are refused."""
    long_dir = "d" * 60 + "/" + "e" * 60
    members = {
        "top.txt": b"top level\n",
        "sub/inner.txt": b"inner file\n" * 50,
        f"{long_dir}/" + "x" * 50 + ".txt": b"long path content\n",
        "unicode-東方.txt": b"touhou\n",
        "zeros.bin": bytes(20000),
        "noise.bin": bytes(random.Random(1).getrandbits(8) for _ in range(3000)),
    }
    files = {}
    for fmt, name in ((tarfile.GNU_FORMAT, "gnu"), (tarfile.PAX_FORMAT, "pax"), (tarfile.USTAR_FORMAT, "ustar")):
        with tarfile.open(BUILD / f"h-{name}.tar", "w", format=fmt) as tf:
            d = tarfile.TarInfo("sub")
            d.type = tarfile.DIRTYPE
            d.mode = 0o750
            d.mtime = MTIME
            tf.addfile(d)
            for n, data in members.items():
                ti = tarfile.TarInfo(n)
                ti.size = len(data)
                ti.mode = 0o640
                ti.mtime = MTIME
                try:
                    tf.addfile(ti, io.BytesIO(data))
                except ValueError:
                    pass                                    # ustar cannot hold the long names
            ln = tarfile.TarInfo("sub/link")
            ln.type = tarfile.SYMTYPE
            ln.linkname = "inner.txt"
            ln.mtime = MTIME
            tf.addfile(ln)
        files[f"media/arch/{name}.tar"] = (BUILD / f"h-{name}.tar").read_bytes()
    files["media/arch/gnu.tgz"] = gzip.compress(files["media/arch/gnu.tar"], mtime=MTIME)
    zb = io.BytesIO()
    with zipfile.ZipFile(zb, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(zipfile.ZipInfo("zdir/", (2023, 11, 14, 22, 13, 20)), b"")
        for n, data in members.items():
            zi = zipfile.ZipInfo(n, (2023, 11, 14, 22, 13, 20))
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = 0o100640 << 16
            zf.writestr(zi, data)
        zi = zipfile.ZipInfo("stored.bin", (2023, 11, 14, 22, 13, 20))
        zi.compress_type = zipfile.ZIP_STORED
        zf.writestr(zi, b"stored " * 20)
    files["media/arch/host.zip"] = zb.getvalue()
    text = b"hello gensokyo\n" * 400
    files["media/arch/hello.txt.gz"] = gzip.compress(text, mtime=MTIME)
    files["media/arch/two.gz"] = gzip.compress(b"first ") + gzip.compress(b"second\n")
    bad = bytearray(files["media/arch/hello.txt.gz"])
    bad[len(bad) // 2] ^= 0x55
    files["media/arch/bad.gz"] = bytes(bad)
    files["media/arch/cut.gz"] = files["media/arch/hello.txt.gz"][:60]
    files["media/arch/cut.tar"] = files["media/arch/gnu.tar"][:900]
    files["media/arch/cut.zip"] = files["media/arch/host.zip"][:300]
    files["media/arch/hello.txt"] = text
    esp = media_esp(files, "esp-media-arch")
    with Qemu(cmdline="text", name="t-media-arch", esp=esp, mem="768M") as vm:
        vm.expect(test_shell.PROMPT, timeout=120)

        def run(cmd, timeout=120):
            return test_shell.run(vm, cmd, timeout=timeout)

        # --- host-made archives unpack correctly
        for kind in ("gnu", "pax", "ustar"):
            out, rc = run(f"rm -rf /tmp/x{kind} && mkdir /tmp/x{kind} && tar -xf /media/arch/{kind}.tar -C /tmp/x{kind} && find /tmp/x{kind} -type f | sort")
            assert rc == 0, f"tar -xf {kind}.tar failed: {out}"
            assert "top.txt" in out and "sub/inner.txt" in out and "zeros.bin" in out, out
            out, rc = run(f"cat /tmp/x{kind}/sub/inner.txt | wc -c; readlink /tmp/x{kind}/sub/link; stat /tmp/x{kind}/top.txt; stat /tmp/x{kind}/sub")
            assert "550" in out and "inner.txt" in out, out
            assert "2023-11-14 22:13:20" in out, f"modification time not restored: {out}"
            assert "0640" in out and "0750" in out, f"modes not restored: {out}"
            if kind != "ustar":
                out, rc = run(f"cat /tmp/x{kind}/{long_dir}/{'x' * 50}.txt")
                assert "long path content" in out, out
        out, rc = run("rm -rf /tmp/xz && mkdir /tmp/xz && tar -xzf /media/arch/gnu.tgz -C /tmp/xz --strip-components=0 && cmp /tmp/xz/top.txt /tmp/xgnu/top.txt && echo same")
        assert "same" in out, out
        out, rc = run("tar -tf /media/arch/gnu.tgz | wc -l; tar -tvf /media/arch/gnu.tar | head -3")
        assert re.search(r"^\d+$", out.split("\n")[0].strip()) and "drwxr-x--- 0/0" in out, out
        out, rc = run("rm -rf /tmp/xu && unzip -d /tmp/xu /media/arch/host.zip && cat /tmp/xu/top.txt && cmp /tmp/xu/zeros.bin /tmp/xgnu/zeros.bin && echo zipsame && stat /tmp/xu/sub/inner.txt && unzip -t /media/arch/host.zip | tail -1")
        assert rc == 0 and "top level" in out and "zipsame" in out, out
        assert "No errors detected" in out and "2023-11-14 22:13:20" in out, out
        out, rc = run("unzip -l /media/arch/host.zip | tail -3")
        assert "files" in out, out
        # --- gzip family
        out, rc = run("gunzip -c /media/arch/hello.txt.gz | cmp - /media/arch/hello.txt; zcat /media/arch/two.gz; gzip -l /media/arch/hello.txt.gz; gzip -t /media/arch/hello.txt.gz; echo t=$?")
        assert "first second" in out and "t=0" in out and str(len(text)) in out, out
        out, rc = run("cp /media/arch/hello.txt /tmp/h.txt && gzip -k -9 /tmp/h.txt && ls /tmp/h.txt /tmp/h.txt.gz && rm /tmp/h.txt && gunzip /tmp/h.txt.gz && cmp /tmp/h.txt /media/arch/hello.txt && echo roundtrip")
        assert "roundtrip" in out, out
        for bad_gz, what in (("bad.gz", "corrupt"), ("cut.gz", "truncated")):
            out, rc = run(f"gzip -t /media/arch/{bad_gz}; echo st=$?; zcat /media/arch/{bad_gz} > /dev/null; echo zc=$?")
            assert "st=1" in out and "zc=1" in out, f"{what} gzip should be refused: {out}"
        out, rc = run("tar -tf /media/arch/cut.tar > /dev/null; echo tar=$?; unzip -t /media/arch/cut.zip; echo zip=$?; echo notazip > /tmp/n.zip; unzip -t /tmp/n.zip; echo nz=$?")
        assert "tar=2" in out and "zip=" in out and "zip=0" not in out and "nz=" in out and "nz=0" not in out, out
        # --- archives made in the guest are read by Python
        out, rc = run("rm -rf /tmp/src && mkdir -p /tmp/src/a/b && echo hello > /tmp/src/a/hello.txt && echo deep > /tmp/src/a/b/deep.txt && ln -s hello.txt /tmp/src/a/link && chmod 750 /tmp/src/a/b "
                      "&& head -c 5000 /dev/zero > /tmp/src/zeros.bin && cp /media/arch/hello.txt /tmp/src/big.txt && cd /tmp && tar -czf g.tgz src && tar -cf g.tar src && zip -rq g.zip src && gzip -c /media/arch/hello.txt > g.gz && echo made")
        assert "made" in out, out
        tgz = tarfile.open(fileobj=io.BytesIO(fetch(vm, "/tmp/g.tgz")))
        names = {m.name: m for m in tgz.getmembers()}
        assert {"src/a/hello.txt", "src/a/b/deep.txt", "src/a/link", "src/zeros.bin", "src/big.txt"} <= set(names), sorted(names)
        assert names["src/a/link"].issym() and names["src/a/link"].linkname == "hello.txt"
        assert names["src/a/b"].mode & 0o777 == 0o750
        assert tgz.extractfile("src/big.txt").read() == text and tgz.extractfile("src/a/b/deep.txt").read() == b"deep\n"
        gt = tarfile.open(fileobj=io.BytesIO(fetch(vm, "/tmp/g.tar")))
        assert sorted(gt.getnames()) == sorted(tgz.getnames())
        z = zipfile.ZipFile(io.BytesIO(fetch(vm, "/tmp/g.zip")))
        assert z.testzip() is None
        assert z.read("src/big.txt") == text and z.read("src/zeros.bin") == bytes(5000) and z.read("src/a/hello.txt") == b"hello\n"
        info = {i.filename: i for i in z.infolist()}
        assert info["src/big.txt"].compress_type == zipfile.ZIP_DEFLATED and info["src/big.txt"].file_size > info["src/big.txt"].compress_size * 3
        assert (info["src/a/link"].external_attr >> 16) & 0o170000 == 0o120000, "symlinks are stored as links"
        assert gzip.decompress(fetch(vm, "/tmp/g.gz")) == text
        print(f"\n     tar/zip/gzip: host archives unpacked, guest archives verified by Python ({len(tgz.getnames())} tar members, {len(z.namelist())} zip entries)")


@harness.test
def media_thumbnails():
    """k_image_thumbnail (libkappa): scaled to fit, never enlarged, transparency flattened onto paper, cached, and a file that is not a picture is refused."""
    big = photo(100, 50, seed=3)
    b = io.BytesIO()
    big.save(b, "PNG")
    jb = io.BytesIO()
    photo(64, 64, seed=4).save(jb, "JPEG", quality=90)
    small = io.BytesIO()
    Image.new("RGB", (20, 10), (200, 30, 40)).save(small, "PNG")
    rgba = Image.new("RGBA", (40, 40), (255, 0, 0, 0))
    for x in range(20):
        for y in range(40):
            rgba.putpixel((x, y), (0, 0, 255, 255))
    ab = io.BytesIO()
    rgba.save(ab, "PNG")
    files = {"media/t/big.png": b.getvalue(), "media/t/photo.jpg": jb.getvalue(), "media/t/small.png": small.getvalue(),
             "media/t/alpha.png": ab.getvalue(), "media/t/notes.txt": b"just text\n"}
    esp = media_esp(files, "esp-media-thumb")
    with Qemu(cmdline="text", name="t-media-thumb", esp=esp, mem="768M") as vm:
        vm.expect(test_shell.PROMPT, timeout=120)
        run = lambda c: test_shell.run(vm, c, timeout=120)  # noqa: E731
        out, rc = run("gallery --thumb 32 /media/t/big.png /tmp/big.ppm")
        assert rc == 0 and "thumbnail /media/t/big.png: 32x16 cached=1" in out, out
        got = Image.open(io.BytesIO(fetch(vm, "/tmp/big.ppm"))).convert("RGB")
        assert got.size == (32, 16)
        # 100 x 50 shrunk to 32 x 16 (an area average with fractional edges); the reference is computed from the definition
        ref = area_reference(big, 32, 16)
        worst = max(abs(a - c) for a, c in zip(got.tobytes(), ref))
        assert worst <= 1, f"thumbnail pixels differ from the exact area average by {worst}"
        out, rc = run("gallery --thumb 32 /media/t/photo.jpg")
        assert "32x32 cached=1" in out, out
        out, rc = run("gallery --thumb 32 /media/t/small.png")
        assert "20x10" in out, f"a small picture must not be enlarged: {out}"
        out, rc = run("gallery --thumb 24 /media/t/alpha.png /tmp/alpha.ppm")
        assert "24x24" in out, out
        a = Image.open(io.BytesIO(fetch(vm, "/tmp/alpha.ppm"))).convert("RGB")
        assert near_rgb(a.getpixel((2, 12)), (0, 0, 255)) and near_rgb(a.getpixel((21, 12)), (0xEC, 0xE6, 0xD6)), "transparent pixels become the paper colour"
        out, rc = run("gallery --thumb 32 /media/t/notes.txt; echo st=$?")
        assert "none" in out and "st=1" in out, out
        out, rc = run("gallery --thumb 32 /media/t/missing.png; echo st=$?")
        assert "none" in out and "st=1" in out, out


# ---------------------------------------------------------------------------------------------- tests: the desktop programs

def solid(w, h, color, mark=None):
    im = Image.new("RGB", (w, h), color)
    if mark:
        im.paste(mark[0], mark[1])
    b = io.BytesIO()
    im.save(b, "PNG")
    return b.getvalue()


RED, GREEN, BLUE, YELLOW = (230, 20, 30), (20, 200, 40), (30, 40, 230), (240, 220, 20)
VIEW_BG = (0x14, 0x18, 0x2C)       # the Gallery's picture area
DESK = (30, 520)                   # a spot of bare desktop: left of every window (they open at x >= 40), below the icons, above the panel


class View:
    """Finds the Gallery's picture area on the screen (a new window opens a little further down and right than the last one,
    so its position is looked up from the pixels, not assumed) and reads pixels relative to its middle."""

    def __init__(self, g):
        self.g = g
        self.cx = self.cy = None

    def locate(self, timeout=120):
        def look():
            w, h, rgb = self.g.screen()
            minx = miny = 10 ** 6
            maxx = maxy = -1
            for y in range(0, h, 4):
                row = y * w * 3
                for x in range(0, w, 4):
                    i = row + x * 3
                    if abs(rgb[i] - VIEW_BG[0]) <= 2 and abs(rgb[i + 1] - VIEW_BG[1]) <= 2 and abs(rgb[i + 2] - VIEW_BG[2]) <= 2:
                        minx, maxx, miny, maxy = min(minx, x), max(maxx, x), min(miny, y), max(maxy, y)
            if maxx - minx >= 500 and maxy - miny >= 300:
                self.cx, self.cy = (minx + maxx) // 2, (miny + maxy) // 2
                return True
            return False
        wait_for(look, timeout, what="the Gallery window")

    def shot(self):
        return self.g.screen()

    def has(self, shot, dx, dy, rgb, tol=12):
        return near(self.g.pixel(self.cx + dx, self.cy + dy, shot), rgb, tol)

    def centre_is(self, rgb, tol=12):
        return self.has(self.g.screen(), 0, 0, rgb, tol)

    def wait_centre(self, rgb, what, timeout=90):
        wait_for(lambda: self.centre_is(rgb), timeout, what=what)


def launch(g, args):
    g.sh("pkill gallery; true")
    time.sleep(0.5)
    g.vm.send(f"gallery {args} &\n")
    v = View(g)
    v.locate()
    return v


@gui_test
def media_gallery():
    """The Gallery opens a picture, walks the folder with the arrow keys, zooms, rotates, flips, plays a GIF and a slideshow, and sets the wallpaper."""
    files = {
        "media/pics/01-red.png": solid(300, 200, RED),
        "media/pics/02-green.png": solid(300, 200, GREEN),
        "media/pics/10-blue.png": solid(300, 200, BLUE),
        "media/pics/notes.txt": b"not a picture\n",
        "media/wide.png": solid(240, 80, RED, (BLUE, (120, 0, 240, 80))),         # left half red, right half blue
        "media/wall.png": solid(64, 64, (10, 90, 200)),
    }
    frames = [Image.new("RGB", (120, 80), c) for c in (RED, GREEN, BLUE)]
    b = io.BytesIO()
    frames[0].save(b, "GIF", save_all=True, append_images=frames[1:], duration=[200, 200, 200], loop=0)
    files["media/anim.gif"] = b.getvalue()
    files["media/bad.png"] = solid(50, 50, RED)[:90]
    esp = media_esp(files, "esp-media-gal")
    with Qemu(cmdline="text", name="t-media-gal", esp=esp, mem="768M") as vm:
        g = watch(Gui(vm))
        g.start_desktop()
        v = launch(g, "--trace /media/pics/01-red.png > /tmp/gallery.log 2>&1")
        v.wait_centre(RED, "the first picture on the screen")
        shot = v.shot()
        # the picture is 300 x 200 at 100% in the middle of the view: its corners are red, the margin dark
        assert v.has(shot, -140, -90, RED) and v.has(shot, 140, 90, RED) and not v.has(shot, -200, 0, RED, 40), "the picture should sit in the middle at 100%"
        g.shot("gallery-red")
        # --- folder navigation: the folder holds three pictures; numbers sort as numbers (10 after 02), the text file is ignored
        g.key("right")
        v.wait_centre(GREEN, "the second picture")
        g.key("right")
        v.wait_centre(BLUE, "the third picture")
        g.key("right")
        v.wait_centre(RED, "wrapping around to the first picture")
        g.key("left")
        v.wait_centre(BLUE, "the previous picture (wrapping)")
        g.key("home")
        v.wait_centre(RED, "Home")
        g.key("end")
        v.wait_centre(BLUE, "End")
        log = g.sh("cat /tmp/gallery.log")
        assert "open /media/pics/01-red.png PNG 300x200" in log and "open /media/pics/02-green.png" in log and "open /media/pics/10-blue.png" in log, log
        # --- zoom: 100% shows the 300 wide picture; "2" shows it at 200%, so the span doubles; 0 fits again
        g.key("home")
        v.wait_centre(RED, "back to red")

        def red_at(dx):
            return v.has(v.shot(), dx, 0, RED, 20)

        assert red_at(140) and not red_at(190), "at 100% the picture ends 150 pixels from the middle"
        g.key("2")
        wait_for(lambda: red_at(190), 30, what="200% zoom")
        assert not red_at(310)
        g.key("0")
        wait_for(lambda: not red_at(190), 30, what="fit")
        # --- rotate and flip on an asymmetric picture
        v = launch(g, "--trace /media/wide.png > /tmp/gallery2.log 2>&1")
        L, R, T, B = (-100, 0), (100, 0), (0, -100), (0, 100)

        def colours(*spec):
            sh = v.shot()
            return all(v.has(sh, dx, dy, c) for (dx, dy), c in spec)

        wait_for(lambda: colours((L, RED), (R, BLUE)), 120, what="the wide picture (red left, blue right)")
        g.key("h")                                                    # flip horizontally: blue left, red right
        wait_for(lambda: colours((L, BLUE), (R, RED)), 30, what="horizontal flip")
        g.key("h")
        wait_for(lambda: colours((L, RED), (R, BLUE)), 30, what="flipped back")
        g.key("r")                                                    # rotate right: red ends up on top
        wait_for(lambda: colours((T, RED), (B, BLUE)), 30, what="rotation to the right")
        g.key("l")
        g.key("l")                                                    # two left turns from "right" = left: red at the bottom
        wait_for(lambda: colours((B, RED), (T, BLUE)), 30, what="rotation to the left")
        g.key("v")                                                    # a vertical flip on the screen: red goes to the top again
        wait_for(lambda: colours((T, RED), (B, BLUE)), 30, what="vertical flip")
        g.shot("gallery-rotated")
        # --- a broken file is explained, not fatal
        g.sh("pkill gallery; true")
        vm.send("gallery --trace /media/bad.png > /tmp/gallery3.log 2>&1 &\n")
        wait_for(lambda: "error /media/bad.png" in g.sh("cat /tmp/gallery3.log"), 120, what="the error for a truncated picture")
        assert "truncated" in g.sh("cat /tmp/gallery3.log"), g.sh("cat /tmp/gallery3.log")
        # --- animated GIF: the centre cycles through the three colours
        v = launch(g, "/media/anim.gif")
        seen = set()

        def cycles():
            sh = v.shot()
            for name, col in (("r", RED), ("g", GREEN), ("b", BLUE)):
                if v.has(sh, 0, 0, col):
                    seen.add(name)
            return len(seen) == 3

        wait_for(cycles, 120, interval=0.1, what="all three frames of the animated GIF")
        # --- slideshow: moves on without a key
        v = launch(g, "--slideshow 1 /media/pics/01-red.png")
        seen.clear()

        def slides():
            sh = v.shot()
            for name, col in (("r", RED), ("g", GREEN), ("b", BLUE)):
                if v.has(sh, 0, 0, col):
                    seen.add(name)
            return len(seen) >= 2

        wait_for(slides, 180, interval=0.3, what="the slideshow to move on by itself")
        # --- set as wallpaper
        g.key("esc")                                                  # stops the slideshow
        v = launch(g, "/media/wall.png")
        v.wait_centre((10, 90, 200), "the wallpaper picture in the Gallery", 120)
        before = g.pixel(*DESK)
        g.key("w")
        conf = wait_for(lambda: (lambda t: t if "/media/wall.png" in t else "")(g.sh("cat ~/.config/hakurei/wallpaper 2>&1")), 60, what="the wallpaper setting file")
        assert "cover" in conf, conf
        wait_for(lambda: near(g.pixel(*DESK), (10, 90, 200), 12), 120, what="the desktop to show the new wallpaper")
        assert not near(before, (10, 90, 200), 30), "the wallpaper pixel should have changed"
        out = g.sh("wallpaper -c; wallpaper")
        assert "night scene" in out, out
        wait_for(lambda: not near(g.pixel(*DESK), (10, 90, 200), 30), 120, what="the desktop to go back to the built-in scene")
        g.shot("gallery-done")


@gui_test
def media_screenshot():
    """Ctrl+F12 and the screenshot command write PNG files into ~/Pictures (named by date and time); the PNG holds the same pixels as /tmp/screenshot.ppm; wallpaper modes work."""
    with Qemu(cmdline="text", name="t-media-shot", mem="768M") as vm:
        g = watch(Gui(vm))
        g.start_desktop()
        g.sh("rm -f /tmp/screenshot.ppm; ls ~/Pictures 2>&1 | head -2")
        g.key("f12", ctrl=True)
        listing = wait_for(lambda: g.sh("ls ~/Pictures/").strip() if "Screenshot-" in g.sh("ls ~/Pictures/ 2>&1") else "", 180, what="the screenshot in ~/Pictures")
        name = re.search(r"Screenshot-\d{4}-\d{2}-\d{2}-\d{6}\.png", listing)
        assert name, f"the file name should carry the date and time: {listing!r}"
        out = wait_for(lambda: g.sh(f"imginfo -s ~/Pictures/{name.group(0)} /tmp/screenshot.ppm") if "ppm" in g.sh("ls /tmp/screenshot.ppm 2>&1") else "", 120, what="/tmp/screenshot.ppm")
        lines = LINE.findall(out)
        assert len(lines) == 2, out
        png, ppm = lines
        assert png[1] == "PNG" and (png[2], png[3]) == ("1024", "768"), png
        assert ppm[1] == "PNM" and (ppm[2], ppm[3]) == ("1024", "768"), ppm
        assert png[6] == ppm[6], "the PNG and the PPM should hold the same pixels (the same capture)"
        # the command: explicit name, then the default name
        out = g.sh("screenshot /tmp/mine.png")
        assert "/tmp/mine.png" in out, out
        out = g.sh("imginfo /tmp/mine.png")
        assert "PNG 1024x768" in out, out
        out = g.sh("screenshot -d 1")
        m = re.search(r"(\S*Pictures/Screenshot-\S+\.png)", out)
        assert m, out
        assert "PNG 1024x768" in g.sh(f"imginfo {m.group(1)}")
        # wallpaper modes: a 4x2 picture (red / blue) in each mode
        img = Image.new("RGB", (4, 2))
        for x in range(4):
            for y in range(2):
                img.putpixel((x, y), (255, 0, 0) if x < 2 else (0, 0, 255))
        b = io.BytesIO()
        img.save(b, "PNG")
        enc = base64.b64encode(b.getvalue()).decode()
        g.sh(f"echo {enc} | base64 -d > /tmp/rb.png")
        for mode in ("stretch", "cover"):                         # both fill the screen: red on the left half, blue on the right
            g.sh(f"wallpaper -m {mode} /tmp/rb.png")
            wait_for(lambda: near(g.pixel(300, 300), (255, 0, 0), 12) and near(g.pixel(900, 300), (0, 0, 255), 12), 120, what=f"{mode} wallpaper")
        g.sh("wallpaper -m fit")
        wait_for(lambda: near(g.pixel(300, 300), (255, 0, 0), 12) and near(g.pixel(900, 300), (0, 0, 255), 12) and not near(g.pixel(500, 60), (255, 0, 0), 40), 120, what="fit wallpaper")
        g.sh("wallpaper -m center")
        wait_for(lambda: not near(g.pixel(300, 300), (255, 0, 0), 40), 120, what="center wallpaper (a 4x2 dot in the middle)")
        g.sh("wallpaper -c")
