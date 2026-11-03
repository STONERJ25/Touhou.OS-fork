#!/usr/bin/env python3
"""Read-only look at an NTFS volume image (optionally inside a BitLocker container with a clear-key protector): list a directory, copy a file out, dump an MFT record.
The debugging tool of the NTFS work (docs/NTFS.md); it shares no code with the kernel driver (kernel/fs/ntfs*.c) and none with the test generator (tests/host/ntfsimg.py),
so the three can be compared with each other.

  python tools/ntfs-look.py IMAGE info                         the boot sector's numbers, the label, the MFT, BitLocker's protectors if there are any
  python tools/ntfs-look.py IMAGE ls [PATH]                    a directory (default /) with size, record number, flags and time stamps; -a also shows the system files
  python tools/ntfs-look.py IMAGE tree [PATH]                  everything below PATH, one line per file
  python tools/ntfs-look.py IMAGE cat PATH [--stream NAME]     the unnamed data stream of a file (or an alternate stream) on standard output
  python tools/ntfs-look.py IMAGE rec N                        a human-readable dump of MFT record N (fixups applied, every attribute, the run list)
  python tools/ntfs-look.py IMAGE hexrec N                     the raw record bytes after the fixups, as a hex dump
  python tools/ntfs-look.py IMAGE bde                          only the BitLocker metadata: protectors, encryption method, and whether the clear key unlocks it

IMAGE is a file; `--offset BYTES` starts the volume that far into it (a partition inside a disk image); `--no-bitlocker` stops the tool from unlocking a BitLocker volume.
As a module (importable):  v = Volume(open(path, "rb").read());  v.listdir("/");  v.read_path("/a/b.txt");  v.record(5)
Nothing is ever written. LZNT1 compression, sparse files, attribute lists, fragmented $MFT, B+tree directories, junctions and symlinks are understood; encrypted (EFS) files
are reported as such and not decrypted.
"""
import argparse
import struct
import sys
import time

EPOCH_DIFF = 11644473600


class NtfsError(Exception):
    pass


def filetime_to_unix(ft):
    return ft // 10_000_000 - EPOCH_DIFF


# ---------------------------------------------------------------------------------------------------------------- BitLocker (clear key only)

class Bde:
    """The decrypted view of a BitLocker volume whose VMK is protected by a clear key. Written from the libbde format description."""

    METHODS = {0x8000: "AES-CBC 128 + Elephant diffuser", 0x8001: "AES-CBC 256 + Elephant diffuser", 0x8002: "AES-CBC 128", 0x8003: "AES-CBC 256",
               0x8004: "AES-XTS 128", 0x8005: "AES-XTS 256"}
    PROTECTORS = {0x0000: "clear key", 0x0100: "TPM", 0x0200: "startup key", 0x0500: "TPM + PIN", 0x0600: "TPM + startup key", 0x0800: "recovery password", 0x2000: "password"}

    def __init__(self, raw_read, size):
        self.raw_read, self.size = raw_read, size
        head = raw_read(0, 512)
        if head[3:11] != b"-FVE-FS-":
            raise NtfsError("no -FVE-FS- signature")
        self.sector = struct.unpack_from("<H", head, 11)[0] or 512
        self.offsets = struct.unpack_from("<QQQ", head, 0xB0)
        self.protectors = []
        self.meta = None
        for off in self.offsets:
            blk = raw_read(off, 64 * 1024) if off and off + 4096 <= size else b""
            if blk[:8] != b"-FVE-FS-":
                continue
            try:
                self._parse(blk)
            except (NtfsError, struct.error, IndexError):
                continue
            if self.meta:
                break
        if not self.meta:
            raise NtfsError("no valid FVE metadata block")
        self.unlock()

    def _entries(self, buf, pos, end):
        out = []
        while pos + 8 <= end:
            size, etype, vtype, ver = struct.unpack_from("<HHHH", buf, pos)
            if size < 8 or pos + size > end:
                break
            out.append((etype, vtype, ver, buf[pos + 8:pos + size]))
            pos += size
        return out

    def _parse(self, blk):
        (self.version,) = struct.unpack_from("<H", blk, 10)
        (self.enc_volume_size,) = struct.unpack_from("<Q", blk, 16)
        (self.header_sectors,) = struct.unpack_from("<I", blk, 28)
        (self.header_offset,) = struct.unpack_from("<Q", blk, 56)
        msize, mver, hsize = struct.unpack_from("<III", blk, 0x40)
        if mver != 1 or hsize != 48 or msize < 48 or 0x40 + msize > len(blk):
            raise NtfsError("bad metadata header")
        self.method_header = struct.unpack_from("<H", blk, 0x40 + 36)[0]
        top = self._entries(blk, 0x40 + hsize, 0x40 + msize)
        self.protectors, self.vmk_entries, self.fvek_entry = [], [], None
        for etype, vtype, ver, data in top:
            if etype == 2 and vtype == 8:
                prot = struct.unpack_from("<H", data, 26)[0]
                self.protectors.append(prot)
                self.vmk_entries.append((prot, self._entries(data, 28, len(data))))
            elif etype == 3 and vtype == 5:
                self.fvek_entry = data
        self.meta = True

    @staticmethod
    def _ccm_open(key, blob):
        from cryptography.hazmat.primitives.ciphers.aead import AESCCM
        nonce, tag, ct = blob[:12], blob[12:28], blob[28:]
        return AESCCM(key, tag_length=16).decrypt(nonce, ct + tag, None)

    def unlock(self):
        vmk = None
        for prot, props in self.vmk_entries:
            if prot != 0:
                continue
            key = next((d[4:] for et, vt, _, d in props if vt == 1), None)
            blob = next((d for et, vt, _, d in props if vt == 5), None)
            if key and blob:
                plain = self._ccm_open(key[:32], blob)
                size, = struct.unpack_from("<I", plain, 0)
                vmk = plain[12:size]
                break
        if vmk is None:
            raise NtfsError("no clear-key protector (protectors: %s)" % ", ".join(self.PROTECTORS.get(p, hex(p)) for p in self.protectors))
        plain = self._ccm_open(vmk, self.fvek_entry)
        size, = struct.unpack_from("<I", plain, 0)
        self.method = struct.unpack_from("<I", plain, 8)[0]
        self.fvek = plain[12:size]
        self.method_name = self.METHODS.get(self.method, hex(self.method))

    def _decrypt(self, data, offset):
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        m, ss, fvek = self.method, self.sector, self.fvek
        out = bytearray()
        for i in range(0, len(data), ss):
            sec, off = data[i:i + ss], offset + i
            if m in (0x8004, 0x8005):
                half = 16 if m == 0x8004 else 32
                d = Cipher(algorithms.AES(fvek[:2 * half]), modes.XTS((off // ss).to_bytes(16, "little"))).decryptor()
                out += d.update(sec) + d.finalize()
            elif m in (0x8002, 0x8003):
                key = fvek[:16 if m == 0x8002 else 32]
                e = Cipher(algorithms.AES(key), modes.ECB()).encryptor()
                iv = e.update(struct.pack("<Q", off) + bytes(8))
                d = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor()
                out += d.update(sec) + d.finalize()
            else:
                out += self._elephant(sec, off)
        return bytes(out)

    def _elephant(self, sec, off):
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        kl = 16 if self.method == 0x8000 else 32
        key, tweak = self.fvek[:kl], self.fvek[32:32 + kl]

        def ecb(k, b):
            e = Cipher(algorithms.AES(k), modes.ECB()).encryptor()
            return e.update(b) + e.finalize()
        e = struct.pack("<Q", off) + bytes(8)
        d = Cipher(algorithms.AES(key), modes.CBC(ecb(key, e))).decryptor()
        words = list(struct.unpack("<%dI" % (len(sec) // 4), d.update(sec) + d.finalize()))
        n = len(words)
        rot = lambda x, k: ((x << k) | (x >> (32 - k))) & 0xFFFFFFFF if k else x   # noqa: E731
        for i in range(n * 3):                      # diffuser B, decryption direction
            words[i % n] = (words[i % n] + (words[(i + 2) % n] ^ rot(words[(i + 5) % n], (0, 10, 0, 25)[i % 4]))) & 0xFFFFFFFF
        for i in range(n * 5):                      # diffuser A, decryption direction
            words[i % n] = (words[i % n] + (words[(i - 2) % n] ^ rot(words[(i - 5) % n], (9, 0, 13, 0)[i % 4]))) & 0xFFFFFFFF
        skey = ecb(tweak, e) + ecb(tweak, e[:15] + b"\x80")
        return bytes(b ^ skey[i & 31] for i, b in enumerate(struct.pack("<%dI" % n, *words)))

    def read(self, off, n):
        """Decrypted bytes [off, off+n) of the volume; the first header_sectors sectors come from the relocated copy."""
        ss = self.sector
        first, last = off // ss, (off + n - 1) // ss
        out = bytearray()
        for s in range(first, last + 1):
            pos = s * ss
            if pos < self.header_sectors * ss:
                raw = self.raw_read(self.header_offset + pos, ss)
            else:
                raw = self.raw_read(pos, ss)
            out += self._decrypt(raw, pos) if pos < self.enc_volume_size else raw
        skip = off - first * ss
        return bytes(out[skip:skip + n])


# ---------------------------------------------------------------------------------------------------------------- LZNT1

def lznt1_decompress(buf, limit=1 << 30):
    """MS-XCA 2.5: chunks with a 16-bit header (bit 15 compressed, 14..12 = 3, 11..0 = size-3 counted with the header; 0 ends the data)."""
    out = bytearray()
    pos = 0
    while pos + 2 <= len(buf):
        (hdr,) = struct.unpack_from("<H", buf, pos)
        if hdr == 0:
            break
        if (hdr >> 12) & 7 != 3:
            raise NtfsError("bad LZNT1 chunk header %#x" % hdr)
        csize = (hdr & 0xFFF) + 1
        body = buf[pos + 2:pos + 2 + csize]
        if len(body) < csize:
            raise NtfsError("truncated LZNT1 chunk")
        pos += 2 + csize
        if not hdr & 0x8000:
            out += body
            continue
        start = len(out)
        i = 0
        while i < len(body):
            flags = body[i]
            i += 1
            for bit in range(8):
                if i >= len(body):
                    break
                if not flags & (1 << bit):
                    out.append(body[i])
                    i += 1
                    continue
                (tok,) = struct.unpack_from("<H", body, i)
                i += 2
                done = len(out) - start
                m = 4
                while (1 << m) < done:
                    m += 1
                disp = (tok >> (16 - m)) + 1
                length = (tok & ((1 << (16 - m)) - 1)) + 3
                if disp > done:
                    raise NtfsError("LZNT1 copy before the start of its chunk")
                for _ in range(length):
                    out.append(out[-disp])
        if len(out) > limit:
            raise NtfsError("LZNT1 output too large")
    return bytes(out)


# ---------------------------------------------------------------------------------------------------------------- the volume

ATTR_NAMES = {0x10: "$STANDARD_INFORMATION", 0x20: "$ATTRIBUTE_LIST", 0x30: "$FILE_NAME", 0x40: "$OBJECT_ID", 0x50: "$SECURITY_DESCRIPTOR", 0x60: "$VOLUME_NAME",
              0x70: "$VOLUME_INFORMATION", 0x80: "$DATA", 0x90: "$INDEX_ROOT", 0xA0: "$INDEX_ALLOCATION", 0xB0: "$BITMAP", 0xC0: "$REPARSE_POINT",
              0xD0: "$EA_INFORMATION", 0xE0: "$EA", 0x100: "$LOGGED_UTILITY_STREAM"}


class Attr:
    def __init__(self, rec_no, buf, pos):
        self.rec_no = rec_no
        self.type, self.length, nonres, namelen, nameoff, self.flags, self.instance = struct.unpack_from("<IIBBHHH", buf, pos)
        if self.length < 16 or self.length % 8 or pos + self.length > len(buf):
            raise NtfsError("record %d: bad attribute length %d at %#x" % (rec_no, self.length, pos))
        self.nonres = bool(nonres)
        self.name = buf[pos + nameoff:pos + nameoff + 2 * namelen].decode("utf-16-le", "surrogatepass") if namelen else ""
        if not self.nonres:
            vlen, voff = struct.unpack_from("<IH", buf, pos + 16)
            if voff + vlen > self.length:
                raise NtfsError("record %d: resident value outside its attribute" % rec_no)
            self.value = bytes(buf[pos + voff:pos + voff + vlen])
        else:
            (self.lowest_vcn, self.highest_vcn, mp, self.cunit) = struct.unpack_from("<QQHB", buf, pos + 16)
            self.alloc, self.size, self.init = struct.unpack_from("<QQQ", buf, pos + 40)
            self.total = struct.unpack_from("<Q", buf, pos + 64)[0] if (self.flags & 0x8001 and mp >= 0x48) else self.alloc
            self.runs = self._runs(buf[pos + mp:pos + self.length], rec_no)

    @staticmethod
    def _runs(raw, rec_no):
        runs, i, lcn, vcn = [], 0, 0, 0
        while i < len(raw) and raw[i]:
            nl, no = raw[i] & 15, raw[i] >> 4
            i += 1
            if nl == 0 or nl > 8 or no > 8 or i + nl + no > len(raw):
                raise NtfsError("record %d: bad run list" % rec_no)
            length = int.from_bytes(raw[i:i + nl], "little")
            i += nl
            if no:
                lcn += int.from_bytes(raw[i:i + no], "little", signed=True)
                runs.append((length, lcn))
            else:
                runs.append((length, None))
            i += no
            vcn += length
        return runs


class Volume:
    def __init__(self, data, offset=0, bitlocker=True, size=None):
        if isinstance(data, (bytes, bytearray, memoryview)):
            blob = bytes(data)
            self._raw = lambda off, n: blob[offset + off:offset + off + n]
            total = len(blob) - offset
        else:                                         # a file object
            f = data
            f.seek(0, 2)
            total = f.tell() - offset

            def rd(off, n, f=f):
                f.seek(offset + off)
                return f.read(n)
            self._raw = rd
        self.size = size or total
        self.bde = None
        head = self._raw(0, 512)
        if head[3:11] == b"-FVE-FS-" and bitlocker:
            self.bde = Bde(self._raw, self.size)
            self._read = self.bde.read
        else:
            self._read = self._raw
        self._boot()
        self._cache = {}
        self._load_mft()
        self._load_upcase()

    # ---- boot sector (libfsntfs "volume header")
    def _boot(self):
        b = self._read(0, 512)
        if b[3:11] != b"NTFS    " or b[510:512] != b"\x55\xAA":
            raise NtfsError("not an NTFS boot sector")
        self.bps = struct.unpack_from("<H", b, 11)[0]
        spc = b[13]
        self.spc = spc if spc <= 128 else 1 << (256 - spc)
        self.cluster = self.bps * self.spc
        self.total_sectors = struct.unpack_from("<Q", b, 40)[0]
        self.mft_lcn, self.mftmirr_lcn = struct.unpack_from("<QQ", b, 48)
        rs, ib = struct.unpack_from("<b", b, 64)[0], struct.unpack_from("<b", b, 68)[0]
        self.rec_size = rs * self.cluster if rs > 0 else 1 << -rs
        self.idx_size = ib * self.cluster if ib > 0 else 1 << -ib
        self.serial = struct.unpack_from("<Q", b, 72)[0]
        self.nclusters = self.total_sectors // self.spc

    def pread(self, off, n):
        data = self._read(off, n)
        if len(data) != n:
            raise NtfsError("read past the end of the volume at %#x" % off)
        return data

    # ---- records
    def _fixup(self, buf, what):
        usa_off, usa_count = struct.unpack_from("<HH", buf, 4)
        if usa_count < 2 or usa_off + 2 * usa_count > len(buf) or (usa_count - 1) * 512 != len(buf):
            raise NtfsError("%s: bad update sequence array (offset %d, count %d)" % (what, usa_off, usa_count))
        usn = buf[usa_off:usa_off + 2]
        b = bytearray(buf)
        for i in range(1, usa_count):
            end = i * 512
            if b[end - 2:end] != usn:
                raise NtfsError("%s: fixup mismatch in stride %d" % (what, i))
            b[end - 2:end] = buf[usa_off + 2 * i:usa_off + 2 * i + 2]
        return bytes(b)

    def _load_mft(self):
        """Record 0 sits at the cluster the boot sector names. Its $DATA run list is the start of the $MFT's map; when the $MFT is fragmented far enough that
        record 0 has an $ATTRIBUTE_LIST, the remaining extents of $DATA are in extension records, which are found through the part of the map known so far."""
        first = self.pread(self.mft_lcn * self.cluster, self.rec_size)
        rec0 = self._fixup(first, "record 0")
        if rec0[:4] != b"FILE":
            raise NtfsError("record 0 is not a FILE record")
        attrs = self._parse_attrs(0, rec0)
        data0 = [a for a in attrs if a.type == 0x80 and a.name == "" and a.nonres and a.lowest_vcn == 0]
        if not data0:
            raise NtfsError("$MFT has no non-resident $DATA")
        self.mft_size = data0[0].size
        runs = list(data0[0].runs)
        lst = [a for a in attrs if a.type == 0x20]
        if lst:
            for typ, name, vcn, ref, inst in list(self._list_entries(0, lst[0])):
                if typ == 0x80 and name == "" and vcn > 0:
                    ext_no = ref & 0xFFFFFFFFFFFF
                    ext = self._parse_attrs(ext_no, self._fixup(self._raw_record(ext_no, runs), "record %d" % ext_no))
                    for a in ext:
                        if a.type == 0x80 and a.name == "" and a.instance == inst and a.lowest_vcn == vcn:
                            runs += a.runs
        self.mft_runs = runs

    def _raw_record(self, no, runs=None):
        runs = runs if runs is not None else self.mft_runs
        off = no * self.rec_size
        out = bytearray()
        while len(out) < self.rec_size:
            data = self._read_runs(runs, off + len(out), self.rec_size - len(out), 1 << 62)
            if not data:
                raise NtfsError("record %d is outside the $MFT" % no)
            out += data
        return bytes(out)

    def _read_runs(self, runs, off, n, limit):
        """Raw cluster data at byte offset `off` of a (non-compressed) run list; sparse runs read as zeros; stops at the end of the list or `limit`."""
        cs = self.cluster
        out = bytearray()
        pos = off
        end = min(off + n, limit)
        vcn_start = 0
        for length, lcn in runs:
            a, b = vcn_start * cs, (vcn_start + length) * cs
            vcn_start += length
            if b <= pos or a >= end:
                continue
            lo, hi = max(a, pos), min(b, end)
            if lcn is None:
                out += bytes(hi - lo)
            else:
                out += self.pread(lcn * cs + (lo - a), hi - lo)
            pos = hi
            if pos >= end:
                break
        return bytes(out)

    def record(self, no):
        if no in self._cache:
            return self._cache[no]
        raw = self._raw_record(no)
        if raw[:4] != b"FILE":
            raise NtfsError("record %d: no FILE signature (%r)" % (no, raw[:4]))
        rec = self._fixup(raw, "record %d" % no)
        self._cache[no] = rec
        return rec

    def _parse_attrs(self, no, rec):
        first, = struct.unpack_from("<H", rec, 20)
        used, = struct.unpack_from("<I", rec, 24)
        pos, out = first, []
        while pos + 8 <= min(used, len(rec)):
            (typ,) = struct.unpack_from("<I", rec, pos)
            if typ == 0xFFFFFFFF:
                break
            a = Attr(no, rec, pos)
            out.append(a)
            pos += a.length
        return out

    def rec_flags(self, no):
        return struct.unpack_from("<H", self.record(no), 22)[0]

    def _list_entries(self, no, la):
        data = la.value if not la.nonres else self.read_attr_data(la)
        pos = 0
        while pos + 26 <= len(data):
            typ, elen, nlen, noff, vcn, ref, inst = struct.unpack_from("<IHBBQQH", data, pos)
            if elen < 26 or pos + elen > len(data):
                raise NtfsError("record %d: bad attribute list entry" % no)
            name = data[pos + noff:pos + noff + 2 * nlen].decode("utf-16-le", "surrogatepass")
            yield typ, name, vcn, ref, inst
            pos += elen

    def attrs(self, no):
        """All attributes of file `no`, the ones in extension records included (through $ATTRIBUTE_LIST)."""
        rec = self.record(no)
        base = self._parse_attrs(no, rec)
        lst = [a for a in base if a.type == 0x20]
        if not lst:
            return base
        out = [a for a in base if a.type != 0x20]
        seen = {(a.type, a.name, a.instance, no) for a in base}
        for typ, name, vcn, ref, inst in self._list_entries(no, lst[0]):
            rno = ref & 0xFFFFFFFFFFFF
            if rno == no:
                continue
            ext = self._parse_attrs(rno, self.record(rno))
            for a in ext:
                if a.type == typ and a.name == name and a.instance == inst and (a.type, a.name, a.instance, rno) not in seen:
                    seen.add((a.type, a.name, a.instance, rno))
                    out.append(a)
        return out

    def read_attr_data(self, attr):
        if not attr.nonres:
            return attr.value
        return self._read_runs(attr.runs, 0, attr.size, attr.size)

    # ---- streams
    def stream_attrs(self, no, name=""):
        got = [a for a in self.attrs(no) if a.type == 0x80 and a.name == name]
        return sorted(got, key=lambda a: getattr(a, "lowest_vcn", 0))

    def read_stream(self, no, name="", off=0, n=None):
        got = self.stream_attrs(no, name)
        if not got:
            raise NtfsError("record %d has no $DATA stream %r" % (no, name))
        a = got[0]
        if not a.nonres:
            data = a.value
            return data[off:off + n] if n is not None else data[off:]
        runs = []
        for x in got:
            runs += x.runs
        size = a.size
        end = size if n is None else min(size, off + n)
        if off >= end:
            return b""
        if a.flags & 0x4000:
            raise NtfsError("encrypted (EFS) file")
        cs = self.cluster
        if a.flags & 0x0001:                                       # LZNT1: units of 2**cunit clusters
            unit = (1 << a.cunit) * cs
            out = bytearray()
            u = off // unit
            while u * unit < end:
                out += self._read_unit(runs, u, unit)
                u += 1
            lo = off - (off // unit) * unit
            data = bytes(out[lo:lo + (end - off)])
        else:
            data = self._read_runs(runs, off, end - off, a.alloc)
        init = a.init
        if off + len(data) > init:                                 # past the initialized size reads as zeros
            keep = max(0, init - off)
            data = data[:keep] + bytes(len(data) - keep)
        return data

    def _read_unit(self, runs, u, unit):
        cs = self.cluster
        ucl = unit // cs
        # which clusters of this unit are real
        vcn, real = 0, []
        for length, lcn in runs:
            lo, hi = max(vcn, u * ucl), min(vcn + length, (u + 1) * ucl)
            if lo < hi:
                real.append((lo - u * ucl, hi - lo, None if lcn is None else lcn + (lo - vcn)))
            vcn += length
        total_real = sum(n for _, n, l in real if l is not None)
        if total_real == 0:
            return bytes(unit)
        if total_real == ucl:                                      # stored plain
            out = bytearray(unit)
            for at, n, l in real:
                if l is not None:
                    out[at * cs:(at + n) * cs] = self.pread(l * cs, n * cs)
            return bytes(out)
        comp = b"".join(self.pread(l * cs, n * cs) for at, n, l in real if l is not None)
        dec = lznt1_decompress(comp)
        return dec + bytes(unit - len(dec)) if len(dec) <= unit else dec[:unit]

    # ---- names, directories
    def _load_upcase(self):
        try:
            raw = self.read_stream(10)
            self.upcase = struct.unpack("<%dH" % (len(raw) // 2), raw[:2 * (len(raw) // 2)])
        except NtfsError:
            self.upcase = None
        self.label = ""
        try:
            self.label = next(a.value for a in self.attrs(3) if a.type == 0x60).decode("utf-16-le")
        except (StopIteration, NtfsError):
            pass

    def _up(self, units):
        if self.upcase and len(self.upcase) == 65536:
            return tuple(self.upcase[u] for u in units)
        return tuple(u - 32 if 97 <= u <= 122 else u for u in units)

    @staticmethod
    def _units(name):
        raw = name.encode("utf-16-le", "surrogatepass")
        return struct.unpack("<%dH" % (len(raw) // 2), raw)

    def _fn(self, key):
        """Fields of a $FILE_NAME value (also an index key)."""
        parent, c, m, r, a, alloc, size, flags, reparse, nlen, ns = struct.unpack_from("<QQQQQQQIIBB", key, 0)
        name = key[66:66 + 2 * nlen].decode("utf-16-le", "surrogatepass")
        return dict(parent=parent & 0xFFFFFFFFFFFF, name=name, ns=ns, flags=flags, size=size, alloc=alloc, reparse=reparse)

    def _index_nodes(self, no):
        """Returns (entries_of_root, read_block): the index header of the root and a function giving a block's raw bytes (fixups applied) for a VCN."""
        attrs = self.attrs(no)
        root = next((a for a in attrs if a.type == 0x90 and a.name == "$I30"), None)
        if root is None:
            raise NtfsError("record %d has no $I30 index" % no)
        v = root.value
        block_size, = struct.unpack_from("<I", v, 8)
        alloc = [a for a in attrs if a.type == 0xA0 and a.name == "$I30"]
        runs = []
        for a in sorted(alloc, key=lambda a: a.lowest_vcn):
            runs += a.runs
        asize = alloc[0].size if alloc else 0
        unit = self.cluster if block_size >= self.cluster else 512

        def block(vcn):
            raw = self._read_runs(runs, vcn * unit, block_size, asize)
            if len(raw) != block_size or raw[:4] != b"INDX":
                raise NtfsError("record %d: no INDX block at vcn %d" % (no, vcn))
            b = self._fixup(raw, "INDX block %d of record %d" % (vcn, no))
            if struct.unpack_from("<Q", b, 16)[0] != vcn:
                raise NtfsError("record %d: INDX block carries VCN %d, expected %d" % (no, struct.unpack_from("<Q", b, 16)[0], vcn))
            return b
        return v, block

    @staticmethod
    def _node_entries(buf, base):
        """Entries of an index node whose index header starts at `base` of buf: yields (offset, length, ref, flags, key)."""
        first, total, _, _ = struct.unpack_from("<IIIB", buf, base)
        pos, end = base + first, base + total
        while pos + 16 <= end:
            ref, elen, klen, flags = struct.unpack_from("<QHHH", buf, pos)
            if elen < 16 or pos + elen > end:
                raise NtfsError("bad index entry")
            yield pos, elen, ref, flags, bytes(buf[pos + 16:pos + 16 + klen]), (struct.unpack_from("<Q", buf, pos + elen - 8)[0] if flags & 1 else None)
            if flags & 2:
                return
            pos += elen

    def index_entries(self, no):
        """In-order list of every $FILE_NAME entry of a directory: dicts with ref (record number), seq, name, ns, flags."""
        root, block = self._index_nodes(no)
        out, stack, seen = [], [], 0

        def visit(buf, base):
            for pos, elen, ref, flags, key, sub in self._node_entries(buf, base):
                if sub is not None:
                    visit_sub(sub)
                if not flags & 2:
                    fn = self._fn(key)
                    fn.update(ref=ref & 0xFFFFFFFFFFFF, seq=ref >> 48)
                    out.append(fn)

        def visit_sub(vcn):
            nonlocal seen
            seen += 1
            if seen > 1_000_000:
                raise NtfsError("index too large or looping")
            visit(block(vcn), 0x18)
        visit(root, 16)
        return out

    def listdir(self, path_or_no="/", system=False):
        no = self.lookup(path_or_no) if isinstance(path_or_no, str) else path_or_no
        out = []
        for e in self.index_entries(no):
            if e["ns"] == 2 or e["name"] in (".", ".."):
                continue
            if e["ref"] < 24 and e["ref"] != 5 and not system:
                continue
            out.append(e)
        return out

    def find(self, dirno, name):
        """B+tree search for `name` (case-insensitive through $UpCase) in a directory index; returns (ref, fn) or None."""
        key = self._up(self._units(name))
        root, block = self._index_nodes(dirno)
        buf, base, depth = root, 16, 0
        while True:
            nxt = None
            for pos, elen, ref, flags, k, sub in self._node_entries(buf, base):
                if flags & 2:
                    nxt = sub
                    break
                fn = self._fn(k)
                ek = self._up(self._units(fn["name"]))
                if ek == key:
                    fn.update(ref=ref & 0xFFFFFFFFFFFF, seq=ref >> 48)
                    return fn
                if key < ek:
                    nxt = sub
                    break
            if nxt is None:
                return None
            depth += 1
            if depth > 32:
                raise NtfsError("index too deep")
            buf, base = block(nxt), 0x18

    def lookup(self, path):
        no = 5
        for part in [p for p in path.split("/") if p]:
            if not self.rec_flags(no) & 2:
                raise NtfsError("not a directory: " + part)
            e = self.find(no, part)
            if e is None:
                raise NtfsError("no such file: " + path)
            no = e["ref"]
        return no

    # ---- stat
    def stat(self, no):
        attrs = self.attrs(no)
        std = next(a for a in attrs if a.type == 0x10).value
        c, m, r, a_, flags = struct.unpack_from("<QQQQI", std, 0)
        rflags = self.rec_flags(no)
        data = next((a for a in attrs if a.type == 0x80 and a.name == "" and (not a.nonres or a.lowest_vcn == 0)), None)
        size = 0 if data is None else (len(data.value) if not data.nonres else data.size)
        rp = next((a for a in attrs if a.type == 0xC0), None)
        tag = struct.unpack_from("<I", rp.value, 0)[0] if rp else 0
        return dict(no=no, dir=bool(rflags & 2), size=0 if rflags & 2 else size, ctime=filetime_to_unix(r), mtime=filetime_to_unix(m), atime=filetime_to_unix(a_),
                    btime=filetime_to_unix(c), flags=flags, links=struct.unpack_from("<H", self.record(no), 18)[0], reparse=tag,
                    compressed=bool(flags & 0x800), sparse=bool(flags & 0x200), encrypted=bool(flags & 0x4000),
                    streams=sorted({a.name for a in attrs if a.type == 0x80 and a.name}))

    def read_path(self, path, stream="", off=0, n=None):
        return self.read_stream(self.lookup(path), stream, off, n)

    def readlink(self, no):
        """Target of a symlink or junction reparse point: (kind, substitute name, print name, relative)."""
        rp = next(a for a in self.attrs(no) if a.type == 0xC0).value
        tag, dlen = struct.unpack_from("<IH", rp, 0)
        d = rp[8:8 + dlen]
        so, sl, po, pl = struct.unpack_from("<HHHH", d, 0)
        if tag == 0xA000000C:
            flags, = struct.unpack_from("<I", d, 8)
            names = d[12:]
            kind, rel = "symlink", bool(flags & 1)
        elif tag == 0xA0000003:
            names, kind, rel = d[8:], "junction", False
        else:
            raise NtfsError("not a link: tag %#x" % tag)
        return kind, names[so:so + sl].decode("utf-16-le", "surrogatepass"), names[po:po + pl].decode("utf-16-le", "surrogatepass"), rel

    # ---- dumps
    def dump_record(self, no):
        rec = self.record(no)
        lines = ["record %d: %s" % (no, rec[:4].decode("latin-1"))]
        seq, links, first, flags, used, alloc, base, nxt = struct.unpack_from("<HHHHIIQH", rec, 16)
        lines.append("  seq %d  links %d  flags %#x (%s)  used %d of %d  base %#x  next attribute id %d  first attribute at %#x" % (
            seq, links, flags, ("in use " if flags & 1 else "free ") + ("directory" if flags & 2 else "file"), used, alloc, base, nxt, first))
        for a in self._parse_attrs(no, rec):
            nm = ATTR_NAMES.get(a.type, hex(a.type))
            tail = ""
            if not a.nonres:
                tail = "resident, %d bytes" % len(a.value)
                if a.type == 0x30:
                    fn = self._fn(a.value)
                    tail += "  name %r ns %d parent %d flags %#x" % (fn["name"], fn["ns"], fn["parent"], fn["flags"])
                elif a.type == 0x10:
                    c, m, r, at, fl = struct.unpack_from("<QQQQI", a.value, 0)
                    tail += "  flags %#x  mtime %s" % (fl, time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(filetime_to_unix(m))))
            else:
                tail = "non-resident vcn %d..%d size %d alloc %d init %d cunit %d runs %s" % (a.lowest_vcn, a.highest_vcn, a.size, a.alloc, a.init, a.cunit, a.runs)
            lines.append("  [%d] %s%s flags %#x  %s" % (a.instance, nm, (" '%s'" % a.name) if a.name else "", a.flags, tail))
        return "\n".join(lines)

    def info(self):
        lines = ["volume label '%s', serial %016x" % (self.label, self.serial), "bytes per sector %d, cluster %d, MFT record %d, index block %d" % (
            self.bps, self.cluster, self.rec_size, self.idx_size), "%d sectors, %d clusters; $MFT at cluster %d (mirror %d), %d bytes" % (
            self.total_sectors, self.nclusters, self.mft_lcn, self.mftmirr_lcn, self.mft_size)]
        if self.bde:
            lines.append("BitLocker: %s; protectors: %s" % (self.bde.method_name, ", ".join(Bde.PROTECTORS.get(p, hex(p)) for p in self.bde.protectors)))
        return "\n".join(lines)


def tstr(t):
    return time.strftime("%Y-%m-%d %H:%M", time.gmtime(t))


def walk(v, path, out):
    for e in v.listdir(path):
        full = (path.rstrip("/") + "/" + e["name"])
        st = v.stat(e["ref"])
        out.append((full, st))
        if st["dir"] and not st["reparse"]:
            walk(v, full, out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("cmd", choices=["info", "ls", "tree", "cat", "rec", "hexrec", "bde"])
    ap.add_argument("arg", nargs="?")
    ap.add_argument("--offset", type=int, default=0)
    ap.add_argument("--stream", default="")
    ap.add_argument("-a", "--all", action="store_true")
    ap.add_argument("--no-bitlocker", action="store_true")
    args = ap.parse_args(argv)
    f = open(args.image, "rb")
    try:
        if args.cmd == "bde":
            f.seek(0, 2)
            size = f.tell() - args.offset

            def rd(off, n):
                f.seek(args.offset + off)
                return f.read(n)
            try:
                b = Bde(rd, size)
                print("unlocked with the clear key: %s; protectors: %s" % (b.method_name, ", ".join(Bde.PROTECTORS.get(p, hex(p)) for p in b.protectors)))
            except NtfsError as e:
                print("not unlocked:", e)
                return 1
            return 0
        v = Volume(f, args.offset, bitlocker=not args.no_bitlocker)
        if args.cmd == "info":
            print(v.info())
        elif args.cmd == "ls":
            for e in v.listdir(args.arg or "/", system=args.all):
                st = v.stat(e["ref"])
                kind = "d" if st["dir"] else ("l" if st["reparse"] in (0xA000000C, 0xA0000003) else "-")
                print("%s %10d  rec %-6d  %s  %s%s" % (kind, st["size"], e["ref"], tstr(st["mtime"]), e["name"], "  [encrypted]" if st["encrypted"] else ""))
        elif args.cmd == "tree":
            out = []
            walk(v, args.arg or "/", out)
            for full, st in out:
                print("%s%s  %d" % (full, "/" if st["dir"] else "", st["size"]))
        elif args.cmd == "cat":
            sys.stdout.buffer.write(v.read_path(args.arg, args.stream))
        elif args.cmd == "rec":
            print(v.dump_record(int(args.arg, 0)))
        elif args.cmd == "hexrec":
            rec = v.record(int(args.arg, 0))
            for i in range(0, len(rec), 16):
                print("%04x  %s  %s" % (i, rec[i:i + 16].hex(" "), "".join(chr(c) if 32 <= c < 127 else "." for c in rec[i:i + 16])))
    except NtfsError as e:
        print("ntfs-look:", e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
