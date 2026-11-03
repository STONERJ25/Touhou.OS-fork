"""The system root of the mcc compiler: headers and libraries in the layout mcc expects under $MCC_ROOT.

    include/       libgap headers (user/libc/include), the shared ABI headers (shared/*.h), the headers of the user libraries
    lib/mcc/include/   mcc's own freestanding headers (stddef.h, stdarg.h, stdint.h, limits.h ...)
    src/mcc/       the source of mcc itself (so that the OS can compile its own compiler)
    lib/crt0.o  lib/libgap.a  lib/libkappa.a ...   what a TouhouOS program is linked with

tools/build.py calls stage_sysroot() after the userland is built and puts the tree into the initrd under
/ScarletDevilMansion (so the compiler on the guest finds it there); the host tests use build/mcc-root directly.
"""
import os
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
GUEST_PREFIX = "ScarletDevilMansion"
WINDOWS_LLVM_DIR = r"C:\Program Files\LLVM\bin"


def _objcopy():
    """llvm-objcopy, looked for the way tools/build.py looks for LLVM (TOUHOU_LLVM_DIR, the PATH, the Windows install folder); None if there is none."""
    exe = ".exe" if os.name == "nt" else ""
    env = os.environ.get("TOUHOU_LLVM_DIR")
    if env:
        p = Path(env) / ("llvm-objcopy" + exe)
        return str(p) if p.is_file() else None
    found = shutil.which("llvm-objcopy")
    if found:
        return found
    p = Path(WINDOWS_LLVM_DIR) / ("llvm-objcopy" + exe)
    return str(p) if os.name == "nt" and p.is_file() else None


def _copy_archive(src, dst):
    """A library archive of the system root without its debug sections. The guest only links with it: the sections are three quarters of the
    archive (11.8 MB of archives against 3.4 MB), and the 64 MiB boot partition of the installed machine must hold the new image beside two
    old ones when an update is pushed (docs/UPDATE.md, "Room on the ESP")."""
    objcopy = _objcopy()
    if not objcopy:
        return _copy_if_changed(src, dst)
    src, dst = Path(src), Path(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists() and dst.stat().st_size < src.stat().st_size and dst.stat().st_mtime_ns >= src.stat().st_mtime_ns:
        return False
    tmp = dst.with_name(dst.name + ".tmp")
    subprocess.run([objcopy, "--strip-debug", str(src), str(tmp)], check=True)
    os.replace(tmp, dst)
    return True


def _copy_if_changed(src, dst):
    src, dst = Path(src), Path(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists() and dst.stat().st_size == src.stat().st_size and dst.stat().st_mtime_ns >= src.stat().st_mtime_ns:
        return False
    shutil.copyfile(src, dst)
    return True


def _headers(base):
    base = Path(base)
    if not base.exists():
        return []
    return [(p.relative_to(base).as_posix(), p) for p in sorted(base.rglob("*.h")) if p.is_file()]


def sysroot_files():
    """[(path inside the root, source file)] for everything the compiler needs."""
    files = []
    for rel, p in _headers(ROOT / "user" / "libc" / "include"):
        files.append(("include/" + rel, p))
    for p in sorted((ROOT / "shared").glob("*.h")):
        files.append(("include/" + p.name, p))
    libs = ROOT / "user" / "lib"
    if libs.exists():
        for d in sorted(libs.iterdir()):
            for rel, p in _headers(d / "include"):
                files.append(("include/" + rel, p))
    for rel, p in _headers(ROOT / "user" / "bin" / "mcc" / "include"):
        files.append(("lib/mcc/include/" + rel, p))
    # the compiler's own source: the guest can rebuild mcc with mcc (see docs/MCC.md, "Self-hosting")
    src = ROOT / "user" / "bin" / "mcc"
    for pat in ("*.c", "*.inc", "*.h"):
        for p in sorted(src.glob(pat)):
            files.append(("src/mcc/" + p.name, p))
    # the rest of what the guest rebuilds with mmake (docs/SELFHOST.md): libgap, kappabox, mmake, and the Makefile that does it
    for rel, pats in (("libc", ("user/libc/src/*.c", "user/libc/src/*.h", "user/libc/src/*.asm", "user/libc/crt0.asm")),
                      ("kappabox", ("user/bin/kappabox/*.c", "user/bin/kappabox/*.h")),
                      ("mmake", ("user/bin/mmake/mmake.c",)),
                      ("mas", ("user/bin/mas/*.c", "user/bin/mas/*.inc"))):
        for pat in pats:
            for p in sorted(ROOT.glob(pat)):
                files.append((f"src/{rel}/{p.name}", p))
    files.append(("src/Makefile", ROOT / "user" / "bin" / "mmake" / "selfhost.mk"))
    files.append(("src/gen-applets.awk", ROOT / "user" / "bin" / "mmake" / "gen-applets.awk"))
    crt0 = BUILD / "obj" / "libc" / "user" / "libc" / "crt0.asm.o"
    if crt0.exists():
        files.append(("lib/crt0.o", crt0))
    lib = BUILD / "lib"
    if lib.exists():
        for p in sorted(lib.glob("*.a")):
            files.append(("lib/" + p.name, p))
    return files


def stage_sysroot(dest=None):
    """Copies the sysroot to build/mcc-root (or dest); returns the directory."""
    dest = Path(dest) if dest else BUILD / "mcc-root"
    for rel, src in sysroot_files():
        (_copy_archive if str(rel).endswith(".a") else _copy_if_changed)(src, dest / rel)
    return dest


def initrd_items(dest=None):
    """[(archive name, path)] for tools/build.py: the staged root under /ScarletDevilMansion."""
    dest = Path(dest) if dest else BUILD / "mcc-root"
    return [(f"{GUEST_PREFIX}/{rel}", dest / rel) for rel, _ in sysroot_files() if (dest / rel).exists()]
