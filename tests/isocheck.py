"""An independent reader for the TouhouOS ISO (tools/mkiso.py), for the tests: it walks the file the way a firmware, a CD driver and a partition tool
would, from the specifications, and reports everything that does not add up. It imports nothing from tools/ (the writer) and shares no code with it.

Specifications used, with the numbering they use:
  * ECMA-119 (ISO 9660): section 8 volume descriptors (byte positions BP 1..2048 are one-based there, and so are the numbers below), section 9
    directory records, section 6.9 / 9.4 path tables, 7.2 and 7.3 the both-endian numbers;
  * the El Torito Bootable CD-ROM Format Specification 1.0: the boot record volume descriptor, the validation entry and its checksum, the
    initial/default entry, section headers;
  * the UEFI specification: 5.2.3 the protective MBR, 5.3 the GPT header and entries (CRC32 over the header with its own CRC field zeroed), 13.3 the
    EFI System Partition and El Torito for UEFI (platform id 0xEF);
  * the FAT specification for the boot image: the BPB, the cluster-count rule that tells FAT12 from FAT16 from FAT32, root directory, chains.

Use:  iso = IsoImage(path); problems = iso.verify(); data = iso.read("/KERNEL.ELF"); fat = iso.fat_files()
"""
import struct
import uuid
import zlib
from pathlib import Path

BLOCK = 2048
SECTOR = 512
ESP_GUID = uuid.UUID("C12A7328-F81F-11D2-BA4B-00A0C93EC93B")


class Entry:
    def __init__(self, path, extent, size, flags, ident):
        self.path, self.extent, self.size, self.flags, self.ident = path, extent, size, flags, ident

    @property
    def is_dir(self):
        return bool(self.flags & 2)


def bp(d, first, last):
    """Bytes BP first..last (one-based, inclusive) of a 2048-byte descriptor, as ECMA-119 numbers them."""
    return d[first - 1:last]


def u16le(b):
    return struct.unpack("<H", b)[0]


def u16be(b):
    return struct.unpack(">H", b)[0]


def u32le(b):
    return struct.unpack("<I", b)[0]


def u32be(b):
    return struct.unpack(">I", b)[0]


class IsoImage:
    def __init__(self, path_or_bytes):
        self.data = Path(path_or_bytes).read_bytes() if isinstance(path_or_bytes, (str, Path)) else bytes(path_or_bytes)
        self.problems = []
        self.entries = {}                               # path -> Entry
        self.pvd = None
        self.catalog = None
        self.gpt = None
        self.fat = None
        self._parse()

    # ------------------------------------------------------------------ small helpers
    def bad(self, text):
        self.problems.append(text)

    def block(self, n, count=1):
        return self.data[n * BLOCK:(n + count) * BLOCK]

    def both(self, raw, what):
        """A both-endian number (7.2.3 / 7.3.3): the little-endian half first, then the big-endian half; they must agree."""
        n = len(raw) // 2
        le = int.from_bytes(raw[:n], "little")
        be = int.from_bytes(raw[n:], "big")
        if le != be:
            self.bad(f"{what}: the two halves of the both-endian number differ ({le} and {be})")
        return le

    # ------------------------------------------------------------------ the volume descriptors
    def _parse(self):
        d = self.data
        if len(d) % BLOCK:
            self.bad(f"the file is {len(d)} bytes: not a whole number of 2048-byte blocks")
        if len(d) < 19 * BLOCK:
            self.bad("the file is too short to hold the system area and the descriptors")
            return
        self.vds = []
        lb = 16
        while lb < 16 + 32:
            sec = self.block(lb)
            if sec[1:6] != b"CD001" or sec[6] != 1:
                self.bad(f"block {lb}: not a volume descriptor (standard identifier {sec[1:6]!r}, version {sec[6]})")
                break
            self.vds.append((sec[0], lb))
            if sec[0] == 255:
                break
            lb += 1
        types = [t for t, _ in self.vds]
        if not types or types[-1] != 255:
            self.bad("no volume descriptor set terminator (type 255) within the first 32 descriptors")
        if types[:1] != [1]:
            self.bad(f"the first volume descriptor must be the primary one (type 1), the types are {types}")
        self._pvd()
        self._boot_record()
        if self.pvd:
            self._tree()
            self._path_tables()
        self._catalog()
        self._gpt()
        self._fat_image()

    def _pvd(self):
        lb = next((b for t, b in self.vds if t == 1), None)
        if lb is None:
            return
        s = self.block(lb)
        p = {}
        p["system_id"] = bp(s, 9, 40).decode("ascii", "replace").rstrip()
        p["volume_id"] = bp(s, 41, 72).decode("ascii", "replace").rstrip()
        p["space_blocks"] = self.both(bp(s, 81, 88), "volume space size")
        p["set_size"] = self.both(bp(s, 121, 124), "volume set size")
        p["seq"] = self.both(bp(s, 125, 128), "volume sequence number")
        p["block_size"] = self.both(bp(s, 129, 132), "logical block size")
        p["path_table_size"] = self.both(bp(s, 133, 140), "path table size")
        p["l_path"] = u32le(bp(s, 141, 144))
        p["l_path_opt"] = u32le(bp(s, 145, 148))
        p["m_path"] = u32be(bp(s, 149, 152))
        p["m_path_opt"] = u32be(bp(s, 153, 156))
        p["root_record"] = bp(s, 157, 190)
        p["version"] = bp(s, 882, 882)[0]
        p["copyright_file"] = bp(s, 703, 739).decode("ascii", "replace").rstrip()
        self.pvd = p
        if p["block_size"] != BLOCK:
            self.bad(f"logical block size {p['block_size']}, expected {BLOCK}")
        if p["space_blocks"] * BLOCK != len(self.data):
            self.bad(f"volume space size {p['space_blocks']} blocks = {p['space_blocks'] * BLOCK} bytes, the file is {len(self.data)} bytes")
        if (p["set_size"], p["seq"]) != (1, 1):
            self.bad(f"volume set size {p['set_size']} / sequence number {p['seq']}, expected 1 / 1")
        if p["version"] != 1:
            self.bad(f"file structure version {p['version']}, expected 1")
        if p["volume_id"] != "TOUHOUOS":
            self.bad(f"volume identifier {p['volume_id']!r}, expected 'TOUHOUOS'")
        if p["l_path_opt"] or p["m_path_opt"]:
            self.bad("optional path tables are given; the writer does not make any")
        for key in (bp(s, 814, 830), bp(s, 831, 847), bp(s, 865, 881)):
            if not key[:16].isdigit():
                self.bad(f"a volume date is not sixteen digits: {key!r}")
        for first, last, what in ((9, 40, "system identifier"), (41, 72, "volume identifier"), (191, 318, "volume set identifier")):
            if any(c not in b" ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_!\"%&'()*+,-./:;<=>?" for c in bp(s, first, last)):
                self.bad(f"{what} holds characters outside the a-characters")

    # ------------------------------------------------------------------ directories (9.1)
    def _record(self, buf, pos):
        n = buf[pos]
        if n == 0:
            return None
        rec = buf[pos:pos + n]
        if n < 34 or n % 2:
            self.bad(f"a directory record of {n} bytes (at least 34, and always even)")
        len_fi = rec[32]
        if 33 + len_fi > n:
            self.bad("a file identifier longer than its directory record")
        pad = 1 if len_fi % 2 == 0 else 0
        if n < 33 + len_fi + pad:
            self.bad("the record is shorter than its identifier and the padding byte")
        e = {
            "xattr": rec[1],
            "extent": self.both(rec[2:10], "record extent"),
            "size": self.both(rec[10:18], "record data length"),
            "date": rec[18:25],
            "flags": rec[25],
            "unit": rec[26], "gap": rec[27],
            "seq": self.both(rec[28:32], "record volume sequence number"),
            "ident": bytes(rec[33:33 + len_fi]),
            "len": n,
        }
        if e["unit"] or e["gap"] or e["xattr"]:
            self.bad("interleave or extended attributes are used; the writer makes none")
        if e["seq"] != 1:
            self.bad("record volume sequence number is not 1")
        y, mo, da, h, mi, s, off = e["date"]
        if not (1 <= mo <= 12 and 1 <= da <= 31 and h < 24 and mi < 60 and s < 61):
            self.bad(f"recording date and time nonsense: {tuple(e['date'])}")
        return e

    def _read_dir(self, path, extent, size, parent_extent, parent_size, is_root=False):
        if size % BLOCK or size == 0:
            self.bad(f"{path}: directory length {size} is not a whole number of blocks")
            return
        if (extent + size // BLOCK) * BLOCK > len(self.data):
            self.bad(f"{path}: the directory extends past the end of the volume")
            return
        buf = self.data[extent * BLOCK:extent * BLOCK + size]
        pos, recs = 0, []
        while pos < size:
            if buf[pos] == 0:
                pos = (pos // BLOCK + 1) * BLOCK
                continue
            e = self._record(buf, pos)
            if (pos % BLOCK) + e["len"] > BLOCK:
                self.bad(f"{path}: a record crosses a block boundary")
            recs.append(e)
            pos += e["len"]
        if len(recs) < 2 or recs[0]["ident"] != b"\x00" or recs[1]["ident"] != b"\x01":
            self.bad(f"{path}: the first two records must be '.' and '..'")
            return
        if (recs[0]["extent"], recs[0]["size"]) != (extent, size):
            self.bad(f"{path}: the '.' record does not point at the directory itself")
        if (recs[1]["extent"], recs[1]["size"]) != (parent_extent, parent_size):
            self.bad(f"{path}: the '..' record does not point at the parent")
        names = []
        for e in recs[2:]:
            ident = e["ident"].decode("ascii", "replace")
            is_dir = bool(e["flags"] & 2)
            if is_dir:
                name = ident
                if not name or not all(c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_" for c in name) or len(name) > 31:
                    self.bad(f"{path}: directory identifier {ident!r} is not a d-string of at most 31 characters")
            else:
                if not ident.endswith(";1"):
                    self.bad(f"{path}: file identifier {ident!r} lacks the version number ';1'")
                stem = ident[:-2]
                base, dot, ext = stem.partition(".")
                if not dot or len(base) + len(ext) > 30 or not all(c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_" for c in base + ext):
                    self.bad(f"{path}: file identifier {ident!r} is not a level 2 name")
                name = stem[:-1] if stem.endswith(".") else stem
            names.append((self._order(ident), name))
            child = path.rstrip("/") + "/" + name
            if e["flags"] & ~2:
                self.bad(f"{child}: file flags {e['flags']:#x}: only the directory bit is expected")
            if e["extent"] * BLOCK + (0 if is_dir else e["size"]) > len(self.data) or (e["extent"] + (e["size"] // BLOCK if is_dir else 0)) * BLOCK > len(self.data):
                self.bad(f"{child}: its extent lies outside the volume")
            if child in self.entries:
                self.bad(f"{child}: listed twice")
            self.entries[child] = Entry(child, e["extent"], e["size"], e["flags"], ident)
            if is_dir:
                self._read_dir(child, e["extent"], e["size"], extent, size)
        if [k for k, _ in names] != sorted(k for k, _ in names):
            self.bad(f"{path}: the records are not in order of increasing identifier (9.3)")

    @staticmethod
    def _order(ident):
        stem = ident.split(";")[0]
        base, _, ext = stem.partition(".")
        return (base.ljust(32), ext.ljust(32))

    def _tree(self):
        r = self._record(self.pvd["root_record"], 0)
        if r is None or r["ident"] != b"\x00" or not r["flags"] & 2:
            self.bad("the root directory record in the primary volume descriptor is not a directory with identifier 00h")
            return
        self.root = r
        self.entries["/"] = Entry("/", r["extent"], r["size"], 2, b"\x00")
        self._read_dir("/", r["extent"], r["size"], r["extent"], r["size"], True)

    # ------------------------------------------------------------------ path tables (9.4)
    def _table(self, extent, size, big):
        raw = self.data[extent * BLOCK:extent * BLOCK + size]
        out, pos = [], 0
        u32 = u32be if big else u32le
        u16 = u16be if big else u16le
        while pos < size:
            li = raw[pos]
            if li == 0:
                self.bad("a path table record with an identifier of length 0")
                break
            ext = u32(raw[pos + 2:pos + 6])
            par = u16(raw[pos + 6:pos + 8])
            ident = bytes(raw[pos + 8:pos + 8 + li])
            out.append((ident, ext, par))
            pos += 8 + li + (li & 1)
        if pos != size:
            self.bad(f"the path table records add up to {pos} bytes, the descriptor says {size}")
        return out

    def _path_tables(self):
        p = self.pvd
        l, m = self._table(p["l_path"], p["path_table_size"], False), self._table(p["m_path"], p["path_table_size"], True)
        if l != m:
            self.bad("the little-endian and big-endian path tables do not hold the same directories")
        if not l or l[0][0] != b"\x00" or l[0][2] != 1:
            self.bad("the first path table record must be the root: identifier 00h, parent number 1")
        dirs = {path: e for path, e in self.entries.items() if e.is_dir}
        if len(l) != len(dirs):
            self.bad(f"the path table has {len(l)} directories, the tree has {len(dirs)}")
        for i, (ident, ext, par) in enumerate(l, 1):
            if par < 1 or par > len(l) or (i > 1 and par >= i + 0 and False):
                self.bad(f"path table record {i}: parent number {par} out of range")
            match = [e for e in dirs.values() if e.extent == ext]
            if len(match) != 1:
                self.bad(f"path table record {i}: extent {ext} is not the extent of exactly one directory of the tree")
        # levels: a record's parent comes earlier in the table (6.9.1: ordered by level, then by parent number)
        for i, (ident, ext, par) in enumerate(l, 1):
            if i > 1 and par >= i:
                self.bad(f"path table record {i}: its parent (number {par}) does not come before it")
        self.path_table = l

    # ------------------------------------------------------------------ El Torito
    def _boot_record(self):
        brs = [b for t, b in self.vds if t == 0]
        if len(brs) != 1:
            self.bad(f"expected one boot record volume descriptor, found {len(brs)}")
            self.boot_catalog_block = None
            return
        s = self.block(brs[0])
        if s[7:39].rstrip(b"\0") != b"EL TORITO SPECIFICATION" or any(s[7 + len(b"EL TORITO SPECIFICATION"):39]):
            self.bad(f"boot system identifier is {s[7:39]!r}, expected 'EL TORITO SPECIFICATION' padded with zeros")
        if any(s[39:71]):
            self.bad("the boot identifier field is not zero")
        self.boot_catalog_block = u32le(s[71:75])
        if brs[0] != self.vds[1][1] and False:
            pass

    def _catalog(self):
        self.catalog = None
        if self.boot_catalog_block is None:
            return
        n = self.boot_catalog_block
        if n * BLOCK + BLOCK > len(self.data):
            self.bad("the boot catalog lies outside the volume")
            return
        sec = self.block(n)
        val = sec[:32]
        if val[0] != 1:
            self.bad(f"validation entry: header id {val[0]}, expected 1")
        if sum(struct.unpack("<16H", val)) & 0xFFFF:
            self.bad("validation entry: the 16-bit words do not add up to zero (the checksum is wrong)")
        if val[30:32] != b"\x55\xaa":
            self.bad("validation entry: key bytes are not 55h AAh")
        if any(val[2:4]):
            self.bad("validation entry: reserved bytes are not zero")
        entries, pos, platform = [], 32, val[1]
        while pos + 32 <= BLOCK:
            e = sec[pos:pos + 32]
            if e[0] in (0x88, 0x00) and any(e):
                entries.append(dict(platform=platform, bootable=e[0] == 0x88, media=e[1] & 15, segment=u16le(e[2:4]), system=e[4], count=u16le(e[6:8]), rba=u32le(e[8:12])))
            elif e[0] in (0x90, 0x91):
                platform = e[1]
                if e[0] == 0x91:
                    pass
            elif not any(e):
                break
            else:
                self.bad(f"catalog entry at byte {pos}: unknown indicator {e[0]:#x}")
            pos += 32
        self.catalog = dict(platform=val[1], entries=entries, id=bytes(val[4:28]).rstrip(b"\0"))
        if val[1] != 0xEF:
            self.bad(f"validation entry: platform {val[1]:#x}, expected 0xEF (EFI)")
        boot = [e for e in entries if e["bootable"]]
        if len(boot) != 1:
            self.bad(f"expected exactly one bootable entry in the catalog, found {len(boot)}")
            return
        b = boot[0]
        if b["media"] != 0:
            self.bad(f"the boot entry's media type is {b['media']}: the UEFI image must be no-emulation (0)")
        if b["rba"] * BLOCK + b["count"] * SECTOR > len(self.data):
            self.bad("the boot image (load RBA and sector count) extends past the end of the volume")
        self.boot_lb = b["rba"]
        self.boot_count = b["count"]

    # ------------------------------------------------------------------ the GPT of the hybrid (UEFI 5.2.3, 5.3)
    def _gpt(self):
        d = self.data
        if d[510:512] != b"\x55\xaa":
            self.bad("protective MBR: no 55AA signature at byte 510")
        mbr = d[446:462]
        if mbr[4] != 0xEE:
            self.bad(f"protective MBR: the first partition record has type {mbr[4]:#x}, expected 0xEE")
        start, size = u32le(mbr[8:12]), u32le(mbr[12:16])
        total = len(d) // SECTOR
        if start != 1 or size not in (total - 1, 0xFFFFFFFF):
            self.bad(f"protective MBR: starts at LBA {start}, size {size}; expected LBA 1 and {total - 1} sectors")
        if mbr[0] != 0:
            self.bad("protective MBR: the boot indicator should be 0")
        if any(d[:446]):
            self.bad("the boot code area of the MBR is not zero")
        for other in (462, 478, 494):
            if any(d[other:other + 16]):
                self.bad("protective MBR: a second partition record is used")
        hdr1 = self._gpt_header(1, total)
        hdr2 = self._gpt_header(total - 1, total)
        if not hdr1 or not hdr2:
            return
        if hdr1["alt"] != total - 1 or hdr2["alt"] != 1 or hdr2["my"] != total - 1:
            self.bad("GPT: the primary and backup headers do not point at each other")
        for k in ("first", "last", "guid", "nentries", "esize", "array_crc"):
            if hdr1[k] != hdr2[k]:
                self.bad(f"GPT: primary and backup headers differ in {k}")
        arrays = []
        for h in (hdr1, hdr2):
            nbytes = h["nentries"] * h["esize"]
            if h["entries_lba"] * SECTOR + nbytes > len(d):
                self.bad("GPT: an entry array lies outside the file")
                return
            raw = d[h["entries_lba"] * SECTOR:h["entries_lba"] * SECTOR + nbytes]
            if zlib.crc32(raw) & 0xFFFFFFFF != h["array_crc"]:
                self.bad(f"GPT: the entry array at LBA {h['entries_lba']} does not match its CRC32")
            arrays.append(raw)
        if arrays[0] != arrays[1]:
            self.bad("GPT: the primary and backup entry arrays differ")
        if hdr1["entries_lba"] != 2 or hdr2["entries_lba"] + (hdr2["nentries"] * hdr2["esize"]) // SECTOR != total - 1:
            self.bad("GPT: the entry arrays are not where the specification puts them (2, and just before the backup header)")
        if hdr1["first"] != 2 + hdr1["nentries"] * hdr1["esize"] // SECTOR or hdr1["last"] != total - 2 - hdr1["nentries"] * hdr1["esize"] // SECTOR:
            self.bad(f"GPT: first/last usable LBA {hdr1['first']}/{hdr1['last']} do not follow from the disk size")
        parts = []
        for i in range(hdr1["nentries"]):
            e = arrays[0][i * hdr1["esize"]:(i + 1) * hdr1["esize"]]
            ptype = uuid.UUID(bytes_le=bytes(e[0:16]))
            if ptype.int == 0:
                continue
            first, last, attrs = struct.unpack_from("<QQQ", e, 32)
            parts.append(dict(index=i + 1, type=ptype, unique=uuid.UUID(bytes_le=bytes(e[16:32])), first=first, last=last, attrs=attrs,
                              name=e[56:128].decode("utf-16-le", "replace").split("\0")[0]))
        self.gpt = dict(header=hdr1, backup=hdr2, parts=parts)
        if len(parts) != 1 or parts[0]["type"] != ESP_GUID:
            self.bad(f"GPT: expected exactly one partition, of the EFI System Partition type; found {[(p['index'], str(p['type'])) for p in parts]}")
            return
        p = parts[0]
        if p["first"] < hdr1["first"] or p["last"] > hdr1["last"] or p["last"] < p["first"]:
            self.bad("GPT: the partition lies outside the usable range")
        if p["unique"].int == 0 or hdr1["guid"].int == 0:
            self.bad("GPT: a GUID is all zeros")

    def _gpt_header(self, lba, total):
        raw = self.data[lba * SECTOR:lba * SECTOR + SECTOR]
        if raw[:8] != b"EFI PART":
            self.bad(f"GPT: no header signature at LBA {lba}")
            return None
        rev, size, crc, reserved, my, alt, first, last = struct.unpack_from("<IIIIQQQQ", raw, 8)
        guid = uuid.UUID(bytes_le=bytes(raw[56:72]))
        entries_lba, nentries, esize, array_crc = struct.unpack_from("<QIII", raw, 72)
        if rev != 0x00010000:
            self.bad(f"GPT header at LBA {lba}: revision {rev:#x}")
        if size < 92 or size > SECTOR:
            self.bad(f"GPT header at LBA {lba}: header size {size}")
            return None
        body = bytearray(raw[:size])
        body[16:20] = b"\0\0\0\0"
        if zlib.crc32(bytes(body)) & 0xFFFFFFFF != crc:
            self.bad(f"GPT header at LBA {lba}: the header CRC32 is wrong")
        if reserved:
            self.bad(f"GPT header at LBA {lba}: the reserved field is not zero")
        if my != lba:
            self.bad(f"GPT header at LBA {lba}: MyLBA says {my}")
        if esize != 128 or nentries < 128:
            self.bad(f"GPT header at LBA {lba}: {nentries} entries of {esize} bytes (the specification wants at least 128 of 128 bytes, a power of two times 128)")
        return dict(my=my, alt=alt, first=first, last=last, guid=guid, entries_lba=entries_lba, nentries=nentries, esize=esize, array_crc=array_crc)

    # ------------------------------------------------------------------ the boot image: a FAT volume (FAT specification)
    def _fat_image(self):
        self.fat = None
        if self.catalog is None or not hasattr(self, "boot_lb"):
            return
        start = self.boot_lb * BLOCK
        img = self.data[start:]
        if img[510:512] != b"\x55\xaa":
            self.bad("boot image: no 55AA signature in its first sector")
            return
        bps, spc, reserved, nfats, root_entries, tot16, media, fatsz16 = struct.unpack_from("<HBHBHHBH", img, 11)
        hidden, tot32 = struct.unpack_from("<II", img, 28)
        total = tot16 or tot32
        if bps != SECTOR or spc not in (1, 2, 4, 8, 16, 32, 64, 128) or nfats not in (1, 2) or reserved < 1 or not fatsz16:
            self.bad(f"boot image: not a FAT12/16 boot sector (bytes per sector {bps}, sectors per cluster {spc}, FATs {nfats}, reserved {reserved}, FAT size {fatsz16})")
            return
        root_sectors = (root_entries * 32 + bps - 1) // bps
        data_start = reserved + nfats * fatsz16 + root_sectors
        nclusters = (total - data_start) // spc
        kind = "FAT12" if nclusters < 4085 else "FAT16" if nclusters < 65525 else "FAT32"
        if kind != "FAT16":
            self.bad(f"boot image: {nclusters} clusters make it {kind} by the Microsoft rule; the ISO's image should be FAT16")
            return
        if total * bps > len(img):
            self.bad("boot image: the FAT volume is larger than the room the ISO has for it")
        esp_first = self.gpt["parts"][0]["first"] if self.gpt and self.gpt["parts"] else None
        if esp_first is not None and esp_first * SECTOR != start:
            self.bad(f"the EFI System Partition starts at byte {esp_first * SECTOR}, the El Torito boot image at byte {start}")
        if esp_first is not None and hidden != esp_first:
            self.bad(f"boot image: hidden sectors {hidden}, the partition starts at LBA {esp_first}")
        if self.gpt and self.gpt["parts"]:
            p = self.gpt["parts"][0]
            if (p["last"] - p["first"] + 1) * SECTOR < total * bps:
                self.bad("the EFI System Partition is smaller than the FAT volume inside it")
        if data_start * bps % BLOCK:
            self.bad("boot image: the data area does not start on a 2048-byte boundary")
        self.fat = dict(img=img, bps=bps, spc=spc, reserved=reserved, nfats=nfats, fatsz=fatsz16, root_entries=root_entries, root_sectors=root_sectors, data_start=data_start,
                        nclusters=nclusters, total=total, hidden=hidden, label=bytes(img[43:54]).decode("ascii", "replace").rstrip(), fstype=bytes(img[54:62]).decode("ascii", "replace").rstrip())

    def _fat_entry(self, c):
        f = self.fat
        off = f["reserved"] * f["bps"] + c * 2
        return u16le(f["img"][off:off + 2])

    def _fat_chain(self, first):
        f = self.fat
        chain, c = [], first
        while 2 <= c < 0xFFF8 and len(chain) <= f["nclusters"]:
            chain.append(c)
            c = self._fat_entry(c)
        if c < 0xFFF8:
            self.bad(f"FAT chain from cluster {first} does not end properly (stopped at {c:#x})")
        return chain

    def fat_files(self):
        """{path: (offset of the first byte inside the boot image, size, contiguous?, bytes)} for every file of the boot image, from a walk of its
        directories with the FAT chains followed (long-name records skipped; the short names are what the firmware's driver needs)."""
        out = {}
        if not self.fat:
            return out
        f = self.fat
        img = f["img"]
        cluster_bytes = f["spc"] * f["bps"]

        def entries(raw):
            for i in range(0, len(raw), 32):
                e = raw[i:i + 32]
                if e[0] == 0:
                    return
                if e[0] == 0xE5 or e[11] == 0x0F:
                    continue
                yield e

        def walk(raw, prefix):
            for e in entries(raw):
                if e[11] & 0x08:                                    # the volume label
                    continue
                name = bytes(e[:8]).decode("ascii").rstrip()
                ext = bytes(e[8:11]).decode("ascii").rstrip()
                nt = e[12]
                if nt & 0x08:
                    name = name.lower()
                if nt & 0x10:
                    ext = ext.lower()
                full = name + ("." + ext if ext else "")
                if full in (".", ".."):
                    continue
                first = struct.unpack_from("<H", e, 26)[0]
                size = struct.unpack_from("<I", e, 28)[0]
                if e[11] & 0x10:
                    chain = self._fat_chain(first)
                    sub = b"".join(img[(f["data_start"] + (c - 2) * f["spc"]) * f["bps"]:(f["data_start"] + (c - 2) * f["spc"]) * f["bps"] + cluster_bytes] for c in chain)
                    walk(sub, prefix + full + "/")
                else:
                    chain = self._fat_chain(first) if size else []
                    if len(chain) * cluster_bytes < size or (chain and (len(chain) - 1) * cluster_bytes >= size):
                        self.bad(f"FAT file {prefix + full}: {len(chain)} clusters for {size} bytes")
                    contiguous = all(b == a + 1 for a, b in zip(chain, chain[1:]))
                    off = (f["data_start"] + (chain[0] - 2) * f["spc"]) * f["bps"] if chain else 0
                    body = b"".join(img[(f["data_start"] + (c - 2) * f["spc"]) * f["bps"]:(f["data_start"] + (c - 1) * f["spc"]) * f["bps"]] for c in chain)[:size]
                    out[prefix + full] = (off, size, contiguous, body)

        root_off = (f["reserved"] + f["nfats"] * f["fatsz"]) * f["bps"]
        walk(img[root_off:root_off + f["root_entries"] * 32], "")
        # the two FATs must be identical copies
        if f["nfats"] == 2:
            a = img[f["reserved"] * f["bps"]:(f["reserved"] + f["fatsz"]) * f["bps"]]
            b = img[(f["reserved"] + f["fatsz"]) * f["bps"]:(f["reserved"] + 2 * f["fatsz"]) * f["bps"]]
            if a != b:
                self.bad("boot image: the two copies of the FAT differ")
        return out

    # ------------------------------------------------------------------ reading and the whole check
    def read(self, path):
        e = self.entries[path]
        return self.data[e.extent * BLOCK:e.extent * BLOCK + e.size]

    def verify(self, expect_files=None):
        """All problems found in the structure, plus: every file of the ISO tree that the boot image also holds must be the same bytes at the same place.
        expect_files: {tree path: bytes} the content that was meant to go in (the writer's inputs)."""
        files = self.fat_files()
        shared = {"/KERNEL.ELF": "kernel.elf", "/INITRD.IMG": "initrd.img", "/CMDLINE.TXT": "cmdline.txt", "/EFI/BOOT/BOOTX64.EFI": "EFI/BOOT/BOOTX64.EFI"}
        for tree, inner in shared.items():
            if tree not in self.entries:
                self.bad(f"the tree has no {tree}")
                continue
            if inner not in files:
                self.bad(f"the boot image has no {inner}")
                continue
            off, size, contiguous, body = files[inner]
            e = self.entries[tree]
            if e.size != size:
                self.bad(f"{tree}: {e.size} bytes in the tree, {size} in the boot image")
            if hasattr(self, "boot_lb") and e.extent * BLOCK != self.boot_lb * BLOCK + off:
                self.bad(f"{tree}: the tree's extent is not the bytes of {inner} inside the boot image")
            if self.read(tree) != body:
                self.bad(f"{tree}: the tree and the boot image hold different bytes")
            if not contiguous:
                self.bad(f"{inner} is not stored in one run of clusters")
        if "/ESP.IMG" in self.entries and hasattr(self, "boot_lb") and self.entries["/ESP.IMG"].extent != self.boot_lb:
            self.bad("/ESP.IMG is not the El Torito boot image")
        for need in ("/README.TXT", "/LICENSE", "/THIRD_PARTY_NOTICES.TXT", "/EFI", "/EFI/BOOT"):
            if need not in self.entries:
                self.bad(f"the tree has no {need}")
        for tree, body in (expect_files or {}).items():
            if tree not in self.entries:
                self.bad(f"the tree has no {tree}")
            elif self.read(tree) != body:
                self.bad(f"{tree}: not the bytes that were put in")
        # no two separate files may overlap except where one lies inside the boot image (the shared files) or is the boot image itself
        spans = sorted((e.extent * BLOCK, e.extent * BLOCK + e.size, p) for p, e in self.entries.items() if not e.is_dir and p not in shared and p != "/ESP.IMG")
        for (a0, a1, pa), (b0, b1, pb) in zip(spans, spans[1:]):
            if b0 < a1:
                self.bad(f"{pa} and {pb} overlap")
        if hasattr(self, "boot_lb"):
            boot = (self.boot_lb * BLOCK, self.boot_lb * BLOCK + (self.fat["total"] * self.fat["bps"] if self.fat else 0))
            for a0, a1, p in spans:
                if a0 < boot[1] and a1 > boot[0]:
                    self.bad(f"{p} overlaps the boot image")
        return list(self.problems)
