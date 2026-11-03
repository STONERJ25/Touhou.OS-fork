"""The ESP write grant: root may write the ESP the installed system started from, for one mount, while blk.ro keeps it read-only otherwise.

Every test boots an installed NVMe disk alone (made once by `install` with `text user=root blk.ro`, as in case_install.py, and copied for
each test), so the ESP is nvme0n1p1, HUMANVILLAGE nvme0n1p2 (writable through blk.rw=), and a SATA decoy may sit next to it. Host-side
checks after the VM is gone: tools/mkdisk.py's checker on the ESP, the files on it, the decoy byte for byte. docs/INSTALL.md ("The ESP
write grant") and docs/UPDATE.md ("Writing the ESP") describe the design.

  TOUHOU_ESP_CUT_RUNS=N python tests/run_all.py espgrant_power_cut       more power cuts (default 4)
  TOUHOU_ESP_CUT_SEED=N                                                 the seed of the first run (default: from the clock)
  TOUHOU_ESP_CUT_AT=replacing                                           cut after that step only (default: a fixed rotation of steps)
  TOUHOU_ESP_CUT_IOPS=N                                                 disk writes per second during the update (default 4)
"""
import hashlib
import os
import random
import re
import shutil
import time

from harness import test, BUILD, scaled
from case_blockdev import sata_disk, nvme_ctrl, must, device_by_model, Guest
from case_install import IMG_DIR, blank, other_os, sha_file, check_installed, InstalledGuest, shell_wait
import mkdisk

CMDLINE = "text user=root blk.ro"
PAD = 400000                                    # bytes appended to the kernel to make a "new" one that still boots


def esp_stamp():
    h = hashlib.sha256()
    for name in ("EFI/BOOT/BOOTX64.EFI", "kernel.elf", "initrd.img"):
        h.update((BUILD / "esp" / name).read_bytes())
    return h.hexdigest()


def installed_base():
    """An NVMe image with TouhouOS installed by `install`; made again whenever build/esp changed."""
    base = IMG_DIR / "espgrant-base.img"
    stamp = base.with_suffix(".stamp")
    want = esp_stamp()
    if base.exists() and stamp.exists() and stamp.read_text() == want:
        return base
    if stamp.exists():
        stamp.unlink()
    target = blank("espgrant-base", 256)
    with Guest("t-espgrant-install", nvme_ctrl("nt", "ESPG0001", [(target, 1)]), cmdline="blk.ro") as g:
        out = g.ok(f"install --yes-i-typed=nvme0n1 --cmdline '{CMDLINE}' nvme0n1", timeout=600)
        assert "TouhouOS is installed on nvme0n1" in out, out
    check_installed(target, cmdline=CMDLINE)
    stamp.write_text(want)
    return target


def fresh(name):
    p = IMG_DIR / f"{name}.img"
    shutil.copyfile(installed_base(), p)
    return p


def esp_check(img):
    """mkdisk's checker on the ESP (partition 1) only: the Report."""
    return mkdisk.check_image(str(img), only=1)


def esp_files(img, *names):
    v = mkdisk.open_volume(str(img), 1)
    try:
        out = {}
        for n in names:
            try:
                out[n] = v.read_file("/" + n)
            except mkdisk.MkdiskError:
                out[n] = None
        return out
    finally:
        v.close()


def disk_bytes(img, off, n):
    with open(img, "rb") as f:
        f.seek(off)
        return f.read(n)


def make_new_kernel(g):
    """/tmp/new.elf in the guest: the installed kernel plus PAD bytes (an ELF with trailing bytes still loads). Returns its bytes."""
    g.ok("mkdir -p /tmp/m && mount -r /dev/nvme0n1p1 /tmp/m && cp /tmp/m/kernel.elf /tmp/new.elf && umount /tmp/m")
    g.ok(f"yes NEWKERNEL | head -c {PAD} >> /tmp/new.elf")
    new = (BUILD / "esp" / "kernel.elf").read_bytes() + (b"NEWKERNEL\n" * (PAD // 10 + 1))[:PAD]
    assert g.sha_of("sha256sum /tmp/new.elf") == hashlib.sha256(new).hexdigest(), "the new kernel in the guest is not what the host expects"
    return new


@test
def espgrant_rules():
    """esptest against the kernel: no grant, refusals (EPERM for non-root, EINVAL, EBADF, EACCES for other partitions, EBUSY), the grant
    lets one mount write and nothing else (partition node, whole disk, a SATA decoy stay read-only), and it ends with the unmount, with
    the close of its file, with the exit of the process that held it, and with BLKWRLOCK, each time leaving the volume read-only."""
    img = fresh("espgrant-rules")
    decoy = other_os("espgrant-decoy", 128, seed=21)
    decoy_sha = sha_file(decoy)
    _, parts = mkdisk.partitions(str(img))
    esp_off = parts[0]["offset"]
    whole_before = disk_bytes(img, 51200, 512)
    with InstalledGuest(img, "nvme", "t-espgrant-rules", extra=sata_disk("dc", decoy, "ide.3", model="ESPG-DECOY")) as g:
        p = g.partitions()
        dc = device_by_model(p, "ESPG-DECOY")
        assert (p["nvme0n1"]["ro"], p["nvme0n1p1"]["ro"], p["nvme0n1p2"]["ro"]) == (1, 1, 0), p
        must(g.ok("cat /proc/bootdisk"), r"\nesp nvme0n1p1 read-only\n")
        out, rc = g.run(f"esptest nvme0n1p1 nvme0n1p2 nvme0n1 {dc} {dc}1", timeout=300)
        assert rc == 0 and "ESPTEST PASS" in out, out
        log = g.ok("dmesg")
        must(log, r"\[Eiki\] esptest \(uid 1000\) asked for write access to the ESP nvme0n1p1 and was refused: only root may")
        must(log, r"blk: nvme0n1p1, the ESP this system started from, is lent to esptest \(pid \d+\) for one read-write mount")
        must(log, r"blk: ESP write grant for nvme0n1p2 refused: it is not the partition the loader came from")
        must(log, r"blk: nvme0n1p1 is read-only again: the ESP write grant ended with the unmount of /BambooForest/esptest")
        must(log, r"ended because the file that held it was closed")
        must(log, r"ended because it was handed back")
        must(log, r"fat32: nvme0n1p1 on /BambooForest/esptest is read-only now: the ESP write grant ended \(everything was written back first\)")
        assert "grant" not in g.ok("cat /proc/bootdisk"), "no grant may be left over"
        p = g.partitions()
        assert (p["nvme0n1"]["ro"], p["nvme0n1p1"]["ro"]) == (1, 1), p
        g.ok("sync")
    assert sha_file(decoy) == decoy_sha, "the decoy was modified"
    assert disk_bytes(img, 51200, 512) == whole_before, "the whole-disk node was written"
    assert disk_bytes(img, esp_off + 51200, 512) not in {bytes([c]) * 512 for c in b"abcdef"}, "the partition node was written"
    rep = esp_check(img)
    assert rep.ok and not rep.warnings, "the ESP is not clean after the test:\n" + rep.text(True)
    files = esp_files(img, "grant1.txt", "grant2.txt", "grant3.txt", "nogrant.txt", "late.txt", "orphan.txt", "after.txt", "kernel.elf")
    assert files["grant1.txt"] == b"first grant\n" and files["grant2.txt"] == b"second grant\n" and files["grant3.txt"] == b"third grant\n", files
    assert not any(files[n] for n in ("nogrant.txt", "late.txt", "orphan.txt", "after.txt")), "a write without the grant landed"
    assert files["kernel.elf"] == (BUILD / "esp" / "kernel.elf").read_bytes()


@test
def espgrant_helper():
    """The espgrant applet: status, refusal for non-root, `run` with the other disks and the whole-disk node read-only meanwhile, a kill
    of espgrant in the middle (the mount turns read-only), then `put -k` of a new kernel; the disk boots that kernel afterwards."""
    img = fresh("espgrant-helper")
    decoy = other_os("espgrant-decoy2", 128, seed=22)
    decoy_sha = sha_file(decoy)
    old = (BUILD / "esp" / "kernel.elf").read_bytes()
    extra = sata_disk("dc", decoy, "ide.3", model="ESPG-DECOY")
    with InstalledGuest(img, "nvme", "t-espgrant-helper", extra=extra) as g:
        dc = device_by_model(g.partitions(), "ESPG-DECOY")
        out = g.ok("espgrant")
        must(out, r"ESP: /dev/nvme0n1p1, the partition this system started from \(read-only under blk\.ro")
        must(out, r"grant: none held")
        out, rc = g.run("su reimu -c 'espgrant run true'")
        assert rc == 1 and "only root may write the ESP" in out, out

        # while espgrant holds the grant, nothing else becomes writable
        script = ("echo hello > $ESP/hello.txt && cat $ESP/hello.txt && cat /proc/bootdisk && "
                  f"for d in nvme0n1 nvme0n1p1 {dc} {dc}1; do dd if=/dev/zero of=/dev/$d bs=512 count=1 seek=100 conv=notrunc; done; lsblk")
        out = g.ok(f"espgrant run sh -c '{script}'", timeout=180)
        must(out, r"is mounted read-write on /BambooForest/espgrant\.\d+ until sh ends")
        must(out, r"hello\n")
        must(out, r"grant nvme0n1p1 pid \d+ espgrant mounted /BambooForest/espgrant\.\d+")
        assert len(re.findall(r"Read-only file system", out)) == 4, out
        must(out, r"nvme0n1p1 .* is read-only again|unmounted; /dev/nvme0n1p1 is read-only again")
        assert "grant" not in g.ok("cat /proc/bootdisk"), "the grant must end with espgrant"
        p = g.partitions()
        assert (p["nvme0n1"]["ro"], p["nvme0n1p1"]["ro"], p[dc]["ro"]) == (1, 1, 1), p

        # espgrant killed while the command runs: the mount stays, read-only
        g.ok("rm -f /tmp/st.txt")
        g.ok("espgrant -d /BambooForest/killed run sh -c 'echo before > $ESP/before.txt; kill -9 $ESPGRANT_PID; sleep 1; "
             "echo after > $ESP/after.txt; echo $? > /tmp/st.txt' > /tmp/run.txt 2>&1; true")
        shell_wait(g, "/tmp/st.txt", r"\d")
        st = g.ok("cat /tmp/st.txt /tmp/run.txt")
        assert not st.startswith("0"), "a write after the grant ended went through:\n" + st
        must(st, r"Read-only file system")
        must(g.ok("cat /proc/mounts"), r"/dev/nvme0n1p1 /BambooForest/killed fat32 ro")
        log = g.ok("dmesg")
        must(log, r"ended because the file that held it was closed; the volume on /BambooForest/killed stays mounted read-only")
        must(log, r"fat32: nvme0n1p1 on /BambooForest/killed is read-only now")
        g.ok("umount /BambooForest/killed")

        # an update of the kernel, keeping the old one
        new = make_new_kernel(g)
        out = g.ok("espgrant -k put /tmp/new.elf kernel.elf", timeout=600)
        steps = re.findall(r"^espgrant: (mounted|staging|staged|synced|verified|kept|replacing|replaced|done) ", out, re.M)
        assert steps == ["mounted", "staging", "staged", "synced", "verified", "kept", "replacing", "replaced", "done"], out
        must(out, r"the ESP holds the new files; /dev/nvme0n1p1 is read-only again")
        out, rc = g.run("su reimu -c 'espgrant rm kernel.old'")
        assert rc == 1, out
        g.ok("sync")
    assert sha_file(decoy) == decoy_sha, "the decoy was modified"
    rep = esp_check(img)
    assert rep.ok and not rep.warnings, "the ESP is not clean after the update:\n" + rep.text(True)
    files = esp_files(img, "kernel.elf", "kernel.old", "kernel.elf.new", "hello.txt", "before.txt", "after.txt", "initrd.img")
    assert files["kernel.elf"] == new, "kernel.elf is not the new kernel"
    assert files["kernel.old"] == old, "kernel.old is not the previous kernel"
    assert files["kernel.elf.new"] is None, "the staged file is left over"
    assert files["hello.txt"] == b"hello\n" and files["before.txt"] == b"before\n" and files["after.txt"] is None, files
    assert files["initrd.img"] == (BUILD / "esp" / "initrd.img").read_bytes()
    with InstalledGuest(img, "nvme", "t-espgrant-helper-boot") as g:
        must(g.log(), rf"loader: kernel\.elf {len(new)} bytes", "(the loader started the new kernel)")
        out = g.ok("espgrant rm kernel.old hello.txt before.txt")
        must(out, r"removed kernel\.old")
    files = esp_files(img, "kernel.old", "hello.txt", "kernel.elf")
    assert files["kernel.old"] is None and files["hello.txt"] is None and files["kernel.elf"] == new, files
    rep = esp_check(img)
    assert rep.ok and not rep.warnings, rep.text(True)


# --------------------------------------------------------------------------------------------------------------- power cuts

# What a power cut may leave on a FAT volume without a journal (as in task 7's crash test): lost clusters, a chain longer than its file,
# FAT copy 2 behind copy 1, a stale FSInfo, the dirty flags (warnings). Anything else is damage.
BENIGN = [re.compile(r) for r in (r"lost cluster", r"FAT copy \d+ differs from copy 1", r"FSInfo free count", r"FSInfo next-free hint")]
SIZE_VS_CHAIN = re.compile(r"size (\d+) needs (\d+) cluster\(s\) but the chain has (\d+)")


def serious(rep):
    bad = []
    for e in rep.errors:
        if any(rx.search(e) for rx in BENIGN):
            continue
        m = SIZE_VS_CHAIN.search(e)
        if m and int(m.group(3)) > int(m.group(2)):
            continue
        bad.append(e)
    return bad


MARKERS = ["replacing", "staging", "replacing", "verified", "replaced", "synced", "staged", "replacing"]


def cut_run(i, seed):
    """One update cut short by QMP quit. Returns (None or a text saying what is wrong, a one-line summary)."""
    rng = random.Random(seed)
    marker = os.environ.get("TOUHOU_ESP_CUT_AT") or MARKERS[i % len(MARKERS)]
    delay = rng.uniform(0.0, 2.5)
    iops = int(os.environ.get("TOUHOU_ESP_CUT_IOPS", "4"))
    old = (BUILD / "esp" / "kernel.elf").read_bytes()
    img = fresh("espgrant-cut")
    tag = f"run {i} seed {seed}: cut {delay:.2f} s after '{marker}'"
    with InstalledGuest(img, "nvme", "t-espgrant-cut") as g:
        new = make_new_kernel(g)
        # slow the disk's writes down to a few per second, so that a cut lands between the requests of the update
        g.vm.qmp("block_set_io_throttle", device="bootdisk", bps=0, bps_rd=0, bps_wr=0, iops=0, iops_rd=0, iops_wr=iops)
        g.vm.send("espgrant put /tmp/new.elf kernel.elf\n")
        g.vm.expect(rf"espgrant: {marker} ", timeout=600)
        time.sleep(delay)
        seen = g.vm.text()
        try:
            g.vm.qmp("quit")                                        # the plug: no sync, no unmount
        except Exception:                                           # noqa: BLE001 -- QEMU may close the socket before it answers
            pass
        g.vm.proc.wait(timeout=scaled(30))
    last = re.findall(r"espgrant: (\w+ \S*)", seen)
    tag += f" (last step seen: {last[-1] if last else '-'})"
    rep = esp_check(img)
    problems = [f"damage: {e}" for e in serious(rep)]
    files = esp_files(img, "kernel.elf", "initrd.img", "cmdline.txt", "EFI/BOOT/BOOTX64.EFI")
    k = files["kernel.elf"]
    which = "old" if k == old else "new" if k == new else None
    if which is None:
        problems.append(f"kernel.elf is neither the old nor the new kernel ({'missing' if k is None else f'{len(k)} bytes'})")
    for n in ("initrd.img", "EFI/BOOT/BOOTX64.EFI"):
        if files[n] != (BUILD / "esp" / n).read_bytes():
            problems.append(f"{n} changed")
    if files["cmdline.txt"] is None or not files["cmdline.txt"].decode(errors="replace").startswith(CMDLINE):
        problems.append("cmdline.txt changed")
    dirty = any("dirty" in w for w in rep.warnings)
    if not problems:
        with InstalledGuest(img, "nvme", "t-espgrant-cut-boot") as g:          # the prompt is reached: it booted
            if not re.search(rf"loader: kernel\.elf {len(k)} bytes", g.log()):
                problems.append(f"the loader did not start the {which} kernel")
            out, rc = g.run("espgrant run true")
            if dirty and not (rc == 1 and "not cleanly unmounted" in out):
                problems.append("a dirty ESP was mounted read-write without -f:\n" + out)
            if not dirty and rc != 0:
                problems.append("a clean ESP could not be granted:\n" + out)
    leftovers = "; ".join(rep.errors)[:300]
    summary = f"{tag}: kernel.elf {which}, ESP {'dirty' if dirty else 'clean'}" + (f", leftovers: {leftovers}" if leftovers else "")
    if problems:
        return f"{tag}:\n  " + "\n  ".join(problems) + "\n" + rep.text(True)[-2500:], summary
    return None, summary


@test
def espgrant_power_cut():
    """An update (kernel.elf.new, flush, rename over kernel.elf) cut by QMP quit at a step of it plus a random delay, N times
    (TOUHOU_ESP_CUT_RUNS, default 4): afterwards the ESP has no damage worse than lost clusters, kernel.elf is the old or the new
    kernel in full, and the disk boots it; a dirty ESP then needs -f."""
    runs = int(os.environ.get("TOUHOU_ESP_CUT_RUNS", "4"))
    base = int(os.environ.get("TOUHOU_ESP_CUT_SEED", str(int(time.time()) & 0xFFFFFF)))
    print(f"     power cuts: {runs} run(s), seeds {base}..{base + runs - 1}", flush=True)
    failed = []
    for i in range(runs):
        why, summary = cut_run(i, base + i)
        print(("     FAILED " if why else "     ") + summary, flush=True)
        if why:
            failed.append(why)
    assert not failed, f"{len(failed)} of {runs} power cuts left the ESP damaged or unbootable:\n" + "\n".join(failed)
