#!/usr/bin/env python3
"""Read-only look at an Anex86 .hdi image of a PC-98 hard disk (docs/the project plan, section 5.1; docs/PC98.md).

  python tools/hdi-look.py PATH [--depth N]                    header, partition table, the DOS volume's parameters and its directory tree (FAT12/FAT16)
  python tools/hdi-look.py PATH --extract NAME DEST            copy one file out of the volume (NAME matches the end of the path, case-insensitively)

The disk has a 4096-byte Anex86 header, then sectors of its own size (512 here); sector 1 holds PC-98's partition table (32-byte entries); the DOS volume uses the logical sector size of its
own BIOS parameter block, which can be larger than the disk's (1024 on 512): the volume is addressed in its own sectors. Nothing is written. The owner's disk images never go into the repository.
"""
import struct
import sys


class Hdi:
    def __init__(self, data):
        hdr = struct.unpack_from("<8I", data, 0)
        self.hsize, self.dsize, self.secsz, self.spt, self.heads, self.cyls = hdr[2], hdr[3], hdr[4], hdr[5], hdr[6], hdr[7]
        if not (self.secsz in (256, 512, 1024) and self.spt and self.heads and self.cyls and self.hsize >= 32):
            raise ValueError("not an Anex86 HDI header (sector size, geometry)")
        self.img = data[self.hsize:]

    def sector(self, lba, n=1):
        return self.img[lba * self.secsz:(lba + n) * self.secsz]

    def partitions(self):
        """PC-98 partition table (second sector): (index, mid, sid, start cylinder, head, sector, end cylinder, head, sector, name)."""
        table = self.sector(1)
        out = []
        for i in range(len(table) // 32):
            e = table[i * 32:(i + 1) * 32]
            if len(e) < 32 or (e[0] == 0 and e[1] == 0):
                continue
            out.append((i, e[0], e[1], e[10] | e[11] << 8, e[9], e[8], e[14] | e[15] << 8, e[13], e[12], e[16:32].split(b"\0")[0].decode("shift_jis", "replace").strip()))
        return out


class Volume:
    """A FAT12/FAT16 DOS volume inside the disk, addressed in the volume's own sectors."""

    def __init__(self, disk, part):
        self.disk = disk
        _, _, _, scyl, shead, ssect, *_ = part
        self.start = (scyl * disk.heads + shead) * disk.spt + ssect
        boot = disk.sector(self.start)
        self.bps, self.spc, self.rsv, self.nfat, self.rootent = struct.unpack_from("<HBHBH", boot, 0x0B)
        self.fatsz = struct.unpack_from("<H", boot, 0x16)[0]
        self.total = struct.unpack_from("<H", boot, 0x13)[0] or struct.unpack_from("<I", boot, 0x20)[0]
        self.oem = boot[3:11].decode("ascii", "replace")
        if not self.bps or not self.spc or not self.fatsz:
            raise ValueError("the partition does not start with a DOS boot sector (no sensible BIOS parameter block)")
        self.rootsec = (self.rootent * 32 + self.bps - 1) // self.bps
        self.fat0 = self.rsv
        self.root0 = self.fat0 + self.nfat * self.fatsz
        self.data0 = self.root0 + self.rootsec
        self.nclusters = (self.total - self.data0) // self.spc
        self.fat16 = self.nclusters >= 4085
        self.fat = self.vsector(self.fat0, self.fatsz)

    def vsector(self, lba, n=1):
        o = self.start * self.disk.secsz + lba * self.bps
        return self.disk.img[o:o + n * self.bps]

    def next(self, c):
        if self.fat16:
            return struct.unpack_from("<H", self.fat, c * 2)[0]
        v = struct.unpack_from("<H", self.fat, c * 3 // 2)[0]
        return (v >> 4) if c & 1 else (v & 0xFFF)

    def chain(self, c):
        out, lim = [], (0xFFF8 if self.fat16 else 0xFF8)
        while 2 <= c < lim and len(out) < 100000:
            out.append(c)
            c = self.next(c)
        return out

    def cluster(self, c):
        return self.vsector(self.data0 + (c - 2) * self.spc, self.spc)

    def read_dir(self, first_cluster):
        raw = b"".join(self.cluster(c) for c in self.chain(first_cluster)) if first_cluster else self.vsector(self.root0, self.rootsec)
        out = []
        for o in range(0, len(raw), 32):
            e = raw[o:o + 32]
            if len(e) < 32 or e[0] == 0:
                break
            if e[0] == 0xE5 or e[11] & 0x08 or e[11] == 0x0F:
                continue
            name, ext = e[:8].decode("shift_jis", "replace").rstrip(), e[8:11].decode("shift_jis", "replace").rstrip()
            out.append((name + ("." + ext if ext else ""), e[11], struct.unpack_from("<H", e, 26)[0], struct.unpack_from("<I", e, 28)[0]))
        return out

    def tree(self, depth=2):
        items = []

        def walk(cluster, prefix, level):
            for full, attr, cl, size in self.read_dir(cluster):
                if full in (".", ".."):
                    continue
                isdir = bool(attr & 0x10)
                items.append((prefix + "/" + full, isdir, size, cl))
                if isdir and level < depth:
                    walk(cl, prefix + "/" + full, level + 1)

        walk(0, "", 1)
        return items

    def read_file(self, cl, size):
        return b"".join(self.cluster(c) for c in self.chain(cl))[:size]


def open_image(path):
    disk = Hdi(open(path, "rb").read())
    parts = disk.partitions()
    usable = [p for p in parts if p[1] & 0x80]
    if not usable:
        raise ValueError("no bootable DOS partition in the partition table")
    return disk, parts, Volume(disk, usable[0])


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 2
    path = argv[0]
    depth = int(argv[argv.index("--depth") + 1]) if "--depth" in argv else 2
    disk, parts, vol = open_image(path)
    print(f"{path.replace(chr(92), '/').split('/')[-1]}: header {disk.hsize}, data {disk.dsize}, {disk.secsz} bytes/sector, {disk.spt} sectors/track, {disk.heads} heads, {disk.cyls} cylinders")
    for i, mid, sid, scyl, shead, ssect, ecyl, ehead, esect, name in parts:
        print(f"  partition entry {i}: mid {mid:#04x} sid {sid:#04x} start C{scyl} H{shead} S{ssect}  end C{ecyl} H{ehead} S{esect}  name '{name}'")
    print(f"  DOS volume at disk sector {vol.start}: {vol.bps} bytes/sector, {vol.spc} sectors/cluster, {vol.rsv} reserved, {vol.nfat} FATs of {vol.fatsz} sectors, {vol.rootent} root entries, "
          f"{vol.total} sectors, OEM '{vol.oem}', FAT{'16' if vol.fat16 else '12'}, {vol.nclusters} clusters")
    items = vol.tree(depth)
    if "--extract" in argv:
        k = argv.index("--extract")
        want, dest = argv[k + 1].upper(), argv[k + 2]
        for p, isdir, size, cl in items:
            if p.upper().endswith(want) and not isdir:
                open(dest, "wb").write(vol.read_file(cl, size))
                print(f"extracted {p} ({size} bytes) to {dest}")
                return 0
        print("no such file:", argv[k + 1])
        return 1
    print(f"  {len(items)} entries (depth {depth}):")
    for p, isdir, size, cl in items:
        print(f"    {p}{'/' if isdir else ''}  {'' if isdir else size}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
