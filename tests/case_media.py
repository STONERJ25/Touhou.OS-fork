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
from guitools import Gui, near, SHRINE_RED, PAPER
from harness import Qemu, BUILD
from imgdata import make_png, expect_png, photo, build_corpus, mutate, clear_transparent, make_gif, many_scans_jpeg
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
    out["media/fuzz/manyscans.jpg"] = many_scans_jpeg(64, 64, 200)       # 200 repeated scans: refused after 64, not decoded for as long as it takes
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
        text, rc = test_shell.run(vm, "imginfo -s /media/fuzz/manyscans.jpg; echo st=$?")
        assert "error" in text and "st=1" in text, f"a JPEG with 200 scans must be refused: {text}"
        text, rc = test_shell.run(vm, "imginfo -s /dev/zero /dev/null /dev/console /dev/tty; echo st=$?", timeout=60)       # not regular files: refused at once, not read
        assert text.count("error") == 4 and "st=1" in text, f"devices are not pictures: {text}"
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
    # hostile archives: names that climb out of the target directory, absolute names, a symlink followed by a file written through it
    evil = io.BytesIO()
    with tarfile.open(fileobj=evil, mode="w", format=tarfile.GNU_FORMAT) as tf:
        def add(name, data=b"bad\n", kind=tarfile.REGTYPE, link=""):
            ti = tarfile.TarInfo(name)
            ti.type = kind
            ti.size = 0 if kind != tarfile.REGTYPE else len(data)
            ti.linkname = link
            ti.mode = 0o644
            ti.mtime = MTIME
            tf.addfile(ti, io.BytesIO(data) if kind == tarfile.REGTYPE else None)
        add("good.txt", b"good\n")
        add("../escape.txt")
        add("a/../../escape2.txt")
        add("/tmp/abs-escape.txt")
        add("lnk", kind=tarfile.SYMTYPE, link="/tmp/xe")
        add("lnk/pwn.txt")
    files["media/arch/evil.tar"] = evil.getvalue()
    ez = io.BytesIO()
    with zipfile.ZipFile(ez, "w") as zf:
        zf.writestr("ok.txt", b"fine\n")
        zf.writestr("../zipescape.txt", b"bad\n")
        zf.writestr("/tmp/zipabs.txt", b"bad\n")
    files["media/arch/evil.zip"] = ez.getvalue()
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
        # --- hostile archives stay inside the directory they are unpacked into
        out, rc = run("rm -rf /tmp/xe /tmp/abs-escape.txt /tmp/zipabs.txt && mkdir -p /tmp/xe/inner && tar -xf /media/arch/evil.tar -C /tmp/xe/inner; echo st=$?; "
                      "ls /tmp/xe /tmp/xe/inner; ls /tmp/abs-escape.txt /tmp/xe/escape.txt /tmp/xe/escape2.txt /tmp/xe/pwn.txt 2>&1")
        assert "good.txt" in out and "st=2" in out, out
        assert out.count("cannot access") == 4, f"nothing may be written outside the directory (all four stray paths must be missing): {out}"
        assert "unsafe or empty name" in out and "refusing to extract through a symbolic link" in out, f"the refusals should be explained: {out}"
        out, rc = run("ls /tmp/xe/inner/tmp; cat /tmp/xe/inner/tmp/abs-escape.txt; ls /tmp/xe/pwn.txt 2>&1")
        assert "abs-escape.txt" in out and "bad" in out and out.count("cannot access") == 1, out         # the absolute name lands inside; nothing goes through the symlink
        out, rc = run("mkdir -p /tmp/xe/zinner && unzip -d /tmp/xe/zinner /media/arch/evil.zip; echo st=$?; ls /tmp/xe/zinner; ls /tmp/xe/zipescape.txt /tmp/zipabs.txt 2>&1")
        assert "ok.txt" in out and "st=1" in out and out.count("cannot access") == 2, out                 # neither stray file exists
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


@harness.test
def media_utimes_fat():
    """tar -x and unzip restore modification times on a FAT volume: utimes() goes through to the directory entry, so the checker that reads the image after the guest is gone sees them."""
    from case_storage import Image, hv_part, Guest, MIB
    t1, t2 = MTIME, MTIME + 86400 + 6                    # even seconds: FAT keeps two-second steps
    tb = io.BytesIO()
    with tarfile.open(fileobj=tb, mode="w", format=tarfile.USTAR_FORMAT) as tf:
        for name, data, mt in (("kept.txt", b"kept\n", t1), ("sub/deep.txt", b"deep\n" * 100, t2)):
            ti = tarfile.TarInfo(name)
            ti.size = len(data)
            ti.mtime = mt
            ti.mode = 0o644
            tf.addfile(ti, io.BytesIO(data))
    zb = io.BytesIO()
    with zipfile.ZipFile(zb, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr(zipfile.ZipInfo("zipped.txt", (2023, 11, 15, 22, 13, 26)), b"zipped\n")
    esp = media_esp({"media/t.tar": tb.getvalue(), "media/t.zip": zb.getvalue()}, "esp-media-fat")
    with Image("media-fat", 64 * MIB, [hv_part({})]) as img:
        with Qemu(cmdline="text user=root", disk=img.path, esp=esp, name="t-media-fat", mem="512M") as vm:
            vm.expect(r"root@gensokyo \S+ # ", timeout=120)
            g = Guest(vm, True)
            g.ok("tar -xf /media/t.tar -C /HumanVillage && unzip -q -d /HumanVillage /media/t.zip && ls -l /HumanVillage /HumanVillage/sub")
            out = g.ok("stat /HumanVillage/kept.txt")
            assert "2023-11-14 22:13:20" in out, f"the guest should see the restored time at once: {out}"
            assert g.poweroff() is not None
        img.check()
        r = img.volume(1)
        try:
            kept, deep, zipped = r.find("/kept.txt"), r.find("/sub/deep.txt"), r.find("/zipped.txt")
            assert kept and deep and zipped, "the extracted files must be on the disk"
            assert kept.mtime == t1, f"kept.txt: {kept.mtime} on disk, expected {t1}"
            assert deep.mtime == t2, f"sub/deep.txt: {deep.mtime} on disk, expected {t2}"
            assert zipped.mtime == 1700086406, f"zipped.txt: {zipped.mtime} on disk (the zip's DOS time 2023-11-15 22:13:26, read in the guest's UTC)"
            assert r.read_file("/sub/deep.txt") == b"deep\n" * 100
        finally:
            r.close()


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
    wait_for(lambda: "gallery" not in g.sh("ps"), 60, what="the previous Gallery to quit")
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
        # the picture is written as Screenshot-...png.part and renamed when it is complete: wait for the final name
        done = r"Screenshot-\d{4}-\d{2}-\d{2}-\d{6}\.png(?!\.part)"
        listing = wait_for(lambda: (lambda ls: ls.strip() if re.search(done, ls) else "")(g.sh("ls ~/Pictures/ 2>&1")), 180, what="the screenshot in ~/Pictures")
        name = re.search(done, listing)
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


@gui_test
def media_panel_wallpaper():
    """Control Panel > Appearance > Wallpaper: a placement button, Night scene and Choose picture... (the file dialog) change ~/.config/hakurei/wallpaper, and the desktop follows."""
    img = Image.new("RGB", (4, 2))
    for x in range(4):
        for y in range(2):
            img.putpixel((x, y), (255, 0, 0) if x < 2 else (0, 0, 255))
    b = io.BytesIO()
    img.save(b, "PNG")
    esp = media_esp({"media/rb.png": b.getvalue()}, "esp-media-panel")
    top, mid, BLUE = (1000, 100), (1000, 300), (0, 0, 255)   # right of the panel window (it opens at x = 40 and ends before 930), clear of the icons: a bar above a fitted 2:1 picture, blue once stretched
    with Qemu(cmdline="text", name="t-media-panel", esp=esp, mem="768M") as vm:
        g = watch(Gui(vm))
        g.start_desktop()
        g.sh("wallpaper -m fit /media/rb.png")
        g.sh("kappa-panel --trace --pane appearance &")
        vm.expect(r"kappa-panel: ready", timeout=180)
        wait_for(lambda: near(g.pixel(140, 340), SHRINE_RED, 60) and near(g.pixel(600, 250), PAPER, 16), 180, what="the Control Panel window (Appearance selected in the sidebar)")
        wait_for(lambda: not near(g.pixel(*top), BLUE, 60) and near(g.pixel(*mid), BLUE, 30), 120, what="the fitted wallpaper (a bar above, blue on the right)")
        g.click(686, 217)                                   # the "Stretch" radio button of the Placement row (the panel is the first window: content at 44, 56)
        vm.expect(r"kappa-panel: wallpaper /media/rb.png stretch", timeout=180)
        assert g.sh("cat ~/.config/hakurei/wallpaper").split()[-2:] == ["/media/rb.png", "stretch"], "the setting file should hold the picture and the placement"
        wait_for(lambda: near(g.pixel(*top), BLUE, 30), 120, what="the stretched wallpaper")
        g.click(505, 185)                                   # "Night scene"
        vm.expect(r"kappa-panel: wallpaper \(none\)", timeout=180)
        assert "built-in night scene" in g.sh("wallpaper")
        wait_for(lambda: not near(g.pixel(*top), BLUE, 60), 120, what="the night scene")
        g.click(346, 185)                                   # "Choose picture...": the file dialog opens, the name field has the focus
        wait_for(lambda: near(g.pixel(500, 166), SHRINE_RED, 70), 120, what="the file dialog")
        g.type("/media/rb.png\n")
        vm.expect(r"kappa-panel: wallpaper /media/rb.png cover", timeout=180)
        assert "/media/rb.png (cover)" in g.sh("wallpaper")
        wait_for(lambda: near(g.pixel(*top), BLUE, 30), 120, what="the chosen wallpaper (covering the screen)")
        g.sh("wallpaper -c")
