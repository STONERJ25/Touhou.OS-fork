"""The consistency check, the repair program and the write order of the FAT driver.

  fsck_c_*      the C checker (shared/fatck.c, built into a host DLL by tools/fatck_host.py) against tools/mkdisk.py's independent
                Python checker, over deliberately damaged images: they must agree on what counts as damage        (no VM)
  fsck_c_repair_*  the repair engine (shared/fatfix.c, what /bin/fsck runs) in the same DLL: every damage of the menu repaired with -y and
                -a on FAT12/16/32 (and 4096-byte sectors), the ways two chains can meet, no room for FOUND.000, a power cut in the
                middle of the repair, random damage                                                                                (no VM)
  fsck_kernel_* the kernel's read-only check of a volume that was not shut down cleanly: read-write when nothing is out of place,
                read-only with the precise reason when something is; hvforce / hvcheck=off
  fsck_cmd_*    /bin/fsck itself in the guest: the options and exit codes on ten volumes of one disk, the refusals, the questions, the
                flow the kernel's message promises (umount, fsck -a, restart)
  (the power cuts at random moments of a write-heavy workload are tests/case_storagecrash.py: storage_crash_*)

Run just these with:  python tests/run_all.py fsck            (python tools/qemu-slot.py -- ... : the fsck_kernel_* and fsck_cmd_* ones boot VMs)
Without a VM:         python tests/run_all.py fsck_c_         (mind the underscore: "fsck_c" alone also selects fsck_cmd_*, which boot VMs)
"""
import contextlib
import os
import random
import re
import shutil
import struct
import time

import mkdisk
import fatck_host
from case_storage import (Image, boot, drive_args, hv_part, klog, MIB, WORK, HV, H, ROOT_PROMPT, sha)
from harness import test, scaled

SCRATCH = WORK / "fsck"


def scratch(name):
    SCRATCH.mkdir(parents=True, exist_ok=True)
    return SCRATCH / name


# ---------------------------------------------------------------------------------------------------------------------
# damage: the same menu for the host-side agreement test and for the kernel / fsck tests

FILES = {
    "/reimu/hello.txt": b"hello from the Human Village\n",
    "/reimu/notes/a very long file name here.txt": b"x" * 5000,
    "/marisa/spell.dat": bytes(range(256)) * 280,
    "/deep/a/b/c/d/e.txt": b"deep\n",
    "/sakuya/empty": b"",
    "/UPPER.TXT": b"u",
    "/lower.txt": b"l",
    "/Mixed Case.Txt": b"mc",
}
for _i in range(40):
    FILES[f"/many/file-{_i:03d}-with-a-longish-name.txt"] = b"f%d\n" % _i


def small_files(kind):
    """The file set, with the 70 KB file shrunk for volumes too small to hold it (FAT12)."""
    return FILES if kind != "fat12" else {k: (v if len(v) < 20000 else b"m" * 9000) for k, v in FILES.items()}


def build_image(path, kind="fat32", files=None, size=None, sector_size=512, **part):
    size = size or {"fat32": 64 * MIB, "fat16": 40 * MIB, "fat12": 4 * MIB}[kind]
    mkdisk.build_image(path, size, [dict(name="HUMANVILLAGE", kind=kind, size="rest", label="HUMANVILLAGE", files=files or small_files(kind), **part)],
                       sector_size=sector_size)
    return path


def make_dirty(r):
    """The same as mkdisk.set_dirty, on an open writable volume."""
    w = r.win
    for i in range(r.nfats if r.bits != 12 else 0):
        base = (r.reserved + i * r.fatsz) * r.bps
        if r.bits == 32:
            v = struct.unpack("<I", w.read(base + 4, 4))[0]
            w.write(base + 4, struct.pack("<I", v & ~0x08000000))
        else:
            v = struct.unpack("<H", w.read(base + 2, 2))[0]
            w.write(base + 2, struct.pack("<H", v & ~0x8000))
    off = 65 if r.bits == 32 else 37
    w.write(off, bytes([w.read(off, 1)[0] | 1]))


def eoc_of(r):
    return {32: 0x0FFFFFFF, 16: 0xFFFF, 12: 0xFFF}[r.bits]


def free_clusters(r, n=6):
    out = []
    for c in range(2, r.nclusters + 2):
        if r.fat_get(c) == 0:
            out.append(c)
            if len(out) == n:
                break
    return out


# name -> (expected class on both checkers, function(r) that damages an open writable FatReader)
def damage_menu(r):
    """Deliberate damage for a volume laid out by FILES. `r` is a writable mkdisk.FatReader; returns {name: (class, fn)}."""
    bits = r.bits
    nclusters = r.nclusters
    eoc = eoc_of(r)
    spell = r.find("/marisa/spell.dat")
    sch = r.chain(spell.first)
    longf = r.find("/reimu/notes/a very long file name here.txt")
    many = r.find("/many")
    mch = r.chain(many.first)
    free = free_clusters(r)
    fat1_off = (r.reserved + r.fatsz) * r.bps

    def fat_pos(copy, c):
        base = (r.reserved + copy * r.fatsz) * r.bps
        return base + (c * 4 if bits == 32 else c * 2 if bits == 16 else c + c // 2)

    m = {}
    m["hole in a chain"] = ("err", lambda r: r.set_fat(sch[3], 0))
    m["chain loops"] = ("err", lambda r: r.set_fat(sch[3], sch[0]))
    m["cross-link"] = ("err", lambda r: r.set_entry("/reimu/hello.txt", first=sch[2]))
    m["bad cluster in a chain"] = ("err", lambda r: r.set_fat(sch[2], {32: 0x0FFFFFF7, 16: 0xFFF7, 12: 0xFF7}[bits]))
    m["reserved FAT value in a chain"] = ("err", lambda r: r.set_fat(sch[2], {32: 0x0FFFFFF3, 16: 0xFFF3, 12: 0xFF3}[bits]))
    m["chain leaves the volume"] = ("err", lambda r: r.set_fat(sch[2], nclusters + 5 if bits != 12 else 0xFF2))
    m["size larger than the chain"] = ("err", lambda r: r.set_entry("/marisa/spell.dat", size=spell.size + 3 * r.cluster_bytes))
    m["size but no cluster"] = ("err", lambda r: r.set_entry("/reimu/hello.txt", first=0))
    m["first cluster outside the volume"] = ("err", lambda r: r.set_entry("/reimu/hello.txt", first=nclusters + 100))

    def dir_size(r):
        r.win.write(r.entry_offset("/reimu/notes") + 28, struct.pack("<I", 4096))
    m["directory with a size"] = ("err", dir_size)
    m["directory without a first cluster"] = ("err", lambda r: r.set_entry("/reimu/notes", first=0))
    m["'..' wrong"] = ("err", lambda r: r.win.write(r.cluster_off(r.find("/reimu/notes").first) + 32 + 26, struct.pack("<H", 7)))
    m["'.' wrong"] = ("err", lambda r: r.win.write(r.cluster_off(r.find("/reimu/notes").first) + 26, struct.pack("<H", 9)))
    m["dots erased"] = ("err", lambda r: r.win.write(r.cluster_off(r.find("/reimu/notes").first), bytes(64)))

    def duplicate(r):
        d = r.find("/reimu")
        raw = r.win.read(r.entry_offset("/reimu/hello.txt"), 32)
        chain = r.chain(d.first)
        data = r.read_clusters(chain)
        for i in range(0, len(data), 32):
            if data[i] == 0:
                r.win.write(r.cluster_off(chain[i // r.cluster_bytes]) + i % r.cluster_bytes, raw)
                break
    m["duplicate entry"] = ("err", duplicate)
    m["long name without its entry"] = ("err", lambda r: r.win.write(r.entry_offset("/reimu/notes/a very long file name here.txt"), b"\xe5"))
    m["illegal character in an 8.3 name"] = ("err", lambda r: r.win.write(r.entry_offset("/reimu/hello.txt") + 2, b"*"))
    m["control character in an 8.3 name"] = ("err", lambda r: r.win.write(r.entry_offset("/UPPER.TXT") + 1, b"\x01"))
    m["volume label in a subdirectory"] = ("err", lambda r: r.win.write(r.entry_offset("/reimu/hello.txt") + 11, b"\x08"))
    m["long name checksum mismatch"] = ("err", lambda r: r.win.write(r.entry_offset("/reimu/notes/a very long file name here.txt") + 3, b"Q"))
    m["long name entry with a cluster field"] = ("err", lambda r: r.win.write(r.entry_offset("/reimu/notes/a very long file name here.txt") - 32 + 26, b"\x05\x00"))
    m["FAT copy 2 differs (clean flags)"] = ("err", lambda r: r.win.write(fat_pos(1, free[0]), b"\xff" * (2 if bits != 12 else 1)))
    m["boot sector without signature"] = ("err", lambda r: r.win.write(510, b"\x00\x00"))
    m["bytes per sector 768"] = ("err", lambda r: r.win.write(11, struct.pack("<H", 768)))
    m["sectors per cluster 3"] = ("err", lambda r: r.win.write(13, b"\x03"))
    m["no FATs"] = ("err", lambda r: r.win.write(16, b"\x00"))
    m["FAT too small for the volume"] = ("err", lambda r: r.win.write(36 if bits == 32 else 22, struct.pack("<I" if bits == 32 else "<H", 1)))
    if bits == 32:
        info = lambda r: r.fsinfo_sector * r.bps
        m["FSInfo free count (clean flags)"] = ("err", lambda r: r.win.write(info(r) + 488, struct.pack("<I", 12345)))
        m["FSInfo signature"] = ("err", lambda r: r.win.write(info(r), b"XXXX"))
        m["FSInfo next-free hint"] = ("err", lambda r: r.win.write(info(r) + 492, struct.pack("<I", 0x0FFFFFF0)))
        m["root chain broken"] = ("err", lambda r: r.set_fat(r.root_cluster, 0))
        m["directory chain loops"] = ("err", lambda r: r.set_fat(mch[-1], mch[0]))
        m["total sectors beyond the device"] = ("err", lambda r: r.win.write(32, struct.pack("<I", 0x7FFFFFFF)))
        m["backup boot sector differs"] = ("warn", lambda r: r.win.write(6 * r.bps + 20, b"\x99"))

    # leftovers a power cut can produce on a dirty volume
    def lost_chain(r):
        r.set_fat(free[0], free[1]); r.set_fat(free[1], free[2]); r.set_fat(free[2], eoc)
        make_dirty(r)
    m["lost chain (dirty)"] = ("leak", lost_chain)
    m["size smaller than the chain"] = ("leak", lambda r: r.set_entry("/marisa/spell.dat", size=spell.size - 3 * r.cluster_bytes))
    m["empty file with a cluster"] = ("leak", lambda r: r.set_entry("/reimu/hello.txt", size=0))

    def garbage(r):
        off = r.cluster_off(mch[-1])
        data = bytearray(r.win.read(off, r.cluster_bytes))
        for i in range(0, len(data) - 96, 32):
            if data[i] == 0:
                data[i + 64:i + 96] = b"G" * 32
                break
        r.win.write(off, bytes(data))
    m["bytes after the end marker"] = ("leak", garbage)

    def copy2_dirty(r):
        r.win.write(fat_pos(1, free[0]), b"\xff" * (2 if bits != 12 else 1))
        make_dirty(r)
    m["FAT copy 2 lags (dirty)"] = ("leak", copy2_dirty)
    if bits == 32:
        def fsinfo_dirty(r):
            r.win.write(r.fsinfo_sector * r.bps + 488, struct.pack("<I", 12345))
            make_dirty(r)
        m["FSInfo lags (dirty)"] = ("leak", fsinfo_dirty)

    # cosmetic
    m["long name padding"] = ("warn", lambda r: r.win.write(r.entry_offset("/reimu/notes/a very long file name here.txt") - 96 + 30, b"\x41\x00"))
    m["long name type byte"] = ("warn", lambda r: r.win.write(r.entry_offset("/reimu/notes/a very long file name here.txt") - 32 + 12, b"\x01"))
    m["lower case in an 8.3 name"] = ("warn", lambda r: r.win.write(r.entry_offset("/UPPER.TXT"), b"u"))
    m["NT reserved bits"] = ("warn", lambda r: r.win.write(r.entry_offset("/UPPER.TXT") + 12, b"\x20"))
    m["reserved attribute bits"] = ("warn", lambda r: r.win.write(r.entry_offset("/UPPER.TXT") + 11, b"\x60"))
    m["month 13"] = ("warn", lambda r: r.win.write(r.entry_offset("/UPPER.TXT") + 16, struct.pack("<H", (46 << 9) | (13 << 5) | 1)))
    m["FAT[0] media byte"] = ("warn", lambda r: r.win.write(fat_pos(0, 0), b"\xf0"))
    m["dirty flags only"] = ("warn", make_dirty)
    m["nothing"] = ("ok", lambda r: None)
    return m


def py_class(path, index=1):
    """What tools/mkdisk.py's checker says about partition `index`: ok / warn / leak / err."""
    try:
        with open(path, "rb") as f:
            ss, parts = mkdisk.partitions(mkdisk.Window(f))
            part = [p for p in parts if p["index"] == index][0]
            w = mkdisk.Window(f, part["offset"], part["length"])
            try:
                rep = mkdisk.FatReader(w, name=f"p{index}").check()
            except mkdisk.MkdiskError as ex:
                return "err", [str(ex)]
    except mkdisk.MkdiskError as ex:
        return "err", [str(ex)]
    if rep.serious:
        return "err", rep.serious
    if rep.benign:
        return "leak", rep.benign
    return ("warn" if rep.warnings else "ok"), rep.warnings


def c_class(path, index=1):
    out = fatck_host.scan_volume(path, index)
    r = out.result
    if not r.opened or r.errors or r.incomplete:
        cls = "err"
    elif r.leaks:
        cls = "leak"
    elif r.warnings:
        cls = "warn"
    else:
        cls = "ok"
    return cls, out


def same_class(py, c):
    """The checkers must agree; the one tolerated difference is an empty file that owns clusters (a warning in Python, a leftover in C)."""
    return py == c or (py == "warn" and c == "leak")


# ---------------------------------------------------------------------------------------------------------------------
# the C checker against the Python checker (no VM)

@test
def fsck_c_agrees_with_python():
    """The C checker (the kernel's and fsck's) and mkdisk.py's Python checker agree, on FAT12, FAT16 and FAT32, about every kind of
    deliberate damage: damage, harmless leftovers of a power cut, cosmetics, nothing."""
    bad = []
    for kind in ("fat32", "fat16", "fat12"):
        base = build_image(scratch(f"agree-{kind}.img"), kind)
        r = mkdisk.open_volume(base, 1)
        try:
            menu = damage_menu(r)
        finally:
            r.close()
        for name, (expect, fn) in menu.items():
            q = scratch(f"agree-{kind}-bad.img")
            shutil.copyfile(base, q)
            r = mkdisk.open_volume(q, 1, writable=True)
            try:
                fn(r)
                r.win.f.flush()
            finally:
                r.close()
            py, why = py_class(q)
            cc, out = c_class(q)
            if not same_class(py, cc) or cc != expect:
                bad.append(f"{kind}: {name}: expected {expect}, python says {py}, C says {cc}\n    py: {why[:2]}\n    C: {out.text()[:400]}")
            q.unlink()
        base.unlink()
    assert not bad, f"{len(bad)} disagreement(s):\n" + "\n".join(bad)


@test
def fsck_c_agrees_on_random_damage():
    """The same, over a few hundred randomly damaged images (flipped FAT bytes, wild FAT entries, trashed directory slots, boot sector
    bytes, FSInfo, dirty flags): the verdict classes must match and the C checker must never crash or hang."""
    rng = random.Random(2024)
    bases = {k: build_image(scratch(f"fuzz-base-{k}.img"), k) for k in ("fat32", "fat16", "fat12")}
    stats = {}
    bad = []
    for i in range(240):
        kind = rng.choice(list(bases))
        q = scratch("fuzz.img")
        seed = rng.randrange(1 << 30)
        what = random_damage(bases[kind], q, random.Random(seed))
        py, why = py_class(q)
        cc, out = c_class(q)
        stats[(py, cc)] = stats.get((py, cc), 0) + 1
        if not same_class(py, cc):
            keep = scratch(f"fuzz-disagree-{kind}-{seed}.img")
            shutil.copyfile(q, keep)
            bad.append(f"{kind} seed {seed} {what}: python {py}, C {cc} ({keep.name})\n    py: {why[:2]}\n    C: {out.text()[:400]}")
    for p in bases.values():
        p.unlink()
    assert not bad, f"{len(bad)} disagreement(s) in 240 cases {stats}:\n" + "\n".join(bad)
    assert stats.get(("err", "err"), 0) > 20 and stats.get(("leak", "leak"), 0) > 5, stats      # the damage really bites


DAMAGE_KINDS = ["fatbyte", "fatentry", "dirbyte", "dirslot", "dirslot2", "boot", "fsinfo", "dirty", "copy2"]
# what the repair fuzz uses: no boot-sector bytes (a volume whose geometry went wrong is refused or meaningless, see REFUSED), and two kinds that
# aim at the files themselves: an entry that takes another file's first cluster (a cross-link, whichever of the two the scan meets first) and a
# wrong size field
REPAIR_KINDS = [k for k in DAMAGE_KINDS if k != "boot"] + ["retarget", "resize"]


def random_damage(src, dst, rng, kinds=DAMAGE_KINDS):
    """Copies src to dst and damages it at random; returns a short description."""
    shutil.copyfile(src, dst)
    r = mkdisk.open_volume(dst, 1, writable=True)
    what = []
    try:
        bits, w = r.bits, r.win
        esz = 4 if bits == 32 else 2
        for _ in range(rng.choice([1, 1, 2, 3, 4])):
            kind = rng.choice(kinds)
            try:
                if kind == "fatbyte":
                    used = (r.nclusters + 2) * esz if bits != 12 else (r.nclusters + 2) * 3 // 2
                    off = r.reserved * r.bps + (rng.randrange(min(used, 4096)) if rng.random() < 0.8 else rng.randrange(used))
                    for i in range(r.nfats if rng.random() < 0.6 else 1):
                        w.write(off + i * r.fatsz * r.bps, bytes([rng.randrange(256)]))
                elif kind == "fatentry":
                    c = rng.randrange(2, 2 + min(r.nclusters, 300))
                    v = rng.choice([0, 0, 1, 2, c, c + 1, r.nclusters + 1, r.nclusters + 2, 0xFFFFFFF, 0x0FFFFFF7, 0xFFF, 0xFF7, 0xFFFF, 0xFFF7,
                                    rng.randrange(2, 2 + min(r.nclusters, 300))])
                    r.set_fat(c, v & {32: 0x0FFFFFFF, 16: 0xFFFF, 12: 0xFFF}[bits])
                elif kind in ("dirbyte", "dirslot", "dirslot2"):
                    d = rng.choice(["/", "/reimu", "/reimu/notes", "/marisa", "/many", "/deep/a/b"])
                    if d == "/" and bits != 32:
                        total = r.root_entries * 32
                        loc = lambda o: (r.reserved + r.nfats * r.fatsz) * r.bps + o
                    else:
                        chain = r.chain(r.root_cluster if d == "/" else r.find(d).first)
                        total = len(chain) * r.cluster_bytes
                        loc = lambda o, chain=chain: r.cluster_off(chain[o // r.cluster_bytes]) + o % r.cluster_bytes
                    hot = min(total, 1024)
                    o = rng.randrange(hot) if rng.random() < 0.8 else rng.randrange(total)
                    if kind == "dirbyte":
                        w.write(loc(o), bytes([rng.randrange(256)]))
                    elif kind == "dirslot":
                        w.write(loc(o & ~31), rng.choice([bytes(32), b"\xe5" + bytes(31), bytes(rng.randrange(256) for _ in range(32))]))
                    else:
                        w.write(loc(o & ~31), w.read(loc(rng.randrange(hot) & ~31), 32))
                elif kind == "boot":
                    w.write(rng.choice(list(range(11, 62)) + [510, 511]), bytes([rng.randrange(256)]))
                elif kind == "fsinfo" and bits == 32:
                    w.write(r.fsinfo_sector * r.bps + rng.choice([0, 1, 484, 488, 489, 492, 508, 100]), bytes([rng.randrange(256)]))
                elif kind == "dirty":
                    make_dirty(r)
                elif kind == "copy2" and r.nfats > 1:
                    c = rng.randrange(2, 2 + min(r.nclusters, 300))
                    w.write(r.reserved * r.bps + r.fatsz * r.bps + (c * esz if bits != 12 else c + c // 2), bytes([rng.randrange(256)]))
                elif kind == "retarget":
                    names = [p for p in FILES if p != "/sakuya/empty"]
                    a, b = rng.choice(names), rng.choice(names)
                    r.set_entry(a, first=rng.choice(r.chain(r.find(b).first)))
                elif kind == "resize":
                    p = rng.choice(list(FILES))
                    size = r.find(p).size
                    r.set_entry(p, size=rng.choice([0, 1, size + 1, size * 2 + 7, max(0, size - r.cluster_bytes), rng.randrange(0, 1 << 20)]))
                what.append(kind)
            except (mkdisk.MkdiskError, AttributeError, IndexError, struct.error):
                pass                                            # an earlier damage made the helper's own lookup fail
        w.f.flush()
    finally:
        r.close()
    return what


# ---------------------------------------------------------------------------------------------------------------------
# the repair engine (shared/fatfix.c, what /bin/fsck runs) on the host: every damage of the menu on FAT12/16/32, -y and -a (no VM)

FIX_ALL, FIX_SAFE = fatck_host.FIX_ALL, fatck_host.FIX_SAFE

# Damage to the boot sector or the geometry: the checker refuses such a volume and the repair never writes to it.
REFUSED = {"boot sector without signature", "bytes per sector 768", "sectors per cluster 3", "no FATs", "FAT too small for the volume",
           "total sectors beyond the device"}

# name of a damage -> (the files it is aimed at: they may change, be renamed or come back as FOUND.000/FILEnnnn.CHK; anything below a
#                      directory listed here is aimed at as well,
#                      whether `-a` is left with an error that needs a decision (-y),
#                      what must survive of the aimed files: "all" = every byte, somewhere (in the file, under another name, in a recovered
#                      chain); "prefix" = what is left of the file is the start of the original; None = nothing is asked)
# Everything else on the volume has to be byte for byte what it was.
P_HELLO = "/reimu/hello.txt"
P_SPELL = "/marisa/spell.dat"
P_LONG = "/reimu/notes/a very long file name here.txt"
AIMED = {
    "hole in a chain": ([P_SPELL], True, "all"),
    "chain loops": ([P_SPELL], True, "all"),
    "cross-link": ([P_HELLO], False, "all"),
    "bad cluster in a chain": ([P_SPELL], True, "prefix"),                 # the cluster that is marked bad is dropped, the rest is saved
    "reserved FAT value in a chain": ([P_SPELL], True, "prefix"),
    "chain leaves the volume": ([P_SPELL], True, "prefix"),                # (FAT12: the value written there is a reserved one)
    "size larger than the chain": ([P_SPELL], False, "all"),
    "size but no cluster": ([P_HELLO], False, "all"),
    "first cluster outside the volume": ([P_HELLO], True, "all"),
    "directory with a size": ([], False, None),
    "directory without a first cluster": (["/reimu/notes"], True, "all"),
    "'..' wrong": ([], False, None),
    "'.' wrong": ([], False, None),
    "dots erased": ([], False, None),
    "duplicate entry": ([], False, None),                             # an exact copy of an entry: removing it loses nothing
    "long name without its entry": ([P_LONG], False, "all"),
    "illegal character in an 8.3 name": ([P_HELLO], True, "all"),
    "control character in an 8.3 name": (["/UPPER.TXT"], True, "all"),
    "volume label in a subdirectory": ([P_HELLO], True, "all"),
    "long name checksum mismatch": ([P_LONG], False, "all"),             # the file stays under its short name
    "long name entry with a cluster field": ([], False, None),
    "FAT copy 2 differs (clean flags)": ([], False, None),
    "FSInfo free count (clean flags)": ([], False, None),
    "FSInfo signature": ([], False, None),
    "FSInfo next-free hint": ([], False, None),
    "root chain broken": ([], True, None),
    "directory chain loops": ([], True, None),
    "backup boot sector differs": ([], False, None),
    "lost chain (dirty)": ([], False, None),
    "size smaller than the chain": ([P_SPELL], False, "prefix"),
    "empty file with a cluster": ([P_HELLO], False, "prefix"),
    "bytes after the end marker": ([], False, None),
    "FAT copy 2 lags (dirty)": ([], False, None),
    "FSInfo lags (dirty)": ([], False, None),
    "long name padding": ([], False, None),
    "long name type byte": ([], False, None),
    "lower case in an 8.3 name": (["/UPPER.TXT"], False, "all"),
    "NT reserved bits": ([], False, None),
    "reserved attribute bits": ([], False, None),
    "month 13": ([], False, None),
    "FAT[0] media byte": ([], False, None),
    "dirty flags only": ([], False, None),
    "nothing": ([], False, None),
}


def parent_of(path):
    return path.rsplit("/", 1)[0] or "/"


def under(path, roots):
    return any(path == q or path.startswith(q.rstrip("/") + "/") for q in roots)


def volume_files(path, index=1):
    """({path: bytes} of every file of partition `index` that can be read, {paths of the directories}) -- what a reader of the volume sees."""
    r = mkdisk.open_volume(path, index)
    files, dirs = {}, set()
    try:
        for ents in r._tree_cache().values():
            for e in ents:
                if e.is_dir:
                    dirs.add(e.path)
                    continue
                try:
                    files[e.path] = r.read_file(e.path)
                except mkdisk.MkdiskError:
                    pass                                              # a chain that does not hold the file
    finally:
        r.close()
    return files, dirs


def volume_flagged_clean(path, index=1):
    r = mkdisk.open_volume(path, index)
    try:
        return r.check().stats["clean"]
    finally:
        r.close()


def recovered(orig, path, after):
    """Are the bytes of the file `path` (`orig`) still on the volume: in the file, under another name in its directory, or in a recovered chain?"""
    if not orig:
        return True
    here = after.get(path)
    near = [b for p, b in after.items() if p == path or parent_of(p) == parent_of(path) or p.startswith("/FOUND.000/")]
    if any(b.startswith(orig) for b in near):
        return True
    if here is not None and orig.startswith(here):                  # cut where the chain broke: the rest is in a recovered chain
        rest = orig[len(here):]
        return any(b.startswith(rest) for p, b in after.items() if p.startswith("/FOUND.000/"))
    return False


def compare_trees(before, after, aimed, strict_new=True):
    """What changed between two (files, dirs) that was not aimed at."""
    (bf, bd), (af, ad) = before, after
    bad = []
    for p, data in sorted(bf.items()):
        if under(p, aimed):
            continue
        if p not in af:
            bad.append(f"{p} is gone")
        elif af[p] != data:
            bad.append(f"{p} changed ({len(data)} -> {len(af[p])} bytes)")
    for p in sorted(bd):
        if not under(p, aimed) and p not in ad:
            bad.append(f"directory {p} is gone")
    homes = {parent_of(q) for q in aimed}
    for p in sorted((set(af) | ad) - set(bf) - bd):
        if p == "/FOUND.000" or p.startswith("/FOUND.000/") or parent_of(p) in homes or under(p, aimed) or not strict_new:
            continue
        bad.append(f"{p} appeared")
    return bad


def damaged_copy(base, fn, q):
    """Copies the image `base` to `q` and damages partition 1 with fn(r)."""
    shutil.copyfile(base, q)
    r = mkdisk.open_volume(q, 1, writable=True)
    try:
        fn(r)
        r.win.f.flush()
    finally:
        r.close()


def repair_case(base, before, name, fn, aimed, leaves, keep, mode, check=None, free_lost=False, work=None, cut_after=None):
    """Damages a copy of `base` with fn(r), repairs it with `mode`, and returns a list of complaints (empty = all well).
    cut_after=k: the first repair is cut by a power failure after k sectors have been written, and the real one is the second run."""
    q = work or scratch("repair-case.img")
    damaged_copy(base, fn, q)
    tag = f"{name} [{'-y' if mode == FIX_ALL else '-a'}{'' if cut_after is None else f', cut after sector {cut_after}'}]"
    bad = []
    h0 = sha(q.read_bytes())
    if name != "nothing" and name not in REFUSED and c_class(q)[0] == "ok":
        return [f"{tag}: the damage did not show: the checker finds the volume in order"]
    if cut_after is not None:
        fatck_host.fix_volume_cut(q, 1, mode, write_budget=cut_after, free_lost=free_lost)
    rc, st, lines, out = fatck_host.fix_volume(q, 1, mode, free_lost=free_lost)
    if name in REFUSED:
        if out.result.opened or rc == 0:
            bad.append(f"{tag}: a volume with a damaged boot sector must be refused (rc {rc})")
        if sha(q.read_bytes()) != h0:
            bad.append(f"{tag}: a refused volume was written to")
        return bad
    if rc != 0:
        bad.append(f"{tag}: the repair stopped with {rc}: {lines[-3:]}")
        return bad
    py, why = py_class(q)
    cc, o = c_class(q)
    left = (not st.clean_now) if leaves is None else (leaves and mode == FIX_SAFE)       # (None: either outcome is fine, but the engine and the checkers must agree)
    if left:
        if py != "err" or cc != "err" or not st.declined or st.clean_now:
            bad.append(f"{tag}: -a was to leave an error that needs a decision, but python says {py}, C says {cc}, declined {st.declined}, clean {st.clean_now}")
    else:
        if py not in ("ok", "warn") or cc not in ("ok", "warn") or not st.clean_now:
            bad.append(f"{tag}: not clean afterwards: python says {py} {why[:2]}, C says {cc} {o.text()[:300]}, lines {lines[-4:]}")
        elif not volume_flagged_clean(q):
            bad.append(f"{tag}: repaired, but the volume is still flagged dirty")
    after = volume_files(q)
    bad += [f"{tag}: {m}" for m in compare_trees(before, after, aimed)]
    if keep and not left:
        for p, data in sorted(before[0].items()):
            if not under(p, aimed):
                continue
            if keep == "all" and not recovered(data, p, after[0]):
                bad.append(f"{tag}: the {len(data)} bytes of {p} are nowhere on the volume any more")
            elif keep == "prefix" and p in after[0] and not data.startswith(after[0][p]):
                bad.append(f"{tag}: {p} is not the start of what it was")
    if check:
        bad += [f"{tag}: {m}" for m in check(before, after, mode)]
    h1 = sha(q.read_bytes())
    rc2, st2, lines2, out2 = fatck_host.fix_volume(q, 1, mode, free_lost=free_lost)
    if sha(q.read_bytes()) != h1 or st2.repaired or st2.wrote:
        bad.append(f"{tag}: a second run changed things: {lines2[:3]}")
    return bad


def repair_menu(kind):
    base = build_image(scratch(f"repair-{kind}.img"), kind)
    before = volume_files(base)
    r = mkdisk.open_volume(base, 1)
    try:
        menu = damage_menu(r)
    finally:
        r.close()
    bad = []
    for name, (expect, fn) in menu.items():
        if name not in AIMED and name not in REFUSED:
            bad.append(f"{name}: the damage menu has a new entry that tests/case_fsck.py does not know (AIMED / REFUSED)")
            continue
        aimed, leaves, keep = AIMED.get(name, ([], False, None))
        for mode in (FIX_ALL, FIX_SAFE):
            bad += repair_case(base, before, name, fn, aimed, leaves, keep, mode)
    base.unlink()
    return bad


@test
def fsck_c_repair_fat32():
    """fsck's repair engine, on FAT32: every damage of the menu repaired with -y (everything) and -a (what loses nothing). Afterwards both checkers say ok or
    warn, the dirty flags are clear, no file that was not aimed at has changed by a byte, a file that lost its entry (or its clusters) is saved in
    FOUND.000 with its bytes, `-a` leaves exactly the damage that needs a decision, and a second run changes nothing."""
    bad = repair_menu("fat32")
    assert not bad, f"{len(bad)} problem(s):\n" + "\n".join(bad)


@test
def fsck_c_repair_fat16():
    """The same on FAT16."""
    bad = repair_menu("fat16")
    assert not bad, f"{len(bad)} problem(s):\n" + "\n".join(bad)


@test
def fsck_c_repair_fat12():
    """The same on FAT12."""
    bad = repair_menu("fat12")
    assert not bad, f"{len(bad)} problem(s):\n" + "\n".join(bad)


@test
def fsck_c_repair_lost_file_comes_back():
    """A file whose directory entry is gone comes back as FOUND.000/FILE0000.CHK with its exact bytes (and nothing but zeros to fill its last cluster), on
    FAT12, FAT16 and FAT32, with -a as well as with -y; its long name, which belonged to the entry, does not."""
    bad = []
    for kind in ("fat32", "fat16", "fat12"):
        base = build_image(scratch(f"lostfile-{kind}.img"), kind)
        before = volume_files(base)
        r = mkdisk.open_volume(base, 1)
        try:
            cb, fn = r.cluster_bytes, damage_menu(r)["long name without its entry"][1]
        finally:
            r.close()
        orig = before[0][P_LONG]
        for mode in (FIX_SAFE, FIX_ALL):
            q = scratch(f"lostfile-{kind}-bad.img")
            damaged_copy(base, fn, q)
            rc, st, lines, out = fatck_host.fix_volume(q, 1, mode)
            files, dirs = volume_files(q)
            chk = sorted(p for p in files if p.startswith("/FOUND.000/"))
            data = files.get("/FOUND.000/FILE0000.CHK", b"")
            tag = f"{kind} {'-y' if mode == FIX_ALL else '-a'}"
            if chk != ["/FOUND.000/FILE0000.CHK"]:
                bad.append(f"{tag}: the recovered files are {chk}")
            elif data[:len(orig)] != orig or any(data[len(orig):]) or len(data) % cb or len(data) - len(orig) >= cb:
                bad.append(f"{tag}: FILE0000.CHK is {len(data)} bytes, the file was {len(orig)}; the bytes differ or the padding is not zeros")
            if P_LONG in files:
                bad.append(f"{tag}: the file is still under its old path")
            q.unlink()
        base.unlink()
    assert not bad, "\n".join(bad)


# --- damage the menu does not have: the ways two chains can meet, and many lost files at once

def put_entry(r, dir_path, raw):
    """Writes a raw 32-byte entry into the first free slot of a directory."""
    chain = r.chain(r.find(dir_path).first)
    data = r.read_clusters(chain)
    for i in range(0, len(data), 32):
        if data[i] == 0:
            r.win.write(r.cluster_off(chain[i // r.cluster_bytes]) + i % r.cluster_bytes, raw)
            return
    raise AssertionError(f"{dir_path} has no free slot")


def x_cross_other_way(r):                       # spell.dat takes hello.txt's cluster: now hello.txt is the one the scan flags, and it is innocent
    r.set_entry(P_SPELL, first=r.find(P_HELLO).first)


def x_twin(r):                                  # a second name for hello.txt's clusters
    raw = bytearray(r.win.read(r.entry_offset(P_HELLO), 32))
    raw[0:11] = b"TWIN    TXT"
    put_entry(r, "/reimu", bytes(raw))


def x_dir_into_file(r):                         # a directory entry that points into the middle of a file
    r.set_entry("/reimu/notes", first=r.chain(r.find(P_SPELL).first)[2])


def x_end_into_start(r):                        # the last cluster of spell.dat links on to hello.txt (the scan meets hello.txt first)
    r.set_fat(r.chain(r.find(P_SPELL).first)[-1], r.find(P_HELLO).first)


def x_start_into_end(r):                        # hello.txt's only cluster links on to spell.dat's first (hello.txt "owns" the whole file; spell.dat is flagged)
    r.set_fat(r.find(P_HELLO).first, r.find(P_SPELL).first)


def x_break_behind_end(r):                      # the file ends in its second cluster, but its chain goes on and then breaks (a trim that a power failure cut short)
    chain = r.chain(r.find(P_SPELL).first)
    r.set_entry(P_SPELL, size=2 * r.cluster_bytes - 100)
    r.set_fat(chain[min(len(chain) - 1, 5)], 0)


def x_lost_loop(r):                             # lost clusters that run round in a circle
    f = free_clusters(r)
    r.set_fat(f[0], f[1]); r.set_fat(f[1], f[2]); r.set_fat(f[2], f[0])
    make_dirty(r)


def x_lost_into_file(r):                        # a lost cluster that points into a file
    r.set_fat(free_clusters(r)[0], r.chain(r.find(P_SPELL).first)[2])
    make_dirty(r)


def x_many_lost(r):                             # the directory with 40 files vanishes: 41 lost chains, FOUND.000 has to grow
    r.set_entry("/many", first=0)


def check_twin(before, after, mode):
    got, want = after[0].get("/reimu/twin.txt"), before[0][P_HELLO]
    return [] if got == want else [f"the second name has {None if got is None else len(got)} bytes, hello.txt had {len(want)}"]


def check_many_saved(before, after, mode):
    saved = [b for p, b in after[0].items() if p.startswith("/FOUND.000/")]
    return [f"{p}: {len(b)} bytes are not among the recovered files" for p, b in before[0].items()
            if p.startswith("/many/") and not any(s.startswith(b) for s in saved)]


# name -> (damage, aimed, -a leaves an error, what survives of the aimed files, extra check)
SCENARIOS = {
    "cross-link, the other way round": (x_cross_other_way, [P_SPELL], False, "all", None),
    "a second name for one chain": (x_twin, ["/reimu/twin.txt"], False, None, check_twin),
    "a directory entry points into a file": (x_dir_into_file, ["/reimu/notes"], None, None, None),      # (which of the two is flagged depends on the order of the scan)
    "the end of one file links to the start of another": (x_end_into_start, [], False, None, None),
    "the start of one file links to another file": (x_start_into_end, [], False, None, None),
    "a chain breaks behind the end of its file": (x_break_behind_end, [P_SPELL], False, "prefix", None),        # nothing is lost by cutting it: -a does that
    "lost clusters in a circle (dirty)": (x_lost_loop, [], False, None, None),
    "a lost cluster that points into a file (dirty)": (x_lost_into_file, [], False, None, None),
    "a directory with 40 files vanishes": (x_many_lost, ["/many"], True, "all", check_many_saved),
}


def lost_chain(r, marker, n=3):
    """n free clusters turned into a chain nobody owns, the first holding `marker`; flags the volume dirty like a power cut."""
    f = free_clusters(r, n + 3)[3:]                                  # (not the first few: they are where the volume's own allocations start)
    for a, b in zip(f, f[1:]):
        r.set_fat(a, b)
    r.set_fat(f[-1], eoc_of(r))
    r.win.write(r.cluster_off(f[0]), marker)
    make_dirty(r)
    return f


@test
def fsck_c_repair_root_full():
    """No room for FOUND.000 (the fixed root directory of a FAT16 volume is full): `-a` says so, fails that one repair without giving up on the rest,
    and leaves the lost clusters and the dirty flag alone; with `-F` the same lost clusters are given back and the volume is clean."""
    files = dict(FILES)
    files.update({f"/x{i}.txt": b"x" for i in range(5)})
    base = build_image(scratch("rootfull.img"), "fat16", files=files, root_entries=16)
    before = volume_files(base)
    r = mkdisk.open_volume(base, 1)
    try:
        assert r.root_entries == 16 and sum(1 for i in range(0, 16 * 32, 32) if r.root_bytes()[i] == 0) == 0, "the root directory is not full"
    finally:
        r.close()
    q = scratch("rootfull-bad.img")
    shutil.copyfile(base, q)
    r = mkdisk.open_volume(q, 1, writable=True)
    try:
        lost = lost_chain(r, b"LOST")
        r.win.f.flush()
    finally:
        r.close()
    h0 = sha(q.read_bytes())
    rc, st, lines, out = fatck_host.fix_volume(q, 1, FIX_SAFE)
    assert rc == 0, (rc, lines)                                     # not an abort
    assert st.failed >= 1 and not st.clean_now and any("cannot make FOUND.000" in ln for ln in lines), (st.failed, lines)
    assert out.result.lost_clusters == len(lost), out.text()        # still there for the next try
    assert volume_files(q) == before, "a failed repair changed the files"
    shutil.copyfile(base, q)
    r = mkdisk.open_volume(q, 1, writable=True)
    try:
        lost = lost_chain(r, b"LOST")
        r.win.f.flush()
    finally:
        r.close()
    rc, st, lines, out = fatck_host.fix_volume(q, 1, FIX_SAFE, free_lost=True)
    assert rc == 0 and st.clean_now and st.freed_clusters == len(lost), (rc, st.clean_now, st.freed_clusters, lines)
    assert volume_files(q) == before and py_class(q)[0] == "ok", py_class(q)


@test
def fsck_c_repair_found_numbering():
    """A second run on a volume that already has FOUND.000 goes on numbering where the first stopped, and keeps what is in the old files."""
    base = build_image(scratch("numbering.img"), "fat16")
    q = scratch("numbering-bad.img")
    shutil.copyfile(base, q)
    for round_, marker in enumerate((b"ONE-ONE-ONE", b"TWO-TWO-TWO")):
        r = mkdisk.open_volume(q, 1, writable=True)
        try:
            lost_chain(r, marker, 2 + round_)
            r.win.f.flush()
        finally:
            r.close()
        rc, st, lines, out = fatck_host.fix_volume(q, 1, FIX_SAFE)
        assert rc == 0 and st.clean_now and st.lost_chains == 1, (rc, st.clean_now, st.lost_chains, lines)
    files, dirs = volume_files(q)
    chk = sorted(p for p in files if p.startswith("/FOUND.000/"))
    assert chk == ["/FOUND.000/FILE0000.CHK", "/FOUND.000/FILE0001.CHK"], chk
    assert files[chk[0]].startswith(b"ONE-ONE-ONE") and files[chk[1]].startswith(b"TWO-TWO-TWO"), [files[c][:12] for c in chk]
    r = mkdisk.open_volume(q, 1)
    try:
        cb = r.cluster_bytes
    finally:
        r.close()
    assert len(files[chk[0]]) == 2 * cb and len(files[chk[1]]) == 3 * cb, [len(files[c]) for c in chk]


@test
def fsck_c_repair_fsinfo_pointer():
    """The boot sector of a FAT32 volume names the backup boot sector as its FSInfo sector (a damaged pointer): the repair must not write an FSInfo
    structure over the backup boot sector, says why it cannot mend the FSInfo, and goes on with the rest."""
    base = build_image(scratch("fsinfo-ptr.img"), "fat32")
    q = scratch("fsinfo-ptr-bad.img")
    shutil.copyfile(base, q)
    r = mkdisk.open_volume(q, 1, writable=True)
    try:
        backup = r.win.read(r.backup_sector * r.bps, r.bps)
        r.win.write(48, struct.pack("<H", r.backup_sector))
        make_dirty(r)
        sec = r.backup_sector
        r.win.f.flush()
    finally:
        r.close()
    rc, st, lines, out = fatck_host.fix_volume(q, 1, FIX_ALL)
    assert rc == 0 and st.failed >= 1 and not st.clean_now and any("cannot rewrite the FSInfo sector" in ln for ln in lines), (rc, st.failed, lines)
    r = mkdisk.open_volume(q, 1)
    try:
        assert r.win.read(sec * r.bps, r.bps) == backup, "the backup boot sector was overwritten"
    finally:
        r.close()


@test
def fsck_c_repair_4kn():
    """The same menu on a FAT16 volume with 4096-byte sectors, where the host session insists on whole-sector reads and writes (as a 4Kn disk does)."""
    base = build_image(scratch("4kn.img"), "fat16", size=64 * MIB, sector_size=4096)
    before = volume_files(base)
    r = mkdisk.open_volume(base, 1)
    try:
        assert r.bps == 4096, r.bps
        menu = damage_menu(r)
    finally:
        r.close()
    bad = []
    for name, (expect, fn) in menu.items():
        aimed, leaves, keep = AIMED.get(name, ([], False, None))
        for mode in (FIX_ALL, FIX_SAFE):
            bad += repair_case(base, before, name, fn, aimed, leaves, keep, mode)
    assert not bad, f"{len(bad)} problem(s):\n" + "\n".join(bad)


@test
def fsck_c_repair_interrupted():
    """The power fails in the middle of fsck itself: for every damage of the menu, and every way two chains can meet, on FAT16 and FAT12 the repair is cut after
    some of the sectors it writes (the first, the last, the middle, and three at random: writes are torn sector by sector, in the order the engine hands them
    over) and run again. The second run must end as an uninterrupted one does: clean, no file that was not aimed at changed, nothing aimed at lost.
    TOUHOU_FSCK_CUT_POINTS=N random points instead of 3 (and FAT32 as well when N is more than 6)."""
    points = int(os.environ.get("TOUHOU_FSCK_CUT_POINTS", "3"))
    rng = random.Random(4711)
    bad, runs = [], 0
    for kind in ("fat16", "fat12") + (("fat32",) if points > 6 else ()):
        base = build_image(scratch(f"cut-{kind}.img"), kind)
        before = volume_files(base)
        r = mkdisk.open_volume(base, 1)
        try:
            menu = damage_menu(r)
        finally:
            r.close()
        cases = [(name, fn, *AIMED[name], None) for name, (expect, fn) in menu.items() if name not in REFUSED]
        cases += [(name, fn, aimed, leaves, keep, check) for name, (fn, aimed, leaves, keep, check) in SCENARIOS.items()]
        for name, fn, aimed, leaves, keep, check in cases:
            for mode in (FIX_ALL, FIX_SAFE):
                dry = scratch("cut-dry.img")
                damaged_copy(base, fn, dry)
                (_rc, _st, _lines, _out), total, _cut = fatck_host.fix_volume_cut(dry, 1, mode)
                if not total:
                    continue
                for k in sorted({0, total - 1, total // 2} | {rng.randrange(total) for _ in range(points)}):
                    bad += [f"{kind}: {m}" for m in repair_case(base, before, name, fn, aimed, leaves, keep, mode, check=check, cut_after=k)]
                    runs += 1
        base.unlink()
    assert not bad, f"{len(bad)} problem(s) in {runs} interrupted repairs:\n" + "\n".join(bad[:40])


# --- random damage, then repair: the engine must always finish, leave a clean volume (-y) or only the damage that needs a decision (-a), and lose nothing

FUZZ_LEFT_BY_A = {"chain", "dirchain", "range", "cross", "nocluster", "dirbig", "dup", "name", "label", "dots", "lfnpad"}


def damaged_files(path):
    try:
        return volume_files(path)
    except (mkdisk.MkdiskError, struct.error, IndexError, KeyError, AttributeError):
        return {}, set()                                            # the reader of the volume itself gives up on it


def fuzz_repair(mode, count, seed0):
    bases = {k: build_image(scratch(f"rfuzz-{k}.img"), k) for k in ("fat32", "fat16", "fat12")}
    befores = {k: volume_files(p) for k, p in bases.items()}
    bad, stats = [], {}
    for i in range(count):
        seed = seed0 + i
        kind = random.Random(seed).choice(list(bases))
        q = scratch("rfuzz.img")
        what = random_damage(bases[kind], q, random.Random(seed), kinds=REPAIR_KINDS)
        tag = f"{kind} seed {seed} {'-y' if mode == FIX_ALL else '-a'} {what}"
        dam = damaged_files(q)[0]
        rc, st, lines, out = fatck_host.fix_volume(q, 1, mode)
        if rc != 0 or not out.result.opened:
            bad.append(f"{tag}: the repair did not finish: rc {rc} {lines[-3:]}")
            continue
        py, why = py_class(q)
        cc, o = c_class(q)
        if mode == FIX_ALL:
            if py not in ("ok", "warn") or cc not in ("ok", "warn") or not st.clean_now:
                bad.append(f"{tag}: not clean: python {py} {why[:2]}; C {cc} {o.text()[:300]}")
        else:
            left = {fatck_host.FK_NAMES[f.code] for f, _ in out.findings if f.sev >= fatck_host.FS_LEAK}
            if left - FUZZ_LEFT_BY_A:
                bad.append(f"{tag}: -a left {sorted(left - FUZZ_LEFT_BY_A)} behind, which it can mend: {o.text()[:300]}")
        stats[(py, cc)] = stats.get((py, cc), 0) + 1
        h1 = sha(q.read_bytes())
        rc2, st2, lines2, _ = fatck_host.fix_volume(q, 1, mode)
        if sha(q.read_bytes()) != h1 or st2.repaired or st2.wrote:
            bad.append(f"{tag}: a second run changed things: {lines2[:3]}")
        if mode == FIX_ALL or st.clean_now:
            after = volume_files(q)[0]
            recovered_bytes = [b for p, b in after.items() if p.startswith("/FOUND.000/")]
            for p, b in dam.items():
                if befores[kind][0].get(p) == b and b and not (b in after.values() or any(c.startswith(b) for c in recovered_bytes)):
                    bad.append(f"{tag}: {p} was fine and its {len(b)} bytes are gone now: {lines[:5]}")
    for p in bases.values():
        p.unlink()
    return bad, stats


@test
def fsck_c_repair_fuzz_y():
    """400 random damages (flipped FAT bytes and entries, trashed or copied directory slots, an entry taking another file's cluster, a wrong size, FSInfo,
    dirty flags): `-y` finishes every time, both checkers call the result clean, a second run changes nothing, and every file the damage left alone is
    still there byte for byte (or saved whole in FOUND.000). TOUHOU_FSCK_FUZZ=N runs more, TOUHOU_FSCK_SEED=S starts elsewhere."""
    n = int(os.environ.get("TOUHOU_FSCK_FUZZ", "400"))
    bad, stats = fuzz_repair(FIX_ALL, n, int(os.environ.get("TOUHOU_FSCK_SEED", "9000")))
    assert not bad, f"{len(bad)} problem(s) in {n} cases {stats}:\n" + "\n".join(bad[:40])


@test
def fsck_c_repair_fuzz_a():
    """The same with `-a`: afterwards only damage that needs a decision may be left (broken chains, entries that point nowhere, repeated or unusable
    names, stray labels, cross-linked directories), nothing the file set had is lost, and a second run changes nothing."""
    n = int(os.environ.get("TOUHOU_FSCK_FUZZ", "400")) // 2
    bad, stats = fuzz_repair(FIX_SAFE, n, int(os.environ.get("TOUHOU_FSCK_SEED", "9000")) + 50000)
    assert not bad, f"{len(bad)} problem(s) in {n} cases {stats}:\n" + "\n".join(bad[:40])


@test
def fsck_c_repair_scenarios():
    """Chains that meet in ways the damage menu does not have (the innocent file is the one the scan flags; two names for one chain; a file's end
    linked to another file; a directory entry into a file; lost clusters in a circle or pointing into a file) and a directory with 40 files
    that vanishes (FOUND.000 has to grow): on FAT12/16/32 with -y and -a nothing that was not aimed at may change by a byte."""
    bad = []
    for kind in ("fat32", "fat16", "fat12"):
        base = build_image(scratch(f"scen-{kind}.img"), kind)
        before = volume_files(base)
        for name, (fn, aimed, leaves, keep, check) in SCENARIOS.items():
            for mode in (FIX_ALL, FIX_SAFE):
                bad += [f"{kind}: {m}" for m in repair_case(base, before, name, fn, aimed, leaves, keep, mode, check)]
        base.unlink()
    assert not bad, f"{len(bad)} problem(s):\n" + "\n".join(bad)


# ---------------------------------------------------------------------------------------------------------------------
# the kernel's check of a volume that was not shut down cleanly

def dirty_image(name, mutate=None, size=64 * MIB, files=None, **part):
    """An Image with a HUMANVILLAGE volume that is flagged dirty, after `mutate(r)` damaged it (r: writable mkdisk.FatReader)."""
    img = Image(name, size, [hv_part(files if files is not None else FILES, **part)])
    r = mkdisk.open_volume(img.path, 1, writable=True)
    try:
        if mutate:
            mutate(r)
        make_dirty(r)
        r.win.f.flush()
    finally:
        r.close()
    return img


@test
def fsck_kernel_tidy():
    """A dirty flag and nothing else wrong (the power went out between two writes): Keine goes through the ledger, finds nothing out of
    place and mounts the volume read-write; the flag is gone after the first sync, and the kernel log reaches the stick."""
    with dirty_image("fsck-tidy") as img:
        with boot(img, root=True, name="t-fsck-tidy", bootlog=True) as (g, vm):
            log = klog(vm)
            assert "vda1 was not closed properly: Keine goes through the whole ledger" in log, log[-2000:]
            assert re.search(r"nothing is out of place: the Human Village may write in it again", log), log[-2000:]
            assert re.search(r"fat32: vda1 mounted on /HumanVillage.*read-write: the consistency check found nothing out of place", log)
            assert re.search(r"/dev/vda1 /HumanVillage fat32 rw ", g.ok("cat /proc/mounts"))
            fc = g.ok("cat /proc/fatcheck")
            assert "vda1: FAT32, flagged dirty: in order" in fc and "0 error(s), 0 harmless leftover(s)" in fc, fc
            g.ok(f"echo written after the check > {H}/after.txt")
            g.ok("sync")
            g.poweroff()
        img.check()                                                  # the flag was cleared again: no warnings
        v = img.volume(1)
        try:
            assert v.read_file("/reimu/after.txt") == b"written after the check\n"
            assert b"nothing is out of place" in v.read_file("/TOUHOU.LOG"), "the kernel log did not reach the volume"
        finally:
            v.close()


@test
def fsck_kernel_leftovers():
    """Harmless leftovers of a power cut (a lost chain, a file whose chain is longer than its size, a FAT copy and FSInfo that lag, bytes
    after a directory's end marker): the volume is mounted read-write, the flag stays until fsck, and the surplus chain is trimmed when
    the file is next written."""
    holder = {}

    def mutate(r):
        free = free_clusters(r)
        eoc = eoc_of(r)
        spell = r.find("/marisa/spell.dat")
        holder["size"] = spell.size - 3 * r.cluster_bytes
        r.set_fat(free[0], free[1]); r.set_fat(free[1], free[2]); r.set_fat(free[2], eoc)
        r.set_entry("/marisa/spell.dat", size=holder["size"])
        r.win.write(r.fsinfo_sector * r.bps + 488, struct.pack("<I", 12345))
        r.win.write((r.reserved + r.fatsz) * r.bps + free[3] * 4, b"\xff\xff\xff\x0f")
        mch = r.chain(r.find("/many").first)
        off = r.cluster_off(mch[-1])
        data = bytearray(r.win.read(off, r.cluster_bytes))
        for i in range(0, len(data) - 96, 32):
            if data[i] == 0:
                data[i + 64:i + 96] = b"G" * 32
                break
        r.win.write(off, bytes(data))

    with dirty_image("fsck-left", mutate) as img:
        want = FILES["/marisa/spell.dat"][:holder["size"]]
        with boot(img, root=True, name="t-fsck-left") as (g, vm):
            log = klog(vm)
            assert re.search(r"vda1 was left untidy and Keine found only harmless leftovers in the ledger \(\d+ lost cluster", log), log[-2500:]
            assert re.search(r"fat32: vda1 mounted on /HumanVillage.*read-write: the consistency check found only harmless leftovers", log)
            assert re.search(r"/dev/vda1 /HumanVillage fat32 rw ", g.ok("cat /proc/mounts"))
            fc = g.ok("cat /proc/fatcheck")
            assert "vda1: FAT32, flagged dirty: in order" in fc and re.search(r"\d+ harmless leftover", fc) and "leftover: " in fc, fc
            assert "fsck -a /dev/vda1" in fc, fc
            assert g.ok(f"sha256sum {HV}/marisa/spell.dat").split()[0] == sha(want)
            g.ok(f"printf tail >> {HV}/marisa/spell.dat")                       # the surplus clusters are given back, then the file grows
            assert g.ok(f"sha256sum {HV}/marisa/spell.dat").split()[0] == sha(want + b"tail")
            g.ok(f"echo fine > {HV}/marisa/new.txt; sync")
            g.poweroff()
        rep = mkdisk.check_image(img.path)
        assert not rep.serious, rep.text()
        assert not rep.stats["p1"]["clean"], "the leftovers are still there: the flag must stay until fsck has tidied them"
        v = img.volume(1)
        try:
            assert v.read_file("/marisa/spell.dat") == want + b"tail" and v.read_file("/marisa/new.txt") == b"fine\n"
        finally:
            v.close()


def damage_cases():
    """(name, mutate, regexes that must appear in the log line for the damage)"""
    def holes(r):
        sch = r.chain(r.find("/marisa/spell.dat").first)
        r.set_fat(sch[3], 0)                                                          # a hole in the middle of a chain
        r.set_entry("/reimu/hello.txt", first=sch[1])                                   # and a file that points into it (it is read first): cross-link

    def dots_and_size(r):
        n = r.find("/reimu/notes")
        r.win.write(r.cluster_off(n.first) + 32 + 26, struct.pack("<H", 7))             # '..' of /reimu/notes points nowhere
        r.set_entry("/marisa/spell.dat", size=r.find("/marisa/spell.dat").size + 4 * r.cluster_bytes)      # size beyond its chain

    def loop(r):
        sch = r.chain(r.find("/marisa/spell.dat").first)
        r.set_fat(sch[5], sch[1])                                                       # a chain that loops back
    return [
        ("holes", holes, [r"free cluster", r"cross-linked"]),
        ("dots", dots_and_size, [r"'\.\.' points to cluster 7", r"needs \d+ cluster\(s\) but the chain has"]),
        ("loop", loop, [r"cross-linked or loops back"]),
    ]


@test
def fsck_kernel_damage():
    """A dirty volume with real damage (a hole in a chain, a cross-link, a wrong '..', a size beyond the chain, a loop) stays read-only:
    the log, /proc/fatcheck and the first lines say precisely what is wrong and how to mend it, nothing is written, the rest is readable."""
    for name, mutate, wants in damage_cases():
        with dirty_image(f"fsck-damage-{name}", mutate) as img:
            h0 = img.sha()
            with boot(img, root=True, name=f"t-fsck-damage-{name}", bootlog=True) as (g, vm):
                log = klog(vm)
                assert re.search(r"vda1 was left untidy and its ledger does not add up: \d+ error\(s\)\. It stays read-only", log), log[-2500:]
                assert "As root, `umount /HumanVillage` and run `fsck -a /dev/vda1`" in log, log[-2500:]
                for rx in wants:
                    assert re.search(rx, log), (rx, log[-2500:])
                assert re.search(r"/dev/vda1 /HumanVillage fat32 ro ", g.ok("cat /proc/mounts"))
                assert re.search(r"fat32: vda1 mounted on /HumanVillage.*read-only: the volume was not cleanly unmounted", log)
                fc = g.ok("cat /proc/fatcheck")
                assert "vda1: FAT32, flagged dirty: DAMAGED" in fc and "fix: as root" in fc, fc
                for rx in wants:
                    assert re.search(rx, fc), (rx, fc)
                assert g.ok(f"cat {HV}/lower.txt").strip() == "l"                      # the rest of the volume reads fine
                g.fails(f"echo no > {HV}/new.txt", r"[Rr]ead-only")
                assert "TOUHOU.LOG is not kept" in klog(vm)
                g.ok("sync")
                g.poweroff()
            assert img.sha() == h0, "a read-only mount wrote to the volume"


@test
def fsck_kernel_force_and_off():
    """`hvforce` mounts a damaged dirty volume read-write anyway and says so; `hvcheck=off` skips the check (dirty = read-only, as before)
    and together with hvforce mounts read-write without a word about the ledger."""
    def mutate(r):
        sch = r.chain(r.find("/marisa/spell.dat").first)
        r.set_fat(sch[3], 0)
    with dirty_image("fsck-force", mutate) as img:
        with boot(img, root=True, name="t-fsck-force", cmdline="hvforce") as (g, vm):
            log = klog(vm)
            assert "hvforce: vda1 is mounted read-write although its ledger does not add up" in log, log[-2500:]
            assert re.search(r"fat32: vda1 mounted on /HumanVillage.*FORCED read-write", log), log[-2500:]
            assert re.search(r"/dev/vda1 /HumanVillage fat32 rw ", g.ok("cat /proc/mounts"))
            g.ok(f"echo forced > {HV}/lower2.txt; sync")
            g.poweroff()
        rep = mkdisk.check_image(img.path)
        assert not rep.stats["p1"]["clean"], "a forced mount must not clear a dirty flag it did not set"
    with dirty_image("fsck-force2", mutate) as img:
        with boot(img, root=True, name="t-fsck-force2", cmdline="hvcheck=off hvforce") as (g, vm):
            log = klog(vm)
            assert "Keine goes through the whole ledger" not in log and "hvforce:" not in log, log[-2500:]
            assert re.search(r"fat32: vda1 mounted on /HumanVillage.*FORCED read-write", log), log[-2500:]
            assert "vda1" not in g.ok("cat /proc/fatcheck")
            g.poweroff()
    with dirty_image("fsck-off", mutate) as img:
        h0 = img.sha()
        with boot(img, root=True, name="t-fsck-off", cmdline="hvcheck=off") as (g, vm):
            assert re.search(r"fat32: vda1 mounted on /HumanVillage.*read-only: the volume was not cleanly unmounted", klog(vm))
            assert "Keine goes through the whole ledger" not in klog(vm)
            g.fails(f"echo no > {HV}/x.txt", r"[Rr]ead-only")
            g.poweroff()
        assert img.sha() == h0


@test
def fsck_kernel_limits():
    """A hostile or huge dirty volume costs time, not safety: nesting beyond the depth limit is reported (read-only), and a 12 GiB volume
    with 3 million clusters is gone through in well under the time bound."""
    deep = "/".join(["d"] * 300) + "/f.txt"
    with dirty_image("fsck-deep", files={"/reimu/a.txt": b"a", deep: b"deep"}) as img:
        with boot(img, root=True, name="t-fsck-deep") as (g, vm):
            log = klog(vm)
            assert re.search(r"directories nested more than 256 deep", log), log[-2500:]
            assert re.search(r"check of its ledger could not be finished", log), log[-2500:]
            assert re.search(r"/dev/vda1 /HumanVillage fat32 ro ", g.ok("cat /proc/mounts"))
            g.poweroff()
    with dirty_image("fsck-big", size=12 * 1024 * MIB, files={"/reimu/a.txt": b"a", "/marisa/b.txt": b"b" * 100000}, cluster=4096) as img:
        with boot(img, root=True, name="t-fsck-big", mem="768M") as (g, vm):
            log = klog(vm)
            m = re.search(r"went through the whole ledger in (\d+) ms", log)
            assert m and int(m.group(1)) < 60000, log[-2500:]
            assert re.search(r"/dev/vda1 /HumanVillage fat32 rw ", g.ok("cat /proc/mounts"))
            g.poweroff()


# ---------------------------------------------------------------------------------------------------------------------
# /bin/fsck in the guest: one disk of small volumes, each damaged in its own way, gone through with every option from a root shell

def menu_steps(*names):
    """A damage function: applies the named entries of damage_menu() one after the other."""
    def fn(r):
        menu = damage_menu(r)
        for n in names:
            menu[n][1](r)
    return fn


# name, kind, size, damage; the n-th is /dev/vda<n> in the guest. The last one is the tenth: a mounted vda10 must not stop fsck from looking at vda1.
CMD_VOLUMES = [
    ("clean32", "fat32", 40 * MIB, None),
    ("flag12", "fat12", 4 * MIB, menu_steps("dirty flags only")),
    ("left16", "fat16", 8 * MIB, menu_steps("lost chain (dirty)", "size smaller than the chain", "FAT copy 2 lags (dirty)", "bytes after the end marker")),
    ("decide32", "fat32", 40 * MIB, menu_steps("hole in a chain", "illegal character in an 8.3 name", "lost chain (dirty)", "bytes after the end marker")),
    ("dots12", "fat12", 4 * MIB, menu_steps("dots erased", "directory with a size", "long name without its entry")),
    ("y12", "fat12", 4 * MIB, menu_steps("hole in a chain", "cross-link")),
    ("free16", "fat16", 8 * MIB, menu_steps("lost chain (dirty)")),
    ("ask16", "fat16", 8 * MIB, menu_steps("hole in a chain", "lost chain (dirty)")),
    ("notfat", "raw", 2 * MIB, None),
    ("mnt16", "fat16", 8 * MIB, None),
]
CMD_ONE = {name: i + 1 for i, (name, *_rest) in enumerate(CMD_VOLUMES)}


def cmd_image(name):
    """An Image of CMD_VOLUMES with the damage applied; returns it and the path of an untouched copy (the host's own run of the same repairs
    is made on that copy, to compare with what the guest did)."""
    parts = []
    for vname, kind, size, _dmg in CMD_VOLUMES:
        if kind == "raw":
            parts.append(dict(name=vname.upper(), kind="raw", size=size))
        else:
            parts.append(dict(name=vname.upper(), kind=kind, size=size, label=vname.upper(), files=small_files(kind)))
    img = Image(name, 170 * MIB, parts)
    for i, (vname, kind, size, dmg) in enumerate(CMD_VOLUMES, 1):
        if dmg is None:
            continue
        r = mkdisk.open_volume(img.path, i, writable=True)
        try:
            dmg(r)
            r.win.f.flush()
        finally:
            r.close()
    twin = scratch(f"{name}-twin.img")
    shutil.copyfile(img.path, twin)
    return img, twin


def gx(g, cmd, want_rc, *rxs, timeout=120):
    """Runs a command in the guest and requires its exit status and some patterns in what it printed; returns the output."""
    out, rc = g.run(cmd, timeout)
    assert rc == want_rc, f"`{cmd}` exited with {rc}, expected {want_rc}:\n{out}"
    for rx in rxs:
        assert re.search(rx, out), f"`{cmd}`: expected /{rx}/ in:\n{out}"
    return out


@test
def fsck_cmd_options_and_exit_codes():
    """/bin/fsck in the guest on ten volumes of one disk (FAT12/16/32; in order, dirty, with leftovers, with damage that needs a decision): -n changes
    nothing and exits 4 when something is wrong, -a/-p mend what loses nothing (exit 1) and say what they leave (exit 4), -y mends the rest, -F frees
    lost chains, and the refusals (a mounted volume, a disk with a mounted partition, not FAT, no such device, usage) exit 8 and 16. Afterwards the
    host looks at the disk: every volume is what the host's own run of the same repairs makes of it, and both checkers agree it is clean."""
    img, twin = cmd_image("fsck-cmd")
    v = CMD_ONE
    dev = lambda name: f"/dev/vda{v[name]}"
    with img:
        with boot(img, root=True, name="t-fsck-cmd") as (g, vm):
            lsblk = g.ok("lsblk")
            assert re.search(r"^vda10\s", lsblk, re.M), lsblk
            # a volume that is in order
            gx(g, f"fsck -n {dev('clean32')}", 0, r"FAT32, \d+ clusters of 512 bytes", r"the ledger is in order")
            gx(g, f"fsck -v -n {dev('clean32')}", 0, r"the ledger is in order")
            out = gx(g, f"fsck -a {dev('clean32')}", 0, r"the ledger is in order")                                 # nothing to do: exit 0, no repair, nothing written
            assert "fixed" not in out and "repair(s) made" not in out, out
            # only the dirty flag
            gx(g, f"fsck -n {dev('flag12')}", 0, r"FAT12", r"nothing is wrong, but the volume is flagged dirty")
            gx(g, f"fsck -a {dev('flag12')}", 1, r"the ledger is in order and the volume is marked clean")
            gx(g, f"fsck -n {dev('flag12')}", 0, r"the ledger is in order")
            # leftovers of a power cut: -n looks (exit 4), without a terminal it looks too, -a mends (exit 1)
            gx(g, f"fsck -n {dev('left16')}", 4, r"FAT16", r"leftover .*lost cluster", r"leftover .*chain", r"nothing was changed\. Run `fsck -a /dev/vda3`")
            gx(g, f"fsck {dev('left16')} < /dev/null", 4, r"no terminal to ask on: behaving like -n")
            out = gx(g, f"fsck -q -n {dev('left16')}", 4, r"\d+ leftover\(s\) of a power cut")                     # -q: the summary, not the findings
            assert not re.search(r"^(leftover|error|warning) ", out, re.M), out
            gx(g, f"fsck -a {dev('left16')}", 1, r"fixed: saved a lost chain of \d+ cluster\(s\) as FOUND\.000/FILE0000\.CHK",
               r"fixed: gave back the clusters behind the end of a file", r"1 lost chain\(s\) of \d+ cluster\(s\) saved in FOUND\.000",
               r"the ledger is in order and the volume is marked clean")
            gx(g, f"fsck -n {dev('left16')}", 0, r"the ledger is in order")
            # damage that needs a decision: -a mends what it may and exits 4, -y does the rest
            gx(g, f"fsck -n {dev('decide32')}", 4, r"\d+ error\(s\)", r"chain runs into a free cluster", r"illegal character")
            out = gx(g, f"fsck -a {dev('decide32')}", 4, r"repair\(s\) were not allowed: -y allows them", r"error\s+/marisa/spell\.dat: chain runs into a free cluster")
            assert "marked clean" not in out, out
            gx(g, f"fsck -y {dev('decide32')}", 1, r"fixed: cut a broken chain at its break", r"fixed: mended an unusable 8\.3 name",
               r"the ledger is in order and the volume is marked clean")
            gx(g, f"fsck -n {dev('decide32')}", 0, r"the ledger is in order")
            # -p is -a; the damage to a directory's own entries
            gx(g, f"fsck -p {dev('dots12')}", 1, r"fixed: wrote '\.' and '\.\.' where they were missing", r"the ledger is in order")     # (not dirty: no "marked clean")
            gx(g, f"fsck -n {dev('dots12')}", 0)
            gx(g, f"fsck -y {dev('y12')}", 1, r"fixed: ", r"the ledger is in order")
            gx(g, f"fsck -n {dev('y12')}", 0)
            # -F: lost chains are given back instead of saved
            out = gx(g, f"fsck -a -F {dev('free16')}", 1, r"1 lost chain\(s\) of \d+ cluster\(s\) freed")
            assert "FOUND.000/" not in out, out
            gx(g, f"fsck -n {dev('free16')}", 0)
            # refusals
            gx(g, f"fsck -n {dev('notfat')}", 8, r"no boot sector signature", r"is not a FAT volume that can be checked")
            gx(g, "fsck -n /dev/vdz9", 8, r"cannot open /dev/vdz9")
            gx(g, "fsck", 16, r"usage: fsck")
            gx(g, "fsck -n", 16, r"usage: fsck")
            gx(g, f"fsck -Z {dev('clean32')}", 16, r"usage: fsck")
            g.ok("mkdir -p /tmp/m")
            g.ok(f"mount -t fat32 {dev('mnt16')} /tmp/m")
            gx(g, f"fsck -n {dev('mnt16')}", 8, r"is mounted \(/dev/vda10 on /tmp/m\)")
            gx(g, "fsck -n /dev/vda", 8, r"is mounted \(/dev/vda10 on /tmp/m\)")
            gx(g, "fsck -n vda10", 8, r"is mounted")
            gx(g, f"fsck -n {dev('clean32')}", 0, r"the ledger is in order")              # vda10 is not a partition of vda1
            g.ok("umount /tmp/m")
            gx(g, f"fsck -n {dev('mnt16')}", 0, r"the ledger is in order")
            g.poweroff()
        # --- the host looks at the disk
        for name, steps in (("flag12", [(FIX_SAFE, False)]), ("left16", [(FIX_SAFE, False)]), ("decide32", [(FIX_SAFE, False), (FIX_ALL, False)]),
                            ("dots12", [(FIX_SAFE, False)]), ("y12", [(FIX_ALL, False)]), ("free16", [(FIX_SAFE, True)]), ("clean32", []), ("mnt16", [])):
            i = v[name]
            for mode, free_lost in steps:
                fatck_host.fix_volume(twin, i, mode, free_lost=free_lost)
            want, got = volume_files(twin, i), volume_files(img.path, i)
            assert got == want, (f"{name}: the guest's fsck left other files than the host's run of the same repairs: {sorted(set(got[0]) ^ set(want[0]))[:6]} "
                                 f"{[p for p in got[0] if p in want[0] and got[0][p] != want[0][p]][:6]}")
            py, why = py_class(img.path, i)
            cc, o = c_class(img.path, i)
            assert py in ("ok", "warn") and cc in ("ok", "warn"), f"{name}: python says {py} {why[:2]}, C says {cc} {o.text()[:300]}"
            assert volume_flagged_clean(img.path, i), f"{name}: still flagged dirty"
    twin.unlink()


@test
def fsck_cmd_4kn():
    """A disk with 4096-byte sectors (logical_block_size=4096) and a FAT16 volume with 4096-byte sectors on it: fsck asks the device for its sector size
    (BLKSSZGET), reads and writes whole sectors, and ends with what the host's own run of the same repairs makes of the volume."""
    img = Image("fsck-4kn", 72 * MIB, [dict(name="FSCK4K", kind="fat16", size="rest", label="FSCK4K", files=small_files("fat16"))], sector_size=4096)
    r = mkdisk.open_volume(img.path, 1, writable=True)
    try:
        assert r.bps == 4096, r.bps
        menu_steps("hole in a chain", "illegal character in an 8.3 name", "lost chain (dirty)", "bytes after the end marker")(r)
        r.win.f.flush()
    finally:
        r.close()
    twin = scratch("fsck-4kn-twin.img")
    shutil.copyfile(img.path, twin)
    with img:
        extra = drive_args(img.path, logical_block_size=4096, physical_block_size=4096)
        with boot(None, root=True, name="t-fsck-4kn", extra=extra) as (g, vm):
            assert re.search(r"^vda\d* \d+ 4096 ", g.ok("cat /proc/partitions"), re.M)
            gx(g, "fsck -n /dev/vda1", 4, r"FAT16, \d+ clusters of 4096 bytes", r"chain runs into a free cluster")
            gx(g, "fsck -v -n /dev/vda1", 4, r"in sectors of 4096 bytes \(the volume's own sectors: 4096\)")                  # (BLKSSZGET said 4096)
            gx(g, "fsck -a /dev/vda1", 4, r"repair\(s\) were not allowed: -y allows them")
            gx(g, "fsck -y /dev/vda1", 1, r"the ledger is in order and the volume is marked clean")
            gx(g, "fsck -n /dev/vda1", 0, r"the ledger is in order")
            g.poweroff()
        for mode in (FIX_SAFE, FIX_ALL):
            fatck_host.fix_volume(twin, 1, mode)
        assert volume_files(img.path) == volume_files(twin), "the guest's fsck on the 4Kn disk left other files than the host's run of the same repairs"
        assert py_class(img.path)[0] in ("ok", "warn") and c_class(img.path)[0] in ("ok", "warn") and volume_flagged_clean(img.path)
    twin.unlink()


@test
def fsck_cmd_not_root():
    """Only root may read or mend a disk: as an ordinary user fsck says so and exits 8 (and a usage error is still 16)."""
    img, twin = cmd_image("fsck-user")
    twin.unlink()
    with img:
        with boot(img, root=False, name="t-fsck-user") as (g, vm):
            gx(g, "fsck -n /dev/vda3", 8, r"cannot open /dev/vda3: Permission denied", r"only root may read or mend a disk")
            gx(g, "fsck -a /dev/vda3", 8, r"only root")
            gx(g, "fsck", 16, r"usage: fsck")
            g.poweroff()


@test
def fsck_cmd_asks():
    """Without -n/-a/-y, on a terminal, fsck asks before every repair: [y/N/a(ll)/q]. 'n' leaves a repair undone (and it is not asked twice), 'y' does
    it, 'q' stops without changing anything more (exit 32), 'a' says yes to the rest."""
    img, twin = cmd_image("fsck-ask")
    twin.unlink()
    dev = f"/dev/vda{CMD_ONE['ask16']}"
    prompt = r"\[y/N/a\(ll\)/q\] "

    def run_interactive(vm, answer):
        """Runs fsck on the terminal; `answer(question) -> 'y' | 'n' | 'a' | 'q'`. Returns (what it printed, exit status, [the questions])."""
        vm.send(f"fsck {dev}; echo __R''C__$?\n")
        start, asked = vm._pos, []
        while True:
            before = vm._pos
            m = vm.expect(prompt + r"|__RC__(\d+)", timeout=120)
            if m.group(1) is not None:
                break
            q = "\n".join(vm.text()[before:m.start()].replace("\r", "").rstrip("\n").split("\n")[-2:])        # the question and its "repair it?" line
            asked.append(q)
            vm.send(answer(q) + "\n")
        out = vm.text()[start:m.end()]
        vm.expect(ROOT_PROMPT, timeout=20)
        return out, int(m.group(1)), asked

    with img:
        with boot(img, root=True, name="t-fsck-ask") as (g, vm):
            # first run: no to cutting the broken chain, yes to saving the lost chains
            out, rc, asked = run_interactive(vm, lambda q: "n" if "runs into a free cluster" in q else "y")
            assert rc == 4, (rc, out)
            assert len(asked) == 2 and "runs into a free cluster" in asked[0] and "save the lost clusters" in asked[1], asked
            assert "left alone (declined)" in out and "repair(s) were not allowed: you said no" in out, out
            assert "FOUND.000/FILE0000.CHK" in out, out
            # second run: the chain is asked about again (a new run), and 'q' stops
            out, rc, asked = run_interactive(vm, lambda q: "q")
            assert rc == 32 and "stopped; nothing more is changed" in out and len(asked) == 1, (rc, asked, out)
            # third run: 'a' says yes to everything
            out, rc, asked = run_interactive(vm, lambda q: "a")
            assert rc == 1 and "the volume is marked clean" in out and len(asked) == 1, (rc, asked, out)
            gx(g, f"fsck -n {dev}", 0, r"the ledger is in order")
            g.poweroff()
        i = CMD_ONE["ask16"]
        files, dirs = volume_files(img.path, i)
        assert "/FOUND.000" in dirs and any(p.startswith("/FOUND.000/FILE") for p in files), sorted(files)[:10]
        assert py_class(img.path, i)[0] in ("ok", "warn") and volume_flagged_clean(img.path, i)


def flow_image(name, damage):
    """A dirty HUMANVILLAGE volume with the named damage of damage_cases() (holes / dots / loop), or with damage_menu() entries ("menu:NAME,NAME")."""
    if damage.startswith("menu:"):
        return dirty_image(name, menu_steps(*damage[5:].split(",")))
    return dirty_image(name, dict((n, m) for n, m, _w in damage_cases())[damage])


@test
def fsck_cmd_kernel_advice():
    """The flow the kernel's message promises: a dirty HUMANVILLAGE volume with damage comes up read-only and says to `umount /HumanVillage` and run
    `fsck -a /dev/vda1`; fsck refuses while it is mounted and mends it once it is not; after a restart the volume is clean, read-write, and nobody
    mentions a ledger."""
    with flow_image("fsck-flow-a", "dots") as img:
        with boot(img, root=True, name="t-fsck-flow-a", bootlog=True) as (g, vm):
            assert "As root, `umount /HumanVillage` and run `fsck -a /dev/vda1`" in klog(vm), klog(vm)[-2000:]
            assert re.search(r"/dev/vda1 /HumanVillage fat32 ro ", g.ok("cat /proc/mounts"))
            gx(g, "fsck -a /dev/vda1", 8, r"is mounted \(/dev/vda1 on /HumanVillage\)", r"Unmount it first")
            g.ok("cd / && umount /HumanVillage")
            gx(g, "fsck -a /dev/vda1", 1, r"fixed: ", r"the ledger is in order and the volume is marked clean")
            gx(g, "fsck -n /dev/vda1", 0, r"the ledger is in order")
            g.poweroff()
        img.check()                                                           # no errors, no warnings: the flags are clear
        with boot(img, root=True, name="t-fsck-flow-a2", bootlog=True) as (g, vm):
            log = klog(vm)
            assert "was not closed properly" not in log and "goes through the whole ledger" not in log and "does not add up" not in log, log[-2000:]
            assert re.search(r"/dev/vda1 /HumanVillage fat32 rw ", g.ok("cat /proc/mounts"))
            g.ok(f"echo written after fsck > {H}/after.txt")
            g.ok("sync")
            g.poweroff()
        img.check()
        v = img.volume(1)
        try:
            assert v.read_file("/reimu/after.txt") == b"written after fsck\n"
        finally:
            v.close()


@test
def fsck_cmd_kernel_advice_remount():
    """Damage that `fsck -a` leaves (it says so, exit 4) and `-y` mends: afterwards the volume is mounted by hand again, without a restart, and is writable."""
    with flow_image("fsck-flow-b", "menu:hole in a chain,illegal character in an 8.3 name") as img:
        with boot(img, root=True, name="t-fsck-flow-b", bootlog=True) as (g, vm):
            assert re.search(r"/dev/vda1 /HumanVillage fat32 ro ", g.ok("cat /proc/mounts"))
            g.ok("cd / && umount /HumanVillage")
            gx(g, "fsck -a /dev/vda1", 4, r"repair\(s\) were not allowed: -y allows them")
            gx(g, "fsck -y /dev/vda1", 1, r"the volume is marked clean")
            g.ok("mount -t fat32 -o userdirs /dev/vda1 /HumanVillage")
            assert re.search(r"/dev/vda1 /HumanVillage fat32 rw ", g.ok("cat /proc/mounts"))
            g.ok(f"echo mended > {H}/after.txt")
            assert g.ok(f"cat {H}/after.txt").strip() == "mended"
            g.ok("sync")
            g.poweroff()
        img.check()


# ---------------------------------------------------------------------------------------------------------------------
# power cuts and fsck together: the images of tests/case_storagecrash.py's kind of run, kept, and gone through by the guest's fsck

@contextlib.contextmanager
def power_cut(seed):
    """One power cut as tests/case_storagecrash.py makes it (fstest -churn, the plug pulled after a random number of operations plus up to a second),
    but the image is kept for the caller: yields (Image, description). The image is removed at the end unless TOUHOU_KEEP_IMAGES is set."""
    rng = random.Random(seed)
    kind, cluster, size = rng.choice([("fat32", None, 96), ("fat32", 512, 96), ("fat32", 4096, 300), ("fat16", None, 96)])
    ops = 20 * rng.randint(0, 60)
    jitter = rng.uniform(0.0, 1.0)
    tag = f"seed {seed} ({kind}, {cluster or 'default'} B clusters, kill after {ops} ops + {jitter:.2f} s)"
    with Image("fsck-cut", size * MIB, [hv_part(cluster=cluster, kind=kind)]) as img:
        with boot(img, name="t-fsck-cut") as (g, vm):
            vm.send(f"fstest -churn {seed} {H}\n")
            vm.expect(r"churn start seed", timeout=60)
            if ops:
                vm.expect(rf"churn {ops}\r?\n", timeout=60 + ops)
            time.sleep(jitter)
            try:
                vm.qmp("quit")                                            # the plug: no sync, no poweroff
            except Exception:                                             # noqa: BLE001 -- QEMU may close the socket before it answers
                pass
            vm.proc.wait(timeout=scaled(30))
        yield img, tag


def fsck_after_cut(seed):
    """Cuts the power, then: the guest's `fsck -n` reports leftovers and nothing worse, `fsck -a` saves the lost clusters and clears the flag, the
    volume is the host's own repair of the same image, the files read as before, and the next mount finds nothing to check. Returns a problem or None."""
    with power_cut(seed) as (img, tag):
        before = volume_files(img.path)
        ledger = fatck_host.scan_volume(img.path, 1)
        if not ledger.ok:
            return f"{tag}: the power cut itself left damage (that is the crash test's business, storage_crash_consistency): {ledger.errors[:3]}"
        twin = scratch("fsck-cut-twin.img")
        shutil.copyfile(img.path, twin)
        try:
            fatck_host.fix_volume(twin, 1, FIX_SAFE)                       # what the repair makes of this image, to compare with the guest's
            return fsck_in_guest_after_cut(img, tag, before, ledger, twin)
        finally:
            twin.unlink(missing_ok=True)


def fsck_in_guest_after_cut(img, tag, before, ledger, twin):
    lost, cb = ledger.result.lost_clusters, ledger.result.cluster_bytes
    with boot(img, root=True, cmdline="nohumanvillage", name="t-fsck-cut-fsck") as (g, vm):          # (not mounted: fsck has the volume to itself)
        out, rc = g.run("fsck -n /dev/vda1", 120)
        if rc not in (0, 4) or re.search(r"^error ", out, re.M):
            return f"{tag}: fsck -n found more than a power cut leaves (exit {rc}):\n{out}"
        if (rc == 4) != bool(ledger.leaks):
            return f"{tag}: fsck -n exit {rc}, the host's scan found {len(ledger.leaks)} leftover(s):\n{out}"
        out, rc = g.run("fsck -a /dev/vda1", 120)
        if ledger.result.dirty or ledger.leaks:                                   # (a cut at a quiet moment, just after the idle flush, leaves a volume that is clean)
            if rc != 1 or "the ledger is in order and the volume is marked clean" not in out:
                return f"{tag}: fsck -a exit {rc}:\n{out}"
        elif rc != 0 or "the ledger is in order" not in out:
            return f"{tag}: the cut left a clean volume, but fsck -a exit {rc}:\n{out}"
        out, rc = g.run("fsck -n /dev/vda1", 120)
        if rc != 0 or "the ledger is in order" not in out:
            return f"{tag}: fsck -n afterwards exit {rc}:\n{out}"
        g.poweroff()
    after = volume_files(img.path)
    if volume_files(twin) != after:
        return f"{tag}: the guest's fsck made another volume than the host's run of the same repairs"
    saved = {p: b for p, b in after[0].items() if p.startswith("/FOUND.000/")}
    if sum(len(b) for b in saved.values()) != lost * cb:
        return f"{tag}: {lost} lost cluster(s) of {cb} bytes before, {sum(len(b) for b in saved.values())} bytes saved in FOUND.000 ({len(saved)} files)"
    if not lost and "/FOUND.000" in after[1]:
        return f"{tag}: nothing was lost, but FOUND.000 was made"
    changed = [p for p, b in before[0].items() if after[0].get(p) != b]
    if changed:
        return f"{tag}: fsck changed files that the power cut had left readable: {changed[:5]}"
    with boot(img, root=True, name="t-fsck-cut-mount") as (g, vm):                      # the next mount: nothing to check, and writable
        log = klog(vm)
        if "was not closed properly" in log or "does not add up" in log:
            return f"{tag}: the kernel still found something to check after fsck:\n{log[-1500:]}"
        if not re.search(r"/dev/vda1 /HumanVillage fat32 rw ", g.ok("cat /proc/mounts")):
            return f"{tag}: the volume is not mounted read-write after fsck"
        g.poweroff()
    rep = mkdisk.check_image(img.path)
    if rep.errors or rep.warnings:
        return f"{tag}: after the next clean shutdown the host's checker says:\n{rep.text(True)}"
    print(f"     power cut {tag}: {lost} lost cluster(s) saved in {len(saved)} file(s), {len(ledger.leaks)} leftover(s) before, ok", flush=True)
    return None


@test
def fsck_cmd_after_power_cuts():
    """Power cuts at random moments of a write-heavy workload (the images of the crash test, kept) and the guest's fsck on them: `fsck -n` reports lost
    clusters and the other harmless leftovers and nothing else, `fsck -a` saves every lost cluster in FOUND.000 and clears the flag, no readable file
    changes, the result is what the host's own run of the same repair makes, and the next mount finds nothing to check. TOUHOU_FSCK_CUTS=N cuts
    (default 2), TOUHOU_CRASH_SEED=S for the seed of the first."""
    runs = int(os.environ.get("TOUHOU_FSCK_CUTS", "2"))
    base = int(os.environ.get("TOUHOU_CRASH_SEED", str(int(time.time()) & 0xFFFFFF)))
    print(f"     power cuts + fsck: {runs} run(s), seeds {base}..{base + runs - 1}", flush=True)
    failed = []
    for i in range(runs):
        why = fsck_after_cut(base + i)
        if why:
            failed.append(why)
            print("     FAILED " + why.splitlines()[0], flush=True)
    assert not failed, f"{len(failed)} of {runs} power cuts did not end well (replay one with TOUHOU_CRASH_SEED=<seed> TOUHOU_FSCK_CUTS=1):\n" + "\n".join(failed)
