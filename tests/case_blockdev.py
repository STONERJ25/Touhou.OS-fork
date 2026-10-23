"""Block device tests: the AHCI (SATA) and NVMe drivers against QEMU's emulated controllers.

Disk images are generated here (build/blkdev/*) with a pattern the host can recompute for any byte range, so every byte the
guest reads is checked against the host and every byte it writes is inspected in the image file after the VM has exited.
The guest runs as root (user=root) because the /dev block nodes are mode 0600.

Layout of a QEMU q35 guest with this harness: port 0 of the built-in AHCI controller is the boot ESP (sda, a QEMU vvfat
disk), port 2 holds QEMU's default CD-ROM, so test disks use ports 1, 3, 4 and 5 (bus names ide.N), or a second controller.
"""
import hashlib
import re
import shutil
import struct
import sys
import time
import zlib
from pathlib import Path

from harness import test, Qemu, BUILD

MiB = 1 << 20
IMG_DIR = BUILD / "blkdev"
PROMPT = r"root@gensokyo \S+ # "
MASK = (1 << 64) - 1


# ---------------------------------------------------------------------------------------------------------- patterns

def pat_a(b):
    """The word that fills 512-byte block b of a pattern-A image (blk_selftest.c computes the same)."""
    return (((b + 1) * 0x9E3779B97F4A7C15) & MASK) ^ 0x5A5A5A5A5A5A5A5A


def pat_b(b):
    return (((b + 7) * 0xD1B54A32D192ED03) & MASK) ^ 0x1234567890ABCDEF


def pattern_blocks(first, count, fn=pat_a):
    return b"".join(struct.pack("<Q", fn(b)) * 64 for b in range(first, first + count))


def pattern_slice(off, length, fn=pat_a):
    """The bytes [off, off+length) of an infinite pattern image."""
    first, last = off // 512, (off + length - 1) // 512
    data = pattern_blocks(first, last - first + 1, fn)
    return data[off - first * 512: off - first * 512 + length]


def write_pattern(path, size, fn=pat_a):
    with open(path, "wb") as f:
        done = 0
        while done < size:
            n = min(MiB, size - done)
            f.write(pattern_blocks(done // 512, n // 512, fn))
            done += n


_pattern_cache = {}


def pattern_image(name, size_mib, fn=pat_a):
    """A fresh pattern image build/blkdev/<name>.img (copied from a cached master so repeated tests stay fast)."""
    IMG_DIR.mkdir(parents=True, exist_ok=True)
    key = (size_mib, fn.__name__)
    master = IMG_DIR / f"master-{fn.__name__}-{size_mib}.img"
    if key not in _pattern_cache or not master.exists():
        write_pattern(master, size_mib * MiB, fn)
        _pattern_cache[key] = master
    dst = IMG_DIR / f"{name}.img"
    shutil.copyfile(master, dst)
    return dst


def sha(data):
    return hashlib.sha256(data).hexdigest()


def sha_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(MiB)
            if not chunk:
                return h.hexdigest()
            h.update(chunk)


# ------------------------------------------------------------------------------------------- partition tables

LINUX_FS = bytes([0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47, 0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4])
DISK_GUID = bytes(range(16))


def gpt_image(name, size_mib, parts, sector=512):
    """Pattern image with a protective MBR and a valid primary and backup GPT. parts: [(first_lba, last_lba, label)]."""
    path = pattern_image(name, size_mib)
    size = size_mib * MiB
    total = size // sector
    entries = bytearray(128 * 128)
    for i, (first, last, label) in enumerate(parts):
        unique = bytes([i + 1]) * 16
        entry = LINUX_FS + unique + struct.pack("<QQQ", first, last, 0) + label.encode("utf-16-le").ljust(72, b"\0")
        entries[i * 128:(i + 1) * 128] = entry
    ent_sectors = len(entries) // sector
    first_usable, last_usable = 2 + ent_sectors, total - 2 - ent_sectors
    entries_crc = zlib.crc32(bytes(entries))

    def header(my, alt, entries_lba):
        h = struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x00010000, 92, 0, 0, my, alt, first_usable, last_usable, DISK_GUID, entries_lba, 128, 128,
                        entries_crc)
        h = h[:16] + struct.pack("<I", zlib.crc32(h[:16] + b"\0\0\0\0" + h[20:])) + h[20:]
        return h.ljust(sector, b"\0")

    mbr = bytearray(sector)
    mbr[446:462] = bytes([0, 0, 2, 0, 0xEE, 0xFF, 0xFF, 0xFF]) + struct.pack("<II", 1, min(total - 1, 0xFFFFFFFF))
    mbr[510:512] = b"\x55\xAA"
    with open(path, "r+b") as f:
        f.seek(0)
        f.write(bytes(mbr))
        f.write(header(1, total - 1, 2))
        f.write(bytes(entries))
        f.seek((total - 1 - ent_sectors) * sector)
        f.write(bytes(entries))
        f.write(header(total - 1, 1, total - 1 - ent_sectors))
    return path


def mbr_image(name, size_mib, parts):
    """Pattern image whose first sector is an MBR. parts: [(start_lba, sectors, type_byte)]."""
    path = pattern_image(name, size_mib)
    sec = bytearray(512)
    for i, (start, count, ptype) in enumerate(parts):
        sec[446 + 16 * i:462 + 16 * i] = bytes([0, 0, 0, 0, ptype, 0, 0, 0]) + struct.pack("<II", start, count)
    sec[510:512] = b"\x55\xAA"
    with open(path, "r+b") as f:
        f.write(bytes(sec))
    return path


# -------------------------------------------------------------------------------------------------- QEMU arguments

def q(path):
    return str(path).replace("\\", "/")


def drive(did, img, read_error=None, write_error=None):
    """-drive arguments for a raw image; read_error / write_error are 512-byte sector numbers that fail with EIO (via blkdebug)."""
    if read_error is None and write_error is None:
        return ["-drive", f"if=none,id={did},file={q(img)},format=raw"]
    spec = f"if=none,id={did},driver=raw,file.driver=blkdebug,file.image.driver=file,file.image.filename={q(img)},rerror=report,werror=report"
    rules = [("read_aio", read_error), ("write_aio", write_error)]
    for i, (event, sector) in enumerate([r for r in rules if r[1] is not None]):
        spec += f",file.inject-error.{i}.event={event},file.inject-error.{i}.errno=5,file.inject-error.{i}.sector={sector}"
    return ["-drive", spec]


def sata_disk(did, img, bus, read_error=None, write_error=None, **props):
    """An IDE disk on an AHCI port (bus 'ide.N' on the built-in controller, 'ahciN.M' on an extra one)."""
    dev = f"ide-hd,drive={did},bus={bus}" + "".join(f",{k}={v}" for k, v in props.items())
    return drive(did, img, read_error, write_error) + ["-device", dev]


def nvme_ctrl(cid, serial, namespaces, **props):
    """One NVMe controller; namespaces is [(image, nsid, {property: value}, {'read_error': .., 'write_error': ..})]."""
    args = []
    for i, ns in enumerate(namespaces):
        img = ns[0]
        errs = ns[3] if len(ns) > 3 else {}
        args += drive(f"{cid}-d{i}", img, **errs)
    args += ["-device", f"nvme,id={cid},serial={serial}" + "".join(f",{k}={v}" for k, v in props.items())]
    for i, ns in enumerate(namespaces):
        nsprops = ns[2] if len(ns) > 2 else {}
        args += ["-device", f"nvme-ns,drive={cid}-d{i},bus={cid},nsid={ns[1]}" + "".join(f",{k}={v}" for k, v in nsprops.items())]
    return args


# ----------------------------------------------------------------------------------------------------- guest access

class Guest:
    """A booted gensokyo with a root shell on the serial console."""

    def __init__(self, name, extra=(), cmdline="", mem="512M", boot_timeout=90):
        self.name, self.extra, self.cmdline, self.mem, self.boot_timeout = name, list(extra), cmdline, mem, boot_timeout
        self.vm = None

    def __enter__(self):
        self.vm = Qemu(cmdline=("text user=root " + self.cmdline).strip(), name=self.name, extra=self.extra, mem=self.mem)
        self.vm.start()
        try:
            self.vm.expect(PROMPT, timeout=self.boot_timeout)
        except Exception:
            self.vm.stop()
            raise
        return self

    def __exit__(self, *exc):
        self.vm.stop()

    def log(self):
        return self.vm.text()

    def run(self, cmd, timeout=90):
        """Runs a command line; returns (output, exit status)."""
        self.vm.send(f"echo __BE''GIN__; {cmd}; echo __EN''D__$?\n")
        m = self.vm.expect(r"\n__BEGIN__\n(.*?)__END__(\d+)\n", timeout=timeout)
        self.vm.expect(PROMPT, timeout=15)
        return m.group(1), int(m.group(2))

    def ok(self, cmd, timeout=90):
        out, rc = self.run(cmd, timeout)
        assert rc == 0, f"'{cmd}' exited with {rc}:\n{out[-800:]}"
        return out

    def sha_of(self, cmd, timeout=90):
        """Runs a pipeline that ends in sha256sum and returns the hex digest."""
        out = self.ok(cmd, timeout)
        m = re.search(r"\b([0-9a-f]{64})\b", out)
        assert m, f"no digest in the output of '{cmd}':\n{out[-500:]}"
        return m.group(1)

    def count_bytes(self, cmd, timeout=60):
        out = self.ok(cmd + " | wc -c", timeout)
        m = re.search(r"(\d+)\s*$", out.strip())
        assert m, out
        return int(m.group(1))

    def partitions(self):
        """/proc/partitions as {name: {...}}."""
        out = self.ok("cat /proc/partitions")
        rows = {}
        for line in out.splitlines():
            f = line.split(None, 8)
            if len(f) >= 8 and re.fullmatch(r"[a-z][a-z0-9]*", f[0]):
                rows[f[0]] = dict(size=int(f[1]), ss=int(f[2]), ro=int(f[3]), parent=f[4], start=int(f[5]), label=f[6], driver=f[7],
                                  model=f[8].strip() if len(f) > 8 else "-")
        return rows


def must(text, pattern, what=""):
    m = re.search(pattern, text)
    assert m, f"expected /{pattern}/ {what}\n--- got ---\n{text[-2500:]}"
    return m


def device_by_model(parts, model):
    names = [n for n, r in parts.items() if r["model"] == model and r["parent"] == "-"]
    assert len(names) == 1, f"{model!r}: {names} in {sorted(parts)}"
    return names[0]


# ------------------------------------------------------------------------------------------------------ read checks

# (bs, skip, count): arbitrary byte ranges [bs*skip, bs*(skip+count)) that start and end inside sectors
RANGES = [(512, 0, 1), (100, 5, 3), (1000, 3, 7), (777, 101, 50), (4096, 1, 3), (3, 1, 600), (65536, 1, 2), (7, 70001, 300), (513, 17, 33)]


def check_reads(g, dev, size):
    """Whole-device hash, unaligned byte ranges, and the short reads at the end of the device."""
    data = pattern_slice(0, size)
    assert g.sha_of(f"sha256sum /dev/{dev}") == sha(data), f"/dev/{dev} does not match the image"
    for bs, skip, count in RANGES:
        off, length = bs * skip, bs * count
        if off + length > size:
            continue
        got = g.sha_of(f"dd if=/dev/{dev} bs={bs} skip={skip} count={count} status=none | sha256sum")
        assert got == sha(data[off:off + length]), f"/dev/{dev}: bs={bs} skip={skip} count={count} returned the wrong bytes"
    # reads at and past the end are short
    assert g.count_bytes(f"dd if=/dev/{dev} bs=4096 skip={size // 4096 - 1} count=10 status=none") == 4096
    assert g.count_bytes(f"dd if=/dev/{dev} bs=4096 skip={size // 4096} count=1 status=none") == 0
    assert g.count_bytes(f"dd if=/dev/{dev} bs=1000 skip={size // 1000 - 1} count=5 status=none") == size - (size // 1000 - 1) * 1000
    out = g.ok(f"dd if=/dev/{dev} of=/dev/null bs=1000 skip={size // 1000 - 1} count=5")
    must(out, r"1\+1 records in", f"(one full and one partial record from {dev})")


# -------------------------------------------------------------------------------------------------------- SATA reads

@test
def blk_sata_read():
    """AHCI: a SATA disk shows up as sdb with the right model, size and data; partial-sector and past-the-end reads behave."""
    size = 4 * MiB
    img = pattern_image("sata-read", 4)
    extra = sata_disk("d1", img, "ide.1", model="TOUHOU-SSD", serial="SN-AHCI-001", ver="FW1.2", rotation_rate=1) + ["-device", "ide-cd,bus=ide.2"]
    with Guest("t-blk-sata-read", extra, cmdline="ahci.selftest") as g:
        log = g.log()
        must(log, r'ahci0: 8086:2922 at 00:1f\.2, AHCI [\d.]+, 6 of 6 ports implemented, 32 command slots, 64-bit DMA')
        must(log, r'ahci0 port 1: [\d.]+ Gbps SSD link, "TOUHOU-SSD" serial SN-AHCI-001 firmware FW1\.2')
        must(log, r'ahci0 port 1: 4 MiB, 8192 sectors of 512 bytes')
        must(log, r'ahci0 port 2: ATAPI device \(CD/DVD\) -- not supported, ignored', "(an ATAPI device must be skipped, not driven)")
        must(log, r'ahci: identify selftest: PASS')
        parts = g.partitions()
        assert "sda" in parts and parts["sda"]["driver"] == "ahci", "the boot disk (the ESP) should be sda"
        sdb = parts["sdb"]
        assert (sdb["size"], sdb["ss"], sdb["ro"], sdb["parent"], sdb["driver"], sdb["model"]) == (size, 512, 0, "-", "ahci", "TOUHOU-SSD"), sdb
        must(g.ok("lsblk"), r"(?m)^sdb\s+4\.0M\s+0\s+disk\s+ahci\s+TOUHOU-SSD\s*$")
        check_reads(g, "sdb", size)
        must(g.ok("cat /proc/ahci"), r'port 1 sdb "TOUHOU-SSD" serial SN-AHCI-001 fw FW1\.2 8192 x 512 ssd +cmds \d+ errors 0 resets 0')


# -------------------------------------------------------------------------------------------------------- NVMe reads

@test
def blk_nvme_read():
    """NVMe: a namespace shows up as nvme0n1 with the right model, size and data; partial-sector and past-the-end reads behave."""
    size = 4 * MiB
    img = pattern_image("nvme-read", 4)
    extra = nvme_ctrl("nvme-a", "TOUHOU01", [(img, 1)])
    with Guest("t-blk-nvme-read", extra) as g:
        log = g.log()
        must(log, r'nvme0: 1b36:0010 at 00:02\.0, NVMe \d+\.\d+, up to 2048 queue entries, doorbell stride 4')
        must(log, r'nvme0: "QEMU NVMe Ctrl" serial TOUHOU01 firmware \S+, namespace ids up to \d+, max transfer 128 KiB per command')
        must(log, r'nvme0: nsid 1: 4 MiB, 8192 blocks of 512 bytes')
        parts = g.partitions()
        n1 = parts["nvme0n1"]
        assert (n1["size"], n1["ss"], n1["ro"], n1["parent"], n1["driver"], n1["model"]) == (size, 512, 0, "-", "nvme", "QEMU NVMe Ctrl"), n1
        must(g.ok("lsblk"), r"(?m)^nvme0n1\s+4\.0M\s+0\s+disk\s+nvme\s+QEMU NVMe Ctrl\s*$")
        check_reads(g, "nvme0n1", size)
        must(g.ok("cat /proc/nvme"), r'nvme0 00:02\.0 1b36:0010 "QEMU NVMe Ctrl" serial TOUHOU01 fw \S+ version \d+\.\d+ cmds \d+ errors 0 flushes \d+ bounced \d+\s+nsid 1 nvme0n1 8192 x 512')


# -------------------------------------------------------------------------------------------------- writes and copies

def expected_copy(dst_bytes, src_bytes, plan):
    """Applies the same byte-range copies the guest does to a host copy of the destination. plan: [(src_off, dst_off, length)]."""
    out = bytearray(dst_bytes)
    for s, d, n in plan:
        out[d:d + n] = src_bytes[s:s + n]
    return bytes(out)


SIZE6 = 6 * MiB
# (guest dd arguments after if=/of=, equivalent (src_off, dst_off, length) on the host)
COPY_STEPS = [
    ("bs=512 count=2048", (0, 0, 1 * MiB)),                             # 1 MiB with 512-byte records
    ("bs=4096 skip=256 seek=256 count=256", (1 * MiB, 1 * MiB, 1 * MiB)),
    ("bs=65536 skip=32 seek=32 count=32", (2 * MiB, 2 * MiB, 2 * MiB)),
    ("bs=1048576 skip=4 seek=4 count=1", (4 * MiB, 4 * MiB, 1 * MiB)),
    ("bs=100 skip=52429 seek=52430 count=33", (5242900, 5243000, 3300)),   # unaligned start and length, straddling sectors
    ("bs=1000 skip=6000 seek=6288 count=3", (6000000, 6288000, 3000)),     # ends inside the last sector region of the disk
]
COPY_PLAN = [step[1] for step in COPY_STEPS]


@test
def blk_copy_matrix():
    """Writes through AHCI and NVMe (SATA->SATA, SATA->NVMe, NVMe->SATA, NVMe->NVMe) at several block sizes land in the image files."""
    src_sata = pattern_image("cm-src-sata", 6)
    src_nvme = pattern_image("cm-src-nvme", 6)
    dsts = {n: pattern_image(f"cm-dst-{n}", 6, pat_b) for n in ("ss", "sn", "ns", "nn")}
    extra = (sata_disk("s0", src_sata, "ide.1", model="SRC-SATA") + sata_disk("s1", dsts["ss"], "ide.3", model="DST-S-FROM-S") +
             sata_disk("s2", dsts["ns"], "ide.4", model="DST-S-FROM-N") +
             nvme_ctrl("nv", "CM0001", [(src_nvme, 1), (dsts["sn"], 2), (dsts["nn"], 3)]))
    pairs = [("sdb", "sdc", "ss"), ("sdb", "nvme0n2", "sn"), ("nvme0n1", "sdd", "ns"), ("nvme0n1", "nvme0n3", "nn")]
    with Guest("t-blk-copy", extra, cmdline="ahci.nosync nvme.nosync") as g:       # (a flush per 512-byte write would just make it slow)
        parts = g.partitions()
        assert device_by_model(parts, "SRC-SATA") == "sdb" and device_by_model(parts, "DST-S-FROM-S") == "sdc" and device_by_model(parts, "DST-S-FROM-N") == "sdd"
        assert all(n in parts for n in ("nvme0n1", "nvme0n2", "nvme0n3")), sorted(parts)
        srcdata = pattern_slice(0, SIZE6)
        for src, dst, tag in pairs:
            for args, _ in COPY_STEPS:
                g.ok(f"dd if=/dev/{src} of=/dev/{dst} {args} conv=notrunc status=none", timeout=120)
            want = expected_copy(pattern_slice(0, SIZE6, pat_b), srcdata, COPY_PLAN)
            assert g.sha_of(f"sha256sum /dev/{dst}") == sha(want), f"{src} -> {dst}: the guest reads back something else than it wrote"
        # a write that starts past the end must fail without touching anything
        out, rc = g.run("dd if=/dev/sdb of=/dev/sdc bs=4096 seek=1536 count=1 conv=notrunc")
        must(out, r"No space left|error writing", "(a write past the end of the device)")
        # and a write that ends exactly at the end must succeed
        g.ok("dd if=/dev/sdb of=/dev/sdc bs=4096 skip=1535 seek=1535 count=1 conv=notrunc status=none")
        g.ok("sync")
    # after the VM has exited: the host images hold exactly what the guest wrote
    base = {tag: expected_copy(pattern_slice(0, SIZE6, pat_b), srcdata, COPY_PLAN) for tag in dsts}
    base["ss"] = expected_copy(base["ss"], srcdata, [(SIZE6 - 4096, SIZE6 - 4096, 4096)])
    for tag, path in dsts.items():
        assert path.read_bytes() == base[tag], f"image of destination '{tag}' differs from what the guest wrote"


@test
def blk_write_flush():
    """Each write request to a disk with a volatile write cache is followed by a flush; reads are not; 'nosync' switches it off."""
    ssd = pattern_image("wf-sata", 2)
    nvm = pattern_image("wf-nvme", 2)
    extra = sata_disk("s0", ssd, "ide.1", model="WF-SATA") + nvme_ctrl("wf", "WF0001", [(nvm, 1)])
    for nosync in (False, True):
        with Guest(f"t-blk-flush-{int(nosync)}", extra, cmdline="ahci.nosync nvme.nosync" if nosync else "") as g:
            log = g.log()
            if nosync:
                must(log, r"ahci0 port 1: 2 MiB, 4096 sectors of 512 bytes, write cache not flushed after writes \(nosync\)")
                must(log, r'nvme0: "QEMU NVMe Ctrl".*volatile write cache \(nosync: not flushed\)')
            else:
                must(log, r"ahci0 port 1: 2 MiB, 4096 sectors of 512 bytes, volatile write cache \(flushed after every write\)")
                must(log, r'nvme0: "QEMU NVMe Ctrl".*volatile write cache \(flushed after every write\)')
            def flushes():
                a = int(must(g.ok("cat /proc/ahci"), r'"WF-SATA".* flushes (\d+)').group(1))
                n = int(must(g.ok("cat /proc/nvme"), r'"QEMU NVMe Ctrl".* flushes (\d+)').group(1))
                return a, n
            before = flushes()
            g.ok("dd if=/dev/sdb of=/dev/null bs=4096 count=10 status=none")
            g.ok("dd if=/dev/nvme0n1 of=/dev/null bs=4096 count=10 status=none")
            assert flushes() == before, "reading must not flush"
            g.ok("dd if=/dev/zero of=/dev/sdb bs=4096 count=20 conv=notrunc status=none")
            g.ok("dd if=/dev/zero of=/dev/nvme0n1 bs=4096 count=20 conv=notrunc status=none")
            after = flushes()
            if nosync:
                assert after == before, f"nosync: no flushes expected, got {before} -> {after}"
            else:
                assert after[0] - before[0] >= 20 and after[1] - before[1] >= 20, f"expected a flush per write request: {before} -> {after}"


# ----------------------------------------------------------------------------------------------- partition tables

@test
def blk_partitions():
    """GPT and MBR partition children (sdb1, nvme0n1p2, a 4096-byte-sector nvme0n2p1, ...) have the right start, size, label and data."""
    gpt512 = gpt_image("part-gpt512", 8, [(2048, 6143, "TOUHOU-ONE"), (6144, 14335, "TOUHOU-TWO")])
    mbr = mbr_image("part-mbr", 8, [(2048, 4096, 0x83), (8192, 4096, 0x0c)])
    nv512 = gpt_image("part-nv512", 8, [(64, 8255, "NVME-FIRST"), (8256, 16319, "NVME-SECOND")])
    nv4k = gpt_image("part-nv4k", 8, [(8, 263, "FOURK-A"), (264, 1031, "FOURK-B")], sector=4096)
    extra = (sata_disk("p0", gpt512, "ide.1", model="GPT-DISK") + sata_disk("p1", mbr, "ide.3", model="MBR-DISK") +
             nvme_ctrl("np", "PART01", [(nv512, 1), (nv4k, 2, {"logical_block_size": 4096, "physical_block_size": 4096})]))
    with Guest("t-blk-parts", extra) as g:
        parts = g.partitions()
        gpt, mb = device_by_model(parts, "GPT-DISK"), device_by_model(parts, "MBR-DISK")
        want = {
            f"{gpt}1": dict(size=4096 * 512, ss=512, parent=gpt, start=2048, label="TOUHOU-ONE", driver="ahci"),
            f"{gpt}2": dict(size=8192 * 512, ss=512, parent=gpt, start=6144, label="TOUHOU-TWO", driver="ahci"),
            f"{mb}1": dict(size=4096 * 512, ss=512, parent=mb, start=2048, label="-", driver="ahci"),
            f"{mb}2": dict(size=4096 * 512, ss=512, parent=mb, start=8192, label="-", driver="ahci"),
            "nvme0n1p1": dict(size=8192 * 512, ss=512, parent="nvme0n1", start=64, label="NVME-FIRST", driver="nvme"),
            "nvme0n1p2": dict(size=8064 * 512, ss=512, parent="nvme0n1", start=8256, label="NVME-SECOND", driver="nvme"),
            "nvme0n2p1": dict(size=256 * 4096, ss=4096, parent="nvme0n2", start=8, label="FOURK-A", driver="nvme"),
            "nvme0n2p2": dict(size=768 * 4096, ss=4096, parent="nvme0n2", start=264, label="FOURK-B", driver="nvme"),
        }
        for name, w in want.items():
            assert name in parts, f"{name} missing from {sorted(parts)}"
            got = {k: parts[name][k] for k in w}
            assert got == w, f"{name}: {got} != {w}"
        assert parts["nvme0n2"]["ss"] == 4096 and parts["nvme0n2"]["size"] == 8 * MiB
        # data inside partitions is the pattern at the partition's offset in the disk
        checks = [(f"{gpt}2", 6144 * 512), (f"{mb}2", 8192 * 512), ("nvme0n1p2", 8256 * 512), ("nvme0n2p2", 264 * 4096), (f"{gpt}1", 2048 * 512)]
        for dev, base in checks:
            got = g.sha_of(f"dd if=/dev/{dev} bs=4096 count=8 status=none | sha256sum")
            assert got == sha(pattern_slice(base, 8 * 4096)), f"/dev/{dev}: wrong data at the start of the partition"
            got = g.sha_of(f"dd if=/dev/{dev} bs=777 skip=3 count=9 status=none | sha256sum")
            assert got == sha(pattern_slice(base + 3 * 777, 9 * 777)), f"/dev/{dev}: wrong data at an unaligned offset"
        # partitions end where the table says: 8192-sector partition, 2 sectors left after skipping 8190
        assert g.count_bytes(f"dd if=/dev/{gpt}2 bs=512 skip=8190 count=10 status=none") == 1024
        assert g.count_bytes("dd if=/dev/nvme0n2p1 bs=4096 skip=255 count=10 status=none") == 4096
        out = g.ok("lsblk")
        must(out, rf"(?m)^{gpt}1\s+2\.0M\s+0\s+part\s+TOUHOU-ONE\s+ahci")
        must(out, r"(?m)^nvme0n2p2\s+3\.0M\s+0\s+part\s+FOURK-B\s+nvme")


# -------------------------------------------------------------------------------------------------- concurrency

@test
def blk_parallel():
    """Several dd processes reading and writing SATA and NVMe disks at the same time stay correct."""
    a_sata = pattern_image("par-sata", 6)
    a_nvme = pattern_image("par-nvme", 6)
    w_sata = pattern_image("par-wsata", 6, pat_b)
    w_nvme = pattern_image("par-wnvme", 6, pat_b)
    extra = sata_disk("d0", a_sata, "ide.1", model="PAR-SATA") + sata_disk("d1", w_sata, "ide.3", model="PAR-WSATA") + \
        nvme_ctrl("np", "PAR001", [(a_nvme, 1), (w_nvme, 2)])
    srcdata = pattern_slice(0, SIZE6)
    with Guest("t-blk-parallel", extra) as g:
        parts = g.partitions()
        rsata, wsata = device_by_model(parts, "PAR-SATA"), device_by_model(parts, "PAR-WSATA")
        script = (
            f"dd if=/dev/{rsata} of=/tmp/p1 bs=64k count=24 status=none & "
            f"dd if=/dev/{rsata} of=/tmp/p2 bs=64k skip=24 count=24 status=none & "
            f"dd if=/dev/nvme0n1 of=/tmp/p3 bs=64k skip=48 count=24 status=none & "
            f"dd if=/dev/nvme0n1 of=/tmp/p4 bs=4k skip=1000 count=100 status=none & "
            f"dd if=/dev/{rsata} of=/dev/{wsata} bs=64k skip=60 seek=60 count=16 conv=notrunc status=none & "
            f"dd if=/dev/nvme0n1 of=/dev/nvme0n2 bs=512 skip=2000 seek=2000 count=2000 conv=notrunc status=none & "
            f"wait")
        g.ok(script, timeout=120)
        want = {"p1": srcdata[0:24 * 65536], "p2": srcdata[24 * 65536:48 * 65536], "p3": srcdata[48 * 65536:72 * 65536], "p4": srcdata[4096000:4096000 + 409600]}
        out = g.ok("sha256sum /tmp/p1 /tmp/p2 /tmp/p3 /tmp/p4")
        for name, data in want.items():
            must(out, rf"{sha(data)}\s+/tmp/{name}", f"(parallel reader {name} got the wrong data)")
        g.ok("sync")
    exp_s = expected_copy(pattern_slice(0, SIZE6, pat_b), srcdata, [(60 * 65536, 60 * 65536, 16 * 65536)])
    exp_n = expected_copy(pattern_slice(0, SIZE6, pat_b), srcdata, [(2000 * 512, 2000 * 512, 2000 * 512)])
    assert w_sata.read_bytes() == exp_s and w_nvme.read_bytes() == exp_n, "parallel writers corrupted a destination image"


# ---------------------------------------------------------------------------------------------- error handling

@test
def blk_sata_errors():
    """AHCI: failing reads and writes give EIO, the link is reset and the disk keeps working; nothing is written by a failed write."""
    src = pattern_image("err-sata", 4)
    extra = sata_disk("e0", src, "ide.1", model="FLAKY-SATA", read_error=100, write_error=200)
    data = pattern_slice(0, 4 * MiB)
    with Guest("t-blk-sata-err", extra) as g:
        dev = device_by_model(g.partitions(), "FLAKY-SATA")
        assert g.sha_of(f"dd if=/dev/{dev} bs=512 skip=90 count=5 status=none | sha256sum") == sha(data[90 * 512:95 * 512])
        out, rc = g.run(f"dd if=/dev/{dev} of=/dev/null bs=512 skip=100 count=1")
        must(out, r"error reading '/dev/\w+': Input/output error")
        must(out, r"ahci0 port 1: command 25 \(lba 100, 1 sectors\) failed: device reported an error")
        must(out, r"ahci0 port 1: link reset done, disk is back")
        # the disk still works, before and after the bad sector, with different request shapes
        assert g.sha_of(f"dd if=/dev/{dev} bs=512 skip=101 count=40 status=none | sha256sum") == sha(data[101 * 512:141 * 512])
        out, rc = g.run(f"dd if=/dev/{dev} of=/dev/null bs=64k count=1")                 # 128 sectors, the bad one is inside
        must(out, r"Input/output error")
        # writes: the injected sector fails, the neighbours do not, and nothing is written for the failing request
        out, rc = g.run(f"dd if=/dev/zero of=/dev/{dev} bs=512 seek=200 count=1 conv=notrunc")
        must(out, r"error writing '/dev/\w+': Input/output error")
        g.ok(f"dd if=/dev/zero of=/dev/{dev} bs=512 seek=201 count=2 conv=notrunc status=none")
        g.ok(f"dd if=/dev/zero of=/dev/{dev} bs=512 seek=198 count=2 conv=notrunc status=none")
        stats = must(g.ok("cat /proc/ahci"), r"port 1 \w+ \"FLAKY-SATA\".*cmds \d+ errors (\d+) resets (\d+) flushes \d+ bounced \d+(\s+DEAD)?")
        assert int(stats.group(1)) >= 2 and int(stats.group(2)) >= 2 and not stats.group(3), stats.group(0)
        g.ok("sync")
    img = src.read_bytes()
    assert img[198 * 512:200 * 512] == bytes(1024) and img[200 * 512:201 * 512] == data[200 * 512:201 * 512], "failing write must not reach the image"
    assert img[201 * 512:203 * 512] == bytes(1024) and img[:198 * 512] == data[:198 * 512] and img[203 * 512:] == data[203 * 512:]


@test
def blk_sata_timeout():
    """AHCI: a command that never completes is abandoned after the timeout, the port is reset and later I/O is correct."""
    src = pattern_image("to-sata", 4)
    data = pattern_slice(0, 4 * MiB)
    extra = sata_disk("t0", src, "ide.1", model="TIMEOUT-SATA")
    with Guest("t-blk-sata-timeout", extra, cmdline="ahci.iotimeout=500 ahci.dropcmd=6") as g:
        dev = device_by_model(g.partitions(), "TIMEOUT-SATA")
        for i in range(4):          # the lost command lands in the boot-time partition scan or in one of these; either way it must heal
            g.run(f"dd if=/dev/{dev} of=/dev/null bs=4096 skip={i * 8} count=8")
        text = g.log()
        must(text, r"ahci0 port 1: command 25 \(lba \d+, \d+ sectors\) failed: timeout")
        must(text, r"ahci0 port 1: link reset done, disk is back")
        assert g.sha_of(f"sha256sum /dev/{dev}") == sha(data), "the disk must read back correctly after the recovery"
        stats = must(g.ok("cat /proc/ahci"), r"port 1 \w+ \"TIMEOUT-SATA\".*errors (\d+) resets (\d+) flushes \d+ bounced \d+(\s+DEAD)?")
        assert int(stats.group(2)) >= 1 and not stats.group(3), stats.group(0)


@test
def blk_nvme_errors():
    """NVMe: a failing block reports status codes and gives EIO, the queue keeps working afterwards; writes fail the same way."""
    src = pattern_image("err-nvme", 4)
    data = pattern_slice(0, 4 * MiB)
    extra = nvme_ctrl("ne", "FLAKY01", [(src, 1, {}, {"read_error": 100, "write_error": 200})])
    with Guest("t-blk-nvme-err", extra) as g:
        assert g.sha_of("dd if=/dev/nvme0n1 bs=512 skip=90 count=5 status=none | sha256sum") == sha(data[90 * 512:95 * 512])
        out, rc = g.run("dd if=/dev/nvme0n1 of=/dev/null bs=512 skip=100 count=1")
        must(out, r"error reading '/dev/nvme0n1': Input/output error")
        must(out, r"nvme0: read of 1 blocks at lba 100 on nsid 1 failed: status type 2 code 0x81")
        # a big request that covers the bad block fails as a whole; neighbours are still fine
        out, rc = g.run("dd if=/dev/nvme0n1 of=/dev/null bs=64k count=1")
        must(out, r"Input/output error")
        assert g.sha_of("dd if=/dev/nvme0n1 bs=512 skip=101 count=300 status=none | sha256sum") == sha(data[101 * 512:401 * 512])
        out, rc = g.run("dd if=/dev/zero of=/dev/nvme0n1 bs=512 seek=200 count=1 conv=notrunc")
        must(out, r"error writing '/dev/nvme0n1': Input/output error")
        must(out, r"nvme0: write of 1 blocks at lba 200 on nsid 1 failed: status type \d code 0x[0-9a-f]+")
        g.ok("dd if=/dev/zero of=/dev/nvme0n1 bs=512 seek=201 count=2 conv=notrunc status=none")
        stats = must(g.ok("cat /proc/nvme"), r"cmds \d+ errors (\d+) flushes \d+ bounced \d+(\s+DEAD)?")
        assert int(stats.group(1)) >= 3 and not stats.group(2), stats.group(0)
        g.ok("sync")
    img = src.read_bytes()
    assert img[200 * 512:201 * 512] == data[200 * 512:201 * 512] and img[201 * 512:203 * 512] == bytes(1024)
    assert img[:200 * 512] == data[:200 * 512] and img[203 * 512:] == data[203 * 512:]


@test
def blk_nvme_timeout():
    """NVMe: a command that never completes makes the controller give up cleanly; later requests fail fast and other disks are fine."""
    nsrc = pattern_image("to-nvme", 4)
    ssrc = pattern_image("to-nvme-sata", 4)
    sdata = pattern_slice(0, 4 * MiB)
    extra = sata_disk("t0", ssrc, "ide.1", model="BYSTANDER") + nvme_ctrl("nt", "TIMEOUT1", [(nsrc, 1)])
    with Guest("t-blk-nvme-timeout", extra, cmdline="nvme.iotimeout=500 nvme.dropcmd=3") as g:
        sata = device_by_model(g.partitions(), "BYSTANDER")
        for i in range(3):
            g.run(f"dd if=/dev/nvme0n1 of=/dev/null bs=4096 skip={i * 4} count=4 status=none")
        text = g.log()
        must(text, r"nvme0: I/O command timed out \(CSTS 0x[0-9a-f]+\); giving up on this controller")
        t0 = time.time()
        out, rc = g.run("dd if=/dev/nvme0n1 of=/dev/null bs=4096 count=1")
        assert time.time() - t0 < 6, "a dead controller must fail requests immediately, not wait again"
        must(out, r"Input/output error")
        must(g.ok("cat /proc/nvme"), r"nvme0 .*errors \d+ flushes \d+ bounced \d+\s+DEAD")
        assert g.sha_of(f"sha256sum /dev/{sata}") == sha(sdata), "the SATA disk must be unaffected by the dead NVMe controller"
        assert g.ok("echo still-alive").strip() == "still-alive"


# ------------------------------------------------------------------------------------------------------- topology

@test
def blk_topology():
    """Two AHCI controllers (one with a 512e disk), two NVMe controllers, namespaces 1 and 3, a metadata namespace that is skipped, 4096-byte blocks."""
    s_main = pattern_image("topo-s1", 4)
    s_512e = pattern_image("topo-s2", 4)
    n1 = pattern_image("topo-n1", 4)
    n3 = pattern_image("topo-n3", 8)
    nm = pattern_image("topo-nm", 4)
    extra = (["-device", "ich9-ahci,id=ahci1"] + sata_disk("a0", s_main, "ide.1", model="MAIN-CTRL-DISK") +
             sata_disk("a1", s_512e, "ahci1.2", model="SECOND-CTRL-512E", physical_block_size=4096) +
             nvme_ctrl("c0", "CTRL0", [(n1, 1), (n3, 3, {"logical_block_size": 4096, "physical_block_size": 4096})]) +
             nvme_ctrl("c1", "CTRL1", [(nm, 1, {"ms": 8})]))
    with Guest("t-blk-topology", extra) as g:
        log = g.log()
        must(log, r"ahci0: 8086:2922 at 00:02\.0")                     # the extra controller enumerates first (lower PCI address)
        must(log, r"ahci1: 8086:2922 at 00:1f\.2")
        must(log, r'ahci0 port 2: [\d.]+ Gbps disk link, "SECOND-CTRL-512E"')
        must(log, r"ahci0 port 2: 4 MiB, 8192 sectors of 512 bytes \(512e: larger physical sectors\)")
        must(log, r'nvme0: "QEMU NVMe Ctrl" serial CTRL0')
        must(log, r"nvme0: nsid 3: 8 MiB, 2048 blocks of 4096 bytes")
        must(log, r'nvme1: "QEMU NVMe Ctrl" serial CTRL1')
        must(log, r"nvme1: nsid 1 is formatted with 8 bytes of metadata per block -- not supported, skipped")
        must(log, r"nvme1: no usable namespace")
        parts = g.partitions()
        by_model = {m: device_by_model(parts, m) for m in ("MAIN-CTRL-DISK", "SECOND-CTRL-512E")}
        assert by_model["SECOND-CTRL-512E"] == "sda" and by_model["MAIN-CTRL-DISK"] == "sdc", by_model       # PCI order decides the letters
        assert "nvme0n1" in parts and "nvme0n3" in parts and "nvme0n2" not in parts, sorted(parts)
        assert not any(n.startswith("nvme1") for n in parts), "a namespace with metadata must not be exposed"
        assert parts["nvme0n3"]["ss"] == 4096 and parts["nvme0n3"]["size"] == 8 * MiB and parts["nvme0n1"]["ss"] == 512
        # data on each device, including the 4096-byte-block namespace and the 512e disk
        for dev, size in (("sda", 4 * MiB), ("sdc", 4 * MiB), ("nvme0n1", 4 * MiB), ("nvme0n3", 8 * MiB)):
            assert g.sha_of(f"sha256sum /dev/{dev}") == sha(pattern_slice(0, size)), f"/dev/{dev} reads back the wrong data"
        # byte ranges that straddle 4096-byte blocks
        for bs, skip, count in ((1000, 3, 9), (4097, 1, 3), (100, 40, 100)):
            got = g.sha_of(f"dd if=/dev/nvme0n3 bs={bs} skip={skip} count={count} status=none | sha256sum")
            assert got == sha(pattern_slice(bs * skip, bs * count)), f"nvme0n3 bs={bs} skip={skip}"
        must(g.ok("cat /proc/nvme"), r"nsid 3 nvme0n3 2048 x 4096")


# ----------------------------------------------------------------------------------------------- in-kernel checks

def selftest_case(label, cmdline, size_mib, timeout=300):
    """Boots one SATA disk and two NVMe namespaces (512 and 4096-byte blocks), runs the in-kernel I/O check on all three."""
    sata = pattern_image(f"st-{label}-sata", size_mib)
    n512 = pattern_image(f"st-{label}-n1", size_mib)
    n4k = pattern_image(f"st-{label}-n2", size_mib)
    extra = sata_disk("s0", sata, "ide.1", model="ST-SATA") + nvme_ctrl("ns", "ST0001", [(n512, 1), (n4k, 2, {"logical_block_size": 4096, "physical_block_size": 4096})])
    with Guest(f"t-blk-st-{label}", extra, cmdline=f"blkselftest=sdb:rw,nvme0n1:rw,nvme0n2:rw {cmdline}", boot_timeout=timeout) as g:
        text = g.log()
        for dev in ("sdb", "nvme0n1", "nvme0n2"):
            m = must(text, rf"selftest {dev}: (PASS|FAIL) \((\d+) problems\), (\d+) KiB requests, read ([\d.]+) MB/s, write ([\d.]+) MB/s")
            assert m.group(1) == "PASS" and m.group(2) == "0", f"{dev}: {m.group(0)}\n{text[-1500:]}"
        if "hbareset" in cmdline:
            must(text, r"ahci0: resetting the controller \(requested on the command line\)")
        assert "FAIL" not in text.split("Welcome")[0], text[-1500:]


@test
def blk_selftest_default():
    """In-kernel check (16 MiB requests, 40 MiB images): reads and writes with many request sizes and buffer alignments."""
    selftest_case("def", "", 40)


@test
def blk_selftest_split():
    """In-kernel check with tiny commands: 7-sector AHCI commands with 1000-byte PRDs, 4 KiB NVMe commands, one in flight."""
    selftest_case("split", "ahci.maxsect=7 ahci.prdlen=1000 ahci.prdmax=8 nvme.window=1 nvme.maxxfer=4096", 4)


@test
def blk_selftest_exhaust():
    """In-kernel check with a PRD table that runs out in mid-sector and an NVMe queue that wraps every few commands."""
    selftest_case("exh", "ahci.prdmax=3 ahci.prdlen=1000 nvme.depth=4 nvme.window=3 nvme.maxxfer=12288", 4)


@test
def blk_selftest_bounce():
    """In-kernel check with every transfer bounced, tiny commands, a two-entry PRD table and a three-entry NVMe queue."""
    selftest_case("bnc", "ahci.bounce ahci.maxsect=5 ahci.prdlen=700 ahci.prdmax=2 nvme.bounce nvme.maxxfer=8192 nvme.depth=3", 2)


@test
def blk_selftest_hba_reset():
    """In-kernel check after an explicit AHCI controller reset (the path used when a command engine will not stop)."""
    selftest_case("rst", "ahci.hbareset", 8)


@test
def blk_nvme_mdts():
    """NVMe with a 8 KiB MDTS limit: the driver honours the controller's maximum transfer size."""
    img = pattern_image("mdts", 8)
    extra = nvme_ctrl("nm", "MDTS01", [(img, 1)], mdts=1)
    with Guest("t-blk-nvme-mdts", extra, cmdline="blkselftest=nvme0n1") as g:
        text = g.log()
        must(text, r"max transfer 8 KiB per command")
        must(text, r"selftest nvme0n1: PASS \(0 problems\), 8192 KiB requests")


@test
def blk_readonly_switch():
    """'blk.ro' on the kernel command line makes every SATA and NVMe disk (and its partitions) read-only: writes fail, images stay untouched."""
    ssd = gpt_image("ro-sata", 4, [(64, 4000, "RO-ONE")])
    nvm = pattern_image("ro-nvme", 4)
    before = (ssd.read_bytes(), nvm.read_bytes())
    extra = sata_disk("s0", ssd, "ide.1", model="RO-SATA") + nvme_ctrl("ro", "RO0001", [(nvm, 1)])
    with Guest("t-blk-readonly", extra, cmdline="blk.ro") as g:
        parts = g.partitions()
        sdb = device_by_model(parts, "RO-SATA")
        for name in (sdb, f"{sdb}1", "nvme0n1"):
            assert parts[name]["ro"] == 1, f"{name} should be read-only: {parts[name]}"
        must(g.ok("lsblk"), rf"(?m)^{sdb}\s+4\.0M\s+1\s+disk")
        for dev in (sdb, f"{sdb}1", "nvme0n1"):
            out, rc = g.run(f"dd if=/dev/zero of=/dev/{dev} bs=512 count=4 seek=100 conv=notrunc")
            must(out, r"error writing", f"(a write to read-only /dev/{dev} must fail)")
        assert g.sha_of(f"dd if=/dev/nvme0n1 bs=512 skip=2 count=4 status=none | sha256sum") == sha(pattern_slice(1024, 2048)), "reads still work"
        stats = g.ok("cat /proc/ahci") + g.ok("cat /proc/nvme")
        assert "flushes 0" in stats, "nothing was written, so nothing was flushed"
    assert (ssd.read_bytes(), nvm.read_bytes()) == before, "a read-only disk was modified"


@test
def blk_dma_below_4g():
    """Caller memory the controllers may not reach (above the 'blk.dmalimit_mb' test limit, standing in for 4 GiB) is bounced; data stays correct."""
    sata = pattern_image("lim-sata", 8)
    n512 = pattern_image("lim-n1", 8)
    n4k = pattern_image("lim-n2", 8)
    extra = sata_disk("s0", sata, "ide.1", model="LIM-SATA") + nvme_ctrl("lm", "LIM001", [(n512, 1), (n4k, 2, {"logical_block_size": 4096, "physical_block_size": 4096})])
    with Guest("t-blk-dmalimit", extra, cmdline="blk.dmalimit_mb=48 blkselftest=sdb:rw,nvme0n1:rw,nvme0n2:rw") as g:
        text = g.log()
        for dev in ("sdb", "nvme0n1", "nvme0n2"):
            must(text, rf"selftest {dev}: PASS \(0 problems\)")
        out = g.ok("cat /proc/ahci") + g.ok("cat /proc/nvme")
        bounced = [int(x) for x in re.findall(r"bounced (\d+)", out)]
        assert any(b > 0 for b in bounced), f"buffers above the limit must have been bounced: {out}"


# ------------------------------------------------------------------------------- the rest of the system, with and without

@test
def blk_system_with_devices():
    """The in-kernel self test, the user-space suite and the shell checks all pass with SATA and NVMe disks attached."""
    sata = pattern_image("sys-sata", 4)
    nv = pattern_image("sys-nvme", 4)
    extra = sata_disk("s0", sata, "ide.1", model="SYS-SATA") + nvme_ctrl("sy", "SYS001", [(nv, 1)])
    for cmdline, marker, name in (("selftest", r"SELFTEST (PASS|FAIL)", "selftest"), ("utest", r"UTEST (PASS|FAIL)", "utest")):
        with Qemu(cmdline=("text " + cmdline).strip(), name=f"t-blk-sys-{name}", extra=extra) as vm:
            vm.expect(marker, timeout=90)
            assert f"{name.upper()} PASS" in vm.text(), vm.text()[-1500:]
            assert vm.wait_exit(10) == 0
    import test_shell
    failures = test_shell.run_checks(verbose=False, extra=extra)
    assert not failures, f"{len(failures)} shell check(s) failed with block devices attached:\n" + "\n".join(failures[:6])


@test
def blk_throughput():
    """Informational: MB/s for 16 MiB reads and writes through /dev (64 KiB requests) and through the driver (16 MiB requests)."""
    ssd = pattern_image("tp-sata", 20)
    nvm = pattern_image("tp-nvme", 20)
    extra = sata_disk("s0", ssd, "ide.1", model="TP-SATA") + nvme_ctrl("tp", "TP0001", [(nvm, 1)])
    with Guest("t-blk-throughput", extra, cmdline="blkselftest=sdb:rw,nvme0n1:rw") as g:
        results = []
        for dev in ("sdb", "nvme0n1"):
            for label, cmd in (("read ", f"dd if=/dev/{dev} of=/dev/null bs=1M count=16"),
                               ("write", f"dd if=/dev/zero of=/dev/{dev} bs=1M count=16 conv=notrunc")):
                t0 = time.time()
                out = g.ok(cmd, timeout=120)
                wall = time.time() - t0
                m = re.search(r"([\d.]+) MB/s", out)
                results.append(f"{dev:8} {label} 16 MiB via dd bs=1M: guest clock {m.group(1) if m else '?'} MB/s, host wall clock {16 / max(wall, 0.001):.1f} MB/s (incl. shell)")
        text = g.log()
        for dev in ("sdb", "nvme0n1"):
            m = must(text, rf"selftest {dev}: PASS .*?, (\d+) KiB requests, read ([\d.]+) MB/s, write ([\d.]+) MB/s")
            results.append(f"{dev:8} in-kernel {m.group(1)} KiB requests: read {m.group(2)} MB/s, write {m.group(3)} MB/s (guest clock)")
    print("\n     " + "\n     ".join(results), file=sys.stderr)
