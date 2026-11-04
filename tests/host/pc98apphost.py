#!/usr/bin/env python3
"""Helpers for the PC-98 application's tests (docs/PC98_APP.md): the host tool tests/host/pc98apptool.c, the guest programs of
tests/host/pc98app/*.asm and the synthetic disk images. Used by tests/host/test_pc98app.py (host, inside WSL on the owner's Windows PC)
and tests/case_pc98app.py (QEMU, on Windows).

Nothing from a real PC-98 is used: every program is written for the tests, every image is made here.

From the command line (builds the tool optimised and without sanitizers, runs it through WSL on Windows; the images stay where they are):

    python tests/host/pc98apphost.py run --disk IMAGE --cycles 400 --font FONT.ROM --ppm build/pc98/x.ppm [...]      (see the header of pc98apptool.c)
"""
import os
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import x86ref  # noqa: E402
from test_pc98 import d88, pattern  # noqa: E402,F401  (the D88 builder and the test patterns, shared with the library's tests)

REPO = x86ref.REPO
APP = REPO / "user" / "bin" / "pc98app"
ASM = HERE / "pc98app"
WINDOWS = os.name == "nt"
OUT = REPO / "build" / ("host-x86-wsl" if WINDOWS or x86ref.OUT.name.endswith("wsl") else "host-x86")
# every source of the application that needs no window (the Kappa parts are main.c, ui.c, ...: they are not here)
APP_SOURCES = ["keymap.c", "config.c", "pacer.c", "audiopipe.c", "disks.c", "idle.c", "emu.c"]
SHARED = [REPO / "shared" / "font8x16.c"]


# ------------------------------------------------------------------ building and running the tool (WSL on Windows)

def wsl_path(p):
    p = Path(p).resolve()
    return f"/mnt/{p.drive[:1].lower()}/" + str(p)[3:].replace("\\", "/")


def host_path(p):
    return wsl_path(p) if WINDOWS else str(p)


def shell(cmd, **kw):
    """Runs an argv list on the build host: WSL's Ubuntu on Windows, the machine itself elsewhere."""
    if not WINDOWS:
        return subprocess.run(cmd, **kw)
    wsl = shutil.which("wsl.exe") or shutil.which("wsl")
    if not wsl:
        raise RuntimeError("no wsl.exe: install WSL with a distribution that has gcc, or run the tests on Linux")
    quoted = " ".join("'" + a.replace("'", "'\\''") + "'" for a in cmd)
    return subprocess.run([wsl, "-d", os.environ.get("TOUHOU_WSL_DISTRO", "Ubuntu"), "-e", "sh", "-c", quoted], **kw)


def tool_sources():
    return [HERE / "pc98apptool.c"] + [APP / s for s in APP_SOURCES] + SHARED + sorted((x86ref.LIB / "src").glob("*.c"))


def build_tool(fast=False):
    """Builds pc98apptool: with AddressSanitizer and UBSan (-O1), or `fast` (-O2, no sanitizers: for measuring). Returns the path."""
    OUT.mkdir(parents=True, exist_ok=True)
    exe = OUT / ("pc98apptool-fast" if fast else "pc98apptool")
    srcs = tool_sources()
    deps = srcs + sorted((x86ref.LIB / "include").glob("*.h")) + sorted((x86ref.LIB / "src").glob("*.h")) + sorted(APP.glob("*.h"))
    if exe.exists() and all(d.stat().st_mtime <= exe.stat().st_mtime for d in deps):
        return exe
    flags = ["-O2", "-g"] if fast else ["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]
    cmd = ["gcc", *flags, "-std=gnu17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-sign-compare",
           "-I", host_path(x86ref.LIB / "include"), "-I", host_path(REPO / "shared"), *[host_path(s) for s in srcs], "-o", host_path(exe), "-lm"]
    r = shell(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("build of pc98apptool failed:\n%s%s" % (r.stdout, r.stderr))
    return exe


def run_tool(args, timeout=900, fast=False):
    """Runs the tool; returns (returncode, stdout lines)."""
    exe = build_tool(fast)
    r = shell([host_path(exe), *[host_path(a) if isinstance(a, Path) else str(a) for a in args]], capture_output=True, text=True,
              errors="replace", timeout=timeout, env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
    if r.returncode not in (0, 1, 2):
        raise RuntimeError("pc98apptool died (%d): %s\n%s" % (r.returncode, r.stderr[-3000:], r.stdout[-3000:]))
    return r.returncode, r.stdout.splitlines()


def kv(line, prefix):
    """The key=value pairs of an output line that starts with `prefix` (as strings)."""
    assert line.startswith(prefix + " "), line
    return dict(p.split("=", 1) for p in line[len(prefix) + 1:].split() if "=" in p)


def find(lines, prefix):
    return next(l for l in lines if l.startswith(prefix + " ") or l == prefix)


def memory(lines):
    """The MEM lines of --dump: {address: byte}."""
    mem = {}
    for l in lines:
        if l.startswith("MEM "):
            addr, data = l[4:].split(":", 1)
            for i, b in enumerate(data.split()):
                mem[int(addr, 16) + i] = int(b, 16)
    return mem


def word(mem, a):
    return mem[a] | mem[a + 1] << 8


# ------------------------------------------------------------------ guest programs

def assemble(name, defines=(), extra_args=()):
    """Assembles tests/host/pc98app/NAME.asm (with -DX=Y for each define) and returns the bytes. Needs nasm."""
    nasm = x86ref.find_nasm()
    if not nasm:
        raise unittest.SkipTest("needs nasm")
    tag = "-".join(d.replace("=", "") for d in defines)
    out = REPO / "build" / "host-pc98app" / f"{name}{('-' + tag) if tag else ''}.bin"
    out.parent.mkdir(parents=True, exist_ok=True)
    cmd = [nasm, "-f", "bin", "-o", str(out), *[f"-D{d}" for d in defines], *extra_args, str(ASM / f"{name}.asm")]
    subprocess.run(cmd, check=True)
    return out.read_bytes()


# ------------------------------------------------------------------ disk images

def boot_d88(boot, cyls=1, name=b"PC98APP TEST"):
    """A 2HD D88 floppy (2 heads, 8 sectors of 1024 bytes per track) whose first sector is the boot program; the other sectors hold
    a pattern, one of them (C0 H0 R5) is the one the demo programs write to."""
    tracks = {}
    for c in range(cyls):
        for h in range(2):
            secs = []
            for r in range(1, 9):
                data = pattern(c * 100 + h * 10 + r, 1024)
                if (c, h, r) == (0, 0, 1):
                    data = (boot + bytes(1024))[:1024]
                secs.append((c, h, r, 3, data, 0, 0, 0))
            tracks[c * 2 + h] = secs
    return d88(tracks, name=name)


def boot_d88_small(boot, name=b"PC98APP TEST"):
    """A D88 with the first track only (head 0 of cylinder 0: eight sectors of 1024 bytes, the boot program in the first): about 9 KB, small
    enough to type into the guest over the serial console in the QEMU tests. The demo programs use only sectors 1 and 5 of it."""
    secs = []
    for r in range(1, 9):
        data = pattern(r, 1024)
        if r == 1:
            data = (boot + bytes(1024))[:1024]
        secs.append((0, 0, r, 3, data, 0, 0, 0))
    return d88({0: secs}, name=name)


def boot_hdi(boot, cyls=2, heads=4, spt=8, ssz=512):
    """An Anex86 HDI whose first sector is the boot program and whose other sectors hold a pattern. (More than two heads: pc98disk takes
    an Anex86 image with two heads or fewer and at most 90 cylinders for a floppy, an FDI.)"""
    import struct
    data = bytearray()
    for n in range(cyls * heads * spt):
        data += pattern(1000 + n, ssz)
    data[:len(boot)] = boot
    head = struct.pack("<8I", 0, 0, 4096, len(data), ssz, spt, heads, cyls)
    return head + bytes(4096 - len(head)) + bytes(data)


def raw_floppy(boot):
    """A raw 1232 KiB (77 x 2 x 8 x 1024) floppy dump."""
    data = bytearray(b"".join(pattern(i, 1024) for i in range(77 * 2 * 8)))
    data[:len(boot)] = boot
    return bytes(data)


def fdi(boot, cyls=77):
    import struct
    data = bytearray(b"".join(pattern(i, 1024) for i in range(cyls * 2 * 8)))
    data[:len(boot)] = boot
    head = struct.pack("<8I", 0, 0x90, 4096, len(data), 1024, 8, 2, cyls)
    return head + bytes(4096 - len(head)) + bytes(data)


def make_image_dir(tag):
    d = REPO / "build" / "host-pc98app" / tag
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True, exist_ok=True)
    return d


def cli_args(argv):
    """The command line of `run` with the paths after --disk, --font, --rhythm, --ppm and --wav (and the path in --save SLOT:PATH)
    made visible to WSL when this runs on Windows. The images and the font stay where they are; they are only read."""
    out, i = [], 0
    while i < len(argv):
        a = argv[i]
        out.append(a)
        if a in ("--disk", "--font", "--rhythm", "--ppm", "--wav") and i + 1 < len(argv):
            if a in ("--ppm", "--wav"):
                Path(argv[i + 1]).resolve().parent.mkdir(parents=True, exist_ok=True)
            out.append(host_path(argv[i + 1]))
            i += 1
        elif a == "--save" and i + 1 < len(argv):
            slot, _, path = argv[i + 1].partition(":")
            out.append(slot + ":" + host_path(path))
            i += 1
        i += 1
    return out


def write_png(ppm, png):
    """A P6 PPM to a PNG (zlib only), for looking at a picture the tool wrote."""
    import struct
    import zlib
    data = Path(ppm).read_bytes()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    rows = b"".join(b"\x00" + parts[3][y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    Path(png).parent.mkdir(parents=True, exist_ok=True)
    Path(png).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                          + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "run":
        argv, png = list(sys.argv[2:]), None
        if "--png" in argv:                                     # --png FILE: the picture at the end as a PNG (through a PPM beside it)
            i = argv.index("--png")
            png = argv[i + 1]
            argv[i:i + 2] = ["--ppm", str(REPO / "build" / "host-pc98app" / (Path(png).stem + ".ppm"))]
        rc, lines = run_tool(["run", *cli_args(argv)], fast=True)
        print("\n".join(lines))
        if png:
            write_png(REPO / "build" / "host-pc98app" / (Path(png).stem + ".ppm"), png)
            print("PNG", png)
        sys.exit(rc)
    print(__doc__)
