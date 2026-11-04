#!/usr/bin/env python3
"""mkiso -- build the bootable TouhouOS ISO (docs/ISO.md). Pure Python, no outside program makes the image.

  python tools/mkiso.py                         build everything, write build/touhou-os.iso (about 20 MiB)
  python tools/mkiso.py --out E:/touhou.iso     another place
  python tools/mkiso.py --cmdline "video=1280x800 blk.ro"
  python tools/mkiso.py --no-build              use what build/ already holds (build/esp/EFI/BOOT/BOOTX64.EFI, kernel.elf, initrd.img)

What comes out is one file that boots under UEFI in three ways, and never touches the disks of the PC it runs on:

  * as a CD/DVD (a virtual drive: QEMU's -cdrom, VirtualBox, VMware; Hyper-V generation 2 has no keyboard or mouse the system can use, docs/ISO.md): the
    firmware reads the El Torito boot catalog, whose UEFI entry (platform id 0xEF) names an embedded FAT image holding EFI/BOOT/BOOTX64.EFI, kernel.elf,
    initrd.img and cmdline.txt;
  * as a USB stick written byte for byte (Rufus in DD mode, `dd`, balenaEtcher): the first 32 KiB hold a protective MBR and a GPT whose EFI System Partition
    entry covers that same FAT image, so the firmware finds an ordinary ESP;
  * as a plain ISO 9660 volume that any operating system can open: the root shows README.TXT, LICENSE, THIRD_PARTY_NOTICES.TXT, EFI/BOOT/BOOTX64.EFI, KERNEL.ELF,
    INITRD.IMG, CMDLINE.TXT and ESP.IMG. Those files do not take space twice: their extents are the very bytes inside the FAT image.

Written from the specifications, not from any other program: ECMA-119 (ISO 9660: the volume descriptors, directory records, path tables, both-endian fields),
the El Torito Bootable CD-ROM Format Specification 1.0 (boot record, validation and default entries, the checksum rule), the UEFI specification (the GPT, the
EFI System Partition, FAT12/16/32 for removable media, the El Torito rules of section 13.3) and the FAT layout (tools/mkdisk.py's FatBuilder, reused).

Reproducible: with SOURCE_DATE_EPOCH set every time stamp comes from it; the GUIDs are derived from the contents, so the same inputs give the same bytes.
There is no persistent storage on the ISO and no HUMANVILLAGE volume: the system runs from memory and the default command line (video=native blk.ro) keeps
every SATA and NVMe disk of the PC read-only.
"""
import argparse
import hashlib
import io
import os
import struct
import subprocess
import sys
import time
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mkdisk  # noqa: E402  (the FAT writer and the GPT writer; the same code that makes the stick image)

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
BLOCK = 2048                       # ECMA-119 logical block size
SECTOR = 512                       # the GPT's logical block size (a USB stick)
DEFAULT_CMDLINE = "video=native blk.ro"
VOLUME_ID = "TOUHOUOS"
CATALOG_ID = b"TouhouOS"
GPT_ENTRY_BYTES = 128 * 128        # 128 entries of 128 bytes
GPT_TAIL = 33 * SECTOR             # the backup entry array (32 sectors) and the backup header (1 sector)

README = """TouhouOS -- an unofficial Touhou Project fan operating system, written from scratch.

You are looking at the live ISO. It boots a complete system from memory: nothing is installed
and nothing on this computer's disks is touched.

How to start it
  * UEFI only (no legacy BIOS mode). Turn Secure Boot OFF in the firmware setup: the loader
    is not signed.
  * A virtual machine: give it an optical drive holding this file and choose firmware type
    UEFI/EFI (VirtualBox: Settings > System > Enable EFI; VMware: VM > Settings > Options >
    Advanced > Firmware type UEFI; QEMU: use OVMF; Hyper-V: Generation 2, see below).
  * A real PC: write this file to a USB stick as a disk image, NOT as a file (Rufus: choose
    DD Image mode when it asks; Linux/macOS: dd if=touhou-os.iso of=/dev/sdX bs=4M; balenaEtcher
    works too). Everything on the stick is replaced. Then pick the stick in the firmware's
    one-time boot menu (F12, F11, F8, Esc ... depends on the maker) and choose the UEFI entry.

What it does with your disks
  Nothing. The command line "blk.ro" makes every SATA and NVMe disk read-only, and the
  system never mounts a disk by itself. There is nothing to save to: this ISO has no
  HUMANVILLAGE volume, so everything you create lives in memory and is gone at power-off.
  Installing is a separate, explicit step ("install" in a terminal; read "man install"
  first). It is never started by itself.

Known limits
  * Keyboard and mouse must be PS/2 or USB. Hyper-V generation 2 machines have neither
    (their keyboard and mouse are VMBus devices this system cannot use yet).
  * Wi-Fi, a graphics driver other than the firmware's framebuffer, suspend and more than one
    processor are not done yet.

Help the project: run "hwreport" in a terminal (Ctrl+Alt+T on the desktop) and send the
report. It leaves out serial numbers and the end of MAC addresses. See docs/HARDWARE_DB.md
and docs/ISO.md in the source tree.

License: MIT, see LICENSE. Names and art of the Touhou Project belong to ZUN and Team Shanghai
Alice; this is a fan work and nothing here grants rights to them (THIRD_PARTY_NOTICES.TXT).
"""


class IsoError(Exception):
    pass


# ---------------------------------------------------------------------------------------------------------------------
# ECMA-119 building blocks

def both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def pad(s, n, fill=b" "):
    b = s.encode("ascii") if isinstance(s, str) else s
    if len(b) > n:
        raise IsoError(f"{s!r} is longer than {n} bytes")
    return b + fill * (n - len(b))


def dec_datetime(ts):
    """The 17-byte date and time of volume descriptors (ECMA-119 8.4.26.1): digits YYYYMMDDHHMMSS and hundredths, then the offset from GMT in 15 minutes."""
    t = time.gmtime(ts)
    return f"{t.tm_year:04d}{t.tm_mon:02d}{t.tm_mday:02d}{t.tm_hour:02d}{t.tm_min:02d}{t.tm_sec:02d}00".encode() + b"\x00"


NO_DATE = b"0" * 16 + b"\x00"                         # "not specified"


def rec_datetime(ts):
    """The 7-byte recording date of a directory record (9.1.5): years since 1900, month, day, hour, minute, second, offset from GMT."""
    t = time.gmtime(ts)
    return bytes([t.tm_year - 1900, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, 0])


def d_chars_ok(s, allowed):
    return all(c in allowed for c in s)


D_CHARS = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")


def file_ident(name):
    """'README.TXT' -> b'README.TXT;1', 'LICENSE' -> b'LICENSE.;1' (level 2: up to 30 characters of name and extension, d-characters only)."""
    base, dot, ext = name.partition(".")
    if not d_chars_ok(base + ext, D_CHARS) or len(base) + len(ext) > 30 or (not base and not ext):
        raise IsoError(f"{name!r} is not an ISO 9660 level 2 file name")
    return f"{base}.{ext};1".encode("ascii")


def dir_ident(name):
    if not d_chars_ok(name, D_CHARS) or not name or len(name) > 31:
        raise IsoError(f"{name!r} is not an ISO 9660 directory name")
    return name.encode("ascii")


def sort_key(ident):
    """ECMA-119 9.3: records are in order of increasing identifier; the name and the extension are compared separately, the shorter padded with spaces."""
    s = ident.decode("ascii").split(";")[0]
    name, dot, ext = s.partition(".")
    return (name.ljust(32), ext.ljust(32))


def record(extent, size, flags, ident, ts):
    n = 33 + len(ident)
    n += n & 1                                         # the record is always an even number of bytes: a padding byte follows an even-length identifier
    r = bytearray(n)
    r[0] = n
    r[2:10] = both32(extent)
    r[10:18] = both32(size)
    r[18:25] = rec_datetime(ts)
    r[25] = flags
    r[28:32] = both16(1)
    r[32] = len(ident)
    r[33:33 + len(ident)] = ident
    return bytes(r)


FLAG_DIR = 0x02


class Node:
    """An entry of the ISO's own tree. A file has either data of its own (bytes) or `at`, a byte offset in the finished image that already holds it."""

    def __init__(self, name, is_dir=False, data=None, at=None, size=None):
        self.name, self.is_dir = name, is_dir
        self.ident = (dir_ident(name) if name else b"\x00") if is_dir else file_ident(name)      # (the root directory has the identifier 00h)
        self.children = []
        self.data, self.at = data, at
        self.size = size if size is not None else (len(data) if data is not None else 0)
        self.extent = 0
        self.parent = None
        self.number = 0                                  # path table number (directories)

    def add(self, child):
        child.parent = self
        self.children.append(child)
        return child

    def dir_bytes(self, ts):
        out = bytearray()
        recs = [record(self.extent, self.dir_size, FLAG_DIR, b"\x00", ts),
                record((self.parent or self).extent, (self.parent or self).dir_size, FLAG_DIR, b"\x01", ts)]
        for c in sorted(self.children, key=lambda c: sort_key(c.ident)):
            recs.append(record(c.extent, c.dir_size if c.is_dir else c.size, FLAG_DIR if c.is_dir else 0, c.ident, ts))
        for r in recs:
            if (len(out) % BLOCK) + len(r) > BLOCK:    # a record never crosses a block boundary
                out += bytes(BLOCK - len(out) % BLOCK)
            out += r
        return bytes(out).ljust(-(-len(out) // BLOCK) * BLOCK, b"\0")

    def measure(self):
        """The directory's size in bytes (a whole number of blocks), which depends only on the names."""
        n = 0
        sizes = [34, 34] + [33 + len(c.ident) + ((33 + len(c.ident)) & 1) for c in self.children]
        for s in sizes:
            if (n % BLOCK) + s > BLOCK:
                n += BLOCK - n % BLOCK
            n += s
        self.dir_size = -(-n // BLOCK) * BLOCK
        return self.dir_size


def walk_dirs(root):
    """Directories in path-table order: by level, then by the number of the parent, then by name (ECMA-119 6.9.1)."""
    order, level = [root], [root]
    root.number = 1
    while level:
        nxt = []
        for d in level:
            nxt += sorted((c for c in d.children if c.is_dir), key=lambda c: c.ident)
        for c in nxt:
            order.append(c)
            c.number = len(order)
        level = nxt
    return order


def path_table(dirs, big):
    out = bytearray()
    for d in dirs:
        ident = b"\x00" if d.parent is None else d.ident
        parent = 1 if d.parent is None else d.parent.number
        ext = struct.pack(">I" if big else "<I", d.extent)
        par = struct.pack(">H" if big else "<H", parent)
        e = bytes([len(ident), 0]) + ext + par + ident
        out += e + (b"\0" if len(ident) & 1 else b"")
    return bytes(out)


# ---------------------------------------------------------------------------------------------------------------------
# the embedded FAT image

def build_fat(files, ts, hidden, min_clusters=4100):
    """The FAT16 image of the ESP: 2048-byte clusters (the ISO's block size) and a data area that starts on a 2048-byte boundary, so that every file in it begins
    on an ISO block and the ISO's own directory can point at the same bytes. Returns (image bytes, {path: offset of the file in the image})."""
    need = sum(-(-len(d) // BLOCK) for _, d in files) + 16
    clusters = max(min_clusters, int(need * 1.02) + 16)
    for _ in range(64):
        total = clusters * BLOCK + 4 * BLOCK
        for reserved in range(1, 9):                    # the data area must begin on a 2048-byte boundary: try the reserved-sector counts until it does
            win = mkdisk.Window(io.BytesIO(bytes(total)), 0, total, writable=True)
            fb = mkdisk.FatBuilder(win, SECTOR, bits=16, label=VOLUME_ID, cluster_bytes=BLOCK, reserved=reserved, hidden=hidden, ts=ts)
            if fb.data_start * SECTOR % BLOCK == 0:
                break
        else:
            raise IsoError("no FAT geometry puts the data area on a block boundary")
        if fb.nclusters > 65524:
            raise IsoError(f"the files need {need} clusters of 2 KiB: too many for the ISO's FAT16 image (at most 65524, about 128 MiB)")
        for path, data in files:
            fb.add_file(path, data, ts=ts)
        try:
            fb.finish()
        except mkdisk.MkdiskError as e:
            if "full" not in str(e):
                raise
            clusters = int(clusters * 1.05) + 8
            continue
        offsets = {}

        def visit(d, prefix):
            for name, kind, payload in d.children:
                if kind == "d":
                    visit(payload, prefix + name + "/")
                else:
                    offsets[prefix + name] = fb.cluster_offset(payload["first"])
        visit(fb.root, "")
        image = win.f.getvalue()
        for p, off in offsets.items():
            if off % BLOCK:
                raise IsoError(f"{p} starts at byte {off} of the FAT image: not on a block boundary")
        return image, offsets
    raise IsoError("the FAT image would not fit")


def fat_size_for(files, ts):
    return len(build_fat(files, ts, 0)[0])


# ---------------------------------------------------------------------------------------------------------------------

def read_inputs(esp, cmdline):
    names = {"EFI/BOOT/BOOTX64.EFI": esp / "EFI" / "BOOT" / "BOOTX64.EFI", "kernel.elf": esp / "kernel.elf", "initrd.img": esp / "initrd.img"}
    files = []
    for arc, p in names.items():
        if not p.is_file():
            raise IsoError(f"missing {p} (run tools/build.py, or leave out --no-build)")
        files.append((arc, p.read_bytes()))
    files.append(("cmdline.txt", (cmdline.strip() + "\n").encode("ascii")))
    return files


def build_iso(out, files, ts, docs, volume_id=VOLUME_ID):
    """Writes the ISO to `out`; `files` are (path, bytes) for the FAT image, `docs` are (ISO name, bytes) for the files that only the ISO tree has.
    Returns a dict describing the layout."""
    esp_files = files
    # sizes first: the layout needs the length of the FAT image, and the FAT image's BPB wants its own start (hidden sectors), which the layout decides
    fat_len = fat_size_for(esp_files, ts)

    # --- the tree
    root = Node("", is_dir=True)
    efi = root.add(Node("EFI", is_dir=True))
    boot = efi.add(Node("BOOT", is_dir=True))
    boot_efi = boot.add(Node("BOOTX64.EFI", at=-1, size=len(dict(esp_files)["EFI/BOOT/BOOTX64.EFI"])))
    shared = {
        "KERNEL.ELF": root.add(Node("KERNEL.ELF", at=-1, size=len(dict(esp_files)["kernel.elf"]))),
        "INITRD.IMG": root.add(Node("INITRD.IMG", at=-1, size=len(dict(esp_files)["initrd.img"]))),
        "CMDLINE.TXT": root.add(Node("CMDLINE.TXT", at=-1, size=len(dict(esp_files)["cmdline.txt"]))),
    }
    esp_node = root.add(Node("ESP.IMG", at=-1, size=fat_len))
    doc_nodes = [root.add(Node(name, data=data)) for name, data in docs]
    dirs = walk_dirs(root)
    for d in dirs:
        d.measure()

    # --- the layout, in blocks
    lb = 16
    pvd, br, term, catalog = lb, lb + 1, lb + 2, lb + 3
    lb += 4
    pt_size = len(path_table(dirs, False))              # (the extents are not known yet, the size is)
    pt_blocks = -(-pt_size // BLOCK)
    l_pt, m_pt = lb, lb + pt_blocks
    lb += 2 * pt_blocks
    for d in dirs:
        d.extent = lb
        lb += d.dir_size // BLOCK
    for n in doc_nodes:
        n.extent = lb
        lb += max(1, -(-n.size // BLOCK))
    esp_lb = lb
    lb += fat_len // BLOCK
    tail_blocks = -(-GPT_TAIL // BLOCK)
    total_blocks = lb + tail_blocks
    total = total_blocks * BLOCK
    esp_node.extent = esp_lb
    fat_image, offsets = build_fat(esp_files, ts, hidden=esp_lb * BLOCK // SECTOR)
    if len(fat_image) != fat_len:
        raise IsoError("the FAT image changed size between the two passes")
    for path, node in (("EFI/BOOT/BOOTX64.EFI", boot_efi), ("kernel.elf", shared["KERNEL.ELF"]), ("initrd.img", shared["INITRD.IMG"]),
                       ("cmdline.txt", shared["CMDLINE.TXT"])):
        node.extent = esp_lb + offsets[path] // BLOCK    # the very bytes inside the FAT image

    # --- volume descriptors
    root_rec = record(root.extent, root.dir_size, FLAG_DIR, b"\x00", ts)
    p = bytearray(BLOCK)
    p[0:8] = b"\x01CD001\x01\x00"
    p[8:40] = pad("TOUHOUOS", 32)
    p[40:72] = pad(volume_id, 32)
    p[80:88] = both32(total_blocks)
    p[120:124] = both16(1)
    p[124:128] = both16(1)
    p[128:132] = both16(BLOCK)
    p[132:140] = both32(pt_size)
    p[140:144] = struct.pack("<I", l_pt)
    p[148:152] = struct.pack(">I", m_pt)
    p[156:190] = root_rec
    p[190:318] = pad("", 128)
    p[318:446] = pad("TOUHOUOS FAN PROJECT", 128)
    p[446:574] = pad("tools/mkiso.py", 128)
    p[574:702] = pad("TOUHOUOS ISO WRITER", 128)
    p[702:739] = pad("LICENSE.;1", 37)
    p[739:776] = pad("", 37)
    p[776:813] = pad("", 37)
    p[813:830] = dec_datetime(ts)
    p[830:847] = dec_datetime(ts)
    p[847:864] = NO_DATE
    p[864:881] = dec_datetime(ts)
    p[881] = 1
    b = bytearray(BLOCK)                                 # the El Torito boot record
    b[0:7] = b"\x00CD001\x01"
    b[7:39] = pad(b"EL TORITO SPECIFICATION", 32, b"\0")
    b[71:75] = struct.pack("<I", catalog)
    t = bytearray(BLOCK)                                 # the terminator
    t[0:7] = b"\xffCD001\x01"

    # --- the boot catalog: a validation entry for the EFI platform, then the one default entry
    val = bytearray(32)
    val[0] = 0x01                                        # header id
    val[1] = 0xEF                                        # platform: EFI
    val[4:4 + len(CATALOG_ID)] = CATALOG_ID
    val[30], val[31] = 0x55, 0xAA
    csum = (-sum(struct.unpack("<16H", bytes(val)))) & 0xFFFF   # the 16-bit words of the entry add up to zero
    val[28:30] = struct.pack("<H", csum)
    ent = bytearray(32)
    ent[0] = 0x88                                        # bootable
    ent[1] = 0x00                                        # no emulation
    ent[6:8] = struct.pack("<H", min(fat_len // SECTOR, 0xFFFF))     # sector count: the image in 512-byte units (capped; a FAT driver reads the size from its own BPB)
    ent[8:12] = struct.pack("<I", esp_lb)                # where the image starts, in blocks
    cat = bytes(val) + bytes(ent)

    # --- write it out
    with open(out, "wb") as f:
        f.truncate(total)

        def put(block, data):
            f.seek(block * BLOCK)
            f.write(data)
        put(pvd, bytes(p))
        put(br, bytes(b))
        put(term, bytes(t))
        put(catalog, cat.ljust(BLOCK, b"\0"))
        put(l_pt, path_table(dirs, False))
        put(m_pt, path_table(dirs, True))
        for d in dirs:
            put(d.extent, d.dir_bytes(ts))
        for n in doc_nodes:
            put(n.extent, n.data)
        put(esp_lb, fat_image)
    esp_first = esp_lb * BLOCK // SECTOR
    ident = hashlib.sha256(fat_image).digest()
    with open(out, "r+b") as f:
        win = mkdisk.Window(f, 0, total, writable=True)
        mkdisk.gpt_write(win, SECTOR, total // SECTOR,
                         [dict(type=mkdisk.GPT_TYPE_ESP, unique=uuid.UUID(bytes=ident[16:32]), first=esp_first, last=esp_first + fat_len // SECTOR - 1,
                               name="EFI System Partition")],
                         uuid.UUID(bytes=ident[0:16]))
    return dict(total=total, blocks=total_blocks, esp_lb=esp_lb, esp_len=fat_len, catalog=catalog, root=root.extent, pvd=pvd, offsets=offsets,
                files={k: v.extent for k, v in shared.items()})


def run(cmd, **kw):
    print("  $", " ".join(str(c) for c in cmd))
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def timestamp():
    sde = os.environ.get("SOURCE_DATE_EPOCH")
    return int(sde) if sde else int(time.time())


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=str(BUILD / "touhou-os.iso"), help="file to write (default build/touhou-os.iso)")
    ap.add_argument("--cmdline", default=DEFAULT_CMDLINE, help=f"kernel command line, written to cmdline.txt on the ISO (default '{DEFAULT_CMDLINE}')")
    ap.add_argument("--no-build", action="store_true", help="do not run tools/build.py first")
    ap.add_argument("--esp", default=str(BUILD / "esp"), help="the folder holding EFI/BOOT/BOOTX64.EFI, kernel.elf and initrd.img (default build/esp)")
    a = ap.parse_args(argv)
    if not a.no_build:
        print("[build]")
        run([sys.executable, ROOT / "tools" / "build.py"])
    try:
        ts = timestamp()
        files = read_inputs(Path(a.esp), a.cmdline)
        docs = [("README.TXT", README.replace("\n", "\r\n").encode("ascii")),
                ("LICENSE", (ROOT / "LICENSE").read_bytes()),
                ("THIRD_PARTY_NOTICES.TXT", (ROOT / "THIRD_PARTY_NOTICES.md").read_bytes())]
        out = Path(a.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        print("[iso]")
        info = build_iso(out, files, ts, docs)
    except (IsoError, mkdisk.MkdiskError, OSError) as e:
        print(f"mkiso: {e}", file=sys.stderr)
        return 1
    sha = hashlib.sha256()
    with open(out, "rb") as f:
        while chunk := f.read(1 << 22):
            sha.update(chunk)
    print(f"  command line: {a.cmdline.strip()!r}")
    print(f"  boot image: the FAT16 ESP at block {info['esp_lb']} ({info['esp_len'] >> 10} KiB), shared with the files KERNEL.ELF, INITRD.IMG, CMDLINE.TXT and EFI/BOOT/BOOTX64.EFI of the tree")
    print()
    print(f"iso    {out}  ({out.stat().st_size} bytes, {out.stat().st_size / 1048576:.2f} MiB; the ESP image is {info['esp_len'] / 1048576:.2f} MiB of it)")
    print(f"sha256 {sha.hexdigest()}")
    print()
    print("Write it to a USB stick as a disk image (Rufus: DD Image mode; Linux: dd if=touhou-os.iso of=/dev/sdX bs=4M) or give it to a virtual machine's optical drive.")
    print("UEFI only, Secure Boot off. docs/ISO.md has the details for QEMU, VirtualBox, VMware and Hyper-V.")
    return 0


if __name__ == "__main__":
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except AttributeError:
            pass
    sys.exit(main())
