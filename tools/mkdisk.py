#!/usr/bin/env python3
"""mkdisk -- TouhouOS disk image builder and FAT/GPT checker.  Pure Python, no external programs.

Build a raw disk image (protective MBR + GPT, an EFI System Partition, a HUMANVILLAGE data partition, extras):

  python tools/mkdisk.py out.img --size 512M --esp build/esp --hv-size 256M --hv-dir some/files
  python tools/mkdisk.py out.img --size 300M --no-esp                         (data disk only, HUMANVILLAGE takes the rest)
  python tools/mkdisk.py out.img --size 700M --sector-size 4096               (4Kn disk: FAT32 needs >= 256 MiB per volume)
  python tools/mkdisk.py out.img --size 128M --table mbr --no-esp             (MBR instead of GPT)
  python tools/mkdisk.py out.img --size 200M --no-hv --extra DECOY:40M:fat32  (any other partition; fat16/raw also exist)

Inspect what a kernel (or anything else) wrote.  This is an independent FAT reader and fsck:

  python tools/mkdisk.py --check out.img          GPT + every FAT volume: FAT copies, FSInfo, LFN checksums, chains,
                                                  cross-links, lost clusters, "." / ".." entries, duplicate names, ...
  python tools/mkdisk.py --ls out.img:2 /reimu    list a directory of partition 2
  python tools/mkdisk.py --cat out.img:2 /reimu/a.txt
  python tools/mkdisk.py --extract out.img:2 /reimu DEST      copy a file or a whole tree to the host
  python tools/mkdisk.py --tree out.img:2         print the whole tree with sizes

As a library (tests/case_storage.py): build_image(), partitions(), open_volume(), FatReader, check_image(), set_dirty().

Volume labels: GPT partition names are UTF-16 and may be long ("HUMANVILLAGE"); the FAT label is limited to 11 characters,
so the volume itself carries it truncated ("HUMANVILLAG").
"""
import argparse
import calendar
import hashlib
import random
import re
import struct
import sys
import time
import uuid
import zlib
from array import array
from pathlib import Path

MIB = 1 << 20
GIB = 1 << 30
DEFAULT_TS = 1767225600          # 2026-01-01 00:00:00 UTC: fixed so that built images are reproducible

GPT_TYPE_ESP = uuid.UUID("C12A7328-F81F-11D2-BA4B-00A0C93EC93B")
GPT_TYPE_BASIC = uuid.UUID("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7")
GPT_TYPE_LINUX = uuid.UUID("0FC63DAF-8483-4772-8E79-3D69D8477DE4")

ATTR_RO, ATTR_HIDDEN, ATTR_SYSTEM, ATTR_VOLUME, ATTR_DIR, ATTR_ARCHIVE = 0x01, 0x02, 0x04, 0x08, 0x10, 0x20
ATTR_LFN = 0x0F

SFN_OK = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789$%'-_@~!(){}^#&")
LFN_ILLEGAL = set('"*/:<>?\\|')


class MkdiskError(Exception):
    pass


def parse_size(text):
    t = str(text).strip().lower()
    mult = 1
    for suffix, m in (("k", 1 << 10), ("m", 1 << 20), ("g", 1 << 30), ("t", 1 << 40)):
        for tail in (suffix + "ib", suffix + "b", suffix):
            if t.endswith(tail):
                t, mult = t[:-len(tail)], m
                break
        if mult != 1:
            break
    return int(float(t) * mult)


# ---------------------------------------------------------------------------------------------------------------------
# byte-addressable windows onto a file

class Window:
    """A window [base, base+length) of an open binary file; all offsets are relative to the window."""

    def __init__(self, f, base=0, length=None, writable=False):
        self.f = f
        self.base = base
        if length is None:
            f.seek(0, 2)
            length = f.tell() - base
        self.length = length
        self.writable = writable

    def read(self, off, n):
        if off < 0 or off + n > self.length:
            raise MkdiskError(f"read of {n} bytes at {off} is outside the window ({self.length} bytes)")
        self.f.seek(self.base + off)
        data = self.f.read(n)
        if len(data) < n:
            data += bytes(n - len(data))
        return data

    def write(self, off, data):
        if not self.writable:
            raise MkdiskError("window is read-only")
        if off < 0 or off + len(data) > self.length:
            raise MkdiskError(f"write of {len(data)} bytes at {off} is outside the window ({self.length} bytes)")
        self.f.seek(self.base + off)
        self.f.write(data)


# ---------------------------------------------------------------------------------------------------------------------
# DOS time

def dos_stamp(ts):
    """unix seconds -> (date, time, hundredths-of-a-second-in-10ms-units: 0 or 100)"""
    t = time.gmtime(max(int(ts), 315532800))
    year = min(max(t.tm_year - 1980, 0), 127)
    date = (year << 9) | (t.tm_mon << 5) | t.tm_mday
    tm = (t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2)
    return date, tm, (t.tm_sec % 2) * 100


def from_dos(date, tm, tenths=0):
    """-> unix seconds, or None if the fields are not a valid date"""
    if date == 0:
        return None
    year, mon, day = (date >> 9) + 1980, (date >> 5) & 15, date & 31
    hour, minute, sec = tm >> 11, (tm >> 5) & 63, (tm & 31) * 2
    if not (1 <= mon <= 12) or not (1 <= day <= calendar.monthrange(year, mon)[1]) or hour > 23 or minute > 59 or sec > 59:
        return None
    return calendar.timegm((year, mon, day, hour, minute, sec)) + (tenths // 100 if tenths <= 199 else 0)


# ---------------------------------------------------------------------------------------------------------------------
# GPT / MBR

def crc32(b):
    return zlib.crc32(b) & 0xFFFFFFFF


def _utf16_name(name):
    raw = name.encode("utf-16-le")[:72]
    return raw + bytes(72 - len(raw))


def gpt_entry_bytes(type_guid, unique, first, last, name, attrs=0):
    return type_guid.bytes_le + unique.bytes_le + struct.pack("<QQQ", first, last, attrs) + _utf16_name(name)


GPT_HDR = "<8sIIIIQQQQ16sQIII"


def gpt_write(win, ss, total_sectors, entries, disk_guid):
    """win: whole-disk window. entries: list of dict(type, unique, first, last, name). Writes MBR, both headers, both arrays."""
    nent, esize = 128, 128
    array_bytes = nent * esize
    array_sectors = (array_bytes + ss - 1) // ss
    first_usable = 2 + array_sectors
    last_usable = total_sectors - 2 - array_sectors
    blob = bytearray(array_sectors * ss)
    for i, e in enumerate(entries):
        blob[i * esize:(i + 1) * esize] = gpt_entry_bytes(e["type"], e["unique"], e["first"], e["last"], e["name"])
    entries_crc = crc32(bytes(blob[:array_bytes]))

    def header(my_lba, alt_lba, entries_lba):
        h = bytearray(struct.pack(GPT_HDR, b"EFI PART", 0x00010000, 92, 0, 0, my_lba, alt_lba, first_usable,
                                  last_usable, disk_guid.bytes_le, entries_lba, nent, esize, entries_crc))
        struct.pack_into("<I", h, 16, crc32(bytes(h)))
        return bytes(h).ljust(ss, b"\0")

    mbr = bytearray(ss)                                     # protective MBR
    size = min(total_sectors - 1, 0xFFFFFFFF)
    mbr[446:462] = struct.pack("<B3sB3sII", 0, b"\x00\x02\x00", 0xEE, b"\xff\xff\xff", 1, size)
    mbr[510:512] = b"\x55\xaa"
    win.write(0, bytes(mbr))
    win.write(1 * ss, header(1, total_sectors - 1, 2))
    win.write(2 * ss, bytes(blob))
    backup_entries = total_sectors - 1 - array_sectors
    win.write(backup_entries * ss, bytes(blob))
    win.write((total_sectors - 1) * ss, header(total_sectors - 1, 1, backup_entries))
    return first_usable, last_usable


def mbr_write(win, ss, entries, signature=0x544F5548):
    mbr = bytearray(ss)
    struct.pack_into("<I", mbr, 440, signature)
    for i, e in enumerate(entries[:4]):
        mbr[446 + 16 * i:462 + 16 * i] = struct.pack("<B3sB3sII", 0x80 if i == 0 else 0, b"\xfe\xff\xff", e["mbr_type"],
                                                     b"\xfe\xff\xff", e["first"], e["last"] - e["first"] + 1)
    mbr[510:512] = b"\x55\xaa"
    win.write(0, bytes(mbr))


def detect_sector_size(win):
    """Logical sector size of a raw image: wherever 'EFI PART' sits at LBA 1, else 512."""
    for ss in (512, 4096, 1024, 2048):
        if win.length >= 2 * ss and win.read(ss, 8) == b"EFI PART":
            return ss
    return 512


def _looks_like_fat(win):
    try:
        b = win.read(0, 512)
    except MkdiskError:
        return False
    if b[510:512] != b"\x55\xaa":
        return False
    bps, spc = struct.unpack_from("<HB", b, 11)
    return bps in (512, 1024, 2048, 4096) and spc in (1, 2, 4, 8, 16, 32, 64, 128) and b[16] in (1, 2, 3, 4) and b[21] >= 0xF0


def partitions(path_or_win):
    """-> (sector_size, [dict(index, name, type, first, last, offset, length, kind, sector_size)]) for GPT or MBR images;
    a bare FAT volume without a partition table is returned as one entry of kind 'whole'."""
    close = None
    if isinstance(path_or_win, (str, Path)):
        close = open(path_or_win, "rb")
        win = Window(close)
    else:
        win = path_or_win
    try:
        ss = detect_sector_size(win)
        out = []
        if win.length >= 2 * ss and win.read(ss, 8) == b"EFI PART":
            hdr = struct.unpack(GPT_HDR, win.read(ss, 92))
            entries_lba, nent, esize = hdr[10], hdr[11], hdr[12]
            raw = win.read(entries_lba * ss, nent * esize)
            for i in range(nent):
                e = raw[i * esize:(i + 1) * esize]
                tg = uuid.UUID(bytes_le=e[0:16])
                if tg.int == 0:
                    continue
                first, last = struct.unpack_from("<QQ", e, 32)
                name = e[56:128].decode("utf-16-le", "replace").split("\0")[0]
                out.append(dict(index=i + 1, name=name, type=tg, first=first, last=last, offset=first * ss,
                                length=(last - first + 1) * ss, kind="gpt", sector_size=ss))
        elif win.read(510, 2) == b"\x55\xaa" and not _looks_like_fat(win):
            mbr = win.read(0, 512)
            for i in range(4):
                e = mbr[446 + 16 * i:462 + 16 * i]
                first, count = struct.unpack_from("<II", e, 8)
                if e[4] == 0 or count == 0:
                    continue
                out.append(dict(index=i + 1, name="", type=e[4], first=first, last=first + count - 1, offset=first * ss,
                                length=count * ss, kind="mbr", sector_size=ss))
        else:
            out.append(dict(index=1, name="", type=None, first=0, last=win.length // ss - 1, offset=0, length=win.length,
                            kind="whole", sector_size=ss))
        return ss, out
    finally:
        if close:
            close.close()


# ---------------------------------------------------------------------------------------------------------------------
# FAT names

def lfn_checksum(sfn11):
    s = 0
    for b in sfn11:
        s = (((s & 1) << 7) | (s >> 1)) & 0xFF
        s = (s + b) & 0xFF
    return s


def _is_plain_83(name):
    """(BASE, EXT, ntres) if the name is a legal 8.3 name in a single case per part; None if it needs a long name."""
    if "." in name:
        base, ext = name.rsplit(".", 1)
    else:
        base, ext = name, ""
    if not base or len(base) > 8 or len(ext) > 3 or "." in base:
        return None
    for ch in base + ext:
        if not ch.isascii() or ch.upper() not in SFN_OK:
            return None
    for part in (base, ext):
        if part != part.upper() and part != part.lower():
            return None
    return base.upper(), ext.upper(), (0x08 if base != base.upper() else 0) | (0x10 if ext != ext.upper() else 0)


def make_short_name(name, taken):
    """-> (sfn bytes[11], ntres, needs_lfn). `taken` holds the 11-byte short names already used in the directory."""
    plain = _is_plain_83(name)
    if plain is not None:
        base, ext, ntres = plain
        sfn = base.ljust(8).encode("ascii") + ext.ljust(3).encode("ascii")
        if sfn in taken:
            raise MkdiskError(f"duplicate short name for {name!r}")
        return sfn, ntres, False
    lossy = False
    s = name.lstrip(". ")
    if s != name:
        lossy = True
    if " " in s:
        s = s.replace(" ", "")
        lossy = True
    if "." in s:
        base, ext = s.rsplit(".", 1)
        if "." in base:
            base = base.replace(".", "")
            lossy = True
    else:
        base, ext = s, ""

    def clean(part):
        nonlocal lossy
        out = []
        for ch in part:
            u = ch.upper()
            if u.isascii() and u in SFN_OK:
                out.append(u)
            else:
                out.append("_")
                lossy = True
        return "".join(out)

    base, ext = clean(base), clean(ext)
    if not base:
        base, lossy = "_", True
    if len(base) > 8 or len(ext) > 3:
        lossy = True
    base, ext = base[:8], ext[:3]
    if not lossy:
        sfn = base.ljust(8).encode("ascii") + ext.ljust(3).encode("ascii")
        if sfn not in taken:
            return sfn, 0, True
    for n in range(1, 1000000):
        tail = "~%d" % n
        sfn = (base[:8 - len(tail)] + tail).ljust(8).encode("ascii") + ext.ljust(3).encode("ascii")
        if sfn not in taken:
            return sfn, 0, True
    raise MkdiskError(f"cannot find a short name for {name!r}")


def pack_lfn_entries(name, checksum):
    raw = name.encode("utf-16-le")
    n = len(raw) // 2
    if n > 255:
        raise MkdiskError(f"name too long: {name!r}")
    u = list(struct.unpack("<%dH" % n, raw))
    count = (n + 12) // 13
    if n % 13:
        u += [0x0000] + [0xFFFF] * (13 * count - n - 1)
    out = []
    for i in range(count, 0, -1):
        chunk = u[(i - 1) * 13:i * 13]
        e = bytearray(32)
        e[0] = i | (0x40 if i == count else 0)
        e[1:11] = struct.pack("<5H", *chunk[0:5])
        e[11] = ATTR_LFN
        e[13] = checksum
        e[14:26] = struct.pack("<6H", *chunk[5:11])
        e[28:32] = struct.pack("<2H", *chunk[11:13])
        out.append(bytes(e))
    return out


def lfn_slot_count(name):
    return (len(name.encode("utf-16-le")) // 2 + 12) // 13


def pack_sfn_entry(sfn, attr, ntres, first_cluster, size, ctime, mtime, atime):
    cdate, ctm, ctenth = dos_stamp(ctime)
    mdate, mtm, _ = dos_stamp(mtime)
    adate, _, _ = dos_stamp(atime)
    return struct.pack("<11sBBBHHHHHHHI", sfn, attr, ntres, ctenth, ctm, cdate, adate, first_cluster >> 16, mtm, mdate,
                       first_cluster & 0xFFFF, size)


# ---------------------------------------------------------------------------------------------------------------------
# FAT formatting and populating

def default_cluster_bytes(size):
    for limit, cb in ((64 * MIB, 512), (128 * MIB, 1024), (256 * MIB, 2048), (8 * GIB, 4096), (16 * GIB, 8192),
                      (32 * GIB, 16384)):
        if size <= limit:
            return cb
    return 32768


class DirNode:
    def __init__(self, name, parent, ts):
        self.name, self.parent, self.ts = name, parent, ts
        self.children = []              # [name, kind, payload]: kind 'd' -> DirNode, 'f' -> dict(data, size, ts, first)
        self.first_cluster = 0
        self.prepared = []

    def has(self, name):
        low = name.lower()
        return any(c[0].lower() == low for c in self.children)


class FatBuilder:
    """Formats a window as FAT32 (bits=32) or FAT16 and fills it with files.  Layout: directories first (the root dir
    of a FAT32 volume lands in cluster 2), then file data, all contiguous."""

    def __init__(self, win, bps, bits=32, label="NO NAME", cluster_bytes=None, nfats=2, reserved=None, root_entries=512,
                 volume_id=0x544F5548, ts=DEFAULT_TS, hidden=0):
        if sys.byteorder != "little":
            raise MkdiskError("big-endian hosts are not supported")
        self.win, self.bps, self.bits, self.ts, self.hidden = win, bps, bits, ts, hidden
        self.label = label.upper()[:11]
        self.volume_id = volume_id
        self.nfats = nfats
        self.total = win.length // bps
        cb = max(cluster_bytes or default_cluster_bytes(win.length), bps)
        if bits == 16:
            while self.total * bps // cb > 65524 and cb < 65536:
                cb *= 2
            self.reserved = reserved or 1
            self.root_entries = root_entries
            self.root_sectors = (root_entries * 32 + bps - 1) // bps
        else:
            self.reserved = reserved or 32
            self.root_entries = 0
            self.root_sectors = 0
        self.spc = cb // bps
        if self.spc not in (1, 2, 4, 8, 16, 32, 64, 128):
            raise MkdiskError(f"cluster size {cb} is not a power-of-two multiple of the sector size {bps}")
        self.cluster_bytes = cb
        self._geometry()
        itemsize = array("I").itemsize
        if bits == 32 and itemsize != 4:
            raise MkdiskError("unsupported platform: unsigned int is not 32 bits")
        self.fat = array("I" if bits == 32 else "H", [0]) * (self.nclusters + 2)
        self.eoc = 0x0FFFFFFF if bits == 32 else 0xFFFF
        self.fat[0] = (0x0FFFFF00 if bits == 32 else 0xFF00) | 0xF8
        self.fat[1] = self.eoc
        self.next_free = 2
        self.root = DirNode("", None, ts)
        self.root_cluster = 0

    def _geometry(self):
        bps, total, spc, nfats = self.bps, self.total, self.spc, self.nfats
        per_sector = bps // (4 if self.bits == 32 else 2)
        fatsz = 1
        while True:
            clusters = (total - self.reserved - nfats * fatsz - self.root_sectors) // spc
            if clusters + 2 <= fatsz * per_sector:
                break
            fatsz += 1
        self.fatsz = fatsz
        self.data_start = self.reserved + nfats * fatsz + self.root_sectors
        self.nclusters = (total - self.data_start) // spc
        if self.bits == 32 and self.nclusters < 65525:
            raise MkdiskError(f"a FAT32 volume of {self.win.length >> 20} MiB with {self.cluster_bytes}-byte clusters has only "
                              f"{self.nclusters} clusters (FAT32 needs >= 65525): make it larger or the clusters smaller")
        if self.bits == 32 and self.nclusters >= 0x0FFFFFF5:
            raise MkdiskError("too many clusters for FAT32; use a larger cluster size")
        if self.bits == 16 and not (4085 <= self.nclusters <= 65524):
            raise MkdiskError(f"FAT16 needs 4085..65524 clusters, this volume would have {self.nclusters}")

    def _alloc(self, n):
        if self.next_free + n > self.nclusters + 2:
            raise MkdiskError("volume is full")
        first = self.next_free
        for c in range(first, first + n - 1):
            self.fat[c] = c + 1
        self.fat[first + n - 1] = self.eoc
        self.next_free += n
        return first

    def cluster_offset(self, c):
        return (self.data_start + (c - 2) * self.spc) * self.bps

    # -- tree building
    def _resolve_dir(self, path):
        node = self.root
        for part in [p for p in path.split("/") if p]:
            nxt = None
            for name, kind, payload in node.children:
                if name.lower() == part.lower():
                    if kind != "d":
                        raise MkdiskError(f"{part!r} is a file")
                    nxt = payload
                    break
            if nxt is None:
                self._check_name(part)
                nxt = DirNode(part, node, self.ts)
                node.children.append([part, "d", nxt])
            node = nxt
        return node

    @staticmethod
    def _check_name(name):
        if not name or name in (".", "..") or name[-1] in ". ":
            raise MkdiskError(f"bad name {name!r}")
        for ch in name:
            if ch in LFN_ILLEGAL or ord(ch) < 32:
                raise MkdiskError(f"illegal character {ch!r} in {name!r}")

    def mkdir(self, path):
        return self._resolve_dir(path)

    def add_file(self, path, data, ts=None):
        """data: bytes, or a host Path whose content is copied at finish()."""
        path = path.strip("/")
        parent, _, name = path.rpartition("/")
        self._check_name(name)
        d = self._resolve_dir(parent)
        if d.has(name):
            raise MkdiskError(f"duplicate name {name!r} in /{parent}")
        size = len(data) if isinstance(data, (bytes, bytearray)) else Path(data).stat().st_size
        d.children.append([name, "f", dict(data=data, size=size, ts=ts or self.ts, first=0)])

    def add_tree(self, host_dir, dest="/"):
        host_dir = Path(host_dir)
        for p in sorted(host_dir.rglob("*")):
            target = dest.rstrip("/") + "/" + p.relative_to(host_dir).as_posix()
            if p.is_dir():
                self.mkdir(target)
            elif p.is_file():
                self.add_file(target, p, ts=int(p.stat().st_mtime))

    # -- serialization
    def _prepare(self, d, is_root):
        taken = set()
        d.prepared = []
        nslots = 1 if is_root else 2                    # volume label entry, or "." and ".."
        for name, kind, payload in d.children:
            sfn, ntres, lfn = make_short_name(name, taken)
            taken.add(sfn)
            d.prepared.append((name, kind, payload, sfn, ntres, lfn))
            nslots += 1 + (lfn_slot_count(name) if lfn else 0)
        if is_root and self.bits == 16:
            if nslots > self.root_entries:
                raise MkdiskError(f"FAT16 root directory is full ({nslots} > {self.root_entries} entries)")
        else:
            clusters = max(1, (nslots * 32 + self.cluster_bytes - 1) // self.cluster_bytes)
            d.first_cluster = self._alloc(clusters)
            if is_root:
                self.root_cluster = d.first_cluster
        for name, kind, payload, *_ in d.prepared:
            if kind == "d":
                self._prepare(payload, False)

    def _place_files(self, d):
        for name, kind, payload in d.children:
            if kind == "d":
                self._place_files(payload)
                continue
            if payload["size"]:
                n = (payload["size"] + self.cluster_bytes - 1) // self.cluster_bytes
                payload["first"] = self._alloc(n)
                data = payload["data"]
                off = self.cluster_offset(payload["first"])
                if isinstance(data, (bytes, bytearray)):
                    self.win.write(off, bytes(data))
                else:
                    with open(data, "rb") as src:
                        while True:
                            chunk = src.read(4 * MIB)
                            if not chunk:
                                break
                            self.win.write(off, chunk)
                            off += len(chunk)

    def _serialize(self, d, is_root):
        out = bytearray()
        if is_root:
            out += pack_sfn_entry(self.label.ljust(11).encode("ascii", "replace"), ATTR_VOLUME | ATTR_ARCHIVE, 0, 0, 0,
                                  self.ts, self.ts, self.ts)
        else:
            parent_cluster = 0 if d.parent is self.root else d.parent.first_cluster
            out += pack_sfn_entry(b".          ", ATTR_DIR, 0, d.first_cluster, 0, d.ts, d.ts, d.ts)
            out += pack_sfn_entry(b"..         ", ATTR_DIR, 0, parent_cluster, 0, d.ts, d.ts, d.ts)
        for name, kind, payload, sfn, ntres, lfn in d.prepared:
            if lfn:
                for e in pack_lfn_entries(name, lfn_checksum(sfn)):
                    out += e
            if kind == "d":
                out += pack_sfn_entry(sfn, ATTR_DIR, ntres, payload.first_cluster, 0, payload.ts, payload.ts, payload.ts)
            else:
                out += pack_sfn_entry(sfn, ATTR_ARCHIVE, ntres, payload["first"], payload["size"], payload["ts"],
                                      payload["ts"], payload["ts"])
        return out

    def _write_dir(self, d, is_root):
        data = self._serialize(d, is_root)
        if is_root and self.bits == 16:
            self.win.write((self.reserved + self.nfats * self.fatsz) * self.bps, bytes(data.ljust(self.root_entries * 32, b"\0")))
        else:
            self.win.write(self.cluster_offset(d.first_cluster), bytes(data))
        for name, kind, payload, *_ in d.prepared:
            if kind == "d":
                self._write_dir(payload, False)

    def finish(self):
        self._prepare(self.root, True)
        self._place_files(self.root)
        self._write_dir(self.root, True)
        self._write_metadata()

    def free_clusters(self):
        return self.nclusters - (self.next_free - 2)

    def _boot_sector(self):
        b = bytearray(self.bps)
        total16 = self.total if self.total < 65536 and self.bits == 16 else 0
        total32 = self.total if total16 == 0 else 0
        b[0:3] = b"\xeb\x58\x90" if self.bits == 32 else b"\xeb\x3c\x90"
        b[3:11] = b"TOUHOUOS"
        struct.pack_into("<HBHBHHBHHHII", b, 11, self.bps, self.spc, self.reserved, self.nfats, self.root_entries, total16,
                         0xF8, self.fatsz if self.bits == 16 else 0, 63, 255, self.hidden & 0xFFFFFFFF, total32)
        label = self.label.ljust(11).encode("ascii", "replace")
        if self.bits == 32:
            struct.pack_into("<IHHIHH", b, 36, self.fatsz, 0, 0, self.root_cluster, 1, 6)
            struct.pack_into("<BBBI11s8s", b, 64, 0x80, 0, 0x29, self.volume_id, label, b"FAT32   ")
        else:
            struct.pack_into("<BBBI11s8s", b, 36, 0x80, 0, 0x29, self.volume_id, label, b"FAT16   ")
        b[510:512] = b"\x55\xaa"
        return bytes(b)

    def _fsinfo(self):
        b = bytearray(self.bps)
        struct.pack_into("<I", b, 0, 0x41615252)
        struct.pack_into("<I", b, 484, 0x61417272)
        struct.pack_into("<II", b, 488, self.free_clusters(), self.next_free)
        struct.pack_into("<I", b, 508, 0xAA550000)
        return bytes(b)

    def _write_metadata(self):
        w, bps = self.win, self.bps
        boot = self._boot_sector()
        w.write(0, boot)
        if self.bits == 32:
            fsinfo = self._fsinfo()
            w.write(1 * bps, fsinfo)
            w.write(6 * bps, boot)
            w.write(7 * bps, fsinfo)
        fat_bytes = self.fat.tobytes().ljust(self.fatsz * bps, b"\0")
        for i in range(self.nfats):
            w.write((self.reserved + i * self.fatsz) * bps, fat_bytes)


# ---------------------------------------------------------------------------------------------------------------------
# FAT reader / checker

class Report:
    def __init__(self, name=""):
        self.name = name
        self.errors, self.warnings, self.notes = [], [], []
        self.stats = {}

    def err(self, msg):
        self.errors.append(msg)

    def warn(self, msg):
        self.warnings.append(msg)

    def note(self, msg):
        self.notes.append(msg)

    @property
    def ok(self):
        return not self.errors

    def extend(self, other, prefix=""):
        self.errors += [prefix + e for e in other.errors]
        self.warnings += [prefix + e for e in other.warnings]
        self.notes += [prefix + e for e in other.notes]

    def text(self, verbose=False):
        out = ["ERROR   " + e for e in self.errors] + ["warning " + w for w in self.warnings]
        if verbose:
            out += ["note    " + n for n in self.notes]
        out.append(f"{self.name or 'result'}: {len(self.errors)} error(s), {len(self.warnings)} warning(s)")
        return "\n".join(out)


class Ent:
    __slots__ = ("name", "short", "is_dir", "attr", "first", "size", "mtime", "ctime", "atime", "slot", "nslots", "has_lfn",
                 "sfn", "ntres", "path")

    def __repr__(self):
        return f"<Ent {self.path!r} {'dir' if self.is_dir else 'file'} first={self.first} size={self.size}>"


_FAT_HDR = "<HBHBHHBHHHII"


class FatReader:
    """Independent FAT16/FAT32 reader and checker over a Window that starts at the volume's first sector."""

    def __init__(self, win, name=""):
        self.win, self.name = win, name
        b = win.read(0, 512)
        self.boot = b
        if b[510:512] != b"\x55\xaa":
            raise MkdiskError("no boot sector signature")
        (self.bps, self.spc, self.reserved, self.nfats, self.root_entries, total16, self.media, fatsz16, self.spt, self.heads,
         self.hidden, total32) = struct.unpack_from(_FAT_HDR, b, 11)
        if self.bps not in (512, 1024, 2048, 4096) or self.spc not in (1, 2, 4, 8, 16, 32, 64, 128) or not (1 <= self.nfats <= 4):
            raise MkdiskError(f"implausible BPB (bps={self.bps}, spc={self.spc}, nfats={self.nfats})")
        self.total = total16 or total32
        self.fatsz32 = struct.unpack_from("<I", b, 36)[0]
        self.fatsz = fatsz16 or self.fatsz32
        self.root_sectors = (self.root_entries * 32 + self.bps - 1) // self.bps
        self.data_start = self.reserved + self.nfats * self.fatsz + self.root_sectors
        self.cluster_bytes = self.spc * self.bps
        if self.total < self.data_start:
            raise MkdiskError("volume smaller than its own metadata")
        self.nclusters = (self.total - self.data_start) // self.spc
        self.bits = 12 if self.nclusters < 4085 else 16 if self.nclusters < 65525 else 32
        self.structural32 = fatsz16 == 0 and self.root_entries == 0 and self.fatsz32 != 0
        if self.bits == 12:
            raise MkdiskError("FAT12 volumes are not supported")
        if self.bits == 32:
            self.ext_flags, self.fs_version, self.root_cluster, self.fsinfo_sector, self.backup_sector = struct.unpack_from("<HHIHH", b, 40)
            self.label_bpb = b[71:82].decode("cp437", "replace").rstrip()
            self.volume_id = struct.unpack_from("<I", b, 67)[0]
            self.flag_byte = b[65]
        else:
            self.root_cluster = 0
            self.fsinfo_sector = 0
            self.label_bpb = b[43:54].decode("cp437", "replace").rstrip()
            self.volume_id = struct.unpack_from("<I", b, 39)[0]
            self.flag_byte = b[37]
        self.label = self.label_bpb
        self._fats = None
        self._file = None
        self.part = None

    # -- low level
    def sector(self, n):
        return self.win.read(n * self.bps, self.bps)

    def cluster_off(self, c):
        return (self.data_start + (c - 2) * self.spc) * self.bps

    def fats(self):
        if self._fats is None:
            self._fats = [self.win.read((self.reserved + i * self.fatsz) * self.bps, self.fatsz * self.bps) for i in range(self.nfats)]
        return self._fats

    def fat_get(self, c, copy=0):
        fat = self.fats()[copy]
        if self.bits == 32:
            return struct.unpack_from("<I", fat, c * 4)[0] & 0x0FFFFFFF
        return struct.unpack_from("<H", fat, c * 2)[0]

    @property
    def eoc_min(self):
        return 0x0FFFFFF8 if self.bits == 32 else 0xFFF8

    @property
    def bad(self):
        return 0x0FFFFFF7 if self.bits == 32 else 0xFFF7

    def _reserved_value(self, v):
        return v == 1 or (self.bits == 32 and 0x0FFFFFF0 <= v < 0x0FFFFFF7) or (self.bits == 16 and 0xFFF0 <= v < 0xFFF7)

    def read_clusters(self, clusters):
        return b"".join(self.win.read(self.cluster_off(c), self.cluster_bytes) for c in clusters)

    def chain(self, first):
        """Clusters of a chain; raises MkdiskError on loops, bad links or out-of-range values."""
        out, seen = [], set()
        c = first
        while True:
            if c < 2 or c >= self.nclusters + 2:
                raise MkdiskError(f"cluster {c} out of range")
            if c in seen:
                raise MkdiskError(f"loop in cluster chain at {c}")
            seen.add(c)
            out.append(c)
            nxt = self.fat_get(c)
            if nxt >= self.eoc_min:
                return out
            if nxt == 0 or nxt == self.bad or self._reserved_value(nxt):
                raise MkdiskError(f"chain from {first} runs into {nxt:#x} after cluster {c}")
            c = nxt

    # -- state flags
    def clean_flags(self):
        """-> (clean_shutdown, no_hard_errors, boot_flag_dirty)"""
        f1 = self.fat_get(1)
        if self.bits == 32:
            return bool(f1 & 0x08000000), bool(f1 & 0x04000000), bool(self.flag_byte & 1)
        return bool(f1 & 0x8000), bool(f1 & 0x4000), bool(self.flag_byte & 1)

    @property
    def dirty(self):
        clean, _, boot_dirty = self.clean_flags()
        return (not clean) or boot_dirty

    def fsinfo(self):
        if self.bits != 32 or self.fsinfo_sector in (0, 0xFFFF) or self.fsinfo_sector >= self.reserved:
            return None
        b = self.sector(self.fsinfo_sector)
        lead = struct.unpack_from("<I", b, 0)[0]
        sig, free, nxt = struct.unpack_from("<III", b, 484)
        trail = struct.unpack_from("<I", b, 508)[0]
        return dict(lead=lead, sig=sig, free=free, next=nxt, trail=trail)

    # -- directories
    def root_bytes(self):
        if self.bits == 16:
            return self.win.read((self.reserved + self.nfats * self.fatsz) * self.bps, self.root_entries * 32)
        return self.read_clusters(self.chain(self.root_cluster))

    def parse_dir(self, data, path, rep, is_root):
        """Parses directory bytes into [Ent]; structural problems go into `rep` when one is given."""
        ents = []
        lfn = None
        ended = False
        garbage_reported = False
        labels = 0

        def orphan(slot):
            if rep:
                rep.err(f"{path}: LFN fragment at slot {slot} is not followed by a matching entry")

        for idx in range(len(data) // 32):
            e = data[idx * 32:idx * 32 + 32]
            first = e[0]
            if ended:
                if any(e) and rep and not garbage_reported:
                    rep.err(f"{path}: non-zero slot {idx} after the end-of-directory marker")
                    garbage_reported = True
                continue
            if first == 0x00:
                ended = True
                if lfn:
                    orphan(lfn["slot"])
                lfn = None
                continue
            attr = e[11]
            if first == 0xE5:
                if lfn:
                    orphan(lfn["slot"])
                lfn = None
                continue
            if attr & 0x3F == ATTR_LFN:
                seq = first
                if seq & 0x40:
                    if lfn:
                        orphan(lfn["slot"])
                    cnt = seq & 0x3F
                    if cnt == 0 or cnt > 20:
                        if rep:
                            rep.err(f"{path}: bad LFN sequence number {seq:#x} at slot {idx}")
                        lfn = None
                        continue
                    lfn = dict(slot=idx, expect=cnt, count=cnt, csum=e[13], parts={})
                elif lfn is None or (seq & 0x3F) != lfn["expect"] or e[13] != lfn["csum"]:
                    if rep:
                        rep.err(f"{path}: out-of-order or orphan LFN entry (seq {seq:#x}) at slot {idx}")
                    lfn = None
                    continue
                if e[12] != 0 and rep:
                    rep.warn(f"{path}: LFN entry at slot {idx} has a non-zero type byte")
                if (e[26] | e[27]) != 0 and rep:
                    rep.err(f"{path}: LFN entry at slot {idx} has a non-zero first-cluster field")
                lfn["parts"][lfn["expect"]] = (struct.unpack_from("<5H", e, 1) + struct.unpack_from("<6H", e, 14)
                                               + struct.unpack_from("<2H", e, 28))
                lfn["expect"] -= 1
                continue
            ent = Ent()
            ent.attr, ent.slot, ent.sfn, ent.ntres = attr, idx, bytes(e[0:11]), e[12]
            base = bytearray(e[0:8])
            if base[0] == 0x05:
                base[0] = 0xE5
            b8 = bytes(base).rstrip(b" ").decode("cp437", "replace")
            x3 = bytes(e[8:11]).rstrip(b" ").decode("cp437", "replace")
            if ent.ntres & 0x08:
                b8 = b8.lower()
            if ent.ntres & 0x10:
                x3 = x3.lower()
            ent.short = b8 + ("." + x3 if x3 else "")
            ent.name = ent.short
            ent.has_lfn = False
            ent.nslots = 1
            if lfn is not None:
                if lfn["expect"] == 0 and lfn["csum"] == lfn_checksum(ent.sfn):
                    units = []
                    for s in range(1, lfn["count"] + 1):
                        units += lfn["parts"][s]
                    chars, term = [], False
                    for u in units:
                        if term:
                            if u != 0xFFFF and rep:
                                rep.warn(f"{path}: LFN padding of {ent.short} is not 0xFFFF")
                                break
                            continue
                        if u == 0:
                            term = True
                            continue
                        chars.append(u)
                    ent.name = b"".join(struct.pack("<H", u) for u in chars).decode("utf-16-le", "replace")
                    ent.has_lfn = True
                    ent.nslots = 1 + lfn["count"]
                elif rep:
                    rep.err(f"{path}: LFN entries before {ent.short!r} do not match it (checksum or count)")
                lfn = None
            ent.first = e[26] | (e[27] << 8) | (((e[20] | (e[21] << 8)) << 16) if self.bits == 32 else 0)
            ent.size = struct.unpack_from("<I", e, 28)[0]
            ent.is_dir = bool(attr & ATTR_DIR)
            ent.ctime = from_dos(struct.unpack_from("<H", e, 16)[0], struct.unpack_from("<H", e, 14)[0], e[13])
            ent.mtime = from_dos(struct.unpack_from("<H", e, 24)[0], struct.unpack_from("<H", e, 22)[0])
            ent.atime = from_dos(struct.unpack_from("<H", e, 18)[0], 0)
            ent.path = path.rstrip("/") + "/" + ent.name
            if rep:
                self._check_short_entry(ent, e, path, rep)
            if attr & ATTR_VOLUME and not ent.has_lfn:
                labels += 1
                if is_root and labels == 1:
                    self.label = bytes(e[0:11]).decode("cp437", "replace").rstrip()
                if not is_root and rep:
                    rep.err(f"{path}: volume label entry outside the root directory")
            ents.append(ent)
        if lfn:
            orphan(lfn["slot"])
        return ents

    def _check_short_entry(self, ent, e, path, rep):
        if ent.attr & ATTR_VOLUME:
            return
        label = f"{path.rstrip('/')}/{ent.name}"
        n11 = ent.sfn
        if n11[0] == 0x20:
            rep.err(f"{label}: short name starts with a space")
        for ch in n11:
            if ch < 0x20 and not (ch == 0x05 and n11[0] == 0x05):
                rep.err(f"{label}: control character in the short name")
                break
            if ch in b'"*+,/:;<=>?[\\]|' or ch == 0x7F:
                rep.err(f"{label}: illegal character {chr(ch)!r} in the short name")
                break
        if any(97 <= ch <= 122 for ch in n11):
            rep.warn(f"{label}: lower-case letters in the short name (should use the NT case flags)")
        if ent.ntres & ~0x18:
            rep.warn(f"{label}: unknown NT reserved bits {ent.ntres:#x}")
        if ent.attr & 0xC0:
            rep.warn(f"{label}: reserved attribute bits {ent.attr:#x}")
        for what, v, raw in (("creation", ent.ctime, struct.unpack_from("<H", e, 16)[0]),
                             ("modification", ent.mtime, struct.unpack_from("<H", e, 24)[0])):
            if v is None and raw != 0:
                rep.err(f"{label}: invalid {what} date")

    def _check_dots(self, ents, path, rep, parent_first, self_first):
        if len(ents) < 2 or ents[0].sfn != b".          " or ents[1].sfn != b"..         ":
            rep.err(f"{path}: missing '.' / '..' entries")
            return
        if ents[0].first != self_first:
            rep.err(f"{path}: '.' points to cluster {ents[0].first}, expected {self_first}")
        if ents[1].first != parent_first:
            rep.err(f"{path}: '..' points to cluster {ents[1].first}, expected {parent_first}")
        if not ents[0].is_dir or not ents[1].is_dir:
            rep.err(f"{path}: '.'/'..' without the directory attribute")

    def _trace(self, first, owner, what, rep):
        """Follows a chain, claims its clusters in `owner`, reports problems. Returns the cluster list (possibly partial)."""
        chain, seen = [], set()
        c = first
        while True:
            if c < 2 or c >= self.nclusters + 2:
                if rep:
                    rep.err(f"{what}: cluster {c} is out of range")
                return chain
            if c in seen:
                if rep:
                    rep.err(f"{what}: loop in the cluster chain at {c}")
                return chain
            if c in owner:
                if rep:
                    rep.err(f"{what}: cluster {c} is cross-linked with {owner[c]}")
                return chain
            owner[c] = what
            seen.add(c)
            chain.append(c)
            nxt = self.fat_get(c)
            if nxt >= self.eoc_min:
                return chain
            if nxt == 0:
                if rep:
                    rep.err(f"{what}: chain runs into a free cluster after {c}")
                return chain
            if nxt == self.bad:
                if rep:
                    rep.err(f"{what}: chain runs into a bad cluster after {c}")
                return chain
            if self._reserved_value(nxt):
                if rep:
                    rep.err(f"{what}: reserved FAT value {nxt:#x} after cluster {c}")
                return chain
            c = nxt

    def walk(self, rep=None):
        """Walks every directory. Returns ({dir path: [Ent]}, {cluster: owner path}); problems are reported into rep."""
        owner, tree = {}, {}
        if self.bits == 32:
            root_chain = self._trace(self.root_cluster, owner, "/", rep)
            data = self.read_clusters(root_chain)
        else:
            data = self.root_bytes()
        stack = [("/", data, True, None, self.root_cluster if self.bits == 32 else 0)]
        while stack:
            path, data, is_root, parent_first, self_first = stack.pop()
            ents = self.parse_dir(data, path, rep, is_root)
            if not is_root:
                if rep:
                    self._check_dots(ents, path, rep, parent_first, self_first)
                ents = [e for e in ents if e.sfn not in (b".          ", b"..         ")]
            ents = [e for e in ents if not (e.attr & ATTR_VOLUME and not e.has_lfn)]
            tree[path] = ents
            names, sfns = {}, {}
            for e in ents:
                if e.name.lower() in names and rep:
                    rep.err(f"{path}: duplicate name {e.name!r} (slots {names[e.name.lower()]} and {e.slot})")
                names[e.name.lower()] = e.slot
                if e.sfn in sfns and rep:
                    rep.err(f"{path}: duplicate short name {e.short!r}")
                sfns[e.sfn] = e.slot
                what = e.path
                if e.is_dir:
                    if e.size != 0 and rep:
                        rep.err(f"{what}: directory with non-zero size {e.size}")
                    if e.first < 2:
                        if rep:
                            rep.err(f"{what}: directory without a first cluster")
                        continue
                    chain = self._trace(e.first, owner, what, rep)
                    if chain:
                        stack.append((what, self.read_clusters(chain), False, self_first if not is_root else 0, e.first))
                else:
                    if e.first == 0:
                        if e.size and rep:
                            rep.err(f"{what}: size {e.size} but no clusters")
                        continue
                    chain = self._trace(e.first, owner, what, rep)
                    need = (e.size + self.cluster_bytes - 1) // self.cluster_bytes
                    if e.size == 0:
                        if rep:
                            rep.warn(f"{what}: empty file owns {len(chain)} cluster(s)")
                    elif len(chain) != need and rep:
                        rep.err(f"{what}: size {e.size} needs {need} cluster(s) but the chain has {len(chain)}")
        return tree, owner

    def check(self):
        rep = Report(self.name)
        if self.bits == 32 and not self.structural32:
            rep.warn("cluster count says FAT32 but the BPB layout is FAT16-style")
        if self.bits == 16 and self.structural32:
            rep.warn("cluster count says FAT16 but the BPB layout is FAT32-style")
        if self.total * self.bps > self.win.length:
            rep.err(f"volume claims {self.total * self.bps} bytes but the partition holds only {self.win.length}")
        if self.bits == 32:
            if self.fs_version != 0:
                rep.warn(f"FAT32 version {self.fs_version:#x}")
            if self.root_cluster < 2 or self.root_cluster >= self.nclusters + 2:
                rep.err(f"root cluster {self.root_cluster} out of range")
                return rep
            if self.root_entries != 0:
                rep.err("FAT32 volume with a fixed root directory")
        elif self.root_entries == 0:
            rep.err("FAT16 volume without root directory entries")
            return rep
        esize = 4 if self.bits == 32 else 2
        if self.fatsz * self.bps < (self.nclusters + 2) * esize:
            rep.err("FAT too small for the cluster count")
            return rep
        fats = self.fats()
        used = (self.nclusters + 2) * esize
        for i in range(1, self.nfats):
            if fats[i][:used] != fats[0][:used]:
                for c in range(self.nclusters + 2):
                    if self.fat_get(c, 0) != self.fat_get(c, i):
                        rep.err(f"FAT copy {i + 1} differs from copy 1 (first difference at cluster {c}: "
                                f"{self.fat_get(c, 0):#x} vs {self.fat_get(c, i):#x})")
                        break
        if (self.fat_get(0) & 0xFF) != self.media:
            rep.warn(f"FAT[0] media byte {self.fat_get(0) & 0xFF:#x} differs from the BPB media {self.media:#x}")
        clean, no_err, boot_dirty = self.clean_flags()
        rep.stats["clean"] = clean and not boot_dirty
        if not clean:
            rep.warn("volume is marked dirty (FAT[1] clean-shutdown bit is clear)")
        if boot_dirty:
            rep.warn("volume is marked dirty (boot sector flag byte, bit 0)")
        if not no_err:
            rep.warn("volume is flagged with a hard error (FAT[1] bit)")
        tree, owner = self.walk(rep)
        rep.stats.update(files=sum(1 for ents in tree.values() for e in ents if not e.is_dir), dirs=len(tree),
                         label=(self.label or self.label_bpb))
        lost, free, bad = [], 0, 0
        for c in range(2, self.nclusters + 2):
            v = self.fat_get(c)
            if v == 0:
                free += 1
            elif v == self.bad:
                bad += 1
            elif c not in owner:
                lost.append(c)
        if lost:
            rep.err(f"{len(lost)} lost cluster(s) (allocated in the FAT but part of no file), first: {lost[:8]}")
        rep.stats.update(free=free, bad=bad, used=self.nclusters - free - bad, clusters=self.nclusters)
        if self.bits == 32:
            info = self.fsinfo()
            if info is None:
                rep.warn("no FSInfo sector")
            elif info["lead"] != 0x41615252 or info["sig"] != 0x61417272 or info["trail"] != 0xAA550000:
                rep.err("FSInfo signatures are wrong")
            else:
                if info["free"] != 0xFFFFFFFF and info["free"] != free:
                    rep.err(f"FSInfo free count {info['free']} but the FAT has {free} free clusters")
                if info["next"] != 0xFFFFFFFF and not (2 <= info["next"] < self.nclusters + 2):
                    rep.err(f"FSInfo next-free hint {info['next']} is out of range")
            if self.backup_sector not in (0, 0xFFFF) and self.backup_sector < self.reserved:
                main, bak = bytearray(self.boot[:90]), bytearray(self.sector(self.backup_sector)[:90])
                main[65] = bak[65] = 0                  # the dirty flag byte is not mirrored
                if main != bak:
                    rep.warn("backup boot sector differs from the main boot sector")
        return rep

    # -- convenience for tests and the CLI
    def _tree_cache(self):
        if not hasattr(self, "_tree"):
            self._tree, _ = self.walk(None)
        return self._tree

    def find(self, path):
        """Entry for a path (case-insensitive; the 8.3 alias also matches), or None. '/' gives a pseudo entry."""
        parts = [p for p in path.split("/") if p]
        tree = self._tree_cache()
        cur, ent = "/", None
        for i, part in enumerate(parts):
            found = None
            for e in tree.get(cur, []):
                if e.name.lower() == part.lower() or e.short.lower() == part.lower():
                    found = e
                    break
            if found is None or (i + 1 < len(parts) and not found.is_dir):
                return None
            ent, cur = found, found.path
        if ent is None:
            ent = Ent()
            ent.name = ent.short = ent.path = "/"
            ent.is_dir, ent.first, ent.size, ent.attr = True, self.root_cluster, 0, ATTR_DIR
        return ent

    def listdir(self, path="/"):
        e = self.find(path)
        if e is None or not e.is_dir:
            raise MkdiskError(f"not a directory: {path}")
        return list(self._tree_cache().get(e.path, []))

    def read_file(self, path):
        e = self.find(path)
        if e is None or e.is_dir:
            raise MkdiskError(f"not a file: {path}")
        if e.size == 0:
            return b""
        chain = self.chain(e.first)
        need = (e.size + self.cluster_bytes - 1) // self.cluster_bytes
        if len(chain) < need:
            raise MkdiskError(f"{path}: cluster chain too short")
        data = bytearray()
        i = 0
        while i < need:                                    # read contiguous runs in one go
            j = i
            while j + 1 < need and chain[j + 1] == chain[j] + 1:
                j += 1
            data += self.win.read(self.cluster_off(chain[i]), (j - i + 1) * self.cluster_bytes)
            i = j + 1
        return bytes(data[:e.size])

    def snapshot(self, with_data=True):
        """{path: ('d', 0, '') | ('f', size, sha256-hex)} for the whole tree (stored-case paths)."""
        out, tree, stack = {}, self._tree_cache(), ["/"]
        while stack:
            d = stack.pop()
            for e in tree.get(d, []):
                if e.is_dir:
                    out[e.path] = ("d", 0, "")
                    stack.append(e.path)
                else:
                    out[e.path] = ("f", e.size, hashlib.sha256(self.read_file(e.path)).hexdigest() if with_data else "")
        return out

    def close(self):
        if self._file:
            self._file.close()
            self._file = None


# ---------------------------------------------------------------------------------------------------------------------
# image level operations

def open_volume(path, index=None, writable=False):
    """Opens partition `index` (1-based; default: the first FAT volume) of an image as a FatReader (call .close())."""
    f = open(path, "r+b" if writable else "rb")
    try:
        ss, parts = partitions(Window(f))
        for p in parts:
            if index is not None and p["index"] != index:
                continue
            w = Window(f, p["offset"], p["length"], writable)
            if _looks_like_fat(w):
                r = FatReader(w, name=f"{Path(path).name}:{p['index']}")
                r._file, r.part = f, p
                return r
    except Exception:
        f.close()
        raise
    f.close()
    raise MkdiskError(f"no FAT volume found in {path}" + (f" partition {index}" if index else ""))


def set_dirty(path, index=None, dirty=True):
    """Flips the clean-shutdown bit (FAT[1], every FAT copy) and the boot-sector flag byte, like an unclean power loss."""
    r = open_volume(path, index, writable=True)
    try:
        w = r.win
        for i in range(r.nfats):
            base = (r.reserved + i * r.fatsz) * r.bps
            if r.bits == 32:
                v = struct.unpack("<I", w.read(base + 4, 4))[0]
                w.write(base + 4, struct.pack("<I", (v & ~0x08000000) if dirty else (v | 0x08000000)))
            else:
                v = struct.unpack("<H", w.read(base + 2, 2))[0]
                w.write(base + 2, struct.pack("<H", (v & ~0x8000) if dirty else (v | 0x8000)))
        off = 65 if r.bits == 32 else 37
        b = w.read(off, 1)[0]
        w.write(off, bytes([(b | 1) if dirty else (b & ~1)]))
        r._file.flush()
    finally:
        r.close()


def check_image(path, only=None):
    """GPT/MBR + FAT check of a whole image. Returns a Report (ok when there are no errors)."""
    rep = Report(Path(path).name)
    with open(path, "rb") as f:
        win = Window(f)
        ss, parts = partitions(win)
        if win.length >= 2 * ss and win.read(ss, 8) == b"EFI PART":
            check_gpt(win, ss, rep)
        for p in parts:
            if only is not None and p["index"] != only:
                continue
            w = Window(f, p["offset"], p["length"])
            if not _looks_like_fat(w):
                rep.note(f"p{p['index']} ({p['name'] or p['type']}): not a FAT volume, skipped")
                continue
            try:
                r = FatReader(w, name=f"p{p['index']}")
                sub = r.check()
            except MkdiskError as ex:
                rep.err(f"[p{p['index']}] {ex}")
                continue
            rep.extend(sub, prefix=f"[p{p['index']}] ")
            rep.stats[f"p{p['index']}"] = sub.stats
            rep.note(f"p{p['index']} {p['name'] or ''}: FAT{r.bits} label {r.label.strip()!r}, {sub.stats.get('files', 0)} files, "
                     f"{sub.stats.get('dirs', 0)} dirs, {sub.stats.get('free', 0)}/{sub.stats.get('clusters', 0)} clusters free, "
                     f"{'clean' if sub.stats.get('clean') else 'DIRTY'}")
    return rep


def check_gpt(win, ss, rep):
    total = win.length // ss
    mbr = win.read(0, 512)
    if mbr[510:512] != b"\x55\xaa":
        rep.err("GPT: protective MBR has no signature")
    elif mbr[450] != 0xEE:
        rep.err(f"GPT: protective MBR partition type is {mbr[450]:#x}, expected 0xEE")

    def read_header(lba):
        raw = win.read(lba * ss, 92)
        return (struct.unpack(GPT_HDR, raw) if raw[:8] == b"EFI PART" else None), raw

    prim, praw = read_header(1)
    back, braw = read_header(total - 1)
    if prim is None:
        rep.err("GPT: no primary header")
        return
    if back is None:
        rep.err("GPT: no backup header at the last LBA")
    for tag, fields, raw in (("primary", prim, praw), ("backup", back, braw)):
        if fields is None:
            continue
        (_, rev, hsize, hcrc, _, my, alt, first_usable, last_usable, guid, entries_lba, nent, esize, ecrc) = fields
        tmp = bytearray(raw[:hsize])
        struct.pack_into("<I", tmp, 16, 0)
        if crc32(bytes(tmp)) != hcrc:
            rep.err(f"GPT: {tag} header CRC is wrong")
        if rev != 0x00010000:
            rep.warn(f"GPT: {tag} header revision {rev:#x}")
        if my != (1 if tag == "primary" else total - 1) or alt != (total - 1 if tag == "primary" else 1):
            rep.err(f"GPT: {tag} header has my_lba={my} alt_lba={alt}")
        arr = win.read(entries_lba * ss, nent * esize)
        if crc32(arr) != ecrc:
            rep.err(f"GPT: {tag} entry array CRC is wrong")
        if first_usable > last_usable or last_usable >= total:
            rep.err(f"GPT: {tag} usable range {first_usable}..{last_usable} is invalid")
        used = []
        for i in range(nent):
            e = arr[i * esize:(i + 1) * esize]
            if uuid.UUID(bytes_le=e[0:16]).int == 0:
                continue
            first, last = struct.unpack_from("<QQ", e, 32)
            if first < first_usable or last > last_usable or last < first:
                rep.err(f"GPT: {tag} entry {i + 1} range {first}..{last} lies outside {first_usable}..{last_usable}")
            used.append((first, last, i + 1))
        used.sort()
        for (a1, a2, ai), (b1, b2, bi) in zip(used, used[1:]):
            if b1 <= a2:
                rep.err(f"GPT: partitions {ai} and {bi} overlap")
    if prim and back:
        if win.read(prim[10] * ss, prim[11] * prim[12]) != win.read(back[10] * ss, back[11] * back[12]):
            rep.err("GPT: primary and backup entry arrays differ")
        if prim[9] != back[9]:
            rep.err("GPT: disk GUIDs of the primary and backup headers differ")


# ---------------------------------------------------------------------------------------------------------------------
# image building

def _guid(rng):
    return uuid.UUID(int=rng.getrandbits(128), version=4)


def build_image(path, size, parts, sector_size=512, table="gpt", seed=1, ts=DEFAULT_TS):
    """Creates a raw image file.  parts: list of dicts with
         name, kind ('fat32' | 'fat16' | 'raw'), size (bytes or 'rest'), label, populate (host dir), files ({'/p': bytes}),
         esp (bool), cluster (cluster size in bytes), type (GPT type uuid), mbr_type.
       Returns [(part dict, FatBuilder|None)]."""
    rng = random.Random(seed)
    ss = sector_size
    size = (size // ss) * ss
    with open(path, "wb") as f:
        f.truncate(size)
    total = size // ss
    align = max(1, MIB // ss)
    array_sectors = (128 * 128 + ss - 1) // ss
    first_usable = 2 + array_sectors if table == "gpt" else 1
    last_usable = total - 2 - array_sectors if table == "gpt" else total - 1
    cursor = ((first_usable + align - 1) // align) * align
    layout = []
    for p in parts:
        if p["size"] == "rest":
            length = last_usable + 1 - cursor
        else:
            length = (p["size"] // ss // align) * align
        if length <= 0 or cursor + length - 1 > last_usable:
            raise MkdiskError(f"partition {p['name']!r} does not fit in {size >> 20} MiB "
                              f"(partitions so far end at {(cursor * ss) >> 20} MiB)")
        layout.append((p, cursor, cursor + length - 1))
        cursor = ((cursor + length + align - 1) // align) * align
    results, gpt_entries, mbr_entries = [], [], []
    with open(path, "r+b") as f:
        disk = Window(f, writable=True)
        for p, first, last in layout:
            win = Window(f, first * ss, (last - first + 1) * ss, writable=True)
            builder = None
            kind = p.get("kind", "fat32")
            if kind in ("fat32", "fat16"):
                builder = FatBuilder(win, ss, 32 if kind == "fat32" else 16, label=p.get("label", p["name"]),
                                     cluster_bytes=p.get("cluster"), ts=ts, hidden=first, volume_id=rng.getrandbits(32))
                if p.get("populate"):
                    builder.add_tree(p["populate"])
                for fp, data in (p.get("files") or {}).items():
                    builder.add_file(fp, data)
                builder.finish()
            ptype = p.get("type") or (GPT_TYPE_ESP if p.get("esp") else GPT_TYPE_BASIC)
            gpt_entries.append(dict(type=ptype, unique=_guid(rng), first=first, last=last, name=p["name"]))
            mbr_entries.append(dict(mbr_type=p.get("mbr_type") or (0xEF if p.get("esp") else 0x0C if kind == "fat32" else
                                                                   0x0E if kind == "fat16" else 0x83), first=first, last=last))
            results.append((p, builder))
        if table == "gpt":
            gpt_write(disk, ss, total, gpt_entries, _guid(rng))
        else:
            if len(mbr_entries) > 4:
                raise MkdiskError("MBR supports at most 4 partitions")
            mbr_write(disk, ss, mbr_entries)
        f.flush()
    return results


def _fmt(n):
    for unit in ("B", "KiB", "MiB", "GiB"):
        if n < 1024 or unit == "GiB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024


# ---------------------------------------------------------------------------------------------------------------------
# command line

def cmd_build(a):
    ss = a.sector_size
    min_fat32 = 258 * MIB if ss == 4096 else 34 * MIB
    parts = []
    if not a.no_esp:
        parts.append(dict(name="TOUHOUOS-ESP", kind="fat32", size=max(parse_size(a.esp_size), min_fat32),
                          label="TOUHOUOS-ESP", populate=a.esp, esp=True))
    for spec in a.extra or []:
        bits = spec.split(":")
        parts.append(dict(name=bits[0], kind=bits[2] if len(bits) > 2 else "fat32", size=parse_size(bits[1]), label=bits[0]))
    if not a.no_hv:
        hv_size = "rest" if a.hv_size == "rest" else max(parse_size(a.hv_size), min_fat32)
        parts.append(dict(name=a.hv_label, kind="fat16" if a.hv_fat16 else "fat32", size=hv_size, label=a.hv_label,
                          populate=a.hv_dir, cluster=parse_size(a.cluster_size) if a.cluster_size else None))
    size = parse_size(a.size)
    build_image(a.out, size, parts, sector_size=ss, table=a.table, seed=a.seed)
    rep = check_image(a.out)
    print(f"wrote {a.out}: {_fmt(size)}, {ss}-byte sectors, {a.table.upper()}, {len(parts)} partition(s)")
    for n in rep.notes:
        print("  " + n)
    if not rep.ok:
        print(rep.text())
        return 1
    return 0


def _split_target(spec):
    m = re.match(r"^(.*):(\d+)$", spec)
    if m and len(m.group(1)) > 1:
        return m.group(1), int(m.group(2))
    return spec, None


def cmd_check(a):
    img, idx = _split_target(a.check)
    rep = check_image(img, only=idx)
    for n in rep.notes:
        print("note    " + n)
    print(rep.text(False))
    return 0 if rep.ok else 1


def cmd_ls(a):
    img, idx = _split_target(a.ls)
    r = open_volume(img, idx)
    try:
        for e in sorted(r.listdir(a.rest[0] if a.rest else "/"), key=lambda x: x.name.lower()):
            when = time.strftime("%Y-%m-%d %H:%M", time.gmtime(e.mtime)) if e.mtime else "-"
            print(f"{'d' if e.is_dir else '-'} {e.size:10d} {when}  {e.name}   [{e.short}]")
    finally:
        r.close()
    return 0


def cmd_cat(a):
    img, idx = _split_target(a.cat)
    r = open_volume(img, idx)
    try:
        sys.stdout.buffer.write(r.read_file(a.rest[0]))
    finally:
        r.close()
    return 0


def cmd_extract(a):
    img, idx = _split_target(a.extract)
    if len(a.rest) < 2:
        print("usage: --extract IMAGE[:N] PATH DEST", file=sys.stderr)
        return 2
    r = open_volume(img, idx)
    try:
        e = r.find(a.rest[0])
        if e is None:
            print(f"no such path: {a.rest[0]}", file=sys.stderr)
            return 1
        dest = Path(a.rest[1])

        def pull(path, is_dir, out):
            if is_dir:
                out.mkdir(parents=True, exist_ok=True)
                for c in r.listdir(path):
                    pull(c.path, c.is_dir, out / c.name)
            else:
                out.parent.mkdir(parents=True, exist_ok=True)
                out.write_bytes(r.read_file(path))

        pull(e.path, e.is_dir, dest / e.name if (not e.is_dir and dest.is_dir()) else dest)
    finally:
        r.close()
    return 0


def cmd_tree(a):
    img, idx = _split_target(a.tree)
    r = open_volume(img, idx)
    try:
        snap = r.snapshot(with_data=False)
        for p in sorted(snap, key=str.lower):
            kind, size, _ = snap[p]
            print(f"{kind} {size:10d} {p}")
    finally:
        r.close()
    return 0


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except AttributeError:
            pass
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rest", nargs="*", help="image to build; or PATH [DEST] for --ls / --cat / --extract")
    ap.add_argument("--check", metavar="IMAGE", help="fsck an image (GPT + all FAT volumes)")
    ap.add_argument("--ls", metavar="IMAGE[:N]", help="list a directory")
    ap.add_argument("--cat", metavar="IMAGE[:N]", help="print a file")
    ap.add_argument("--extract", metavar="IMAGE[:N]", help="copy a file or tree to the host")
    ap.add_argument("--tree", metavar="IMAGE[:N]", help="print the whole tree")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--size", default="256M", help="total image size (default 256M)")
    ap.add_argument("--sector-size", type=int, default=512, choices=(512, 4096))
    ap.add_argument("--table", choices=("gpt", "mbr"), default="gpt")
    ap.add_argument("--esp", metavar="DIR", default=None, help="populate the EFI System Partition from DIR (e.g. build/esp)")
    ap.add_argument("--no-esp", action="store_true")
    ap.add_argument("--esp-size", default="64M")
    ap.add_argument("--no-hv", action="store_true", help="do not create the HUMANVILLAGE partition")
    ap.add_argument("--hv-size", default="rest", help="size of the HUMANVILLAGE partition or 'rest'")
    ap.add_argument("--hv-dir", default=None, help="populate HUMANVILLAGE from this host directory")
    ap.add_argument("--hv-label", default="HUMANVILLAGE")
    ap.add_argument("--hv-fat16", action="store_true")
    ap.add_argument("--cluster-size", default=None, help="cluster size of the HUMANVILLAGE volume in bytes")
    ap.add_argument("--extra", action="append", metavar="NAME:SIZE[:fat32|fat16|raw]", help="additional partitions")
    ap.add_argument("--seed", type=int, default=1, help="seed for the generated GUIDs / volume ids")
    a = ap.parse_args(argv)
    for flag, fn in (("check", cmd_check), ("ls", cmd_ls), ("cat", cmd_cat), ("extract", cmd_extract), ("tree", cmd_tree)):
        if getattr(a, flag):
            return fn(a)
    if len(a.rest) != 1:
        ap.error("exactly one output image is required")
    a.out = a.rest[0]
    return cmd_build(a)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except MkdiskError as ex:
        print(f"mkdisk: {ex}", file=sys.stderr)
        sys.exit(2)
