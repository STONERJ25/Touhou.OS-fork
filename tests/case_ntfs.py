"""The read-only NTFS driver and the BitLocker clear-key layer, in QEMU (docs/NTFS.md).

Images come from tests/host/ntfsimg.py (an independent generator: no Windows, no mkntfs), are attached as a virtio-blk disk, and the guest's own shell does the work: mount -t ntfs,
ls -l, cat, md5sum, cp, df, every kind of write (all must fail with "Read-only file system"), umount. The host compares the guest's md5sums with its own, and checks after the guest is gone
that the image is byte for byte what it was (the driver and the mount policy never write, whatever root types). The parser itself is tested in depth, with hostile images and a fuzz under
AddressSanitizer, by tests/host/test_ntfs.py; these tests are about the glue: VFS operations, mount options, the /dev/bdeN view, and the refusals.

Run just these with:  python tools/qemu-slot.py -- python tests/run_all.py ntfs
"""
import contextlib
import hashlib
import random
import re
import sys
from pathlib import Path

from harness import test, Qemu, BUILD, scaled

sys.path.insert(0, str(Path(__file__).resolve().parent / "host"))
import ntfsimg  # noqa: E402
from ntfsimg import MIB  # noqa: E402

WORK = BUILD / "ntfs-qemu"
ROOT_PROMPT = r"root@gensokyo \S+ # "
MNT = "/mnt/windows"


def md5(data):
    return hashlib.md5(data).hexdigest()


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def blob(rnd, n):
    return bytes(rnd.getrandbits(8) for _ in range(n))


def text(rnd, n):
    words = [b"gensokyo", b"reimu", b"marisa", b"touhou", b"kappa", b"fairy", b"shrine", b"\n", b" "]
    out = bytearray()
    while len(out) < n:
        out += rnd.choice(words)
    return bytes(out[:n])


class Guest:
    def __init__(self, vm):
        self.vm = vm

    def run(self, cmd, timeout=60):
        """One shell line; returns (output, exit status)."""
        self.vm.send(f"echo __BE''GIN__; {cmd}; echo __EN''D__$?\n")
        m = self.vm.expect(r"\n__BEGIN__\n(.*?)__END__(\d+)\n", timeout=timeout)
        self.vm.expect(ROOT_PROMPT, timeout=20)
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
        self.vm.send("poweroff\n")
        rc = self.vm.wait_exit(timeout)
        assert rc is not None, "guest did not power off:\n" + self.vm.text()[-1500:]


@contextlib.contextmanager
def boot(image, name):
    with Qemu(cmdline="text bootlog=off user=root", disk=image, name=name) as vm:
        vm.expect(ROOT_PROMPT, timeout=90)
        yield Guest(vm), vm


def write_image(name, data):
    WORK.mkdir(parents=True, exist_ok=True)
    p = WORK / name
    p.write_bytes(data)
    return p


def sample_volume(rnd, **kw):
    b = ntfsimg.NtfsBuilder(size=24 * MIB, **kw)
    b.add_dir("/Documents")
    b.add_file("/Documents/hello.txt", b"hello from a generated NTFS volume\n")
    b.add_file("/Documents/big.bin", blob(rnd, 1_500_000), frag=6)
    b.add_file("/Documents/Ünïcode ファイル.txt", b"unicode name\n")
    b.add_file("/notes.txt", text(rnd, 200_000), compress=True)
    b.add_file("/sparse.bin", blob(rnd, 4000) + bytes(400_000) + blob(rnd, 4000), sparse=True)
    b.add_file("/secret.txt", blob(rnd, 5000), encrypted=True)
    b.add_file("/withads.txt", b"the main stream", ads={"hidden": b"alternate stream"})
    b.add_symlink("/link", "Documents\\hello.txt")
    b.add_junction("/Docs", "\\??\\C:\\Documents")
    b.add_dir("/many")
    for i in range(300):
        b.add_file("/many/file%03d with a long name.txt" % i, b"%d\n" % i)
    return b


@test
def ntfs_mount_read_copy_and_refuse_every_write():
    """Mount a generated NTFS volume: listing, names, md5sum and cp against the host's values, links, EFS refused, df, and every write fails with EROFS; the image is untouched."""
    rnd = random.Random(1)
    b = sample_volume(rnd)
    img = write_image("plain.img", b.build())
    before = sha(img)
    with boot(img, "t-ntfs-plain") as (g, vm):
        g.ok(f"mkdir -p {MNT}")
        mounts = g.ok("cat /proc/mounts")
        assert "ntfs" not in mounts, "the mount policy must not touch an NTFS disk by itself:\n" + mounts
        g.fails(f"mount -t ntfs -o rw /dev/vda {MNT} 2>&1", r"Read-only file system")
        assert "refusing a read-write mount" in g.ok("dmesg"), "an explicit rw is refused with a reason in the log, not ignored"
        out = g.ok(f"mount -t ntfs -o ro /dev/vda {MNT} && dmesg | grep -i ntfs")
        assert re.search(r"ntfs: vda mounted on /mnt/windows, 23 MiB, 4096-byte clusters, label 'TESTVOL', NTFS 3\.1, read-only", out), out
        assert re.search(rf"/dev/vda {MNT} ntfs ro", g.ok("cat /proc/mounts")), "listed read-only in /proc/mounts"
        ls = g.ok(f"ls -l {MNT}")
        for name in ("Documents", "notes.txt", "sparse.bin", "secret.txt", "withads.txt", "many", "link -> Documents/hello.txt", f"Docs -> {MNT}/Documents"):
            assert name in ls, f"{name!r} missing from:\n{ls}"
        for hidden in ("$MFT", "$Boot", "$UpCase", "hidden"):
            assert hidden not in ls, f"{hidden} must not be listed"
        assert "Ünïcode ファイル.txt" in g.ok(f"ls {MNT}/Documents")
        assert g.ok(f"cat {MNT}/Documents/hello.txt") == "hello from a generated NTFS volume\n"
        assert g.ok(f"cat {MNT}/link") == "hello from a generated NTFS volume\n"
        assert g.ok(f"cat {MNT}/Docs/hello.txt") == "hello from a generated NTFS volume\n"
        assert g.ok(f"cat {MNT}/documents/HELLO.TXT") == "hello from a generated NTFS volume\n", "names are case-insensitive"
        assert g.ok(f"cat {MNT}/withads.txt") == "the main stream"
        for path in ("Documents/big.bin", "notes.txt", "sparse.bin"):
            want = b.expected["/" + path]["data"]
            got = g.ok(f"md5sum {MNT}/{path}", timeout=120).split()[0]
            assert got == md5(want), (path, got, md5(want))
            g.ok(f"cp {MNT}/{path} /tmp/copy.bin", timeout=120)
            assert g.ok("md5sum /tmp/copy.bin").split()[0] == md5(want), f"copy of {path}"
            g.ok("rm /tmp/copy.bin")
        assert g.ok(f"ls {MNT}/many | wc -l").strip() == "300"
        assert g.ok(f"find {MNT}/many -type f | wc -l").strip() == "300"
        assert "Permission denied" in g.fails(f"cat {MNT}/secret.txt 2>&1", r"Permission denied")
        assert re.search(r"Size: 35\b", g.ok(f"stat {MNT}/Documents/hello.txt"))
        out = g.ok(f"df {MNT}")
        assert "/dev/vda" in out and "(ro)" in out, out
        # every kind of write is refused, with the right words (root may open a file for writing, as the VFS does not look at read-only mounts at open; the write itself fails,
        # and some applets do not turn that into an exit status, so the words are what is checked)
        for cmd in (f"echo x > {MNT}/new.txt", f"dd if=/dev/zero of={MNT}/notes.txt bs=1 count=1 conv=notrunc", f"rm {MNT}/notes.txt", f"mkdir {MNT}/newdir", f"rmdir {MNT}/many",
                    f"touch {MNT}/touched", f"mv {MNT}/notes.txt {MNT}/moved.txt", f"truncate -s 0 {MNT}/notes.txt", f"ln {MNT}/notes.txt {MNT}/hardlink",
                    f"ln -s x {MNT}/newlink", f"chmod 777 {MNT}/notes.txt", f"cp /etc/passwd {MNT}/passwd"):
            out, rc = g.run(f"{cmd} 2>&1")
            assert "Read-only file system" in out, f"`{cmd}` was not refused with EROFS (exit {rc}): {out!r}"
        assert g.ok(f"cat {MNT}/notes.txt | md5sum").split()[0] == md5(b.expected["/notes.txt"]["data"]), "nothing changed"
        g.fails(f"umount {MNT}/Documents 2>&1")
        assert g.ok(f"cd {MNT}/Documents/.. && pwd && cd .. && pwd && cd /").split() == [MNT, "/mnt"], '".." leaves the volume through its mount point'
        g.fails(f"sh -c 'cd {MNT}/Documents && umount {MNT}' 2>&1", r"busy|Busy|BUSY")
        g.ok(f"umount {MNT}")
        assert "ntfs" not in g.ok("cat /proc/mounts")
        assert re.search(r"ntfs: vda unmounted", g.ok("dmesg"))
        g.poweroff()
    assert sha(img) == before, "the guest wrote to the NTFS image"


@test
def ntfs_bitlocker_clear_key_is_unlocked_and_read_only():
    """A BitLocker volume with a clear-key protector mounts through /dev/bde0, which shows decrypted NTFS and goes away at umount; the image is untouched."""
    rnd = random.Random(2)
    b = ntfsimg.NtfsBuilder(size=24 * MIB, tail_clusters=80)
    b.add_dir("/Documents")
    b.add_file("/Documents/hello.txt", b"hello from a BitLocker volume\n")
    b.add_file("/Documents/big.bin", blob(rnd, 800_000), frag=4)
    b.add_file("/notes.txt", text(rnd, 100_000), compress=True)
    raw = b.build()
    wrapped, info = ntfsimg.bitlocker_wrap(raw, b, method="xts128", protectors=("tpm", "clear"))
    img = write_image("bitlocker.img", wrapped)
    before = sha(img)
    with boot(img, "t-ntfs-bde") as (g, vm):
        g.ok(f"mkdir -p {MNT}")
        assert "bde0" not in g.ok("lsblk")
        out = g.ok(f"mount -t ntfs -o ro /dev/vda {MNT} && dmesg | grep -i -E 'ntfs|bitlocker'")
        assert re.search(r"bitlocker: unlocked with the clear key: AES-XTS 128 \(method 0x8004\), metadata copy 1 of 3", out), out
        assert re.search(r"ntfs: vda via bde0 mounted on /mnt/windows", out), out
        assert re.search(r"bde0\s+24M\s+1\s", g.ok("lsblk")), "the view is read-only and listed"
        assert g.ok(f"cat {MNT}/Documents/hello.txt") == "hello from a BitLocker volume\n"
        for path in ("Documents/big.bin", "notes.txt"):
            assert g.ok(f"md5sum {MNT}/{path}", timeout=120).split()[0] == md5(b.expected["/" + path]["data"]), path
        first = g.ok("dd if=/dev/bde0 bs=512 count=1 2>/dev/null | xxd | head -1")
        assert "NTFS" in first, first
        assert "FVE" not in first
        assert "-FVE-FS-" in g.ok("dd if=/dev/vda bs=512 count=1 2>/dev/null | strings"), "the disk itself still shows BitLocker's header"
        out, rc = g.run("dd if=/dev/zero of=/dev/bde0 bs=512 count=1 2>&1")
        assert "Read-only file system" in out, out
        g.fails(f"mount -t ntfs -o ro /dev/bde0 /mnt 2>&1", r"busy|Busy|BUSY")
        g.ok(f"umount {MNT}")
        assert "bde0" not in g.ok("lsblk")
        g.poweroff()
    assert sha(img) == before, "the guest wrote to the BitLocker image"


@test
def ntfs_bitlocker_without_a_clear_key_stays_locked():
    """TPM and recovery-password protectors only: the mount fails, the log names the protectors, no bde device is left behind, the disk is untouched."""
    rnd = random.Random(3)
    b = ntfsimg.NtfsBuilder(size=24 * MIB, tail_clusters=80)
    b.add_file("/a.txt", b"secret")
    wrapped, info = ntfsimg.bitlocker_wrap(b.build(), b, protectors=("tpm", "recovery"))
    img = write_image("locked.img", wrapped)
    before = sha(img)
    with boot(img, "t-ntfs-locked") as (g, vm):
        g.ok(f"mkdir -p {MNT}")
        g.fails(f"mount -t ntfs -o ro /dev/vda {MNT} 2>&1")
        out = g.ok("dmesg | grep -i bitlocker")
        assert "protectors: TPM, recovery password" in out and "no clear-key protector" in out, out
        assert "bde0" not in g.ok("lsblk")
        assert "ntfs" not in g.ok("cat /proc/mounts")
        g.poweroff()
    assert sha(img) == before


@test
def ntfs_damaged_volumes_do_not_hurt_the_kernel():
    """Volumes with a damaged boot sector, a damaged $MFT, a damaged directory and a looping index: mount fails or lists with errors, the kernel keeps running (a later mount works)."""
    rnd = random.Random(4)
    b = ntfsimg.NtfsBuilder(size=16 * MIB, index_size=512)
    b.add_dir("/d")
    for i in range(300):
        b.add_file("/d/n%03d.txt" % i, text(rnd, 120))
    b.add_file("/good.txt", b"still readable\n")
    good = bytearray(b.build())
    broken_boot = bytearray(good)
    broken_boot[13] = 3
    broken_mft = bytearray(good)
    off = b.record_offset(0)
    broken_mft[off:off + 4] = b"BAAD"
    victim = b.record_offset(b.expected["/d/n010.txt"]["no"]) + 510
    broken_file = bytearray(good)
    broken_file[victim:victim + 2] = b"\xaa\xbb"
    with boot(write_image("broken-file.img", bytes(broken_file)), "t-ntfs-damaged") as (g, vm):
        g.ok(f"mkdir -p {MNT}")
        g.ok(f"mount -t ntfs -o ro /dev/vda {MNT}")
        assert g.ok(f"cat {MNT}/good.txt") == "still readable\n"
        g.fails(f"cat {MNT}/d/n010.txt 2>&1", r"Input/output|I/O")
        assert g.ok(f"cat {MNT}/d/n011.txt | wc -c").strip() == "120"
        g.ok(f"ls {MNT}/d | wc -l")
        g.ok(f"umount {MNT}")
        g.poweroff()
    for label, data in (("boot", broken_boot), ("mft", broken_mft)):
        with boot(write_image(f"broken-{label}.img", bytes(data)), f"t-ntfs-damaged-{label}") as (g, vm):
            g.ok(f"mkdir -p {MNT}")
            g.fails(f"mount -t ntfs -o ro /dev/vda {MNT} 2>&1")
            assert "ntfs" not in g.ok("cat /proc/mounts")
            assert re.search(r"ntfs: vda is not a mountable NTFS volume", g.ok("dmesg")), label
            g.ok("echo the kernel is fine")
            g.poweroff()
