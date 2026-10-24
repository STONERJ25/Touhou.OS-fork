"""Persistent storage: virtio-blk, FAT32/FAT16, the HUMANVILLAGE mount policy, df/statfs and the safety rules.

Every test builds its own disk image with tools/mkdisk.py (pure Python), boots the kernel with it, drives the shell over
the serial console, and -- after a clean power-off -- looks at what the KERNEL left on the disk with mkdisk's independent
FAT checker (FAT copies, LFN checksums, chains, cross-links, lost clusters, FSInfo, dirty flags). Images live in
build/storage-tests/ and are deleted afterwards (set TOUHOU_KEEP_IMAGES=1 to keep them for inspection).

Run just these with:  python tests/run_all.py storage
"""
import contextlib
import hashlib
import os
import random
import re
import shutil
import struct
from pathlib import Path

from harness import test, Qemu, BUILD, scaled
import mkdisk

MIB = 1 << 20
WORK = BUILD / "storage-tests"
USER_PROMPT = r"reimu@gensokyo \S+ \$ "
ROOT_PROMPT = r"root@gensokyo \S+ # "
HV = "/HumanVillage"
H = "/HumanVillage/reimu"


# ---------------------------------------------------------------------------------------------------------------------
# helpers

def sha(data):
    return hashlib.sha256(data).hexdigest()


def file_sha(path, start=0, length=None):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        f.seek(start)
        left = length
        while left is None or left > 0:
            chunk = f.read(1 << 20 if left is None else min(1 << 20, left))
            if not chunk:
                break
            h.update(chunk)
            if left is not None:
                left -= len(chunk)
    return h.hexdigest()


class Image:
    """A disk image on the host that is removed again unless TOUHOU_KEEP_IMAGES is set."""

    def __init__(self, name, size, parts, **kw):
        WORK.mkdir(parents=True, exist_ok=True)
        self.path = WORK / f"{name}.img"
        self.parts = mkdisk.build_image(self.path, size, parts, **kw)
        rep = mkdisk.check_image(self.path)
        assert rep.ok and not rep.warnings, "freshly built image is not clean:\n" + rep.text(True)

    def sha(self):
        return file_sha(self.path)

    def check(self, allow_dirty=False, **kw):
        """fsck the image after the guest is gone: no errors and (by default) no warnings, i.e. cleanly unmounted."""
        rep = mkdisk.check_image(self.path, **kw)
        problems = rep.errors + ([] if allow_dirty else rep.warnings)
        assert not problems, "disk check failed:\n" + rep.text(True)
        return rep

    def volume(self, index=1):
        return mkdisk.open_volume(self.path, index)

    def part_hash(self, index):
        _, parts = mkdisk.partitions(self.path)
        p = [x for x in parts if x["index"] == index][0]
        return file_sha(self.path, p["offset"], p["length"])

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        if not os.environ.get("TOUHOU_KEEP_IMAGES"):
            try:
                self.path.unlink()
            except OSError:
                pass


def hv_part(files=None, cluster=None, kind="fat32", label="HUMANVILLAGE", name="HUMANVILLAGE", size="rest"):
    return dict(name=name, kind=kind, size=size, label=label, files=files or {}, cluster=cluster)


def decoy_esp(size=34 * MIB):
    """A non-bootable EFI System Partition (no EFI/BOOT/BOOTX64.EFI, or OVMF would boot the copy instead of build/esp)."""
    return dict(name="TOUHOUOS-ESP", kind="fat32", size=size, label="TOUHOUOS-ESP", esp=True,
                files={"/EFI/OTHEROS/grubx64.efi": b"not a bootloader " * 100, "/notes.txt": b"keep out\n"})


class Guest:
    def __init__(self, vm, root):
        self.vm = vm
        self.root = root
        self.prompt = ROOT_PROMPT if root else USER_PROMPT

    def run(self, cmd, timeout=60):
        """Runs one shell line; returns (output, exit status)."""
        self.vm.send(f"echo __BE''GIN__; {cmd}; echo __EN''D__$?\n")
        m = self.vm.expect(r"\n__BEGIN__\n(.*?)__END__(\d+)\n", timeout=timeout)
        self.vm.expect(self.prompt, timeout=20)
        return m.group(1), int(m.group(2))

    def ok(self, cmd, timeout=60):
        out, rc = self.run(cmd, timeout)
        assert rc == 0, f"`{cmd}` exited with {rc}:\n{out}"
        return out

    def fails(self, cmd, expect=None, timeout=60):
        out, rc = self.run(cmd, timeout)
        assert rc != 0, f"`{cmd}` should have failed but printed:\n{out}"
        if expect:
            assert re.search(expect, out), f"`{cmd}`: expected /{expect}/ in:\n{out}"
        return out

    def poweroff(self, timeout=40):
        """Root: the poweroff applet (sync, then the machine stops). reimu may not power the machine off, so there:
        sync (which marks the volume clean) and pull the plug through QMP."""
        if self.root:
            self.vm.send("poweroff\n")
            rc = self.vm.wait_exit(timeout)
            assert rc is not None, "guest did not power off:\n" + self.vm.text()[-1500:]
            return rc
        self.ok("sync")
        try:
            self.vm.qmp("quit")
        except Exception:                       # noqa: BLE001 -- QEMU may close the socket before it answers
            pass
        return self.vm.proc.wait(timeout=scaled(timeout))


@contextlib.contextmanager
def boot(image=None, root=False, cmdline="", name="t-storage", extra=(), mem="512M", bootlog=False):
    # text: these tests type into the console shell, not the desktop. bootlog=off: the kernel keeps its log as TOUHOU.LOG on a writable
    # HUMANVILLAGE volume (docs/REAL_HARDWARE.md), which would change the byte-for-byte comparisons of these tests; case_usbstorage.py wants it.
    cl = ("text " + ("" if bootlog else "bootlog=off ") + cmdline + (" user=root" if root else "")).strip()
    disk = image.path if image is not None else None
    with Qemu(cmdline=cl, disk=disk, name=name, extra=list(extra), mem=mem) as vm:
        vm.expect(ROOT_PROMPT if root else USER_PROMPT, timeout=90)
        yield Guest(vm, root), vm


def klog(vm):
    return vm.text()


def df_row(g, path):
    """(1K-blocks, used, available) of the filesystem holding path, from `df`."""
    out = g.ok(f"df {path}")
    for line in out.splitlines():
        m = re.match(r"\S+\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+%|-)\s+(\S+)", line)
        if m:
            return int(m.group(1)), int(m.group(2)), int(m.group(3))
    raise AssertionError("cannot parse df output:\n" + out)


def drive_args(path, **props):
    """QEMU arguments for a virtio-blk disk with custom device properties (transport, block size)."""
    extra = "".join(f",{k}={v}" for k, v in props.items())
    return ["-drive", f"if=none,id=d0,format=raw,file={path}", "-device", f"virtio-blk-pci,drive=d0{extra}"]


# ---------------------------------------------------------------------------------------------------------------------
# the checker itself (pure Python, no QEMU): an fsck that never complains is useless

@test
def storage_mkdisk_selftest():
    """tools/mkdisk.py builds valid GPT/MBR images (512/4096 sectors, FAT32/FAT16) and its fsck catches deliberate damage."""
    d = WORK / "selftest"
    d.mkdir(parents=True, exist_ok=True)
    rng = random.Random(3)
    big = bytes(rng.randrange(256) for _ in range(70000))
    long_name = "/dir/sub/a very long name with spaces.markdown"
    files = {"/ReadMe.TXT": b"x", long_name: big, "/dir/Ünïcode ファイル.txt": b"u", "/lower.txt": b"l"}
    p = d / "good.img"
    try:
        for tag, kw, size, parts in (
                ("gpt512", dict(sector_size=512), 80 * MIB, [decoy_esp(), hv_part(files)]),
                ("gpt4k", dict(sector_size=4096), 700 * MIB, [hv_part(files)]),
                ("mbr", dict(table="mbr"), 80 * MIB, [hv_part(files)]),
                ("fat16", dict(), 80 * MIB, [hv_part(files, kind="fat16")])):
            mkdisk.build_image(p, size, parts, **kw)
            rep = mkdisk.check_image(p)
            assert rep.ok and not rep.warnings, f"{tag}: " + rep.text(True)
            r = mkdisk.open_volume(p, 2 if tag == "gpt512" else 1)
            try:
                assert r.read_file(long_name) == big, tag
                assert r.find(long_name.upper()) is not None, tag                       # case-insensitive lookup
                assert sorted(e.name for e in r.listdir("/")) == ["ReadMe.TXT", "dir", "lower.txt"], tag
                assert r.label.strip() == "HUMANVILLAG", r.label
            finally:
                r.close()

        # --- deliberate damage must be reported ---
        mkdisk.build_image(p, 80 * MIB, [hv_part(files)])
        r = mkdisk.open_volume(p, 1)
        base = r.part["offset"]
        fat0 = base + r.reserved * r.bps
        fat1 = fat0 + r.fatsz * r.bps
        entry = r.find(long_name)
        chain = r.chain(entry.first)
        sub = r.find("/dir/sub")
        sub_dir = base + r.cluster_off(sub.first)
        top_clusters = r.nclusters
        info = base + r.fsinfo_sector * r.bps
        r.close()

        def damage(poke_at, data, expect):
            q = d / "bad.img"
            shutil.copyfile(p, q)
            with open(q, "r+b") as fh:
                fh.seek(poke_at)
                fh.write(data)
            rep = mkdisk.check_image(q)
            q.unlink()
            assert any(re.search(expect, e) for e in rep.errors), f"expected /{expect}/ in {rep.errors}"

        u32 = lambda v: struct.pack("<I", v)
        damage(fat0 + chain[3] * 4, u32(0), "free cluster")                                   # hole in a chain (copy 1 only)
        damage(fat0 + chain[3] * 4, u32(chain[0]), "loop|cross-linked")                       # chain that loops back
        damage(fat0 + (top_clusters + 1) * 4, u32(0x0FFFFFFF), "lost cluster")                # a leaked cluster
        damage(fat1 + (top_clusters + 1) * 4, u32(0x0FFFFFFF), "FAT copy 2 differs")          # mirror mismatch
        damage(sub_dir + entry.slot * 32 + 1, b"Q", "do not match")                                    # LFN checksum no longer fits the 8.3 name
        damage(info + 488, u32(12345), "FSInfo free count")                                   # lying FSInfo
        damage(512 + 56, b"\x42", "header CRC")                                               # GPT header edited without its CRC
        damage(2 * 512 + 40, b"\x01", "entry array CRC")                                      # GPT entry edited without the array CRC
        damage(fat0 + chain[1] * 4, u32(chain[5]), "cross-linked|loop|chain has")             # chain jumps ahead: leaks the middle
    finally:
        shutil.rmtree(d, ignore_errors=True)


# ---------------------------------------------------------------------------------------------------------------------
# block devices and the mount policy

@test
def storage_blk_visible():
    """virtio-blk disk with GPT partitions vda1/vda2 in lsblk; only HUMANVILLAGE is mounted; the ESP is left alone."""
    with Image("visible", 100 * MIB, [decoy_esp(), hv_part()]) as img:
        esp0 = img.part_hash(1)
        with boot(img, name="t-storage-visible") as (g, vm):
            out = g.ok("lsblk")
            assert re.search(r"^vda\s+\S+\s+0\s+disk", out, re.M), out
            assert re.search(r"^vda1\s+\S+\s+0\s+part\s+TOUHOUOS-ESP\s+virtio-blk", out, re.M), out
            assert re.search(r"^vda2\s+\S+\s+0\s+part\s+HUMANVILLAGE\s+virtio-blk", out, re.M), out
            devs = g.ok("ls /dev")
            assert all(n in devs.split() for n in ("vda", "vda1", "vda2")), devs
            mounts = g.ok("mount")
            assert re.search(r"/dev/vda2 on /HumanVillage type fat32", mounts), mounts
            assert "vda1" not in mounts, mounts
            assert re.search(r"\[Kappa\] virtio-blk .* transport", klog(vm))
            assert re.search(r"\[Keine\] fat32: vda2 mounted on /HumanVillage", klog(vm)), klog(vm)[-1500:]
            g.poweroff()
        rep = img.check()
        assert rep.stats["p2"]["clean"]
        assert img.part_hash(1) == esp0, "the ESP changed"


@test
def storage_automount_read():
    """HUMANVILLAGE auto-mounts; files made by the Python builder read back byte for byte; ownership policy; zero writes."""
    rng = random.Random(11)
    blob = bytes(rng.randrange(256) for _ in range(300000))
    files = {
        "/reimu/hello.txt": b"Hello from the Human Village\n",
        "/reimu/ReadMe.TXT": b"mixed case\n",
        "/reimu/a very long file name with spaces and CAPS.markdown": b"long\n",
        "/reimu/.hidden-config": b"dot\n",
        "/reimu/notes/Café 東方.txt": "unicode ☃\n".encode(),
        "/marisa/spell.dat": b"marisa's\n",
        "/sakuya/empty.txt": b"",
        "/shared/spell.dat": blob,
        "/shared/lower.txt": b"lower\n",
        "/shared/empty.txt": b"",
        "/shared/UPPER.TXT": b"upper\n",
        "/shared/Mixed_Name.Txt": b"mixed\n",
        "/deep/a/b/c/d/e/f/g.txt": b"deep\n",
    }
    for i in range(150):
        files[f"/many/file-{i:04d}-with-a-longish-name.txt"] = f"file {i}\n".encode()
    with Image("automount", 80 * MIB, [hv_part(files)]) as img:
        h0 = img.sha()
        with boot(img, name="t-storage-automount") as (g, vm):
            assert g.ok(f"cat {H}/hello.txt").strip() == "Hello from the Human Village"
            assert g.ok(f"cat {H}/readme.txt").strip() == "mixed case"                          # lookups ignore case
            assert g.ok(f"cat {H}/README.TXT").strip() == "mixed case"
            assert g.ok(f"cat '{H}/a very long file name with spaces and CAPS.markdown'").strip() == "long"
            assert g.ok(f"cat {H}/notes/Caf*.txt").strip() == "unicode ☃"
            assert g.ok(f"cat {HV}/shared/lower.txt").strip() == "lower"
            ls = g.ok(f"ls {H}")
            for name in ("hello.txt", "ReadMe.TXT", "notes"):
                assert name in ls, ls
            assert "Café 東方.txt" in g.ok(f"ls {H}/notes")
            assert "Mixed_Name.Txt" in g.ok(f"ls {HV}/shared")                                   # stored case is kept
            assert int(g.ok(f"ls {HV}/many | wc -l").strip()) == 150
            out = g.ok(f"sha256sum {HV}/shared/spell.dat {H}/hello.txt {HV}/shared/UPPER.TXT")
            for data in (blob, b"Hello from the Human Village\n", b"upper\n"):
                assert sha(data) in out, out
            assert g.ok(f"wc -c < {HV}/shared/empty.txt").strip() == "0" and not g.ok(f"cat {HV}/shared/empty.txt").strip()
            # ownership policy: homes belong to their user (0700 / 0600), everything else to root (0755 / 0644)
            st = g.ok(f"ls -ld {H} {HV}/marisa {HV}/shared {HV}")
            assert re.search(rf"^drwx------\s+\d+\s+reimu\s+reimu\s.*{H}$", st, re.M), st
            assert re.search(rf"^drwx------\s+\d+\s+marisa\s+marisa\s.*{HV}/marisa$", st, re.M), st
            assert re.search(rf"^drwxr-xr-x\s+\d+\s+root\s+root\s.*{HV}/shared$", st, re.M), st
            assert re.search(rf"^drwxr-xr-x\s+\d+\s+root\s+root\s.*\s{HV}$", st, re.M), st
            st = g.ok(f"ls -l {H}/hello.txt {HV}/shared/lower.txt")
            assert re.search(r"^-rw-------\s+\d+\s+reimu\s+reimu\s+29\s.*hello.txt$", st, re.M), st
            assert re.search(r"^-rw-r--r--\s+\d+\s+root\s+root\s+6\s.*lower.txt$", st, re.M), st
            g.fails(f"ls {HV}/marisa", r"[Pp]ermission denied")                                  # reimu cannot enter marisa's home
            g.fails(f"cat {HV}/marisa/spell.dat", r"[Pp]ermission denied")
            g.ok("sync")
            g.poweroff()
        assert img.sha() == h0, "a read-only session modified the image"


@test
def storage_write_roundtrip():
    """Small file, 5 MiB file, 300 long names, rename, delete, dirs, truncate, append, overwrite: sync, poweroff, reboot."""
    with Image("roundtrip", 64 * MIB, [hv_part()]) as img:
        seq = "".join(f"{i}\n" for i in range(1, 2001)).encode()
        with boot(img, name="t-storage-write") as (g, vm):
            kb0 = df_row(g, HV)
            g.ok(f"echo 'hello village' > {H}/small.txt")
            g.ok(f"dd if=/dev/urandom of={H}/big.bin bs=1k count=5120 2>/dev/null")
            big_sum = g.ok(f"sha256sum {H}/big.bin").split()[0]
            assert len(big_sum) == 64
            g.ok(f"mkdir {H}/docs {H}/many")
            g.ok(f"mkdir -p {H}/Deep/Er/Still && echo deepfile > {H}/Deep/Er/Still/Bottom.TXT")
            g.ok(f"i=0; while [ $i -lt 300 ]; do echo $i > \"{H}/many/Mixed-Case_File Number $i.TextFile.dat\"; i=$((i+1)); done",
                 timeout=180)
            assert int(g.ok(f"ls {H}/many | wc -l").strip()) == 300
            g.ok(f"mv {H}/small.txt '{H}/docs/Renamed Small File.TXT'")
            g.ok(f"rm '{H}/many/Mixed-Case_File Number 7.TextFile.dat' '{H}/many/Mixed-Case_File Number 299.TextFile.dat'")
            g.ok(f"mkdir {H}/tmpdir && echo x > {H}/tmpdir/f && rm {H}/tmpdir/f && rmdir {H}/tmpdir")
            g.fails(f"rmdir {H}/docs", r"not empty")
            g.ok(f"seq 1 2000 > {H}/seq.txt && truncate -s 100 {H}/seq.txt")                      # shrink
            g.ok(f"seq 1 2000 > {H}/seq2.txt && truncate -s 20000 {H}/seq2.txt")                   # grow with zeros
            g.ok(f"echo first > {H}/docs/a.txt; echo second >> {H}/docs/a.txt; echo third >> {H}/docs/a.txt")
            g.ok(f"seq 1 2000 > {H}/mid.txt; printf 'XXXXXXXX' | dd of={H}/mid.txt bs=1 seek=10 conv=notrunc 2>/dev/null")
            g.ok(f"printf 'AB' > {H}/sparse.bin; printf 'Z' | dd of={H}/sparse.bin bs=1 seek=10000 conv=notrunc 2>/dev/null")
            g.ok(f"echo old > {H}/replace.txt; echo newer > {H}/replace.txt; mv {H}/replace.txt {H}/docs/a.txt")   # rename over a file
            g.ok(f"mkdir {H}/dirA {H}/dirB && echo moved > {H}/dirA/m.txt && mv {H}/dirA {H}/dirB/dirA2")        # move a directory
            kb1 = df_row(g, HV)
            assert kb1[0] == kb0[0], (kb0, kb1)
            assert kb1[1] - kb0[1] >= 5000, f"df used did not grow by the 5 MiB file: {kb0} -> {kb1}"
            assert kb1[1] + kb1[2] == kb1[0], kb1
            g.ok("sync")
            df_last = df_row(g, HV)
            g.poweroff()
        rep = img.check()
        v = img.volume(1)
        try:
            free = rep.stats["p1"]["free"]
            assert df_last[2] == free * v.cluster_bytes // 1024, (df_last, free, v.cluster_bytes)       # df matches the FAT exactly
            assert sha(v.read_file("/reimu/big.bin")) == big_sum and len(v.read_file("/reimu/big.bin")) == 5 * MIB
            assert v.read_file("/reimu/docs/Renamed Small File.TXT") == b"hello village\n"
            assert v.read_file("/reimu/seq.txt") == seq[:100]
            assert v.read_file("/reimu/seq2.txt") == seq + bytes(20000 - len(seq))
            assert v.read_file("/reimu/docs/a.txt") == b"newer\n"
            exp_mid = bytearray(seq)
            exp_mid[10:18] = b"XXXXXXXX"
            assert v.read_file("/reimu/mid.txt") == bytes(exp_mid)
            assert v.read_file("/reimu/sparse.bin") == b"AB" + bytes(9998) + b"Z"
            assert v.read_file("/reimu/dirB/dirA2/m.txt") == b"moved\n"
            assert v.find("/reimu/dirA") is None and v.find("/reimu/tmpdir") is None
            names = sorted(e.name for e in v.listdir("/reimu/many"))
            assert len(names) == 298 and "Mixed-Case_File Number 7.TextFile.dat" not in names
            assert "Mixed-Case_File Number 150.TextFile.dat" in names
            assert v.read_file("/reimu/many/Mixed-Case_File Number 123.TextFile.dat") == b"123\n"
            assert v.read_file("/reimu/Deep/Er/Still/Bottom.TXT") == b"deepfile\n"
            assert v.find("/reimu/Deep").name == "Deep"                                                 # stored case survives
            big_chain = v.chain(v.find("/reimu/big.bin").first)
            assert big_chain == list(range(big_chain[0], big_chain[0] + len(big_chain))), "the allocator should keep a 5 MiB file in one run"
        finally:
            v.close()
        with boot(img, root=True, name="t-storage-write2") as (g, vm):                                # second boot (root: real poweroff)
            assert g.ok(f"sha256sum {H}/big.bin").split()[0] == big_sum
            assert g.ok(f"cat {H}/docs/a.txt").strip() == "newer"
            assert g.ok(f"ls {H}/many | wc -l").strip() == "298"
            assert g.ok(f"wc -c < {H}/sparse.bin").strip() == "10001"
            g.ok(f"echo more > {H}/second-boot.txt")
            g.poweroff()
        img.check()
        v = img.volume(1)
        try:
            assert v.read_file("/reimu/second-boot.txt") == b"more\n" and sha(v.read_file("/reimu/big.bin")) == big_sum
        finally:
            v.close()


@test
def storage_reboot_persists():
    """Data written just before the guest `reboot` command (no explicit sync) is on the disk and the volume is clean."""
    with Image("reboot", 64 * MIB, [hv_part()]) as img:
        with boot(img, root=True, name="t-storage-reboot") as (g, vm):
            g.ok(f"echo before-reboot > {H}/r.txt; dd if=/dev/urandom of={H}/r.bin bs=1k count=300 2>/dev/null")
            s = g.ok(f"sha256sum {H}/r.bin").split()[0]
            g.ok(f"echo even-more > {H}/r2.txt")
            vm.send("reboot\n")                                     # reboot syncs first; QEMU runs with -no-reboot and exits
            assert vm.wait_exit(40) is not None
        img.check()
        with boot(img, name="t-storage-reboot2") as (g, vm):
            assert g.ok(f"cat {H}/r.txt").strip() == "before-reboot"
            assert g.ok(f"cat {H}/r2.txt").strip() == "even-more"
            assert g.ok(f"sha256sum {H}/r.bin").split()[0] == s
            g.poweroff()
        img.check()


@test
def storage_dirty_volume_readonly():
    """A volume whose clean-shutdown flag is clear mounts read-only (warning, nothing written); `-o force` overrides."""
    with Image("dirty", 64 * MIB, [hv_part({"/reimu/keep.txt": b"precious\n"})]) as img:
        mkdisk.set_dirty(img.path, 1, True)
        h0 = img.sha()
        with boot(img, name="t-storage-dirty") as (g, vm):
            assert re.search(r"\[Keine\] fat32: .*read-only: the volume was not cleanly unmounted", klog(vm)), klog(vm)[-1500:]
            assert re.search(r"/dev/vda1 /HumanVillage fat32 ro ", g.ok("cat /proc/mounts"))
            assert g.ok(f"cat {H}/keep.txt").strip() == "precious"
            g.fails(f"echo no > {H}/new.txt", r"[Rr]ead-only")
            g.fails(f"mkdir {H}/newdir", r"[Rr]ead-only")
            g.fails(f"rm {H}/keep.txt", r"[Rr]ead-only")
            g.ok("sync")
            g.poweroff()
        assert img.sha() == h0, "a dirty volume mounted read-only was modified"
        with boot(img, root=True, name="t-storage-dirty2") as (g, vm):       # forced read-write: allowed, flag stays set
            g.ok("umount /HumanVillage")
            g.ok("mount -t fat32 -o force /dev/vda1 /HumanVillage")
            assert "FORCED" in klog(vm)
            g.ok(f"echo forced > {H}/forced.txt")
            g.ok("sync")
            g.poweroff()
        rep = img.check(allow_dirty=True)
        assert not rep.stats["p1"]["clean"], "a forced mount must not clear a dirty flag it did not set"
        v = img.volume(1)
        try:
            assert v.read_file("/reimu/forced.txt") == b"forced\n"
        finally:
            v.close()
        mkdisk.set_dirty(img.path, 1, False)
        img.check()


@test
def storage_foreign_disks_untouched():
    """No HUMANVILLAGE: boot continues with tmpfs homes and the other partitions stay byte-identical (also an ESP-only disk)."""
    other = dict(name="OTHEROS", kind="fat32", size=40 * MIB, label="WINDOWS", files={"/Users/someone/doc.txt": b"belongs to another OS\n"})
    raw = dict(name="linux", kind="raw", size=8 * MIB, type=mkdisk.GPT_TYPE_LINUX)
    with Image("foreign", 120 * MIB, [decoy_esp(), other, raw]) as img:
        _, parts = mkdisk.partitions(img.path)
        with open(img.path, "r+b") as f:                    # recognisable junk in the raw partition
            f.seek(parts[2]["offset"])
            f.write(bytes(range(256)) * 4096)
        h0 = img.sha()
        with boot(img, root=True, name="t-storage-foreign") as (g, vm):
            log = klog(vm)
            assert "no HUMANVILLAGE volume found" in log, log[-1500:]
            assert "fat32:" not in log, "something was mounted"
            mounts = g.ok("mount")
            assert "fat32" not in mounts and "vda" not in mounts, mounts
            assert re.search(r"drwxr-xr-x\s+\d+\s+reimu", g.ok("ls -l /HumanVillage")), "homes should be the tmpfs ones"
            g.ok("echo ok > /HumanVillage/reimu/ram.txt")
            g.ok("lsblk; cat /proc/partitions; df; ls /dev; dd if=/dev/vda1 of=/dev/null bs=4k count=4 2>/dev/null")
            g.ok("sync")
            g.poweroff()
        assert img.sha() == h0, "a disk without HUMANVILLAGE was modified"
    with Image("espo", 64 * MIB, [decoy_esp(40 * MIB)]) as img:
        h0 = img.sha()
        with boot(img, name="t-storage-espo") as (g, vm):
            assert "no HUMANVILLAGE volume found" in klog(vm)
            g.ok("echo ok > /HumanVillage/reimu/ram.txt; sync")
            g.poweroff()
        assert img.sha() == h0, "an ESP-only disk was modified"


@test
def storage_label_discovery_and_isolation():
    """A FAT volume label (MBR disk, no GPT names) is found; a decoy partition with another label is never written."""
    decoy = dict(name="", kind="fat32", size=40 * MIB, label="WINDOWS", files={"/Windows/System32/x.dll": b"MZ" * 1000})
    hv = hv_part({"/reimu/readme.txt": b"found by label\n"})
    with Image("mbr", 120 * MIB, [decoy, hv], table="mbr") as img:
        d0 = img.part_hash(1)
        with boot(img, name="t-storage-mbr") as (g, vm):
            assert re.search(r"\[Keine\] fat32: vda2 mounted on /HumanVillage", klog(vm)), klog(vm)[-1200:]
            assert g.ok(f"cat {H}/readme.txt").strip() == "found by label"
            assert re.search(r"^vda2\s+\S+\s+0\s+part\s+HUMANVILLAG\s", g.ok("lsblk"), re.M)           # the FAT label is shown
            g.ok(f"dd if=/dev/urandom of={H}/x.bin bs=1k count=700 2>/dev/null; mkdir {H}/d; sync")
            g.poweroff()
        img.check()
        assert img.part_hash(1) == d0, "the decoy partition was modified"
    with Image("nohv", 64 * MIB, [hv_part({"/reimu/a.txt": b"a\n"})]) as img:        # explicit opt-out on the command line
        h0 = img.sha()
        with boot(img, cmdline="nohumanvillage", name="t-storage-nohv") as (g, vm):
            assert "nohumanvillage" in klog(vm)
            assert "fat32" not in g.ok("mount")
            g.poweroff()
        assert img.sha() == h0


@test
def storage_ownership_and_modes():
    """Owners/modes on FAT: user homes private (0700/0600), the rest root 0755/0644, chmod/chown only where harmless."""
    with Image("owners", 64 * MIB, [hv_part({"/shared/pub.txt": b"p\n"})]) as img:
        with boot(img, root=True, name="t-storage-owners") as (g, vm):
            g.ok(f"echo mine > {H}/made-by-root.txt; mkdir {HV}/marisa/sub; echo m > {HV}/marisa/sub/f.txt")
            out = g.ok(f"ls -l {H}/made-by-root.txt {HV}/marisa/sub/f.txt; ls -ld {HV}/marisa/sub")
            assert re.search(r"^-rw-------\s+\d+\s+reimu\s+reimu\s.*made-by-root.txt", out, re.M), out
            assert re.search(r"^-rw-------\s+\d+\s+marisa\s+marisa\s.*f.txt", out, re.M), out
            assert re.search(r"^drwx------\s+\d+\s+marisa\s+marisa\s.*sub", out, re.M), out
            g.ok(f"echo top > {HV}/top.txt; mkdir {HV}/topdir")
            out = g.ok(f"ls -ld {HV}/top.txt {HV}/topdir")
            assert re.search(r"^-rw-r--r--\s+\d+\s+root\s+root\s.*top.txt", out, re.M), out
            assert re.search(r"^drwxr-xr-x\s+\d+\s+root\s+root\s.*topdir", out, re.M), out
            # no-ops succeed, anything that would pretend to store a mode/owner fails
            g.ok(f"chmod 600 {H}/made-by-root.txt; chown reimu {H}/made-by-root.txt; chown reimu:reimu {H}/made-by-root.txt")
            g.fails(f"chmod 666 {H}/made-by-root.txt", r"[Oo]peration not permitted")
            g.fails(f"chmod 700 {HV}/topdir", r"[Oo]peration not permitted")
            g.ok(f"chmod 755 {HV}/topdir")                       # already 0755: harmless, succeeds without storing anything
            g.fails(f"chown marisa {H}/made-by-root.txt", r"[Oo]peration not permitted")
            g.fails(f"chown reimu {HV}/top.txt", r"[Oo]peration not permitted")
            g.ok(f"chmod 400 {H}/made-by-root.txt")                  # the DOS read-only attribute is what a mode can really change
            assert re.search(r"^-r--------", g.ok(f"ls -l {H}/made-by-root.txt"), re.M)
            g.ok(f"chmod 600 {H}/made-by-root.txt")
            assert re.search(r"^-rw-------", g.ok(f"ls -l {H}/made-by-root.txt"), re.M)
            g.ok("sync")
            g.poweroff()
        img.check()
        v = img.volume(1)
        try:
            assert not (v.find("/reimu/made-by-root.txt").attr & mkdisk.ATTR_RO)
        finally:
            v.close()
        with boot(img, name="t-storage-owners2") as (g, vm):                  # as reimu: own home yes, others no
            assert g.ok(f"cat {H}/made-by-root.txt").strip() == "mine"
            g.ok(f"echo reimu-writes > {H}/by-reimu.txt")
            g.fails(f"echo x > {HV}/top.txt", r"[Pp]ermission denied")
            g.fails(f"cat {HV}/marisa/sub/f.txt", r"[Pp]ermission denied")
            assert g.ok(f"cat {HV}/shared/pub.txt").strip() == "p"
            assert re.search(r"^-rw-------\s+\d+\s+reimu\s+reimu", g.ok(f"ls -l {H}/by-reimu.txt"), re.M)
            g.poweroff()
        img.check()


@test
def storage_mount_umount_cli():
    """`mount -t fat32 -o ...`, `umount` flushes and detaches (busy is refused), double mounts and bad options are rejected."""
    with Image("cli", 64 * MIB, [hv_part({"/shared/x.txt": b"x\n"})]) as img:
        with boot(img, root=True, name="t-storage-cli") as (g, vm):
            g.fails(f"cd {HV} && umount {HV}", r"busy")                            # the shell itself is inside
            g.ok("cd /")
            g.ok(f"(echo held > /BambooForest/holding; while [ ! -e /BambooForest/stopholder ]; do sleep 1; done) < {HV}/shared/x.txt &")
            g.ok("while [ ! -e /BambooForest/holding ]; do sleep 1; done")        # the file is open once the marker exists (a slow guest can be late)
            g.fails(f"umount {HV}", r"busy")                                      # a background process holds a file open
            g.ok("touch /BambooForest/stopholder; sleep 3")                       # let it finish
            g.ok(f"echo before-umount > {HV}/shared/u.txt")                       # no sync: umount must flush
            g.fails("mount -t fat32 /dev/vda1 /mnt", r"[Bb]usy")                  # already mounted
            g.ok(f"umount {HV}")
            assert "unmounted cleanly" in klog(vm)
            assert "type fat32" not in g.ok("mount")
            ls = g.ok(f"ls {HV}")
            assert "shared" not in ls and "reimu" in ls, ls                       # the tmpfs homes show through again
            g.fails("mount -t fat32 -o bogus=1 /dev/vda1 /mnt", r"failed")
            g.fails("mount -t fat32 /dev/nosuchdisk /mnt", r"failed")
            g.fails("mount -t fat32 /dev/vda /mnt", r"failed")                    # the whole disk holds no FAT volume
            g.ok("mount -t fat32 -o uid=1000,gid=1000,fmask=0177,dmask=0077 /dev/vda1 /mnt")
            out = g.ok("ls -l /mnt/shared")
            assert re.search(r"^-rw-------\s+\d+\s+reimu\s+reimu\s.*u.txt", out, re.M), out
            assert re.search(r"^drwx------\s+\d+\s+reimu\s+reimu\s.*shared", g.ok("ls -ld /mnt/shared"), re.M)
            assert g.ok("cat /mnt/shared/u.txt").strip() == "before-umount"
            assert re.search(r"/dev/vda1 /mnt fat32 rw", g.ok("cat /proc/mounts"))
            g.ok("umount /mnt")
            g.ok("mount -r -t vfat /dev/vda1 /mnt")                               # alias name, read-only flag
            g.fails("echo no > /mnt/shared/new", r"[Rr]ead-only")
            assert re.search(r"/dev/vda1 /mnt vfat ro", g.ok("cat /proc/mounts"))
            g.poweroff()                                                          # poweroff syncs, then the machine stops
        img.check()


@test
def storage_df_and_statfs():
    """df: sane numbers, -h/-T/-k/-P, paths and errors; statfs on tmpfs, FAT and procfs."""
    with Image("df", 100 * MIB, [hv_part()]) as img:
        with boot(img, name="t-storage-df") as (g, vm):
            out = g.ok("df -T")
            assert re.search(r"^Filesystem\s+Type\s+1K-blocks\s+Used\s+Available\s+Use%\s+Mounted on", out, re.M), out
            assert re.search(r"^/dev/vda1\s+fat32\s+\d+\s+\d+\s+\d+\s+\d+%\s+/HumanVillage", out, re.M), out
            assert re.search(r"^gensokyo\s+tmpfs\s+", out, re.M), out
            assert "/proc" not in out, "pseudo filesystems without blocks are hidden"
            assert "Keine" in out                                                # the themed banner (stdout is a terminal)
            total, used, avail = df_row(g, HV)
            assert total > 90000 and used < 4000 and used + avail == total, (total, used, avail)
            assert total * 1024 < 100 * MIB
            assert re.search(r"^/dev/vda1\s+\d+\s+\d+\s+\d+\s+\d+%\s+/HumanVillage$", g.ok("df -k"), re.M)
            h = g.ok(f"df -h {H}")
            assert re.search(r"/dev/vda1\s+\d+\.?\d*M\s+\S+\s+\d+\.?\d*M\s+\d+%\s+/HumanVillage", h), h
            assert re.search(r"^/dev/vda1\s+\S+\s+\S+\s+\S+\s+\S+\s+/HumanVillage", g.ok(f"df -P {H}"), re.M)
            g.ok(f"dd if=/dev/zero of={H}/z.bin bs=1k count=3000 2>/dev/null")
            t2, u2, a2 = df_row(g, HV)
            assert t2 == total and 3000 <= u2 - used <= 3100 and a2 == t2 - u2, (total, used, avail, t2, u2, a2)
            g.ok(f"rm {H}/z.bin")
            t3, u3, a3 = df_row(g, HV)
            assert u3 == used and a3 == avail, "freed space is not returned"
            assert "procfs" in g.ok("df -a -T /proc")
            g.fails("df /no/such/place", r"No such file")
            g.fails("df -Z", r"invalid option")
            g.poweroff()
        img.check()


# ---------------------------------------------------------------------------------------------------------------------
# geometry, transports, FAT16

def mini_workload(g, base, tag):
    """A short mixed workload used by the geometry tests; returns what to verify afterwards."""
    g.ok(f"mkdir -p {base}/w && cd {base}/w")
    g.ok("dd if=/dev/urandom of=data.bin bs=1k count=1500 2>/dev/null")
    g.ok(f"i=0; while [ $i -lt 60 ]; do echo {tag}$i > \"Long Name Number $i - {tag}.txt\"; i=$((i+1)); done", timeout=120)
    g.ok("mv 'Long Name Number 5 - %s.txt' renamed-five.txt && rm 'Long Name Number 6 - %s.txt'" % (tag, tag))
    g.ok("truncate -s 123 data.bin")
    s = g.ok("sha256sum data.bin").split()[0]
    g.ok("cd /")
    return s


def verify_mini(img, tag, sum123, base="/reimu"):
    v = img.volume(1)
    try:
        data = v.read_file(f"{base}/w/data.bin")
        assert len(data) == 123 and sha(data) == sum123
        names = [e.name for e in v.listdir(f"{base}/w")]
        assert len(names) == 59 + 1 and "renamed-five.txt" in names, names             # 60 files - 1 removed + data.bin
        assert v.read_file(f"{base}/w/Long Name Number 17 - {tag}.txt") == f"{tag}17\n".encode()
        assert v.read_file(f"{base}/w/renamed-five.txt") == f"{tag}5\n".encode()
    finally:
        v.close()


@test
def storage_fat32_4k_clusters():
    """FAT32 with 4 KiB clusters on a larger volume: multi-cluster writes, directory growth, truncate."""
    with Image("c4k", 320 * MIB, [hv_part(cluster=4096)]) as img:
        with boot(img, name="t-storage-c4k") as (g, vm):
            assert "4096-byte clusters" in klog(vm), klog(vm)[-800:]
            s = mini_workload(g, H, "c4k")
            g.poweroff()
        img.check()
        verify_mini(img, "c4k", s)


@test
def storage_fat16_volume():
    """FAT16 read/write (the QEMU ESP is FAT16): fixed root directory that fills up gracefully, 8.3 and long names, fsck clean."""
    files = {"/reimu/KEEP.TXT": b"fat16 content\n", "/reimu/Long Name Here.txt": b"ll\n"}
    with Image("fat16", 80 * MIB, [hv_part(files, kind="fat16")]) as img:
        with boot(img, root=True, name="t-storage-fat16") as (g, vm):
            assert re.search(r"\[Keine\] fat16: vda1 mounted on /HumanVillage", klog(vm)), klog(vm)[-800:]
            assert g.ok(f"cat {H}/keep.txt").strip() == "fat16 content"
            assert "fat16" in g.ok(f"df -T {HV}")
            s = mini_workload(g, H, "f16")
            # the root directory of FAT16 is a fixed table of 512 entries: fill it until the kernel says no
            out = g.ok(f"cd {HV}; i=0; while [ $i -lt 700 ]; do echo $i > r$i.txt 2>/dev/null || break; i=$((i+1)); done; echo reached=$i; cd /",
                       timeout=240)
            reached = int(re.search(r"reached=(\d+)", out).group(1))
            assert 490 <= reached <= 512, out
            g.fails(f"echo x > {HV}/one-more.txt", r"No space left")
            g.ok(f"rm {HV}/r0.txt && echo again > {HV}/again.txt")                  # room again after a delete
            g.poweroff()
        img.check()
        verify_mini(img, "f16", s)
        v = img.volume(1)
        try:
            assert v.bits == 16 and v.read_file("/reimu/Long Name Here.txt") == b"ll\n"
            assert v.read_file("/r%d.txt" % (reached - 1)) == b"%d\n" % (reached - 1)
            assert v.read_file("/again.txt") == b"again\n"
            assert v.find("/r0.txt") is None
        finally:
            v.close()


@test
def storage_4096_byte_sectors():
    """A 4Kn disk (logical_block_size=4096, FAT with 4096-byte sectors): mount, write, flush, fsck."""
    with Image("s4k", 600 * MIB, [hv_part({"/reimu/hello.txt": b"hi 4k\n"})], sector_size=4096) as img:
        extra = drive_args(img.path, logical_block_size=4096, physical_block_size=4096)
        with boot(None, name="t-storage-4k", extra=extra) as (g, vm):
            assert re.search(r"4096-byte sectors", klog(vm))
            assert re.search(r"^vda\d* \d+ 4096 ", g.ok("cat /proc/partitions"), re.M)
            assert g.ok(f"cat {H}/hello.txt").strip() == "hi 4k"
            s = mini_workload(g, H, "s4k")
            g.poweroff()
        img.check()
        verify_mini(img, "s4k", s)
        v = img.volume(1)
        try:
            assert v.bps == 4096
        finally:
            v.close()


@test
def storage_posix_semantics():
    """/bin/fstest (380 checks of errno values, open-unlinked files, renames, names, holes, 700-entry directories) on three FAT geometries."""
    for tag, size, part, root in (("c512", 80 * MIB, hv_part(), False), ("c4k", 320 * MIB, hv_part(cluster=4096), True),
                                  ("fat16", 80 * MIB, hv_part(kind="fat16"), True)):
        with Image("posix-" + tag, size, [part]) as img:
            with boot(img, root=root, name="t-storage-posix") as (g, vm):
                out = g.ok(f"fstest {H}", timeout=300)
                assert "FSTEST PASS" in out, f"{tag}:\n{out[-2500:]}"
                if root:
                    out = g.ok(f"fstest {HV}", timeout=300)                 # the root directory of the volume (FAT16: fixed table)
                    assert "FSTEST PASS" in out, f"{tag} root:\n{out[-2500:]}"
                g.poweroff()
            rep = img.check()
            assert rep.stats["p1"]["files"] == 0, "fstest must clean up after itself"


@test
def storage_virtio_transports():
    """virtio-blk over the legacy I/O-port transport and over the modern (capability/MMIO) transport."""
    for variant, expect in (("disable-modern=on", "legacy transport"), ("disable-legacy=on", "modern transport")):
        with Image("transport", 64 * MIB, [hv_part({"/reimu/t.txt": b"transport\n"})]) as img:
            extra = ["-drive", f"if=none,id=d0,format=raw,file={img.path}", "-device", f"virtio-blk-pci,drive=d0,{variant}"]
            with boot(None, name="t-storage-transport", extra=extra) as (g, vm):
                assert expect in klog(vm), (variant, klog(vm)[-800:])
                assert g.ok(f"cat {H}/t.txt").strip() == "transport"
                g.ok(f"dd if=/dev/urandom of={H}/rnd bs=1k count=600 2>/dev/null; echo w > {H}/w.txt")
                g.poweroff()
            img.check()
            v = img.volume(1)
            try:
                assert v.read_file("/reimu/w.txt") == b"w\n" and len(v.read_file("/reimu/rnd")) == 600 * 1024
            finally:
                v.close()


# ---------------------------------------------------------------------------------------------------------------------
# randomized stress against a model

POOL_SIZES = [1, 100, 511, 512, 513, 2000, 4095, 4096, 4097, 9000, 70000, 262144, 300001]
NAMES = ["a", "b", "DATA", "Readme", "x1", "LOG", "notes.txt", "K.bin", "A Long File Name.txt", "MixedCase_Name.Bin",
         "file.with.dots.tar.gz", "UPPER LOWER mix.dat", "report (final) v2.doc", "lower.txt", "NoExt", "ten_chars_x.ab"]


class StressModel:
    """The tree the guest should end up with, mirrored operation by operation (names compare case-insensitively)."""

    def __init__(self, root):
        self.root = root
        self.dirs = {root}
        self.files = {}                     # path -> bytes
        self.low = {root.lower()}           # every path in use, lower-cased

    def has(self, p):
        return p.lower() in self.low

    def add(self, p, is_dir, data=b""):
        assert not self.has(p), p
        self.low.add(p.lower())
        if is_dir:
            self.dirs.add(p)
        else:
            self.files[p] = data

    def drop(self, p):
        self.low.discard(p.lower())
        self.files.pop(p, None)
        self.dirs.discard(p)

    def children(self, d):
        pre = d + "/"
        return [p for p in list(self.files) + list(self.dirs) if p.startswith(pre) and "/" not in p[len(pre):]]

    def snapshot(self, strip):
        out = {d[len(strip):]: ("d", 0, "") for d in self.dirs}
        out.update({p[len(strip):]: ("f", len(data), sha(data)) for p, data in self.files.items()})
        return out


def stress_script(seed, nops, root):
    """-> (shell script, model, pool). The same random choices drive the script and the model."""
    rng = random.Random(seed)
    pool = {i: bytes(rng.randrange(256) for _ in range(sz)) for i, sz in enumerate(POOL_SIZES)}
    m = StressModel(root)
    lines = ["# generated by tests/case_storage.py", f"mkdir {root}"]
    counter = [0]

    def fresh(d):
        while True:
            counter[0] += 1
            base = rng.choice(NAMES)
            stem, dot, ext = base.rpartition(".")
            name = base if (rng.random() < 0.25 and counter[0] % 3) else (f"{stem}{counter[0]}.{ext}" if dot else f"{base}{counter[0]}")
            if not m.has(d + "/" + name):
                return name

    def emit(cmd):
        lines.append(f"{cmd} || echo STRESSFAIL {len(lines)}")

    q = lambda p: '"' + p + '"'
    for _ in range(nops):
        files, dirs = sorted(m.files), sorted(m.dirs)
        r = rng.random()
        if r < 0.22 or not files:
            d = rng.choice(dirs)
            p = d + "/" + fresh(d)
            k = rng.choice(list(pool))
            emit(f"cp /HumanVillage/pool/p{k} {q(p)}")
            m.add(p, False, pool[k])
        elif r < 0.34:
            p, k = rng.choice(files), rng.choice(list(pool))
            emit(f"cat /HumanVillage/pool/p{k} >> {q(p)}")
            m.files[p] += pool[k]
        elif r < 0.44:
            p = rng.choice(files)
            k = rng.choice([k for k in pool if len(pool[k]) >= 512])
            blocks = min(rng.randint(1, len(pool[k]) // 512), 40)
            seek = rng.randint(0, len(m.files[p]) // 512 + 2)
            emit(f"dd if=/HumanVillage/pool/p{k} of={q(p)} bs=512 seek={seek} count={blocks} conv=notrunc 2>/dev/null")
            data = bytearray(m.files[p])
            end = (seek + blocks) * 512
            if end > len(data):
                data.extend(bytes(end - len(data)))
            data[seek * 512:end] = pool[k][:blocks * 512]
            m.files[p] = bytes(data)
        elif r < 0.48:
            p, k = rng.choice(files), rng.choice(list(pool))
            emit(f"cp /HumanVillage/pool/p{k} {q(p)}")                  # replace the whole content
            m.files[p] = pool[k]
        elif r < 0.60:
            p = rng.choice(files)
            emit(f"rm {q(p)}")
            m.drop(p)
        elif r < 0.68:
            p = rng.choice(files)                                      # rename within a directory (sometimes only the case)
            d, leaf = p.rsplit("/", 1)
            if rng.random() < 0.2:
                new = d + "/" + leaf.swapcase()
                if new == p or (m.has(new) and new.lower() != p.lower()):
                    continue
                data = m.files[p]
                emit(f"mv {q(p)} {q(new)}")
                m.drop(p)
                m.add(new, False, data)
            else:
                new = d + "/" + fresh(d)
                data = m.files[p]
                emit(f"mv {q(p)} {q(new)}")
                m.drop(p)
                m.add(new, False, data)
        elif r < 0.74 and len(files) >= 2:
            a, b = rng.sample(files, 2)                                # rename over an existing file
            data = m.files[a]
            emit(f"mv {q(a)} {q(b)}")
            m.drop(a)
            m.files[b] = data
        elif r < 0.80:
            p, d2 = rng.choice(files), rng.choice(dirs)                # move to another directory
            new = d2 + "/" + fresh(d2)
            data = m.files[p]
            emit(f"mv {q(p)} {q(new)}")
            m.drop(p)
            m.add(new, False, data)
        elif r < 0.86 and len(dirs) < 12:
            d = rng.choice(dirs)
            p = d + "/" + fresh(d)
            emit(f"mkdir {q(p)}")
            m.add(p, True)
        elif r < 0.90:
            empties = [x for x in dirs if x != root and not m.children(x)]
            if not empties:
                continue
            p = rng.choice(empties)
            emit(f"rmdir {q(p)}")
            m.drop(p)
        elif r < 0.94:
            cands = [x for x in dirs if x != root]
            if not cands:
                continue
            src, d2 = rng.choice(cands), rng.choice(dirs)              # move a whole directory
            if d2 == src or d2.startswith(src + "/"):
                continue
            new = d2 + "/" + fresh(d2)
            emit(f"mv {q(src)} {q(new)}")
            moved_f = {p: data for p, data in m.files.items() if p.startswith(src + "/")}
            moved_d = [x for x in m.dirs if x == src or x.startswith(src + "/")]
            for x in moved_d:
                m.drop(x)
            for p in moved_f:
                m.drop(p)
            for x in moved_d:
                m.add(new + x[len(src):], True)
            for p, data in moved_f.items():
                m.add(new + p[len(src):], False, data)
        else:
            p = rng.choice(files)
            cur = len(m.files[p])
            size = rng.choice([0, cur // 2, max(0, cur - 1), cur + 1, cur + 4096, cur * 2 + 7, rng.randint(0, 20000)])
            emit(f"truncate -s {size} {q(p)}")
            m.files[p] = m.files[p][:size] + bytes(max(0, size - cur))
    lines.append("echo STRESSDONE")
    return "\n".join(lines) + "\n", m, pool


@test
def storage_random_stress():
    """~450 random create/append/overwrite/truncate/delete/rename/mkdir/rmdir operations mirrored in a Python model (fixed seed)."""
    root = "/HumanVillage/reimu/st"
    script, model, pool = stress_script(0x70DE, 450, root)
    files = {f"/pool/p{k}": data for k, data in pool.items()}
    files["/stress.sh"] = script.encode()
    with Image("stress", 80 * MIB, [hv_part(files)]) as img:
        with boot(img, name="t-storage-stress") as (g, vm):
            out = g.ok("gapsh /HumanVillage/stress.sh", timeout=900)
            assert "STRESSDONE" in out, out[-1500:]
            assert "STRESSFAIL" not in out, "\n".join(l for l in out.splitlines() if "STRESSFAIL" in l or "rror" in l)[:1500]
            g.poweroff()
        img.check()
        v = img.volume(1)
        try:
            got = {k: val for k, val in v.snapshot().items() if k.startswith("/reimu/st")}
            stored = {e.path for d in v._tree_cache().values() for e in d}
        finally:
            v.close()
        want = model.snapshot("/HumanVillage")
        missing, extra = sorted(set(want) - set(got)), sorted(set(got) - set(want))
        assert not missing and not extra, f"missing {missing[:8]} extra {extra[:8]}"
        bad = [p for p in want if want[p] != got[p]]
        assert not bad, f"{len(bad)} mismatches, first: {bad[:5]} want {want[bad[0]]} got {got[bad[0]]}"
        assert all(p in stored for p in want), "stored name case differs from the model"


# ---------------------------------------------------------------------------------------------------------------------
# power loss, periodic flush, read-only media, hostile volumes

@test
def storage_periodic_flush():
    """Without any sync the flusher thread writes the data back within a few seconds and marks the volume clean."""
    with Image("periodic", 64 * MIB, [hv_part()]) as img:
        with boot(img, name="t-storage-periodic") as (g, vm):
            g.ok(f"echo written-without-sync > {H}/p.txt; dd if=/dev/urandom of={H}/p.bin bs=1k count=200 2>/dev/null")
            g.ok("sleep 7")                                         # guest time: more than the flush age (3 s) plus the tick (1 s)
            try:
                vm.qmp("quit")                                      # pull the plug: no sync, no poweroff
            except Exception:                                       # noqa: BLE001
                pass
            vm.proc.wait(timeout=scaled(20))
        img.check()                                                 # clean: the idle flush cleared the dirty flags
        v = img.volume(1)
        try:
            assert v.read_file("/reimu/p.txt") == b"written-without-sync\n" and len(v.read_file("/reimu/p.bin")) == 200 * 1024
        finally:
            v.close()


@test
def storage_unclean_power_loss():
    """Pulling the plug right after a write leaves the dirty flags set; the next boot refuses to write to that volume."""
    with Image("powerloss", 64 * MIB, [hv_part({"/reimu/old.txt": b"old\n"})]) as img:
        with boot(img, name="t-storage-loss") as (g, vm):
            g.ok(f"echo new > {H}/new.txt")
            try:
                vm.qmp("stop")                                      # freeze the guest first: no flush thread can sneak in
                vm.qmp("quit")
            except Exception:                                       # noqa: BLE001
                pass
            vm.proc.wait(timeout=scaled(20))
        rep = img.check(allow_dirty=True)
        assert not rep.stats["p1"]["clean"], "the volume must be marked dirty while it has unwritten changes"
        h1 = img.sha()
        with boot(img, name="t-storage-loss2") as (g, vm):
            assert re.search(r"read-only: the volume was not cleanly unmounted", klog(vm)), klog(vm)[-1200:]
            assert g.ok(f"cat {H}/old.txt").strip() == "old"
            g.fails(f"echo again > {H}/again.txt", r"[Rr]ead-only")
            g.ok("sync")
            g.poweroff()
        assert img.sha() == h1, "a read-only mount wrote to the volume"


@test
def storage_readonly_device():
    """A write-protected virtio disk (VIRTIO_BLK_F_RO): shown as RO, mounted read-only, never written, homes not created."""
    with Image("rodev", 64 * MIB, [hv_part({"/reimu/a.txt": b"a\n"})]) as img:
        h0 = img.sha()
        extra = ["-drive", f"if=none,id=d0,format=raw,file={img.path},readonly=on", "-device", "virtio-blk-pci,drive=d0"]
        with boot(None, root=True, name="t-storage-rodev", extra=extra) as (g, vm):
            assert re.search(r"^vda\s+\S+\s+1\s+disk", g.ok("lsblk"), re.M)
            assert re.search(r"virtio-blk .*read-only", klog(vm)) or "read-only" in klog(vm)
            assert re.search(r"/dev/vda1 /HumanVillage fat32 ro ", g.ok("cat /proc/mounts"))
            assert "cannot create /HumanVillage/marisa" in klog(vm) and "read-only" in klog(vm), klog(vm)[-1500:]
            assert g.ok(f"cat {H}/a.txt").strip() == "a"
            g.fails(f"echo x > {H}/b.txt", r"[Rr]ead-only")
            assert "Read-only file system" in g.ok("dd if=/dev/zero of=/dev/vda bs=512 count=1 2>&1")      # even root cannot write the raw device
            g.ok("sync")
            g.poweroff()
        assert img.sha() == h0


def damaged_boot_sector_cases(total_sectors):
    """(offset in the boot sector, bytes, reason the kernel must give) for a FAT32 volume of `total_sectors` sectors."""
    return [
        (11, struct.pack("<H", 768), "unsupported sector size"),
        (13, bytes([3]), "bad cluster size"),
        (16, bytes([0]), "bad number of FATs"),
        (14, struct.pack("<H", 0), "no reserved sectors"),
        (32, struct.pack("<I", total_sectors + 100), "larger than its device"),
        (36, struct.pack("<I", 3), "FAT is too small"),
        (44, struct.pack("<I", 0x0FFFFFF0), "root cluster out of range"),
        (510, b"\x00\x00", "no boot sector signature"),
        (22, struct.pack("<H", 5), "FAT32 geometry is inconsistent"),
    ]


@test
def storage_refuses_damaged_volumes():
    """Nine different broken boot sectors are refused with a reason, nothing is written, and the kernel stays alive."""
    parts = [dict(name=f"BAD{i + 1}", kind="fat32", size=34 * MIB, label=f"BAD{i + 1}") for i in range(9)]
    with Image("badbpb", 340 * MIB, parts) as img:
        _, layout = mkdisk.partitions(img.path)
        cases = damaged_boot_sector_cases(layout[0]["length"] // 512)
        with open(img.path, "r+b") as f:
            for p, (off, data, _) in zip(layout, cases):
                f.seek(p["offset"] + off)
                f.write(data)
        h0 = img.sha()
        with boot(img, root=True, name="t-storage-badbpb") as (g, vm):
            assert "no HUMANVILLAGE volume found" in klog(vm)
            for i, (off, data, reason) in enumerate(cases):
                g.fails(f"mount -t fat32 /dev/vda{i + 1} /mnt", r"failed")
                assert re.search(rf"vda{i + 1} is not a mountable FAT volume: .*{reason}", klog(vm)), (i + 1, reason, klog(vm)[-1500:])
            assert "type fat32" not in g.ok("mount")
            g.ok("sync")
            g.poweroff()
        assert img.sha() == h0, "refused volumes must not be touched"


@test
def storage_contains_damaged_files():
    """Loops, out-of-range clusters, short/long chains and cross-linked directories give EIO, never a hang or a stray write."""
    files = {
        "/reimu/good.txt": b"good\n",
        "/reimu/loop.bin": b"L" * 1500,
        "/reimu/short.bin": b"S" * 2000,
        "/reimu/surplus.bin": b"U" * 2000,
        "/reimu/ghost.txt": b"G" * 10,
        "/reimu/badfirst.bin": b"B" * 700,
        "/reimu/evil/inner.txt": b"inner\n",
        "/reimu/crossdir/x": b"x",
        "/reimu/victim.bin": b"V" * 3000,
        "/other/safe.txt": b"safe\n",
    }
    with Image("damaged", 64 * MIB, [hv_part(files)]) as img:
        r = mkdisk.open_volume(img.path, 1, writable=True)
        try:
            loop = r.chain(r.find("/reimu/loop.bin").first)
            r.set_fat(loop[-1], loop[0])                                                  # the chain runs in a circle
            r.set_entry("/reimu/short.bin", size=4000)                                    # claims 8 clusters, owns 4
            r.set_entry("/reimu/surplus.bin", size=600)                                   # claims 2 clusters, owns 4
            r.set_entry("/reimu/ghost.txt", first=0)                                      # a size but no clusters
            r.set_entry("/reimu/badfirst.bin", first=r.nclusters + 100)                   # first cluster beyond the volume
            r.set_entry("/reimu/evil", first=r.nclusters + 100)                           # directory beyond the volume
            r.set_entry("/reimu/crossdir", first=r.find("/reimu/good.txt").first)         # directory sharing a file's cluster
            r.set_entry("/reimu/victim.bin", first=r.nclusters + 2)                       # exactly one past the last cluster
        finally:
            r.close()
        with boot(img, root=True, name="t-storage-damaged") as (g, vm):
            assert g.ok(f"cat {H}/good.txt").strip() == "good" and g.ok(f"cat {HV}/other/safe.txt").strip() == "safe"
            out, _ = g.run(f"cat {H}/loop.bin | wc -c", timeout=30)                       # terminates, whatever it returns
            assert out.strip().isdigit(), out
            g.fails(f"printf x | cat - >> {H}/loop.bin", r"[Ii]nput/output|I/O")
            assert g.ok(f"cat {H}/surplus.bin | wc -c").strip() == "600"                  # readable ...
            g.fails(f"printf x | cat - >> {H}/surplus.bin", r"[Ii]nput/output|I/O")                 # ... but never written
            g.fails(f"truncate -s 10 {H}/surplus.bin", r"[Ii]nput/output|I/O")
            g.fails(f"cat {H}/short.bin > /dev/null", r"[Ii]nput/output|I/O")
            g.fails(f"printf x | cat - >> {H}/short.bin", r"[Ii]nput/output|I/O")
            g.fails(f"cat {H}/ghost.txt", r"[Ii]nput/output|I/O")
            g.fails(f"cat {H}/badfirst.bin", r"[Ii]nput/output|I/O")
            g.fails(f"cat {H}/victim.bin", r"[Ii]nput/output|I/O")
            out, _ = g.run(f"ls {H}/evil", timeout=30)                                  # the damaged directory reads as empty
            assert not out.strip(), out
            g.run(f"ls -la {H}/crossdir", timeout=30)                                     # garbage entries, but no crash
            assert re.search(r"^-rw-------\s+\d+\s+reimu\s+reimu\s+0\s.*ghost.txt", g.ok(f"ls -l {H}"), re.M)
            g.ok(f"rm {H}/surplus.bin {H}/short.bin {H}/ghost.txt {H}/badfirst.bin")      # deleting is always possible
            g.ok(f"echo still-writable > {H}/fine.txt && cat {H}/fine.txt")
            assert g.ok("echo alive").strip() == "alive"
            g.ok("sync")
            g.poweroff()
        assert "INCIDENT" not in klog(vm) and "BOUNDARY VIOLATION" not in klog(vm)
        v = img.volume(1)
        try:
            assert v.read_file("/other/safe.txt") == b"safe\n" and v.read_file("/reimu/good.txt") == b"good\n"
            assert v.read_file("/reimu/fine.txt") == b"still-writable\n"
        finally:
            v.close()


@test
def storage_fsinfo_hints():
    """A lying FSInfo does not mislead df or the allocator, and is corrected by the next clean sync."""
    homes = {"/reimu/a.txt": b"a\n", "/marisa/m.txt": b"m\n", "/sakuya/s.txt": b"s\n"}        # all three homes exist: boot creates nothing
    with Image("fsinfo", 64 * MIB, [hv_part(homes)]) as img:
        v = mkdisk.open_volume(img.path, 1, writable=True)
        try:
            true_free = img.check().stats["p1"]["free"]
            cluster = v.cluster_bytes
            v.win.write(v.fsinfo_sector * v.bps + 488, struct.pack("<II", 12345, 7))        # free count and next-free hint
        finally:
            v.close()
        rep = mkdisk.check_image(img.path)
        assert any("FSInfo free count" in e for e in rep.errors), rep.text()
        with boot(img, name="t-storage-fsinfo") as (g, vm):
            assert df_row(g, HV)[2] == true_free * cluster // 1024, "df must come from the FAT, not from the hint"
            g.ok(f"echo fresh > {H}/b.txt; dd if=/dev/urandom of={H}/c.bin bs=1k count=100 2>/dev/null")
            g.poweroff()
        img.check()                                             # FSInfo is right again


@test
def storage_mkdisk_cli():
    """tools/mkdisk.py as a command: build, --check (exit status), --ls, --cat, --extract, --tree; MBR, FAT16, 4096-byte sectors."""
    import subprocess
    import sys
    tool = [sys.executable, str(Path(mkdisk.__file__))]
    d = WORK / "cli-tool"
    shutil.rmtree(d, ignore_errors=True)
    (d / "src" / "a dir").mkdir(parents=True)
    (d / "src" / "hello.txt").write_bytes(b"hello from the host\n")
    (d / "src" / "a dir" / "file two with a long name.txt").write_bytes(b"two\n" * 1000)
    run = lambda *a: subprocess.run(tool + [str(x) for x in a], capture_output=True)
    try:
        img = d / "t.img"
        r = run(img, "--size", "140M", "--esp", d / "src", "--hv-dir", d / "src", "--hv-size", "60M")
        assert r.returncode == 0, r.stdout + r.stderr
        assert b"TOUHOUOS-ESP" in r.stdout and b"HUMANVILLAGE" in r.stdout, r.stdout
        r = run("--check", img)
        assert r.returncode == 0 and b"0 error(s)" in r.stdout, r.stdout
        assert b"file two with a long name.txt" in run("--ls", f"{img}:2", "/a dir").stdout
        assert b"[FILETW~1.TXT]" in run("--ls", f"{img}:2", "/a dir").stdout                    # the generated short name is shown
        assert run("--cat", f"{img}:2", "/hello.txt").stdout == b"hello from the host\n"
        assert run("--cat", f"{img}:1", "/HELLO.TXT").stdout == b"hello from the host\n"        # ESP, case-insensitive
        assert b"/a dir/file two with a long name.txt" in run("--tree", f"{img}:2").stdout
        out = d / "out"
        assert run("--extract", f"{img}:2", "/", out).returncode == 0
        assert (out / "a dir" / "file two with a long name.txt").read_bytes() == b"two\n" * 1000
        with open(img, "r+b") as f:                                                                # damage: a FAT byte
            v = mkdisk.open_volume(img, 2)
            try:
                off = v.part["offset"] + v.reserved * v.bps + 4000 * 4
            finally:
                v.close()
            f.seek(off)
            f.write(b"\xff\xff\xff\x0f")
        r = run("--check", img)
        assert r.returncode == 1 and b"lost cluster" in r.stdout, r.stdout
        for opts in (["--table", "mbr", "--no-esp"], ["--no-esp", "--hv-fat16", "--size", "80M"], ["--no-hv", "--size", "100M"],
                     ["--sector-size", "4096", "--size", "600M", "--no-esp"]):
            kw = ["--size", "100M"] if "--size" not in opts else []
            r = run(d / "t2.img", *kw, *opts)
            assert r.returncode == 0, (opts, r.stdout, r.stderr)
            assert run("--check", d / "t2.img").returncode == 0, opts
    finally:
        shutil.rmtree(d, ignore_errors=True)


@test
def storage_small_ram():
    """With 96 MiB of RAM (a small page cache) a 60 MiB file and 200 files still go through intact."""
    with Image("smallram", 200 * MIB, [hv_part()]) as img:
        with boot(img, name="t-storage-smallram", mem="96M") as (g, vm):
            g.ok(f"dd if=/dev/urandom of={H}/big bs=1k count=61440 2>/dev/null", timeout=300)
            s = g.ok(f"sha256sum {H}/big", timeout=120).split()[0]
            g.ok(f"mkdir {H}/d; i=0; while [ $i -lt 200 ]; do echo $i > \"{H}/d/file number $i with a longish name.txt\"; i=$((i+1)); done",
                 timeout=180)
            assert g.ok(f"sha256sum {H}/big", timeout=120).split()[0] == s                     # re-read after eviction
            g.poweroff()
        img.check()
        v = img.volume(1)
        try:
            assert sha(v.read_file("/reimu/big")) == s and len(v.listdir("/reimu/d")) == 200
        finally:
            v.close()


@test
def storage_two_disks():
    """Two virtio disks: HUMANVILLAGE on vda, another FAT32 volume mounted by hand from vdb; copy and move between them."""
    other = dict(name="DATA", kind="fat32", size="rest", label="DATA", files={"/in-b.txt": b"from b\n"})
    with Image("two-a", 64 * MIB, [hv_part()]) as a, Image("two-b", 64 * MIB, [other]) as b:
        with boot(a, root=True, name="t-storage-two", extra=["-drive", f"if=none,id=d1,format=raw,file={b.path}",
                                                              "-device", "virtio-blk-pci,drive=d1"]) as (g, vm):
            out = g.ok("lsblk")
            assert re.search(r"^vda\s", out, re.M) and re.search(r"^vdb1\s.*DATA", out, re.M), out
            g.ok("mount -t fat32 /dev/vdb1 /mnt")
            assert re.search(r"/dev/vdb1 on /mnt type fat32", g.ok("mount"))
            g.ok(f"dd if=/dev/urandom of={H}/x.bin bs=1k count=2000 2>/dev/null")
            g.ok(f"cp {H}/x.bin /mnt/x-copy.bin")
            g.ok(f"cmp {H}/x.bin /mnt/x-copy.bin")
            g.ok(f"mv /mnt/in-b.txt {H}/moved.txt")                                          # EXDEV: copy, then remove
            g.ok(f"mv {H}/x.bin /mnt/x-moved.bin")
            sums = g.ok("sha256sum /mnt/x-copy.bin /mnt/x-moved.bin").split()
            assert sums[0] == sums[2]
            out = g.ok("df -T")
            assert "/dev/vda1" in out and "/dev/vdb1" in out, out
            g.poweroff()
        a.check()
        b.check()
        va, vb = a.volume(1), b.volume(1)
        try:
            assert va.read_file("/reimu/moved.txt") == b"from b\n" and va.find("/reimu/x.bin") is None
            assert sha(vb.read_file("/x-copy.bin")) == sums[0] and vb.find("/in-b.txt") is None
        finally:
            va.close()
            vb.close()
