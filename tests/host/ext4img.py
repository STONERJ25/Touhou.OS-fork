#!/usr/bin/env python3
"""Volumes for the ext2/ext3/ext4 tests (docs/EXT4.md): `mke2fs -d` fills an image from a generated source tree, and the manifest that decides whether the driver is right is computed
from that **source tree on the host** (type, mode, owner, size, crc32 of the content, link target, hard-link groups), not from anything the generator remembers.

  python3 tests/host/ext4img.py build OUTDIR PROFILE [PROFILE ...]     writes OUTDIR/PROFILE.img and OUTDIR/PROFILE.json (run it on Linux or in WSL)
  python3 tests/host/ext4img.py list

On Windows the callers (tests/case_ext4.py) go through `ensure()`, which runs the same script in WSL's Ubuntu: a Linux source tree needs real symbolic links, fifos, sparse files and hard
links, and `mke2fs`, `debugfs` and `e2fsck` (e2fsprogs 1.47) are the oracle: the images must be clean under `e2fsck -fn` (a check of the generator), and `debugfs` shows what a
reference implementation sees. Nothing of e2fsprogs' source was read: only its programs are used.

The images are deterministic in content (seeded generators); their time stamps and the on-disk allocation come from mke2fs.
"""
import hashlib
import json
import os
import random
import re
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
MIB = 1 << 20
GIB = 1 << 30
UUID = "11111111-2222-3333-4444-555555555555"

# ---------------------------------------------------------------------------------------------- running things in Linux (WSL on Windows)


def wsl_path(p):
    p = Path(p).resolve()
    return f"/mnt/{p.drive[:1].lower()}/" + str(p)[3:].replace("\\", "/")


def linux(cmd, root=False, check=True, **kw):
    """Runs a shell command line in Linux: directly on Linux, in WSL's Ubuntu on Windows."""
    if os.name == "nt":
        wsl = shutil.which("wsl.exe") or "wsl.exe"
        argv = [wsl, "-d", os.environ.get("TOUHOU_WSL_DISTRO", "Ubuntu"), *(["-u", "root"] if root else []), "-e", "sh", "-c", cmd]
    else:
        argv = ["sh", "-c", cmd]
    return subprocess.run(argv, check=check, **kw)


def have_tools():
    """mke2fs, debugfs and e2fsck, where the images are made."""
    try:
        r = linux("command -v mke2fs debugfs e2fsck >/dev/null && command -v python3 >/dev/null", check=False, capture_output=True, timeout=60)
        return r.returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


# ---------------------------------------------------------------------------------------------- crc32, with holes

_CRC_TABLE = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ 0xEDB88320 if _c & 1 else _c >> 1
    _CRC_TABLE.append(_c)


def _mat_vec(m, v):
    r = 0
    i = 0
    while v:
        if v & 1:
            r ^= m[i]
        v >>= 1
        i += 1
    return r


def _mat_sq(m):
    return [_mat_vec(m, col) for col in m]


_ZPOW = []                                  # _ZPOW[k]: the register's change when 2**k zero bytes are appended, as a 32x32 matrix over GF(2) (column i: the image of bit i)


def crc32_zeros(crc, n):
    """crc32 of (the data that has crc `crc`) followed by n zero bytes, without making them: the register is changed linearly by every byte, so n bytes are a matrix power."""
    if not _ZPOW:
        _ZPOW.append([(((1 << i) >> 8) ^ _CRC_TABLE[(1 << i) & 255]) for i in range(32)])
    r = ~crc & 0xFFFFFFFF
    k = 0
    while n:
        while len(_ZPOW) <= k:
            _ZPOW.append(_mat_sq(_ZPOW[-1]))
        if n & 1:
            r = _mat_vec(_ZPOW[k], r)
        n >>= 1
        k += 1
    return ~r & 0xFFFFFFFF


def file_crc(path):
    """crc32 of a file's content, skipping the holes of a sparse file (they are zeros: crc32_zeros)."""
    crc = 0
    with open(path, "rb") as f:
        size = os.fstat(f.fileno()).st_size
        pos = 0
        while pos < size:
            try:
                data = os.lseek(f.fileno(), pos, os.SEEK_DATA)
            except OSError:                              # nothing but a hole to the end
                data = size
            if data > pos:
                crc = crc32_zeros(crc, data - pos)
                pos = data
                continue
            hole = os.lseek(f.fileno(), pos, os.SEEK_HOLE)
            f.seek(pos)
            left = hole - pos
            while left > 0:
                chunk = f.read(min(left, 4 * MIB))
                if not chunk:
                    break
                crc = zlib.crc32(chunk, crc)
                left -= len(chunk)
            pos = hole
    return crc & 0xFFFFFFFF


# ---------------------------------------------------------------------------------------------- source trees


class Ctx:
    """What a tree builder cannot do as an ordinary user: owners and modes mke2fs cannot copy, device nodes, odd time stamps. `debugfs -w` does it after mke2fs,
    and the expected manifest entry is changed to match (the manifest of the *source tree* says what mke2fs was given; these are the deliberate differences)."""

    def __init__(self, bsize=4096, isize=256):
        self.bsize = bsize                               # a symbolic link's target must fit in one block
        self.isize = isize                               # an inode of 128 bytes has no extra fields
        self.cmds = []
        self.over = {}                                   # path -> fields that replace the source tree's
        self.add = {}                                    # path -> entries the source tree does not have

    def sif(self, path, field, value, **expect):
        self.cmds.append(f"set_inode_field {path} {field} {value}")
        self.over.setdefault(path, {}).update(expect)

    def mknod(self, path, kind, major, minor, mode):
        d, name = path.rsplit("/", 1)
        self.cmds += [f"cd {d or '/'}", f"mknod {name} {kind} {major} {minor}", f"set_inode_field {path} mode 0{mode:o}"]
        self.add[path] = dict(kind=kind, mode=mode & 0o7777, uid=0, gid=0, size=0, mtime=None, nlink=1, group=None, rdev=os.makedev(major, minor))


def rnd_bytes(rnd, n):
    return rnd.randbytes(n)


def rnd_text(rnd, n):
    words = [b"gensokyo", b"reimu", b"marisa", b"touhou", b"kappa", b"fairy", b"shrine", b"\n", b" ", b"sakuya", b"patchouli"]
    out = bytearray()
    while len(out) < n:
        out += rnd.choice(words)
    return bytes(out[:n])


def put(root, rel, data=b"", mode=0o644):
    p = Path(root) / rel.lstrip("/")
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(data)
    os.chmod(p, mode)
    return p


def tree_basic(root, rnd, ctx):
    """Ordinary files, directories, names, links, modes."""
    put(root, "/hello.txt", b"hello from a generated ext volume\n")
    put(root, "/empty", b"")
    put(root, "/small.bin", rnd_bytes(rnd, 100))
    put(root, "/text.txt", rnd_text(rnd, 50_000))
    put(root, "/data.bin", rnd_bytes(rnd, 1_500_000))
    put(root, "/exact4k.bin", rnd_bytes(rnd, 4096))
    put(root, "/exact64k.bin", rnd_bytes(rnd, 65536))
    put(root, "/one.bin", b"x")
    put(root, "/dir/sub/deep/file.txt", b"deep file\n")
    for i in range(30):
        put(root, f"/dir/f{i:02d}.txt", rnd_text(rnd, 10 + i * 211))
    put(root, "/dir/with space.txt", b"a name with a space\n")
    put(root, "/dir/ファイル.txt", b"unicode name\n")
    put(root, "/dir/.hidden", b"hidden\n")
    p = put(root, "/holes.bin", rnd_bytes(rnd, 5000))
    with open(p, "r+b") as f:                           # data, a hole of 300 KB, data
        f.seek(305_000)
        f.write(rnd_bytes(rnd, 5000))
    os.symlink("hello.txt", Path(root) / "rel-link")
    os.symlink("/dir/sub/deep/file.txt", Path(root) / "abs-link")
    os.symlink("nowhere/at/all", Path(root) / "dangling")
    os.symlink("dir", Path(root) / "dir-link")
    os.link(Path(root) / "hello.txt", Path(root) / "hard1")
    os.link(Path(root) / "hello.txt", Path(root) / "dir" / "hard2")
    put(root, "/modes/exec", b"#!/bin/sh\n", 0o755)
    put(root, "/modes/private", b"secret\n", 0o600)
    put(root, "/modes/none", b"no access\n", 0o644)
    ctx.sif("/modes/none", "mode", "0100000", mode=0)                 # a file nobody can open: mke2fs (not root) could not read it, so the mode is set in the image
    put(root, "/modes/owned", b"owned by a high uid and gid\n")
    ctx.sif("/modes/owned", "uid", 70000, uid=70000)
    ctx.sif("/modes/owned", "gid", 1000000, gid=1000000)
    put(root, "/times/negative", b"before 1970\n")
    ctx.sif("/times/negative", "mtime", "@-256", mtime=-256)           # a signed 32-bit time: 256 seconds before the epoch (debugfs writes 0xffffff00, no epoch bit)
    if ctx.isize > 128:
        put(root, "/times/far", b"after 2038\n")
        ctx.sif("/times/far", "mtime", "0xFFFFFF00", mtime=4294967040)
        ctx.sif("/times/far", "mtime_extra", "0x1", mtime=4294967040)    # the epoch bit: 2106
        put(root, "/times/nanoseconds", b"a time with nanoseconds\n")
        ctx.sif("/times/nanoseconds", "mtime", "0x6b000000", mtime=0x6b000000 + (3 << 32))
        ctx.sif("/times/nanoseconds", "mtime_extra", "0x3b9aca07", mtime=0x6b000000 + (3 << 32), mtime_ns=250000001)   # epoch bits 3 and 250000001 ns
    put(root, "/modes/setuid", b"x", 0o4755)
    put(root, "/modes/setgid", b"x", 0o2755)
    (Path(root) / "modes" / "sticky").mkdir()
    os.chmod(Path(root) / "modes" / "sticky", 0o1777)
    (Path(root) / "modes" / "closed").mkdir()
    os.chmod(Path(root) / "modes" / "closed", 0o700)
    (Path(root) / "emptydir").mkdir()


def tree_indirect(root, rnd, ctx):
    """Data in every range of an ext2/ext3 block map for 1 KiB blocks: direct (< 12 KiB), single (< 268 KiB), double (< 64.3 MiB), triple, with holes between."""
    p = put(root, "/ind.bin", rnd_bytes(rnd, 20_000))
    with open(p, "r+b") as f:
        for off in (100_000, 400_000, 1_000_000, 30 * MIB, 64 * MIB + 5000, 70 * MIB + 12345):
            f.seek(off)
            f.write(rnd_bytes(rnd, 7000 + off % 3000))
        f.truncate(71 * MIB)                            # the file ends in a hole after the last data
    put(root, "/ind-dense.bin", rnd_bytes(rnd, 3_000_000))   # dense: a long run of single and double indirect blocks


def tree_names(root, rnd, ctx):
    names = ["a", "bb", "x" * 100, "y" * 254, "z" * 255, "é" * 127 + "e", "フ" * 85, "name with spaces and (parens) [brackets]", "-rf", "~tilde", "dot.in.name.txt", "UPPER", "MiXed"]
    for n in names:
        put(root, f"/names/{n}", b"content of " + n.encode()[:40] + b"\n")
    (Path(root) / "names" / ("d" * 255)).mkdir()
    put(root, f"/names/{'d' * 255}/inside", b"in a directory with a 255-byte name\n")


def tree_types(root, rnd, ctx):
    """Every file type that a tar can carry, and symbolic links of the interesting lengths."""
    for n in (1, 10, 58, 59, 60, 61, 100, 255, 1000, 4000):
        if n >= ctx.bsize:
            continue
        target = ("t" * (n - 1) + "/")[:n] if n > 1 else "t"
        os.symlink(target, Path(root) / f"link{n:04d}")
    os.mkfifo(Path(root) / "fifo", 0o640)
    s = socket.socket(socket.AF_UNIX)
    s.bind(str(Path(root) / "sock"))
    s.close()
    os.chmod(Path(root) / "sock", 0o660)
    put(root, "/file", b"regular file\n")
    os.link(Path(root) / "file", Path(root) / "file-hardlink")
    (Path(root) / "tdir").mkdir()
    os.symlink("file", Path(root) / "tdir" / "to-file")
    ctx.mknod("/chardev", "c", 4, 66, 0o20644)
    ctx.mknod("/blkdev", "b", 259, 1, 0o60660)
    ctx.mknod("/chardev2", "c", 300, 5000, 0o20600)


def tree_many(root, rnd, ctx, n=20000):
    """One directory with n entries (a hashed directory on ext4)."""
    d = Path(root) / "many"
    d.mkdir()
    for i in range(n):
        p = d / f"file-{i:05d}-{(i * 2654435761) % 1000003:06d}.dat"
        if i % 500 == 0:
            p.write_bytes(rnd_bytes(rnd, 1 + (i * 37) % 5000))
        else:
            p.write_bytes(b"")
    put(root, "/after.txt", b"after the big directory\n")


def tree_big(root, rnd, ctx):
    """A file of 5 GiB with holes: data at the start, in the middle and at the end."""
    p = Path(root) / "big5g.bin"
    with open(p, "wb") as f:
        f.write(b"START" * 1000)
        f.seek(2 * GIB + 12345)
        f.write(b"MIDDLE" * 700)
        f.seek(4 * GIB + 4096 * 7)
        f.write(rnd_bytes(rnd, 100_000))
        f.seek(5 * GIB)
        f.write(b"END" * 3000)
    put(root, "/small.txt", b"small neighbour\n")
    p = Path(root) / "just-over-4g.bin"                 # one byte past 4 GiB: the size needs the high word
    with open(p, "wb") as f:
        f.seek(4 * GIB)
        f.write(b"Z")


def tree_fragmented(root, rnd, ctx):
    """Files with many extents (islands of data in a sparse file): an extent tree with index and leaf blocks."""
    p = Path(root) / "islands.bin"
    with open(p, "wb") as f:
        for i in range(1200):
            f.seek(i * 3 * 4096)
            f.write(rnd_bytes(rnd, 4096 if i % 3 else 1500))
    put(root, "/plain.txt", b"neighbour\n")


def tree_inline(root, rnd, ctx):
    """Sizes around what an inode can hold (60 bytes of i_block, the rest in the inode body), small directories, long links."""
    for n in (0, 1, 59, 60, 61, 80, 100, 120, 150, 200, 400, 5000):
        put(root, f"/f{n:04d}", rnd_bytes(rnd, n))
    for k in range(1, 6):                              # directories with 1..5 entries: inline, then (for the last ones) too big for the inode
        for j in range(k * 2):
            put(root, f"/d{k}/entry{j}", b"e%d\n" % j)
    for n in (30, 59, 60, 61, 90, 100, 150):
        os.symlink("q" * n, Path(root) / f"ln{n:03d}")
    put(root, "/dir/nested/inner.txt", b"inner\n")


def tree_speed(root, rnd, ctx):
    """One file of 100 MB (read speed), and a small neighbour."""
    put(root, "/big100m.bin", rnd_bytes(rnd, 100_000_000))
    put(root, "/small.txt", b"small neighbour\n")


def tree_many_small(root, rnd, ctx):
    tree_many(root, rnd, ctx, 6000)                      # 6000 names of about 30 bytes: a directory of more than 128 KiB, where the name index of the parser starts


# ---------------------------------------------------------------------------------------------- profiles

# name: size, mke2fs arguments (everything except -d, the image and the size), source trees, post-processing (debugfs commands: a function of nothing)
PROFILES = {
    # ext2 with 1 KiB blocks: block maps up to the triple indirect range, holes, an old-style layout; no journal
    "ext2_1k": dict(size=64 * MIB, mkfs=["-t", "ext2", "-b", "1024", "-L", "ext2-1k", "-N", "2000"], trees=["basic", "indirect"]),
    # ext3: block maps and a journal, 4 KiB blocks
    "ext3": dict(size=48 * MIB, mkfs=["-t", "ext3", "-b", "4096", "-L", "ext3"], trees=["basic", "types"]),
    # ext4 with the defaults of this mke2fs: extents, flex_bg, 64bit, metadata_csum, dir_index, a journal
    "ext4": dict(size=64 * MIB, mkfs=["-t", "ext4", "-b", "4096", "-L", "ext4-default"], trees=["basic", "types", "fragmented"]),
    "ext4_1k": dict(size=64 * MIB, mkfs=["-t", "ext4", "-b", "1024", "-L", "ext4-1k", "-N", "4000"], trees=["basic", "fragmented"]),
    "ext4_64k": dict(size=300 * MIB, mkfs=["-F", "-t", "ext4", "-b", "65536", "-L", "ext4-64k"], trees=["basic", "types"]),
    "ext4_nojournal": dict(size=32 * MIB, mkfs=["-t", "ext4", "-b", "4096", "-O", "^has_journal", "-L", "nojournal"], trees=["basic"]),
    "ext4_inline": dict(size=32 * MIB, mkfs=["-t", "ext4", "-b", "4096", "-O", "inline_data,^has_journal", "-I", "256", "-L", "inline"], trees=["inline", "types"]),
    "ext4_inline_128": dict(size=16 * MIB, mkfs=["-t", "ext4", "-b", "1024", "-O", "^has_journal,^metadata_csum", "-I", "128", "-L", "inode128"], trees=["basic"]),
    "ext4_bigdir": dict(size=96 * MIB, mkfs=["-t", "ext4", "-b", "4096", "-N", "40000", "-L", "bigdir"], trees=["many"]),
    "ext4_speed": dict(size=128 * MIB, mkfs=["-t", "ext4", "-b", "4096", "-O", "^has_journal", "-L", "speed"], trees=["speed"]),
    "ext4_big": dict(size=6 * GIB, mkfs=["-t", "ext4", "-b", "4096", "-O", "^has_journal", "-L", "big"], trees=["big"]),
    "ext2_big": dict(size=6 * GIB, mkfs=["-t", "ext2", "-b", "4096", "-L", "ext2big", "-N", "256"], trees=["big"]),
    "ext4_names": dict(size=32 * MIB, mkfs=["-t", "ext4", "-b", "4096", "-L", "names"], trees=["names"]),
    "ext4_metabg": dict(size=600 * MIB, mkfs=["-t", "ext4", "-b", "1024", "-O", "meta_bg,^resize_inode,^has_journal", "-L", "metabg32"], extended="lazy_itable_init=1", trees=["basic"]),
    "ext4_metabg64": dict(size=600 * MIB, mkfs=["-t", "ext4", "-b", "1024", "-O", "meta_bg,^resize_inode,64bit,^has_journal", "-L", "metabg64"], extended="lazy_itable_init=1", trees=["basic"]),
    "ext4_bigalloc": dict(size=64 * MIB, mkfs=["-t", "ext4", "-b", "4096", "-O", "bigalloc", "-C", "16384", "-L", "bigalloc"], trees=["basic", "types"]),
    # small images for the fuzz (the fuzz copies the whole image for every case) and for the hand-made damage of the hostile tests
    "fz_ext2": dict(size=32 * MIB, mkfs=["-t", "ext2", "-b", "1024", "-L", "fz-ext2", "-N", "512"], trees=["basic", "indirect"]),
    "fz_ext3": dict(size=24 * MIB, mkfs=["-t", "ext3", "-b", "2048", "-L", "fz-ext3", "-N", "512"], trees=["basic", "types"]),
    "fz_ext4": dict(size=24 * MIB, mkfs=["-t", "ext4", "-b", "4096", "-O", "^has_journal", "-L", "fz-ext4", "-N", "512"], trees=["basic", "types", "fragmented"]),
    "fz_ext4_1k": dict(size=24 * MIB, mkfs=["-t", "ext4", "-b", "1024", "-L", "fz-ext4-1k", "-N", "512"], trees=["basic", "types", "fragmented"]),
    "fz_inline": dict(size=16 * MIB, mkfs=["-t", "ext4", "-b", "1024", "-O", "inline_data,^has_journal", "-I", "256", "-L", "fz-inline", "-N", "512"], trees=["inline", "types"]),
    # a directory of more than 128 KiB that is a block map (direct and single indirect blocks), 1 KiB blocks: the name index on an ext2 directory
    "ext2_bigdir": dict(size=24 * MIB, mkfs=["-t", "ext2", "-b", "1024", "-L", "ext2-bigdir", "-N", "6400"], trees=["many_small"]),
    "fz_nofiletype": dict(size=24 * MIB, mkfs=["-t", "ext2", "-b", "1024", "-O", "^filetype", "-L", "fz-nofiletype", "-N", "512"], trees=["basic", "types"]),
    "fz_bigdir": dict(size=24 * MIB, mkfs=["-t", "ext4", "-b", "1024", "-O", "^has_journal", "-L", "fz-bigdir", "-N", "6400"], trees=["many_small"]),
    "fz_bigdir_64k": dict(size=80 * MIB, mkfs=["-F", "-t", "ext4", "-b", "65536", "-O", "^has_journal", "-L", "fz-bigdir64", "-N", "6400"], trees=["many_small"]),
    "fz_metabg": dict(size=40 * MIB, mkfs=["-t", "ext4", "-b", "1024", "-O", "meta_bg,^resize_inode,64bit,^has_journal", "-L", "fz-metabg", "-N", "512"], trees=["basic", "types"]),
    "ext4_flex_off": dict(size=48 * MIB, mkfs=["-t", "ext4", "-b", "2048", "-O", "^flex_bg,^64bit,^has_journal,^metadata_csum", "-L", "noflex"], trees=["basic"]),
    # feature mixes: a directory without the filetype byte (the type comes from the inode), every group with a superblock copy, sparse_super2, inodes of 1 KiB, and the features that
    # only matter to a writer or to a particular directory (mmp, large_dir, ea_inode, a checksum seed, casefold, an orphan file, fast commits, stable inodes, verity)
    "ext2_nofiletype": dict(size=32 * MIB, mkfs=["-t", "ext2", "-b", "1024", "-O", "^filetype", "-L", "nofiletype", "-N", "512"], trees=["basic", "types"]),
    "ext2_nosparse": dict(size=40 * MIB, mkfs=["-t", "ext2", "-b", "1024", "-O", "^sparse_super,^resize_inode", "-L", "nosparse", "-N", "512"], trees=["basic"]),
    "ext4_sparse2": dict(size=32 * MIB, mkfs=["-t", "ext4", "-b", "2048", "-O", "sparse_super2,^resize_inode,^has_journal", "-L", "sparse2", "-N", "512"], trees=["basic"]),
    "ext4_isize1024": dict(size=32 * MIB, mkfs=["-t", "ext4", "-b", "4096", "-I", "1024", "-O", "^has_journal", "-L", "isize1024", "-N", "512"], trees=["basic"]),
    "ext4_featuremix": dict(size=32 * MIB, mkfs=["-t", "ext4", "-b", "4096", "-O", "mmp,large_dir,ea_inode,metadata_csum_seed,casefold,orphan_file,fast_commit,stable_inodes,verity",
                                                 "-L", "featuremix", "-N", "512"], extended="encoding=utf8", trees=["basic"]),
}

TREES = {"many_small": tree_many_small, "speed": tree_speed,"basic": tree_basic, "indirect": tree_indirect, "names": tree_names, "types": tree_types, "many": tree_many, "big": tree_big, "fragmented": tree_fragmented, "inline": tree_inline}


def kind_of(mode):
    return {stat.S_IFREG: "f", stat.S_IFDIR: "d", stat.S_IFLNK: "l", stat.S_IFCHR: "c", stat.S_IFBLK: "b", stat.S_IFIFO: "p", stat.S_IFSOCK: "s"}[stat.S_IFMT(mode)]


def manifest_of(src):
    """What the image must contain, from the source tree: {path: {...}}, and the hard-link groups."""
    files = {}
    src = Path(src)
    for dirpath, dirnames, filenames in os.walk(src):
        dirnames.sort()
        for name in sorted(dirnames + filenames):
            p = Path(dirpath) / name
            rel = "/" + str(p.relative_to(src)).replace(os.sep, "/")
            st = os.lstat(p)
            k = kind_of(st.st_mode)
            e = dict(kind=k, mode=st.st_mode & 0o7777, uid=st.st_uid, gid=st.st_gid, mtime=int(st.st_mtime), nlink=st.st_nlink, group=st.st_ino)
            if k == "f":
                e["size"] = st.st_size
                e["crc"] = file_crc(p)
                if st.st_size <= 128 * MIB:                      # the guest's md5sum is compared with this one (QEMU tests)
                    h = hashlib.md5()
                    with open(p, "rb") as f:
                        for chunk in iter(lambda: f.read(4 * MIB), b""):
                            h.update(chunk)
                    e["md5"] = h.hexdigest()
            elif k == "l":
                e["target"] = os.readlink(p)
                e["size"] = len(e["target"].encode())
            elif k == "d":
                e["size"] = None
            else:
                e["size"] = 0
            files[rel] = e
    return files


def sh_quote(s):
    return "'" + str(s).replace("'", "'\\''") + "'"


def debugfs_commands(img, cmds):
    """Runs debugfs -w with a command file."""
    with tempfile.NamedTemporaryFile("w", suffix=".cmd", delete=False) as t:
        t.write("\n".join(cmds) + "\n")
    try:
        subprocess.run(["debugfs", "-w", "-f", t.name, str(img)], check=True, capture_output=True)
    finally:
        os.unlink(t.name)


def profile_hash(profile):
    """What an image was made from: this profile's settings and the whole of this script apart from the table of profiles (so adding a profile does not remake the others)."""
    text = Path(__file__).read_bytes()
    a, b = text.index(b"\nPROFILES = {"), text.index(b"\nTREES = {")
    return hashlib.sha1(text[:a] + text[b:] + repr(sorted(PROFILES[profile].items())).encode()).hexdigest()


def build_one(profile, outdir):
    """Builds OUTDIR/PROFILE.img, .json (the manifest) and .gen (profile_hash) on Linux. Returns the manifest. Everything is made in a directory of the Linux file system and moved
    into OUTDIR at the end: mke2fs writes an image in many small pieces, which a Windows drive seen from WSL does slowly (a directory of 20000 files: tens of minutes)."""
    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix="e4stage-"))
    try:
        manifest = build_in(profile, stage)
        for ext in ("img", "json", "gen"):
            dest = outdir / f"{profile}.{ext}"
            if dest.exists():
                dest.unlink()
            shutil.move(str(stage / f"{profile}.{ext}"), str(dest))
    finally:
        shutil.rmtree(stage, ignore_errors=True)
    return manifest


def build_in(profile, outdir):
    spec = PROFILES[profile]
    outdir = Path(outdir)
    img = outdir / f"{profile}.img"
    seed = zlib.crc32(profile.encode())
    rnd = random.Random(seed)
    work = Path(tempfile.mkdtemp(prefix=f"e4src-{profile}-"))
    try:
        mk = spec["mkfs"]
        ctx = Ctx(int(mk[mk.index("-b") + 1]) if "-b" in mk else 4096, int(mk[mk.index("-I") + 1]) if "-I" in mk else 256)
        for t in spec["trees"]:
            TREES[t](work, rnd, ctx)
        if img.exists():
            img.unlink()
        size_k = spec["size"] // 1024
        extended = ",".join(x for x in (spec.get("extended"), "hash_seed=" + UUID) if x)
        cmd = ["mke2fs", "-q", "-F", *spec["mkfs"], "-U", UUID, "-E", extended, "-d", str(work), str(img), f"{size_k}k"]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError(f"mke2fs failed for {profile}:\n{' '.join(cmd)}\n{r.stdout}{r.stderr}")
        files = manifest_of(work)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if ctx.cmds:
        debugfs_commands(img, ctx.cmds)
    for path, fields in ctx.over.items():
        files[path].update(fields)
    files.update(ctx.add)
    fsck = subprocess.run(["e2fsck", "-fn", str(img)], capture_output=True, text=True)
    manifest = dict(profile=profile, files=files, fsck_status=fsck.returncode, fsck_output=fsck.stdout + fsck.stderr, uuid=UUID, size=spec["size"], mkfs=spec["mkfs"])
    (outdir / f"{profile}.json").write_text(json.dumps(manifest, indent=0))
    (outdir / f"{profile}.gen").write_text(profile_hash(profile))
    return manifest


def ensure(outdir, profiles):
    """For the tests: makes sure OUTDIR/PROFILE.img, .json and .gen exist and were made by this version of the script for this profile (builds the others in Linux, in WSL on
    Windows). Returns {profile: manifest}."""
    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    def current(p):
        gen = outdir / f"{p}.gen"
        return (outdir / f"{p}.img").exists() and (outdir / f"{p}.json").exists() and gen.exists() and gen.read_text() == profile_hash(p)

    todo = [p for p in profiles if not current(p)]
    if todo:
        script = Path(__file__).resolve()
        if os.name == "nt":
            cmd = f"python3 {sh_quote(wsl_path(script))} build {sh_quote(wsl_path(outdir))} " + " ".join(todo)
        else:
            cmd = f"{sh_quote(sys.executable)} {sh_quote(script)} build {sh_quote(outdir)} " + " ".join(todo)
        r = linux(cmd, check=False, capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError(f"building {todo} failed:\n{r.stdout}{r.stderr}")
    return {p: json.loads((outdir / f"{p}.json").read_text()) for p in profiles}


def main(argv):
    if len(argv) >= 1 and argv[0] == "list":
        for name, spec in PROFILES.items():
            print(f"{name:16s} {spec['size'] // MIB:6d} MiB  {' '.join(spec['mkfs'])}  trees: {', '.join(spec['trees'])}")
        return 0
    if len(argv) >= 3 and argv[0] == "build":
        for p in argv[2:]:
            m = build_one(p, argv[1])
            print(f"{p}: {len(m['files'])} entries, e2fsck status {m['fsck_status']}")
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
