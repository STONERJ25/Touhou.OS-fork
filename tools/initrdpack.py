"""initrd.img from the archive: tools/build.py's compression step, apart so that tests/host/test_inflate_tools.py can run it without LLVM (docs/UPDATE.md, "The compressed initrd").

`build/initrd.tar` is the ustar archive; `build/initrd.img` is what goes on the ESP: that archive as one gzip member (RFC 1952) at zlib level 9 with the modification time 0 and no
file name, so that the same archive always gives the same bytes, or, with TOUHOU_INITRD=plain, the archive as it is (the form before 2026-11-03, which a machine whose
installed `update` applet does not know gzip needs for the push that brings it the new kernel).
"""
import hashlib
import os
import struct
import sys
import zlib
from pathlib import Path

GZIP_LEVEL = 9


def mode(env=None):
    """"gzip" (the default) or "plain", from TOUHOU_INITRD."""
    value = (os.environ if env is None else env).get("TOUHOU_INITRD", "gzip").strip().lower() or "gzip"
    if value not in ("gzip", "plain"):
        sys.exit(f"build: TOUHOU_INITRD={value!r}: it is 'gzip' (the default) or 'plain'")
    return value


def compress(data):
    """The archive as the gzip member the kernel unpacks: a fixed ten-byte header (no flags, no time, no name, "slowest algorithm", OS 255 = unknown), zlib's deflate at level 9 and
    the trailer. The header is written here rather than by gzip.compress() because that leaves the OS byte to the zlib of the platform (3 on Linux, 255 on this Windows), and the
    same archive should give the same bytes on every PC (the deflate stream itself depends on the zlib version only)."""
    c = zlib.compressobj(GZIP_LEVEL, zlib.DEFLATED, -15)
    body = c.compress(data) + c.flush()
    return b"\x1f\x8b\x08\x00" + bytes(4) + bytes([2 if GZIP_LEVEL == 9 else 0, 255]) + body + struct.pack("<II", zlib.crc32(data), len(data) & 0xFFFFFFFF)


def pack(tar_path, img_path, say=print, rel=str):
    """Writes img_path from tar_path in the form TOUHOU_INITRD asks for; does nothing when the archive and the form are those the existing file was made from. Returns img_path."""
    img_path = Path(img_path)
    m = mode()
    data = Path(tar_path).read_bytes()
    want = f"{m}:{GZIP_LEVEL if m == 'gzip' else 0}:{hashlib.sha256(data).hexdigest()}"
    stamp = img_path.with_name(img_path.name + ".cmd")
    if img_path.exists() and stamp.exists() and stamp.read_text() == want:
        return img_path
    blob = compress(data) if m == "gzip" else data
    tmp = img_path.with_name(img_path.name + ".new")
    tmp.write_bytes(blob)
    os.replace(tmp, img_path)
    stamp.write_text(want)
    if m == "gzip":
        say(f"  GZIP {rel(img_path)} ({len(data) // 1024} KiB -> {len(blob) // 1024} KiB, {100 * len(blob) / max(1, len(data)):.1f}%)")
    else:
        say(f"  COPY {rel(img_path)} (TOUHOU_INITRD=plain: the tar as it is, {len(blob) // 1024} KiB)")
    return img_path


def describe(image):
    """A phrase about an initrd.img for the build's size note: its form and, for a gzip one, what it unpacks to (the last four bytes of a gzip member are its length mod 2^32)."""
    raw = Path(image).read_bytes()
    if raw[:2] == b"\x1f\x8b":
        unpacked = int.from_bytes(raw[-4:], "little")
        return f" (initrd.img is gzip: {len(raw) / 1048576:.1f} MiB for a {unpacked / 1048576:.1f} MiB archive, {100 * len(raw) / max(1, unpacked):.1f}%)"
    return " (initrd.img is a plain tar archive because of TOUHOU_INITRD=plain: without it the build compresses it, to about 40% of this)"
