"""USB mass storage: a stick behind QEMU's xHCI (usb-storage) becomes sdb, its HUMANVILLAGE partition becomes /HumanVillage.

Every test builds its own disk image with tools/mkdisk.py, attaches it with `-device usb-storage` on a `qemu-xhci`, drives the shell over
the serial console and -- after the guest is gone -- looks at what the kernel left on the image with mkdisk's independent FAT checker.
Images live in build/storage-tests/ (see case_storage.py) and are deleted afterwards (TOUHOU_KEEP_IMAGES=1 keeps them).

Run just these with:  python tests/run_all.py usbstor
"""
import re
import subprocess
import sys
import time
from pathlib import Path

import mkdisk
import case_storage
from case_storage import (Image, Guest, hv_part, decoy_esp, file_sha, sha, MIB, WORK, HV, H, ROOT_PROMPT, USER_PROMPT)
from harness import test, Qemu, BUILD, scaled


def boot(*args, **kw):
    """case_storage.boot with the kernel log kept on the stick (the storage tests switch that off; these tests are about it)."""
    kw.setdefault("bootlog", True)
    return case_storage.boot(*args, **kw)

ROOT = Path(__file__).resolve().parent.parent
XHCI = ["-device", "qemu-xhci,id=xhci"]


def stick(img, n=1, ro=False, bs=None, attach=True, extra=""):
    """QEMU arguments for a USB stick: the image behind usb-storage on the xHCI controller. attach=False defines only the drive, so
    the test can plug the device in later with plug()."""
    path = img.path if hasattr(img, "path") else img
    # (-blockdev nodes, not -drive: QEMU deletes an `-drive if=none` backend when its device is unplugged, so a stick could not be plugged in again)
    ro_opt = ",read-only=on" if ro else ""
    args = ["-blockdev", f"driver=file,node-name=stick{n}f,filename={path}{ro_opt}", "-blockdev", f"driver=raw,node-name=stick{n},file=stick{n}f{ro_opt}"]
    if attach:
        args += ["-device", f"usb-storage,drive=stick{n},id=ustor{n},bus=xhci.0" + (f",logical_block_size={bs},physical_block_size={bs}" if bs else "") + extra]
    return args


def plug(vm, n=1, bs=None):
    kw = dict(driver="usb-storage", drive=f"stick{n}", id=f"ustor{n}", bus="xhci.0")
    if bs:
        kw["logical_block_size"] = kw["physical_block_size"] = bs
    vm.qmp("device_add", **kw)


def unplug(vm, n=1):
    vm.qmp("device_del", id=f"ustor{n}")


def wait_log(vm, pattern, timeout=90, since=0):
    """Waits until the serial log holds pattern (searches the whole log from offset `since`); returns the match."""
    rx = re.compile(pattern, re.S)
    end = time.time() + scaled(timeout)
    while time.time() < end:
        m = rx.search(vm.text(), since)
        if m:
            return m
        if not vm.alive():
            break
        time.sleep(0.1)
    raise TimeoutError(f"/{pattern}/ never appeared in the log:\n" + vm.text()[-2500:])


def no_incident(vm):
    text = vm.text()
    for bad in ("INCIDENT", "BOUNDARY VIOLATION", "assertion failed"):
        assert bad not in text, f"{bad!r} in the log:\n" + text[-3000:]


KLOG_LINE = re.compile(r"^\[[A-Z][A-Za-z]+\] .*(\n|$)", re.M)


def say(g, cmd, timeout=60):
    """What a command printed, without the kernel log lines that the console interleaves with it at unlucky moments, stripped."""
    return KLOG_LINE.sub("", g.ok(cmd, timeout)).strip()


def host_read(img, path, part=2):
    r = mkdisk.open_volume(img.path, part)
    try:
        return r.read_file(path)
    finally:
        r.close()


def stick_image(name, size=100 * MIB, files=None, esp=True, **kw):
    parts = ([decoy_esp()] if esp else []) + [hv_part(files or {"/reimu/hello.txt": b"hello from the stick\n"})]
    return Image(name, size, parts, **kw)


NUMBERS = "".join(f"{i}\n" for i in range(1, 400001)).encode()          # `seq 1 400000`: 2.7 MB, easy to make in the guest and on the host


# ---------------------------------------------------------------------------------------------------------------------
# the device

@test
def usbstor_enumerate_read():
    """A stick shows up as sdb (model, size, partitions, driver usb-storage), reads match the host byte for byte, HUMANVILLAGE is
    mounted before the first session, the ESP is neither mounted nor touched."""
    big = bytes((i * 7 + (i >> 8)) & 0xFF for i in range(3 * MIB))
    files = {"/reimu/hello.txt": b"hello from the stick\n", "/shared/big.bin": big, "/shared/Mixed Case.TXT": b"mixed\n"}
    # a second stick whose volume is not HUMANVILLAGE: nothing ever writes to it, so its checksum can be compared as a whole
    data_parts = [dict(name="DATA", kind="fat32", size="rest", label="MYSTUFF", files={"/photos/a.jpg": big[:200000]})]
    with stick_image("usbs-enum", files=files) as img, Image("usbs-enum-data", 80 * MIB, data_parts) as data:
        esp0 = img.part_hash(1)
        with boot(None, root=True, name="t-usbs-enum", extra=XHCI + stick(img, n=1) + stick(data, n=2)) as (g, vm):
            text = vm.text()
            # the volume was mounted while the kernel was still starting up, before init opened the first session
            m_mount = re.search(r"\[Keine\] fat32: sdb2 mounted on /HumanVillage", text)
            m_init = re.search(r"\[Alice\] init: /sbin/init", text)
            assert m_mount and m_init and m_mount.start() < m_init.start(), text[-3000:]
            assert re.search(r"\[Kappa\] usb-storage 1-\d+ lun 0: \"QEMU QEMU HARDDISK\", 100 MiB in 512-byte blocks, writable", text), text[-3000:]
            assert re.search(r"\[Kappa\] usb: the bus settled after \d+ ms", text), text[-2000:]
            out = g.ok("lsblk")
            assert re.search(r"^sdb\s+100M\s+0\s+disk\s+usb-storage\s+QEMU QEMU HARDDISK", out, re.M), out
            assert re.search(r"^sdb1\s+34M\s+0\s+part\s+TOUHOUOS-ESP\s+usb-storage", out, re.M), out
            assert re.search(r"^sdb2\s+\S+\s+0\s+part\s+HUMANVILLAGE\s+usb-storage", out, re.M), out
            assert re.search(r"^sdc\s+80M\s+0\s+disk\s+usb-storage", out, re.M) and re.search(r"^sdc1\s+\S+\s+0\s+part\s+DATA", out, re.M), out
            out = g.ok("cat /proc/partitions")
            assert re.search(r"^sdb 104857600 512 0 - 0 - usb-storage QEMU QEMU HARDDISK$", out, re.M), out
            out = g.ok("cat /proc/usbstorage")
            assert len(re.findall(r"usb-storage 1-\d+: 46f4:0001 bulk-only, 1 LUN, 0 reset recoveries", out)) == 2, out
            assert re.search(r"lun 0: online \"QEMU QEMU HARDDISK\" sdb 204800 x 512 rw", out), out
            out = g.ok("lsusb")
            assert len(re.findall(r"ID 46f4:0001 QEMU QEMU USB HARDDRIVE, super-speed.*driver usb-storage", out)) == 2, out
            assert re.search(r"If 0: class 08/06/50, 2 endpoints, driver usb-storage", out), out
            out = g.ok("mount")
            assert re.search(r"/dev/sdb2 on /HumanVillage type fat32", out) and "sdb1" not in out and "sdc" not in out, out
            assert "/dev/sdb2" in g.ok("df /HumanVillage")
            # reads: a whole stick that nobody writes, the untouched front of the other one (partition table + ESP), a range that starts and
            # ends inside sectors, a partition, a file and a long file name
            assert file_sha(data.path) in g.ok("sha256sum /dev/sdc", timeout=240), "whole-device checksum differs from the host's"
            front = open(img.path, "rb").read(35 * MIB)
            assert sha(front) in g.ok("dd if=/dev/sdb bs=1M count=35 2>/dev/null | sha256sum", timeout=120), "the front of the stick differs"
            assert sha(front[1000:6000]) in g.ok("dd if=/dev/sdb bs=1 skip=1000 count=5000 2>/dev/null | sha256sum"), "unaligned range differs"
            assert img.part_hash(1) in g.ok("sha256sum /dev/sdb1", timeout=120), "partition 1 differs"
            assert sha(big) in g.ok(f"sha256sum {HV}/shared/big.bin"), "file read through FAT differs"
            assert say(g, f"cat '{HV}/shared/Mixed Case.TXT'") == "mixed"
            assert say(g, f"cat {H}/hello.txt") == "hello from the stick"
            g.ok("sync")
            g.poweroff()
        no_incident(vm)
        img.check()
        assert img.part_hash(1) == esp0, "the ESP of the stick changed"


@test
def usbstor_persist_reboot():
    """Two boots on the same stick: files written in the first (2.7 MB, many small ones, renames, deletes) are there in the second,
    the image is clean after each poweroff, and nothing but the HUMANVILLAGE partition changes."""
    with stick_image("usbs-persist") as img:
        esp0 = img.part_hash(1)
        with boot(None, root=True, name="t-usbs-persist1", extra=XHCI + stick(img)) as (g, vm):
            g.ok(f"seq 1 400000 > {HV}/numbers.txt")
            g.ok(f"mkdir -p {HV}/reimu/docs && for i in 1 2 3 4 5 6 7 8 9 10; do echo line $i > {HV}/reimu/docs/file$i.txt; done")
            g.ok(f"mv {HV}/reimu/docs/file3.txt '{HV}/reimu/docs/renamed three.txt' && rm {HV}/reimu/docs/file4.txt")
            g.poweroff()
        rep = img.check()
        assert rep.stats["p2"]["clean"]
        assert host_read(img, "/numbers.txt") == NUMBERS, "the 2.7 MB file did not reach the stick intact"
        assert host_read(img, "/reimu/docs/renamed three.txt") == b"line 3\n"
        assert host_read(img, "/reimu/docs/file10.txt") == b"line 10\n"
        r = mkdisk.open_volume(img.path, 2)
        try:
            assert r.find("/reimu/docs/file4.txt") is None and r.find("/reimu/docs/file3.txt") is None
        finally:
            r.close()
        with boot(None, root=True, name="t-usbs-persist2", extra=XHCI + stick(img)) as (g, vm):
            assert "not cleanly unmounted" not in vm.text(), vm.text()[-2000:]
            assert sha(NUMBERS) in g.ok(f"sha256sum {HV}/numbers.txt")
            assert say(g, f"cat '{HV}/reimu/docs/renamed three.txt'") == "line 3"
            assert say(g, f"ls {HV}/reimu/docs | wc -l") == "9"
            g.ok(f"echo second boot >> {HV}/reimu/docs/file1.txt")
            g.poweroff()
        img.check()
        assert host_read(img, "/reimu/docs/file1.txt") == b"line 1\nsecond boot\n"
        assert img.part_hash(1) == esp0


@test
def usbstor_write_protected():
    """A write-protected stick (MODE SENSE says so) is registered read-only: lsblk shows RO, the volume mounts read-only, root cannot
    write /dev/sdb either, and the image is byte-identical afterwards."""
    with stick_image("usbs-ro") as img:
        h0 = img.sha()
        with boot(None, root=True, name="t-usbs-ro", extra=XHCI + stick(img, ro=True)) as (g, vm):
            out = g.ok("lsblk")
            assert re.search(r"^sdb\s+\S+\s+1\s+disk\s+usb-storage", out, re.M) and re.search(r"^sdb2\s+\S+\s+1\s+part", out, re.M), out
            assert re.search(r"write-protected", vm.text()), vm.text()[-2000:]
            assert re.search(r"\[Keine\] fat32: sdb2 mounted on /HumanVillage.*read-only", vm.text()), vm.text()[-2500:]
            assert say(g, f"cat {H}/hello.txt") == "hello from the stick"
            g.fails(f"touch {HV}/new.txt", r"[Rr]ead-only|EROFS")
            out, _ = g.run("dd if=/dev/zero of=/dev/sdb2 bs=512 count=1 2>&1")                # (dd reports the error and still exits 0)
            assert re.search(r"[Rr]ead-only|EROFS|not permitted", out) and "1+0 records out" not in out, out
            assert re.search(r"TOUHOU.LOG is not kept", vm.text()), vm.text()[-2000:]
            g.ok("sync")
            g.poweroff()
        no_incident(vm)
        assert img.sha() == h0, "a write-protected stick was modified"


# ---------------------------------------------------------------------------------------------------------------------
# hot-plug

@test
def usbstor_hotplug_idle():
    """A stick plugged in after boot is mounted by the policy hook; pulled out while idle, its disks vanish, the mount is dropped
    cleanly, and plugging it in again brings the Human Village back. Dirty after a pull without sync: the next mount is read-only."""
    with stick_image("usbs-hot", files={"/reimu/hello.txt": b"hello from the stick\n", "/other/big.bin": bytes(range(256)) * 8192}) as img:
        with boot(None, root=True, name="t-usbs-hot", extra=XHCI + stick(img, attach=False)) as (g, vm):
            assert "sdb" not in g.ok("cat /proc/partitions")
            assert say(g, "mount | grep -c HumanVillage || true") == "0"
            mark = len(vm.text())
            plug(vm)
            wait_log(vm, r"\[Keine\] the Human Village keeps its records on the stick now \(sdb2", timeout=120, since=mark)
            out = g.ok("lsblk; mount")
            assert re.search(r"^sdb2\s+\S+\s+0\s+part\s+HUMANVILLAGE", out, re.M) and "/dev/sdb2 on /HumanVillage" in out, out
            assert say(g, f"cat {H}/hello.txt") == "hello from the stick"
            g.ok(f"echo before the pull > {HV}/reimu/note.txt; sync")
            # pull it out while nothing is going on
            mark = len(vm.text())
            unplug(vm)
            wait_log(vm, r"blk: sdb removed together with sdb1, sdb2", timeout=60, since=mark)
            wait_log(vm, r"usb-storage \S+: the stick was pulled out", timeout=10, since=mark)
            out = g.ok("cat /proc/partitions; ls /dev; cat /proc/usbstorage")
            assert "sdb" not in out.replace("usb-storage", ""), out
            assert "unplugged" in out, out
            g.ok(f"ls {HV}/reimu")                                                    # (cached: the directory was read before the pull)
            g.fails(f"cat {HV}/other/big.bin > /dev/null", r"No such device|I/O error|ENODEV")      # a file that was never read has to come from the stick
            assert re.search(r"\[Keine\] fat32: sdb2 vanished while it was mounted on /HumanVillage", vm.text()), vm.text()[-2500:]
            g.ok("umount /HumanVillage")
            assert re.search(r"was dropped without a write-back: its device is gone", vm.text()), vm.text()[-2500:]
            assert say(g, "mount | grep -c HumanVillage || true") == "0"
            # plug it in again (a new device id): same letters, mounted again by the hook
            mark = len(vm.text())
            vm.qmp("device_add", driver="usb-storage", drive="stick1", id="ustor2", bus="xhci.0")
            wait_log(vm, r"\[Keine\] the Human Village keeps its records on the stick now \(sdb2", timeout=120, since=mark)
            out = say(g, f"cat {HV}/reimu/note.txt; ls -la {HV}/reimu")
            assert out.startswith("before the pull\n"), out + vm.text()[-2500:]
            # write something and pull the stick without a sync: the volume is flagged dirty, the next mount is read-only
            g.ok(f"echo unsynced > {HV}/reimu/late.txt")
            mark = len(vm.text())
            vm.qmp("device_del", id="ustor2")
            wait_log(vm, r"blk: sdb removed", timeout=60, since=mark)
            g.ok("umount /HumanVillage")
            mark = len(vm.text())
            vm.qmp("device_add", driver="usb-storage", drive="stick1", id="ustor3", bus="xhci.0")
            wait_log(vm, r"\[Keine\] fat32: sdb2 mounted on /HumanVillage.*read-only: the volume was not cleanly unmounted", timeout=120, since=mark)
            no_incident(vm)
            out = g.ok(f"cat {HV}/reimu/note.txt")                                          # the shell is alive and the data readable
            assert out.strip() == "before the pull", out + vm.text()[-2500:]


@test
def usbstor_behind_a_hub():
    """A stick behind a (full-speed) hub: enumerated, mounted, written; pulling the hub out takes the stick with it, and the disks vanish
    without a crash (the disconnect arrives from the hub driver, not from the stick's own port)."""
    with stick_image("usbs-hub", files={"/reimu/hello.txt": b"hello from the stick\n"}) as img:
        extra = XHCI + ["-device", "usb-hub,id=hub1,bus=xhci.0,port=1"] + stick(img, attach=False) + [
            "-device", "usb-storage,drive=stick1,id=ustor1,bus=xhci.0,port=1.1"]
        with boot(None, root=True, name="t-usbs-hub", extra=extra) as (g, vm):
            # (QEMU's port=1 is a pair of root ports: a full-speed hub lands on the USB2 half, which the guest numbers 5, so no "1-1" here)
            m = wait_log(vm, r"usb-storage (1-\d+)\.1 lun 0: .* carried over the river as sdb", timeout=120)
            behind = m.group(1)
            out = g.ok("lsusb; lsblk")
            assert re.search(r"Port \d+\.1: ID 46f4:0001 .*full-speed.*driver usb-storage", out), out
            assert re.search(r"^sdb\s+100M\s+0\s+disk\s+usb-storage", out, re.M), out
            assert "/dev/sdb2 on /HumanVillage" in g.ok("mount")
            assert say(g, f"cat {H}/hello.txt") == "hello from the stick"
            g.ok(f"seq 1 20000 > {HV}/behind.txt; sync")
            mark = len(vm.text())
            vm.qmp("device_del", id="hub1")                                # the hub and everything behind it
            wait_log(vm, r"blk: sdb removed together with sdb1, sdb2", timeout=90, since=mark)
            wait_log(vm, re.escape(f"usb-storage {behind}.1: the stick was pulled out"), timeout=30, since=mark)
            assert "sdb" not in g.ok("cat /proc/partitions")
            no_incident(vm)
            assert say(g, "echo alive") == "alive"


@test
def usbstor_unplug_during_io():
    """Pulling the stick out while a long read is running: the reader fails (no hang, no panic), the disks vanish, the system carries on."""
    with stick_image("usbs-io", size=300 * MIB) as img:
        with boot(None, root=True, name="t-usbs-io", extra=XHCI + stick(img)) as (g, vm):
            g.ok("(dd if=/dev/sdb of=/dev/null bs=1M > /tmp/dd.out 2>&1; echo done-$? >> /tmp/dd.out) &")
            time.sleep(1.5)
            mark = len(vm.text())
            unplug(vm)
            wait_log(vm, r"blk: sdb removed", timeout=90, since=mark)
            end = time.time() + scaled(90)
            out = ""
            while time.time() < end:
                out = g.ok("cat /tmp/dd.out")
                if "done-" in out:
                    break
                time.sleep(1)
            assert re.search(r"dd: error reading '/dev/sdb': (No such device|Input/output error)", out), "the reader should have failed:\n" + out
            assert "300+0 records out" not in out, out                       # (dd reports the error and exits 0; what matters is that it stopped short)
            copied = int(re.search(r"(\d+) bytes copied", out).group(1))
            assert 0 < copied < 300 * MIB, out
            no_incident(vm)
            assert say(g, "echo still alive") == "still alive"
            assert "sdb" not in g.ok("cat /proc/partitions")


# ---------------------------------------------------------------------------------------------------------------------
# hostile and odd devices

def corrupt_images():
    """Three sticks with broken partition tables, and one that is blank. Returns [(name, path)]."""
    out = []
    p = WORK / "usbs-blank.img"
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(b"\0" * (32 * MIB))
    out.append(("blank", p))
    # (The firmware repairs a GPT whose primary copy is bad but whose backup is good -- OVMF writes the stick before the kernel ever
    # starts -- so both copies are damaged here.)
    backup_header = (64 * MIB // 512 - 1) * 512
    backup_array = (64 * MIB // 512 - 33) * 512
    # 1. a GPT with a header that makes no sense (insane entry count and size) and a garbage entry array
    p = WORK / "usbs-badgpt.img"
    mkdisk.build_image(p, 64 * MIB, [hv_part({"/reimu/x.txt": b"x"})])
    with open(p, "r+b") as f:
        f.seek(512 + 80)
        f.write(b"\xff\xff\xff\xff" + b"\x00\x10\x00\x00")           # nentries = 0xFFFFFFFF, entry size = 4096
        f.seek(1024)
        f.write(bytes(range(256)) * 8)                                # the entry array
        f.seek(backup_header)
        f.write(b"\0" * 512)
    out.append(("badgpt", p))
    # 2. a GPT whose entries point beyond the end of the stick and overlap (one labelled HUMANVILLAGE)
    p = WORK / "usbs-wildgpt.img"
    mkdisk.build_image(p, 64 * MIB, [hv_part({"/reimu/x.txt": b"x"})])
    with open(p, "r+b") as f:
        f.seek(1024)
        ent = bytearray(f.read(128))
        ent[40:48] = (1 << 40).to_bytes(8, "little")                  # last LBA far beyond the disk
        wild = bytes(ent) + bytes(ent[:32]) + (2048).to_bytes(8, "little") + (10 ** 9).to_bytes(8, "little") + bytes(ent[48:])
        for at in (1024, backup_array):
            f.seek(at)
            f.write(wild)
    out.append(("wildgpt", p))
    # 3. an MBR whose entries lie about their size and overlap
    p = WORK / "usbs-badmbr.img"
    sec = bytearray(512)
    for i, (typ, lba, cnt) in enumerate(((0x0C, 2048, 0xFFFFFFF0), (0x0C, 1, 5), (0xEE, 1, 0xFFFFFFFF), (0x07, 4096, 100))):
        sec[446 + 16 * i + 4] = typ
        sec[446 + 16 * i + 8:446 + 16 * i + 12] = lba.to_bytes(4, "little")
        sec[446 + 16 * i + 12:446 + 16 * i + 16] = cnt.to_bytes(4, "little")
    sec[510:512] = b"\x55\xaa"
    p.write_bytes(bytes(sec) + b"\0" * (16 * MIB))
    out.append(("badmbr", p))
    return out


@test
def usbstor_corrupt_tables():
    """Four sticks with a blank, a garbage GPT, a wild GPT and a lying MBR: the kernel registers the disks, finds no volume worth
    mounting, writes nothing, and boots normally."""
    imgs = corrupt_images()
    try:
        hashes = {name: file_sha(path) for name, path in imgs}
        extra = list(XHCI)
        for i, (name, path) in enumerate(imgs, 1):
            extra += stick(path, n=i)
        with boot(None, root=True, name="t-usbs-corrupt", extra=extra) as (g, vm):
            wait_log(vm, r"usb: the bus settled", timeout=120)
            out = g.ok("lsblk")
            for letter in "bcde":
                assert re.search(rf"^sd{letter}\s+\S+\s+0\s+disk\s+usb-storage", out, re.M), out
            assert "HumanVillage" not in g.ok("mount"), g.ok("mount")
            assert len(re.findall(r"sd[b-e] has no HUMANVILLAGE volume", vm.text())) == 4, vm.text()[-3000:]
            no_incident(vm)
            assert say(g, "echo ok") == "ok"
            g.ok("sync")
            g.poweroff()
        for name, path in imgs:
            assert file_sha(path) == hashes[name], f"{name}: a hostile stick was written to"
    finally:
        for _, path in imgs:
            try:
                path.unlink()
            except OSError:
                pass


@test
def usbstor_uas_ignored():
    """A UAS-only device (usb-uas) is not driven: one log line says why, no disk appears, boot and the other stick are unaffected."""
    with stick_image("usbs-uas") as img, stick_image("usbs-uas2") as img2:
        extra = XHCI + ["-device", "usb-uas,id=uas1", "-drive", f"if=none,id=uasdisk,format=raw,file={img2.path}",
                        "-device", "scsi-hd,bus=uas1.0,drive=uasdisk"] + stick(img)
        with boot(None, root=True, name="t-usbs-uas", extra=extra) as (g, vm):
            wait_log(vm, r"usb: the bus settled", timeout=120)
            text = vm.text()
            assert re.search(r"mass storage interface 0 speaks subclass 06 / protocol 62 \(UAS, CBI, ATAPI\.\.\.\): only SCSI over the bulk-only transport is supported -- ignored", text), text[-3000:]
            out = g.ok("lsblk")
            assert re.search(r"^sdb\s", out, re.M) and not re.search(r"^sdc\s", out, re.M), out
            assert "/dev/sdb2 on /HumanVillage" in g.ok("mount")
            assert re.search(r"If 0: class 08/06/62, \d+ endpoints, driver none", g.ok("lsusb")), g.ok("lsusb")
            no_incident(vm)
            g.poweroff()
        assert file_sha(img2.path) == img2.sha()


@test
def usbstor_two_sticks():
    """Two sticks that both carry a HUMANVILLAGE volume: both are disks, exactly one (the first found) is mounted, the other is left
    alone with a log line and is not written to."""
    with stick_image("usbs-two-a", files={"/reimu/which.txt": b"first\n"}) as a, stick_image("usbs-two-b", files={"/reimu/which.txt": b"second\n"}) as b:
        hb0 = b.sha()
        extra = XHCI + stick(a, n=1) + stick(b, n=2)
        with boot(None, root=True, name="t-usbs-two", extra=extra) as (g, vm):
            wait_log(vm, r"usb: the bus settled", timeout=120)
            out = g.ok("lsblk")
            assert re.search(r"^sdb\s", out, re.M) and re.search(r"^sdc\s", out, re.M), out
            mounts = g.ok("mount")
            assert len(re.findall(r"HumanVillage", mounts)) == 1, mounts
            holder = re.search(r"/dev/(sd[bc]2) on /HumanVillage", mounts).group(1)
            other = "sdc2" if holder == "sdb2" else "sdb2"
            wait_log(vm, rf"\[Keine\] {other} holds a HUMANVILLAGE volume too \(partition label\), but /HumanVillage already lives on /dev/{holder}: the first one found wins", timeout=60)
            which = g.ok(f"cat {H}/which.txt").strip()
            assert which == ("first" if holder == "sdb2" else "second"), (holder, which)
            no_incident(vm)
            g.poweroff()
        if holder == "sdb2":
            assert b.sha() == hb0, "the stick that lost was written to"
            a.check()
        else:
            a.check()


@test
def usbstor_wrong_label_untouched():
    """A stick whose volume is not called HUMANVILLAGE (and the Windows-style one with a FAT label only) is never mounted or written."""
    parts = [decoy_esp(), dict(name="DATA", kind="fat32", size="rest", label="MYSTUFF", files={"/photos/a.jpg": b"jpeg"})]
    with Image("usbs-wrong", 100 * MIB, parts) as img:
        h0 = img.sha()
        with boot(None, root=True, name="t-usbs-wrong", extra=XHCI + stick(img)) as (g, vm):
            wait_log(vm, r"usb: the bus settled", timeout=120)
            assert re.search(r"^sdb2\s+\S+\s+0\s+part\s+DATA", g.ok("lsblk"), re.M)
            assert "HumanVillage" not in g.ok("mount")
            assert re.search(r"sdb has no HUMANVILLAGE volume", vm.text()), vm.text()[-2000:]
            g.ok("sync")
            g.poweroff()
        assert img.sha() == h0, "a stick without a HUMANVILLAGE volume was written to"


@test
def usbstor_internal_disk_wins():
    """An internal disk (virtio-blk here) that carries a HUMANVILLAGE volume is mounted at boot; a stick with one is left alone."""
    with stick_image("usbs-int-stick", files={"/reimu/which.txt": b"stick\n"}) as s, stick_image("usbs-int-disk", files={"/reimu/which.txt": b"internal\n"}, esp=False) as d:
        hs0 = s.sha()
        with Qemu(cmdline="text user=root", disk=d.path, name="t-usbs-internal", extra=XHCI + stick(s)) as vm:
            vm.expect(ROOT_PROMPT, timeout=120)
            g = Guest(vm, True)
            wait_log(vm, r"usb: the bus settled", timeout=120)
            assert re.search(r"/dev/vda\d on /HumanVillage", g.ok("mount")), g.ok("mount")
            assert say(g, f"cat {H}/which.txt") == "internal"
            wait_log(vm, r"\[Keine\] sdb2 holds a HUMANVILLAGE volume too .*already lives on /dev/vda\d: the first one found wins", timeout=60)
            g.ok("sync")
            g.poweroff()
        assert s.sha() == hs0


@test
def usbstor_removable_media():
    """A card reader: the medium is taken out (the disk and its partitions vanish, the unit is asked every two seconds), put back (the
    disk is registered again), and swapped for another one under an online disk (unit attention: dropped and probed again)."""
    data_parts = [dict(name="DATA", kind="fat32", size="rest", label="MYSTUFF", files={"/photos/a.jpg": b"jpeg" * 1000})]
    with Image("usbs-rm-a", 100 * MIB, data_parts) as a, Image("usbs-rm-b", 50 * MIB, data_parts) as b:
        front = open(a.path, "rb").read(MIB)
        ha0, hb0 = a.sha(), b.sha()
        extra = XHCI + stick(a, n=1, attach=False) + stick(b, n=2, attach=False) + [
            "-device", "usb-bot,id=bot1,bus=xhci.0", "-device", "scsi-hd,bus=bot1.0,drive=stick1,id=hd1,removable=on,scsi-id=0,lun=0"]
        with boot(None, root=True, name="t-usbs-removable", extra=extra) as (g, vm):
            assert re.search(r"^sdb\s+100M\s+0\s+disk\s+usb-storage", g.ok("lsblk"), re.M)
            assert sha(front) in g.ok("dd if=/dev/sdb bs=1M count=1 2>/dev/null | sha256sum")
            # the medium is taken out
            mark = len(vm.text())
            vm.qmp("eject", id="hd1", force=True)
            out, _ = g.run("dd if=/dev/sdb of=/dev/null bs=512 count=1 2>&1")
            assert re.search(r"No such device or address|Input/output error|No such device", out), out
            wait_log(vm, r"usb-storage \S+ lun 0: the medium was taken out of sdb", timeout=60, since=mark)
            wait_log(vm, r"blk: sdb removed together with sdb1", timeout=30, since=mark)
            out = g.ok("lsblk; cat /proc/usbstorage")
            assert not re.search(r"^sdb\s", out, re.M) and "no medium" in out, out
            # it comes back (the unit is polled every two seconds)
            mark = len(vm.text())
            vm.qmp("blockdev-change-medium", id="hd1", filename=str(a.path), format="raw")
            wait_log(vm, r"usb-storage \S+ lun 0: \"QEMU QEMU HARDDISK\", 100 MiB in 512-byte blocks, writable -- carried over the river as sdb", timeout=90, since=mark)
            out = g.ok("lsblk")
            assert re.search(r"^sdb\s+100M\s+0\s+disk", out, re.M) and re.search(r"^sdb1\s+\S+\s+0\s+part\s+DATA", out, re.M), out
            assert sha(front) in g.ok("dd if=/dev/sdb bs=1M count=1 2>/dev/null | sha256sum")
            # another medium goes in under an online disk: the next request notices (unit attention), the disk is registered afresh
            mark = len(vm.text())
            vm.qmp("blockdev-change-medium", id="hd1", filename=str(b.path), format="raw")
            g.run("dd if=/dev/sdb of=/dev/null bs=512 count=1 2>&1")
            wait_log(vm, r"usb-storage \S+ lun 0: the medium was changed under sdb", timeout=60, since=mark)
            wait_log(vm, r"usb-storage \S+ lun 0: \"QEMU QEMU HARDDISK\", 50 MiB in 512-byte blocks, writable -- carried over the river as sdb", timeout=90, since=mark)
            assert re.search(r"^sdb\s+50M\s+0\s+disk", g.ok("lsblk"), re.M)
            no_incident(vm)
            g.ok("sync")
            g.poweroff()
        assert a.sha() == ha0 and b.sha() == hb0, "a card reader's media were written to"


@test
def usbstor_empty_lun_slots():
    """GET MAX LUN says 2 (QEMU's stick has one real LUN, and cannot present more: usb-bot hangs the firmware with two): the three slots
    are brought up independently, LUN 0 becomes sdb and is mounted, the empty slots are skipped without a fuss."""
    with stick_image("usbs-lun0") as a:
        with boot(None, root=True, cmdline="usbstorage.fault=maxlun:2", name="t-usbs-luns", extra=XHCI + stick(a)) as (g, vm):
            out = g.ok("lsblk")
            assert re.search(r"^sdb\s+100M\s+0\s+disk\s+usb-storage", out, re.M) and not re.search(r"^sdc\s", out, re.M), out
            out = g.ok("cat /proc/usbstorage")
            assert "3 LUNs" in out and re.search(r"lun 0: online .* sdb 204800 x 512 rw", out), out
            assert len(re.findall(r"lun [12]: given up \(no device at this LUN\)", out)) == 2, out
            assert "/dev/sdb2 on /HumanVillage" in g.ok("mount")
            no_incident(vm)
            g.poweroff()
        a.check()


@test
def usbstor_driver_off():
    """usbstorage=off: the stick is listed by lsusb without a driver, no disk appears, nothing is mounted or written."""
    with stick_image("usbs-off") as img:
        h0 = img.sha()
        with boot(None, root=True, cmdline="usbstorage=off", name="t-usbs-off", extra=XHCI + stick(img)) as (g, vm):
            wait_log(vm, r"usb 1-\d+: new \S+ device 46f4:0001", timeout=60)
            assert "usb-storage: disabled on the kernel command line (usbstorage=off)" in vm.text()
            assert re.search(r"If 0: class 08/06/50, 2 endpoints, driver none", g.ok("lsusb"))
            assert "sdb" not in g.ok("cat /proc/partitions") and "HumanVillage" not in g.ok("mount")
            g.poweroff()
        assert img.sha() == h0


# ---------------------------------------------------------------------------------------------------------------------
# block size, time stamps

@test
def usbstor_4k_blocks():
    """A stick with 4096-byte logical blocks: the partition table (4Kn GPT) is read, HUMANVILLAGE mounts and survives a reboot."""
    parts = [hv_part({"/reimu/hello.txt": b"4k hello\n"}, cluster=4096)]
    with Image("usbs-4k", 300 * MIB, parts, sector_size=4096) as img:
        with boot(None, root=True, name="t-usbs-4k1", extra=XHCI + stick(img, bs=4096)) as (g, vm):
            assert re.search(r"4096-byte blocks", vm.text()), vm.text()[-2500:]
            assert re.search(r"^sdb\s+300M\s+0\s+disk\s+usb-storage", g.ok("lsblk"), re.M)
            assert re.search(r"^sdb 314572800 4096 0 ", g.ok("cat /proc/partitions"), re.M)
            assert say(g, f"cat {H}/hello.txt") == "4k hello"
            front = open(img.path, "rb").read(MIB)                       # the GPT area: nothing writes there
            assert sha(front) in g.ok("dd if=/dev/sdb bs=4096 count=256 2>/dev/null | sha256sum"), "the front of a 4Kn stick differs"
            g.ok(f"seq 1 100000 > {HV}/seq.txt")
            g.poweroff()
        img.check()
        assert host_read(img, "/seq.txt", part=1) == "".join(f"{i}\n" for i in range(1, 100001)).encode()
        with boot(None, root=True, name="t-usbs-4k2", extra=XHCI + stick(img, bs=4096)) as (g, vm):
            assert sha("".join(f"{i}\n" for i in range(1, 100001)).encode()) in g.ok(f"sha256sum {HV}/seq.txt")
            g.poweroff()


@test
def usbstor_timestamps():
    """FAT keeps local time: with tz=JST-9 the boot-time mount shows UTC+9 on disk and reads it back; the mount applet passes the
    current offset of its own zone; without a zone nothing changes (UTC, as before)."""
    stamp = "2030-01-02 03:04:05"
    with stick_image("usbs-tz", files={"/reimu/old.txt": b"old\n"}) as img:
        # 1. tz=JST-9 on the command line: the policy mounts with tzoffset=540
        with boot(None, root=True, cmdline="tz=JST-9", name="t-usbs-tz1", extra=XHCI + stick(img)) as (g, vm):
            assert "times are local (UTC+9:00)" in vm.text(), vm.text()[-2500:]
            # the file the host made has its fields set to 2026-01-01 00:00:00 = local time; read as JST that is 2025-12-31 15:00:00 UTC
            assert "Modify: 2025-12-31 15:00:00 +0000" in g.ok(f"TZ=UTC stat {H}/old.txt"), g.ok(f"stat {H}/old.txt")
            g.ok(f"date -u -s '{stamp}' > /dev/null")
            g.ok(f"echo new > {HV}/new.txt")
            g.ok(f"mkdir {HV}/newdir")
            assert re.search(r"Modify: 2030-01-02 03:0[45]:\d\d \+0000", g.ok(f"TZ=UTC stat {HV}/new.txt")), g.ok(f"TZ=UTC stat {HV}/new.txt")
            g.poweroff()
        r = mkdisk.open_volume(img.path, 2)
        try:
            e = r.find("/new.txt")
            assert time.strftime("%Y-%m-%d %H:%M", time.gmtime(e.mtime)) == "2030-01-02 12:04", time.gmtime(e.mtime)     # the fields hold local time
            d = r.find("/newdir")
            assert time.strftime("%Y-%m-%d %H", time.gmtime(d.mtime)) == "2030-01-02 12", time.gmtime(d.mtime)
        finally:
            r.close()
        # 2. no tz=: UTC on disk, exactly as before
        with boot(None, root=True, name="t-usbs-tz2", extra=XHCI + stick(img)) as (g, vm):
            assert "times are local" not in vm.text()
            assert "Modify: 2026-01-01 00:00:00 +0000" in g.ok(f"TZ=UTC stat {H}/old.txt"), g.ok(f"stat {H}/old.txt")
            g.ok(f"date -u -s '{stamp}' > /dev/null")
            g.ok(f"echo utc > {HV}/utc.txt")
            g.poweroff()
        r = mkdisk.open_volume(img.path, 2)
        try:
            assert time.strftime("%Y-%m-%d %H:%M", time.gmtime(r.find("/utc.txt").mtime)) == "2030-01-02 03:04"
        finally:
            r.close()
        # 3. the mount applet: nohumanvillage, then mount by hand in a zone with summer time (winter date: EST, UTC-5)
        us = "EST5EDT,M3.2.0,M11.1.0"
        with boot(None, root=True, cmdline="nohumanvillage", name="t-usbs-tz3", extra=XHCI + stick(img)) as (g, vm):
            g.ok("date -u -s '2030-01-02 15:00:00' > /dev/null")
            g.ok(f"TZ={us} mount -t fat32 /dev/sdb2 /mnt")
            g.ok("echo winter > /mnt/winter.txt")
            g.ok("umount /mnt")
            g.ok("date -u -s '2030-07-02 15:00:00' > /dev/null")
            g.ok(f"TZ={us} mount -t fat32 /dev/sdb2 /mnt")
            g.ok("echo summer > /mnt/summer.txt")
            g.ok("umount /mnt")
            g.ok("date -u -s '2030-07-02 15:00:00' > /dev/null")
            g.ok("mount -t fat32 -o tzoffset=90 /dev/sdb2 /mnt")                  # explicit option wins
            g.ok("echo explicit > /mnt/explicit.txt")
            g.ok("umount /mnt")
            g.poweroff()
        r = mkdisk.open_volume(img.path, 2)
        try:
            when = lambda p: time.strftime("%Y-%m-%d %H:%M", time.gmtime(r.find(p).mtime))
            assert when("/winter.txt") == "2030-01-02 10:00", when("/winter.txt")      # 15:00 UTC is 10:00 in New York in January
            assert when("/summer.txt") == "2030-07-02 11:00", when("/summer.txt")      # and 11:00 in July
            assert when("/explicit.txt") == "2030-07-02 16:30", when("/explicit.txt")
        finally:
            r.close()
        img.check()


# ---------------------------------------------------------------------------------------------------------------------
# protocol trouble

@test
def usbstor_reset_recovery():
    """A command wrapper the stick stalls and a corrupted status wrapper, in the middle of a long read and of a long write, are answered
    with the bulk-only reset recovery: the data stays correct, the counters show the recoveries, the stick is not given up on."""
    with stick_image("usbs-recover") as img:
        front = open(img.path, "rb").read(35 * MIB)                    # the part of the stick nobody writes
        with boot(None, root=True, cmdline="usbstorage.fault=stall:200,csw:330", name="t-usbs-recover", extra=XHCI + stick(img)) as (g, vm):
            assert sha(front) in g.ok("dd if=/dev/sdb bs=1M count=35 2>/dev/null | sha256sum", timeout=240)      # 560 commands: both faults land inside
            text = vm.text()
            assert re.search(r"fault injection: command wrapper 200 goes out with a damaged signature", text), text[-3000:]
            assert re.search(r"fault injection: status wrapper 330 is corrupted", text), text[-3000:]
            assert "given up on" not in text
            out = g.ok("cat /proc/usbstorage")
            m = re.search(r"(\d+) reset recover", out)
            assert m and int(m.group(1)) >= 2, out
            assert "/dev/sdb2 on /HumanVillage" in g.ok("mount")
            g.ok(f"seq 1 400000 > {HV}/numbers.txt")
            no_incident(vm)
            g.poweroff()
        img.check()
        assert host_read(img, "/numbers.txt") == NUMBERS


@test
def usbstor_recovery_in_bringup():
    """The same faults while the stick is being brought up (the third command stalls, the seventh status is corrupted): the bring-up
    recovers and carries on, the disk is registered, HUMANVILLAGE mounts."""
    with stick_image("usbs-recover2") as img:
        with boot(None, root=True, cmdline="usbstorage.fault=stall:3,csw:7", name="t-usbs-recover2", extra=XHCI + stick(img)) as (g, vm):
            text = vm.text()
            assert "fault injection: command wrapper 3 goes out with a damaged signature" in text and "fault injection: status wrapper 7 is corrupted" in text, text[-3000:]
            assert "given up on" not in text
            m = re.search(r"(\d+) reset recover", g.ok("cat /proc/usbstorage"))
            assert m and int(m.group(1)) >= 2
            assert "/dev/sdb2 on /HumanVillage" in g.ok("mount")
            assert say(g, f"cat {H}/hello.txt") == "hello from the stick"
            g.poweroff()


@test
def usbstor_silent_stick_bounded():
    """A stick that stops answering (the second command gets no answer): the bring-up gives up after the timeout, boot is not held up
    beyond the bounded wait, no disk appears, and the rest of the machine works. A hang in the middle of I/O drops the disk and fails the request."""
    with stick_image("usbs-silent") as img:
        h0 = img.sha()
        t0 = time.time()
        with boot(None, root=True, cmdline="usbstorage.fault=drop:2 usbstorage.timeout=1500", name="t-usbs-silent", extra=XHCI + stick(img)) as (g, vm):
            text = vm.text()
            assert re.search(r"fault injection: command wrapper 2 gets no answer", text), text[-3000:]
            wait_log(vm, r"usb-storage \S+: no answer to TEST UNIT READY -- the stick is given up on", timeout=60)
            out = g.ok("cat /proc/usbstorage; lsblk")
            assert "given up on" in out and not re.search(r"^sdb\s", out, re.M), out
            assert "still busy after" not in vm.text(), "the boot wait should have ended when the stick was given up on"
            assert "HumanVillage" not in g.ok("mount")
            no_incident(vm)
            assert say(g, "echo alive") == "alive"
            g.poweroff()
        assert img.sha() == h0


@test
def usbstor_sync_hang():
    """A stick that hangs on SYNCHRONIZE CACHE but answers everything else is not given up on: one reset, it is not asked to sync again,
    and the volume keeps working (writes still land)."""
    with stick_image("usbs-synchang") as img:
        with boot(None, root=True, cmdline="usbstorage.fault=syncdrop:2 usbstorage.timeout=4000", name="t-usbs-synchang", extra=XHCI + stick(img)) as (g, vm):
            wait_log(vm, r"usb-storage \S+: SYNCHRONIZE CACHE hung the stick; it answered again after a reset and will not be asked to sync any more", timeout=90)
            assert "given up on" not in vm.text(), vm.text()[-2500:]
            out = g.ok("cat /proc/usbstorage")
            assert re.search(r"lun 0: online", out) and re.search(r"1 reset recover", out), out
            g.ok(f"seq 1 400000 > {HV}/numbers.txt")
            no_incident(vm)
            g.poweroff()
        img.check()
        assert host_read(img, "/numbers.txt") == NUMBERS


@test
def usbstor_late_ready():
    """A stick that needs 12 s to become ready: boot waits the bounded 8 s and goes on without it; when it is ready the hot-plug hook
    mounts its HUMANVILLAGE volume. A stick that never gets ready (30 s) is given up on after 20 s."""
    with stick_image("usbs-late") as img:
        with Qemu(cmdline="text user=root usbstorage.fault=notready:12000", name="t-usbs-late", extra=XHCI + stick(img)) as vm:
            vm.expect(ROOT_PROMPT, timeout=180)
            g = Guest(vm, True)
            text = vm.text()
            assert re.search(r"\[Kappa\] usb: still busy after 8000 ms", text), text[-3000:]
            assert "HumanVillage" not in g.ok("mount")                 # the session started without the stick's volume
            wait_log(vm, r"\[Keine\] the Human Village keeps its records on the stick now", timeout=180)
            assert say(g, f"cat {H}/hello.txt") == "hello from the stick"
            no_incident(vm)
            g.poweroff()
        with Qemu(cmdline="text user=root usbstorage.fault=notready:30000", name="t-usbs-never", extra=XHCI + stick(img)) as vm:
            vm.expect(ROOT_PROMPT, timeout=180)
            g = Guest(vm, True)
            wait_log(vm, r"usb-storage \S+ lun 0: \"QEMU QEMU HARDDISK\" did not become ready within 20 seconds -- given up on", timeout=240)
            assert "sdb" not in g.ok("cat /proc/partitions")
            assert "given up" in g.ok("cat /proc/usbstorage")
            g.poweroff()


@test
def usbstor_selftests():
    """The validators (capacity, block size, status wrapper, sense data, INQUIRY) against hand-made hostile answers, and the TZ parser."""
    with stick_image("usbs-self") as img:
        with Qemu(cmdline="text usbstorage.selftest storage.selftest", name="t-usbs-selftest", extra=XHCI + stick(img)) as vm:
            m = vm.expect(r"storage selftest: (\d+) time zone checks, (\d+) failed", timeout=120)
            assert int(m.group(1)) >= 10 and m.group(2) == "0", vm.text()[-2500:]
            m = vm.expect(r"usb-storage selftest: (\d+) checks, (\d+) failed", timeout=120)
            assert int(m.group(1)) >= 25 and m.group(2) == "0", vm.text()[-2500:]
            assert "selftest:" not in re.sub(r"(usb-)?storage selftest: \d+ (checks|time zone checks), 0 failed", "", vm.text()), vm.text()[-2500:]


# ---------------------------------------------------------------------------------------------------------------------
# the kernel log on the stick

@test
def usbstor_bootlog_clean():
    """TOUHOU.LOG on the stick holds the whole boot log (from the first line), grows while the system runs, ends with the shutdown line
    after poweroff; the next boot rotates it to TOUHOU.PRV."""
    with stick_image("usbs-log") as img:
        with boot(None, root=True, name="t-usbs-log1", extra=XHCI + stick(img)) as (g, vm):
            assert re.search(r"the kernel log is kept on the stick: TOUHOU.LOG", vm.text())
            log = g.ok(f"cat {HV}/TOUHOU.LOG")
            assert "[Gensokyo] kernel 0.1.0 \"Story of Eastern Wonderland\" starting" in log[:2500], log[:300]      # after the banner the console printed first
            assert "[Keine] fat32: sdb2 mounted on /HumanVillage" in log, log[-500:]
            g.ok("logger -t testing a line that must reach the stick")
            deadline = time.time() + scaled(60)
            while time.time() < deadline and "a line that must reach the stick" not in g.ok(f"cat {HV}/TOUHOU.LOG"):
                time.sleep(1)                                          # the writer thread appends every ~5 s
            assert "a line that must reach the stick" in g.ok(f"cat {HV}/TOUHOU.LOG")
            g.poweroff()
        rep = img.check()
        assert rep.stats["p2"]["clean"]
        text = host_read(img, "/TOUHOU.LOG").decode()
        assert "[Gensokyo] kernel 0.1.0 \"Story of Eastern Wonderland\" starting" in text[:2500], text[:300]
        for needle in ("[Marisa] blk: sdb  usb-storage", "[Keine] fat32: sdb2 mounted on /HumanVillage", "boot complete", "[Alice] init: /sbin/init",
                       "a line that must reach the stick"):
            assert needle in text, f"{needle!r} missing from TOUHOU.LOG:\n" + text[-1500:]
        assert text.rstrip().endswith("this line ends a clean run --"), text[-300:]
        first_boot = text
        with boot(None, root=True, name="t-usbs-log2", extra=XHCI + stick(img)) as (g, vm):
            g.poweroff()
        img.check()
        prv = host_read(img, "/TOUHOU.PRV").decode()
        assert prv == first_boot, "TOUHOU.PRV is not the previous boot's TOUHOU.LOG"
        new = host_read(img, "/TOUHOU.LOG").decode()
        assert "[Gensokyo] kernel 0.1.0" in new[:2500] and new != prv and new.rstrip().endswith("this line ends a clean run --")


@test
def usbstor_bootlog_killed():
    """The machine dies right after the volume was mounted (power cut, hang): TOUHOU.LOG is already on the stick with the boot lines up
    to the mount, and the stick is still a consistent FAT volume."""
    with stick_image("usbs-kill") as img:
        with Qemu(cmdline="text user=root", name="t-usbs-kill", extra=XHCI + stick(img)) as vm:
            vm.expect(r"\[Keine\] bootlog: TOUHOU.LOG holds the boot log up to here", timeout=180)
            vm.proc.kill()                                              # the plug is pulled: no sync, no poweroff
            vm.proc.wait(timeout=scaled(20))
        rep = mkdisk.check_image(img.path)
        assert rep.ok, "the stick is damaged after the kill:\n" + rep.text(True)
        text = host_read(img, "/TOUHOU.LOG").decode()
        assert "[Gensokyo] kernel 0.1.0 \"Story of Eastern Wonderland\" starting" in text[:2500], text[:300]
        assert "[Keine] fat32: sdb2 mounted on /HumanVillage" in text and "[Marisa] pci" in text, text[-1500:]
        assert "this line ends a clean run" not in text, "a killed machine cannot have said goodbye"


@test
def usbstor_bootlog_never_when_unsafe():
    """No TOUHOU.LOG on a read-only stick, a dirty volume (mounted read-only by the policy) or with bootlog=off; the images stay as they were."""
    # read-only stick
    with stick_image("usbs-log-ro") as img:
        h0 = img.sha()
        with boot(None, root=True, name="t-usbs-log-ro", extra=XHCI + stick(img, ro=True)) as (g, vm):
            g.fails(f"cat {HV}/TOUHOU.LOG 2>&1", r"No such file")
            assert re.search(r"TOUHOU.LOG is not kept: the Human Village is mounted read-only", vm.text()), vm.text()[-2000:]
            g.poweroff()
        assert img.sha() == h0
    # dirty volume (the last user pulled the stick without ejecting it)
    with stick_image("usbs-log-dirty") as img:
        mkdisk.set_dirty(img.path, 2, True)
        h0 = img.sha()
        with boot(None, root=True, name="t-usbs-log-dirty", extra=XHCI + stick(img)) as (g, vm):
            assert re.search(r"read-only: the volume was not cleanly unmounted", vm.text()), vm.text()[-2500:]
            assert re.search(r"TOUHOU.LOG is not kept: the Human Village is mounted read-only", vm.text())
            g.poweroff()
        assert img.sha() == h0, "a dirty volume was written to"
    # bootlog=off
    with stick_image("usbs-log-off") as img:
        with boot(None, root=True, cmdline="bootlog=off", name="t-usbs-log-off", extra=XHCI + stick(img)) as (g, vm):
            assert "bootlog=off: the kernel log is not kept on the stick" in vm.text()
            g.poweroff()
        r = mkdisk.open_volume(img.path, 2)
        try:
            assert r.find("/TOUHOU.LOG") is None
        finally:
            r.close()


# ---------------------------------------------------------------------------------------------------------------------
# the real thing: booting from the stick itself

@test
def usbstor_boot_from_stick():
    """The image built by tools/mkimage.py on a USB stick, the way a PC boots it: the firmware loads the kernel from the stick, the
    kernel takes over the controller, and the stick's HUMANVILLAGE partition is /HumanVillage -- persistently, over two boots."""
    img = BUILD / "t-usbs-boot.img"
    out = subprocess.run([sys.executable, str(ROOT / "tools" / "mkimage.py"), "--no-build", "--out", str(img), "--size", "256M", "--cmdline", "text user=root blk.ro"],
                         capture_output=True, text=True)
    assert out.returncode == 0, out.stdout[-800:] + out.stderr[-800:]
    try:
        esp0 = _part_hash(img, 1)
        for boot_no in (1, 2):
            with Qemu(cmdline="", name=f"t-usbs-boot{boot_no}", mem="768M", boot_image=img, boot_bus="usb") as vm:
                vm.expect(ROOT_PROMPT, timeout=180)
                g = Guest(vm, True)
                text = vm.text()
                assert re.search(r"cmdline 'text user=root blk.ro'", text)
                m_mount = re.search(r"\[Keine\] fat32: sda2 mounted on /HumanVillage", text)
                assert m_mount, text[-3000:]
                assert re.search(r"^sda\s+\S+\s+0\s+disk\s+usb-storage", g.ok("lsblk"), re.M)       # blk.ro does not apply to a stick
                if boot_no == 1:
                    assert "No such file" in g.fails(f"cat {HV}/persist.txt 2>&1")
                    g.ok(f"echo written in the first boot > {HV}/persist.txt")
                else:
                    assert say(g, f"cat {HV}/persist.txt") == "written in the first boot"
                    assert "not cleanly unmounted" not in text
                g.poweroff()
        rep = mkdisk.check_image(img)
        assert rep.ok and not rep.warnings, rep.text(True)
        assert _part_hash(img, 1) == esp0, "the ESP of the boot stick changed"
        assert host_read_path(img, "/persist.txt") == b"written in the first boot\n"
    finally:
        try:
            img.unlink()
        except OSError:
            pass


def _part_hash(path, index):
    _, parts = mkdisk.partitions(path)
    p = [x for x in parts if x["index"] == index][0]
    return file_sha(path, p["offset"], p["length"])


def host_read_path(path, name, part=2):
    r = mkdisk.open_volume(path, part)
    try:
        return r.read_file(name)
    finally:
        r.close()
