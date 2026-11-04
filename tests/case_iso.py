"""The ISO (docs/ISO.md): the file tools/mkiso.py makes, booted the two ways a person uses it, in QEMU with OVMF.

  * as a CD (an optical drive on the Q35's SATA controller, the way VirtualBox, VMware and Hyper-V present an ISO): the firmware reads the El Torito UEFI
    entry and starts the loader from the embedded FAT image;
  * as a raw USB disk (a usb-storage device on xHCI holding the very same file, the way Rufus in DD mode or `dd` make a stick): the firmware finds the GPT's
    EFI System Partition.

Each boots to the desktop's log line. Two more boots in text mode, with a blank disk attached, show what the brief asks to be sure of: nothing on the
disks is touched, there is no HUMANVILLAGE volume and nothing complains about that (also at shutdown), and the installers refuse politely (from the CD) or plan
from the ISO's own boot partition (from the stick).

The structure of the file itself is checked on the host (tests/host/test_mkiso.py, tests/isocheck.py); these tests need build/esp (tools/build.py).

Run just these with:  python tools/qemu-slot.py -- python tests/run_all.py iso_
"""
import hashlib
import os
import re
import subprocess
import sys
import time
from pathlib import Path

import isocheck
import test_shell
from harness import test, Qemu, BUILD, scaled

ROOT = Path(__file__).resolve().parent.parent
EPOCH = "1000000000"


class IsoQemu(Qemu):
    """A Qemu whose only boot device is the ISO: mode "cd" (an ide-cd on the Q35 SATA controller) or "usb" (usb-storage behind xHCI, read-only)."""

    def __init__(self, iso, mode, **kw):
        super().__init__(**kw)
        self.iso, self.mode = Path(iso), mode

    def _boot_drive_args(self):
        iso = str(self.iso).replace("\\", "/")
        if self.mode == "cd":
            return ["-drive", f"if=none,id=cd0,media=cdrom,readonly=on,format=raw,file={iso}", "-device", "ide-cd,drive=cd0,bootindex=0"]
        return ["-drive", f"if=none,id=usbiso,format=raw,readonly=on,file={iso}", "-device", "qemu-xhci,id=bootxhci",
                "-device", "usb-storage,bus=bootxhci.0,drive=usbiso,bootindex=0"]


def make_iso(name, cmdline=None):
    """Builds an ISO from the current build/esp with the real tool; the reader of tests/isocheck.py must find it sound."""
    out = BUILD / f"{name}.iso"
    cmd = [sys.executable, str(ROOT / "tools" / "mkiso.py"), "--no-build", "--out", str(out)] + (["--cmdline", cmdline] if cmdline else [])
    r = subprocess.run(cmd, capture_output=True, text=True, env=dict(os.environ, SOURCE_DATE_EPOCH=EPOCH))
    assert r.returncode == 0, r.stdout[-600:] + r.stderr[-600:]
    problems = isocheck.IsoImage(out).verify()
    assert not problems, problems
    return out


def blank_disk(name, mib=256):
    p = BUILD / name
    with open(p, "wb") as f:
        f.truncate(mib << 20)
    return p


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1 << 22):
            h.update(chunk)
    return h.hexdigest()


def desktop_is_up(vm, timeout=60):
    """The screen holds a picture, not a console: hundreds of different colours (the wallpaper, the panel), where a text console has a handful."""
    end = time.time() + scaled(timeout)
    colours = 0
    while time.time() < end:
        w, h, rgb = vm.screen_ppm()
        seen = set()
        for y in range(0, h, max(1, h // 40)):
            for x in range(0, w, max(1, w // 64)):
                i = (y * w + x) * 3
                seen.add(rgb[i:i + 3])
        colours = len(seen)
        if colours >= 40:
            return colours
        time.sleep(0.5)
    raise AssertionError(f"the screen never showed a desktop (only {colours} different colours in the sample)")


def kernel_lines(text):
    return [l for l in text.splitlines() if re.match(r"\[\w+\] ", l)]


@test
def iso_boots_as_a_cd_to_the_desktop():
    """The ISO in an optical drive: El Torito UEFI entry -> loader from the embedded FAT image -> kernel -> reimu's desktop, with no HUMANVILLAGE volume."""
    iso = make_iso("t-iso-cd")
    with IsoQemu(iso, "cd", cmdline="", name="t-iso-cd", mem="768M") as vm:
        vm.expect(r"opening the desktop for reimu", timeout=150)
        colours = desktop_is_up(vm)
        txt = vm.text()
        assert "UEFI QEMU DVD-ROM" in txt, "the firmware should have started the loader from the optical drive:\n" + txt[:800]
        assert "loader: the boot volume's device path names no partition" in txt, "a CD has no partition: the loader says so\n" + txt[:1200]
        assert re.search(r"cmdline 'video=native blk\.ro update\.loader=1'", txt), "the ISO's own cmdline.txt must be used:\n" + txt[:1500]
        assert re.search(r"loader: kernel\.elf \d+ bytes", txt) and re.search(r"loader: initrd\.img \d+ bytes", txt)
        assert "no HUMANVILLAGE volume found: homes stay in RAM (tmpfs)" in txt
        assert "ATAPI device (CD/DVD) -- not supported, ignored" in txt, "the SATA driver should leave the optical drive alone"
        assert "boot complete after" in txt
        bad = [l for l in kernel_lines(txt) if re.search(r"panic|oops|exception|cannot|failed|error", l, re.I)]
        assert not bad, "lines that look like trouble:\n" + "\n".join(bad[:10])
        assert colours >= 40


@test
def iso_boots_as_a_usb_disk_to_the_desktop():
    """The same file as a raw USB disk: the firmware finds the GPT's EFI System Partition, the loader says which partition it came from, the desktop opens."""
    iso = make_iso("t-iso-usb")
    with IsoQemu(iso, "usb", cmdline="", name="t-iso-usb", mem="768M") as vm:
        vm.expect(r"opening the desktop for reimu", timeout=150)
        colours = desktop_is_up(vm)
        txt = vm.text()
        m = re.search(r"loader: booted from partition 1 \(GPT, start (\d+), (\d+) blocks\)", txt)
        assert m, "the loader should have found its GPT partition:\n" + txt[:1500]
        part = isocheck.IsoImage(iso).gpt["parts"][0]
        assert (int(m.group(1)), int(m.group(2))) == (part["first"], part["last"] - part["first"] + 1), (m.groups(), part["first"], part["last"])
        assert re.search(r"cmdline 'video=native blk\.ro update\.loader=1'", txt)
        assert re.search(r"\[Kappa\] .*xhci|\[Kappa\] usb", txt)
        assert "usb-storage" in txt, "the stick should be driven as a USB mass storage device"
        assert not re.search(r"fat32: \w+ mounted on /HumanVillage", txt), "the ISO's own partitions are not HUMANVILLAGE"
        assert "boot complete after" in txt
        assert colours >= 40


def ok(vm, cmd, timeout=90):
    out, rc = test_shell.run(vm, cmd, timeout=timeout)
    return out, rc


@test
def iso_from_the_cd_touches_no_disk_and_the_installers_refuse():
    """Text mode from the CD with a blank disk attached: no HUMANVILLAGE volume (homes in memory), `install` and `install --alongside` refuse and the disk is
    byte for byte what it was, and shutting down says nothing about a missing volume."""
    iso = make_iso("t-iso-text-cd", "text blk.ro")
    disk = blank_disk("t-iso-blank-cd.img")
    before = sha(disk)
    with IsoQemu(iso, "cd", cmdline="", name="t-iso-text-cd", mem="768M", disk=disk) as vm:
        vm.expect(test_shell.PROMPT, timeout=150)
        out, rc = ok(vm, "mount | grep -i -c humanvillage; ls -d /HumanVillage/reimu; cat /proc/cmdline")
        assert out.splitlines()[0].strip() == "0", out                      # nothing is mounted as the stick's volume
        assert "/HumanVillage/reimu" in out and "text blk.ro" in out, out   # the home exists, in memory
        out, rc = ok(vm, "su marisa -c 'sudo install'")
        assert "vda" in out and "the firmware did not say which disk the system was started from" in out, out
        for cmd, needle in (("sudo install --dry-run vda", "cannot tell which partition the system was started from"),
                            ("sudo install --force --dry-run vda", "--force cannot be allowed"),
                            ("sudo install --alongside --dry-run vda", None),
                            ("sudo install --alongside vda", None)):
            out, rc = ok(vm, f"su marisa -c '{cmd}'")
            assert rc != 0, f"`{cmd}` should have been refused:\n{out}"
            assert "nothing was written" in out, f"`{cmd}`:\n{out}"
            if needle:
                assert needle in out, f"`{cmd}`:\n{out}"
        vm.send("su marisa -c 'sudo poweroff'\n")
        rc = vm.wait_exit(60)
        assert rc is not None, "the machine did not power off:\n" + vm.text()[-1500:]
        txt = vm.text()
        tail = txt[txt.rfind("poweroff"):]
        noisy = [l for l in kernel_lines(tail) if re.search(r"fail|error|cannot|unable|refus|humanvillage|volume", l, re.I)]
        assert not noisy, "shutting down complained:\n" + "\n".join(noisy)
        humans = [l for l in kernel_lines(txt) if re.search(r"humanvillage", l, re.I)]
        assert humans == ["[Keine] no HUMANVILLAGE volume found: homes stay in RAM (tmpfs)"], humans
    assert sha(disk) == before, "the blank disk changed"
    disk.unlink()                                                                   # (256 MiB of nothing; kept only when a check above failed)


@test
def iso_from_a_stick_plans_an_install_from_its_own_boot_partition():
    """Text mode from the raw USB disk with a blank disk attached: `install --dry-run` reads the system from the stick's FAT16 boot partition, plans, and
    writes nothing; the blank disk is untouched. The stick is larger than the image (45 MiB of zeros after it, as on a real stick that held something else
    before), so the backup partition table is not at the end of the disk: the system must not mind."""
    iso = make_iso("t-iso-text-usb", "text blk.ro")
    stick = BUILD / "t-iso-stick.img"
    stick.write_bytes(iso.read_bytes() + bytes(45 << 20))
    disk = blank_disk("t-iso-blank-usb.img")
    before = sha(disk)
    with IsoQemu(stick, "usb", cmdline="", name="t-iso-text-usb", mem="768M", disk=disk) as vm:
        vm.expect(test_shell.PROMPT, timeout=150)
        out, rc = ok(vm, "cat /proc/bootdisk; cat /proc/partitions")
        assert "partition 1" in out and "table gpt" in out, out
        assert re.search(r"^sda1 \d+ 512 \d sda \d+ ", out, re.M), "the ESP of the hybrid image should be sda1:\n" + out
        out, rc = ok(vm, "su marisa -c 'sudo install --dry-run vda'")
        assert rc == 0, out
        assert "nothing was written" in out, out
        for name in ("kernel.elf", "initrd.img", "cmdline.txt", "BOOTX64.EFI"):
            assert name in out, f"the plan should name {name}:\n{out}"
        out, rc = ok(vm, "su marisa -c 'sudo install --dry-run sda'")                # the disk the system started from
        assert rc != 0 and "nothing was written" in out, out
        out, rc = ok(vm, "mount | grep -i -c humanvillage")
        assert out.strip().splitlines()[0] == "0", out
    assert sha(disk) == before, "the blank disk changed"
    assert stick.read_bytes()[:len(iso.read_bytes())] == iso.read_bytes() and not any(stick.read_bytes()[len(iso.read_bytes()):]), "the stick changed"
    disk.unlink()
    stick.unlink()
