"""Updates the machine pulls by itself (docs/UPDATE.md, "Pulling from a server"): `update pull URL`, the signed manifest of tools/serve-update.py, the anti-rollback
number, the push's own ESP sequence and trial, and the self-confirmation of a pulled trial start.

The guest is the installed NVMe image of case_update.py. The server is tools/serve-update.py's own, run in this process on 127.0.0.1 (the guest reaches it at
10.0.2.2 through QEMU's user network) with a throwaway signing key made here; the real key (~/.touhou-os) is never read. The installed image has no key: the
tests put the throwaway public key into the running system's /etc/update/pubkey (root may, as root may write the ESP anyway) and into the new initrd.

  python tools/qemu-slot.py -- python tests/run_all.py update_pull_
"""
import importlib.util
import io
import re
import tarfile
import threading
import time
from pathlib import Path

from harness import test, BUILD, scaled
from qemu import free_port
from case_espgrant import esp_files, esp_check
from case_update import UpdateGuest, new_system, esp_stamp_in_guest, kernel_bytes_in_log, ROOT_PROMPT

ROOT = Path(__file__).resolve().parent.parent
_spec = importlib.util.spec_from_file_location("serve_update", ROOT / "tools" / "serve-update.py")
su = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(su)
from cryptography.hazmat.primitives.asymmetric import ec          # noqa: E402  (serve-update.py needs it as well)

KEY = ec.generate_private_key(ec.SECP256R1())                     # throwaway, for this run only
OTHER = ec.generate_private_key(ec.SECP256R1())
PUB_HEX = su.public_point(KEY).hex()


def pull_system(tag, hang=False):
    """case_update's pretend next version, with this run's public key in its initrd (the new system must verify the manifest again to confirm itself)."""
    src = new_system(tag, hang=hang)
    rd = src / "initrd.img"
    with tarfile.open(rd, "a", format=tarfile.USTAR_FORMAT) as t:
        data = (PUB_HEX + "\n").encode()
        info = tarfile.TarInfo("Hakurei/etc/update/pubkey")
        info.size, info.mode = len(data), 0o644
        t.addfile(info, io.BytesIO(data))
    return src


class Server:
    """serve-update.py's server on 127.0.0.1:PORT in a thread. raw=True skips make_server()'s own check, to serve files that are not the manifest's."""

    def __init__(self, manifest, files, raw=False, damaged=None):
        self.srv = su.UpdateServer("127.0.0.1", 0, manifest, files, quiet=True) if raw else su.make_server("127.0.0.1", 0, manifest, files, quiet=True)
        if damaged is not None:
            self.srv.state["manifest"] = damaged                    # (the server itself would not take a manifest that does not parse)
        self.url = f"http://10.0.2.2:{self.srv.server_address[1]}"
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.srv.shutdown()
        self.srv.server_close()


def files_of(src, cmdline=None):
    files = {"kernel.elf": (src / "kernel.elf").read_bytes(), "initrd.img": (src / "initrd.img").read_bytes()}
    if cmdline:
        files["cmdline.txt"] = (cmdline.strip() + "\n").encode()
    return files


def plain_cmdline(g):
    cl = re.search(r"loader: cmdline '([^']*)'", g.vm.text()).group(1)
    return re.sub(r"\s*update\.(?:loader|trial|fellback)\S*", "", cl).strip()


def put_key(g):
    out, rc = g.run(f"mkdir -p /etc/update && echo {PUB_HEX} > /etc/update/pubkey && echo key-ok")
    assert "key-ok" in out, out


@test
def update_pull_whole_way():
    """`update pull URL` fetches and verifies a signed update, installs it on trial and restarts; the new start fetches the manifest again through its own
    network stack, verifies it, and confirms itself (update.autoconfirm); `update status` says so; the ESP holds the new pair, the old one as .old and update.iss;
    the same manifest again is "nothing new", and an older one is refused (rollback protection)"""
    src = pull_system("pull1")
    with UpdateGuest("update-pull") as g:
        old_len = kernel_bytes_in_log(g)[-1]
        out, rc = g.run("update pull http://10.0.2.2:1/ --no-reboot")
        assert rc != 0 and "no signing key in this image" in out, out
        put_key(g)
        issued = int(time.time())
        cmdline = plain_cmdline(g) + " update.autoconfirm=15 update.pull.delay=3600 update.pull=http://10.0.2.2:{port}"
        port = free_port()
        files = files_of(src, cmdline.format(port=port))
        manifest = su.build_manifest(files, KEY, "pull-v1", issued)
        srv = su.make_server("127.0.0.1", port, manifest, files, quiet=True)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        try:
            g.vm.send(f"update pull http://10.0.2.2:{port}/\n")
            for want in (r"signed by this image's key", r"fetched cmdline\.txt", r"fetched initrd\.img \(\d+ bytes, hash ok\)", r"fetched kernel\.elf",   # (the manifest's order)
                         r"armed update\.try", r"restarting in"):
                g.vm.expect(want, timeout=300)
            g.vm.expect(r"loader: the new files are on trial \(update\.try\): start 1 of 2", timeout=240)
            g.vm.expect(r"self-confirmation: the new files fetched and verified the signed manifest again", timeout=400)
            g.vm.expect(r"confirmed: the new files are the installed system", timeout=60)
            # (the prompt and the lease came before the confirmation, which needs the network: nothing more to wait for)
            assert kernel_bytes_in_log(g)[-1] == len(files["kernel.elf"]) != old_len
            out = g.run("cat /etc/update-marker; update status")[0]
            assert "pushed pull1" in out and "version pull-v1" in out and "update.try: absent" in out and "previous kernel and initrd kept: yes" in out, out
            out, rc = g.run(f"update pull http://10.0.2.2:{port}/ --no-reboot")
            assert rc == 0 and "nothing new" in out and "result: unchanged" in out, out
            before = esp_stamp_in_guest(g)
            older = su.build_manifest(files_of(new_system("pull0")), KEY, "pull-v0", issued - 100)
            with Server(older, files_of(new_system("pull0"))) as s:
                out, rc = g.run(f"update pull {s.url} --no-reboot")
            assert rc == 2 and "rollback protection" in out, out
            assert esp_stamp_in_guest(g) == before
            g.run("sync")
        finally:
            srv.shutdown()
            srv.server_close()
    f = esp_files(g.img, "kernel.elf", "kernel.old", "initrd.img", "update.try", "update.iss", "update.mf")
    assert f["kernel.elf"] == files["kernel.elf"] and f["initrd.img"] == files["initrd.img"] and f["kernel.old"] == (BUILD / "esp" / "kernel.elf").read_bytes()
    assert f["update.try"] is None and f["update.iss"] == f"issued {issued}\ndone\n".encode(), f["update.iss"]
    assert b"pulled http://10.0.2.2:" in f["update.mf"], f["update.mf"]
    rep = esp_check(g.img)
    assert rep.ok and not rep.warnings, rep.text(True)


@test
def update_pull_refusals():
    """a wrong signature, another key, a file whose hash does not match, a file larger than listed, a damaged manifest, a path that is not one of the four,
    an unreachable server: each is refused with its reason and the ESP is unchanged; a manifest of the installed files is "nothing to do", and with
    --power-off-after the machine then switches itself off"""
    src = pull_system("pullr")
    files = files_of(src)
    good = su.build_manifest(files, KEY, "pull-r", int(time.time()))
    body, _ = su.split_manifest(good)
    sig_line = good[len(body):]
    flipped = body + sig_line[:20] + (b"0" if sig_line[20:21] != b"0" else b"1") + sig_line[21:]
    k = files["kernel.elf"]
    bad_hash = dict(files, **{"kernel.elf": k[:-1] + bytes([k[-1] ^ 1])})
    bigger = dict(files, **{"kernel.elf": k + b"more"})
    cases = [
        ("a wrong signature", flipped, files, False, 2, "the signature does not match"),
        ("another key", su.build_manifest(files, OTHER, "pull-r", int(time.time())), files, False, 2, "signed by key"),
        ("a file whose hash does not match", good, bad_hash, True, 2, "arrived damaged"),
        ("a file larger than listed", good, bigger, True, 2, "more than the"),
        ("a damaged manifest", good.replace(b"\nend\n", b"\r\nend\n"), files, "damaged", 2, "the manifest is refused"),
        ("a path that is not one of the four", su.build_manifest({"etc/passwd": b"x"}, KEY, "pull-r", int(time.time())), {"etc/passwd": b"x"}, False, 2,
         "is not a file an update may carry"),
    ]
    with UpdateGuest("update-pull-refuse") as g:
        put_key(g)
        before = esp_stamp_in_guest(g)
        for what, manifest, served, raw, code, want in cases:
            with (Server(good, served, damaged=manifest) if raw == "damaged" else Server(manifest, served, raw=raw)) as s:
                out, rc = g.run(f"update pull {s.url} --no-reboot", timeout=300)
            assert rc == code and want in out and "nothing was changed" in out, (what, rc, out)
        out, rc = g.run(f"update pull http://10.0.2.2:{free_port()}/ --no-reboot", timeout=200)
        assert rc == 1 and "nothing was changed" in out, ("unreachable", rc, out)
        assert esp_stamp_in_guest(g) == before, "a refused pull changed the ESP"
        same = {"kernel.elf": (BUILD / "esp" / "kernel.elf").read_bytes(), "initrd.img": (BUILD / "esp" / "initrd.img").read_bytes()}
        with Server(su.build_manifest(same, KEY, "pull-same", int(time.time())), same) as s:
            g.vm.send(f"update pull {s.url} --power-off-after\n")
            g.vm.expect(r"everything in the manifest is installed already: nothing to do", timeout=300)
            g.vm.expect(r"switching off", timeout=60)
            assert g.vm.wait_exit(120) is not None, "the machine did not power off"


# ---------------------------------------------------------------------------------------------------- the automatic mode, through the command-line words

AUTO_WORDS = " update.autoconfirm=10 update.pull.delay=5 update.pull.interval=0 update.pull.then=poweroff update.pull=http://10.0.2.2:{port}"


class Again(UpdateGuest):
    """The same disk image started once more, with nobody at the console: the tests only read what the machine says by itself."""

    def __init__(self, g, name):
        super().__init__(name, image=g.img)

    def __enter__(self):
        self.vm.start()
        return self


def serve(port, manifest, files):
    srv = su.make_server("127.0.0.1", port, manifest, files, quiet=True)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def stop(srv):
    srv.shutdown()
    srv.server_close()


def off(g, timeout=180):
    assert g.vm.wait_exit(scaled(timeout)) is not None, "the machine did not switch itself off"


def first_auto_system(g, tag, port, issued):
    """Brings the guest to an installed, confirmed system whose command line carries the words of the automatic mode: a pull by hand of a build with that
    cmdline.txt, whose trial start then does everything by itself (the self-confirmation after 10 s, then the power-off). Returns (server, cmdline, files)."""
    put_key(g)
    cmdline = plain_cmdline(g) + AUTO_WORDS.format(port=port)
    files = files_of(pull_system(tag), cmdline)
    srv = serve(port, su.build_manifest(files, KEY, tag, issued), files)
    try:
        g.vm.send(f"update pull http://10.0.2.2:{port}/\n")
        for want in (r"restarting in", r"loader: the new files are on trial \(update\.try\): start 1 of 2",
                     r"self-confirmation: the new files fetched and verified the signed manifest again", r"confirmed: the new files are the installed system",
                     r"switching off \(update\.pull\.then=poweroff: the update is confirmed\)"):
            g.vm.expect(want, timeout=400)
        off(g)
    except BaseException:
        stop(srv)
        raise
    return srv, cmdline, files


@test
def update_pull_auto_words():
    """update.pull=URL update.pull.delay=5 update.pull.interval=0 update.pull.then=poweroff on the command line, nobody at the keyboard: a start with a newer
    manifest on the server pulls it, restarts on trial, confirms itself and switches off; the next start finds nothing new and switches off after the check"""
    port, issued = free_port(), int(time.time())
    with UpdateGuest("update-pull-auto") as g:
        srv, cmdline, files_a = first_auto_system(g, "autoA", port, issued)
    try:
        stop(srv)
        files_b = files_of(pull_system("autoBB"), cmdline)
        assert len(files_b["kernel.elf"]) != len(files_a["kernel.elf"])
        srv = serve(port, su.build_manifest(files_b, KEY, "autoBB", issued + 1), files_b)
        with Again(g, "update-pull-auto-2") as g2:
            for want in (r"signed by this image's key \w+: version autoBB", r"restarting in", r"loader: the new files are on trial \(update\.try\): start 1 of 2",
                         r"self-confirmation: the new files fetched and verified the signed manifest again",
                         r"switching off \(update\.pull\.then=poweroff: the update is confirmed\)"):
                g2.vm.expect(want, timeout=400)
            off(g2)
            assert kernel_bytes_in_log(g2)[-1] == len(files_b["kernel.elf"])
        with Again(g, "update-pull-auto-3") as g3:
            g3.vm.expect(rf"issued {issued + 1} is the last pull installed here: nothing new", timeout=400)
            g3.vm.expect(r"switching off \(update\.pull\.then=poweroff: nothing was installed\)", timeout=60)
            off(g3)
            assert "restarting in" not in g3.vm.text()
    finally:
        stop(srv)
    f = esp_files(g.img, "kernel.elf", "initrd.img", "update.try", "update.iss")
    assert f["kernel.elf"] == files_b["kernel.elf"] and f["initrd.img"] == files_b["initrd.img"] and f["update.try"] is None
    assert f["update.iss"] == f"issued {issued + 1}\ndone\n".encode(), f["update.iss"]
    rep = esp_check(g.img)
    assert rep.ok and not rep.warnings, rep.text(True)


@test
def update_pull_hung_kernel_falls_back_and_is_not_pulled_again():
    """the automatic mode pulls a kernel that hangs in its first instruction: two counted starts (the reset button: QMP system_reset), then the loader starts
    the previous files, and that start does NOT pull (it would fetch the same manifest and loop); `update pull` by hand is refused there too. After
    `update rollback` the next start finds the manifest of the failed build "nothing new" (its number is the last one installed), and an older manifest is
    refused (rollback protection survives the fallback and the rollback)"""
    port, issued = free_port(), int(time.time())
    with UpdateGuest("update-pull-hung") as g:
        srv, cmdline, files_a = first_auto_system(g, "hungA", port, issued)
    try:
        stop(srv)
        files_c = files_of(pull_system("hungCC", hang=True), cmdline)
        srv = serve(port, su.build_manifest(files_c, KEY, "hungCC", issued + 2), files_c)
        with Again(g, "update-pull-hung-2") as g2:
            g2.vm.expect(r"restarting in", timeout=400)
            g2.vm.expect(r"loader: the new files are on trial \(update\.try\): start 1 of 2", timeout=240)
            time.sleep(scaled(10))
            g2.vm.qmp("system_reset")
            g2.vm.expect(r"loader: the new files are on trial \(update\.try\): start 2 of 2", timeout=120)
            time.sleep(scaled(5))
            g2.vm.qmp("system_reset")
            g2.vm.expect(r"loader: starting the previous kernel and initrd", timeout=120)
            mark = len(g2.vm.text())
            g2.wait_both(ROOT_PROMPT, r"update\.pull: this start fell back to the previous files: not pulling", timeout=300)
            time.sleep(scaled(20))                                   # (the delay of the automatic pull is 5 s: it would have begun by now)
            after = g2.vm.text()[mark:]
            assert g2.vm.alive() and "fetching" not in after and "switching off" not in after, after[-2000:]
            out, rc = g2.run(f"update pull http://10.0.2.2:{port}/ --no-reboot")
            assert rc == 2 and "this start runs the previous files" in out, out
            out, rc = g2.run("update rollback")
            assert rc == 0, out
            g2.run("sync")
            g2.vm.send("reboot\n")
            g2.vm.expect(rf"issued {issued + 2} is the last pull installed here: nothing new", timeout=400)
            g2.vm.expect(r"switching off \(update\.pull\.then=poweroff: nothing was installed\)", timeout=60)
            off(g2)
        stop(srv)
        older = dict(files_a, **{"kernel.elf": files_a["kernel.elf"] + b"older"})
        srv = serve(port, su.build_manifest(older, KEY, "older", issued + 1), older)
        with Again(g, "update-pull-hung-3") as g3:
            g3.vm.expect(rf"issued at {issued + 1}, before the last pull installed here \({issued + 2}\).*rollback protection", timeout=400)
            g3.vm.expect(r"switching off", timeout=60)
            off(g3)
    finally:
        stop(srv)
    f = esp_files(g.img, "kernel.elf", "update.try", "update.iss")
    assert f["kernel.elf"] == files_a["kernel.elf"] and f["update.try"] is None and f["update.iss"] == f"issued {issued + 2}\ndone\n".encode(), f["update.iss"]


@test
def update_pull_number_survives_a_power_cut():
    """the plug is pulled in the middle of a pulled install (QMP quit right after the anti-rollback number was recorded, while the files are being written):
    update.iss on the disk already holds the manifest's number, so an older manifest is refused afterwards whatever state the files are in; and the same
    manifest is not "nothing new" (the number is there without "done"): the files are compared and the install is done again, to its end"""
    src = pull_system("cut1")
    files = files_of(src)
    issued = int(time.time())
    manifest = su.build_manifest(files, KEY, "cut-v1", issued)
    with UpdateGuest("update-pull-cut") as g:
        put_key(g)
        with Server(manifest, files) as s:
            g.vm.send(f"update pull {s.url} --no-reboot\n")
            g.vm.expect(rf"update: recorded issued {issued} in update\.iss", timeout=400)
            seen = g.vm.text()
            try:
                g.vm.qmp("quit")                                    # the plug: no sync, no unmount
            except Exception:                                       # noqa: BLE001 -- QEMU may close the socket before it answers
                pass
            g.vm.proc.wait(timeout=scaled(30))
    f = esp_files(g.img, "update.iss", "update.try", "kernel.elf")
    assert f["update.iss"] in (f"issued {issued}\n".encode(), f"issued {issued}\ndone\n".encode()), f["update.iss"]
    if "armed update.try" not in seen:
        # the number is on the disk and nothing else of the update is: recording it must not arm the trial (the old files would be "on trial" with no set to fall back to)
        assert f["update.try"] is None and f["kernel.elf"] == (BUILD / "esp" / "kernel.elf").read_bytes(), "the write of update.iss changed more than update.iss"
    with UpdateGuest("update-pull-cut-2", image=g.img) as g2:
        put_key(g2)
        old = files_of(new_system("cut0"))
        with Server(su.build_manifest(old, KEY, "cut-v0", issued - 100), old) as s:
            out, rc = g2.run(f"update pull {s.url} --no-reboot", timeout=300)
        assert rc == 2 and "rollback protection" in out, out
        out = g2.run("update status")[0]
        assert "update.try: absent" in out or "armed update.try" in seen, out
        if "update.try: present" in out:                             # (the cut came late: the trial was armed; this start is its trial start or a mix: end it by hand)
            g2.run("update confirm --anyway")
        with Server(manifest, files) as s:
            out, rc = g2.run(f"update pull {s.url} --no-reboot", timeout=400)
        # (the cut left the ESP flagged dirty; update.iss without "done" explains the dirt as this machine's own interrupted pull, so the install mounts with
        # force and goes through: docs/UPDATE_PULL_REVIEW.md, finding 12)
        assert "was begun here and not finished" in out and "check it first" not in out, out      # (the kernel's own line says "FORCED read-write although ...")
        assert rc == 0 and ("result: installed" in out or "result: unchanged" in out) and "nothing new" not in out, out
        g2.run("sync")
    f = esp_files(g.img, "kernel.elf", "initrd.img", "update.iss")
    assert f["kernel.elf"] == files["kernel.elf"] and f["initrd.img"] == files["initrd.img"], "the manifest whose install was cut was not installed afterwards"
    assert f["update.iss"] == f"issued {issued}\ndone\n".encode(), f["update.iss"]
