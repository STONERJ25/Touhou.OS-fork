#!/usr/bin/env python3
"""An independent NTFS image generator (and a BitLocker wrapper) for the tests of the read-only NTFS driver.

Nothing here shares code with kernel/fs/ntfs*.c or with tools/ntfs-look.py. It is written from the public descriptions of the formats (the libfsntfs
"New Technologies File System (NTFS) format" document, the MS-XCA description of LZNT1, the libbde "BitLocker Drive Encryption (BDE) format" document,
Ferguson's "AES-CBC + Elephant diffuser" paper, NIST SP 800-38C and IEEE 1619), and every field offset is written down next to its use.
`mkntfs` is not needed: the point is that the driver is tested on volumes that something else laid out.

    b = NtfsBuilder(size=32 * MIB)
    b.add_file("/hello.txt", b"hello")                           # small: resident in its MFT record
    b.add_file("/dir/big.bin", data, frag=5)                     # non-resident, five fragments in scrambled order
    b.add_file("/c.txt", text, compress=True)                    # LZNT1, compression units of 16 clusters
    raw = b.build()                                              # the bytes of the volume (what a partition holds)
    b.expected["/hello.txt"]["data"]                             # what a reader must find

    locked, info = bitlocker_wrap(raw, b, method="xts128", protectors=("clear",))     # a BitLocker volume with a clear-key protector

Run it as a script to write an image:  python tests/host/ntfsimg.py out.img [--bitlocker]
This is a test tool. It is slow and simple on purpose, and it may build volumes real Windows would never write; hostile variants are made by the
tests by editing the bytes it produced (the builder says where things are).
"""
import argparse
import functools
import random
import struct
import sys

KIB, MIB = 1 << 10, 1 << 20

# ---- attribute type codes (libfsntfs, "attribute types") ----
A_STD, A_LIST, A_NAME, A_OBJID, A_SEC, A_VOLNAME, A_VOLINFO, A_DATA = 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80
A_IROOT, A_IALLOC, A_BITMAP, A_REPARSE, A_EAINFO, A_EA, A_LOGGED = 0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0x100
A_END = 0xFFFFFFFF

# ---- file attribute flags (the same bits in $STANDARD_INFORMATION and $FILE_NAME) ----
F_READONLY, F_HIDDEN, F_SYSTEM, F_ARCHIVE = 0x1, 0x2, 0x4, 0x20
F_SPARSE, F_REPARSE, F_COMPRESSED, F_ENCRYPTED = 0x200, 0x400, 0x800, 0x4000
F_DUP_INDEX = 0x10000000                        # in $FILE_NAME only: the file has an $I30 index, i.e. it is a directory

# ---- attribute header "data flags" (offset 12) ----
DF_COMPRESSED, DF_ENCRYPTED, DF_SPARSE = 0x0001, 0x4000, 0x8000

# ---- $FILE_NAME name spaces ----
NS_POSIX, NS_WIN32, NS_DOS, NS_WIN32_DOS = 0, 1, 2, 3

REC_INUSE, REC_DIR = 0x0001, 0x0002

EPOCH_DIFF = 11644473600                        # seconds from 1601-01-01 (FILETIME) to 1970-01-01
SYSTEM_NAMES = {0: "$MFT", 1: "$MFTMirr", 2: "$LogFile", 3: "$Volume", 4: "$AttrDef", 5: ".", 6: "$Bitmap", 7: "$Boot", 8: "$BadClus", 9: "$Secure",
                10: "$UpCase", 11: "$Extend"}
FIRST_USER_RECORD = 24


def align8(n):
    return (n + 7) & ~7


def filetime(unix_seconds, extra_100ns=0):
    return (int(unix_seconds) + EPOCH_DIFF) * 10_000_000 + extra_100ns


def utf16(name):
    return name.encode("utf-16-le", "surrogatepass")


_UPCASE_WORDS = None
_UPCASE_BYTES = None


def upcase_table():
    """The $UpCase file: 65536 little-endian 16-bit words, the simple (one code unit to one code unit) upper-case mapping."""
    global _UPCASE_WORDS, _UPCASE_BYTES
    if _UPCASE_WORDS is None:
        out = []
        for c in range(65536):
            if 0xD800 <= c <= 0xDFFF:
                out.append(c)
                continue
            u = chr(c).upper()
            out.append(ord(u) if len(u) == 1 and ord(u) < 0x10000 else c)
        _UPCASE_WORDS = out
        _UPCASE_BYTES = struct.pack("<65536H", *out)
    return _UPCASE_BYTES


@functools.lru_cache(maxsize=None)
def collation_key(name):
    """What a directory index sorts by: the UTF-16 code units of the name, each mapped through $UpCase, compared unit by unit (a prefix sorts first)."""
    upcase_table()
    raw = utf16(name)
    return tuple(_UPCASE_WORDS[u] for u in struct.unpack("<%dH" % (len(raw) // 2), raw))


# ---------------------------------------------------------------------------------------------------------------- runlists

def encode_runs(runs):
    """Mapping pairs. runs = [(length in clusters, lcn or None)] in VCN order (None = a sparse run). Each run is a header byte, a length field and an
    LCN field; the header's low nibble is the byte count of the length field, its high nibble the byte count of the LCN field, which is a signed
    difference to the previous run's LCN (for the first run: to zero). A sparse run has an LCN field of 0 bytes. A zero byte ends the list."""
    out = bytearray()
    prev = 0
    for length, lcn in runs:
        assert length > 0
        nl = 1
        while length >= 1 << (8 * nl):
            nl += 1
        lb = length.to_bytes(nl, "little")
        if lcn is None:
            out.append(nl)
            out += lb
            continue
        delta = lcn - prev
        n = 1
        while not (-(1 << (8 * n - 1)) <= delta < (1 << (8 * n - 1))):
            n += 1
        out.append(nl | (n << 4))
        out += lb + delta.to_bytes(n, "little", signed=True)
        prev = lcn
    out.append(0)
    return bytes(out)


# ---------------------------------------------------------------------------------------------------------------- LZNT1 (a compressor, for the tests)

def lznt1_chunk(chunk):
    """One LZNT1 chunk (MS-XCA 2.5) for up to 4096 bytes. Header (16 bits): bit 15 = compressed, bits 14..12 = 3 (the signature), bits 11..0 = the
    chunk's size, counted with the header, minus 3 (that is: the number of data bytes minus one). The data is groups of one flag byte (LSB first) and
    up to eight tokens: flag 0 = a literal byte, flag 1 = a 2-byte copy token holding (displacement-1) in its high M bits and (length-3) in the low 16-M
    bits, where M is the smallest value >= 4 for which 2**M >= the number of bytes already produced in this chunk. A full 4096-byte chunk that does not
    shrink is stored as it is: header 0x3FFF (compressed bit clear), then 4096 bytes."""
    n = len(chunk)
    body = bytearray()
    where = {}
    pos = 0
    flag_at, nflag = None, 0
    while pos < n:
        if nflag == 0:
            flag_at = len(body)
            body.append(0)
        m = 4
        while (1 << m) < pos:
            m += 1
        best_len = best_disp = 0
        if pos >= 1 and pos + 3 <= n:
            max_len = min((1 << (16 - m)) + 2, n - pos)
            for cand in reversed(where.get(chunk[pos:pos + 3], ())[-32:]):
                disp = pos - cand
                if disp > (1 << m):
                    break
                ln = 0
                while ln < max_len and chunk[cand + ln] == chunk[pos + ln]:      # byte by byte: a match may overlap what it produces
                    ln += 1
                if ln > best_len:
                    best_len, best_disp = ln, disp
        if best_len >= 3:
            body += struct.pack("<H", ((best_disp - 1) << (16 - m)) | (best_len - 3))
            body[flag_at] |= 1 << nflag
            step = best_len
        else:
            body.append(chunk[pos])
            step = 1
        for k in range(step):
            if pos + k + 3 <= n:
                where.setdefault(chunk[pos + k:pos + k + 3], []).append(pos + k)
        pos += step
        nflag = (nflag + 1) & 7
    if n == 4096 and len(body) >= 4096:
        return struct.pack("<H", 0x3FFF) + bytes(chunk)
    return struct.pack("<H", 0xB000 | (len(body) - 1)) + bytes(body)


def lznt1_compress(data):
    return b"".join(lznt1_chunk(data[i:i + 4096]) for i in range(0, len(data), 4096))


# ---------------------------------------------------------------------------------------------------------------- attributes

class Attr:
    def __init__(self, atype, name="", flags=0):
        self.type, self.name, self.flags, self.instance, self.lowest_vcn = atype, name, flags, 0, 0

    def sort_key(self):
        return (self.type, collation_key(self.name), self.lowest_vcn)


class ResidentAttr(Attr):
    """Resident attribute header (libfsntfs): 0 type(4) 4 record length(4) 8 non-resident flag(1) = 0 9 name length(1) 10 name offset(2) 12 data flags(2)
    14 instance(2) 16 value length(4) 20 value offset(2) 22 indexed flag(1) 23 padding(1); the name starts at 0x18, the value at the next 8-byte boundary."""

    def __init__(self, atype, value, name="", flags=0, indexed=0):
        super().__init__(atype, name, flags)
        self.value, self.indexed = bytes(value), indexed

    def build(self):
        nm = utf16(self.name)
        val_off = align8(0x18 + len(nm))
        length = align8(val_off + len(self.value))
        out = bytearray(length)
        struct.pack_into("<IIBBHHHIHBB", out, 0, self.type, length, 0, len(self.name), 0x18, self.flags, self.instance, len(self.value), val_off, self.indexed, 0)
        out[0x18:0x18 + len(nm)] = nm
        out[val_off:val_off + len(self.value)] = self.value
        return bytes(out)


class NonResidentAttr(Attr):
    """Non-resident attribute header: 16 lowest VCN(8) 24 highest VCN(8) 32 mapping pairs offset(2) 34 compression unit, log2 of clusters(1) 35 padding(5)
    40 allocated size(8) 48 data size(8) 56 initialized size(8) [64 compressed size(8): present when the attribute is compressed or sparse (first extent)]."""

    def __init__(self, atype, runs, alloc, size, init, name="", flags=0, cunit=0, lowest_vcn=0, total_alloc=None, runbytes=None):
        super().__init__(atype, name, flags)
        self.runs, self.alloc, self.size, self.init, self.cunit = runs, alloc, size, init, cunit
        self.lowest_vcn = lowest_vcn
        self.highest_vcn = lowest_vcn + sum(r[0] for r in runs) - 1
        self.total_alloc = total_alloc
        self.runbytes = runbytes

    def build(self):
        nm = utf16(self.name)
        has_comp = bool(self.flags & (DF_COMPRESSED | DF_SPARSE)) and self.lowest_vcn == 0
        hsize = 0x48 if has_comp else 0x40
        mp_off = align8(hsize + len(nm))
        runs = self.runbytes if self.runbytes is not None else encode_runs(self.runs)
        length = align8(mp_off + len(runs))
        out = bytearray(length)
        struct.pack_into("<IIBBHHH", out, 0, self.type, length, 1, len(self.name), hsize, self.flags, self.instance)
        struct.pack_into("<QQHB5xQQQ", out, 16, self.lowest_vcn, self.highest_vcn & 0xFFFFFFFFFFFFFFFF, mp_off, self.cunit, self.alloc, self.size, self.init)
        if has_comp:
            struct.pack_into("<Q", out, 0x40, self.total_alloc if self.total_alloc is not None else self.alloc)
        out[hsize:hsize + len(nm)] = nm
        out[mp_off:mp_off + len(runs)] = runs
        return bytes(out)


def std_info(ctime, mtime, rtime, atime, flags, v3=True, sec_id=0x100):
    """$STANDARD_INFORMATION: 0 creation 8 modification 16 MFT-entry modification 24 access (FILETIMEs) 32 file flags(4) 36 maximum versions(4) 40 version(4)
    44 class id(4) [48 owner id(4) 52 security id(4) 56 quota charged(8) 64 update sequence number(8): NTFS 3.0 and later, 72 bytes in all]."""
    v = struct.pack("<QQQQIIII", ctime, mtime, rtime, atime, flags, 0, 0, 0)
    if v3:
        v += struct.pack("<IIQQ", 0, sec_id, 0, 0)
    return v


def file_name_value(parent_ref, name, ns, ctime, mtime, rtime, atime, alloc, size, flags, reparse=0):
    """$FILE_NAME: 0 parent directory reference(8) 8 creation 16 modification 24 MFT modification 32 access (FILETIMEs) 40 allocated size(8) 48 data size(8)
    56 flags(4) 60 reparse tag or EA size(4) 64 name length in characters(1) 65 name space(1) 66 name (UTF-16, no terminator)."""
    nm = utf16(name)
    return struct.pack("<QQQQQQQIIBB", parent_ref, ctime, mtime, rtime, atime, alloc, size, flags, reparse, len(nm) // 2, ns) + nm


def list_entry(atype, name, vcn, ref, inst):
    """$ATTRIBUTE_LIST entry: 0 attribute type(4) 4 entry length(2) 6 name length(1) 7 name offset(1) 8 lowest VCN(8) 16 file reference(8) 24 attribute id(2) 26 name."""
    nm = utf16(name)
    length = align8(26 + len(nm))
    out = bytearray(length)
    struct.pack_into("<IHBBQQH", out, 0, atype, length, len(name), 26, vcn, ref, inst)
    out[26:26 + len(nm)] = nm
    return bytes(out)


# ---------------------------------------------------------------------------------------------------------------- records

def apply_fixups(buf, usa_off, usa_count, usn):
    """The update sequence array: its first word is the sequence number; the following words hold the original last two bytes of each 512-byte
    stride of the structure, which are replaced on disk by the sequence number (so that a torn write is noticed)."""
    struct.pack_into("<H", buf, usa_off, usn)
    for i in range(1, usa_count):
        end = i * 512 - 2
        struct.pack_into("<H", buf, usa_off + 2 * i, struct.unpack_from("<H", buf, end)[0])
        struct.pack_into("<H", buf, end, usn)


class Record:
    """An MFT record under construction (libfsntfs "MFT entry header"): 0 'FILE' 4 update sequence array offset(2) 6 its length in words(2) 8 log sequence
    number(8) 16 sequence number(2) 18 hard link count(2) 20 first attribute offset(2) 22 flags(2) 24 used size(4) 28 allocated size(4) 32 base record
    reference(8) 40 next attribute id(2) [42 padding(2) 44 record number(4): NTFS 3.1; the array then starts at 0x30, else at 0x2A]."""

    def __init__(self, no, seq=1, flags=REC_INUSE, base=0, links=1, old_header=False):
        self.no, self.seq, self.flags, self.base, self.links, self.old_header = no, seq, flags, base, links, old_header
        self.attrs = []
        self.next_instance = 0

    def add(self, attr):
        attr.instance = self.next_instance
        self.next_instance += 1
        self.attrs.append(attr)
        return attr

    def first_attr(self, rec_size):
        return align8((0x2A if self.old_header else 0x30) + 2 * (rec_size // 512 + 1))

    def body_size(self):
        return sum(len(a.build()) for a in self.attrs)

    def build(self, rec_size, usn=1):
        usa_count = rec_size // 512 + 1
        usa_off = 0x2A if self.old_header else 0x30
        first = align8(usa_off + 2 * usa_count)
        buf = bytearray(rec_size)
        pos = first
        for a in sorted(self.attrs, key=Attr.sort_key):
            b = a.build()
            if pos + len(b) + 8 > rec_size:
                raise ValueError(f"MFT record {self.no}: its attributes do not fit ({pos + len(b) + 8} > {rec_size})")
            buf[pos:pos + len(b)] = b
            pos += len(b)
        struct.pack_into("<II", buf, pos, A_END, 0)
        struct.pack_into("<4sHHQHHHHIIQH", buf, 0, b"FILE", usa_off, usa_count, 0, self.seq, self.links, first, self.flags, pos + 8, rec_size, self.base,
                         self.next_instance)
        if not self.old_header:
            struct.pack_into("<HI", buf, 0x2A, 0, self.no)
        apply_fixups(buf, usa_off, usa_count, usn)
        return bytes(buf)


# ---------------------------------------------------------------------------------------------------------------- the builder

class Node:
    def __init__(self, name, parent, is_dir):
        self.names = [(parent, name)] if parent is not None else []       # (directory, name) for every hard link; the first is the primary one
        self.is_dir = is_dir
        self.children = {}
        self.keys = set()                       # collation keys of the names in this directory (names are unique case-insensitively)
        self.data = b""
        self.size = None                        # logical size when different from len(data)
        self.init = None                        # initialized size when smaller than the size
        self.kind = "dir" if is_dir else "file"
        self.no = None
        self.seq = 1
        self.attrs = 0 if is_dir else F_ARCHIVE
        self.compress = self.sparse = self.encrypted = self.nonres = False
        self.frag = 1
        self.ads = {}
        self.times = None                       # (creation, modification, record change, access) in Unix seconds
        self.reparse = None
        self.attrlist = False                   # force an $ATTRIBUTE_LIST and extension records
        self.per_ext = 1                        # attributes per extension record
        self.old_std = self.old_header = False
        self.dos = True                         # False: no DOS twin names for this node; None: a single Win32+DOS name whatever it looks like
        self.large_index = False                # force $INDEX_ALLOCATION for a directory
        self.record = None
        self.fn_alloc = self.fn_size = self.fn_flags = 0
        self.dos_names = {}

    @property
    def parent(self):
        return self.names[0][0]


class NtfsBuilder:
    def __init__(self, size=32 * MIB, cluster=4096, sector=512, record_size=1024, index_size=4096, label="TESTVOL", serial=0x1234ABCD5678EF01,
                 seed=1, version=(3, 1), now=1_700_000_000, dos_names=True, mft_fragments=1, volume_flags=0, cunit_exp=4, tail_clusters=0, hidden=0,
                 trim_zero_chunks=True, entry_slack=0):
        assert size % sector == 0 and cluster % sector == 0 and record_size % 512 == 0 and index_size % 512 == 0 and record_size >= sector
        self.size, self.cluster, self.sector, self.rec_size, self.idx_size = size, cluster, sector, record_size, index_size
        self.label, self.serial, self.version, self.now, self.dos_names = label, serial, version, now, dos_names
        self.volume_flags, self.cunit_exp, self.mft_fragments = volume_flags, cunit_exp, mft_fragments
        self.tail_clusters = tail_clusters                  # clusters at the end that stay free (the BitLocker wrapper keeps its metadata there)
        self.hidden_sectors, self.trim_zero_chunks = hidden, trim_zero_chunks
        self.rnd = random.Random(seed)
        self.spc = cluster // sector
        self.total_sectors = size // sector - 1             # the BPB leaves out the last sector, which holds the backup boot sector
        self.nclusters = self.total_sectors // self.spc
        self.root = Node("", None, True)
        self.root.no, self.root.seq = 5, 5
        self.nodes = []
        self.expected = {}
        self.dos_counter = 0
        self.resident_max = record_size // 2 - 12

    # ------------------------------------------------------------ the tree
    def _walk_to(self, path):
        parts = [p for p in path.split("/") if p]
        node = self.root
        for p in parts[:-1]:
            node = self._child(node, p)
        return node, (parts[-1] if parts else "")

    @staticmethod
    def _child(parent, name):
        key = collation_key(name)
        if key not in parent.keys:
            raise KeyError(name)
        for k, v in parent.children.items():
            if collation_key(k) == key:
                return v
        raise KeyError(name)

    def _new(self, path, is_dir, **kw):
        parent, name = self._walk_to(path)
        assert name and collation_key(name) not in parent.keys, f"{path}: exists"
        n = Node(name, parent, is_dir)
        for k, v in kw.items():
            assert hasattr(n, k), k
            setattr(n, k, v)
        n.no = FIRST_USER_RECORD + len(self.nodes)
        self.nodes.append(n)
        parent.children[name] = n
        parent.keys.add(collation_key(name))
        return n

    def add_dir(self, path, **kw):
        return self._new(path, True, **kw)

    def add_file(self, path, data=b"", **kw):
        n = self._new(path, False, **kw)
        n.data = bytes(data)
        return n

    def add_symlink(self, path, target, relative=True, print_name=None, **kw):
        """Symbolic link reparse point, tag 0xA000000C; data: 0 substitute name offset(2) 2 substitute name size(2) 4 print name offset(2) 6 print name size(2)
        8 flags(4; 1 = relative) 12 the names (UTF-16): the substitute name, then the print name."""
        sub, prn = utf16(target), utf16(print_name if print_name is not None else target)
        n = self._new(path, False, **kw)
        n.kind = "symlink"
        n.reparse = (0xA000000C, struct.pack("<HHHHI", 0, len(sub), len(sub), len(prn), 1 if relative else 0) + sub + prn)
        n.attrs |= F_REPARSE
        return n

    def add_junction(self, path, target, print_name=None, **kw):
        """Mount point (junction), tag 0xA0000003, on an empty directory; data: 0 substitute name offset(2) 2 size(2) 4 print name offset(2) 6 size(2) 8 names."""
        sub, prn = utf16(target), utf16(print_name if print_name is not None else target)
        n = self._new(path, True, **kw)
        n.kind = "junction"
        n.reparse = (0xA0000003, struct.pack("<HHHH", 0, len(sub), len(sub) + 2, len(prn)) + sub + b"\0\0" + prn + b"\0\0")
        n.attrs |= F_REPARSE
        return n

    def add_reparse(self, path, tag, data=b"", **kw):
        """A file with any other reparse tag (cloud placeholder, WOF, ...)."""
        n = self._new(path, False, **kw)
        n.kind = "reparse"
        n.reparse = (tag, bytes(data))
        n.attrs |= F_REPARSE
        return n

    def add_link(self, path, existing):
        """A hard link: one more $FILE_NAME (and index entry) for an existing file record."""
        parent, name = self._walk_to(path)
        node = self._lookup(existing)
        assert collation_key(name) not in parent.keys, f"{path}: exists"
        node.names.append((parent, name))
        parent.children[name] = node
        parent.keys.add(collation_key(name))
        return node

    def _lookup(self, path):
        node = self.root
        for p in [p for p in path.split("/") if p]:
            node = self._child(node, p)
        return node

    # ------------------------------------------------------------ clusters
    def alloc(self, n):
        assert n > 0
        lcn = self.bump
        if lcn + n > self.nclusters - self.tail_clusters:
            raise ValueError("the volume is full")
        self.bump += n
        self.mark(lcn, n)
        return lcn

    def mark(self, lcn, n):
        for c in range(lcn, lcn + n):
            self.bitmap[c >> 3] |= 1 << (c & 7)

    def store(self, lcn, payload):
        self.writes.append((lcn * self.cluster, bytes(payload)))

    # ------------------------------------------------------------ data attributes
    def _fragments(self, nclu, pieces):
        """nclu clusters in `pieces` extents with a free cluster between them. Returned in VCN order, scrambled so that the run list has negative LCN deltas."""
        pieces = max(1, min(pieces, nclu))
        sizes = [nclu // pieces + (1 if i < nclu % pieces else 0) for i in range(pieces)]
        got = []
        for s in sizes:
            got.append((s, self.alloc(s)))
            if pieces > 1:
                self.bump += 1
        if pieces > 2:
            got = got[1::2] + got[0::2]
        elif pieces == 2:
            got.reverse()
        return got

    def _data_attr(self, node, data, name, main):
        size = len(data) if (not main or node.size is None) else node.size
        init = size if (not main or node.init is None) else min(node.init, size)
        compress, sparse = (node.compress, node.sparse) if main else (False, False)
        flags = (DF_COMPRESSED if compress else 0) | (DF_SPARSE if sparse else 0) | (DF_ENCRYPTED if (main and node.encrypted) else 0)
        if not (compress or sparse or node.nonres or node.encrypted or node.frag > 1) and size == len(data) and size <= self.resident_max:
            return ResidentAttr(A_DATA, data, name)
        cs = self.cluster
        content = (data + bytes(max(0, size - len(data))))[:size]
        if init < size:
            content = content[:init] + bytes(size - init)
        if compress or sparse:
            return self._unit_attr(content, size, init, name, flags, compress)
        nclu = (size + cs - 1) // cs
        if nclu == 0:
            return NonResidentAttr(A_DATA, [], 0, size, init, name, flags)
        runs = self._fragments(nclu, node.frag if main else 1)
        payload = content + bytes(nclu * cs - len(content))
        vc = 0
        for ln, lcn in runs:
            self.store(lcn, payload[vc * cs:(vc + ln) * cs])
            vc += ln
        return NonResidentAttr(A_DATA, runs, nclu * cs, size, init, name, flags)

    def _unit_attr(self, content, size, init, name, flags, compress):
        """Compressed or sparse data. A compression unit is 2**cunit clusters. An all-zero unit is one sparse run. A compressed unit that needs fewer clusters
        than the unit is stored as that many real clusters plus a sparse run for the rest; any other unit is stored as it is. A sparse file without
        compression only leaves out the clusters that are entirely zero."""
        cs, cu = self.cluster, self.cunit_exp
        ucl = 1 << cu
        ubytes = ucl * cs
        nunits = (size + ubytes - 1) // ubytes
        runs, real = [], 0
        for u in range(nunits):
            chunk = content[u * ubytes:(u + 1) * ubytes]
            chunk += bytes(ubytes - len(chunk))
            if not any(chunk):
                runs.append((ucl, None))
                continue
            if compress:
                body = chunk.rstrip(b"\0")
                body = body + bytes(-len(body) % 4096) if self.trim_zero_chunks else chunk
                comp = lznt1_compress(body) + b"\0\0"
                need = (len(comp) + cs - 1) // cs
                if need < ucl:
                    lcn = self.alloc(need)
                    self.store(lcn, comp + bytes(need * cs - len(comp)))
                    runs += [(need, lcn), (ucl - need, None)]
                    real += need
                    continue
                lcn = self.alloc(ucl)
                self.store(lcn, chunk)
                runs.append((ucl, lcn))
                real += ucl
                continue
            i = 0
            while i < ucl:
                j = i
                if any(chunk[i * cs:(i + 1) * cs]):
                    while j < ucl and any(chunk[j * cs:(j + 1) * cs]):
                        j += 1
                    lcn = self.alloc(j - i)
                    self.store(lcn, chunk[i * cs:j * cs])
                    runs.append((j - i, lcn))
                    real += j - i
                else:
                    while j < ucl and not any(chunk[j * cs:(j + 1) * cs]):
                        j += 1
                    runs.append((j - i, None))
                i = j
        merged = []
        for ln, lcn in runs:
            if merged and merged[-1][1] is None and lcn is None:
                merged[-1] = (merged[-1][0] + ln, None)
            elif merged and merged[-1][1] is not None and lcn is not None and merged[-1][1] + merged[-1][0] == lcn:
                merged[-1] = (merged[-1][0] + ln, merged[-1][1])
            else:
                merged.append((ln, lcn))
        return NonResidentAttr(A_DATA, merged, nunits * ubytes, size, init, name, flags, cunit=cu, total_alloc=real * cs)

    # ------------------------------------------------------------ names
    @staticmethod
    def _is_83(name):
        base, _, ext = name.partition(".")
        return (name == name.upper() and 0 < len(base) <= 8 and len(ext) <= 3 and "." not in ext
                and all(ord(c) < 128 and c not in ' +,;=[]"*/:<>?\\|' for c in name))

    def _dos_name(self, name):
        self.dos_counter += 1
        base, dot, ext = name.rpartition(".") if "." in name.lstrip(".") else (name, "", "")
        keep = "".join(c for c in base.upper() if c.isalnum() and ord(c) < 128)[:6] or "FILE"
        suffix = "~%d" % self.dos_counter
        extpart = ("." + "".join(c for c in ext.upper() if c.isalnum() and ord(c) < 128)[:3]) if ext else ""
        return keep[:8 - len(suffix)] + suffix + extpart

    def _names_of(self, node):
        """[(directory, name, namespace)] for every $FILE_NAME of a node. A name that is not 8.3 gets a second, DOS-name entry (when dos_names is on)."""
        out = []
        for par, nm in node.names:
            if node.dos is None or self._is_83(nm):
                out.append((par, nm, NS_WIN32_DOS))
            elif self.dos_names and node.dos:
                dn = node.dos_names.setdefault((id(par), nm), self._dos_name(nm))
                out.append((par, nm, NS_WIN32))
                out.append((par, dn, NS_DOS))
            else:
                out.append((par, nm, NS_WIN32))
        return out

    # ------------------------------------------------------------ one node's record
    def _times(self, node):
        t = node.times or (self.now - 86400 * 30, self.now - 86400, self.now - 3600, self.now - 60)
        return tuple(filetime(x, (node.no or 0) * 7 & 0xFFFFF) for x in t)

    def _make_record(self, node):
        rec = Record(node.no, seq=node.seq, flags=REC_INUSE | (REC_DIR if node.is_dir else 0), links=len(node.names), old_header=node.old_header)
        t = self._times(node)
        flags = node.attrs | (F_COMPRESSED if node.compress else 0) | (F_SPARSE if node.sparse else 0) | (F_ENCRYPTED if node.encrypted else 0)
        rec.add(ResidentAttr(A_STD, std_info(*t, flags, v3=not node.old_std)))
        data_attrs = []
        if not node.is_dir:
            data_attrs.append(self._data_attr(node, node.data, "", True))
            for sname, sdata in sorted(node.ads.items()):
                data_attrs.append(self._data_attr(node, sdata, sname, False))
        main = data_attrs[0] if data_attrs else None
        size = alloc = 0
        if isinstance(main, NonResidentAttr):
            size, alloc = main.size, main.alloc
        elif isinstance(main, ResidentAttr):
            size = len(main.value)
        node.fn_flags = flags | (F_DUP_INDEX if node.is_dir else 0)
        node.fn_size, node.fn_alloc = size, alloc
        tag = node.reparse[0] if node.reparse else 0
        for par, nm, ns in self._names_of(node):
            rec.add(ResidentAttr(A_NAME, file_name_value(par.no | (par.seq << 48), nm, ns, *t, alloc, size, node.fn_flags, tag), indexed=1))
        if node.reparse:
            rec.add(ResidentAttr(A_REPARSE, struct.pack("<IHH", node.reparse[0], len(node.reparse[1]), 0) + node.reparse[1]))
        for a in data_attrs:
            rec.add(a)
        if node.is_dir:
            self._dir_index(node, rec)
        node.record = rec
        return rec

    # ------------------------------------------------------------ directory indexes
    @staticmethod
    def _entry(ref, key, sub=None, last=False):
        """Index entry: 0 file reference(8) 8 entry length(2) 10 key length(2) 12 flags(2: 1 = has a sub-node, 2 = last entry) 14 padding(2) 16 key, then the
        sub-node VCN (8 bytes) at the very end when flag 1 is set. Entries are 8-byte aligned."""
        length = align8(16 + len(key)) + (8 if sub is not None else 0)
        out = bytearray(length)
        struct.pack_into("<QHHHH", out, 0, ref, length, len(key), (1 if sub is not None else 0) | (2 if last else 0), 0)
        out[16:16 + len(key)] = key
        if sub is not None:
            struct.pack_into("<Q", out, length - 8, sub)
        return bytes(out)

    def _dir_entries(self, node):
        """Sorted [(file reference, $FILE_NAME value)] of a directory: one per name of every child (a hard-linked file has several)."""
        ents, seen = [], set()
        for child in node.children.values():
            if id(child) in seen:
                continue
            seen.add(id(child))
            t = self._times(child)
            for par, name, ns in self._names_of(child):
                if par is not node:
                    continue
                key = file_name_value(node.no | (node.seq << 48), name, ns, *t, child.fn_alloc, child.fn_size, child.fn_flags,
                                      child.reparse[0] if child.reparse else 0)
                ents.append((collation_key(name), name, child.no | (child.seq << 48), key))
        ents.sort(key=lambda e: (e[0], e[1]))
        return [(e[2], e[3]) for e in ents]

    def _vcn_field(self, block_index):
        """What an index block's number looks like in a sub-node pointer and in the block's own header: in clusters when a block is at least one cluster
        long, else in units of 512 bytes."""
        return block_index * (self.idx_size // (self.cluster if self.idx_size >= self.cluster else 512))

    def _tree(self, items, root_cap, block_cap):
        """A B+tree like NTFS's. items = sorted [(ref, key)]. A node is a list of entries and an end entry (no key). An entry with a sub-node points to the node
        holding the keys that sort before it; the end entry's sub-node holds the keys after the last one. Built bottom up: pack a level into nodes and
        promote the entry between two neighbouring nodes to the level above. Returns (root entries as bytes, blocks), a block being (entry bytes, is a branch)."""
        blocks = []
        level = [(ref, key, None) for ref, key in items]                # (reference, key, child block index or None)
        end_child = None

        def esize(it):
            return len(self._entry(it[0], it[1], None if it[2] is None else 0))

        def endsize(child):
            return len(self._entry(0, b"", None if child is None else 0, True))

        while True:
            if sum(esize(it) for it in level) + endsize(end_child) <= root_cap:
                root = [self._entry(r, k, None if c is None else self._vcn_field(c)) for r, k, c in level]
                root.append(self._entry(0, b"", None if end_child is None else self._vcn_field(end_child), True))
                return root, blocks
            upper, i, n = [], 0, len(level)
            while True:
                group, size = [], endsize(0 if end_child is not None else None)
                while i < n and size + esize(level[i]) <= block_cap:
                    group.append(level[i])
                    size += esize(level[i])
                    i += 1
                if n - i == 1:                      # one entry left: it would become the separator and leave the last node empty, so take one back
                    i -= 1
                    group.pop()
                if not group:
                    raise ValueError("an index block is too small for even one entry")
                last = i >= n
                sep = None if last else level[i]
                child = end_child if last else sep[2]
                ents = [self._entry(r, k, None if c is None else self._vcn_field(c)) for r, k, c in group]
                ents.append(self._entry(0, b"", None if child is None else self._vcn_field(child), True))
                blocks.append((ents, child is not None or any(c is not None for _, _, c in group)))
                if last:
                    end_child = len(blocks) - 1
                    break
                upper.append((sep[0], sep[1], len(blocks) - 1))
                i += 1
            level = upper

    def _dir_index(self, node, rec):
        """$INDEX_ROOT ($I30) in the record; a big directory also gets $INDEX_ALLOCATION and $BITMAP ($I30).
        $INDEX_ROOT value: 0 indexed attribute type(4) = 0x30 4 collation rule(4) = 1 (file names) 8 index block size(4) 12 clusters per index block(1) [3 padding]
        16 an index header: 0 offset of the first entry, from the start of this header(4) 4 index length counting the header(4) 8 allocated size(4) 12 flags(1: 1 = has sub-nodes)."""
        entries = self._dir_entries(node)
        bs = self.idx_size
        cpb = bs // self.cluster if bs >= self.cluster else 256 - (bs.bit_length() - 1)
        head = struct.pack("<IIIB3x", A_NAME, 1, bs, cpb)
        used = rec.first_attr(self.rec_size) + rec.body_size() + 8
        fixed = 0x20 + 32 + 0x50 + 0x38                                 # the $INDEX_ROOT header and root header, room for $INDEX_ALLOCATION and $BITMAP
        root_cap = self.rec_size - used - fixed
        flat = sum(len(self._entry(r, k)) for r, k in entries) + len(self._entry(0, b"", None, True))
        if flat <= root_cap and not node.large_index:
            body = b"".join(self._entry(r, k) for r, k in entries) + self._entry(0, b"", None, True)
            rec.add(ResidentAttr(A_IROOT, head + struct.pack("<IIIB3x", 16, 16 + len(body), 16 + len(body), 0) + body, "$I30"))
            return
        usa_count = bs // 512 + 1
        first_abs = align8(0x28 + 2 * usa_count)                         # the entries start after the update sequence array (0x40 for 4096-byte blocks)
        block_cap = bs - first_abs - 8
        root_ents, blocks = self._tree(entries, 96 if node.large_index else max(root_cap, 24), block_cap)
        body = b"".join(root_ents)
        rec.add(ResidentAttr(A_IROOT, head + struct.pack("<IIIB3x", 16, 16 + len(body), 16 + len(body), 1) + body, "$I30"))
        # INDX block: 0 'INDX' 4 update sequence array offset(2) = 0x28 6 its length in words(2) 8 log sequence number(8) 16 this block's VCN(8) 24 an index header
        # as above (but its 'offset of the first entry' counts from offset 0x18 and its allocated size is the block size minus 0x18); the array sits at 0x28.
        data = bytearray()
        for i, (ents, branch) in enumerate(blocks):
            blk = bytearray(bs)
            body = b"".join(ents)
            struct.pack_into("<4sHHQQ", blk, 0, b"INDX", 0x28, usa_count, 0, self._vcn_field(i))
            struct.pack_into("<IIIB3x", blk, 0x18, first_abs - 0x18, first_abs - 0x18 + len(body), bs - 0x18, 1 if branch else 0)
            assert first_abs + len(body) <= bs - 8, "index block overflow"
            blk[first_abs:first_abs + len(body)] = body
            apply_fixups(blk, 0x28, usa_count, 1 + (i & 0x7FFF))
            data += blk
        nclu = (len(data) + self.cluster - 1) // self.cluster
        lcn = self.alloc(nclu)
        self.store(lcn, bytes(data) + bytes(nclu * self.cluster - len(data)))
        rec.add(NonResidentAttr(A_IALLOC, [(nclu, lcn)], nclu * self.cluster, len(data), len(data), "$I30"))
        bm = bytearray(align8((len(blocks) + 7) // 8))
        for i in range(len(blocks)):
            bm[i >> 3] |= 1 << (i & 7)
        rec.add(ResidentAttr(A_BITMAP, bytes(bm), "$I30"))
        node.index_blocks = len(blocks)

    # ------------------------------------------------------------ extension records
    def _spill(self, rec, node, ext):
        """When a record does not fit (or the node asks for it) the data-like attributes move to extension records, `per_ext` of them to each, and the base
        record gets an $ATTRIBUTE_LIST that says where everything is. The list does not list itself."""
        fits = rec.first_attr(self.rec_size) + rec.body_size() + 8 <= self.rec_size
        if fits and not node.attrlist:
            return
        keep, move, first_name = [], [], True
        for a in rec.attrs:
            if a.type in (A_STD, A_IROOT) or (a.type == A_NAME and first_name):
                keep.append(a)
                first_name = first_name and a.type != A_NAME
            elif a.type in (A_DATA, A_IALLOC, A_BITMAP, A_REPARSE, A_NAME):
                move.append(a)
            else:
                keep.append(a)
        if not move:
            return
        entries = []
        for i in range(0, len(move), max(1, node.per_ext)):
            en = self.next_ext
            self.next_ext += 1
            er = Record(en, seq=1, flags=REC_INUSE, base=rec.no | (rec.seq << 48), links=0)
            for a in move[i:i + max(1, node.per_ext)]:
                er.add(a)
                entries.append((a.type, a.name, a.lowest_vcn, en | (er.seq << 48), a.instance))
            ext[en] = er
        for a in keep:
            entries.append((a.type, a.name, a.lowest_vcn, rec.no | (rec.seq << 48), a.instance))
        rec.attrs = keep
        self._add_list(rec, entries)

    @staticmethod
    def _add_list(rec, entries):
        entries.sort(key=lambda e: (e[0], collation_key(e[1]), e[2], e[4]))
        la = ResidentAttr(A_LIST, b"".join(list_entry(t, nm, v, r, i) for t, nm, v, r, i in entries))
        rec.add(la)

    # ------------------------------------------------------------ system files
    def _sysrec(self, no, flags=REC_INUSE, seq=None):
        rec = Record(no, seq=seq if seq is not None else (1 if no == 0 else no), flags=flags)
        self.records[no] = rec
        return rec

    def _sys_names(self, rec, name, size=0, alloc=0, dir_=False):
        t = tuple(filetime(self.now - 86400 * 400) for _ in range(4))
        fl = F_HIDDEN | F_SYSTEM
        rec.add(ResidentAttr(A_STD, std_info(*t, fl)))
        rec.add(ResidentAttr(A_NAME, file_name_value(5 | (5 << 48), name, NS_WIN32_DOS, *t, alloc, size, fl | (F_DUP_INDEX if dir_ else 0)), indexed=1))

    def _bootsector(self):
        """Boot sector (libfsntfs "volume header"): 0 jump(3) 3 'NTFS    ' 11 bytes per sector(2) 13 sectors per cluster(1) 14..20 FAT fields (0) 21 media(1)
        22 (0) 24 sectors per track(2) 26 heads(2) 28 hidden sectors(4) 32 (0) 36 (0x80 0x00 0x80 0x00) 40 total sectors(8) 48 $MFT cluster(8) 56 $MFTMirr cluster(8)
        64 clusters per file record(signed 1) 68 clusters per index block(signed 1) 72 serial(8) 80 checksum(4) 84 boot code 510 0x55 0xAA. A negative record
        size value n means 2**-n bytes."""
        def enc(nbytes):
            return nbytes // self.cluster if nbytes >= self.cluster else 256 - (nbytes.bit_length() - 1)
        b = bytearray(self.sector)
        b[0:3] = b"\xEB\x52\x90"
        b[3:11] = b"NTFS    "
        struct.pack_into("<HB", b, 11, self.sector, self.spc)
        b[21] = 0xF8
        struct.pack_into("<HHI", b, 24, 63, 255, self.hidden_sectors)
        struct.pack_into("<BBBB", b, 36, 0x80, 0, 0x80, 0)
        struct.pack_into("<QQQ", b, 40, self.total_sectors, self.mft_lcn, self.mftmirr_lcn)
        b[64] = enc(self.rec_size)
        b[68] = enc(self.idx_size)
        struct.pack_into("<Q", b, 72, self.serial)
        code = b"\x33\xc0\x8e\xd0\xbc\x00\x7c\xfb\x68\xc0\x07\x1f\x1e\x68\x66\x00"
        b[84:84 + len(code)] = code
        text = b"\r\nA disk read error occurred\0\r\nNTLDR is missing\0\r\nNTLDR is compressed\0\r\nPress Ctrl+Alt+Del to restart\r\n\0"
        b[0x100:0x100 + len(text)] = text
        b[510:512] = b"\x55\xAA"
        return bytes(b)

    @staticmethod
    def _attrdef():
        """$AttrDef: 160-byte entries (0 name, UTF-16, 128 bytes; 128 type(4) 132 display rule(4) 136 collation rule(4) 140 flags(4) 144 minimum size(8) 152 maximum size(8))."""
        defs = [("$STANDARD_INFORMATION", 0x10, 0x30, 0x48), ("$ATTRIBUTE_LIST", 0x20, 0, -1), ("$FILE_NAME", 0x30, 0x44, 0x242), ("$OBJECT_ID", 0x40, 0, 0x100),
                ("$SECURITY_DESCRIPTOR", 0x50, 0, -1), ("$VOLUME_NAME", 0x60, 2, 0x100), ("$VOLUME_INFORMATION", 0x70, 0xC, 0xC), ("$DATA", 0x80, 0, -1),
                ("$INDEX_ROOT", 0x90, 0, -1), ("$INDEX_ALLOCATION", 0xA0, 0, -1), ("$BITMAP", 0xB0, 0, -1), ("$REPARSE_POINT", 0xC0, 0, 0x4000),
                ("$EA_INFORMATION", 0xD0, 0, 8), ("$EA", 0xE0, 0, 0x10000), ("$LOGGED_UTILITY_STREAM", 0x100, 0, 0x10000)]
        out = bytearray()
        for name, t, mn, mx in defs:
            e = bytearray(160)
            e[:len(utf16(name))] = utf16(name)
            struct.pack_into("<IIIIqq", e, 128, t, 0, 0, 0, mn, mx)
            out += e
        return bytes(out) + bytes(160)

    def _make_root(self):
        """The root directory (record 5): its index also lists the system files ($MFT ... $Extend and '.'), as every NTFS root does."""
        root = self.root
        saved = dict(root.children)
        for no, name in SYSTEM_NAMES.items():
            sn = Node(name, root, no == 11 or no == 5)
            sn.no, sn.seq, sn.dos = no, (1 if no == 0 else no), None
            sn.fn_flags = F_HIDDEN | F_SYSTEM | (F_DUP_INDEX if no in (5, 11) else 0)
            root.children[name] = sn
        rec = Record(5, seq=5, flags=REC_INUSE | REC_DIR)
        t = tuple(filetime(self.now - 86400 * 400) for _ in range(4))
        rec.add(ResidentAttr(A_STD, std_info(*t, F_HIDDEN | F_SYSTEM)))
        rec.add(ResidentAttr(A_NAME, file_name_value(5 | (5 << 48), ".", NS_WIN32_DOS, *t, 0, 0, F_HIDDEN | F_SYSTEM | F_DUP_INDEX), indexed=1))
        self._dir_index(root, rec)
        root.children = saved
        root.record = rec

    # ------------------------------------------------------------ build
    def build(self):
        cs = self.cluster
        self.writes, self.records, self.bump = [], {}, 0
        self.bitmap = bytearray((self.nclusters + 7) // 8)
        n_user = len(self.nodes)
        # Record numbers: 0..23 system files, then the nodes, then extension records. The MFT is reserved generously; its unused tail stays zero.
        self.n_records = FIRST_USER_RECORD + 2 * n_user + 64
        self.next_ext = FIRST_USER_RECORD + n_user
        boot_clusters = max(1, (16 * self.sector + cs - 1) // cs)
        self.alloc(boot_clusters)
        mft_clusters = (self.n_records * self.rec_size + cs - 1) // cs
        self.n_records = mft_clusters * cs // self.rec_size
        self.bump = max(boot_clusters, 4)
        pieces, left = [], mft_clusters
        k = max(1, min(self.mft_fragments, mft_clusters // 8, 5))
        for i in range(k):
            ln = left // (k - i)
            pieces.append((ln, self.alloc(ln)))
            if k > 1:
                self.bump += 2
            left -= ln
        self.mft_pieces = pieces
        self.mft_lcn = pieces[0][1]
        upcase = upcase_table()
        up_lcn = self.alloc(len(upcase) // cs)
        self.store(up_lcn, upcase)
        attrdef = self._attrdef()
        ad_n = (len(attrdef) + cs - 1) // cs
        ad_lcn = self.alloc(ad_n)
        self.store(ad_lcn, attrdef)
        log_n = max(1, 64 * KIB // cs)
        log_lcn = self.alloc(log_n)
        self.store(log_lcn, b"RSTR" + b"\xff" * (log_n * cs - 4))
        mirr_n = max(1, (4 * self.rec_size + cs - 1) // cs)
        self.mftmirr_lcn = self.alloc(mirr_n)
        bm_len = align8((self.nclusters + 7) // 8)
        bm_n = (bm_len + cs - 1) // cs
        bm_lcn = self.alloc(bm_n)
        # user nodes: files first, then directories deepest first (a parent's index copies its children's final sizes), then the root
        for n in self.nodes:
            if not n.is_dir:
                self._make_record(n)
        for n in sorted([x for x in self.nodes if x.is_dir], key=lambda x: -self._depth(x)):
            self._make_record(n)
        self._make_root()
        ext = {}
        for n in self.nodes + [self.root]:
            self._spill(n.record, n, ext)
            self.records[n.no] = n.record
        self.records.update(ext)
        # system files
        mft_size = self.n_records * self.rec_size
        mftbm = bytearray(align8((self.n_records + 7) // 8))
        for no in list(range(FIRST_USER_RECORD)) + list(self.records):
            mftbm[no >> 3] |= 1 << (no & 7)
        t = self._sysrec(0)
        self._sys_names(t, "$MFT", mft_size, mft_size)
        mft_ext = {}
        if len(pieces) > 1:
            # the $MFT's own $DATA beyond its first extent lives in extension records 12, 13, ... named by an $ATTRIBUTE_LIST in record 0
            first = t.add(NonResidentAttr(A_DATA, pieces[:1], mft_size, mft_size, mft_size, ""))
            entries, vc = [], pieces[0][0]
            for j, run in enumerate(pieces[1:]):
                er = Record(12 + j, seq=12 + j, flags=REC_INUSE, base=0 | (1 << 48), links=0)
                a = er.add(NonResidentAttr(A_DATA, [run], mft_size, mft_size, mft_size, "", lowest_vcn=vc))
                entries.append((A_DATA, "", vc, (12 + j) | ((12 + j) << 48), a.instance))
                mft_ext[12 + j] = er
                vc += run[0]
            entries.append((A_DATA, "", 0, 0 | (1 << 48), first.instance))
        else:
            t.add(NonResidentAttr(A_DATA, pieces, mft_size, mft_size, mft_size, ""))
            entries = None
        if len(mftbm) <= 128:
            bmattr = t.add(ResidentAttr(A_BITMAP, bytes(mftbm)))
        else:
            n_bm = (len(mftbm) + cs - 1) // cs
            lcn = self.alloc(n_bm)
            self.store(lcn, bytes(mftbm))
            bmattr = t.add(NonResidentAttr(A_BITMAP, [(n_bm, lcn)], n_bm * cs, len(mftbm), len(mftbm), ""))
        if entries is not None:
            for a in t.attrs:
                if a.type in (A_STD, A_NAME, A_BITMAP):
                    entries.append((a.type, a.name, 0, 0 | (1 << 48), a.instance))
            self._add_list(t, entries)
        m = self._sysrec(1)
        self._sys_names(m, "$MFTMirr", 4 * self.rec_size, mirr_n * cs)
        m.add(NonResidentAttr(A_DATA, [(mirr_n, self.mftmirr_lcn)], mirr_n * cs, 4 * self.rec_size, 4 * self.rec_size, ""))
        r = self._sysrec(2)
        self._sys_names(r, "$LogFile", log_n * cs, log_n * cs)
        r.add(NonResidentAttr(A_DATA, [(log_n, log_lcn)], log_n * cs, log_n * cs, log_n * cs, ""))
        r = self._sysrec(3)
        self._sys_names(r, "$Volume")
        r.add(ResidentAttr(A_VOLNAME, utf16(self.label)))
        r.add(ResidentAttr(A_VOLINFO, struct.pack("<QBBH", 0, self.version[0], self.version[1], self.volume_flags)))
        r = self._sysrec(4)
        self._sys_names(r, "$AttrDef", len(attrdef), ad_n * cs)
        r.add(NonResidentAttr(A_DATA, [(ad_n, ad_lcn)], ad_n * cs, len(attrdef), len(attrdef), ""))
        r = self._sysrec(6)
        self._sys_names(r, "$Bitmap", bm_len, bm_n * cs)
        r.add(NonResidentAttr(A_DATA, [(bm_n, bm_lcn)], bm_n * cs, bm_len, bm_len, ""))
        r = self._sysrec(7)
        self._sys_names(r, "$Boot", boot_clusters * cs, boot_clusters * cs)
        r.add(NonResidentAttr(A_DATA, [(boot_clusters, 0)], boot_clusters * cs, boot_clusters * cs, boot_clusters * cs, ""))
        r = self._sysrec(8)
        self._sys_names(r, "$BadClus")
        r.add(ResidentAttr(A_DATA, b""))
        r = self._sysrec(9)
        self._sys_names(r, "$Secure")
        r.add(ResidentAttr(A_DATA, b"", "$SDS"))
        r = self._sysrec(10)
        self._sys_names(r, "$UpCase", len(upcase), len(upcase))
        r.add(NonResidentAttr(A_DATA, [(len(upcase) // cs, up_lcn)], len(upcase), len(upcase), len(upcase), ""))
        r = self._sysrec(11, REC_INUSE | REC_DIR)
        self._sys_names(r, "$Extend", dir_=True)
        r.add(ResidentAttr(A_IROOT, struct.pack("<IIIB3x", A_NAME, 1, self.idx_size, self.idx_size // cs if self.idx_size >= cs else 256 - (self.idx_size.bit_length() - 1)) +
                           struct.pack("<IIIB3x", 16, 16 + 24, 16 + 24, 0) + self._entry(0, b"", None, True), "$I30"))
        for no in range(12, 24):
            if no in mft_ext:
                self.records[no] = mft_ext[no]
            elif no not in self.records:
                self._sysrec(no, REC_INUSE, seq=no)
        # serialise
        mft = bytearray(mft_clusters * cs)
        for no, rec in self.records.items():
            if no >= self.n_records:
                raise ValueError(f"record {no} does not fit the MFT reservation ({self.n_records} records)")
            mft[no * self.rec_size:(no + 1) * self.rec_size] = rec.build(self.rec_size, usn=1 + (no & 0x7FFF))
        img = bytearray(self.size)
        img[0:self.sector] = self._bootsector()
        img[self.size - self.sector:] = img[0:self.sector]
        vc = 0
        for ln, lcn in pieces:
            self._put(img, lcn * cs, bytes(mft[vc * cs:(vc + ln) * cs]))
            vc += ln
        self._put(img, self.mftmirr_lcn * cs, bytes(mft[:4 * self.rec_size]))
        for off, payload in self.writes:
            self._put(img, off, payload)
        bm = bytearray(bm_len)
        bm[:len(self.bitmap)] = self.bitmap
        for c in range(self.nclusters, bm_len * 8):
            bm[c >> 3] |= 1 << (c & 7)
        self._put(img, bm_lcn * cs, bytes(bm))
        self.mft = bytes(mft)
        self.expected = self._expected()
        self.image = bytes(img)
        return self.image

    @staticmethod
    def _put(img, off, payload):
        assert off + len(payload) <= len(img), (off, len(payload), len(img))
        img[off:off + len(payload)] = payload

    def _depth(self, n):
        d = 0
        while n is not self.root:
            n, d = n.parent, d + 1
        return d

    def _expected(self):
        out = {}

        def walk(n, prefix):
            for name, c in sorted(n.children.items()):
                if c.no is None or c.no < FIRST_USER_RECORD:
                    continue
                path = prefix + "/" + name
                size = len(c.data) if c.size is None else c.size
                data = (c.data + bytes(max(0, size - len(c.data))))[:size]
                if c.init is not None and c.init < size:
                    data = data[:c.init] + bytes(size - c.init)
                t = c.times or (self.now - 86400 * 30, self.now - 86400, self.now - 3600, self.now - 60)
                out[path] = dict(kind=c.kind, data=b"" if c.is_dir else data, size=0 if c.is_dir else size, ads=dict(c.ads), node=c, no=c.no,
                                 encrypted=c.encrypted, reparse=c.reparse, nlink=len(c.names), btime=t[0], mtime=t[1], ctime=t[2], atime=t[3])
                if c.is_dir and c.names[0][0] is n and c.names[0][1] == name:
                    walk(c, path)
        walk(self.root, "")
        return out

    # ------------------------------------------------------------ where things are, for tests that damage an image
    def record_offset(self, no):
        """Byte offset in the image of MFT record `no`."""
        off = no * self.rec_size
        for ln, lcn in self.mft_pieces:
            if off < ln * self.cluster:
                return lcn * self.cluster + off
            off -= ln * self.cluster
        raise ValueError(no)


# ---------------------------------------------------------------------------------------------------------------- BitLocker

BDE_METHODS = {"cbc128-diffuser": 0x8000, "cbc256-diffuser": 0x8001, "cbc128": 0x8002, "cbc256": 0x8003, "xts128": 0x8004, "xts256": 0x8005}
PROTECT = {"clear": 0x0000, "tpm": 0x0100, "startup": 0x0200, "tpm-pin": 0x0500, "recovery": 0x0800, "password": 0x2000}


def _aes_ecb(key, block16):
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    e = Cipher(algorithms.AES(key), modes.ECB()).encryptor()
    return e.update(block16) + e.finalize()


def ccm_seal(key, nonce12, plain):
    """BitLocker's key wrapping: AES-CCM with a 12-byte nonce, a 16-byte tag and no associated data (NIST SP 800-38C). Stored form: tag, then ciphertext."""
    from cryptography.hazmat.primitives.ciphers.aead import AESCCM
    sealed = AESCCM(key, tag_length=16).encrypt(nonce12, plain, None)
    return sealed[-16:] + sealed[:-16]


def _rotl32(x, n):
    return ((x << n) | (x >> (32 - n))) & 0xFFFFFFFF if n else x


def elephant_encrypt(plain, offset, aes_key, tweak_key):
    """AES-CBC + Elephant diffuser, encryption direction (Ferguson 2006, section 4): xor the sector key, diffuser A, diffuser B, AES-CBC.
    The tweak e(s) is the byte offset of the sector (little endian, 8 bytes) and 8 zero bytes; the CBC IV is AES(aes_key, e(s)); the sector key is
    AES(tweak_key, e(s)) followed by AES(tweak_key, e(s) with the last byte set to 128), repeated over the sector. The diffusers work on 32-bit
    little-endian words d[0..n-1] with indices modulo n: A (encryption): for i = n*5-1 down to 0: d[i] -= d[i-2] ^ rotl(d[i-5], Ra[i mod 4]),
    Ra = 9, 0, 13, 0; B (encryption): for i = n*3-1 down to 0: d[i] -= d[i+2] ^ rotl(d[i+5], Rb[i mod 4]), Rb = 0, 10, 0, 25."""
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    e = struct.pack("<Q", offset) + bytes(8)
    skey = _aes_ecb(tweak_key, e) + _aes_ecb(tweak_key, e[:15] + b"\x80")
    x = bytes(p ^ skey[i & 31] for i, p in enumerate(plain))
    n = len(x) // 4
    d = list(struct.unpack("<%dI" % n, x))
    ra, rb = (9, 0, 13, 0), (0, 10, 0, 25)
    for i in range(n * 5 - 1, -1, -1):
        d[i % n] = (d[i % n] - (d[(i - 2) % n] ^ _rotl32(d[(i - 5) % n], ra[i % 4]))) & 0xFFFFFFFF
    for i in range(n * 3 - 1, -1, -1):
        d[i % n] = (d[i % n] - (d[(i + 2) % n] ^ _rotl32(d[(i + 5) % n], rb[i % 4]))) & 0xFFFFFFFF
    iv = _aes_ecb(aes_key, e)
    enc = Cipher(algorithms.AES(aes_key), modes.CBC(iv)).encryptor()
    return enc.update(struct.pack("<%dI" % n, *d)) + enc.finalize()


def bde_encrypt_sector(method, plain, offset, sector_size, fvek):
    """Encrypts one sector of the volume at byte `offset` of the (logical) volume. `fvek` is the key material of the FVEK entry."""
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    m = BDE_METHODS[method] if isinstance(method, str) else method
    if m in (0x8004, 0x8005):                       # XTS: both keys, then the sector number as a 16-byte little-endian tweak
        half = 16 if m == 0x8004 else 32
        enc = Cipher(algorithms.AES(fvek[:2 * half]), modes.XTS((offset // sector_size).to_bytes(16, "little"))).encryptor()
        return enc.update(plain) + enc.finalize()
    klen = 16 if m in (0x8000, 0x8002) else 32
    if m in (0x8002, 0x8003):                       # CBC: IV = AES-ECB(key, offset as 16 bytes little endian)
        iv = _aes_ecb(fvek[:klen], struct.pack("<Q", offset) + bytes(8))
        enc = Cipher(algorithms.AES(fvek[:klen]), modes.CBC(iv)).encryptor()
        return enc.update(plain) + enc.finalize()
    return elephant_encrypt(plain, offset, fvek[:klen], fvek[32:32 + klen])


def bitlocker_wrap(volume, builder, method="xts128", protectors=("clear",), seed=7, header_sectors=16, encrypted_size=None, label="WINDESK C: 2026/10/25",
                   metadata_copies=3, clear_key_method=0x2000, damage_copies=(), entry_align=1, fvek_pad=0, version=2):
    """Turns a plain volume into a BitLocker volume; `builder` must have been made with tail_clusters >= 4 * 64 KiB worth of clusters (that space holds
    the metadata). Returns (bytes, info). Layout (libbde, Windows 7 and later):
      - sector 0 becomes the BitLocker volume header: the NTFS header's BPB with '-FVE-FS-' as the OEM id, a GUID at 160 and the byte offsets of three
        FVE metadata blocks at 176, 184 and 192;
      - the first `header_sectors` sectors of the original volume are stored ENCRYPTED (with the tweak of their original position) at the 'volume header
        offset' of the metadata block header; physical sectors 1..header_sectors-1 read as zero;
      - everything else below the tail is encrypted in place;
      - an FVE metadata block: a 64-byte block header ('-FVE-FS-', size, version 2, encrypted volume size, number of volume header sectors, the three
        block offsets, the volume header offset), the 48-byte metadata header, then entries: a VMK entry per protector, the FVEK entry (AES-CCM encrypted
        with the VMK), a description, a metadata area descriptor. A clear-key VMK entry holds, as nested properties, the clear key itself and the VMK
        encrypted with it."""
    import uuid
    rnd = random.Random(seed)
    size, sector, cs = len(volume), builder.sector, builder.cluster
    tail = builder.tail_clusters * cs
    assert tail >= 4 * 64 * KIB, "make the NtfsBuilder with tail_clusters covering 256 KiB for the metadata"
    area = size - sector - tail                        # the tail clusters hold the relocated header and the metadata blocks
    hdr_copy = area
    md_offsets = [area + 64 * KIB * (i + 1) for i in range(3)]
    mcode = BDE_METHODS[method]
    klen = {0x8004: 32, 0x8005: 64, 0x8000: 64, 0x8001: 64, 0x8002: 32, 0x8003: 32}[mcode]
    fvek = bytes(rnd.getrandbits(8) for _ in range(klen))
    fvek_blob = fvek + bytes(fvek_pad)
    vmk = bytes(rnd.getrandbits(8) for _ in range(32))
    vol_guid = uuid.UUID(int=rnd.getrandbits(128), version=4).bytes_le
    now_ft = filetime(1_700_000_000)
    clear = bytes(rnd.getrandbits(8) for _ in range(32))
    counter = [1]

    def key_blob(key, method_code, material):
        """AES-CCM encrypted key: 0 nonce(12: FILETIME + counter) 12 tag(16) 28 ciphertext of [size(4) version(2) unknown(2) method(4) key]."""
        plain = struct.pack("<IHHI", 12 + len(material), 1, 0, method_code) + material
        counter[0] += 1
        nonce = struct.pack("<QI", now_ft + counter[0], counter[0])
        return nonce + ccm_seal(key, nonce, plain)

    def entry(etype, vtype, data, ver=1):
        size_ = 8 + len(data)
        pad = (-size_ % 8) if entry_align == 8 else 0
        return struct.pack("<HHHH", size_ + pad, etype, vtype, ver) + data + bytes(pad)

    entries = []
    for prot in protectors:
        kid = uuid.UUID(int=rnd.getrandbits(128), version=4).bytes_le
        if prot == "clear":
            nested = entry(0, 1, struct.pack("<I", clear_key_method) + clear) + entry(0, 5, key_blob(clear, 0x2000, vmk))
        else:
            salt = bytes(rnd.getrandbits(8) for _ in range(16))
            nested = entry(0, 3, struct.pack("<I", 0x1000) + salt + entry(0, 5, bytes(rnd.getrandbits(8) for _ in range(72)))) + \
                entry(0, 5, bytes(rnd.getrandbits(8) for _ in range(72)))
        entries.append(entry(2, 8, kid + struct.pack("<Q", now_ft) + struct.pack("<HH", 0, PROTECT[prot]) + nested, 3 if prot == "clear" else 1))
    entries.append(entry(3, 5, key_blob(vmk, mcode, fvek_blob)))
    entries.append(entry(7, 2, utf16(label) + b"\0\0"))
    entries.append(entry(0xF, 0xF, struct.pack("<QQHHIQQIII", hdr_copy, header_sectors * sector, 3, 36, 0, 0, 0, 0, sector, sector)))
    body = b"".join(entries)
    msize = 48 + len(body)
    enc_size = size - sector if encrypted_size is None else encrypted_size
    block_hdr = struct.pack("<8sHHHHQIIQQQQ", b"-FVE-FS-", 4, version, 4, 4, enc_size, 0, header_sectors, md_offsets[0], md_offsets[1], md_offsets[2], hdr_copy)
    md_hdr = struct.pack("<IIII16sIHHQ", msize, 1, 48, msize, vol_guid, 16, mcode, 0, now_ft)
    meta = block_hdr + md_hdr + body
    assert len(block_hdr) == 64 and len(md_hdr) == 48 and len(meta) <= 64 * KIB
    out = bytearray(volume)
    for off in range(0, area, sector):
        if off < enc_size:
            out[off:off + sector] = bde_encrypt_sector(method, volume[off:off + sector], off, sector, fvek)
    for i in range(header_sectors):                  # the relocated header: encrypted as if it still sat at offset 0
        out[hdr_copy + i * sector:hdr_copy + (i + 1) * sector] = bde_encrypt_sector(method, volume[i * sector:(i + 1) * sector], i * sector, sector, fvek)
    for i in range(1, header_sectors):
        out[i * sector:(i + 1) * sector] = bytes(sector)
    vh = bytearray(volume[:sector])
    vh[3:11] = b"-FVE-FS-"
    vh[0x54:0xA0] = bytes(0xA0 - 0x54)
    vh[0xA0:0xB0] = vol_guid
    struct.pack_into("<QQQ", vh, 0xB0, *md_offsets)
    vh[0xC8:0x1FE] = bytes(0x1FE - 0xC8)
    out[0:sector] = vh
    for i, o in enumerate(md_offsets[:metadata_copies]):
        out[o:o + len(meta)] = bytes(len(meta)) if i in damage_copies else meta
    info = dict(fvek=fvek, vmk=vmk, clear=clear, md_offsets=md_offsets, hdr_copy=hdr_copy, method=mcode, guid=vol_guid, area=area, meta=meta,
                header_sectors=header_sectors)
    return bytes(out), info


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--bitlocker", action="store_true")
    ap.add_argument("--size-mib", type=int, default=32)
    args = ap.parse_args(argv)
    b = NtfsBuilder(size=args.size_mib * MIB, tail_clusters=80 if args.bitlocker else 0)
    b.add_dir("/Documents")
    b.add_file("/Documents/hello.txt", b"hello from a generated NTFS volume\n")
    b.add_file("/Documents/big.bin", bytes(range(256)) * 400, frag=3)
    raw = b.build()
    if args.bitlocker:
        raw, _ = bitlocker_wrap(raw, b)
    with open(args.out, "wb") as f:
        f.write(raw)
    print(f"wrote {args.out}: {len(raw)} bytes, {len(b.expected)} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
