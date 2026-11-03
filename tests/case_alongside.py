"""install --alongside: TouhouOS added in the free space of a disk that keeps what it has (the Dell's Windows disk, shrunk by its owner).

The disks are Windows-like (ESP with a decoy boot manager, MSR, an NTFS or BitLocker C: with its boot sector marked, free space at the end). After
the installer the host compares the image with the original: every byte outside the new partitions and the GPT structures must be identical, the
GPT must be sound with exactly two new entries and unchanged headers (but for the checksums), and the disk must boot alone to the desktop from the
new ESP, which is partition 4 here. The loader's "bootentry" / "findinstalled" words are tested against OVMF's variable store. docs/INSTALL.md.
"""
import re
import struct
import subprocess
import sys
import uuid
import zlib
from pathlib import Path

from harness import test, Qemu, BUILD
from case_blockdev import Guest, nvme_ctrl, sata_disk, must, device_by_model, PROMPT
from case_install import (blank, vbr, poke, sha_file, boot_installed, GPT_MSR, INSTALLED_CMDLINE, MiB, IMG_DIR)
import mkdisk

ROOT = Path(__file__).resolve().parent.parent


def windows_gap_disk(name, size_mib, gap_mib, bitlocker=False, ss=512):
    """ESP (40 MiB, decoy bootmgfw.efi), MSR (16 MiB), C: (NTFS or BitLocker) and gap_mib of free space at the end."""
    p = blank(name, size_mib)
    c_mib = size_mib - 1 - 40 - 16 - gap_mib - 1
    assert c_mib > 10, "the disk is too small for the gap"
    esp = (dict(name="EFI system partition", kind="fat32", size=40 * MiB, label="SYSTEM", esp=True, files={"/EFI/Microsoft/Boot/bootmgfw.efi": b"MZ not really windows " * 64})
           if ss == 512 else dict(name="EFI system partition", kind="raw", size=40 * MiB, esp=True))          # (FAT32 needs 258 MiB on 4096-byte sectors)
    mkdisk.build_image(str(p), size_mib * MiB, [
        esp,
        dict(name="Microsoft reserved partition", kind="raw", size=16 * MiB, type=GPT_MSR),
        dict(name="Basic data partition", kind="raw", size=c_mib * MiB, type=mkdisk.GPT_TYPE_BASIC)], sector_size=ss, seed=33)
    _, parts = mkdisk.partitions(str(p))
    poke(p, parts[2]["offset"], vbr(b"-FVE-FS-" if bitlocker else b"NTFS    "))
    return p


def read_gpt(path, ss):
    """The primary and the backup table as raw bytes plus the numbers that matter."""
    size = Path(path).stat().st_size
    with open(path, "rb") as f:
        f.seek(ss)
        prim = f.read(ss)
        f.seek(size - ss)
        back = f.read(ss)
        entries_lba = struct.unpack_from("<Q", prim, 72)[0]
        nent, esize = struct.unpack_from("<II", prim, 80)
        f.seek(entries_lba * ss)
        pent = f.read(nent * esize)
        bent_lba = struct.unpack_from("<Q", back, 72)[0]
        f.seek(bent_lba * ss)
        bent = f.read(nent * esize)
        f.seek(0)
        mbr = f.read(ss)
    return dict(prim=prim, back=back, pent=pent, bent=bent, nent=nent, esize=esize, entries_lba=entries_lba, bent_lba=bent_lba, mbr=mbr, size=size, ss=ss)


def entry(g, i):
    e = g["pent"][i * g["esize"]:(i + 1) * g["esize"]]
    if not any(e[:16]):
        return None
    first, last = struct.unpack_from("<QQ", e, 32)
    name = e[56:128].decode("utf-16-le").rstrip("\0")
    return dict(type=uuid.UUID(bytes_le=e[:16]), unique=uuid.UUID(bytes_le=e[16:32]), first=first, last=last, name=name, raw=e)


def check_alongside(orig_copy, now, ss, nold=3):
    """Everything that must hold after `install --alongside`: returns the GPT entries of the new partitions."""
    a, b = read_gpt(orig_copy, ss), read_gpt(now, ss)
    # the table: sound (mkdisk's own checker), same shape, old entries untouched, two new ones
    out = subprocess.run([sys.executable, str(ROOT / "tools" / "mkdisk.py"), "--check", str(now)], capture_output=True, text=True)
    assert out.returncode == 0 and "0 error(s)" in out.stdout, f"mkdisk --check:\n{out.stdout}{out.stderr}"
    assert (a["nent"], a["esize"], a["entries_lba"], a["bent_lba"]) == (b["nent"], b["esize"], b["entries_lba"], b["bent_lba"])
    assert b["pent"] == b["bent"], "the two copies of the entry array differ"
    for i in range(nold):
        assert a["pent"][i * a["esize"]:(i + 1) * a["esize"]] == b["pent"][i * b["esize"]:(i + 1) * b["esize"]], f"existing partition {i + 1} changed"
    new = [entry(b, i) for i in range(nold, b["nent"]) if entry(b, i)]
    assert [e["name"] for e in new] == ["TOUHOUOS-ESP", "HUMANVILLAGE"], new
    assert entry(a, nold) is None
    assert new[0]["type"] == mkdisk.GPT_TYPE_ESP and new[1]["type"] == mkdisk.GPT_TYPE_BASIC, new
    # the headers: every field but the two checksums is as it was
    for tag in ("prim", "back"):
        x, y = bytearray(a[tag]), bytearray(b[tag])
        for buf in (x, y):
            buf[16:20] = b"\0\0\0\0"
            buf[88:92] = b"\0\0\0\0"
        assert x == y, f"the {tag} GPT header changed in more than its checksums"
    assert a["mbr"] == b["mbr"], "the protective MBR changed"
    # nothing outside the new partitions and the GPT structures differs
    asec = (a["nent"] * a["esize"] + ss - 1) // ss
    total = a["size"] // ss
    allowed = sorted([(new[0]["first"] * ss, (new[1]["last"] + 1) * ss), (ss, 2 * ss), (a["entries_lba"] * ss, (a["entries_lba"] + asec) * ss),
                      (a["bent_lba"] * ss, (a["bent_lba"] + asec) * ss), ((total - 1) * ss, total * ss)])
    rngs, cur = [], 0
    for lo, hi in allowed:
        if lo > cur:
            rngs.append((cur, lo))
        cur = max(cur, hi)
    if cur < a["size"]:
        rngs.append((cur, a["size"]))
    with open(orig_copy, "rb") as fa, open(now, "rb") as fb:
        for lo, hi in rngs:
            pos = lo
            while pos < hi:
                n = min(MiB, hi - pos)
                fa.seek(pos)
                fb.seek(pos)
                assert fa.read(n) == fb.read(n), f"bytes changed outside the new partitions and the GPT, in {pos}..{pos + n}"
                pos += n
    # the new ESP holds the boot files; HUMANVILLAGE is a FAT32 volume of the right name
    esp = mkdisk.open_volume(str(now), nold + 1)
    try:
        for name in ("EFI/BOOT/BOOTX64.EFI", "kernel.elf", "initrd.img"):
            assert esp.read_file("/" + name) == (BUILD / "esp" / name).read_bytes(), f"{name} on the new ESP differs from build/esp"
        cmd = esp.read_file("/cmdline.txt").decode().strip()
    finally:
        esp.close()
    hv = mkdisk.open_volume(str(now), nold + 2)
    try:
        assert hv.bits == 32 and hv.label == "HUMANVILLAG", (hv.bits, hv.label)
    finally:
        hv.close()
    assert cmd.startswith(INSTALLED_CMDLINE + " blk.rw=" + str(new[1]["unique"])) and cmd.endswith(" bootentry"), cmd
    return new


def copy_of(p):
    q = p.with_name(p.stem + "-orig.img")
    q.write_bytes(p.read_bytes())
    return q


@test
def install_alongside_windows():
    """A Windows-like NVMe disk (ESP, MSR, NTFS C:, 300 MiB free at the end): `install --alongside` lists what stays, shows the plan, asks for the disk's name, adds
    partitions 4 and 5 in the free space and changes nothing else (checked byte for byte), and the disk boots to the desktop from partition 4."""
    disk = windows_gap_disk("alongside-win", 640, 300, bitlocker=True)
    orig = copy_of(disk)
    with Guest("t-alongside", nvme_ctrl("wn", "WINNVME1", [(disk, 1)]), cmdline="blk.ro") as g:
        listing = g.ok("install")
        must(listing, r"(?m)^nvme0n1\s.*holds WINDOWS \(protected")
        out, rc = g.run("install --alongside --force nvme0n1")
        assert rc == 1 and "erases nothing" in out and "nothing was written" in out, out
        out = g.ok(f"install --alongside --dry-run --cmdline '{INSTALLED_CMDLINE}' nvme0n1")
        must(out, r"NOTHING THAT IS ON THE DISK NOW IS CHANGED")
        must(out, r"It stays exactly as it is:")
        must(out, r"BitLocker-encrypted volume\s+<- WINDOWS")
        must(out, r"NOTE: nvme0n1 holds a BitLocker volume")
        must(out, r"partition 4  TOUHOUOS-ESP  64 MiB")
        must(out, r"partition 5  HUMANVILLAGE  \d+ MiB")
        must(out, r"dry-run: stopping here, nothing was written")
        assert sha_file(disk) == sha_file(orig), "--dry-run wrote something"
        g.vm.send(f"install --alongside --cmdline '{INSTALLED_CMDLINE}' nvme0n1\n")
        g.vm.expect(r"Type the disk's name \(nvme0n1\) to go on", timeout=120)
        g.vm.send("nvme0n1\n")
        g.vm.expect(r"TouhouOS is installed on nvme0n1", timeout=900)
        g.vm.expect(PROMPT, timeout=60)
        text = g.vm.text()
        must(text, r"checking that nothing that was on the disk before has changed")
        must(text, r"everything that was on nvme0n1 is as it was")
    new = check_alongside(orig, disk, 512)
    assert new[0]["first"] % 2048 == 0 and new[1]["first"] % 2048 == 0, "1 MiB alignment"
    for stale in BUILD.glob("t-alongside-boot-*"):
        stale.unlink()                                                  # a firmware variable store from an earlier run would hand out another Boot#### number
    log = boot_installed(disk, "nvme", "t-alongside-boot", part=4)
    must(log, r"loader: bootentry: Boot0020 \"TouhouOS\" added at the end of BootOrder \(\d+ entries\); read back: ok")


@test
def install_alongside_bootentry_once():
    """The installed system's loader (word `bootentry`) adds its boot menu entry the first time and says the entry exists the second time: the same OVMF variable
    store is used for both starts."""
    disk = windows_gap_disk("alongside-entry", 640, 300)
    with Guest("t-alongside-e", nvme_ctrl("wn", "WINNVME2", [(disk, 1)]), cmdline="blk.ro") as g:
        g.ok(f"install --alongside --yes-i-typed=nvme0n1 --cmdline '{INSTALLED_CMDLINE}' nvme0n1", timeout=900)
    for stale in BUILD.glob("t-alongside-entry-*"):
        stale.unlink()
    for n in (1, 2):
        with Qemu(cmdline="", name="t-alongside-entry", mem="768M", boot_image=disk, boot_bus="nvme") as vm:
            vm.expect(r"opening the desktop for reimu", timeout=240)
            log = vm.text()
        if n == 1:
            must(log, r"loader: bootentry: Boot0020 \"TouhouOS\" added at the end of BootOrder \(\d+ entries\); read back: ok")
        else:
            must(log, r"loader: bootentry: Boot0020 is this partition's entry already")
            assert "added at the end of BootOrder" not in log, log[-1500:]


@test
def install_alongside_findinstalled():
    """From the stick (word `findinstalled` in its cmdline.txt) the loader finds the TouhouOS that was installed beside Windows on the internal disk and gives it a
    boot menu entry. The stick is a USB image that boots first; the installed disk is an NVMe drive."""
    disk = windows_gap_disk("alongside-find", 640, 300)
    with Guest("t-alongside-f1", nvme_ctrl("wn", "WINNVME3", [(disk, 1)]), cmdline="blk.ro") as g:
        g.ok(f"install --alongside --yes-i-typed=nvme0n1 --cmdline '{INSTALLED_CMDLINE}' nvme0n1", timeout=900)
    stick = BUILD / "t-alongside-stick.img"
    out = subprocess.run([sys.executable, str(ROOT / "tools" / "mkimage.py"), "--no-build", "--out", str(stick), "--size", "256M",
                          "--cmdline", "text user=root blk.ro findinstalled"], capture_output=True, text=True)
    assert out.returncode == 0, out.stdout[-800:] + out.stderr[-800:]
    for stale in BUILD.glob("t-alongside-f2-*"):
        stale.unlink()
    with Qemu(cmdline="", name="t-alongside-f2", mem="768M", boot_image=stick, boot_bus="usb", extra=nvme_ctrl("wn", "WINNVME3", [(disk, 1)])) as vm:
        vm.expect(r"loader: bootentry: Boot0020 \"TouhouOS\" added at the end of BootOrder", timeout=240)
        vm.expect(r"Gensokyo\] kernel", timeout=120)
        text = vm.text()
        must(text, r"loader: findinstalled: a TouhouOS boot partition on an internal disk \(partition 4\)")
        must(text, r"added at the end of BootOrder \(\d+ entries\); read back: ok")
        assert "findinstalled: no installed TouhouOS found" not in text


@test
def install_alongside_refusals():
    """Nothing is written when the disk has no room (full, or less than a boot partition and a FAT32 volume), when its GPT is damaged, when --force is given,
    or when it has no GPT at all; the messages say what to do. Every refused image is byte for byte what it was."""
    full = windows_gap_disk("alongside-full", 400, 0)
    small = windows_gap_disk("alongside-small", 400, 60)
    damaged = windows_gap_disk("alongside-damaged", 400, 200)
    g0 = read_gpt(damaged, 512)
    for lba in (g0["entries_lba"], g0["bent_lba"]):                 # the same byte in both copies of the array: no checksum matches any more (with one good copy the
        poke(damaged, lba * 512 + 128 + 5, b"\x99")                 # firmware would repair the table before the OS starts)
    mbr_only = blank("alongside-mbr", 400)
    mkdisk.build_image(str(mbr_only), 400 * MiB, [dict(name="DATA", kind="fat32", size=100 * MiB, label="DATA")], table="mbr")
    imgs = [full, small, damaged, mbr_only]
    before = {p: sha_file(p) for p in imgs}
    extra = (nvme_ctrl("n1", "FULL0001", [(full, 1)]) + nvme_ctrl("n2", "SMALL001", [(small, 1)]) + nvme_ctrl("n3", "DAMAGED1", [(damaged, 1)]) +
             nvme_ctrl("n4", "MBRONLY1", [(mbr_only, 1)]))
    with Guest("t-alongside-r", extra, cmdline="blk.ro") as g:
        def refused(dev, pattern):
            out, rc = g.run(f"install --alongside --yes-i-typed={dev} {dev}", timeout=120)
            assert rc == 1 and "nothing was written" in out, f"{dev}: {out}"
            must(out, pattern, f"(from {dev})")
        refused("nvme0n1", r"largest free space on the disk is only")
        refused("nvme1n1", r"largest free space on the disk is only 6\d MiB")
        refused("nvme2n1", r"checksum of a partition entry array is wrong")
        refused("nvme3n1", r"no primary GPT header")
        out, rc = g.run("install --alongside --erase-phrase=x nvme0n1")
        assert rc == 1 and "erases nothing" in out, out
    for p in imgs:
        assert sha_file(p) == before[p], f"{p.name} was modified by a refused install"


@test
def install_alongside_4k():
    """A 4096-byte-sector NVMe namespace with Windows-like partitions and room: the 258 MiB boot partition and the FAT32 volume fit in the gap, the checks hold."""
    disk = windows_gap_disk("alongside-4k", 1280, 700, ss=4096)
    orig = copy_of(disk)
    ns = (disk, 1, {"logical_block_size": 4096, "physical_block_size": 4096})
    with Guest("t-alongside-4k", nvme_ctrl("w4", "WIN4K001", [ns]), cmdline="blk.ro") as g:
        out = g.ok(f"install --alongside --yes-i-typed=nvme0n1 --cmdline '{INSTALLED_CMDLINE}' nvme0n1", timeout=900)
        assert "4096-byte sectors" in out and "TouhouOS is installed" in out, out
    new = check_alongside(orig, disk, 4096)
    assert (new[0]["last"] - new[0]["first"] + 1) * 4096 == 258 * MiB, new[0]
