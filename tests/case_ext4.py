"""The read-only ext2/ext3/ext4 driver in QEMU (docs/EXT4.md).

Images come from tests/host/ext4img.py: `mke2fs -d` fills them from a generated source tree (in WSL's Ubuntu on Windows; the tests print SKIP and pass where there is no WSL), and the
manifest that decides whether the driver is right comes from that source tree (type, mode, owner, size, md5 of the content, link target, hard links), not from the generator's memory.
They are attached as virtio-blk, SATA (AHCI) and NVMe disks, with and without a partition table, and the guest's own tools do the work: mount -t ext4, find, md5sum, stat, readlink,
df, lsblk, dd. The host compares what the guest saw with the manifest, and checks after the guest is gone that the image is byte for byte what it was (the driver and the mount
policy never write, whatever root types). The parser itself is tested in depth, with hostile images and a fuzz under AddressSanitizer, by tests/host/test_ext4.py; these tests are
about the glue: VFS operations, mount options, the block layer's detection, the three storage drivers, the refusals and, last, the Linux root of docs/COMPAT.md on a disk.

Run just these with:  python tools/qemu-slot.py -- python tests/run_all.py ext4_
"""
import contextlib
import hashlib
import random
import re
import struct
import sys
import time
import zlib
from pathlib import Path

from harness import test, Qemu, BUILD, scaled

sys.path.insert(0, str(Path(__file__).resolve().parent / "host"))
import ext4img  # noqa: E402
from ext4img import MIB  # noqa: E402

WORK = BUILD / "ext4-qemu"
ROOT_PROMPT = r"root@gensokyo \S+ # "
MNT = "/mnt/linux"

_SKIP = None


def skip_reason():
    global _SKIP
    if _SKIP is None:
        _SKIP = "" if ext4img.have_tools() else "no WSL with mke2fs, debugfs and e2fsck (the images are made by e2fsprogs)"
    return _SKIP


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def q(path):
    return str(path).replace("\\", "/")


FOURK = dict(logical_block_size=4096, physical_block_size=4096)           # a disk whose sectors are 4096 bytes ("4Kn")


def virtio(did, img, **props):
    return ["-drive", f"if=none,id={did},format=raw,file={q(img)}", "-device", f"virtio-blk-pci,drive={did}" + "".join(f",{k}={v}" for k, v in props.items())]


def sata(did, img, bus="ide.1"):
    return ["-drive", f"if=none,id={did},format=raw,file={q(img)}", "-device", f"ide-hd,drive={did},bus={bus}"]


def nvme(did, img, serial="EXT4NVME", **props):
    if not props:
        return ["-drive", f"if=none,id={did},format=raw,file={q(img)}", "-device", f"nvme,drive={did},serial={serial}"]
    return ["-drive", f"if=none,id={did},format=raw,file={q(img)}", "-device", f"nvme,id=c-{did},serial={serial}",
            "-device", f"nvme-ns,drive={did},bus=c-{did},nsid=1" + "".join(f",{k}={v}" for k, v in props.items())]


class Guest:
    def __init__(self, vm):
        self.vm = vm

    def run(self, cmd, timeout=60):
        """One shell line; returns (output, exit status)."""
        self.vm.send(f"echo __BE''GIN__; {cmd}; echo __EN''D__$?\n")
        m = self.vm.expect(r"\n__BEGIN__\n(.*?)__END__(\d+)\n", timeout=scaled(timeout))
        self.vm.expect(ROOT_PROMPT, timeout=scaled(20))
        return m.group(1), int(m.group(2))

    def ok(self, cmd, timeout=60):
        out, rc = self.run(cmd, timeout)
        assert rc == 0, f"`{cmd}` exited with {rc}:\n{out}"
        return out

    def fails(self, cmd, expect=None, timeout=60):
        out, rc = self.run(cmd, timeout)
        assert rc != 0, f"`{cmd}` should have failed but printed:\n{out}"
        if expect:
            assert re.search(expect, out), f"`{cmd}`: expected /{expect}/ in:\n{out}"
        return out

    def poweroff(self, timeout=40):
        self.vm.send("poweroff\n")
        rc = self.vm.wait_exit(scaled(timeout))
        assert rc is not None, "guest did not power off:\n" + self.vm.text()[-1500:]


@contextlib.contextmanager
def boot(name, disk=None, extra=(), cmdline="text bootlog=off user=root", mem="512M"):
    with Qemu(cmdline=cmdline, disk=disk, extra=list(extra), name=name, mem=mem) as vm:
        vm.expect(ROOT_PROMPT, timeout=scaled(90))
        yield Guest(vm), vm


def id_maps(g):
    """/etc/passwd and /etc/group of the guest as {name: id}: find prints names for the ids it knows and numbers for the others."""
    maps = []
    for path in ("/etc/passwd", "/etc/group"):
        m = {}
        for line in g.ok(f"cat {path}").splitlines():
            parts = line.split(":")
            if len(parts) >= 3 and parts[2].isdigit():
                m[parts[0]] = int(parts[2])
        maps.append(m)
    return maps


def guest_view(g, mnt, timeout=180):
    """The mounted volume as the guest's tools see it: {path: {...}} from one find, the md5 of every regular file, and the target of every symbolic link."""
    users, groups = id_maps(g)

    def num(m, s):
        return int(s) if s.isdigit() else m[s]

    out = g.ok(f"find {mnt} -printf '%y\\t%p\\t%s\\t%m\\t%i\\t%T\\t%u\\t%g\\n'", timeout=timeout)
    rows = {}
    for line in out.splitlines():
        f = line.split("\t")
        if len(f) == 8 and f[1].startswith(mnt):
            rows[f[1][len(mnt):] or "/"] = dict(kind=f[0], size=int(f[2]), mode=int(f[3], 8), ino=int(f[4]), mtime=int(f[5]), uid=num(users, f[6]), gid=num(groups, f[7]))
    md5 = {}
    out = g.ok(f"find {mnt} -type f -exec md5sum {{}} +", timeout=timeout)
    for line in out.splitlines():
        m = re.match(r"([0-9a-f]{32})\s+(.*)$", line)
        if m and m.group(2).startswith(mnt):
            md5[m.group(2)[len(mnt):]] = m.group(1)
    return rows, md5


def kstats(g):
    """/proc/kstats of the guest as {name: number}: vnodes, files, heap_bytes, warnings ..."""
    return {k: int(v) for k, v in (line.split() for line in g.ok("cat /proc/kstats").splitlines() if len(line.split()) == 2 and line.split()[1].isdigit())}


def guest_links(g, mnt, paths):
    """The target of each link, one readlink for all (a link that cannot be read prints nothing, so a missing line is a failure the caller sees)."""
    out = g.ok("readlink " + " ".join(f"'{mnt}{p}'" for p in paths), timeout=60)
    return out.splitlines()


def compare_view(manifest, rows, md5, mnt_links=None, ignore=("/lost+found", "/")):
    """What the guest saw against the manifest of the source tree; returns a list of problems (empty when they agree)."""
    bad = []
    seen_group = {}
    for path, exp in manifest["files"].items():
        row = rows.get(path)
        if row is None:
            bad.append(f"{path}: missing from the guest's find")
            continue
        kind = "f" if exp["kind"] == "s" else exp["kind"]                     # find has no word for a socket
        if row["kind"] != kind:
            bad.append(f"{path}: kind {row['kind']} against {kind}")
        if row["mode"] != exp["mode"]:
            bad.append(f"{path}: mode {row['mode']:o} against {exp['mode']:o}")
        if row["uid"] != exp["uid"] or row["gid"] != exp["gid"]:
            bad.append(f"{path}: owner {row['uid']}:{row['gid']} against {exp['uid']}:{exp['gid']}")
        if exp.get("mtime") is not None and exp["mtime"] >= 0 and row["mtime"] != exp["mtime"]:
            bad.append(f"{path}: mtime {row['mtime']} against {exp['mtime']}")
        if exp["kind"] in "fl" and row["size"] != exp["size"]:
            bad.append(f"{path}: size {row['size']} against {exp['size']}")
        if exp["kind"] == "f" and "md5" in exp and md5.get(path) != exp["md5"]:
            bad.append(f"{path}: md5 {md5.get(path)} against {exp['md5']}")
        if exp["kind"] != "d" and exp.get("group") is not None:
            if exp["group"] in seen_group:
                if row["ino"] != seen_group[exp["group"]]:
                    bad.append(f"{path}: not the same inode as its hard link")
            else:
                seen_group[exp["group"]] = row["ino"]
    extra = [p for p in rows if p not in manifest["files"] and p not in ignore and not p.startswith("/lost+found")]
    if extra:
        bad.append(f"entries the source tree did not have: {extra[:5]}")
    return bad


def walk_and_compare(g, mnt, manifest, name):
    rows, md5 = guest_view(g, mnt)
    bad = compare_view(manifest, rows, md5)
    links = [(p, e["target"]) for p, e in manifest["files"].items() if e["kind"] == "l" and len(e["target"]) < 1000]
    got = guest_links(g, mnt, [p for p, _ in links])
    if got != [t for _, t in links]:
        bad.append(f"link targets differ: {[(p, t, x) for (p, t), x in zip(links, got + [None] * len(links)) if t != x][:4]}")
    assert not bad, f"{name}: the guest's view differs from the source tree:\n" + "\n".join(bad[:25])


# ---------------------------------------------------------------------------------------------- the tests

@test
def ext4_mount_read_walk_and_refuse_every_write():
    """Mount a generated ext4 volume: lsblk names it, mount -o rw is refused, a walk with find, md5sum and readlink equals the source tree (type, mode, owner, size, times, md5, hard links,
    links), real permissions are enforced, df, twelve kinds of write all fail with 'Read-only file system', busy while inside, umount; four more volumes (ext2 with 1 KiB blocks and
    indirect maps, ext3, inline data, meta_bg) are walked the same way; every image is unchanged afterwards."""
    if skip_reason():
        print("SKIP:", skip_reason())
        return
    ms = ext4img.ensure(WORK, ["ext4", "ext2_1k", "ext3", "ext4_inline", "fz_metabg"])
    img = {p: WORK / f"{p}.img" for p in ms}
    before = {p: sha(img[p]) for p in img}
    extra = virtio("d1", img["ext2_1k"]) + virtio("d2", img["ext3"]) + virtio("d3", img["ext4_inline"]) + virtio("d4", img["fz_metabg"])
    with boot("t-ext4-walk", disk=img["ext4"], extra=extra) as (g, vm):
        g.ok(f"mkdir -p {MNT}")
        k0 = kstats(g)
        mounts = g.ok("cat /proc/mounts")
        assert "ext" not in mounts.replace("/dev", ""), "the mount policy must not touch an ext volume by itself:\n" + mounts
        lsblk = g.ok("lsblk")
        assert re.search(r"(?m)^vda\s+64M\s+0\s+disk\s+ext4\s+ext4-default\s", lsblk), lsblk
        assert re.search(r"(?m)^vdb\s+64M\s+0\s+disk\s+ext2\s+ext2-1k\s", lsblk), lsblk
        assert re.search(r"(?m)^vdc\s+\S+\s+0\s+disk\s+ext3\s+ext3\s", lsblk), lsblk
        assert re.search(r"(?m)^vde\s+\S+\s+0\s+disk\s+ext4\s+fz-metabg\s", lsblk), lsblk
        assert "vda ext4 ext4-default" in g.ok("cat /proc/blkfs")
        g.fails(f"mount -t ext4 -o rw /dev/vda {MNT} 2>&1", r"Read-only file system")
        assert "refusing a read-write mount" in g.ok("dmesg")
        g.fails(f"mount -t ext4 -o ro,bogus /dev/vda {MNT} 2>&1")
        assert "unknown mount option 'bogus'" in g.ok("dmesg")
        g.fails(f"mount -t ext4 -o ro /dev/vdz {MNT} 2>&1")
        out = g.ok(f"mount -t ext4 -o ro /dev/vda {MNT} && dmesg | grep -i 'ext4:'")
        assert re.search(r"ext4: vda mounted on /mnt/linux, 64 MiB, 4096-byte blocks, label 'ext4-default', ext4 \(.*extent.*\), read-only", out), out
        assert re.search(rf"/dev/vda {MNT} ext4 ro", g.ok("cat /proc/mounts")), "listed read-only in /proc/mounts"
        walk_and_compare(g, MNT, ms["ext4"], "ext4")
        ls = g.ok(f"ls -l {MNT}")
        assert re.search(r"hello\.txt", ls) and "rel-link -> hello.txt" in ls and "abs-link -> /dir/sub/deep/file.txt" in ls, ls
        assert g.ok(f"cat {MNT}/dir/ファイル.txt") == "unicode name\n"
        assert g.ok(f"cat {MNT}/rel-link") == "hello from a generated ext volume\n"
        assert g.ok(f"cat {MNT}/dir-link/f03.txt | wc -c").strip() == str(ms["ext4"]["files"]["/dir/f03.txt"]["size"])
        assert re.search(r"Links: 3\b", g.ok(f"stat {MNT}/hello.txt")), "three names for one inode"
        assert re.search(r"Uid: \(70000/", g.ok(f"stat {MNT}/modes/owned")), "a user id above 16 bits"
        out = g.fails(f"cat {MNT}/chardev 2>&1", r"chardev")
        assert re.search(r"No such device|Unknown error", out), f"a device node on a disk is a name only (ENXIO), but cat said: {out!r}"   # the C library has no text for ENXIO
        out = g.ok(f"ls -l {MNT}/chardev {MNT}/blkdev {MNT}/fifo")
        assert re.search(r"^c\S+ .*/chardev$", out, re.M) and re.search(r"^b\S+ .*/blkdev$", out, re.M) and re.search(r"^p\S+ .*/fifo$", out, re.M), out
        # permissions are the real ones of the inode: /modes/private is 0600 and belongs to uid 1000
        users, groups = id_maps(g)
        owner = next(n for n, u in users.items() if u == 1000)
        other = next(n for n, u in users.items() if u > 1000)
        assert g.ok(f"su {owner} -c 'cat {MNT}/modes/private'") == "secret\n"
        g.fails(f"su {other} -c 'cat {MNT}/modes/private' 2>&1", r"Permission denied")
        assert g.ok(f"su {other} -c 'cat {MNT}/hello.txt'").startswith("hello")
        g.fails(f"su {other} -c 'ls {MNT}/modes/closed' 2>&1", r"Permission denied")
        out = g.ok(f"df {MNT}")
        assert "/dev/vda" in out and "(ro)" in out, out
        # every kind of write is refused, with the right words (root may open a file for writing, as the VFS does not look at read-only mounts at open; the write itself fails)
        for cmd in (f"echo x > {MNT}/new.txt", f"dd if=/dev/zero of={MNT}/text.txt bs=1 count=1 conv=notrunc", f"rm {MNT}/text.txt", f"mkdir {MNT}/newdir", f"rmdir {MNT}/emptydir",
                    f"touch {MNT}/touched", f"mv {MNT}/text.txt {MNT}/moved.txt", f"truncate -s 0 {MNT}/text.txt", f"ln {MNT}/text.txt {MNT}/hardlink",
                    f"ln -s x {MNT}/newlink", f"chmod 777 {MNT}/text.txt", f"chown 5 {MNT}/text.txt", f"cp /etc/passwd {MNT}/passwd"):
            out, rc = g.run(f"{cmd} 2>&1")
            assert "Read-only file system" in out, f"`{cmd}` was not refused with EROFS (exit {rc}): {out!r}"
        walk_and_compare(g, MNT, ms["ext4"], "ext4 after the attempts to write")
        g.fails(f"umount {MNT}/dir 2>&1")
        assert g.ok(f"cd {MNT}/dir/.. && pwd && cd .. && pwd && cd /").split() == [MNT, "/mnt"], '".." leaves the volume through its mount point'
        g.fails(f"sh -c 'cd {MNT}/dir && umount {MNT}' 2>&1", r"busy|Busy|BUSY")
        g.ok(f"umount {MNT}")
        assert "ext4" not in g.ok("cat /proc/mounts")
        assert re.search(r"ext4: vda unmounted", g.ok("dmesg"))
        # four more kinds of volume, mounted with the other two names of the driver
        for dev, prof, fstype in (("vdb", "ext2_1k", "ext2"), ("vdc", "ext3", "ext3"), ("vdd", "ext4_inline", "ext4"), ("vde", "fz_metabg", "ext4")):
            g.ok(f"mount -t {fstype} -o ro /dev/{dev} {MNT}")
            assert re.search(rf"/dev/{dev} {MNT} {fstype} ro", g.ok("cat /proc/mounts"))
            walk_and_compare(g, MNT, ms[prof], prof)
            g.ok(f"umount {MNT}")
        # nothing is left behind by five mounts, five walks (thousands of vnodes and open files) and five unmounts: every vnode and open file given back, the memory of the caches too
        k1 = kstats(g)
        assert k1["vnodes"] <= k0["vnodes"] + 3 and k1["files"] <= k0["files"] + 3, f"vnodes or open files were left behind: {k0} -> {k1}"
        assert k1["heap_bytes"] < k0["heap_bytes"] + 512 * 1024, f"the heap grew by {k1['heap_bytes'] - k0['heap_bytes']} bytes: the caches of an unmounted volume must be freed: {k0} -> {k1}"
        assert k1["warnings"] == k0["warnings"], f"kernel warnings during the mounts: {k0} -> {k1}"
        g.poweroff()
    for p in img:
        assert sha(img[p]) == before[p], f"the guest wrote to the {p} image"


# ---------------------------------------------------------------------------------------------- disks with partition tables

GUID_LINUX = bytes.fromhex("af3dc60f838472478e793d69d8477de4")          # 0FC63DAF-8483-4772-8E79-3D69D8477DE4, as GPT stores it (mixed endian)
GUID_ESP = bytes.fromhex("28732ac11ff8d211ba4b00a0c93ec93b")            # C12A7328-F81F-11D2-BA4B-00A0C93EC93B


def crc(b):
    return zlib.crc32(b) & 0xFFFFFFFF


def gpt_disk(path, parts, ss=512):
    """A disk image with a GPT: parts is [(type guid, name, bytes)], each started on a 1 MiB boundary; ss is the sector size (512 or 4096). Returns {name: (first lba, last lba)}."""
    mib, layout = 2048 * 512 // ss, []                                    # sectors in 1 MiB
    esec = 128 * 128 // ss                                                # sectors of the table of 128 entries
    first = mib
    for i, (t, name, data) in enumerate(parts):
        n = (len(data) + ss - 1) // ss
        layout.append((t, name, first, first + n - 1, data))
        first = (first + n + mib - 1) // mib * mib
    total = first + mib
    disk = bytearray(total * ss)
    mbr = bytearray(512)
    mbr[446 + 4] = 0xEE
    mbr[446 + 8:446 + 16] = struct.pack("<II", 1, min(total - 1, 0xFFFFFFFF))
    mbr[510:512] = b"\x55\xaa"
    disk[0:512] = mbr
    entries = bytearray(128 * 128)
    for i, (t, name, a, b, data) in enumerate(layout):
        entries[i * 128:(i + 1) * 128] = t + bytes([i + 1]) * 16 + struct.pack("<QQQ", a, b, 0) + name.encode("utf-16-le").ljust(72, b"\0")
        disk[a * ss:a * ss + len(data)] = data
    for lba, backup, elba in ((1, total - 1, 2), (total - 1, 1, total - 1 - esec)):
        h = bytearray(512)
        h[0:8] = b"EFI PART"
        struct.pack_into("<IIII", h, 8, 0x00010000, 92, 0, 0)
        struct.pack_into("<QQQQ", h, 24, lba, backup, 2 + esec, total - 2 - esec)
        h[56:72] = bytes(range(16))
        struct.pack_into("<QIII", h, 72, elba, 128, 128, crc(bytes(entries)))
        struct.pack_into("<I", h, 16, crc(bytes(h[:92])))
        disk[lba * ss:lba * ss + 512] = h
    disk[2 * ss:2 * ss + len(entries)] = entries
    disk[(total - 1 - esec) * ss:(total - 1 - esec) * ss + len(entries)] = entries
    Path(path).write_bytes(bytes(disk))
    return {name: (a, b) for (t, name, a, b, d) in layout}


def mbr_disk(path, ptype, data):
    """A disk image with an MBR that has one partition of the given type, at LBA 2048."""
    ss = 512
    n = (len(data) + ss - 1) // ss
    disk = bytearray((2048 + n + 2048) * ss)
    disk[446 + 4] = ptype
    disk[446 + 8:446 + 16] = struct.pack("<II", 2048, n)
    disk[510:512] = b"\x55\xaa"
    disk[2048 * ss:2048 * ss + len(data)] = data
    Path(path).write_bytes(bytes(disk))


@test
def ext4_on_ahci_nvme_and_virtio_with_partition_tables():
    """The same driver on a SATA disk, an NVMe namespace and virtio disks, whole or inside a partition: lsblk and /proc/blkfs name the filesystem of a Linux-typed partition (GPT or MBR)
    and of a disk without a partition table, and leave an EFI system partition alone (even when an ext superblock sits inside it); a partition cannot be mounted together with its disk;
    each volume is walked and compared with its source tree; the images are unchanged afterwards."""
    if skip_reason():
        print("SKIP:", skip_reason())
        return
    ms = ext4img.ensure(WORK, ["ext4_1k", "fz_ext4", "fz_ext3"])
    data = {p: (WORK / f"{p}.img").read_bytes() for p in ms}
    mbr, gpt, nv = WORK / "disk-mbr.img", WORK / "disk-gpt.img", WORK / "disk-nvme.img"
    mbr_disk(mbr, 0x83, data["fz_ext4"])
    gpt_disk(gpt, [(GUID_ESP, "esp-decoy", data["fz_ext4"][:4 * MIB]), (GUID_LINUX, "garbage", bytes(range(256)) * 4096)])
    gpt_disk(nv, [(GUID_LINUX, "", data["fz_ext3"])])
    sat = WORK / "ext4_1k.img"
    imgs = [mbr, gpt, nv, sat]
    before = {p: sha(p) for p in imgs}
    extra = virtio("d1", gpt) + sata("d2", sat) + nvme("d3", nv)
    with boot("t-ext4-controllers", disk=mbr, extra=extra) as (g, vm):
        g.ok(f"mkdir -p {MNT}")
        lsblk = g.ok("lsblk")
        assert re.search(r"(?m)^vda\s+\S+\s+0\s+disk\s+virtio-blk\b", lsblk), "a disk with a partition table holds no filesystem of its own:\n" + lsblk
        assert re.search(r"(?m)^vda1\s+\S+\s+0\s+part\s+ext4\s+fz-ext4\s", lsblk), lsblk
        assert re.search(r"(?m)^vdb1\s+\S+\s+0\s+part\s+esp-decoy\s", lsblk), "an EFI system partition is not looked into:\n" + lsblk
        assert re.search(r"(?m)^vdb2\s+\S+\s+0\s+part\s+garbage\s", lsblk), lsblk
        assert re.search(r"(?m)^sdb\s+\S+\s+0\s+disk\s+ext4\s+ext4-1k\s", lsblk), lsblk
        assert re.search(r"(?m)^nvme0n1p1\s+\S+\s+0\s+part\s+ext3\s+fz-ext3\s", lsblk), lsblk
        blkfs = g.ok("cat /proc/blkfs")
        assert sorted(l.split()[0] for l in blkfs.splitlines()) == ["nvme0n1p1", "sdb", "vda1"], blkfs
        log = g.ok("dmesg | grep 'holds ext'")
        assert "blk: vda1 holds ext4 (label 'fz-ext4')" in log and "blk: nvme0n1p1 holds ext3" in log and "blk: sdb holds ext4 (label 'ext4-1k')" in log, log
        for dev, prof, fstype in (("vda1", "fz_ext4", "ext4"), ("sdb", "ext4_1k", "ext4"), ("nvme0n1p1", "fz_ext3", "ext3")):
            g.ok(f"mount -t {fstype} -o ro /dev/{dev} {MNT}")
            assert re.search(rf"/dev/{dev} {MNT} {fstype} ro", g.ok("cat /proc/mounts"))
            walk_and_compare(g, MNT, ms[prof], f"{prof} on {dev}")
            if dev == "vda1":
                g.fails("mount -t ext4 -o ro /dev/vda /mnt 2>&1", r"busy|Busy|BUSY")       # a disk and one of its partitions
                g.fails("mount -t ext4 -o ro /dev/vda1 /mnt 2>&1", r"busy|Busy|BUSY")      # the partition twice
            g.ok(f"umount {MNT}")
        g.fails(f"mount -t ext4 -o ro /dev/vdb2 {MNT} 2>&1")
        assert re.search(r"ext4: vdb2 is not a mountable ext2/3/4 volume: not an ext2/3/4 volume", g.ok("dmesg")), "garbage is refused with a reason"
        g.poweroff()
    for p in imgs:
        assert sha(p) == before[p], f"the guest wrote to {p.name}"


@test
def ext4_on_disks_with_4096_byte_sectors():
    """Disks with 4096-byte logical sectors ("4Kn": a virtio-blk disk and an NVMe namespace with logical_block_size=4096): the superblock at byte 1024 is inside the first sector and every
    read of the parser that does not start and end on a sector goes through the bounce buffer of the driver. A whole disk with a volume of 4 KiB blocks, a GPT with a Linux partition that
    holds a volume of 1 KiB blocks (a Linux kernel would not mount that, since a block may not be smaller than a sector, but this reader is byte granular, and every metadata read of it is
    ragged), and an NVMe namespace the same: each is named by lsblk, walked and compared with its source tree; the images are unchanged afterwards."""
    if skip_reason():
        print("SKIP:", skip_reason())
        return
    ms = ext4img.ensure(WORK, ["ext4", "ext4_1k", "fz_ext3"])
    whole = WORK / "ext4.img"
    gpt, nv = WORK / "disk-gpt4k.img", WORK / "disk-nvme4k.img"
    gpt_disk(gpt, [(GUID_LINUX, "", (WORK / "ext4_1k.img").read_bytes())], ss=4096)         # no partition name: lsblk shows the label of the file system
    gpt_disk(nv, [(GUID_LINUX, "", (WORK / "fz_ext3.img").read_bytes())], ss=4096)
    imgs = [whole, gpt, nv]
    before = {p: sha(p) for p in imgs}
    extra = virtio("d1", gpt, **FOURK) + nvme("d2", nv, **FOURK)
    with boot("t-ext4-4kn", disk=None, extra=virtio("d0", whole, **FOURK) + extra) as (g, vm):
        g.ok(f"mkdir -p {MNT}")
        k0 = kstats(g)
        parts = g.ok("cat /proc/partitions")
        for dev in ("vda", "vdb", "vdb1", "nvme0n1", "nvme0n1p1"):
            assert re.search(rf"(?m)^{dev} +\d+ +4096 ", parts), f"{dev} should have 4096-byte sectors:\n{parts}"
        lsblk = g.ok("lsblk")
        assert re.search(r"(?m)^vda\s+\S+\s+0\s+disk\s+ext4\s+ext4-default\s", lsblk), lsblk
        assert re.search(r"(?m)^vdb1\s+\S+\s+0\s+part\s+ext4\s+ext4-1k\s", lsblk), lsblk
        assert re.search(r"(?m)^nvme0n1p1\s+\S+\s+0\s+part\s+ext3\s+fz-ext3\s", lsblk), lsblk
        for dev, prof, fstype in (("vda", "ext4", "ext4"), ("vdb1", "ext4_1k", "ext4"), ("nvme0n1p1", "fz_ext3", "ext3")):
            g.ok(f"mount -t {fstype} -o ro /dev/{dev} {MNT}")
            assert re.search(rf"/dev/{dev} {MNT} {fstype} ro", g.ok("cat /proc/mounts"))
            walk_and_compare(g, MNT, ms[prof], f"{prof} on {dev} (4096-byte sectors)")
            g.ok(f"umount {MNT}")
        k1 = kstats(g)
        assert k1["warnings"] == k0["warnings"] and k1["vnodes"] <= k0["vnodes"] + 3, f"warnings or vnodes left behind: {k0} -> {k1}"
        g.poweroff()
    for p in imgs:
        assert sha(p) == before[p], f"the guest wrote to {p.name}"


@test
def ext4_a_directory_of_twenty_thousand_files_and_a_big_file():
    """One hashed directory with 20000 entries is listed (ls, and ls -l, which looks up and stats every name), every name is compared with the source tree, the files that have content
    are read back, a name that is not there is not found; a file of 100 MB is read with dd, cat and md5sum (the numbers are printed: this is QEMU's emulated CPU and virtio disk,
    not hardware)."""
    if skip_reason():
        print("SKIP:", skip_reason())
        return
    ms = ext4img.ensure(WORK, ["ext4_bigdir", "ext4_speed"])
    bigdir, speed = WORK / "ext4_bigdir.img", WORK / "ext4_speed.img"
    before = {p: sha(p) for p in (bigdir, speed)}
    with boot("t-ext4-speed", disk=bigdir, extra=virtio("d1", speed)) as (g, vm):
        g.ok(f"mkdir -p {MNT} /mnt/speed")
        g.ok(f"mount -t ext4 -o ro /dev/vda {MNT}")

        def timed(cmd, timeout=600):
            t0 = time.time()
            out = g.ok(cmd, timeout=timeout)
            return out, time.time() - t0

        files = ms["ext4_bigdir"]["files"]
        out, t_ls = timed(f"ls {MNT}/many | wc -l")
        assert out.strip() == "20000", out
        out, t_lsl = timed(f"ls -l {MNT}/many | wc -l")
        assert out.strip() in ("20001", "20000"), out
        # every name, once and in order (the manifest has them from the source tree), and the content of the files that have any
        names = sorted(p[len("/many/"):] for p in files if p.startswith("/many/"))
        assert len(names) == 20000
        out, t_names = timed(f"ls {MNT}/many | sort | md5sum")
        assert out.split()[0] == hashlib.md5(("\n".join(names) + "\n").encode()).hexdigest(), "the names in the directory are the names of the source tree"
        withdata = [p for p, e in files.items() if p.startswith("/many/") and e["kind"] == "f" and e["size"] > 0]
        assert len(withdata) >= 40
        out, t_md5s = timed("md5sum " + " ".join(f"{MNT}{p}" for p in withdata), timeout=300)
        got = {line.split()[1][len(MNT):]: line.split()[0] for line in out.splitlines() if line.strip()}
        assert got == {p: files[p]["md5"] for p in withdata}, "the files with content read back as they were made"
        out = g.ok(f"stat {MNT}/many/{names[0]} {MNT}/many/{names[12345]} {MNT}/many/{names[-1]}")
        assert out.count("Size:") == 3, out
        gone = g.fails(f"cat {MNT}/many/file-00000-000001.dat 2>&1", r"No such file")
        out, t_cnt = timed(f"ls {MNT}/many | grep -c '^file-0199'")
        assert out.strip() == str(sum(1 for n in names if n.startswith("file-0199"))), out
        print(f"     (20000 entries in one directory: ls {t_ls:.1f} s, ls -l {t_lsl:.1f} s, names checked {t_names:.1f} s, {len(withdata)} files read {t_md5s:.1f} s, wall clock, QEMU TCG)")
        g.ok(f"umount {MNT}")
        g.ok(f"mount -t ext4 -o ro /dev/vdb /mnt/speed")
        size = ms["ext4_speed"]["files"]["/big100m.bin"]["size"]
        out, t_dd = timed("dd if=/mnt/speed/big100m.bin of=/dev/null bs=1M 2>&1", timeout=900)
        m = re.search(r"([\d.]+) MB/s", out)
        out, t_md5 = timed("md5sum /mnt/speed/big100m.bin", timeout=900)
        assert out.split()[0] == ms["ext4_speed"]["files"]["/big100m.bin"]["md5"], out
        out, t_cat = timed("cat /mnt/speed/big100m.bin | wc -c", timeout=900)
        assert out.strip() == str(size), out
        print(f"     (reading {size / 1e6:.0f} MB: dd bs=1M {t_dd:.1f} s ({m.group(1) if m else '?'} MB/s by dd's own clock), cat | wc {t_cat:.1f} s, md5sum {t_md5:.1f} s)")
        g.ok("umount /mnt/speed")
        g.poweroff()
    for p in (bigdir, speed):
        assert sha(p) == before[p], f"the guest wrote to {p.name}"


# ---------------------------------------------------------------------------------------------- damaged volumes and odd ones


def wsl_path(p):
    return ext4img.wsl_path(p)


def derive(profile, name, debugfs=(), mutate=None):
    """WORK/NAME.img: a copy of a profile's image, changed by debugfs commands (in WSL) and/or a function that edits its bytes. Returns the path."""
    src, dst = WORK / f"{profile}.img", WORK / f"{name}.img"
    dst.write_bytes(src.read_bytes())
    if debugfs:
        cmds = WORK / f"{name}.cmd"
        cmds.write_bytes(("\n".join(debugfs) + "\n").encode())
        r = ext4img.linux(f"debugfs -w -f '{wsl_path(cmds)}' '{wsl_path(dst)}'", check=False, capture_output=True, text=True)
        assert r.returncode == 0, f"debugfs for {name}: {r.stdout}{r.stderr}"
    if mutate:
        data = bytearray(dst.read_bytes())
        mutate(data)
        dst.write_bytes(bytes(data))
    return dst


def bmap(img, path, lblk=0):
    """The volume block that holds a file's logical block (debugfs)."""
    r = ext4img.linux(f"debugfs -R 'bmap {path} {lblk}' '{wsl_path(img)}'", check=False, capture_output=True, text=True)
    return int(r.stdout.split()[-1])


def poke(data, off, value):
    data[off:off + len(value)] = value


@test
def ext4_damaged_volumes_do_not_hurt_the_kernel():
    """Volumes with a damaged superblock, root inode, inode table pointer, directory block and extent tree, one cut in half, and six made by random byte changes in the metadata:
    the mount fails with a reason in the log or the volume lists with errors (EIO for the damaged file or directory, the rest readable), and the kernel keeps running: no panic, no
    warning, the shell answers, and a good volume mounts afterwards. The parser's own fuzz (thousands of cases, ASan and UBSan) is tests/host/test_ext4.py."""
    if skip_reason():
        print("SKIP:", skip_reason())
        return
    ms = ext4img.ensure(WORK, ["fz_ext4", "fz_ext2", "fz_inline"])
    cases = {}                                                  # name -> (image, does the mount succeed, what a refusal says)
    cases["magic"] = (derive("fz_ext4", "bad-magic", mutate=lambda d: poke(d, 1024 + 0x38, b"\0\0")), False, r"not an ext2/3/4 volume")
    cases["root"] = (derive("fz_ext4", "bad-root", debugfs=["set_inode_field <2> mode 0"]), False, r"root directory")
    cases["itable"] = (derive("fz_ext4", "bad-itable", debugfs=["set_bg 0 inode_table 4294967290"]), False, r"root directory")
    cases["short"] = (derive("fz_ext4", "bad-short", mutate=lambda d: d.__delitem__(slice(len(d) // 2, None))), False, r"holds only")
    tmp = derive("fz_ext4", "bad-dir")
    blk = bmap(tmp, "/dir")
    cases["dir"] = (derive("fz_ext4", "bad-dir", mutate=lambda d: poke(d, blk * 4096, bytes(4096))), True, None)
    cases["extent"] = (derive("fz_ext4", "bad-extent", debugfs=["set_inode_field /data.bin block[0] 0"]), True, None)
    rnd = random.Random(2026)
    for k, prof in enumerate(["fz_ext4", "fz_ext4", "fz_ext2", "fz_ext2", "fz_inline", "fz_inline"]):
        def damage(d, rnd=rnd):
            for _ in range(24):
                d[rnd.randrange(1024, 1 * MIB)] = rnd.randrange(256)
        cases[f"random{k}"] = (derive(prof, f"bad-random{k}", mutate=damage), None, None)
    names = list(cases)
    imgs = [cases[n][0] for n in names]
    before = {p: sha(p) for p in imgs}
    good = WORK / "fz_ext4.img"
    with boot("t-ext4-damaged", disk=imgs[0], extra=sum((virtio(f"d{i}", img) for i, img in enumerate(imgs[1:])), []) + virtio("dgood", good), mem="768M") as (g, vm):
        g.ok(f"mkdir -p {MNT}")
        for i, name in enumerate(names):
            dev = "vd" + chr(ord("a") + i)
            img, mounts, refusal = cases[name]
            out, rc = g.run(f"mount -t ext4 -o ro /dev/{dev} {MNT} 2>&1", timeout=60)
            if mounts is False:
                assert rc != 0, f"{name}: a damaged volume should not mount, but it did"
                assert re.search(rf"ext4: {dev} is not a mountable ext2/3/4 volume: .*{refusal}", g.ok("dmesg")), f"{name}: the log must say why"
                continue
            if rc != 0:                                         # a random change may have made the volume unusable: it must have said why
                assert re.search(rf"ext4: {dev} is not a mountable", g.ok("dmesg")), f"{name}: mount failed without a reason: {out}"
                continue
            # (a damaged size field can make a file of 400 GB of zeros: only the files under 8 MiB are summed)
            g.run(f"ls -lR {MNT} > /dev/null 2>&1; find {MNT} -type f -size -8192k -exec md5sum {{}} + > /dev/null 2>&1; find {MNT} -type l -exec readlink {{}} + > /dev/null 2>&1", timeout=240)
            if name == "dir":
                assert g.ok(f"cat {MNT}/hello.txt").startswith("hello"), "the rest of the volume is readable"
                out, rc = g.run(f"cat {MNT}/dir/f03.txt 2>&1")          # (ls cannot tell an error from the end of a listing: the C library's readdir returns NULL for both)
                assert rc != 0 and "Input/output error" in out, f"a name in the damaged directory answers EIO: {out!r}"
            if name == "extent":
                assert g.ok(f"cat {MNT}/hello.txt").startswith("hello")
                out, rc = g.run(f"cat {MNT}/data.bin > /dev/null 2>&1")
                assert rc != 0, "the file with a damaged extent tree cannot be read"
                assert g.ok(f"cat {MNT}/text.txt | wc -c").strip() == str(ms["fz_ext4"]["files"]["/text.txt"]["size"])
            g.ok(f"umount {MNT}", timeout=60)
        g.ok("echo the kernel is fine")
        out, rc = g.run("dmesg | grep -E 'WARNING|panic|PANIC|BUG:|Oops|assertion failed'")
        assert not out.strip(), f"the log has warnings after the damaged volumes:\n{out[:1500]}"
        g.ok(f"mount -t ext4 -o ro /dev/vd{chr(ord('a') + len(names))} {MNT}")
        walk_and_compare(g, MNT, ms["fz_ext4"], "the good volume after the damaged ones")
        g.ok(f"umount {MNT}")
        g.poweroff()
    for p in imgs:
        assert sha(p) == before[p], f"the guest wrote to {p.name}"


@test
def ext4_mount_options_encrypted_files_and_a_volume_that_needs_recovery():
    """The words of a Linux fstab are accepted and `nosuid` really clears the set-id bits; a volume that needs journal recovery or says it has errors mounts as it is with a log line
    that says what that can mean; an encrypted file answers ENOKEY and the rest of the volume reads; ext2, ext3 and ext4 are one driver under three names."""
    if skip_reason():
        print("SKIP:", skip_reason())
        return
    ms = ext4img.ensure(WORK, ["fz_ext4"])
    odd = derive("fz_ext4", "odd", debugfs=["feature needs_recovery", "set_super_value state 2", "feature encrypt", "set_inode_field /hello.txt flags 0x80800"])
    before = sha(odd)
    with boot("t-ext4-options", disk=odd) as (g, vm):
        g.ok(f"mkdir -p {MNT}")
        g.ok(f"mount -t ext4 -o ro,noatime,nodev,noexec,errors=remount-ro,data=ordered,barrier=1,commit=5,user_xattr,acl,noload,i_version /dev/vda {MNT}")
        log = g.ok("dmesg | grep 'ext4'")
        assert "needs journal recovery" in log and "the journal is not replayed" in log, log
        assert "marked as having errors" in log, log
        assert re.search(r"-rwsr-xr-x .* root|-rwsr-xr-x .* reimu", g.ok(f"ls -l {MNT}/modes/setuid")), "without nosuid the set-user-id bit is the file's own"
        out, rc = g.run(f"cat {MNT}/hello.txt 2>&1")
        assert rc != 0 and re.search(r"Unknown error|required key", out), f"an encrypted file has no key here: {out!r}"       # the C library has no text for ENOKEY
        out, rc = g.run(f"cat {MNT}/hard1 2>&1")
        assert rc != 0, "a hard link to an encrypted file is the same encrypted file"
        assert re.search(r"Size: 34\b", g.ok(f"stat {MNT}/hello.txt")), "an encrypted file can be stat'ed"
        assert g.ok(f"cat {MNT}/dir/f03.txt | wc -c").strip() == str(ms["fz_ext4"]["files"]["/dir/f03.txt"]["size"]), "the rest of the volume reads"
        g.ok(f"umount {MNT}")
        g.ok(f"mount -t ext3 -o ro,nosuid /dev/vda {MNT}")                    # one driver, three names: an ext4 volume mounts as ext3 too, and /proc/mounts says what was typed
        assert re.search(rf"/dev/vda {MNT} ext3 ro", g.ok("cat /proc/mounts"))
        assert "-rwxr-xr-x" in g.ok(f"ls -l {MNT}/modes/setuid") and "-rwxr-sr-x" not in g.ok(f"ls -l {MNT}/modes/setgid") and "-rwxr-xr-x" in g.ok(f"ls -l {MNT}/modes/setgid"), "nosuid"
        g.ok(f"umount {MNT}")
        g.fails(f"mount -t ext2 -o ro,uid=5 /dev/vda {MNT} 2>&1")             # there are no owners to override
        assert "unknown mount option 'uid=5'" in g.ok("dmesg")
        g.ok(f"mount -t ext2 -r /dev/vda {MNT}")
        out = g.ok(f"df {MNT}")
        assert "/dev/vda" in out and "(ro)" in out
        g.ok(f"umount {MNT}")
        g.poweroff()
    assert sha(odd) == before


# ---------------------------------------------------------------------------------------------- the Linux root on a disk

def root_image(tar):
    """WORK/linux-root.img: the tree of the Linux root archive (tools/make-linux-root.py) as an ext4 volume. mke2fs -d runs in WSL as root (`wsl -u root`) so that the owners stay
    root, as they are in the archive: unpack the archive into a directory, then `mke2fs -t ext4 -d DIR IMAGE SIZE`. Built again when the archive is newer."""
    img = WORK / "linux-root.img"
    if img.exists() and img.stat().st_mtime > tar.stat().st_mtime and img.stat().st_mtime > Path(ext4img.__file__).stat().st_mtime:
        return img
    WORK.mkdir(parents=True, exist_ok=True)
    cmd = (f"set -e; D=$(mktemp -d /tmp/e4root-XXXXXX); tar -xpf '{wsl_path(tar)}' -C $D; rm -f '{wsl_path(img)}'; "
           f"mke2fs -q -F -t ext4 -b 4096 -O ^has_journal -L linuxroot -d $D '{wsl_path(img)}' 96M; rm -rf $D")
    r = ext4img.linux(cmd, root=True, check=False, capture_output=True, text=True)
    assert r.returncode == 0 and img.exists(), f"making the Linux root volume failed:\n{r.stdout}{r.stderr}"
    return img


@test
def ext4_linux_root_on_a_disk():
    """`mount -t ext4 -o ro /dev/vda /linux` makes the Linux root exactly as an unpacked archive does (docs/COMPAT.md, "The Linux root"): linux /bin/ls /, cat, bash and dash scripts,
    the dynamic programs of the loader tests (a PIE, a non-PIE with an interpreter, dlopen, fork and exec) print what a real Linux prints; /linux has a device number of its own, so
    `linuxroot` says a root is mounted; `linuxroot -d` refuses (there is nothing to empty, the volume is read only) and a second root cannot be unpacked over it; umount is refused
    while a Linux program runs in the root and works when none does; the image is unchanged."""
    import case_linux_dyn as dyn
    from case_linux import Inputs, build_inputs, compare_with_linux, TRACE
    if skip_reason() or dyn.skip_reason():
        print("SKIP:", skip_reason() or dyn.skip_reason())
        return
    build_inputs()
    tar = dyn.build_root()
    img = root_image(tar)
    before = sha(img)
    failures = []
    names = dyn.selected(dyn.SCRIPTS)
    files = ["dyn_hello", "dyn_hello_nopie", "dyn_dlopen", "dyn_spawn", "libdynplug.so"] + [s + ".sh" for s in names]
    with contextlib.ExitStack() as stack:
        srv = stack.enter_context(Inputs())
        vm = stack.enter_context(Qemu(cmdline="text user=root" + TRACE, name="t-ext4-linux-root", net="virtio-net-pci", mem="1G", disk=img))
        vm.expect(ROOT_PROMPT, timeout=scaled(300))
        for n in files:
            dyn.fetch_root(vm, srv.port, n)

        class W:                                                # what dyn.compare_all wants of a World
            def run(self, cmd, timeout=90):
                return dyn.root_run(vm, cmd, timeout=timeout)

        w = W()
        out, rc = w.run("linuxroot", timeout=30)
        assert rc != 0 and "no Linux root" in out, out
        out, rc = w.run("mkdir -p /linux && mount -t ext4 -o ro /dev/vda /linux && dmesg | grep 'ext4: vda mounted'", timeout=60)
        assert rc == 0 and "mounted on /linux" in out and "label 'linuxroot'" in out, f"{rc} {out!r}"
        out, rc = w.run("linuxroot", timeout=30)
        assert rc == 0 and "a Linux root is mounted on /linux" in out, out
        t0 = time.time()
        out, rc = w.run("linux /bin/ls /", timeout=120)
        print(f"     (linux /bin/ls / from the disk: {time.time() - t0:.1f} s, including the shell round trip)")
        assert rc == 0 and "usr" in out.split() and "lib64" in out.split() and "etc" in out.split(), f"linux /bin/ls /: {rc} {out!r}"
        out, rc = w.run("linux /bin/cat /etc/os-release", timeout=60)
        assert rc == 0 and out.startswith('PRETTY_NAME="Ubuntu'), f"cat /etc/os-release: {rc} {out!r}"
        out, rc = w.run("linux /bin/sh -c 'echo $((1+2))'", timeout=60)
        assert rc == 0 and out == "3\n", f"sh -c: {rc} {out!r}"
        out, rc = w.run("linux /bin/ls -ld /bin /etc/os-release", timeout=60)
        assert rc == 0 and re.search(r"^l\S+ .*root\s+root .* /bin -> usr/bin$", out, re.M) and re.search(r"^-rw-r--r-- .*root\s+root .* /etc/os-release", out, re.M), \
            f"a link, an owner and a mode of the root come from the inodes: {out!r}"
        # the loader programs, against what a real Linux prints
        dyn.compare_all(w, {"dyn_hello": ("FOO=bar linux /tmp/dyn_hello alpha beta", 90), "dyn_hello_nopie": ("FOO=bar linux /tmp/dyn_hello_nopie alpha beta", 90),
                            "dyn_dlopen": ("LD_LIBRARY_PATH=/tmp linux /tmp/dyn_dlopen", 120), "dyn_spawn": ("linux /tmp/dyn_spawn", 180)}, failures)
        # bash and dash scripts
        for shell, scripts in (("/bin/bash", names), ("/bin/sh", names[:1])):
            for n in scripts:
                cmd = f"rm -rf /tmp/wk; mkdir /tmp/wk; (cd /tmp/wk && linux /usr/bin/env -i PATH=/usr/bin:/bin {shell} /tmp/{n}.sh 2>&1 | cat)"
                out, rc = w.run(cmd, timeout=dyn.SCRIPTS[n][1])
                try:
                    compare_with_linux(n, out, rc, ("[Outside World]",))
                except AssertionError as e:
                    failures.append(f"({shell}) {e}")
        # the root is a volume, not an archive
        out, rc = w.run("linuxroot -d", timeout=60)
        assert rc != 0 and "ext4 volume, not an unpacked archive" in out and "nothing to empty" in out, f"linuxroot -d on a disk root: {rc} {out!r}"
        assert "/dev/vda /linux ext4 ro" in w.run("cat /proc/mounts")[0], "the volume is still mounted after linuxroot -d"
        out, rc = w.run("linux /bin/ls /usr | head -3", timeout=60)
        assert rc == 0 and out.strip(), "and the root still works"
        out, rc = w.run("linuxroot /tmp/nothing.tar", timeout=30)
        assert rc != 0 and "already holds" in out, f"{rc} {out!r}"
        out, rc = w.run("df /linux", timeout=30)
        assert "/dev/vda" in out and "(ro)" in out, out
        # umount: busy while a Linux program lives in the root, fine when none does
        out, rc = w.run("linux /bin/sleep 6 & sleep 2; umount /linux 2>&1; wait", timeout=60)
        assert re.search(r"busy|Busy", out), f"umount while a Linux program runs in the root: {out!r}"
        out, rc = w.run("umount /linux; linuxroot", timeout=60)
        assert "no Linux root" in out, f"umount of the disk root: {out!r}"
        assert "/dev/vda" not in w.run("cat /proc/mounts", timeout=30)[0], "the volume is gone from /proc/mounts"
        out, rc = w.run("dmesg | grep -E 'WARNING|panic|BUG:|Oops'", timeout=30)
        assert not out.strip(), f"warnings in the log: {out!r}"
        vm.send("poweroff\n")
        assert vm.wait_exit(scaled(40)) is not None, "guest did not power off"
    assert not failures, "\n\n".join(failures)
    assert sha(img) == before, "the guest wrote to the Linux root volume"
