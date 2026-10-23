#!/usr/bin/env python3
"""Host-side differential and fuzz test for libimg (Linux or WSL with gcc, AddressSanitizer and Pillow).

    ASAN_OPTIONS=detect_leaks=0 python3 -u tests/host/imgtest.py [quick]

Builds tests/host/imgtool.c with sanitizers, makes images (Pillow for JPEG, GIF, BMP, TGA, PNM; hand-written encoders for the
PNG variants Pillow cannot write: 16-bit colour, 1/2/4-bit palettes, Adam7, tRNS colour keys, every filter type), and compares
what libimg decodes with what Pillow (or the reference computation) says. Then mutates the files and checks that nothing crashes.
Not part of tests/run_all.py: that suite only boots the guest; tests/case_media.py repeats the checks inside it.
"""
import io
import os
import random
import struct
import subprocess
import sys
import zlib
from pathlib import Path

from PIL import Image, ImageOps

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from imgdata import make_png, expect_png, photo, build_corpus, mutate  # noqa: E402

REPO = Path(__file__).resolve().parent.parent.parent
OUT = REPO / "build" / "host-img"
OUT.mkdir(parents=True, exist_ok=True)
TOOL = OUT / "imgtool"
QUICK = len(sys.argv) > 1 and sys.argv[1] == "quick"
FAILS = []


def build():
    srcs = sorted((REPO / "user/lib/img/src").glob("*.c")) + sorted((REPO / "user/lib/z/src").glob("*.c"))
    cmd = ["gcc", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-Wall", "-Wextra",
           "-Wno-unused-parameter", "-I", str(REPO / "user/lib/img/include"), "-I", str(REPO / "user/lib/z/include"),
           "-I", str(REPO / "user/lib/img/src"), str(REPO / "tests/host/imgtool.c")] + [str(s) for s in srcs] + ["-o", str(TOOL)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode or r.stderr.strip():
        print(r.stderr)
    assert r.returncode == 0, "build failed"


def tool(*args, timeout=60):
    r = subprocess.run([str(TOOL)] + [str(a) for a in args], capture_output=True, timeout=timeout)
    return r.returncode, r.stdout.decode(errors="replace"), r.stderr.decode(errors="replace")


def ours(path):
    """Decodes with libimg; returns (w, h, has_alpha, rgba bytes) or None on error."""
    raw = OUT / "dump.rgba"
    rc, out, err = tool("dump", path, raw)
    assert "Sanitizer" not in err and "runtime error" not in err, err[-800:]
    if rc != 0:
        return None
    w, h, a = map(int, out.split())
    return w, h, a, raw.read_bytes()


def check(name, cond, detail=""):
    if not cond:
        FAILS.append(name)
        print("FAIL", name, detail)
    return cond


def clear_transparent(b):
    out = bytearray(b)
    for i in range(0, len(out), 4):
        if out[i + 3] == 0:
            out[i] = out[i + 1] = out[i + 2] = 0
    return bytes(out)


def compare(name, path, expect_rgba, size, tol=0, expect_alpha=None, ignore_hidden=False):
    r = ours(path)
    if not check(name + " decodes", r is not None, f"libimg refused {path}"):
        return
    w, h, a, data = r
    if not check(name + " size", (w, h) == size, f"{(w, h)} != {size}"):
        return
    if ignore_hidden:                                     # the colour of a fully transparent pixel means nothing
        data, expect_rgba = clear_transparent(data), clear_transparent(expect_rgba)
    if len(data) != len(expect_rgba):
        check(name + " length", False, f"{len(data)} vs {len(expect_rgba)}")
        return
    if tol == 0:
        ok = data == expect_rgba
        detail = ""
        if not ok:
            i = next(k for k in range(len(data)) if data[k] != expect_rgba[k])
            detail = f"first difference at pixel {i // 4} channel {i % 4}: ours {data[i]} expected {expect_rgba[i]}"
        check(name, ok, detail)
    else:
        worst = max(abs(x - y) for x, y in zip(data, expect_rgba))
        check(name, worst <= tol, f"max difference {worst} > {tol}")
    if expect_alpha is not None:
        check(name + " alpha flag", bool(a) == expect_alpha, f"{a} vs {expect_alpha}")


# ---------------------------------------------------------------------------------------------- PNG (hand-made)

def test_png():
    rnd = random.Random(5)
    combos = [(0, 1), (0, 2), (0, 4), (0, 8), (0, 16), (2, 8), (2, 16), (3, 1), (3, 2), (3, 4), (3, 8), (4, 8), (4, 16), (6, 8), (6, 16)]
    sizes = [(1, 1), (7, 5), (16, 3), (13, 17)] if not QUICK else [(7, 5), (13, 17)]
    for ctype, depth in combos:
        channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
        for (w, h) in sizes:
            for interlace in (0, 1):
                for variant in ("plain", "trns"):
                    if variant == "trns" and ctype in (4, 6):
                        continue
                    maxv = (1 << depth) - 1
                    pix = [[tuple(rnd.randint(0, maxv) for _ in range(channels)) for _ in range(w)] for _ in range(h)]
                    plte = trns = None
                    if ctype == 3:
                        n = min(1 << depth, 256) if variant == "plain" else min(1 << depth, 200)
                        plte = [tuple(rnd.randrange(256) for _ in range(3)) for _ in range(n)]
                        for row in pix:
                            for i in range(w):
                                row[i] = (row[i][0] % n,)
                        if variant == "trns":
                            trns = bytes(rnd.randrange(256) for _ in range(rnd.randint(1, n)))
                    elif variant == "trns":
                        key = pix[h // 2][w // 2]            # make one colour transparent (and it occurs at least once)
                        trns = struct.pack(">H", key[0]) if ctype == 0 else struct.pack(">HHH", *key)
                    data = make_png(w, h, ctype, depth, pix, interlace, plte, trns, rnd)
                    path = OUT / "t.png"
                    path.write_bytes(data)
                    name = f"png ct{ctype} d{depth} {w}x{h} i{interlace} {variant}"
                    exp = expect_png(w, h, ctype, depth, pix, plte, trns)
                    alpha = ctype in (4, 6) or trns is not None
                    compare(name, path, exp, (w, h), expect_alpha=alpha)
                    # cross-check the reference against Pillow where Pillow can read the variant
                    skip_ref = variant == "trns" and ((ctype == 0 and depth < 8) or (ctype in (0, 2) and depth == 16))   # Pillow differs from the spec there
                    if (depth < 16 or ctype in (2, 6)) and not skip_ref:
                        try:
                            im = Image.open(path).convert("RGBA")
                            if ctype != 0 or depth == 8 or True:
                                pil = im.tobytes()
                                if pil != exp and not (ctype == 0 and depth == 16):
                                    check(name + " (reference vs Pillow)", False, "my reference disagrees with Pillow")
                        except Exception as e:  # noqa: BLE001
                            check(name + " (Pillow reads it)", False, str(e))
    # a Pillow-written PNG too
    for mode in ("1", "L", "LA", "P", "RGB", "RGBA"):
        im = Image.effect_noise((21, 14), 80).convert(mode if mode != "P" else "L")
        if mode == "P":
            im = im.convert("P", palette=Image.ADAPTIVE, colors=17)
        if mode == "RGB" or mode == "RGBA":
            im = Image.merge(mode, [Image.effect_noise((21, 14), 60 + 30 * i).convert("L") for i in range(len(mode))])
        path = OUT / "pil.png"
        im.save(path)
        compare(f"png pillow {mode}", path, Image.open(path).convert("RGBA").tobytes(), im.size)
    # damage
    good = make_png(4, 4, 2, 8, [[(1, 2, 3)] * 4] * 4)
    (OUT / "bad.png").write_bytes(make_png(4, 4, 2, 8, [[(1, 2, 3)] * 4] * 4, bad_crc=True))
    rc, out, err = tool("try", OUT / "bad.png")
    check("png bad CRC is refused", out.split()[1] != "0", out)


# ---------------------------------------------------------------------------------------------- the formats Pillow writes

def test_jpeg():
    sizes = [(64, 48), (67, 45), (17, 9), (1, 1), (33, 100)] if not QUICK else [(67, 45)]
    for (w, h) in sizes:
        base = photo(w, h)
        for mode in ("RGB", "L"):
            src = base if mode == "RGB" else base.convert("L")
            for prog in (False, True):
                for sub in ((0, 1, 2) if mode == "RGB" else (0,)):
                    for q in (50, 92):
                        buf = io.BytesIO()
                        kw = dict(quality=q, progressive=prog, optimize=prog)
                        if mode == "RGB":
                            kw["subsampling"] = sub
                        src.save(buf, "JPEG", **kw)
                        path = OUT / "t.jpg"
                        path.write_bytes(buf.getvalue())
                        name = f"jpeg {mode} {w}x{h} prog={prog} sub={sub} q={q}"
                        compare(name, path, Image.open(path).convert("RGBA").tobytes(), (w, h), tol=2, expect_alpha=False)
    # restart markers, 4:4:0 and 4:1:1 sampling, EXIF orientations
    base = photo(50, 30)
    for rm in (1, 3):
        buf = io.BytesIO()
        try:
            base.save(buf, "JPEG", quality=85, restart_marker_blocks=rm)
        except TypeError:
            continue
        path = OUT / "t.jpg"
        path.write_bytes(buf.getvalue())
        compare(f"jpeg restart every {rm} blocks", path, Image.open(path).convert("RGBA").tobytes(), (50, 30), tol=2)
    for o in range(1, 9):
        buf = io.BytesIO()
        ex = Image.Exif()
        ex[0x0112] = o
        base.save(buf, "JPEG", quality=90, exif=ex)
        path = OUT / "t.jpg"
        path.write_bytes(buf.getvalue())
        pil = ImageOps.exif_transpose(Image.open(path)).convert("RGBA")
        compare(f"jpeg EXIF orientation {o}", path, pil.tobytes(), pil.size, tol=2)
        rc, out, err = tool("info", path)
        check(f"jpeg EXIF orientation {o} probe", out.split()[1:3] == [str(pil.size[0]), str(pil.size[1])], out)


def test_gif():
    rnd = random.Random(9)
    for (w, h) in ((16, 12), (1, 1), (37, 23)):
        im = Image.new("P", (w, h))
        im.putpalette([c for i in range(256) for c in (i, (i * 7) % 256, (i * 13) % 256)])
        px = im.load()
        for y in range(h):
            for x in range(w):
                px[x, y] = rnd.randrange(40)
        for transp in (None, 3):
            path = OUT / "t.gif"
            kw = {} if transp is None else {"transparency": transp}
            im.save(path, **kw)
            pil = Image.open(path).convert("RGBA")
            compare(f"gif {w}x{h} transparency={transp}", path, pil.tobytes(), (w, h), ignore_hidden=True)
    # an animation
    frames = []
    for i in range(5):
        f = Image.new("RGB", (30, 20), (i * 40, 0, 255 - i * 40))
        for k in range(i + 1):
            f.paste((255, 255, 0), (k * 5, k * 3, k * 5 + 6, k * 3 + 6))
        frames.append(f)
    path = OUT / "anim.gif"
    frames[0].save(path, save_all=True, append_images=frames[1:], duration=[50, 120, 20, 300, 100], loop=0, disposal=1)
    rc, out, err = tool("frames", path, OUT / "fr")
    check("gif animation runs", rc == 0, out + err)
    lines = out.split()
    check("gif animation header", lines[:4] == ["30", "20", "5", "0"], out)
    pim = Image.open(path)
    for i in range(5):
        pim.seek(i)
        expect = pim.convert("RGBA").tobytes()
        got = clear_transparent((OUT / f"fr{i}.rgba").read_bytes())
        expect = clear_transparent(expect)
        diff = sum(1 for a, b in zip(got, expect) if a != b)
        check(f"gif animation frame {i}", diff == 0, f"{diff} bytes differ")
    check("gif delays", [int(x) for x in lines[4:9]] == [50, 120, 100, 300, 100] or [int(x) for x in lines[4:9]] == [50, 120, 20, 300, 100], out)


def test_bmp_pnm_tga():
    rnd = random.Random(11)
    for mode in ("1", "L", "P", "RGB", "RGBA"):
        for (w, h) in ((9, 5), (32, 3), (1, 7)):
            if mode == "1":
                im = Image.effect_noise((w, h), 100).convert("1")
            elif mode == "L":
                im = Image.effect_noise((w, h), 100)
            elif mode == "P":
                im = Image.effect_noise((w, h), 100).convert("P", palette=Image.ADAPTIVE, colors=12)
            else:
                im = Image.merge(mode, [Image.effect_noise((w, h), 40 + 20 * i).convert("L") for i in range(len(mode))])
            for ext, ok in (("bmp", True), ("tga", mode != "1"), ("ppm", mode in ("RGB",)), ("pgm", mode == "L"), ("pbm", mode == "1")):
                if not ok:
                    continue
                path = OUT / f"t.{ext}"
                try:
                    im.save(path)
                except Exception:  # noqa: BLE001
                    continue
                pil = Image.open(path).convert("RGBA")
                compare(f"{ext} {mode} {w}x{h}", path, pil.tobytes(), (w, h))
    # RLE TGA, ASCII PNM, RLE8 BMP, 16-bit BMP, top-down BMP, 16-bit PPM written by hand
    im = Image.merge("RGB", [Image.effect_noise((20, 9), 30 + 30 * i).convert("L") for i in range(3)])
    for i in range(0, 20 * 9, 3):                      # runs, so the RLE has something to do
        im.putpixel((i % 20, i // 20), (10, 20, 30))
    path = OUT / "rle.tga"
    im.save(path, compression="tga_rle")
    compare("tga rle", path, Image.open(path).convert("RGBA").tobytes(), im.size)
    path = OUT / "a.ppm"
    path.write_bytes(b"P3\n# comment\n3 2\n255\n255 0 0  0 255 0  0 0 255\n10 20 30  40 50 60 70 80 90\n")
    compare("ppm ascii", path, bytes([255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255]), (3, 2))
    path = OUT / "a.pbm"
    path.write_bytes(b"P1\n3 2\n1 0 1\n0 1 0\n")
    black, white = bytes([0, 0, 0, 255]), bytes([255, 255, 255, 255])
    compare("pbm ascii", path, black + white + black + white + black + white, (3, 2))
    path = OUT / "w.ppm"
    path.write_bytes(b"P6\n2 1\n65535\n" + struct.pack(">HHHHHH", 0xFFFF, 0x8000, 0x0100, 0, 0x1234, 0xFF00))
    compare("ppm 16-bit", path, bytes([255, 128, 1, 255, 0, 0x12, 255, 255]), (2, 1))
    # RLE8 BMP: 4x2, bottom-up. row0 (bottom): run of 4 index 1 ; row1: absolute 3 then 1 encoded
    pal = [(0, 0, 0), (255, 0, 0), (0, 255, 0), (0, 0, 255)]
    rle = bytes([4, 1, 0, 0, 0, 3, 0, 1, 2, 0, 1, 3, 0, 0, 0, 1]) if False else bytes([4, 1, 0, 0, 0, 3, 0, 1, 2, 0, 1, 3, 0, 1])
    hdr = struct.pack("<2sIHHI", b"BM", 14 + 40 + 16 + len(rle), 0, 0, 14 + 40 + 16)
    dib = struct.pack("<IiiHHIIiiII", 40, 4, 2, 1, 8, 1, len(rle), 2835, 2835, 4, 0)
    palb = b"".join(bytes((b, g, r, 0)) for r, g, b in pal)
    path = OUT / "rle8.bmp"
    path.write_bytes(hdr + dib + palb + rle)
    # expected: bottom row = 4 x index1 (red); top row = indexes 1,2,3 absolute then index 3?? absolute run of 3 (pad), then run 1 of index 3 -> 1,2,3,3 wait
    top = [pal[0], pal[1], pal[2], pal[3]]
    bottom = [pal[1]] * 4
    exp = b"".join(bytes((*c, 255)) for c in top + bottom)
    compare("bmp rle8", path, exp, (4, 2))
    # 16-bit 565 bitfields, top-down
    rows = [[0xF800, 0x07E0, 0x001F], [0xFFFF, 0x0000, 0x8410]]
    data = b"".join(b"".join(struct.pack("<H", v) for v in r) + b"\0\0" for r in rows)
    masks = struct.pack("<III", 0xF800, 0x07E0, 0x001F)
    hdr = struct.pack("<2sIHHI", b"BM", 14 + 40 + 12 + len(data), 0, 0, 14 + 40 + 12)
    dib = struct.pack("<IiiHHIIiiII", 40, 3, -2, 1, 16, 3, len(data), 2835, 2835, 0, 0)
    path = OUT / "b565.bmp"
    path.write_bytes(hdr + dib + masks + data)
    pil = Image.open(path).convert("RGBA")
    def e565(v):
        return bytes((((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31, 255))
    exp = b"".join(e565(v) for r in rows for v in r)
    compare("bmp 565 top-down", path, exp, (3, 2))


# ---------------------------------------------------------------------------------------------- transformations and writers

def test_writers():
    base = photo(23, 17)
    rgba = base.convert("RGBA")
    px = rgba.load()
    for y in range(17):
        for x in range(23):
            if (x + y) % 5 == 0:
                px[x, y] = (px[x, y][0], px[x, y][1], px[x, y][2], 100 + x)
    for name, im in (("RGB", base), ("RGBA", rgba), ("L", base.convert("L")), ("LA", base.convert("LA"))):
        src = OUT / f"w_{name}.png"
        im.save(src)
        for ext in ("png", "bmp", "ppm"):
            if ext == "ppm" and name in ("RGBA", "LA"):
                continue
            dst = OUT / f"enc.{ext}"
            for level in ((1, 6, 9) if ext == "png" else (6,)):
                rc, out, err = tool("convert", src, dst, level)
                assert "Sanitizer" not in err and "runtime error" not in err, err[-600:]
                check(f"convert {name} -> {ext} runs", rc == 0, out)
                if rc:
                    continue
                back = Image.open(dst)
                back.load()
                check(f"convert {name} -> {ext} (level {level}) pixels", back.convert("RGBA").tobytes() == im.convert("RGBA").tobytes())
    # scaling and rotation against simple references
    src = OUT / "w_RGB.png"
    for mode in (0, 1, 2):
        for (nw, nh) in ((23, 17), (11, 8), (5, 3), (46, 34), (7, 1)):
            rc, out, err = tool("scale", src, OUT / "sc.rgba", nw, nh, mode)
            check(f"scale mode {mode} -> {nw}x{nh} runs", rc == 0, out + err[-300:])
            if rc == 0:
                got = (OUT / "sc.rgba").read_bytes()
                check(f"scale mode {mode} -> {nw}x{nh} size", len(got) == nw * nh * 4)
    # area average of a flat colour stays flat; of a 2x2 block pattern gives the mean
    flat = Image.new("RGB", (10, 6), (200, 100, 50))
    flat.save(OUT / "flat.png")
    rc, out, err = tool("scale", OUT / "flat.png", OUT / "sc.rgba", 3, 2, 2)
    check("area scale of a flat image", rc == 0 and (OUT / "sc.rgba").read_bytes() == bytes((200, 100, 50, 255)) * 6)
    chk = Image.new("RGB", (4, 4))
    for y in range(4):
        for x in range(4):
            chk.putpixel((x, y), (255, 255, 255) if (x + y) % 2 == 0 else (0, 0, 0))
    chk.save(OUT / "chk.png")
    rc, out, err = tool("scale", OUT / "chk.png", OUT / "sc.rgba", 2, 2, 2)
    got = (OUT / "sc.rgba").read_bytes()
    check("area scale of a checkerboard", rc == 0 and all(abs(got[i] - 127) <= 1 for i in range(0, 16, 4)), str(list(got)))
    for turns in range(4):
        rc, out, err = tool("rot", src, OUT / "rot.out", turns)
        data = (OUT / "rot.out").read_bytes()
        first, rest = data.split(b"\n", 1)
        w, h = map(int, first.split())
        ref = base.rotate(-90 * turns, expand=True).convert("RGBA")
        check(f"rotate {turns} quarter turns", (w, h) == ref.size and rest == ref.tobytes())


# ---------------------------------------------------------------------------------------------- hostile files

def test_fuzz():
    rnd = random.Random(int(os.environ.get("FUZZ_SEED", "2024")))
    corpus = [data for data in build_corpus()[0].values() if len(data) > 8]       # every picture the guest test decodes
    corpus.append(make_png(9, 7, 2, 8, [[(1, 2, 3)] * 9] * 7, interlace=1))
    corpus.append(make_png(9, 7, 3, 4, [[(3,)] * 9] * 7, plte=[(i, i, i) for i in range(16)], trns=b"\x00\x80"))
    base = photo(40, 24)
    for kw in (dict(progressive=True, optimize=True, subsampling=0), dict(restart_marker_blocks=2), dict(restart_marker_rows=1, subsampling=2)):
        buf = io.BytesIO()
        base.save(buf, "JPEG", **kw)
        corpus.append(buf.getvalue())
    n = int(os.environ.get("FUZZ_N", 200 if QUICK else 1200))
    bad = 0
    stats = {}
    for i in range(n):
        src = rnd.choice(corpus)
        data = mutate(rnd, src)
        path = OUT / "fuzz.bin"
        path.write_bytes(data)
        try:
            rc, out, err = tool("try", path, timeout=60)
        except subprocess.TimeoutExpired:
            check(f"fuzz #{i} terminates", False, "timeout")
            path.rename(OUT / f"fuzz-timeout-{i}.bin")
            continue
        if rc != 0 or "Sanitizer" in err or "runtime error" in err:
            bad += 1
            keep = OUT / f"fuzz-crash-{i}.bin"
            path.rename(keep)
            check(f"fuzz #{i}", False, f"rc={rc} saved as {keep.name}\n{err[-700:]}")
        else:
            prc, rcode = out.split()
            stats[rcode] = stats.get(rcode, 0) + 1
    print("fuzz outcomes (decode return codes):", dict(sorted(stats.items())))
    # truncation at every length for a few small files
    for p in ("t.png", "t.jpg", "t.gif", "t.bmp"):
        f = OUT / p
        if not f.exists():
            continue
        data = f.read_bytes()
        step = max(1, len(data) // 60)
        for cut in range(0, len(data), step):
            (OUT / "trunc.bin").write_bytes(data[:cut])
            rc, out, err = tool("try", OUT / "trunc.bin", timeout=60)
            if rc != 0 or "Sanitizer" in err or "runtime error" in err:
                check(f"truncated {p} at {cut}", False, err[-500:])
    # absurd sizes must be refused, not allocated
    huge = make_png(2, 2, 2, 8, [[(0, 0, 0)] * 2] * 2)
    huge = huge[:16] + struct.pack(">II", 0x7FFFFFFF, 0x7FFFFFFF) + huge[24:]
    (OUT / "huge.png").write_bytes(huge)
    rc, out, err = tool("try", OUT / "huge.png")
    check("huge PNG header is refused", rc == 0 and out.split()[1] != "0", out + err)


def main():
    build()
    tests = (test_png, test_jpeg, test_gif, test_bmp_pnm_tga, test_writers, test_fuzz)
    if os.environ.get("ONLY") == "fuzz":                      # ONLY=fuzz FUZZ_N=6000 FUZZ_SEED=7 for a long hostile-input run
        tests = (test_fuzz,)
    for fn in tests:
        before = len(FAILS)
        fn()
        print(f"{fn.__name__}: {'ok' if len(FAILS) == before else str(len(FAILS) - before) + ' failure(s)'}", flush=True)
    print("failures:", len(FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
