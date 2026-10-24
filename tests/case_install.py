"""The installer: `install DISK` puts TouhouOS on a SATA, NVMe or virtio disk; the disk then boots on its own, with no stick and no fat: ESP.

Every test boots the usual way (the fat: ESP is the boot disk, sda), with `blk.ro` and a root shell on the serial console, and attaches
target and decoy disks. After the VM is gone the host checks the images: the target with the independent checker (`tools/mkdisk.py
--check`), its partitions against the layout mkdisk itself builds, its files against build/esp; the decoys byte for byte. The installed
image is then booted alone (Qemu(boot_image=..., boot_bus=...)) up to reimu's desktop. docs/INSTALL.md describes the design.
"""
import hashlib
import re
import subprocess
import sys
import time
import uuid
from pathlib import Path

from harness import test, Qemu, BUILD, scaled
from case_blockdev import Guest, sata_disk, nvme_ctrl, drive, must, device_by_model
from guitools import START_BUTTON, SHRINE_RED, near
import mkdisk

ROOT = Path(__file__).resolve().parent.parent
MiB = 1 << 20
IMG_DIR = BUILD / "install"
INSTALLED_CMDLINE = "video=1024x768 blk.ro"


# ------------------------------------------------------------------------------------------------------------ images

def blank(name, size_mib):
    IMG_DIR.mkdir(parents=True, exist_ok=True)
    p = IMG_DIR / f"{name}.img"
    if p.exists():
        p.unlink()
    with open(p, "wb") as f:
        f.truncate(size_mib * MiB)
    return p


def other_os(name, size_mib, seed=7):
    """A disk that belongs to somebody else: GPT, an EFI partition with a decoy loader, a data partition with files."""
    p = blank(name, size_mib)
    mkdisk.build_image(str(p), size_mib * MiB, [
        dict(name="EFI system partition", kind="fat32", size=40 * MiB, label="SYSTEM", esp=True,
             files={"/EFI/Microsoft/Boot/bootmgfw.efi": b"MZ not really windows " * 64}),
        dict(name="Basic data partition", kind="fat32", size="rest", label="WINDOWS", files={"/Users/owner/diary.txt": b"keep out\n" * 300}),
    ], seed=seed)
    return p


def bare_fat(name, size_mib):
    """A whole-disk FAT32 volume without any partition table (as some cameras and old sticks have)."""
    p = blank(name, size_mib)
    with open(p, "r+b") as f:
        b = mkdisk.FatBuilder(mkdisk.Window(f, writable=True), 512, 32, label="CAMERA")
        b.add_file("/DCIM/IMG_0001.JPG", b"\xff\xd8 not a photo" * 100)
        b.finish()
    return p


def sha_file(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def check_installed(img):
    """Host-side checks of an installed disk; returns the GPT disk GUID (text)."""
    out = subprocess.run([sys.executable, str(ROOT / "tools" / "mkdisk.py"), "--check", str(img)], capture_output=True, text=True)
    assert out.returncode == 0 and "0 error(s), 0 warning(s)" in out.stdout, f"mkdisk --check {img.name}:\n{out.stdout}{out.stderr}"
    ss, parts = mkdisk.partitions(str(img))
    assert [p["name"] for p in parts] == ["TOUHOUOS-ESP", "HUMANVILLAGE"], parts
    assert parts[0]["type"] == mkdisk.GPT_TYPE_ESP and parts[1]["type"] == mkdisk.GPT_TYPE_BASIC, parts
    # the layout is the one mkdisk builds for an image of the same size (1 MiB alignment, 64 MiB ESP or 258 MiB on 4Kn, the rest)
    ref = IMG_DIR / "reference.img"
    esp_size = 258 * MiB if ss == 4096 else 64 * MiB
    mkdisk.build_image(str(ref), img.stat().st_size, [dict(name="TOUHOUOS-ESP", kind="raw", size=esp_size, esp=True),
                                                       dict(name="HUMANVILLAGE", kind="raw", size="rest")], sector_size=ss)
    _, want = mkdisk.partitions(str(ref))
    ref.unlink()
    assert [(p["first"], p["last"]) for p in parts] == [(p["first"], p["last"]) for p in want], (parts, want)
    esp = mkdisk.open_volume(str(img), 1)
    try:
        assert esp.bits == 32 and esp.label == "TOUHOUOS-ES", (esp.bits, esp.label)
        for name in ("EFI/BOOT/BOOTX64.EFI", "kernel.elf", "initrd.img"):
            assert esp.read_file("/" + name) == (BUILD / "esp" / name).read_bytes(), f"{name} on the installed ESP differs from build/esp"
        cmdline = esp.read_file("/cmdline.txt").decode()
    finally:
        esp.close()
    hv = mkdisk.open_volume(str(img), 2)
    try:
        assert hv.bits == 32 and hv.label == "HUMANVILLAG" and not hv.dirty, (hv.bits, hv.label)
    finally:
        hv.close()
    with open(img, "rb") as f:
        f.seek(ss)
        hdr = f.read(92)
    guid = str(uuid.UUID(bytes_le=hdr[56:72]))
    assert cmdline.strip() == f"{INSTALLED_CMDLINE} blk.rw={guid}", f"cmdline.txt {cmdline!r}, disk GUID {guid}"
    return guid


def boot_installed(img, bus, name, extra=(), want_rw=True):
    """Boots the installed image alone, as a PC would boot its disk, up to reimu's desktop. Returns the serial log."""
    with Qemu(cmdline="", name=name, mem="768M", boot_image=img, boot_bus=bus, extra=list(extra)) as vm:
        vm.expect(r"opening the desktop for reimu", timeout=240)
        deadline = time.time() + scaled(60)
        ok = False
        while time.time() < deadline and not ok:
            ok = near(vm.pixel(*START_BUTTON, vm.screen_ppm()), SHRINE_RED, 60)
            time.sleep(0.5)
        vm.screenshot(BUILD / f"{name}.png")
        log = vm.text()
        assert ok, "the desktop panel never appeared; serial tail:\n" + log[-1500:]
    must(log, r"loader: booted from partition 1 \(GPT, start \d+")
    must(log, re.escape(f"cmdline '{INSTALLED_CMDLINE} blk.rw="), "(the installed cmdline.txt)")
    m = must(log, r"\[Keine\] fat32: (\S+) mounted on /HumanVillage[^\n]*")
    assert "read-only" not in m.group(0), m.group(0)
    if want_rw:
        must(log, r"named by blk\.rw=: writable although blk\.ro is set")
    return log


def shell_wait(g, logfile, pattern, timeout=600):
    """Polls a log file the guest writes in the background until it matches."""
    deadline = time.time() + scaled(timeout)
    while True:
        out, _ = g.run(f"cat {logfile}", timeout=60)
        if re.search(pattern, out):
            return out
        assert time.time() < deadline, f"no /{pattern}/ in {logfile}:\n{out[-2000:]}"
        time.sleep(1)


# ------------------------------------------------------------------------------------------------------------- tests

@test
def install_sata_boots_desktop():
    """SATA under blk.ro: --force over another system's partitions, the unlock covers that one open file only (partition node, a second
    open, the other SATA disk and an NVMe drive all stay read-only meanwhile), then the disk boots alone to the desktop."""
    target = other_os("sata-target", 512, seed=3)
    decoy_s = other_os("sata-decoy", 128, seed=4)
    decoy_n = other_os("nvme-decoy", 128, seed=5)
    before = {p: sha_file(p) for p in (decoy_s, decoy_n)}
    extra = (sata_disk("tg", target, "ide.1", model="INST-TARGET") + sata_disk("dc", decoy_s, "ide.3", model="INST-DECOY") +
             nvme_ctrl("dn", "DECOY01", [(decoy_n, 1)]))
    with Guest("t-install-sata", extra, cmdline="blk.ro") as g:
        parts = g.partitions()
        sdx, decoy = device_by_model(parts, "INST-TARGET"), device_by_model(parts, "INST-DECOY")
        out, rc = g.run(f"install --yes-i-typed={sdx} --cmdline '{INSTALLED_CMDLINE}' {sdx}")
        assert rc == 1 and "is not empty" in out and "nothing was written" in out, out
        g.ok("rm -f /tmp/go")
        g.ok(f"install --force --yes-i-typed={sdx} --hold=/tmp/go --cmdline '{INSTALLED_CMDLINE}' {sdx} > /tmp/inst.log 2>&1 &")
        shell_wait(g, "/tmp/inst.log", r"holding the write unlock")
        # while the installer holds the unlock: nothing but its own open file may write
        for dev in (sdx, f"{sdx}1", f"{sdx}2", decoy, f"{decoy}2", "nvme0n1", "nvme0n1p2"):
            out, _ = g.run(f"dd if=/dev/zero of=/dev/{dev} bs=512 count=1 seek=40 conv=notrunc")
            must(out, r"error writing .*Read-only file system", f"(/dev/{dev} must stay read-only while {sdx} is unlocked for the installer)")
        assert g.partitions()[sdx]["ro"] == 1, "the disk itself stays registered read-only"
        must(g.ok("dmesg"), rf"blk: {sdx} unlocked for writing by install \(pid \d+\), through one open file only")
        g.ok("touch /tmp/go")
        log = shell_wait(g, "/tmp/inst.log", r"TouhouOS is installed|nothing was written|failed")
        assert "TouhouOS is installed on" in log and "--force: it holds partitions" in log, log
        must(g.ok("dmesg"), rf"blk: {sdx} is read-only again")
        out, _ = g.run(f"dd if=/dev/zero of=/dev/{sdx} bs=512 count=1 seek=40 conv=notrunc")
        must(out, r"Read-only file system", "(the unlock ended with the installer)")
    for p, h in before.items():
        assert sha_file(p) == h, f"{p.name} was modified"
    check_installed(target)
    log = boot_installed(target, "sata", "t-install-sata-boot", extra=sata_disk("dc", decoy_s, "ide.3", model="INST-DECOY"))
    must(log, r"\[Marisa\] blk: sd[a-z]+\s+ahci[^\n]*, read-only\s+INST-DECOY", "(the other disk stays read-only under blk.ro)")
    assert sha_file(decoy_s) == before[decoy_s], "the decoy was modified by the installed system"


@test
def install_nvme_boots_desktop():
    """NVMe under blk.ro, confirmed by typing the name at the terminal; the disk then boots alone to the desktop."""
    target = blank("nvme-target", 512)
    with Guest("t-install-nvme", nvme_ctrl("nt", "INST0001", [(target, 1)]), cmdline="blk.ro") as g:
        vm = g.vm
        vm.send(f"install --cmdline '{INSTALLED_CMDLINE}' nvme0n1\n")
        vm.expect(r"Type the disk's name \(nvme0n1\) to go on", timeout=120)
        vm.send("nvme0n1\n")
        vm.expect(r"TouhouOS is installed on nvme0n1", timeout=600)
        vm.expect(r"root@gensokyo \S+ # ", timeout=60)
    check_installed(target)
    boot_installed(target, "nvme", "t-install-nvme-boot")


@test
def install_virtio_boots_desktop():
    """virtio-blk (never read-only, so the unlock is a formality): a whole-disk FAT volume is refused without --force, installed with it,
    and the disk boots alone to the desktop."""
    target = bare_fat("virtio-target", 384)
    with Guest("t-install-virtio", drive("vt", target) + ["-device", "virtio-blk-pci,drive=vt"], cmdline="blk.ro") as g:
        out, rc = g.run(f"install --yes-i-typed=vda --cmdline '{INSTALLED_CMDLINE}' vda")
        assert rc == 1 and "a FAT file system (no partition table)" in out, out
        out = g.ok(f"install --force --yes-i-typed=vda --cmdline '{INSTALLED_CMDLINE}' vda", timeout=600)
        assert "TouhouOS is installed on vda" in out and "(it was writable anyway)" in g.ok("dmesg"), out
    check_installed(target)
    boot_installed(target, "virtio", "t-install-virtio-boot", want_rw=False)


@test
def install_nvme_4k():
    """An NVMe namespace with 4096-byte blocks: the ESP grows to 258 MiB as mkdisk makes it (FAT32 needs 65525 clusters), the image checks clean."""
    target = blank("nvme4k-target", 640)
    ns = (target, 1, {"logical_block_size": 4096, "physical_block_size": 4096})
    with Guest("t-install-4k", nvme_ctrl("n4", "INST4K01", [ns]), cmdline="blk.ro") as g:
        out = g.ok(f"install --yes-i-typed=nvme0n1 --cmdline '{INSTALLED_CMDLINE}' nvme0n1", timeout=900)
        assert "4096-byte sectors" in out and "TouhouOS is installed" in out, out
    check_installed(target)


@test
def install_unlock_rules():
    """The block layer's write unlock itself (user/bin/unlocktest.c): root only (EPERM for anyone else, even holding the open file), the
    disk's own name, whole disks only, files opened for writing only, one open file only, released by BLKWRLOCK and by close, never for
    a write-protected medium or a disk with a mounted partition; the one block written through it is the only change to the disk."""
    disk = other_os("unlock-nvme", 128, seed=11)
    wp = blank("unlock-wp", 8)
    before, before_wp = disk.read_bytes(), sha_file(wp)
    extra = nvme_ctrl("un", "UNLOCK01", [(disk, 1)]) + ["-drive", f"if=none,id=wp,file={wp},format=raw,readonly=on",
                                                       "-device", "virtio-blk-pci,drive=wp"]
    with Guest("t-install-unlock", extra, cmdline="blk.ro") as g:
        out, rc = g.run("unlocktest nvme0n1 vda")
        assert rc == 0 and "UNLOCKTEST PASS" in out, out
        g.ok("mkdir -p /tmp/m && mount -r -t fat32 /dev/nvme0n1p2 /tmp/m")
        out, rc = g.run("unlocktest --busy nvme0n1")
        assert rc == 0 and "UNLOCKTEST PASS" in out, out
        g.ok("umount /tmp/m")
        log = g.ok("dmesg")
        must(log, r"\[Eiki\] unlocktest \(uid 1000\) asked to unlock nvme0n1 for writing and was refused")
        must(log, r"blk: nvme0n1 is read-only again \(the file that held the write unlock was closed\)")
    after = disk.read_bytes()
    assert after[51200:51712] == b"E" * 512, "the block written through the unlocked file is missing"
    assert after[:51200] == before[:51200] and after[51712:] == before[51712:], "something else on the disk changed"
    assert sha_file(wp) == before_wp


class StickGuest(Guest):
    """Like Guest, but the system starts from a stick image on USB (as on the owner's PC) instead of the fat: ESP."""

    def __init__(self, name, stick, extra=()):
        super().__init__(name, extra)
        self.stick = stick

    def __enter__(self):
        self.vm = Qemu(cmdline="", name=self.name, extra=self.extra, mem=self.mem, boot_image=self.stick, boot_bus="usb")
        self.vm.start()
        try:
            self.vm.expect(r"root@gensokyo \S+ # ", timeout=240)
        except Exception:
            self.vm.stop()
            raise
        return self


@test
def install_from_stick_over_old_install():
    """Started from a stick image on USB (the Dell's case): the files come from the stick's FAT32 ESP; a disk with the same layout but other
    GUIDs (an older install) is told apart from the stick and reinstalled with --force; a second copy of the stick image (same GUIDs) is
    refused because it cannot be told apart from the disk the system started from."""
    stick = IMG_DIR / "stick.img"
    IMG_DIR.mkdir(parents=True, exist_ok=True)
    out = subprocess.run([sys.executable, str(ROOT / "tools" / "mkimage.py"), "--no-build", "--out", str(stick), "--size", "256M",
                          "--cmdline", "text user=root blk.ro"], capture_output=True, text=True)
    assert out.returncode == 0, out.stdout[-800:] + out.stderr[-800:]
    twin = IMG_DIR / "stick-twin.img"
    twin.write_bytes(stick.read_bytes())
    old = blank("old-install", 512)
    mkdisk.build_image(str(old), 512 * MiB, [dict(name="TOUHOUOS-ESP", kind="fat32", size=64 * MiB, esp=True, files={"/kernel.elf": b"old"}),
                                             dict(name="HUMANVILLAGE", kind="fat32", size="rest", files={"/reimu/old.txt": b"old home\n"})], seed=2)
    twin_sha = sha_file(twin)
    extra = nvme_ctrl("ol", "OLDINST1", [(old, 1)]) + sata_disk("tw", twin, "ide.1", model="TWIN-STICK")
    with StickGuest("t-install-stick", stick, extra) as g:
        boot = g.ok("cat /proc/bootdisk")
        must(boot, r"table gpt\nstart 2048\nsize 131072\n")
        must(boot, r"device nvme0n1p1 nvme0n1")                    # same number, start and size as the stick's ESP ...
        parts = g.partitions()
        twin_dev = device_by_model(parts, "TWIN-STICK")
        stick_dev = [n for n, r in parts.items() if r["parent"] == "-" and r["driver"] == "usb-storage"]
        assert len(stick_dev) == 1 and re.search(rf"device {stick_dev[0]}1 {stick_dev[0]}\n", boot), (stick_dev, boot)
        listing = g.ok("install")
        must(listing, r"(?m)^sd[a-z]+\s.*the system was started from this disk")
        for dev in (stick_dev[0], twin_dev):           # the stick and its twin carry the same GUIDs: neither can be ruled out
            out, rc = g.run(f"install --force --yes-i-typed={dev} {dev}")
            assert rc == 1 and re.search(r"could not be ruled out|is the disk this system was started from", out), out
        out = g.ok(f"install --force --yes-i-typed=nvme0n1 --cmdline '{INSTALLED_CMDLINE}' nvme0n1", timeout=600)   # ... but other GUIDs
        assert "TouhouOS is installed on nvme0n1" in out, out
        must(g.ok("dmesg"), r"fat32: sd[a-z]+1 mounted on /BambooForest/install-esp\.\d+, .*read-only")
    assert sha_file(twin) == twin_sha, "the twin of the stick was modified"
    check_installed(old)


@test
def install_refusals():
    """Nothing is written when: the disk has partitions (no --force), the typed name is wrong (at the terminal or --yes-i-typed), the caller
    is not root, the disk is the one the system started from, the name is a partition, a partition is mounted, the disk is too small."""
    used = other_os("ref-used", 128, seed=9)
    empty = blank("ref-empty", 256)
    tiny = blank("ref-tiny", 64)
    images = (used, empty, tiny)
    before = {p: sha_file(p) for p in images}
    extra = (sata_disk("ru", used, "ide.1", model="REF-USED") + nvme_ctrl("re", "REF00001", [(empty, 1)]) +
             sata_disk("rt", tiny, "ide.3", model="REF-TINY"))
    with Guest("t-install-refuse", extra, cmdline="blk.ro") as g:
        parts = g.partitions()
        used_d, tiny_d = device_by_model(parts, "REF-USED"), device_by_model(parts, "REF-TINY")

        def refused(cmd, *patterns):
            out, rc = g.run(cmd)
            assert rc != 0 and "nothing was written" in out, f"'{cmd}' should have been refused (rc {rc}):\n{out}"
            for p in patterns:
                must(out, p, f"(from '{cmd}')")
            return out

        listing = g.ok("install")
        must(listing, r"(?m)^sda\s.*the system was started from this disk")
        must(listing, rf"(?m)^{used_d}\s.*holds partitions {used_d}1\S* {used_d}2.*needs --force")
        must(listing, r"(?m)^nvme0n1\s.*empty")
        refused(f"install --yes-i-typed={used_d} {used_d}", r"is not empty: it holds partitions", r"Give --force")
        refused("install --yes-i-typed=nvme0n2 nvme0n1", r"--yes-i-typed=nvme0n2 does not name the disk nvme0n1")
        refused("install --yes-i-typed=NVME0N1 nvme0n1", r"does not name the disk")
        refused("su reimu -c 'install --yes-i-typed=nvme0n1 nvme0n1'", r"only root can install")
        refused("install --force --yes-i-typed=sda sda", r"sda is the disk this system was started from \(sda1 holds the running loader\)")
        refused("install --force --yes-i-typed=sda1 sda1", r"sda1 is a partition of sda")
        refused(f"install --yes-i-typed={tiny_d} {tiny_d}", r"too small")
        refused("install --yes-i-typed=sdz sdz", r"there is no disk called sdz")
        g.ok(f"mkdir -p /tmp/m && mount -r -t fat32 /dev/{used_d}2 /tmp/m")
        refused(f"install --force --yes-i-typed={used_d} {used_d}", rf"/dev/{used_d}2 is mounted on /tmp/m: umount it first")
        g.ok("umount /tmp/m")
        # the confirmation typed at the terminal: anything but the exact name stops it
        for answer in ("sdb", "", "nvme0n1 please"):
            g.vm.send("install nvme0n1; echo __RC''=$?\n")
            g.vm.expect(r"Type the disk's name \(nvme0n1\) to go on", timeout=120)
            g.vm.send(answer + "\n")
            m = g.vm.expect(r"__RC=(\d+)", timeout=120)
            assert m.group(1) == "1", f"typing {answer!r} must stop the installer"
            must(g.vm.text()[-600:], re.escape(f"you typed '{answer}', not 'nvme0n1'"))
            g.vm.expect(r"root@gensokyo \S+ # ", timeout=30)
        # the dry run shows the plan; the new cmdline.txt is the running one plus blk.rw= (no --cmdline given)
        out = g.ok("install --dry-run nvme0n1")
        must(out, r"cmdline\.txt: text user=root blk\.ro nosounds blk\.rw=[0-9a-f-]{36}\n")
        must(out, r"--dry-run: stopping here, nothing was written")
        log = g.ok("dmesg")
        assert "unlocked for writing" not in log, "no refusal may get as far as the unlock:\n" + log[-1500:]
    for p, h in before.items():
        assert sha_file(p) == h, f"{p.name} was modified by a refused install"
