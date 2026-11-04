"""The initrd in its two forms (docs/UPDATE.md, "The compressed initrd"): kernel/fs/initrd.c unpacks a ustar archive that is either plain or one gzip member.

  python tools/qemu-slot.py -- python tests/run_all.py initrd_

  initrd_default_image_boots     the image tools/build.py made (gzip unless TOUHOU_INITRD=plain): boots to the shell, the log line says which form it was, and every file
                                 of the archive is in the guest, byte for byte (md5 of each, taken in the guest)
  initrd_plain_boots             the same archive as a plain tar: unchanged behaviour
  initrd_damaged_is_refused      a gzip stream with a bit flipped, cut short, with a wrong CRC or length, with bytes after the end, a gzip of something that is not a tar,
                                 and a file that is neither: one clear line in the panic, no shell, the machine restarts (panic=1) and nothing like a triple fault is logged
  initrd_damaged_on_trial_falls_back
                                 the same damage in an update on trial: both starts panic over the initrd, restart by themselves, and the loader brings the previous set back
  initrd_bomb_is_refused         a gzip stream of a tar with a 300 MiB file in 300 KB: stopped at the 256 MiB ceiling, with the panic line saying so

The archive is the build's own (build/initrd.tar, or the unpacked build/esp/initrd.img); each case boots a private copy of the ESP with the initrd it needs.
"""
import gzip
import hashlib
import io
import os
import re
import shutil
import struct
import tarfile
import time
import zlib

import test_shell
from harness import test, Qemu, BUILD

PROMPT = test_shell.PROMPT


def build_tar():
    """The plain archive of the current build."""
    p = BUILD / "initrd.tar"
    if p.is_file():
        return p.read_bytes()
    d = (BUILD / "esp" / "initrd.img").read_bytes()
    return gzip.decompress(d) if d[:2] == b"\x1f\x8b" else d


def private_esp(name, initrd):
    """build/<name>-esp: the loader and the kernel of this build, and this initrd."""
    esp = BUILD / f"{name}-esp"
    (esp / "EFI" / "BOOT").mkdir(parents=True, exist_ok=True)
    shutil.copyfile(BUILD / "esp" / "EFI" / "BOOT" / "BOOTX64.EFI", esp / "EFI" / "BOOT" / "BOOTX64.EFI")
    shutil.copyfile(BUILD / "esp" / "kernel.elf", esp / "kernel.elf")
    (esp / "initrd.img").write_bytes(initrd)
    return esp


def archive_md5s(tar):
    """{path in the guest: md5} for the regular files of the archive."""
    out = {}
    with tarfile.open(fileobj=io.BytesIO(tar)) as t:
        for m in t:
            if m.isfile() and m.mode & 0o004:                  # (the shell the test types into is reimu's: a file only root may read, like /etc/shadow, is not hashed)
                out["/" + m.name] = hashlib.md5(t.extractfile(m).read()).hexdigest()
    return out


def check_guest_files(vm, tar):
    """Every regular file of the archive is in the guest with the same md5. (The guest writes some files itself while it starts; those are excluded by name, and the list
    of them is short enough to be looked at: it is printed when a file differs.)"""
    want = archive_md5s(tar)
    roots = sorted({"/" + p.split("/")[1] for p in want})
    out, rc = test_shell.run(vm, f"find {' '.join(roots)} -type f | sort | xargs -n 25 md5sum 2>&1", timeout=900)
    got = {}
    for line in out.splitlines():
        m = re.fullmatch(r"([0-9a-f]{32})\s+(\S.*)", line.strip())
        if m:
            got[m[2]] = m[1]
    missing = sorted(set(want) - set(got))
    differ = sorted(p for p in want if p in got and got[p] != want[p])
    assert not missing, f"{len(missing)} of {len(want)} files of the archive are missing in the guest, e.g. {missing[:5]}"
    assert not differ, f"{len(differ)} of {len(want)} files differ from the archive, e.g. {differ[:5]}"
    return len(want)


def initrd_line(vm):
    m = vm.expect(r"\[Keine\] (initrd\.img (?:was gzip-compressed|is a plain tar archive)[^\n]*)\n", timeout=90)
    return m.group(1)


@test
def initrd_default_image_boots():
    """the image tools/build.py made boots, says which form it is (gzip unless TOUHOU_INITRD=plain), and every file of the archive arrives intact"""
    img = (BUILD / "esp" / "initrd.img").read_bytes()
    is_gzip = img[:2] == b"\x1f\x8b"
    tar = gzip.decompress(img) if is_gzip else img
    if os.environ.get("TOUHOU_INITRD", "gzip").strip().lower() in ("", "gzip"):
        assert is_gzip, "tools/build.py should write build/esp/initrd.img compressed unless TOUHOU_INITRD=plain is set"
    with Qemu(cmdline="text", name="t-initrd-default") as vm:
        line = initrd_line(vm)
        vm.expect(PROMPT, timeout=90)
        if is_gzip:
            m = re.fullmatch(r"initrd\.img was gzip-compressed: (\d+) KiB -> (\d+) KiB \((\d+)%\), unpacked in (\d+) ms: the inflate with its CRC-32 (\d+) ms, the files (\d+) ms", line)
            assert m, line
            assert int(m[1]) == len(img) >> 10 and int(m[2]) == len(tar) >> 10, line
        else:
            assert re.fullmatch(r"initrd\.img is a plain tar archive \(\d+ KiB\), unpacked in \d+ ms", line), line
        n = check_guest_files(vm, tar)
        assert n > 100
        print(f"\n   {line}", end="")


@test
def initrd_plain_boots():
    """the archive as a plain tar, as before the compression existed: boots, unchanged, every file intact"""
    tar = build_tar()
    esp = private_esp("t-initrd-plain", tar)
    with Qemu(cmdline="text", name="t-initrd-plain", esp=esp) as vm:
        line = initrd_line(vm)
        vm.expect(PROMPT, timeout=90)
        assert re.fullmatch(r"initrd\.img is a plain tar archive \(\d+ KiB\), unpacked in \d+ ms", line), line
        check_guest_files(vm, tar)
        print(f"\n   {line}", end="")


def damaged_variants(tar):
    gz = gzip.compress(tar, 9, mtime=0)
    n = len(gz)
    flipped = bytearray(gz)
    flipped[n // 2] ^= 0x10
    crc = bytearray(gz)
    crc[-8] ^= 1
    size = bytearray(gz)
    size[-1] ^= 1
    # a stream that is a good gzip member but holds something that is not an archive
    junk_inside = gzip.compress(os.urandom(200000), 6, mtime=0)
    return [
        ("a bit flipped in the middle", bytes(flipped), r"initrd\.img is damaged: .*gzip stream of \d+ bytes"),
        ("cut in the middle", gz[: n // 2], r"initrd\.img is damaged: the stream ends too early"),
        ("cut before the trailer", gz[:-8], r"initrd\.img is damaged: the stream ends too early"),
        ("wrong CRC-32 in the trailer", bytes(crc), r"initrd\.img is damaged: CRC-32 mismatch"),
        ("wrong length in the trailer", bytes(size), r"initrd\.img is damaged: length mismatch"),
        ("bytes after the end", gz + bytes(16), r"initrd\.img is damaged: bytes after the end of the stream"),
        ("a second gzip member", gz + gzip.compress(b"x", 6, mtime=0), r"initrd\.img is damaged: bytes after the end of the stream"),
        ("a gzip of something that is not a tar", junk_inside, r"initrd\.img is not a tar archive"),
        ("neither gzip nor tar", b"\x01" + os.urandom(4999), r"initrd\.img is not a tar archive"),
    ]


@test
def initrd_damaged_is_refused():
    """a damaged compressed initrd gives one clear panic line, never a shell, and the machine restarts by itself (panic=1) without anything like a triple fault"""
    tar = build_tar()
    for what, initrd, why in damaged_variants(tar):
        name = "t-initrd-bad"
        esp = private_esp(name, initrd)
        with Qemu(cmdline="text panic=1", name=name, esp=esp) as vm:
            m = vm.expect(r"\*\*\* INCIDENT: KERNEL PANIC \*\*\*\n([^\n]*)\n", timeout=90)
            reason = m.group(1)
            assert re.search(why, reason), f"{what}: the panic said {reason!r}, expected /{why}/"
            assert "boot complete" not in vm.text(), f"{what}: the system came up although the initrd is damaged"
            rc = vm.wait_exit(60)                           # panic=1: the machine resets itself after a second; the harness's -no-reboot ends QEMU there
            assert rc is not None, f"{what}: the machine neither restarted nor stopped\n" + vm.diagnose()
            log = vm.log_path.read_text(errors="replace")
            assert "riple fault" not in log, f"{what}: QEMU logged a triple fault:\n{log[-1500:]}"
            assert "restarting now" in vm.text(), f"{what}: no restart was announced\n{vm.text()[-800:]}"


@test
def initrd_damaged_on_trial_falls_back():
    """an update on trial whose initrd is a damaged gzip stream (one that did not go through `update recv`'s check: a cut in the ESP, say): every start of it panics over the
    initrd with the clear line, restarts by itself 30 s later, the loader counts both starts, and the third is the previous kernel and initrd, which report the panic"""
    from case_reliability import private_esp as trial_esp, trial_files, root, ROOT
    good = (BUILD / "esp" / "initrd.img").read_bytes()
    gz = good if good[:2] == b"\x1f\x8b" else gzip.compress(good, 6, mtime=0)
    bad = bytearray(gz)
    bad[len(bad) // 3] ^= 0x20
    esp = trial_esp("t-initrd-trial-esp", extra=trial_files("text user=root"))
    (esp / "initrd.img").write_bytes(bytes(bad))
    with Qemu(cmdline="text user=root", name="t-initrd-trial", esp=esp, no_reboot=False) as vm:
        vm.expect(r"loader: the new files are on trial \(update\.try\): start 1 of 2", timeout=120)
        vm.expect(r"\*\*\* INCIDENT: KERNEL PANIC \*\*\*\ninitrd\.img is damaged: ", timeout=60)
        vm.expect(r"the machine restarts in 30 s \(panic=30: the default on a start under trial\)", timeout=30)
        vm.expect(r"loader: the new files are on trial \(update\.try\): start 2 of 2", timeout=150)
        vm.expect(r"\*\*\* INCIDENT: KERNEL PANIC \*\*\*\ninitrd\.img is damaged: ", timeout=60)
        vm.expect(r"loader: starting the previous kernel and initrd \(kernel\.old, initrd\.old, cmdline\.old\): the new files did not confirm themselves in time", timeout=150)
        vm.expect(r"\[Akyuu\] previous panic: initrd\.img is damaged: ", timeout=90)
        vm.expect(ROOT, timeout=120)
        out, _ = root(vm, "cat /proc/cmdline")
        assert "update.fellback" in out, out


def tar_with_giant_file(megabytes):
    """A ustar archive holding one file of `megabytes` MiB of zeros, as a gzip member; made in a stream so that the host never holds the zeros."""
    info = tarfile.TarInfo("big/zeros")
    info.size = megabytes << 20
    header = info.tobuf(format=tarfile.USTAR_FORMAT)
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    raw = [c.compress(header)]
    crc = zlib.crc32(header)
    chunk = bytes(1 << 20)
    for _ in range(megabytes):
        raw.append(c.compress(chunk))
        crc = zlib.crc32(chunk, crc)
    tail = bytes(1024)                                      # the end-of-archive blocks
    raw.append(c.compress(tail))
    crc = zlib.crc32(tail, crc)
    raw.append(c.flush())
    total = len(header) + (megabytes << 20) + len(tail)
    return b"\x1f\x8b\x08\x00" + bytes(4) + b"\x02\xff" + b"".join(raw) + struct.pack("<II", crc, total & 0xFFFFFFFF)


@test
def initrd_bomb_is_refused():
    """a gzip stream of a few hundred kilobytes that unpacks to 300 MiB is stopped at the kernel's 256 MiB ceiling, with a panic line that says so"""
    gz = tar_with_giant_file(300)
    assert len(gz) < 1 << 20, len(gz)
    esp = private_esp("t-initrd-bomb", gz)
    t0 = time.time()
    with Qemu(cmdline="text panic=1", name="t-initrd-bomb", esp=esp) as vm:
        m = vm.expect(r"\*\*\* INCIDENT: KERNEL PANIC \*\*\*\n([^\n]*)\n", timeout=240)
        assert "more output than allowed" in m.group(1), m.group(1)
        assert vm.wait_exit(60) is not None, vm.diagnose()
    print(f"\n   stopped after {time.time() - t0:.0f} s with: {m.group(1)}", end="")
