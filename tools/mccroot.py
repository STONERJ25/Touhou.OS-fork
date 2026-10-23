"""The system root of the mcc compiler: headers and libraries in the layout mcc expects under $MCC_ROOT.

    include/       libgap headers (user/libc/include), the shared ABI headers (shared/*.h), the headers of the user libraries
    lib/mcc/include/   mcc's own freestanding headers (stddef.h, stdarg.h, stdint.h, limits.h ...)
    lib/crt0.o  lib/libgap.a  lib/libkappa.a ...   what a TouhouOS program is linked with

tools/build.py calls stage_sysroot() after the userland is built and puts the tree into the initrd under
/ScarletDevilMansion (so the compiler on the guest finds it there); the host tests use build/mcc-root directly.
"""
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
GUEST_PREFIX = "ScarletDevilMansion"


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
        _copy_if_changed(src, dest / rel)
    return dest


def initrd_items(dest=None):
    """[(archive name, path)] for tools/build.py: the staged root under /ScarletDevilMansion."""
    dest = Path(dest) if dest else BUILD / "mcc-root"
    return [(f"{GUEST_PREFIX}/{rel}", dest / rel) for rel, _ in sysroot_files() if (dest / rel).exists()]
