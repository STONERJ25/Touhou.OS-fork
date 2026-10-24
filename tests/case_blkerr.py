"""Failures on the write path of the AHCI and NVMe drivers, injected in QEMU (docs/HARDWARE_AUDIT.md, "Write-path review").

What QEMU 8.2 can do here, and what these tests use:
  * blkdebug `inject-error` rules with `iotype=write|flush` (and `sector=`, `once=on`): a write or a cache flush that the disk answers
    with an error. QEMU's AHCI turns it into a task file error (ERR, ABRT), its NVMe into status type 2 code 0x80 (write fault, DNR
    clear). Several one-shot rules on one sector make an error that outlives the NVMe driver's own retries and then goes away.
  * QMP `block_set_io_throttle` at run time: a disk that suddenly takes far longer than the driver's timeout for a write (a slow device),
    with real data underneath, so the image can be inspected afterwards. QEMU finishes or releases the throttled request when the guest
    resets the port or the controller.
  * `ahci.losewrite=LBA` (a kernel test knob): a write that the disk reports done but never stores, for install's read-back.
Not simulated: a device that never completes a command *and* ignores resets (QEMU's device models wait for outstanding backend I/O when the
guest resets them, so a blkdebug breakpoint that is never resumed hangs QEMU itself); a torn write on power loss; an NVMe controller that sets
CSTS.CFS by itself; media errors that the drive reports only after its own long internal retries. The lost-command knobs `ahci.dropcmd` /
`nvme.dropcmd` (tests/case_blockdev.py) cover a command the device never answers.
"""
import re
import struct
import time
from pathlib import Path

from harness import test, scaled
from case_blockdev import Guest, pattern_image, pattern_slice, sha, device_by_model, must, q, MiB
import mkdisk

IMG_DIR = Path(__file__).resolve().parent.parent / "build" / "blkerr"


def faulty(did, img, rules):
    """-drive arguments for img behind blkdebug; rules: [{'iotype': 'write', 'sector': 123, 'once': 'on'}, ...] (errno 5 = EIO)."""
    spec = f"if=none,id={did},driver=raw,file.driver=blkdebug,file.image.driver=file,file.image.filename={q(img)},rerror=report,werror=report"
    for i, r in enumerate(rules):
        spec += f",file.inject-error.{i}.event=none,file.inject-error.{i}.errno=5"
        spec += "".join(f",file.inject-error.{i}.{k}={v}" for k, v in r.items())
    return ["-drive", spec]


def sata(did, img, bus, model, rules=()):
    return (faulty(did, img, rules) if rules else ["-drive", f"if=none,id={did},file={q(img)},format=raw"]) + \
        ["-device", f"ide-hd,drive={did},bus={bus},model={model}"]


def nvme(cid, serial, img, rules=(), ns_props=""):
    did = f"{cid}-d0"
    return (faulty(did, img, rules) if rules else ["-drive", f"if=none,id={did},file={q(img)},format=raw"]) + \
        ["-device", f"nvme,id={cid},serial={serial}", "-device", f"nvme-ns,drive={did},bus={cid},nsid=1{ns_props}"]


def fat_image(name, size_mib):
    """A GPT disk with one FAT32 partition labelled DATA. Returns (path, first sector of the data area (cluster 2), on the disk)."""
    IMG_DIR.mkdir(parents=True, exist_ok=True)
    p = IMG_DIR / f"{name}.img"
    if p.exists():
        p.unlink()
    mkdisk.build_image(str(p), size_mib * MiB, [dict(name="DATA", kind="fat32", size="rest", label="DATA",
                                                     files={"/keep.txt": b"keep me\n" * 100})], seed=5)
    _, parts = mkdisk.partitions(str(p))
    with open(p, "rb") as f:
        f.seek(parts[0]["offset"])
        bs = f.read(512)
    reserved, nfats, fatsz = struct.unpack_from("<H", bs, 14)[0], bs[16], struct.unpack_from("<I", bs, 36)[0]
    return p, parts[0]["offset"] // 512 + reserved + nfats * fatsz


def fat_check(img):
    rep = mkdisk.check_image(str(img))
    assert rep.ok, f"{img.name}: the file system checker found errors:\n{rep.text(True)}"
    return rep


def alive(g):
    assert g.ok("echo still-alive", timeout=60).strip() == "still-alive", "the shell no longer answers"


# --------------------------------------------------------------------------------------------------------------- flush

@test
def blk_err_flush():
    """A disk whose cache flush fails: the write that needed the flush fails with EIO on SATA (task file error; the flush is sent once
    more for the rest of the cache) and on NVMe (write fault, retried three times); both disks keep working for reads."""
    simg, nimg = pattern_image("errflush-sata", 4), pattern_image("errflush-nvme", 4)
    data = pattern_slice(0, 4 * MiB)
    extra = sata("fs", simg, "ide.1", "FLUSHFAIL-SATA", [dict(iotype="flush")]) + nvme("fn", "FLUSHF01", nimg, [dict(iotype="flush")])
    with Guest("t-blk-err-flush", extra) as g:
        dev = device_by_model(g.partitions(), "FLUSHFAIL-SATA")
        for node, model in ((dev, "sata"), ("nvme0n1", "nvme")):
            out, rc = g.run(f"dd if=/dev/zero of=/dev/{node} bs=4096 count=1 seek=16 conv=notrunc")
            must(out, rf"error writing '/dev/{node}': Input/output error", f"({model}: a failed flush must fail the write)")
        log = g.ok("dmesg")
        must(log, r"ahci0 port 1: command ea \(lba 0, 0 sectors\) failed: device reported an error")
        must(log, r"ahci0 port 1: the cache flush failed; flushed once more for the rest of the cache \(failed again\); the write is reported as failed")
        must(log, r"nvme0: flush of nsid 1: status type 2 code 0x80, trying again \(1 of 3\)")
        must(log, r"nvme0: flush of nsid 1 failed: status type 2 code 0x80")
        # reads still work, nothing was given up on
        assert g.sha_of(f"dd if=/dev/{dev} bs=4096 skip=32 count=8 status=none | sha256sum") == sha(data[32 * 4096:40 * 4096])
        assert g.sha_of("dd if=/dev/nvme0n1 bs=4096 skip=32 count=8 status=none | sha256sum") == sha(data[32 * 4096:40 * 4096])
        stats = g.ok("cat /proc/ahci /proc/nvme")
        assert "DEAD" not in stats, stats
        must(stats, r'"FLUSHFAIL-SATA".*errors [1-9]')
        alive(g)
    for img in (simg, nimg):
        b = img.read_bytes()
        assert b[:16 * 4096] == data[:16 * 4096] and b[17 * 4096:] == data[17 * 4096:], f"{img.name}: bytes outside the written block changed"


# ------------------------------------------------------------------------------------------------- FAT write-back errors

@test
def blk_err_fat_writeback():
    """A FAT32 volume whose disk fails the write-back: `sync` reports EIO, umount refuses (the data stays in memory), the volume's
    metadata on the disk is not touched (data is written before the FAT and the directory); a one-off error heals with the next sync
    and the volume then checks clean with the file intact."""
    simg, s_data = fat_image("errfat-sata", 64)
    nimg, n_data = fat_image("errfat-nvme", 64)
    s_bad, n_bad = s_data + 600, n_data + 600            # inside the first MiB of free clusters, where the new file goes
    extra = (sata("es", simg, "ide.1", "FATFAIL-SATA", [dict(iotype="write", sector=s_bad)]) +
             nvme("en", "FATFAIL1", nimg, [dict(iotype="write", sector=n_bad, once="on") for _ in range(4)]))   # one more than the driver retries
    with Guest("t-blk-err-fat", extra) as g:
        dev = device_by_model(g.partitions(), "FATFAIL-SATA")
        g.ok(f"mkdir -p /tmp/s /tmp/n && mount -t fat32 /dev/{dev}1 /tmp/s && mount -t fat32 /dev/nvme0n1p1 /tmp/n")
        for d in ("s", "n"):
            g.ok(f"dd if=/dev/urandom of=/tmp/{d}.bin bs=64k count=16 2>/dev/null && cp /tmp/{d}.bin /tmp/{d}/big.bin")
        want = {d: re.search(r"[0-9a-f]{64}", g.ok(f"sha256sum /tmp/{d}.bin")).group(0) for d in ("s", "n")}
        # the persistent error on SATA: sync says so, again and again, and umount refuses
        out, rc = g.run("sync")
        assert rc == 1 and "writing back to a disk failed: Input/output error" in out, f"sync must report the failed write-back (rc {rc}):\n{out}"
        out, rc = g.run("sync")
        assert rc == 1, "the data is still dirty, so a second sync must fail too"
        out, rc = g.run("umount /tmp/s")
        assert rc != 0, f"umount must refuse while the volume cannot be written back:\n{out}"
        log = g.ok("dmesg")
        must(log, rf"ahci0 port 1: command 35 \(lba \d+, \d+ sectors\) failed: device reported an error")
        must(log, rf"fat32: cannot unmount {dev}1: writing the volume back failed \(-5\)")
        must(log, r"nvme0: write of \d+ blocks at lba \d+ on nsid 1 failed: status type 2 code 0x80")
        assert want["s"] in g.ok("sha256sum /tmp/s/big.bin"), "the file must still read correctly from memory"
        # the one-off error on NVMe was reported by one of the syncs above (or the flusher thread) and is gone now: umount writes the rest
        out, rc = g.run("umount /tmp/n")
        assert rc == 0, f"once the error is gone the NVMe volume must unmount cleanly:\n{out}"
        must(g.ok("dmesg"), r"fat32: nvme0n1p1 unmounted cleanly")
        alive(g)
        g.vm.proc.kill()                                     # pull the plug on the SATA volume that cannot be written back
        g.vm.proc.wait()
    rep = fat_check(nimg)
    assert rep.stats["p1"]["clean"], "the NVMe volume was unmounted cleanly"
    v = mkdisk.open_volume(str(nimg), 1)
    try:
        assert sha(v.read_file("/big.bin")) == want["n"], "the file on the healed NVMe volume differs"
        assert v.read_file("/keep.txt") == b"keep me\n" * 100
    finally:
        v.close()
    fat_check(simg)                                          # no errors: dirty at most, nothing points at data that never arrived
    v = mkdisk.open_volume(str(simg), 1)
    try:
        assert v.find("/big.bin") is None, "the directory entry must not reach the disk before its data"
        assert v.read_file("/keep.txt") == b"keep me\n" * 100
    finally:
        v.close()


# ----------------------------------------------------------------------------------------------------- slow device

@test
def blk_err_slow_device():
    """A disk that suddenly takes far longer than the driver's timeout (QEMU I/O throttling switched on at run time): the write fails
    with EIO instead of hanging, the shell answers meanwhile; SATA resets the link and keeps the disk, NVMe gives the controller up and
    later requests fail at once; every sector holds either the old or the new data; the other disk is not affected."""
    simg, nimg = pattern_image("slow-sata", 4), pattern_image("slow-nvme", 4)
    data = pattern_slice(0, 4 * MiB)
    extra = sata("ss", simg, "ide.1", "SLOW-SATA") + nvme("sn", "SLOW0001", nimg)
    with Guest("t-blk-err-slow", extra, cmdline="ahci.iotimeout=2000 nvme.iotimeout=2000") as g:
        dev = device_by_model(g.partitions(), "SLOW-SATA")
        for drive_id, node in (("ss", dev), ("sn-d0", "nvme0n1")):
            # 4 KiB/s for writes: the first 64 KiB request passes, the second has to wait about 16 s (the driver gives up after 2 s)
            g.vm.qmp("block_set_io_throttle", device=drive_id, bps=0, bps_rd=0, bps_wr=4096, iops=0, iops_rd=0, iops_wr=0)
            g.ok(f"rm -f /tmp/dd.log; dd if=/dev/zero of=/dev/{node} bs=64k count=2 seek=4 conv=notrunc > /tmp/dd.log 2>&1 &")
            alive(g)                                         # the system is not stuck while the disk is
            deadline = time.time() + scaled(300)
            while "records out" not in g.ok("cat /tmp/dd.log"):
                assert time.time() < deadline, "dd never finished:\n" + g.ok("cat /tmp/dd.log; dmesg | tail -20")
                time.sleep(2)
            must(g.ok("cat /tmp/dd.log"), rf"error writing '/dev/{node}': Input/output error")
            g.vm.qmp("block_set_io_throttle", device=drive_id, bps=0, bps_rd=0, bps_wr=0, iops=0, iops_rd=0, iops_wr=0)
        log = g.ok("dmesg")
        must(log, r"ahci0 port 1: command 35 \(lba \d+, 128 sectors\) failed: timeout")
        must(log, r"ahci0 port 1: link reset done, disk is back")
        must(log, r"nvme0: I/O command timed out \(CSTS 0x[0-9a-f]+\); giving up on this controller")
        # SATA: alive and correct after the episode; NVMe: fails at once from now on
        g.ok(f"dd if=/dev/zero of=/dev/{dev} bs=4096 count=1 seek=100 conv=notrunc status=none")
        assert g.sha_of(f"dd if=/dev/{dev} bs=4096 skip=200 count=8 status=none | sha256sum") == sha(data[200 * 4096:208 * 4096])
        t0 = time.time()
        out, rc = g.run("dd if=/dev/nvme0n1 of=/dev/null bs=4096 count=1")
        must(out, r"Input/output error")
        assert time.time() - t0 < scaled(10), "a controller that was given up on must fail requests at once"
        stats = g.ok("cat /proc/ahci /proc/nvme")
        must(stats, r'"SLOW-SATA".*resets [1-9]')
        assert re.search(r'"SLOW-SATA"[^\n]*DEAD', stats) is None, stats
        must(stats, r"nvme0 [^\n]*DEAD")
    zero = bytes(512)
    for img in (simg, nimg):
        b = img.read_bytes()
        for s in range(len(b) // 512):
            got, old = b[s * 512:(s + 1) * 512], data[s * 512:(s + 1) * 512]
            in_dd = 4 * 128 <= s < 6 * 128 or (img == simg and 800 <= s < 808)
            assert got == old or (in_dd and got == zero), f"{img.name}: sector {s} holds neither the old nor the new data"


# ---------------------------------------------------------------------------------------------------------- the installer

def esp_fat_params(sectors, bps=512):
    """(sectors per cluster, sectors per FAT, first data sector) of the ESP install.c makes (fat_init(), the same loop)."""
    size = sectors * bps
    cb = 512 if size <= 64 * MiB else 1024 if size <= 128 * MiB else 2048 if size <= 256 * MiB else 4096
    cb = max(cb, bps)
    spc, per_sector = cb // bps, bps // 4
    fatsz = (sectors // spc + 2) // per_sector
    fatsz = fatsz - 16 if fatsz > 16 else 1
    while (sectors - 32 - 2 * fatsz) // spc + 2 > fatsz * per_sector:
        fatsz += 1
    return spc, fatsz, 32 + 2 * fatsz


@test
def blk_err_install_write():
    """install meets a failing disk: a write error in the middle (SATA) stops it with the place and the disk's error, and the partition
    table is never written; a cache flush that fails at the end (NVMe, the driver's own flushes switched off) fails it too."""
    from case_install import blank, INSTALLED_CMDLINE
    simg, nimg = blank("err-write-sata", 256), blank("err-flush-nvme", 256)
    bad = 2048 + 32 + 1                                      # the ESP's first FAT (partition 1 starts at 1 MiB, 32 reserved sectors)
    extra = sata("iw", simg, "ide.1", "INSTFAIL-SATA", [dict(iotype="write", sector=bad)]) + nvme("if", "INSTFL01", nimg, [dict(iotype="flush")])
    with Guest("t-blk-err-install", extra, cmdline="blk.ro nvme.nosync") as g:
        dev = device_by_model(g.partitions(), "INSTFAIL-SATA")
        out, rc = g.run(f"install --yes-i-typed={dev} --cmdline '{INSTALLED_CMDLINE}' {dev}", timeout=900)
        assert rc == 1, f"install must fail (rc {rc}):\n{out}"
        must(out, rf"install: writing {dev} failed at byte {2080 * 512} \(sector 2080, the FATs of TOUHOUOS-ESP\): Input/output error")
        must(out, r"TouhouOS is NOT installed")
        assert "TouhouOS is installed" not in out and "writing the GPT" not in out, out
        out, rc = g.run(f"install --yes-i-typed=nvme0n1 --cmdline '{INSTALLED_CMDLINE}' nvme0n1", timeout=900)
        assert rc == 1, f"install must fail when the final flush fails (rc {rc}):\n{out}"
        must(out, r"install: flushing the write cache of nvme0n1 failed: Input/output error")
        must(out, r"TouhouOS is NOT reliably installed")
        log = g.ok("dmesg")
        must(log, rf"ahci0 port 1: command 35 \(lba \d+, \d+ sectors\) failed: device reported an error")
        must(log, r"nvme0: flush of nsid 1 failed: status type 2 code 0x80")
        must(log, rf"blk: {dev} is read-only again")
        must(log, r"blk: nvme0n1 is read-only again")
        alive(g)
    b = simg.read_bytes()
    assert b[:34 * 512] == bytes(34 * 512) and b[-33 * 512:] == bytes(33 * 512), "the GPT and the MBR must not be written after a failed write"


@test
def blk_err_install_readback():
    """install's read-back: a disk that silently loses writes (ahci.losewrite) in six places, the protective MBR, the backup GPT header,
    the ESP's boot sector backups, the ESP's second FAT, kernel.elf and HUMANVILLAGE's FAT, is caught, each place is named, and install
    fails loudly."""
    from case_install import blank, INSTALLED_CMDLINE
    img = blank("err-readback", 256)
    total = 256 * MiB // 512
    esp_first, esp_sectors = 2048, 64 * MiB // 512
    hv_first = esp_first + esp_sectors
    spc, fatsz, data = esp_fat_params(esp_sectors)
    boot_clusters = -(-(Path(IMG_DIR.parent / "esp" / "EFI" / "BOOT" / "BOOTX64.EFI").stat().st_size) // (spc * 512))
    kernel_first = 5 + boot_clusters                         # clusters 2-4: the root, EFI and EFI/BOOT directories; then BOOTX64.EFI
    lost = [0, total - 1, esp_first + 6, esp_first + 32 + fatsz, esp_first + data + (kernel_first - 2) * spc + 100, hv_first + 32]
    with Guest("t-blk-err-readback", sata("ir", img, "ide.1", "LOSSY-SATA"),
               cmdline="blk.ro ahci.losewrite=" + ",".join(str(x) for x in lost)) as g:
        dev = device_by_model(g.partitions(), "LOSSY-SATA")
        out, rc = g.run(f"install --yes-i-typed={dev} --cmdline '{INSTALLED_CMDLINE}' {dev}", timeout=900)
        assert rc == 1, f"install must fail when the disk does not read back as written (rc {rc}):\n{out}"
        for what in ("the protective MBR (sector 0)", "the backup GPT header (last sector)",
                     "the boot sector, FSInfo and their backups of TOUHOUOS-ESP", "the FATs of TOUHOUOS-ESP", "kernel.elf", "the FATs of HUMANVILLAGE"):
            must(out, r"install: READ-BACK MISMATCH: " + re.escape(what) + r", \d+ bytes at byte \d+ \(sector \d+\): differs from what was written")
        must(out, rf"\*\*\* {dev} DOES NOT READ BACK AS WRITTEN \*\*\*")
        must(out, r"install: 6 of \d+ written areas differ or cannot be read")
        assert "TouhouOS is installed" not in out, out
        assert len(re.findall(r"test knob ahci\.losewrite", g.ok("dmesg"))) == 6
        alive(g)
