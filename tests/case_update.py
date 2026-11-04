"""Updates pushed over ssh (docs/UPDATE.md): tools/push-update.py, the `update` applet, the loader's trial counter and its fallback to the previous kernel.

Every test copies the installed NVMe image of case_espgrant.py (made by `install`, cached), boots it alone with a network card whose port 22 is
forwarded to the host, and puts the "main" key of case_ssh.py into marisa's ~/.ssh on the volume (so it survives the restarts). The "new
system" is made on the host: the kernel with trailing bytes (an ELF with trailing bytes still loads) and the initrd with one more file,
/etc/update-marker, which says that the new files are the ones running.

  python tests/run_all.py update_
"""
import gzip
import hashlib
import importlib.util
import io
import re
import shutil
import subprocess
import sys
import tarfile
import time
from pathlib import Path

import mkdisk
import test_shell
from harness import test, Qemu, BUILD, scaled
from qemu import free_port
from case_espgrant import installed_base, esp_files, esp_check
from case_install import IMG_DIR
from case_ssh import SSH, WORK, make_key

ROOT = Path(__file__).resolve().parent.parent
PUSH = ROOT / "tools" / "push-update.py"
ROOT_PROMPT = r"root@gensokyo \S+ # "

_spec = importlib.util.spec_from_file_location("push_update", PUSH)
push_update = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(push_update)


def new_system(tag, hang=False, gz=False):
    """A directory with the kernel.elf and initrd.img of a pretend next version. hang: the kernel's first instruction is a jump to itself. gz: the initrd as a gzip
    member, like the build's own (docs/UPDATE.md, "The compressed initrd"); otherwise a plain tar (several tests append bytes to it: only a plain tar stays valid).
    A plain tar leaves out the libraries and the sources of the in-guest build (about 7 MB that nothing needs at run time): the whole archive is 24 MB now, and
    the test ESP (the installer's 64 MiB) must hold the current set, the new one and the copy that is kept ("not enough room on the ESP" otherwise)."""
    out = BUILD / "update-src" / tag
    out.mkdir(parents=True, exist_ok=True)
    kernel = bytearray((BUILD / "esp" / "kernel.elf").read_bytes())
    if hang:
        e_entry = int.from_bytes(kernel[24:32], "little")
        phoff, phentsize, phnum = int.from_bytes(kernel[32:40], "little"), int.from_bytes(kernel[54:56], "little"), int.from_bytes(kernel[56:58], "little")
        for i in range(phnum):
            ph = phoff + i * phentsize
            p_type = int.from_bytes(kernel[ph:ph + 4], "little")
            p_offset, p_vaddr, p_filesz = (int.from_bytes(kernel[ph + o:ph + o + 8], "little") for o in (8, 16, 32))
            if p_type == 1 and p_vaddr <= e_entry < p_vaddr + p_filesz:
                kernel[p_offset + e_entry - p_vaddr:p_offset + e_entry - p_vaddr + 2] = b"\xeb\xfe"       # jmp .
                break
        else:
            raise AssertionError("no segment holds the kernel's entry point")
    kernel += f"NEWKERNEL {tag}\n".encode() * 20000
    src = tarfile.open(BUILD / "esp" / "initrd.img")
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as dst:
        for m in src:
            if not gz and m.name.startswith(("ScarletDevilMansion/lib/", "ScarletDevilMansion/src/")):
                continue
            dst.addfile(m, src.extractfile(m) if m.isfile() else None)
        marker = f"pushed {tag}\n".encode()
        info = tarfile.TarInfo("Hakurei/etc/update-marker")
        info.size, info.mode = len(marker), 0o644
        dst.addfile(info, io.BytesIO(marker))
    (out / "kernel.elf").write_bytes(bytes(kernel))
    (out / "initrd.img").write_bytes(gzip.compress(buf.getvalue(), 6, mtime=0) if gz else buf.getvalue())
    return out


class UpdateGuest:
    """The installed image booted alone; a root console on the serial port; marisa reachable by ssh at 127.0.0.1:self.port."""

    def __init__(self, name, image=None):
        self.name = name
        self.img = Path(image) if image else IMG_DIR / f"{name}.img"
        if not image:
            shutil.copyfile(installed_base(), self.img)
        self.port = free_port()
        self.known = WORK / f"known_hosts_{name}"
        self.known.unlink(missing_ok=True)
        self.vm = Qemu(cmdline="", name=name, mem="768M", boot_image=str(self.img), boot_bus="nvme", net="e1000", no_reboot=False,
                       hostfwd=[f"tcp:127.0.0.1:{self.port}-:22"])

    def __enter__(self):
        self.vm.start()
        try:
            self.wait_both(ROOT_PROMPT, r"\[Yukari\] eth0: lease", timeout=240)
            pub = make_key("main")[1]
            self.run("mkdir -p /HumanVillage/marisa/.ssh && chown marisa /HumanVillage/marisa/.ssh")
            self.run(f"echo '{pub}' > /HumanVillage/marisa/.ssh/authorized_keys && chown marisa /HumanVillage/marisa/.ssh/authorized_keys")
            deadline = time.time() + scaled(60)
            while ":22" not in self.run("netstat -l")[0]:
                assert time.time() < deadline, "sshd never started listening:\n" + self.run("dmesg | grep sshd")[0]
                time.sleep(1)
        except BaseException:
            self.vm.stop()
            raise
        return self

    def __exit__(self, *exc):
        self.vm.stop()

    def wait_both(self, *patterns, timeout):
        """The prompt and the DHCP lease line arrive in either order: wait for both, then move past them."""
        deadline = time.time() + scaled(timeout)
        while True:
            text, start = self.vm.text(), self.vm._pos
            found = [re.compile(p, re.S).search(text, start) for p in patterns]          # (only what came after the last match: a restarted machine shows its prompt again)
            if all(found):
                self.vm._pos = max(m.end() for m in found)
                return
            if not self.vm.alive():
                raise RuntimeError("QEMU exited while booting\n" + self.vm.diagnose())
            if time.time() > deadline:
                raise TimeoutError(f"the guest never showed {patterns}\n" + self.vm.diagnose())
            time.sleep(0.2)

    def run(self, cmd, timeout=90):
        return test_shell.run(self.vm, cmd, timeout=timeout, prompt=ROOT_PROMPT)

    def ssh(self, command, input=None, timeout=300):
        args = [SSH, "-p", str(self.port), "-i", str(make_key("main")[0]), "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes",
                "-o", "StrictHostKeyChecking=accept-new", "-o", f"UserKnownHostsFile={self.known.as_posix()}", "-o", "ConnectTimeout=30",
                "-q", "marisa@127.0.0.1", command]
        return subprocess.run(args, input=input, capture_output=True, timeout=scaled(timeout))

    def push(self, src, version, *extra, wait=300, timeout=900):
        # (--no-key-check: the tool's guard wants the image to authorize the key it logs in with (commit 8c43292); here the key is the test's own and lives on the volume's
        # ~marisa/.ssh, put there by __enter__, not in the initrd)
        cmd = [sys.executable, str(PUSH), "--host", "127.0.0.1", "--port", str(self.port), "--key", str(make_key("main")[0]), "--no-key-check",
               "--ssh-option", f"UserKnownHostsFile={self.known.as_posix()}", "--src", str(src), "--version", version, "--wait", str(int(scaled(wait))), *extra]
        return subprocess.run(cmd, capture_output=True, text=True, timeout=scaled(timeout))

    def loader_lines(self):
        return self.vm.text()


def kernel_bytes_in_log(g):
    return [int(x) for x in re.findall(r"loader: kernel\.(?:elf|old) (\d+) bytes", g.vm.text())]


@test
def update_push_and_confirm():
    """push-update.py sends a new kernel and initrd through ssh; the machine writes them to the ESP, keeps the old pair, restarts on trial with the
    new files, answers ssh again, and `update confirm` ends the trial: update.try is gone, kernel.old and initrd.old hold the old pair. The pushed initrd is a gzip
    member (so is the installed one, the build's own): the machine accepts it, the loader loads it, the kernel inflates it, and /etc/update-marker comes out of it"""
    src = new_system("v2", gz=True)
    assert (src / "initrd.img").read_bytes()[:2] == b"\x1f\x8b"
    with UpdateGuest("update-push") as g:
        old_len = kernel_bytes_in_log(g)[-1]
        out = g.run("update status")[0]
        assert "this start: the installed files" in out and "no push has been installed yet" in out and "update.try: absent" in out, out
        g.run("update status | head -n 1")                         # a closed pipe used to kill the applet between its mount and its umount: the next status said "busy" (the Dell, 2026-11-03)
        out = g.run("update status")[0]
        assert "cannot read the ESP" not in out and "update.try: absent" in out, out
        r = g.push(src, "test-v2")
        assert r.returncode == 0, (r.returncode, r.stdout, r.stderr)
        for want in ("received kernel.elf", "received initrd.img", "all hashes match", "armed update.try", "restarting in", "the machine is back",
                     "this start: the new files, on trial (start 1 of 2)", "confirmed: the new files are the installed system", "push-update: done."):
            assert want in r.stdout, (want, r.stdout)
        log = g.vm.text()
        new_len = (src / "kernel.elf").stat().st_size
        assert re.search(r"loader: the new files are on trial \(update\.try\): start 1 of 2", log), log[-1500:]
        assert kernel_bytes_in_log(g)[-1] == new_len and old_len != new_len, (kernel_bytes_in_log(g), old_len, new_len)
        assert re.search(r"cmdline '[^']*update\.trial=1", log), log[-1500:]
        out = g.run("cat /etc/update-marker; update status; grep -c update.trial /proc/cmdline")[0]
        assert "pushed v2" in out and "installed by the last push: version test-v2" in out and "update.try: absent" in out, out
        assert "previous kernel and initrd kept: yes" in out, out
        g.run("sync")
    files = esp_files(g.img, "kernel.elf", "kernel.old", "initrd.img", "initrd.old", "update.try", "update.mf", "cmdline.txt", "cmdline.old")
    assert files["kernel.elf"] == (src / "kernel.elf").read_bytes() and files["initrd.img"] == (src / "initrd.img").read_bytes()
    assert files["kernel.old"] == (BUILD / "esp" / "kernel.elf").read_bytes() and files["initrd.old"] == (BUILD / "esp" / "initrd.img").read_bytes()
    assert files["update.try"] is None and files["cmdline.old"] is None and files["cmdline.txt"], files.keys()
    assert files["update.mf"].startswith(b"touhou-push 1\nversion test-v2\n"), files["update.mf"]
    rep = esp_check(g.img)
    assert rep.ok and not rep.warnings, "the ESP is not clean after the update:\n" + rep.text(True)


@test
def update_unconfirmed_falls_back():
    """a push that nobody confirms: the watchdog restarts the machine twice (two counted starts), then the loader starts the previous kernel, initrd
    and command line (update.fellback), `update status` says so and `update rollback` makes it permanent"""
    src = new_system("v3")
    with UpdateGuest("update-fallback") as g:
        cmdline = re.search(r"loader: cmdline '([^']*)'", g.vm.text()).group(1)
        assert "update.loader=1" in cmdline, cmdline                                  # the loader says it counts the starts of a trial
        cmdline = re.sub(r"\s*update\.(?:loader|trial|fellback)\S*", "", cmdline)       # (its words are the loader's: a pushed cmdline.txt must not carry them)
        new_cmdline = cmdline + " update.watchdog=20"
        r = g.push(src, "test-v3", "--cmdline", new_cmdline, "--no-confirm", wait=300)
        assert r.returncode == 0 and "not confirming" in r.stdout, (r.returncode, r.stdout, r.stderr)
        assert "this start: the new files, on trial (start 1 of 2)" in r.stdout, r.stdout
        g.vm.expect(r"on trial \(start 1\): the machine restarts in 20 seconds", timeout=60)
        g.vm.expect(r"\[update\] the new files were not confirmed in time: restarting", timeout=120)
        g.vm.expect(r"loader: the new files are on trial \(update\.try\): start 2 of 2", timeout=300)
        g.vm.expect(r"\[update\] the new files were not confirmed in time: restarting", timeout=300)
        g.vm.expect(r"loader: starting the previous kernel and initrd \(kernel\.old, initrd\.old, cmdline\.old\): the new files did not confirm themselves in time",
                    timeout=300)
        g.wait_both(ROOT_PROMPT, r"\[Yukari\] eth0: lease", timeout=300)
        out = g.run("ls /etc/update-marker; grep -c update.fellback /proc/cmdline; update status")[0]
        assert "No such file" in out and "this start: the PREVIOUS kernel and initrd" in out and "update.try: present" in out, out
        assert "update.watchdog" not in g.run("cat /proc/cmdline")[0], "the old command line must be the one in use"
        out, rc = g.run("update rollback")
        assert rc == 0 and "the previous kernel and initrd are the installed ones again" in out, out
        g.run("sync")
        g.vm.send("reboot\n")
        g.vm.expect(r"loader: kernel\.elf \d+ bytes", timeout=120)
        g.vm.expect(ROOT_PROMPT, timeout=300)
        out = g.run("update status")[0]
        assert "this start: the installed files" in out and "update.try: absent" in out, out
        assert "No such file" in g.run("ls /etc/update-marker")[0]
        g.run("sync")
    files = esp_files(g.img, "kernel.elf", "update.try", "cmdline.txt")
    assert files["kernel.elf"] == (BUILD / "esp" / "kernel.elf").read_bytes() and files["update.try"] is None
    assert b"update.watchdog" not in files["cmdline.txt"]
    rep = esp_check(g.img)
    assert rep.ok and not rep.warnings, rep.text(True)


@test
def update_hung_kernel_falls_back():
    """a pushed kernel that hangs in its first instruction: nothing in the machine can restart it, so the reset button does (QMP system_reset); the
    loader counts each start and brings the previous kernel back at the third"""
    src = new_system("v4", hang=True)
    with UpdateGuest("update-hung") as g:
        r = g.push(src, "test-v4", "--no-confirm", wait=40)
        assert r.returncode == 4, (r.returncode, r.stdout, r.stderr)                      # it never came back
        g.vm.expect(r"loader: the new files are on trial \(update\.try\): start 1 of 2", timeout=120)
        time.sleep(scaled(10))
        assert not re.search(r"\[Hakurei\]", g.vm.text()[g.vm.text().rindex("start 1 of 2"):]), "the hung kernel printed something"
        g.vm.qmp("system_reset")
        g.vm.expect(r"loader: the new files are on trial \(update\.try\): start 2 of 2", timeout=120)
        time.sleep(scaled(5))
        g.vm.qmp("system_reset")
        g.vm.expect(r"loader: starting the previous kernel and initrd \(kernel\.old, initrd\.old\): the new files did not confirm themselves in time", timeout=120)
        g.vm.expect(ROOT_PROMPT, timeout=300)
        out = g.run("update status; ls /etc/update-marker")[0]
        assert "this start: the PREVIOUS kernel and initrd" in out and "No such file" in out, out
        g.run("sync")
    assert esp_files(g.img, "kernel.old")["kernel.old"] == (BUILD / "esp" / "kernel.elf").read_bytes()


@test
def update_refusals_and_rules():
    """damaged, incomplete, unsuitable and unwanted pushes change nothing on the ESP; an identical push does nothing; a push on trial blocks the next one
    until `update confirm`; the previous pair kept is always the pair that was installed before"""
    good = new_system("v5")
    k, i = (good / "kernel.elf").read_bytes(), (good / "initrd.img").read_bytes()
    stream = push_update.build_stream([("kernel.elf", k), ("initrd.img", i)], "refuse-1")
    flipped = bytearray(stream)
    flipped[-5000] ^= 1
    junk_kernel = bytes(range(256)) * 40
    junk_initrd = bytes(range(256)) * 8
    badcmd = b"text blk.ro \x01\x02\n"
    gz = gzip.compress(i, 6, mtime=0)                                            # a good compressed initrd, and the ways it can be wrong (the machine inflates it to see: docs/UPDATE.md)
    gz_crc = bytearray(gz)
    gz_crc[-8] ^= 1
    gz_flip = bytearray(gz)
    gz_flip[len(gz) // 2] ^= 0x40

    def with_initrd(data, version):
        return push_update.build_stream([("kernel.elf", k), ("initrd.img", data)], version)
    cases = [
        ("gzip initrd cut short", with_initrd(gz[:-3000], "refuse-g1"), "", "is a gzip stream that ends too early"),
        ("gzip initrd with a wrong CRC-32", with_initrd(bytes(gz_crc), "refuse-g2"), "", "is a damaged gzip stream"),
        ("gzip initrd with a bit flipped", with_initrd(bytes(gz_flip), "refuse-g3"), "", "is a damaged gzip stream"),
        ("gzip initrd followed by bytes", with_initrd(gz + bytes(100), "refuse-g4"), "", "has bytes after the end of its gzip stream"),
        ("gzip initrd that holds no tar", with_initrd(gzip.compress(bytes(range(256)) * 100, 6, mtime=0), "refuse-g5"), "", "does not hold a tar archive"),
        ("damaged in transit", bytes(flipped), "", "arrived damaged"),
        ("cut short", stream[:-1000], "", "the input ended after"),
        ("kernel without initrd", push_update.build_stream([("kernel.elf", k)], "refuse-2"), "", "go together"),
        ("loader without --loader", push_update.build_stream([("kernel.elf", k), ("initrd.img", i), ("EFI/BOOT/BOOTX64.EFI", b"MZ" + bytes(2000))], "refuse-3"),
         "", "only replaced with --loader"),
        ("kernel that is not an ELF file", push_update.build_stream([("kernel.elf", junk_kernel), ("initrd.img", i)], "refuse-4"), "", "is not an x86-64 ELF file"),
        ("initrd that is not an archive", push_update.build_stream([("kernel.elf", k), ("initrd.img", junk_initrd)], "refuse-5"), "", "is not a tar archive and not a gzip stream"),
        ("command line with control characters", push_update.build_stream([("kernel.elf", k), ("initrd.img", i), ("cmdline.txt", badcmd)], "refuse-6"), "",
         "characters that do not belong"),
        ("a file an update may not carry", b"touhou-push 1\nversion refuse-7\nfile etc/passwd 3 " + hashlib.sha256(b"abc").hexdigest().encode() + b"\nend\nabc",
         "", "is not a file an update may carry"),
        ("not a push at all", b"hello\n", "", "the input is not a push"),
    ]
    with UpdateGuest("update-refuse") as g:
        before = esp_stamp_in_guest(g)
        for what, data, flags, want in cases:
            r = g.ssh("sudo update recv" + flags, input=data)
            text = (r.stdout + r.stderr).decode("utf-8", "replace")
            assert r.returncode != 0 and want in text, (what, r.returncode, text)
        assert esp_stamp_in_guest(g) == before, "a refused push changed the ESP"
        r = g.ssh("sudo update recv", input=push_update.build_stream([("kernel.elf", (BUILD / "esp" / "kernel.elf").read_bytes()),
                                                                       ("initrd.img", (BUILD / "esp" / "initrd.img").read_bytes())], "same"))
        assert r.returncode == 0 and b"nothing to do" in r.stdout, (r.returncode, r.stdout, r.stderr)
        assert esp_stamp_in_guest(g) == before
        r = g.ssh("sudo update recv --no-such-option", input=stream)                   # an unknown option is refused before the input is read
        assert r.returncode == 2, (r.returncode, r.stdout, r.stderr)
        r = g.ssh("sudo update recv", input=stream)                                    # a real push, no restart: on trial from now on
        assert r.returncode == 0 and b"update: done" in r.stdout and b"armed update.try" in r.stdout, (r.returncode, r.stdout, r.stderr)
        r = g.ssh("sudo update recv", input=push_update.build_stream([("kernel.elf", k + b"more"), ("initrd.img", i)], "refuse-8"))
        assert r.returncode != 0 and b"still on trial" in r.stderr, (r.returncode, r.stdout, r.stderr)
        out = g.run("update status")[0]
        assert "update.try: present" in out and "version refuse-1" in out and "previous kernel and initrd kept: yes, complete" in out, out
        out, rc = g.run("update confirm")                                              # nothing has proved the files: this start is not the trial start
        assert rc != 0 and "not the trial start" in out, out
        assert "update.try: present" in g.run("update status")[0]
        r = g.ssh("sudo update recv --no-trial", input=push_update.build_stream([("kernel.elf", k + b"more"), ("initrd.img", i)], "refuse-8b"))
        assert r.returncode != 0 and b"still on trial" in r.stderr, (r.returncode, r.stdout, r.stderr)        # a push that is not on trial must not end the trial of an earlier one
        r = g.ssh("sudo update recv --replace-trial", input=push_update.build_stream([("kernel.elf", k + b"more"), ("initrd.img", i)], "refuse-8"))
        assert r.returncode == 0 and b"replacing the update that is on trial" in r.stdout and b"update: done" in r.stdout, (r.returncode, r.stdout, r.stderr)
        out, rc = g.run("update confirm --anyway")
        assert rc == 0 and "confirmed" in out, out
        v3 = push_update.build_stream([("kernel.elf", k + b"third"), ("initrd.img", i + bytes(1024))], "refuse-9")
        r = g.ssh("sudo update recv --no-trial", input=v3)
        assert r.returncode == 0 and b"update: done" in r.stdout and b"(not on trial)" in r.stdout and b"update: result: installed" in r.stdout, (r.returncode, r.stdout, r.stderr)
        out = g.run("update status")[0]
        assert "version refuse-9" in out and "update.try: absent" in out, out
        g.run("sync")
    files = esp_files(g.img, "kernel.elf", "kernel.old", "initrd.img", "initrd.old", "update.try")
    assert files["kernel.elf"] == k + b"third" and files["kernel.old"] == k + b"more", "the kept pair must be the one installed before the last push"
    assert files["initrd.img"] == i + bytes(1024) and files["initrd.old"] == i and files["update.try"] is None
    rep = esp_check(g.img)
    assert rep.ok and not rep.warnings, rep.text(True)


def esp_stamp_in_guest(g):
    """A fingerprint of the files on the ESP, read by the guest through a read-only mount."""
    out = g.run("mkdir -p /tmp/e && mount -r /dev/nvme0n1p1 /tmp/e && (cd /tmp/e && sha256sum kernel.elf initrd.img cmdline.txt EFI/BOOT/BOOTX64.EFI; ls) ; umount /tmp/e")[0]
    return out


@test
def update_confirm_works_on_a_dirty_esp():
    """a power cut during an update leaves the ESP flagged dirty and the trial armed; the new files still start (the loader does not care), `update recv`
    says how to go on (--force), and `update confirm` still ends the trial: a system that works must be able to say so"""
    src = new_system("v6")
    with UpdateGuest("update-dirty") as g:
        r = g.ssh("sudo update recv", input=push_update.build_stream([("kernel.elf", (src / "kernel.elf").read_bytes()),
                                                                       ("initrd.img", (src / "initrd.img").read_bytes())], "dirty-1"))
        assert r.returncode == 0 and b"armed update.try" in r.stdout, (r.returncode, r.stdout, r.stderr)
        g.run("sync")
        img = g.img
    mkdisk.set_dirty(str(img), 1)                                    # the cut: the file system was not closed
    assert any("dirty" in w for w in mkdisk.check_image(str(img), only=1).warnings)
    with UpdateGuest("update-dirty", image=img) as g:
        assert re.search(r"loader: the new files are on trial \(update\.try\): start 1 of 2", g.vm.text()), g.vm.text()[-1500:]
        assert "pushed v6" in g.run("cat /etc/update-marker")[0]
        r = g.ssh("sudo update recv", input=push_update.build_stream([("kernel.elf", (src / "kernel.elf").read_bytes() + b"x"),
                                                                       ("initrd.img", (src / "initrd.img").read_bytes())], "dirty-2"))
        assert r.returncode != 0 and b"not cleanly unmounted" in r.stderr and b"update recv --force" in r.stderr, (r.returncode, r.stdout, r.stderr)
        out, rc = g.run("update confirm")
        assert rc == 0 and "confirmed" in out, out
        out = g.run("update status")[0]
        assert "update.try: absent" in out and "version dirty-1" in out, out
        g.run("sync")
    assert esp_files(img, "update.try")["update.try"] is None


def pair_stream(src, version):
    return push_update.build_stream([("kernel.elf", (src / "kernel.elf").read_bytes()), ("initrd.img", (src / "initrd.img").read_bytes())], version)


def wait_for_sshd(g):
    deadline = time.time() + scaled(120)
    while ":22" not in g.run("netstat -l")[0]:
        assert time.time() < deadline, "sshd never started listening:\n" + g.run("dmesg | grep sshd")[0]
        time.sleep(1)


@test
def update_a_push_is_not_proven_by_the_start_that_received_it():
    """the start that received a push does not run it: `update status` must not call the files on the ESP "on trial" and `update confirm` must refuse, also when that
    start was itself a trial start (its command line says update.trial=1 for the files it began with); after a restart the same files can be confirmed
    (2026-10-25: a script confirmed files that had never run, on the start that had only received them)"""
    first, second = new_system("v7"), new_system("v8")
    with UpdateGuest("update-written") as g:
        r = g.ssh("sudo update recv", input=pair_stream(first, "written-1"))
        assert r.returncode == 0 and b"update: result: installed" in r.stdout, (r.returncode, r.stdout, r.stderr)
        out = g.run("update status")[0]                                                  # (a plain start that has received the push)
        assert "this start: not the files that are on the ESP now" in out and "this start: the new files, on trial" not in out and "update.try: present" in out, out
        out, rc = g.run("update confirm")
        assert rc != 0 and "not the trial start" in out, out
        g.run("sync")
        g.vm.send("reboot\n")
        g.vm.expect(r"loader: the new files are on trial \(update\.try\): start 1 of 2", timeout=240)
        g.wait_both(ROOT_PROMPT, r"\[Yukari\] eth0: lease", timeout=300)
        assert "pushed v7" in g.run("cat /etc/update-marker")[0]
        out = g.run("update status")[0]                                                  # the trial start of the first push: its files are the ones running
        assert "this start: the new files, on trial (start 1 of 2)" in out and "version written-1" in out, out
        out, rc = g.run("update confirm")
        assert rc == 0 and "confirmed" in out, out
        wait_for_sshd(g)
        r = g.ssh("sudo update recv", input=pair_stream(second, "written-2"))            # a second push, in the start that has just been confirmed
        assert r.returncode == 0 and b"update: result: installed" in r.stdout, (r.returncode, r.stdout, r.stderr)
        out = g.run("update status")[0]
        assert "this start: not the files that are on the ESP now" in out and "this start: the new files, on trial" not in out, out
        assert "installed by the last push: version written-2" in out and "update.try: present" in out, out
        out, rc = g.run("update confirm")                                                # update.trial=1 is still on its command line: it must not be believed
        assert rc != 0 and "written to the ESP after this start began" in out, out
        assert "update.try: present" in g.run("update status")[0]
        g.run("sync")
        g.vm.send("reboot\n")
        g.vm.expect(r"loader: the new files are on trial \(update\.try\): start 1 of 2", timeout=240)    # (a new trial counts from 1 again)
        g.wait_both(ROOT_PROMPT, r"\[Yukari\] eth0: lease", timeout=300)
        assert "pushed v8" in g.run("cat /etc/update-marker")[0]
        out = g.run("update status")[0]
        assert "this start: the new files, on trial (start 1 of 2)" in out and "version written-2" in out, out
        out, rc = g.run("update confirm")
        assert rc == 0 and "confirmed" in out, out
        g.run("sync")
    files = esp_files(g.img, "kernel.elf", "kernel.old", "update.try")
    assert files["kernel.elf"] == (second / "kernel.elf").read_bytes() and files["kernel.old"] == (first / "kernel.elf").read_bytes() and files["update.try"] is None
    rep = esp_check(g.img)
    assert rep.ok and not rep.warnings, rep.text(True)


@test
def update_restart_is_proved_by_a_token_in_tmp():
    """push-update.py takes a machine for restarted only when a login finds the token it left in /tmp gone (RAM: empty at every start). The state of port 22 proves nothing
    (a sshd that was turning its address away looked like a restart, and the old start was confirmed, 2026-10-25) and neither does the uptime (a virtual machine on a busy
    host lags behind real time: the first version of this check took a start that had not restarted for one that had)"""
    look = push_update.look_at_token
    assert look(0, b"abc123\nEND\n", "abc123") == "same"
    assert look(0, b"cat: /tmp/.push-update-token: No such file\nEND\n", "abc123") == "new"
    assert look(255, b"", "abc123") is None and look(None, b"", "abc123") is None          # no answer
    assert look(0, b"", "abc123") is None                                                  # a login that ran nothing proves nothing
