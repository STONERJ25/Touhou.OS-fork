#!/usr/bin/env python3
"""Host-side tests of the tools for updating from far away: tools/sshvia.py, the --via options of tools/push-update.py and tools/wol.py, and tools/remote-update.py (docs/REMOTE_UPDATE.md).

    python tools/hosttest.py discover -s tests/host -p "test_remote_update.py" -v

No QEMU and no ssh server: the ssh client is replaced by a small script (the "fake ssh") that records how it was called and answers the way a jump host and a machine would (the machine is
"on" when a file in the test's state directory says so, and the wake script that is piped to the jump host switches it on). The real thing (the OpenSSH client through the sshd of a TouhouOS
guest, the push, the restart on trial, the confirmation) is tests/case_update.py, `update_push_through_a_jump_host`.
"""
import importlib.util
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
TOOLS = REPO / "tools"
sys.path.insert(0, str(TOOLS))
import sshvia  # noqa: E402


def load(filename):
    spec = importlib.util.spec_from_file_location(filename.replace("-", "_").replace(".py", ""), TOOLS / filename)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


FAKE_SSH = '''#!%(python)s
import json, os, sys
state = os.environ["FAKE_SSH_STATE"]
args = sys.argv[1:]
data = b"" if sys.stdin.isatty() else sys.stdin.buffer.read()
config = ""
if "-F" in args:
    try:
        config = open(args[args.index("-F") + 1]).read()
    except OSError:
        pass
with open(os.path.join(state, "calls.jsonl"), "a") as f:
    f.write(json.dumps({"argv": args, "stdin": data.decode("utf-8", "replace"), "config": config}) + "\\n")
def path(name):
    return os.path.join(state, name)
def opt(name):
    for i, a in enumerate(args):
        if a == "-o" and i + 1 < len(args) and args[i + 1].startswith(name + "="):
            return args[i + 1].split("=", 1)[1]
    return None
command = args[-1] if args else ""
target = args[-2] if len(args) > 1 else ""
up = os.path.exists(path("up"))
if up and os.path.exists(path("up_host")):
    up = target.endswith("@" + open(path("up_host")).read().strip())
if opt("PreferredAuthentications") == "none":                 # the fetch of the host key: the key goes into the known_hosts file named, the login is refused
    if up:
        kind, blob = open(path("hostkey.pub")).read().split()[:2]
        with open(opt("UserKnownHostsFile"), "a") as f:
            f.write(opt("HostKeyAlias") + " " + kind + " " + blob + "\\n")
    sys.stderr.write("Permission denied (none).\\n")
    sys.exit(255)
proxied = any(a.startswith("ProxyJump=") for a in args)
if not proxied and "-F" in args and command.startswith("python3"):      # a command on the jump host
    if "print(1)" in command:
        print(1)
        sys.exit(0)
    if b"Sends a Wake-on-LAN" in data[:400]:                   # the wake script
        print("wol: sent 2 packets")
        if not os.path.exists(path("stay-off")):
            open(path("up"), "w").close()
        sys.exit(0)
    if b"TOUHOU_ARP_FILE" in data:                              # the discovery script
        print(open(path("discovered")).read().strip() if os.path.exists(path("discovered")) else "")
        sys.exit(0)
    if "s.connect" in command:
        print("192.168.1.5")
        sys.exit(0)
    sys.exit(1)
if command == "sudo poweroff" and up:                          # the machine goes down and the connection with it
    os.remove(path("up"))
    sys.exit(255)
if command == "true":                                          # a login to the machine
    if up:
        sys.exit(0)
    sys.stderr.write("channel 0: open failed: connect failed: No route to host\\n")
    sys.exit(255)
sys.stderr.write("fake ssh: no answer for %%r\\n" %% command)
sys.exit(255)
'''


class Fake:
    """A directory with the fake ssh, its state, a host key and a config for tools/remote-update.py."""

    def __init__(self, tc):
        self.dir = Path(tempfile.mkdtemp(prefix="remote-test-"))
        tc.addCleanup(shutil.rmtree, self.dir, True)
        self.ssh = self.dir / "fakessh"
        self.ssh.write_text(FAKE_SSH % {"python": sys.executable}, encoding="utf-8", newline="\n")
        self.ssh.chmod(self.ssh.stat().st_mode | stat.S_IXUSR)
        (self.dir / "state").mkdir()
        self.state = self.dir / "state"
        subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", str(self.state / "hostkey")], check=True, capture_output=True)
        shown = subprocess.run(["ssh-keygen", "-lf", str(self.state / "hostkey.pub")], capture_output=True, text=True).stdout.split()
        self.fingerprint = shown[1]
        old = {k: os.environ.get(k) for k in ("FAKE_SSH_STATE",)}
        os.environ["FAKE_SSH_STATE"] = str(self.state)
        tc.addCleanup(lambda: [os.environ.pop(k) if v is None else os.environ.__setitem__(k, v) for k, v in old.items()])
        self.cfg = {"via": "me@jump.example:2222", "via_key": str(self.dir / "jumpkey"), "via_options": [], "host": "192.168.1.185", "user": "marisa", "port": 22,
                    "key": str(self.dir / "machinekey"), "ssh_options": [], "mac": "aa:bb:cc:dd:ee:ff", "wake_bind": "192.168.1.5", "broadcast": "192.168.1.255", "wake_seconds": 3,
                    "fingerprint": self.fingerprint, "known_hosts": str(self.dir / "known_hosts"), "host_key_alias": "test-machine", "boot_wait": 5}

    def switch_on(self):
        (self.state / "up").write_text("")

    def calls(self):
        p = self.state / "calls.jsonl"
        return [json.loads(line) for line in p.read_text().splitlines()] if p.exists() else []

    def wakes(self):
        return [c for c in self.calls() if "Sends a Wake-on-LAN" in c["stdin"][:400]]


class ParseViaTests(unittest.TestCase):
    def test_forms(self):
        self.assertEqual(sshvia.parse_via("host"), (None, "host", None))
        self.assertEqual(sshvia.parse_via("me@host"), ("me", "host", None))
        self.assertEqual(sshvia.parse_via("me@host:2222"), ("me", "host", 2222))
        self.assertEqual(sshvia.parse_via("100.99.48.122"), (None, "100.99.48.122", None))
        self.assertEqual(sshvia.parse_via("me@[fe80::1]:22"), ("me", "fe80::1", 22))
        self.assertEqual(sshvia.parse_via("[2001:db8::5]"), (None, "2001:db8::5", None))

    def test_refused(self):
        for bad in ("", "me@", "@host", "host:0", "host:70000", "a b", "me@host:port", "2001:db8::5"):
            with self.assertRaises(ValueError, msg=repr(bad)):
                sshvia.parse_via(bad)


class HopTests(unittest.TestCase):
    def test_config_puts_the_callers_options_first_then_the_key_then_the_defaults(self):
        text = sshvia.Hop("me@jump.example", key="/keys/my key", options=["StrictHostKeyChecking=accept-new", "UserKnownHostsFile=/tmp/kh"]).config_text()
        lines = [ln.strip() for ln in text.splitlines()]
        self.assertEqual(lines[0], "Host jump.example")
        self.assertLess(lines.index("StrictHostKeyChecking accept-new"), lines.index("StrictHostKeyChecking yes"))      # ssh keeps the first value: the caller's wins
        self.assertIn("UserKnownHostsFile /tmp/kh", lines)
        self.assertIn('IdentityFile "/keys/my key"', lines)                          # a path with a space is quoted
        self.assertIn("IdentitiesOnly yes", lines)
        for want in ("BatchMode yes", "ServerAliveInterval 15", "LogLevel ERROR"):
            self.assertIn(want, lines)

    def test_no_key_leaves_it_to_ssh(self):
        text = sshvia.Hop("jump.example").config_text()
        self.assertNotIn("IdentityFile", text)
        self.assertNotIn("IdentitiesOnly", text)

    def test_the_file_exists_while_open_and_is_removed_after(self):
        with sshvia.Hop("me@jump.example:2222") as hop:
            args = hop.ssh_args()
            path = Path(args[1])
            self.assertEqual(args[0], "-F")
            self.assertTrue(path.is_file())
            self.assertIn("Host jump.example", path.read_text())
            self.assertEqual(hop.proxy_jump(), "ProxyJump=me@jump.example:2222")
            self.assertEqual(hop.destination(), ["-p", "2222", "me@jump.example"])
        self.assertFalse(path.exists())

    def test_destination_without_user_and_port(self):
        self.assertEqual(sshvia.Hop("jump.example").destination(), ["jump.example"])


class WolTests(unittest.TestCase):
    def test_the_script_is_piped_to_the_jump_host_with_the_arguments(self):
        f = Fake(self)
        wol = TOOLS / "wol.py"
        r = subprocess.run([sys.executable, str(wol), "aa:bb:cc:dd:ee:ff", "--via", "me@jump.example:2222", "--via-key", f.cfg["via_key"], "--bind", "192.168.1.5",
                            "--broadcast", "192.168.1.255", "--seconds", "3", "--ssh", str(f.ssh)], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("wol: sent 2 packets", r.stdout)
        (call,) = f.calls()
        self.assertEqual(call["argv"][:1], ["-F"])
        self.assertEqual(call["argv"][2:5], ["-p", "2222", "me@jump.example"])
        self.assertEqual(call["argv"][5], "python3 - aa:bb:cc:dd:ee:ff --to 192.168.1.185 --broadcast 192.168.1.255 --seconds 3 --interval 2 --no-limited-broadcast --bind 192.168.1.5")
        self.assertEqual(call["stdin"], wol.read_bytes().decode("utf-8"))
        self.assertIn("IdentityFile", call["config"])

    def test_a_helper_installed_on_the_jump_host_is_asked_instead(self):
        f = Fake(self)
        r = subprocess.run([sys.executable, str(TOOLS / "wol.py"), "aa:bb:cc:dd:ee:ff", "--via", "me@jump.example", "--remote-command", "touhou-wake", "--ssh", str(f.ssh)],
                           capture_output=True, text=True, timeout=60)
        (call,) = f.calls()
        self.assertEqual(call["argv"][-1], "touhou-wake aa:bb:cc:dd:ee:ff")
        self.assertEqual(call["stdin"], "")

    def test_the_piped_copy_runs_on_its_own(self):
        """What the jump host runs is this file on standard input of `python3 -`: no __file__, no tools/sshvia.py next to it."""
        r = subprocess.run([sys.executable, "-", "aa:bb:cc:dd:ee:ff", "--to", "127.0.0.1", "--bind", "127.0.0.1", "--broadcast", "127.0.0.255", "--seconds", "0", "--no-limited-broadcast"],
                           input=(TOOLS / "wol.py").read_bytes(), capture_output=True, timeout=60, cwd=tempfile.gettempdir())
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        self.assertIn(b"102-byte magic packet for aa:bb:cc:dd:ee:ff, from 127.0.0.1 to 127.0.0.255:9, 127.0.0.255:7", r.stdout)

    def test_a_bad_mac_and_a_bad_jump_host_are_refused(self):
        for args in (["not-a-mac"], ["aa:bb:cc:dd:ee:ff", "--via", "a b"]):
            r = subprocess.run([sys.executable, str(TOOLS / "wol.py"), *args], capture_output=True, text=True, timeout=60)
            self.assertEqual(r.returncode, 2, args)


class DiscoverTests(unittest.TestCase):
    def run_script(self, arp_text, mac="aa:bb:cc:dd:ee:ff", prefix="127.0.0."):
        arp = Path(tempfile.mkdtemp(prefix="arp-")) / "arp"
        self.addCleanup(shutil.rmtree, arp.parent, True)
        arp.write_text(arp_text, encoding="utf-8")
        script = load("remote-update.py").DISCOVER_PY
        r = subprocess.run([sys.executable, "-", mac, prefix], input=script.encode(), capture_output=True, timeout=60, env=dict(os.environ, TOUHOU_ARP_FILE=str(arp)))
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        return r.stdout.decode().strip()

    HEAD = "IP address       HW type     Flags       HW address            Mask     Device\n"

    def test_finds_the_address_by_the_mac(self):
        text = self.HEAD + "192.168.1.1  0x1  0x2  7c:9a:54:5c:af:56  *  enp6s0\n10.0.0.77  0x1  0x2  aa:bb:cc:dd:ee:ff  *  enp6s0\n"
        self.assertEqual(self.run_script(text), "192.168.1.77")

    def test_an_incomplete_entry_is_not_an_answer(self):
        text = self.HEAD + "192.168.1.185  0x1  0x0  aa:bb:cc:dd:ee:ff  *  enp6s0\n"
        self.assertEqual(self.run_script(text), "")


class RemoteFlowTests(unittest.TestCase):
    def setUp(self):
        self.mod = load("remote-update.py")
        self.mod.Remote.POLL = 0.05
        self.said = []

    def remote(self, f, **cfg):
        c = dict(f.cfg, **cfg)
        r = self.mod.Remote(c, ssh=str(f.ssh), say=self.said.append)
        self.addCleanup(r.close)
        return r

    def test_a_machine_that_is_on_is_not_woken(self):
        f = Fake(self)
        f.switch_on()
        r = self.remote(f)
        self.assertEqual(r.ensure_up(), 0)
        self.assertEqual(f.wakes(), [])
        self.assertTrue(r.pinned(), "its host key is kept")
        self.assertIn("test-machine ssh-ed25519 ", Path(f.cfg["known_hosts"]).read_text())

    def test_a_machine_that_is_off_is_woken_and_waited_for(self):
        f = Fake(self)
        r = self.remote(f)
        # the machine is off, so the host key cannot be fetched yet; the wake switches the fake on
        self.assertEqual(r.ensure_up(), 0)
        self.assertEqual(len(f.wakes()), 1)
        text = "\n".join(self.said)
        self.assertIn("waking it", text)
        self.assertIn("the machine is on after", text)
        self.assertTrue(r.pinned())

    def test_a_machine_that_never_answers_is_reported_after_a_second_wake_up(self):
        f = Fake(self)
        (f.state / "stay-off").write_text("")
        self.mod.Remote.REWAKE_AFTER = 0.2
        self.addCleanup(setattr, self.mod.Remote, "REWAKE_AFTER", 100)
        r = self.remote(f, boot_wait=1)
        self.assertEqual(r.ensure_up(), 6)
        self.assertEqual(len(f.wakes()), 2)
        self.assertIn("firmware may have started Windows", "\n".join(self.said))

    def test_no_wake_means_no_wake(self):
        f = Fake(self)
        r = self.remote(f)
        self.assertEqual(r.ensure_up(wake=False), 6)
        self.assertEqual(f.wakes(), [])

    def test_the_machine_is_looked_for_by_its_mac_when_its_address_has_changed(self):
        f = Fake(self)
        (f.state / "stay-off").write_text("")
        (f.state / "up").write_text("")
        (f.state / "up_host").write_text("192.168.1.99")
        (f.state / "discovered").write_text("192.168.1.99\n")
        # it is on, but not where the config says: the login fails until the jump host has been asked
        self.mod.Remote.DISCOVER_AFTER = 0.2
        self.addCleanup(setattr, self.mod.Remote, "DISCOVER_AFTER", 120)
        r = self.remote(f, boot_wait=5)
        self.assertEqual(r.ensure_up(), 0)
        self.assertEqual(r.host, "192.168.1.99")
        self.assertIn("is at 192.168.1.99 now", "\n".join(self.said))

    def test_a_host_key_that_is_not_the_pinned_one_is_refused(self):
        f = Fake(self)
        f.switch_on()
        r = self.remote(f, fingerprint="SHA256:" + "A" * 43)
        with self.assertRaises(self.mod.PinRefused):
            r.machine_up()
        self.assertFalse(Path(f.cfg["known_hosts"]).exists(), "nothing was kept")
        logins = [c for c in f.calls() if c["argv"][-1] == "true" and not any(a == "PreferredAuthentications=none" for a in c["argv"])]
        self.assertEqual(logins, [], "no login was attempted with a key that did not match")

    def test_without_a_fingerprint_nothing_is_trusted_unless_asked(self):
        f = Fake(self)
        f.switch_on()
        r = self.remote(f, fingerprint=None)
        status, text = r.pin()
        self.assertEqual(status, "refused")
        self.assertIn(f.fingerprint, text)
        status, text = r.pin(trust_first_use=True)
        self.assertEqual(status, "ok")

    def test_off_powers_the_machine_down_and_notices(self):
        f = Fake(self)
        f.switch_on()
        r = self.remote(f)
        self.assertEqual(r.cmd_off(), 0)
        self.assertIn("the machine is off", "\n".join(self.said))
        self.assertFalse((f.state / "up").exists())
        self.assertTrue(any(c["argv"][-1] == "sudo poweroff" for c in f.calls()))

    def test_off_on_a_machine_that_is_off_does_nothing(self):
        f = Fake(self)
        r = self.remote(f)
        self.assertEqual(r.cmd_off(), 0)
        self.assertFalse(any(c["argv"][-1] == "sudo poweroff" for c in f.calls()))

    def test_a_jump_host_that_does_not_answer_is_reported(self):
        f = Fake(self)
        bad = Path(f.dir / "badssh")
        bad.write_text("#!/bin/sh\necho 'Permission denied (publickey).' >&2\nexit 255\n", encoding="utf-8", newline="\n")
        bad.chmod(bad.stat().st_mode | stat.S_IXUSR)
        r = self.mod.Remote(dict(f.cfg), ssh=str(bad), say=self.said.append)
        self.addCleanup(r.close)
        self.assertEqual(r.ensure_up(), 5)
        self.assertIn("Permission denied (publickey)", "\n".join(self.said))

    def test_push_checks_first_then_wakes_then_pushes(self):
        f = Fake(self)
        r = self.remote(f)
        runs = []
        r.push_update.run = lambda argv: runs.append(list(argv)) or 0           # (the real Ssh class stays: the login checks use it)
        self.assertEqual(r.cmd_push(["--version", "v9"]), 0)
        self.assertEqual(len(runs), 2)
        self.assertEqual(runs[0][-1], "--dry-run")
        self.assertNotIn("--dry-run", runs[1])
        self.assertEqual(len(f.wakes()), 1)
        second = runs[1]
        self.assertEqual(second[:2], ["--host", "192.168.1.185"])
        for want in ("--via", "me@jump.example:2222", "--via-key", f.cfg["via_key"], "--version", "v9", "--ssh", str(f.ssh)):
            self.assertIn(want, second)
        self.assertIn(f"HostKeyAlias=test-machine", second)
        self.assertIn(f"UserKnownHostsFile={Path(f.cfg['known_hosts']).as_posix()}", second)

    def test_a_push_that_is_refused_before_it_starts_does_not_wake_the_machine(self):
        f = Fake(self)
        r = self.remote(f)
        r.push_update.run = lambda argv: 2
        self.assertEqual(r.cmd_push([]), 2)
        self.assertEqual(f.wakes(), [])


class PushViaTests(unittest.TestCase):
    def test_the_options_reach_ssh_and_a_failure_says_which_hop(self):
        f = Fake(self)
        src = f.dir / "esp"
        src.mkdir()
        (src / "kernel.elf").write_bytes(b"\x7fELF" + bytes(100))
        import io
        import tarfile
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w", format=tarfile.USTAR_FORMAT) as t:
            info = tarfile.TarInfo("Hakurei/etc/hello")
            info.size = 3
            t.addfile(info, io.BytesIO(b"hi\n"))
        (src / "initrd.img").write_bytes(buf.getvalue())
        r = subprocess.run([sys.executable, str(TOOLS / "push-update.py"), "--host", "192.168.1.185", "--via", "me@jump.example:2222", "--via-key", f.cfg["via_key"],
                            "--via-option", "UserKnownHostsFile=/tmp/kh", "--ssh", str(f.ssh), "--src", str(src), "--no-key-check", "--version", "t1"],
                           capture_output=True, text=True, timeout=120)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        self.assertIn("through me@jump.example:2222", r.stdout)
        self.assertIn("ssh said: fake ssh: no answer", r.stderr)
        self.assertIn("runs through the jump host me@jump.example:2222", r.stderr)
        call = f.calls()[0]
        argv = call["argv"]
        self.assertIn("ProxyJump=me@jump.example:2222", argv)
        self.assertIn("LogLevel=ERROR", argv)
        self.assertIn("ConnectTimeout=30", argv)
        self.assertTrue(argv[argv.index("-F") + 1].endswith(".conf"))
        self.assertIn("UserKnownHostsFile /tmp/kh", call["config"])
        self.assertIn("IdentityFile", call["config"])
        self.assertIn("StrictHostKeyChecking yes", call["config"])

    def test_without_via_nothing_changes(self):
        f = Fake(self)
        src = f.dir / "esp"
        src.mkdir()
        r = subprocess.run([sys.executable, str(TOOLS / "push-update.py"), "--host", "192.168.1.185", "--ssh", str(f.ssh), "--src", str(src), "--dry-run"], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 2)                                       # no kernel.elf there: refused before any ssh
        self.assertEqual(f.calls(), [])

    def test_a_bad_jump_host_is_a_usage_error(self):
        r = subprocess.run([sys.executable, str(TOOLS / "push-update.py"), "--host", "x", "--via", "a b", "--dry-run"], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 2)
        self.assertIn("not a jump host", r.stderr)


if __name__ == "__main__":
    unittest.main()
