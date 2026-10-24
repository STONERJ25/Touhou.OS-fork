"""The consistency check, the repair program and the write order of the FAT driver.

  fsck_c_*      the C checker (shared/fatck.c, built into a host DLL by tools/fatck_host.py) against tools/mkdisk.py's independent
                Python checker, over deliberately damaged images: they must agree on what counts as damage        (no VM)
  fsck_kernel_* the kernel's read-only check of a volume that was not shut down cleanly: read-write when nothing is out of place,
                read-only with the precise reason when something is; hvforce / hvcheck=off
  fsck_cmd_*    /bin/fsck on unmounted volumes (FAT12/16/32): -n, -a, -y, refusals
  fsck_power_*  hard power cuts at random moments of a write-heavy workload leave nothing worse than lost clusters

Run just these with:  python tests/run_all.py fsck
"""
import random
import re
import shutil
import struct
import time

import mkdisk
import fatck_host
from case_storage import (Image, boot, hv_part, klog, MIB, WORK, HV, H, sha)
from harness import test

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


def build_image(path, kind="fat32", files=None, size=None):
    size = size or {"fat32": 64 * MIB, "fat16": 40 * MIB, "fat12": 4 * MIB}[kind]
    mkdisk.build_image(path, size, [dict(name="HUMANVILLAGE", kind=kind, size="rest", label="HUMANVILLAGE", files=files or small_files(kind))])
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


def py_class(path):
    """What tools/mkdisk.py's checker says about partition 1: ok / warn / leak / err."""
    try:
        with open(path, "rb") as f:
            ss, parts = mkdisk.partitions(mkdisk.Window(f))
            w = mkdisk.Window(f, parts[0]["offset"], parts[0]["length"])
            try:
                rep = mkdisk.FatReader(w, name="p1").check()
            except mkdisk.MkdiskError as ex:
                return "err", [str(ex)]
    except mkdisk.MkdiskError as ex:
        return "err", [str(ex)]
    if rep.serious:
        return "err", rep.serious
    if rep.benign:
        return "leak", rep.benign
    return ("warn" if rep.warnings else "ok"), rep.warnings


def c_class(path):
    out = fatck_host.scan_volume(path, 1)
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


def random_damage(src, dst, rng):
    """Copies src to dst and damages it at random; returns a short description."""
    shutil.copyfile(src, dst)
    r = mkdisk.open_volume(dst, 1, writable=True)
    what = []
    try:
        bits, w = r.bits, r.win
        esz = 4 if bits == 32 else 2
        for _ in range(rng.choice([1, 1, 2, 3, 4])):
            kind = rng.choice(["fatbyte", "fatentry", "dirbyte", "dirslot", "dirslot2", "boot", "fsinfo", "dirty", "copy2"])
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
                what.append(kind)
            except mkdisk.MkdiskError:
                pass                                            # an earlier damage made the helper's own lookup fail
        w.f.flush()
    finally:
        r.close()
    return what


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
