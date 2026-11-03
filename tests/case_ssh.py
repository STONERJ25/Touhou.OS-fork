"""The SSH server (docs/SSH.md), tried with the real OpenSSH client of the host and with hand-made garbage.

Every test boots a text console with a network card whose port 22 is forwarded to a port on the host, puts the public half of a throw-away key pair
into /etc/ssh/authorized_keys/marisa, and connects with `ssh`. They need the OpenSSH client of the host (ssh, ssh-keygen, ssh-keyscan; scp and sftp for ssh_sftp_and_scp) and fail with a message where it is missing.

The ssh_forward_* tests drive port forwarding (forward.c) with the same client: ssh -L, -W, -J and -D, against services of the guest itself (nc, httpd, its own
sshd) and, with sshd.forward=any, against Python servers on the host, which the guest reaches as 10.0.2.2 (QEMU's user-mode network)."""
import hashlib
import os
import re
import shutil
import socket
import socketserver
import struct
import subprocess
import threading
import time

import test_shell
from harness import test, BUILD, scaled
from qemu import free_port
from case_net import Guest, _Server, _UdpServer, _DnsHandler

SSH = shutil.which("ssh")
KEYGEN = shutil.which("ssh-keygen")
WORK = BUILD / "ssh-test"
ANSI = re.compile(chr(27) + r"\[[0-9;?]*[A-Za-z]")


def make_key(name):
    """A key pair without a passphrase in build/ssh-test (made once per name). Returns (private key path, public key line)."""
    assert SSH and KEYGEN, "these tests need the OpenSSH client (ssh, ssh-keygen, ssh-keyscan; scp and sftp for one of them) on the host"
    WORK.mkdir(parents=True, exist_ok=True)
    path = WORK / name
    if not path.exists():
        subprocess.run([KEYGEN, "-q", "-t", "ed25519", "-N", "", "-C", f"test-{name}", "-f", str(path)], check=True)
    return path, (WORK / (name + ".pub")).read_text().strip()


class SshGuest(Guest):
    """A root console with sshd running, port 22 reachable at 127.0.0.1:self.port, and the key "main" authorized for marisa."""

    def __init__(self, name, authorize=("main",), cmdline_extra="", **kw):
        self.port = free_port()
        self.authorize = authorize
        super().__init__(name, net="e1000", root=True, hostfwd=[f"tcp:127.0.0.1:{self.port}-:22"], cmdline=("sshd " + cmdline_extra).strip(), **kw)

    def __enter__(self):
        try:
            super().__enter__()
            self.install_keys("marisa", self.authorize)
            deadline = time.time() + scaled(60)
            while ":22" not in self.out("netstat -l"):
                assert time.time() < deadline, "sshd never started listening:\n" + self.out("dmesg | grep -i sshd")
                time.sleep(1)
        except BaseException:
            self.vm.stop()                                     # __exit__ does not run when __enter__ fails: a guest that never came up must not outlive the test
            raise
        return self

    def install_keys(self, user, names):
        lines = [make_key(n)[1] for n in names]
        self.run("mkdir -p /etc/ssh/authorized_keys")
        self.run("rm -f /etc/ssh/authorized_keys/" + user)
        for line in lines:
            self.run(f"echo '{line}' >> /etc/ssh/authorized_keys/{user}")
        self.run(f"chmod 644 /etc/ssh/authorized_keys/{user}")

    def ssh(self, *cmd, user="marisa", key="main", extra=(), input=None, timeout=90, check=False):
        """Runs the ssh client against the guest. Returns the CompletedProcess (bytes)."""
        known = WORK / "known_hosts"
        known.unlink(missing_ok=True)
        args = [SSH, "-p", str(self.port), "-i", str(make_key(key)[0]), "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes",
                "-o", "StrictHostKeyChecking=no", "-o", f"UserKnownHostsFile={known.as_posix()}", "-o", "ConnectTimeout=30", "-q", *extra,
                f"{user}@127.0.0.1", *cmd]
        return subprocess.run(args, input=input, capture_output=True, timeout=scaled(timeout), check=check)


@test
def ssh_login_and_exec():
    """OpenSSH logs in with its key and runs commands: output, a failing status, stderr kept apart from stdout, and the log names the key"""
    with SshGuest("t-ssh-exec") as g:
        r = g.ssh("echo hello-from-gensokyo; id -un")
        assert r.returncode == 0 and b"hello-from-gensokyo" in r.stdout and b"marisa" in r.stdout, (r.returncode, r.stdout, r.stderr)
        r = g.ssh("exit 3")
        assert r.returncode == 3, (r.returncode, r.stdout, r.stderr)
        r = g.ssh("echo to-out; echo to-err >&2")
        assert r.stdout.strip() == b"to-out" and b"to-err" in r.stderr, (r.stdout, r.stderr)
        log = g.out("dmesg | grep sshd")
        assert re.search(r"accepted publickey for marisa from \S+ port \d+: ED25519 SHA256:", log), log
        assert "host key ssh-ed25519 SHA256:" in log, log
        assert re.search(r"marisa: exec echo \(\d+ bytes; sshd\.logcmds shows them\)", log) and "hello-from-gensokyo" not in log, log      # the first word and the length, not the command


@test
def ssh_refusals():
    """a key that is not listed, an unknown resident, root, a resident without keys, and key files anyone could have written are all turned away; then the right key still works"""
    with SshGuest("t-ssh-refuse", cmdline_extra="sshd.throttle=0") as g:
        def refused(**kw):
            r = g.ssh("echo should-not-run", **kw)
            assert r.returncode == 255 and b"should-not-run" not in r.stdout, (kw, r.returncode, r.stdout, r.stderr)
            return r

        refused(key="other")                                      # a valid key, but nobody listed it
        refused(user="nobody-here")                               # no such resident
        refused(user="reimu")                                     # exists, but has no key file
        g.install_keys("root", ["main"])                          # even listed, root may not come in this way
        refused(user="root")
        g.run("chmod 666 /etc/ssh/authorized_keys/marisa")        # a key file others can write is not to be trusted
        refused()
        g.run("chmod 644 /etc/ssh/authorized_keys/marisa; chown marisa /etc/ssh/authorized_keys/marisa")
        refused()                                                 # nor one owned by the resident themself, in /etc
        g.run("chown root /etc/ssh/authorized_keys/marisa")
        r = g.ssh("echo welcome-back")
        assert r.returncode == 0 and b"welcome-back" in r.stdout, (r.returncode, r.stdout, r.stderr)
        log = g.out("dmesg | grep sshd")
        assert "can be written by others" in log and "is owned by uid" in log, log
        assert re.search(r"failed publickey for nobody-here from", log), log


class Interactive:
    """An ssh client with a terminal (-tt) whose standard input and output are pipes: send() types, expect() waits for output."""

    def __init__(self, g, *args, user="marisa", key="main"):
        import threading
        known = WORK / "known_hosts"
        known.unlink(missing_ok=True)
        cmd = [SSH, "-tt", "-p", str(g.port), "-i", str(make_key(key)[0]), "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes",
               "-o", "StrictHostKeyChecking=no", "-o", f"UserKnownHostsFile={known.as_posix()}", "-q", f"{user}@127.0.0.1", *args]
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.buf = b""
        self.pos = 0
        self.lock = threading.Lock()
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        while True:
            chunk = self.p.stdout.read1(4096)
            if not chunk:
                return
            with self.lock:
                self.buf += chunk

    def text(self):
        with self.lock:
            return ANSI.sub("", self.buf.decode("utf-8", "replace"))

    def send(self, s):
        self.p.stdin.write(s.encode() if isinstance(s, str) else s)
        self.p.stdin.flush()

    def expect(self, pattern, timeout=60):
        deadline = time.time() + scaled(timeout)
        while time.time() < deadline:
            t = self.text()
            m = re.compile(pattern, re.S).search(t, self.pos)
            if m:
                self.pos = m.end()
                return m
            if self.p.poll() is not None and not m:
                t = self.text()
                m = re.compile(pattern, re.S).search(t, self.pos)
                if m:
                    self.pos = m.end()
                    return m
                raise AssertionError(f"ssh ended ({self.p.returncode}) before /{pattern}/ appeared:\n{t[-1500:]}")
            time.sleep(0.1)
        raise TimeoutError(f"/{pattern}/ never appeared:\n{self.text()[-1500:]}")

    def wait(self, timeout=60):
        return self.p.wait(timeout=scaled(timeout))

    def kill(self):
        if self.p.poll() is None:
            self.p.kill()


@test
def ssh_pty_shell():
    """ssh -tt gives a login shell on a pty: the prompt, the environment, Ctrl+C stopping a command, a second concurrent session, and exit ending it"""
    prompt = r"marisa@gensokyo \S+ \$ "

    def run(term, cmd, timeout=60):
        """One command typed at the terminal; what it printed (the markers have a quote in the middle, so the echo of the line does not match them)."""
        term.send(f"echo __BE''GIN__; {cmd}; echo __EN''D__\n")
        out = term.expect(r"\n__BEGIN__\r?\n(.*?)\r?\n?__END__\r?\n", timeout=timeout).group(1)
        term.expect(prompt, timeout=30)
        return out

    with SshGuest("t-ssh-pty") as g:
        a = Interactive(g)
        try:
            a.expect(prompt, timeout=90)
            out = run(a, "echo pty-ok-$USER; tty; echo ssh=$SSH_CONNECTION; echo term=$TERM")
            assert "pty-ok-marisa" in out and "/dev/pts/" in out and re.search(r"ssh=10\.0\.2\.2 \d+ 10\.0\.2\.15 22", out), out
            a.send("sleep 100\n")
            time.sleep(2)
            a.send("\x03")                                         # the pty's line discipline turns this into SIGINT for the foreground job
            a.expect(prompt, timeout=30)
            assert "alive" in run(a, "echo alive")
            b = Interactive(g)                                      # a second login while the first is open
            try:
                b.expect(prompt, timeout=90)
                assert "second-session" in run(b, "echo second-session")
                b.send("exit\n")
                assert b.wait(30) == 0
            finally:
                b.kill()
            assert "first-still-here" in run(a, "echo first-still-here")
            a.send("exit\n")
            assert a.wait(30) == 0, a.text()[-500:]
        finally:
            a.kill()
        deadline = time.time() + scaled(30)
        while re.search(r"gapsh", g.out("ps | grep marisa")):         # no shell of marisa's is left behind
            assert time.time() < deadline, g.out("ps")
            time.sleep(1)


@test
def ssh_bulk_and_rekey():
    """3 MB go up through `cat >` and 3 MB come back down, intact, with the client re-keying every 256 KB; the server's flow control copes with both directions"""
    import hashlib
    data = os.urandom(3 * 1024 * 1024)
    want = hashlib.sha256(data).hexdigest().encode()
    with SshGuest("t-ssh-bulk") as g:
        r = g.ssh("cat > /tmp/up.bin; sha256sum /tmp/up.bin", input=data, extra=("-o", "RekeyLimit=256K"), timeout=300)
        assert r.returncode == 0 and want in r.stdout, (r.returncode, r.stdout, r.stderr)
        r = g.ssh("cat /tmp/up.bin", extra=("-o", "RekeyLimit=256K"), timeout=300)
        assert r.returncode == 0 and r.stdout == data, (r.returncode, len(r.stdout), r.stderr[-300:])
        r = g.ssh("head -c 100000 /dev/zero | wc -c; echo done")
        assert r.returncode == 0 and b"100000" in r.stdout and b"done" in r.stdout, (r.stdout, r.stderr)


class VolumeGuest:
    """A root console on a disk image with a HUMANVILLAGE volume, sshd running, port 22 forwarded; ssh() as in SshGuest."""

    def __init__(self, img, name, authorize=("main",)):
        from case_storage import Guest as StorageGuest
        from harness import Qemu
        self.port = free_port()
        self.authorize = authorize
        self.vm = Qemu(cmdline="text bootlog=off sshd user=root", disk=img.path, net="e1000", hostfwd=[f"tcp:127.0.0.1:{self.port}-:22"], name=name)
        self.g = StorageGuest(self.vm, True)

    def __enter__(self):
        self.vm.start()
        try:
            patterns = [r"root@gensokyo \S+ # ", r"\[Yukari\] eth0: lease"]   # in either order (a quick lease comes before the prompt), as case_net.Guest
            deadline = time.time() + scaled(240)
            while True:
                text = self.vm.text()
                found = [re.search(p, text) for p in patterns]
                if all(found):
                    break
                assert self.vm.alive(), "QEMU exited while booting\n" + self.vm.diagnose()
                assert time.time() < deadline, f"the guest never showed {patterns}\n" + self.vm.diagnose()
                time.sleep(0.2)
            self.vm._pos = max(m.end() for m in found)
            deadline = time.time() + scaled(60)
            while ":22" not in self.g.ok("netstat -l"):
                assert time.time() < deadline, "sshd never started listening"
                time.sleep(1)
        except BaseException:
            self.vm.stop()                                     # (as in SshGuest: no __exit__ after a failed __enter__)
            raise
        return self

    def __exit__(self, *exc):
        self.vm.stop()

    ssh = SshGuest.ssh
    install_keys = SshGuest.install_keys

    def run(self, cmd, timeout=60):
        return self.g.run(cmd, timeout)

    def out(self, cmd, timeout=60):
        return self.g.ok(cmd, timeout)


def host_key_seen(g):
    """The fingerprint of the host key as an SSH client sees it (ssh-keyscan), in the form ssh-keygen -l prints."""
    import base64
    import hashlib
    keyscan = shutil.which("ssh-keyscan")
    r = subprocess.run([keyscan, "-p", str(g.port), "-t", "ed25519", "127.0.0.1"], capture_output=True, timeout=scaled(60))
    m = re.search(rb"ssh-ed25519 (\S+)", r.stdout)
    assert m, (r.stdout, r.stderr)
    blob = base64.b64decode(m.group(1))
    return "SHA256:" + base64.b64encode(hashlib.sha256(blob).digest()).decode().rstrip("=")


@test
def ssh_host_key_and_home_keys_persist():
    """with a HUMANVILLAGE volume the host key is made once and kept (the fingerprint in the log is the one a client sees, and it is the same after a restart), and a key in ~/.ssh/authorized_keys on the volume logs in"""
    from case_storage import Image, hv_part, MIB
    with Image("ssh-hostkey", 64 * MIB, [hv_part()]) as img:
        with VolumeGuest(img, "t-ssh-vol1") as g:
            seen = host_key_seen(g)
            log = g.out("dmesg | grep 'sshd.*host key'")
            assert seen in log and "new, kept on the HUMANVILLAGE volume" in log, (seen, log)
            g.install_keys("marisa", ["other"])                       # /etc lists a different key ...
            g.out("mkdir -p /HumanVillage/marisa/.ssh && chown marisa /HumanVillage/marisa/.ssh")
            pub = make_key("main")[1]
            g.out(f"echo '{pub}' > /HumanVillage/marisa/.ssh/authorized_keys && chown marisa /HumanVillage/marisa/.ssh/authorized_keys")
            print(g.out("ls -l /HumanVillage/marisa/.ssh /etc/ssh/authorized_keys"))
            r = g.ssh("echo from-the-home-key")                         # ... and ~/.ssh/authorized_keys lists "main"
            log = g.out("dmesg | grep sshd | tail -5")
            assert r.returncode == 0 and b"from-the-home-key" in r.stdout, (r.returncode, r.stdout, r.stderr, log)
            g.out("sync")
            g.vm.send("poweroff\n")
            g.vm.wait_exit(40)
        img.check()
        vol = img.volume(1)
        try:
            key = vol.read_file("/.system/ssh_host_ed25519_key")
        finally:
            vol.close()
        assert re.fullmatch(rb"[0-9a-f]{64}\n", key), key

        with VolumeGuest(img, "t-ssh-vol2") as g:
            assert host_key_seen(g) == seen
            assert "read from the HUMANVILLAGE volume" in g.out("dmesg | grep 'sshd.*host key'")
            r = g.ssh("echo still-there")                                # the home key survived the restart too
            assert r.returncode == 0 and b"still-there" in r.stdout, (r.returncode, r.stdout, r.stderr)


# ------------------------------------------------------------------------------------------------ hand-made protocol traffic

def ssh_packet(payload):
    """A binary packet before any keys are in use (RFC 4253 section 6)."""
    import struct
    pad = 8 - ((5 + len(payload)) % 8)
    if pad < 4:
        pad += 8
    body = bytes([pad]) + payload + bytes(pad)
    return struct.pack(">I", len(body)) + body


def sstr(b):
    import struct
    return struct.pack(">I", len(b)) + b


def kexinit_payload(kex, hostkey="ssh-ed25519", cipher="chacha20-poly1305@openssh.com"):
    nl = lambda s: sstr(s.encode())          # noqa: E731
    return (bytes([20]) + bytes(16) + nl(kex) + nl(hostkey) + nl(cipher) * 2 + nl("hmac-sha2-256") * 2 + nl("none") * 2 + nl("") * 2 +
            b"\0" + bytes(4))


class Raw:
    """A bare TCP connection to the guest's sshd that speaks just enough SSH to be rude."""

    def __init__(self, g, banner=True):
        self.s = socket.create_connection(("127.0.0.1", g.port), timeout=scaled(30))
        self.buf = b""
        if banner:
            self.line()

    def line(self):
        while b"\n" not in self.buf:
            chunk = self.s.recv(4096)
            if not chunk:
                break
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\n")
        return line

    def send(self, data):
        self.s.sendall(data)

    def packet(self):
        """The next plain packet's payload, or None when the server closed the connection instead."""
        import struct
        while len(self.buf) < 4:
            chunk = self.s.recv(4096)
            if not chunk:
                return None
            self.buf += chunk
        n = struct.unpack(">I", self.buf[:4])[0]
        while len(self.buf) < 4 + n:
            chunk = self.s.recv(4096)
            if not chunk:
                return None
            self.buf += chunk
        body, self.buf = self.buf[4:4 + n], self.buf[4 + n:]
        return body[1:n - body[0]]

    def closed_within(self, seconds):
        self.s.settimeout(scaled(seconds))
        try:
            while True:
                if not self.s.recv(4096):
                    return True
        except socket.timeout:
            return False
        except OSError:
            return True

    def close(self):
        self.s.close()


def disconnect_reason(payload):
    import struct
    assert payload and payload[0] == 1, payload
    return struct.unpack(">I", payload[1:5])[0]


@test
def ssh_garbage_and_limits():
    """not-SSH, absurd lengths, bad padding, no common algorithm, a strict-KEX violation, bad key-exchange values, and 17 connections at once are all refused without harm; a real login works afterwards and nothing is left running"""
    import struct
    with SshGuest("t-ssh-garbage", cmdline_extra="sshd.throttle=0 sshd.peraddr=0") as g:
        def processes():
            return len(re.findall(r"^.*\bsshd\b.*$", g.out("ps"), re.M))

        base = processes()
        assert base == 1, g.out("ps")

        c = Raw(g)
        c.send(b"GET / HTTP/1.0\r\n\r\n")
        assert b"Protocol mismatch" in c.line() + c.s.recv(200) and c.closed_within(10)
        c.close()

        c = Raw(g)
        c.send(b"SSH-2.0-rude\r\n\xff\xff\xff\xff" + os.urandom(100))
        assert c.closed_within(15)
        c.close()

        c = Raw(g)
        c.send(b"SSH-2.0-rude\r\n" + struct.pack(">I", 12) + bytes(12))   # padding length 0
        assert c.closed_within(15)
        c.close()

        c = Raw(g)
        c.send(b"SSH-2.0-rude\r\n" + ssh_packet(kexinit_payload("diffie-hellman-group14-sha1")))
        assert c.packet()[0] == 20                                        # the server's own KEXINIT
        assert disconnect_reason(c.packet()) == 3                         # KEY_EXCHANGE_FAILED
        c.close()

        c = Raw(g)                                                        # Terrapin: something in front of the KEXINIT under strict key exchange
        c.send(b"SSH-2.0-rude\r\n" + ssh_packet(bytes([2]) + sstr(b"padding")) +
               ssh_packet(kexinit_payload("curve25519-sha256,kex-strict-c-v00@openssh.com")))
        assert c.packet()[0] == 20
        assert disconnect_reason(c.packet()) == 2                         # PROTOCOL_ERROR
        c.close()

        for bad_point, reason in ((b"x" * 31, 2), (bytes(32), 3)):         # a public value of the wrong length, and the all-zero point
            c = Raw(g)
            c.send(b"SSH-2.0-rude\r\n" + ssh_packet(kexinit_payload("curve25519-sha256")) + ssh_packet(bytes([30]) + sstr(bad_point)))
            assert c.packet()[0] == 20
            assert disconnect_reason(c.packet()) == reason, bad_point
            c.close()

        c = Raw(g)                                                        # the same ignore message without the strict marker is allowed
        c.send(b"SSH-2.0-rude\r\n" + ssh_packet(bytes([2]) + sstr(b"padding")) + ssh_packet(kexinit_payload("curve25519-sha256")))
        assert c.packet()[0] == 20
        assert not c.closed_within(3)
        c.close()

        r = g.ssh("echo still-fine")
        assert r.returncode == 0 and b"still-fine" in r.stdout, (r.returncode, r.stdout, r.stderr)

        crowd = [Raw(g) for _ in range(16)]                               # 16 connections hold every place; the 17th is turned away
        extra = Raw(g, banner=False)
        assert extra.closed_within(15)
        extra.close()
        for c in crowd:
            c.close()
        deadline = time.time() + scaled(60)
        while processes() != 1:                                          # the children end when their connections do
            assert time.time() < deadline, g.out("ps")
            time.sleep(1)
        r = g.ssh("echo room-again")
        assert r.returncode == 0 and b"room-again" in r.stdout, (r.returncode, r.stdout, r.stderr)
        log = g.out("dmesg | grep sshd")
        for what in ("protocol version not supported", "no matching algorithm", "strict key exchange", "too many connections"):
            assert what in log, (what, log)


@test
def ssh_silent_client_is_dropped():
    """a client that stops answering (the virtual cable is pulled, as for a laptop that fell asleep) is asked three times and then dropped: its shell is
    hung up and its place is free again; a client that answers is not asked twice"""
    prompt = r"marisa@gensokyo \S+ \$ "
    with SshGuest("t-ssh-alive", cmdline_extra="sshd.keepalive=3") as g:
        a = Interactive(g)
        try:
            a.expect(prompt, timeout=90)
            assert re.search(r"gapsh", g.out("ps | grep marisa")), g.out("ps")
            time.sleep(scaled(8))                                    # idle for longer than two probe intervals: the client answers them
            a.send("echo still-here\n")
            a.expect(r"still-here", timeout=30)
            g.vm.qmp("set_link", name="net0", up=False)             # now nothing gets through any more
            deadline = time.time() + scaled(90)
            while re.search(r"gapsh", g.out("ps | grep marisa")):
                assert time.time() < deadline, g.out("ps") + g.out("dmesg | grep sshd | tail")
                time.sleep(2)
            log = g.out("dmesg | grep sshd")
            assert re.search(r"the client has not answered for 3 probes: connection dropped", log), log
        finally:
            a.kill()
        g.vm.qmp("set_link", name="net0", up=True)
        r = None
        for _ in range(30):                                          # the link comes back, DHCP starts over, ssh works again
            r = g.ssh("echo back-again", timeout=30)
            if r.returncode == 0:
                break
            time.sleep(3)
        assert r.returncode == 0 and b"back-again" in r.stdout, (r.returncode, r.stdout, r.stderr)


@test
def ssh_address_limits():
    """four connections from one address hold its places and the fifth is closed at once; five connections that never log in within a minute stop the address being
    served for a while (one line in the log, not one per attempt); with sshd.logcmds the whole command of an exec request is logged"""
    with SshGuest("t-ssh-limits", cmdline_extra="sshd.logcmds") as g:
        def processes():
            return len(re.findall(r"^.*\bsshd\b.*$", g.out("ps"), re.M))

        crowd = [Raw(g) for _ in range(4)]
        extra = Raw(g, banner=False)
        assert extra.closed_within(15)                                    # the fifth place of this address does not exist
        extra.close()
        for c in crowd:
            c.close()
        deadline = time.time() + scaled(60)
        while processes() != 1:
            assert time.time() < deadline, g.out("ps")
            time.sleep(1)
        r = g.ssh("echo whole-command-visible")                           # four failures so far; a login wipes the slate
        assert r.returncode == 0 and b"whole-command-visible" in r.stdout, (r.returncode, r.stdout, r.stderr)
        for _ in range(5):                                                # five connections that never log in
            c = Raw(g)
            c.close()
            time.sleep(0.5)
        deadline = time.time() + scaled(60)
        while processes() != 1:
            assert time.time() < deadline, g.out("ps")
            time.sleep(1)
        r = g.ssh("echo should-not-run")                                  # the address has used up its minute
        assert r.returncode == 255 and b"should-not-run" not in r.stdout, (r.returncode, r.stdout, r.stderr)
        r = g.ssh("echo should-not-run-either")
        assert r.returncode == 255 and b"should-not-run-either" not in r.stdout, (r.returncode, r.stdout, r.stderr)
        log = g.out("dmesg | grep sshd")
        assert len(re.findall(r"connections in a minute without a login: not served for the next 60 seconds", log)) == 1, log
        assert "exec echo whole-command-visible" in log, log


@test
def ssh_sftp_and_scp():
    """the stock scp (which speaks SFTP) and sftp work: a file each way intact, a directory tree, ls, mkdir, rename, chmod, remove, rmdir, a symlink,
    a read of a file the resident may not read is refused, and the files have the resident's owner"""
    import hashlib
    data = os.urandom(1_500_000)
    want = hashlib.sha256(data).hexdigest().encode()
    tree = WORK / "tree"
    shutil.rmtree(tree, ignore_errors=True)
    (tree / "sub").mkdir(parents=True)
    (tree / "a.txt").write_bytes(b"alpha\n")
    (tree / "sub" / "b.bin").write_bytes(bytes(range(256)) * 20)
    with SshGuest("t-ssh-sftp") as g:
        key = make_key("main")[0]
        known = WORK / "known_hosts"
        common = ["-P", str(g.port), "-i", str(key), "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no",
                  "-o", f"UserKnownHostsFile={known.as_posix()}", "-q"]

        def run(prog, *args, **kw):
            known.unlink(missing_ok=True)
            return subprocess.run([shutil.which(prog), *args], capture_output=True, timeout=scaled(kw.pop("timeout", 240)), **kw)

        up = WORK / "up.bin"
        up.write_bytes(data)
        r = run("scp", *common, str(up), "marisa@127.0.0.1:/tmp/up.bin")
        assert r.returncode == 0, (r.returncode, r.stdout, r.stderr)
        assert want in g.ssh("sha256sum /tmp/up.bin").stdout
        down = WORK / "down.bin"
        down.unlink(missing_ok=True)
        r = run("scp", *common, "marisa@127.0.0.1:/tmp/up.bin", str(down))
        assert r.returncode == 0 and down.read_bytes() == data, (r.returncode, r.stdout, r.stderr)
        r = run("scp", *common, "-r", str(tree), "marisa@127.0.0.1:/tmp/")
        assert r.returncode == 0, (r.returncode, r.stdout, r.stderr)
        r = g.ssh("cat /tmp/tree/a.txt; ls /tmp/tree/sub; wc -c < /tmp/tree/sub/b.bin")
        assert b"alpha" in r.stdout and b"b.bin" in r.stdout and b"5120" in r.stdout, (r.stdout, r.stderr)

        batch = WORK / "batch.txt"
        batch.write_text("\n".join([
            "pwd", "ls /etc", "mkdir /tmp/made", "put " + (tree / "a.txt").as_posix() + " /tmp/made/one.txt", "rename /tmp/made/one.txt /tmp/made/two.txt",
            "chmod 600 /tmp/made/two.txt", "ln -s /tmp/made/two.txt /tmp/made/link", "ls -l /tmp/made", "get /tmp/made/two.txt " + (WORK / "two.txt").as_posix(),
            "rm /tmp/made/link", "rm /tmp/made/two.txt", "rmdir /tmp/made", "get /etc/shadow " + (WORK / "shadow.txt").as_posix(), ""]))
        (WORK / "two.txt").unlink(missing_ok=True)
        (WORK / "shadow.txt").unlink(missing_ok=True)
        r = run("sftp", "-b", str(batch), "-P", str(g.port), "-i", str(key), "-o", "IdentitiesOnly=yes", "-o", "StrictHostKeyChecking=no",
                "-o", f"UserKnownHostsFile={known.as_posix()}", "marisa@127.0.0.1")
        out = (r.stdout + r.stderr).decode("utf-8", "replace")
        assert (WORK / "two.txt").read_bytes() == b"alpha\n", out                              # the file went up, was renamed and came back down
        assert "Remote working directory: /HumanVillage/marisa" in out or "Remote working directory: /" in out, out
        assert "two.txt" in out and "link" in out and "-rw-------" in out, out                 # the long listing shows the new mode
        assert not (WORK / "shadow.txt").exists() and r.returncode != 0, out                   # /etc/shadow is root's: the last command fails, sftp says so
        assert b"No such file" in g.ssh("ls /tmp/made 2>&1").stdout
        owner = g.ssh("ls -l /tmp/up.bin").stdout
        assert b"marisa" in owner, owner


# ------------------------------------------------------------------------------------------------ port forwarding (forward.c)

def client_args(g, known, key="main"):
    """The options of every client below; each has a known_hosts file of its own (the guest's host key is new at every boot, and several clients run at once)."""
    known.unlink(missing_ok=True)
    return ["-p", str(g.port), "-i", str(make_key(key)[0]), "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no",
            "-o", f"UserKnownHostsFile={known.as_posix()}", "-o", "ConnectTimeout=30"]


def stdio_forward(g, target, input=b"", timeout=90):
    """ssh -W TARGET: the client's standard input and output are the forwarded channel. Returns the CompletedProcess (bytes)."""
    return subprocess.run([SSH, *client_args(g, WORK / "known_w"), "-W", target, "marisa@127.0.0.1"], input=input, capture_output=True,
                          timeout=scaled(timeout))


class Tunnel:
    """An ssh client that holds forwards open (-L, -D ... in `specs`) while a test uses them. Its session prints a word once the login is done; the client
    sets up its listeners before it opens the session, so when the word has come they are listening."""
    count = 0

    def __init__(self, g, *specs, extra=()):
        Tunnel.count += 1
        known = WORK / f"known_t{Tunnel.count}"
        self.args = [SSH, *client_args(g, known), "-o", "ExitOnForwardFailure=yes", *extra, *specs, "marisa@127.0.0.1", "echo tunnel-ready; sleep 100000"]
        self.out = self.err = b""
        self.p = None

    def _pump(self, stream, which):
        while True:
            chunk = stream.read1(4096)
            if not chunk:
                return
            setattr(self, which, getattr(self, which) + chunk)

    def __enter__(self):
        self.p = subprocess.Popen(self.args, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        threading.Thread(target=self._pump, args=(self.p.stdout, "out"), daemon=True).start()
        threading.Thread(target=self._pump, args=(self.p.stderr, "err"), daemon=True).start()
        deadline = time.time() + scaled(60)
        while b"tunnel-ready" not in self.out:
            assert self.p.poll() is None, f"the tunnel's ssh ended ({self.p.returncode}): {self.err[-600:]!r}"
            assert time.time() < deadline, f"the tunnel never came up: {self.err[-600:]!r}"
            time.sleep(0.1)
        return self

    def __exit__(self, *exc):
        if self.p.poll() is None:
            self.p.kill()
        self.p.wait(timeout=30)


def serve_any_port(server_cls, handler):
    """case_net.serve(), but the system picks the port: a free TCP port number need not be free (or allowed: Windows reserves ranges) for UDP."""
    srv = server_cls(("127.0.0.1", 0), handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]


class _StreamEcho(socketserver.BaseRequestHandler):
    """Echoes every byte as it comes (case_net's _Echo answers at the end): a forwarded channel busy both ways at once. Counts its connections."""
    connections = 0

    def handle(self):
        type(self).connections += 1
        self.request.settimeout(120)
        try:
            while True:
                chunk = self.request.recv(65536)
                if not chunk:
                    break
                self.request.sendall(chunk)
        except OSError:
            pass


def recv_all(s, limit=None):
    """What the socket gives until its EOF (or until `limit` bytes)."""
    chunks, n = [], 0
    while limit is None or n < limit:
        chunk = s.recv(65536 if limit is None else min(65536, limit - n))
        if not chunk:
            break
        chunks.append(chunk)
        n += len(chunk)
    return b"".join(chunks)


def recv_exact(s, n):
    data = b""
    while len(data) < n:
        chunk = s.recv(n - len(data))
        if not chunk:
            break
        data += chunk
    return data


def socks5(port, host, dport, timeout=60):
    """Sends a SOCKS5 CONNECT through ssh -D (RFC 1928); returns the socket. The answer is socks_reply()'s: OpenSSH says "succeeded" once the server has
    confirmed the channel, and closes the connection without a word when the open fails."""
    s = socket.create_connection(("127.0.0.1", port), timeout=scaled(timeout))
    s.sendall(b"\x05\x01\x00")                                         # version 5, one method: no authentication
    assert recv_exact(s, 2) == b"\x05\x00"
    try:
        addr = b"\x01" + socket.inet_aton(host)
    except OSError:
        addr = b"\x03" + bytes([len(host)]) + host.encode()
    s.sendall(b"\x05\x01\x00" + addr + struct.pack(">H", dport))
    return s


def socks_reply(s):
    """True when the CONNECT succeeded, False when the client closed the connection instead."""
    reply = recv_exact(s, 10)
    assert reply == b"" or reply[:2] == b"\x05\x00", reply
    return reply != b""


def wait_listening(g, port, timeout=30):
    deadline = time.time() + scaled(timeout)
    while not re.search(rf":{port}\b", g.out("netstat -l")):
        assert time.time() < deadline, f"nothing listens on port {port} in the guest:\n" + g.out("netstat -l; ps")
        time.sleep(0.5)


def wait_no_process(g, name, timeout=30):
    deadline = time.time() + scaled(timeout)
    while g.out(f"pidof {name}").strip():
        assert time.time() < deadline, f"{name} is still running:\n" + g.out("ps; netstat -t")
        time.sleep(0.5)


def run_threads(*fns, timeout=300):
    """Runs the functions at the same time and re-raises the first exception one of them raised."""
    errors = []

    def wrap(fn):
        try:
            fn()
        except BaseException as e:      # noqa: BLE001
            errors.append(e)
    threads = [threading.Thread(target=wrap, args=(fn,), daemon=True) for fn in fns]
    for t in threads:
        t.start()
    for t in threads:
        t.join(scaled(timeout))
        assert not t.is_alive(), "a transfer did not finish in time"
    if errors:
        raise errors[0]


def forward_log(g):
    return g.out("dmesg | grep 'sshd.*forward'")


@test
def ssh_forward_local_transfer():
    """ssh -L to services of the guest itself (sshd.forward=local, the default): 5 MB down and 5 MB up through two forwards of one connection at the same
    time, intact, with the client re-keying every megabyte and EOF passed on both ways; a reader that stops for seconds pauses the service, not the data
    (the client checks that its window is never overrun); ssh -W reaches the guest's own sshd (also by its own address), and ssh -J through the guest to
    itself carries 2 MB both ways through `cat`; the log names target, originator, result and the byte counts"""
    size = 5 * 1024 * 1024
    up = os.urandom(size)
    with SshGuest("t-ssh-fwd-xfer") as g:
        out = g.out(f"head -c {size} /dev/urandom > /tmp/down.bin; sha256sum /tmp/down.bin")
        want_down = re.search(r"\b[0-9a-f]{64}\b", out).group(0)
        g.run("nc -l 7000 < /tmp/down.bin &")                                  # sends the file, then its EOF
        g.run("nc -l 7001 < /dev/null > /tmp/up.bin &")                        # sends nothing (its EOF comes at once) and keeps what arrives
        g.run("nc -l 7002 < /tmp/down.bin &")
        for port in (7000, 7001, 7002):
            wait_listening(g, port)
        pd, pu, pp = free_port(), free_port(), free_port()
        got = {}
        with Tunnel(g, "-L", f"127.0.0.1:{pd}:127.0.0.1:7000", "-L", f"127.0.0.1:{pu}:localhost:7001", "-L", f"127.0.0.1:{pp}:127.0.0.1:7002",
                    extra=("-o", "RekeyLimit=1M")):
            def download():
                with socket.create_connection(("127.0.0.1", pd), timeout=scaled(120)) as s:
                    got["down"] = recv_all(s)

            def upload():
                with socket.create_connection(("127.0.0.1", pu), timeout=scaled(120)) as s:
                    s.sendall(up)
                    s.shutdown(socket.SHUT_WR)
                    got["back"] = recv_all(s)                                  # nothing comes back: the EOF of the service's side ends it
            t0 = time.time()
            run_threads(download, upload)
            print(f"     5 MB each way through two forwards in {time.time() - t0:.1f} s")
            assert hashlib.sha256(got["down"]).hexdigest() == want_down, len(got["down"])
            assert got["back"] == b"", got["back"][:100]

            # the reader stops after the first 100 KB for five seconds: the client's window (2 MB) and the buffers fill, sshd stops reading the service
            # (nc blocks in send), and when the reader goes on everything is still there
            with socket.create_connection(("127.0.0.1", pp), timeout=scaled(120)) as s:
                first = recv_all(s, 100 * 1024)
                time.sleep(5)
                paused = first + recv_all(s)
            assert hashlib.sha256(paused).hexdigest() == want_down, len(paused)
            wait_no_process(g, "nc")                                           # each nc ends when its peer has ended
            assert hashlib.sha256(up).hexdigest() in g.out("sha256sum /tmp/up.bin")

        for target in ("127.0.0.1:22", "10.0.2.15:22"):                         # the guest's own sshd, by the loopback and by the machine's own address
            r = stdio_forward(g, target)
            assert r.returncode == 0 and r.stdout.startswith(b"SSH-2.0-TouhouOS"), (target, r.returncode, r.stdout, r.stderr[-400:])

        # ssh -J: the guest is the jump host on the way to ... itself (localhost:22 as the guest sees it). -J passes only -F on to the ssh it starts, so the
        # options are in a config file.
        cfg = WORK / "jump_config"
        common = (f"    User marisa\n    IdentityFile {make_key('main')[0].as_posix()}\n    IdentitiesOnly yes\n    BatchMode yes\n"
                  "    StrictHostKeyChecking no\n    RekeyLimit 1M\n    ConnectTimeout 30\n")
        for k in ("known_j1", "known_j2"):
            (WORK / k).unlink(missing_ok=True)
        cfg.write_text(f"Host fwd-jump\n    HostName 127.0.0.1\n    Port {g.port}\n    UserKnownHostsFile {(WORK / 'known_j1').as_posix()}\n{common}"
                       f"Host fwd-inner\n    HostName localhost\n    Port 22\n    ProxyJump fwd-jump\n    UserKnownHostsFile {(WORK / 'known_j2').as_posix()}\n{common}")
        data = os.urandom(2 * 1024 * 1024)
        r = subprocess.run([SSH, "-F", cfg.as_posix(), "fwd-inner", "cat"], input=data, capture_output=True, timeout=scaled(300))
        assert r.returncode == 0 and r.stdout == data, (r.returncode, len(r.stdout), r.stderr[-600:])

        log = forward_log(g)
        assert re.search(r"marisa: forward to 127\.0\.0\.1:7000 \(for 127\.0\.0\.1:\d+\): open", log), log
        assert re.search(r"marisa: forward to localhost:7001 = 127\.0\.0\.1 \(for 127\.0\.0\.1:\d+\): open", log), log
        assert re.search(rf"forward to 127\.0\.0\.1:7000 \(for [^)]*\): closed[^\n]*\b0 bytes to the target and {size} from it", log), log
        assert re.search(rf"forward to localhost:7001 = 127\.0\.0\.1 \(for [^)]*\): closed[^\n]*\b{size} bytes to the target and 0 from it", log), log
        assert re.search(r"forward to 10\.0\.2\.15:22 \(for [^)]*\): open", log), log
        assert re.search(r"forward to localhost:22 = 127\.0\.0\.1 \(for [^)]*\): open", log), log
        assert "accepted publickey for marisa from 127.0.0.1" in g.out("dmesg | grep sshd"), log      # the inner login of -J


def remote_forward_try(g, *specs, cmd="echo the-session-ran"):
    """ssh -R SPEC... with ExitOnForwardFailure: the client gives up (255) when the server refuses a forward. Returns the CompletedProcess (bytes)."""
    args = [SSH, *client_args(g, WORK / "known_r"), "-o", "ExitOnForwardFailure=yes"]
    for spec in specs:
        args += ["-R", spec]
    return subprocess.run([*args, "marisa@127.0.0.1", cmd], capture_output=True, timeout=scaled(90))


@test
def ssh_forward_policy_local():
    """with sshd.forward=local (the default) only this machine may be reached: an address elsewhere, and a name that the resolver turns into one, are refused as
    administratively prohibited before anything connects there; a port nobody listens on fails as "Connection refused"; a target that never answers (the
    loopback drops every frame) fails at the connect deadline; an unknown name, IPv6 and a non-name fail; ssh -R is off unless asked for; the log says why each time"""
    srv, eport = serve_any_port(_Server, _StreamEcho)
    dns, dport = serve_any_port(_UdpServer, _DnsHandler)
    _StreamEcho.connections = 0
    try:
        with SshGuest("t-ssh-fwd-local", cmdline_extra="sshd.throttle=0") as g:
            def refused(target, why):
                r = stdio_forward(g, target)
                err = r.stderr.decode("utf-8", "replace")
                assert r.returncode == 255 and why in err and not r.stdout, (target, r.returncode, r.stdout, err[-500:])

            refused(f"10.0.2.2:{eport}", "open failed: administratively prohibited: only this machine may be reached (sshd.forward=local)")
            g.run(f"printf 'nameserver 10.0.2.2#{dport}\\n' > /etc/resolv.conf")
            refused(f"kappa.gensokyo:{eport}", "open failed: administratively prohibited: only this machine may be reached")
            assert _StreamEcho.connections == 0, "a refused forward reached its target"
            refused("127.0.0.1:1", "open failed: connect failed: Connection refused")
            refused("nosuch.gensokyo:22", "open failed: connect failed: no such host")
            refused("[::1]:22", "open failed: connect failed: IPv6 is not supported here")
            refused("bad/name:22", "open failed: connect failed: not a host name")
            g.run("echo 'lo_drop 100' > /proc/net/tune")                       # the loopback swallows every frame now: no SYN is ever answered
            t0 = time.time()
            refused("127.0.0.1:22", "open failed: connect failed: the target did not answer in time")
            assert time.time() - t0 > 8, time.time() - t0                       # the deadline (10 s), not an error that came early
            g.run("echo 'lo_drop 0' > /proc/net/tune")
            r = stdio_forward(g, "127.0.0.1:22")
            assert r.returncode == 0 and r.stdout.startswith(b"SSH-2.0-TouhouOS"), (r.returncode, r.stdout, r.stderr[-400:])
            r = remote_forward_try(g, f"9001:127.0.0.1:{eport}")                # ssh -R: off unless sshd.remoteforward=local
            assert r.returncode == 255 and b"remote port forwarding failed for listen port 9001" in r.stderr and b"the-session-ran" not in r.stdout, \
                (r.returncode, r.stdout, r.stderr[-400:])
            log = forward_log(g)
            for what in (r"port forwarding to this machine only \(sshd\.forward=local\), at most 16 channels per connection and 64 in all",
                         r"remote forwarding \(ssh -R\) off \(sshd\.remoteforward=off\)",
                         r"remote forward localhost:9001: refused: remote forwarding is off on this server \(sshd\.remoteforward=off\)",
                         rf"forward to 10\.0\.2\.2:{eport} \(for [^)]*\): refused: only this machine may be reached \(sshd\.forward=local\)",
                         rf"forward to kappa\.gensokyo:{eport} = 10\.0\.2\.2 \(for [^)]*\): refused: only this machine",
                         r"forward to 127\.0\.0\.1:1 \(for [^)]*\): failed: Connection refused",
                         r"forward to nosuch\.gensokyo:22 \(for [^)]*\): failed: no such host",
                         r"forward to 127\.0\.0\.1:22 \(for [^)]*\): failed: the target did not answer in time"):
                assert re.search(what, log), (what, log)
    finally:
        srv.shutdown()
        dns.shutdown()


@test
def ssh_forward_off():
    """with sshd.forward=off every forward is refused as administratively prohibited, the log says so at the start, and logins and commands work as before"""
    with SshGuest("t-ssh-fwd-off", cmdline_extra="sshd.forward=off") as g:
        r = stdio_forward(g, "127.0.0.1:22")
        assert r.returncode == 255 and b"administratively prohibited: port forwarding is off on this server (sshd.forward=off)" in r.stderr, \
            (r.returncode, r.stdout, r.stderr[-400:])
        r = g.ssh("echo still-a-shell")
        assert r.returncode == 0 and b"still-a-shell" in r.stdout, (r.returncode, r.stdout, r.stderr)
        log = g.out("dmesg | grep sshd")
        assert "port forwarding off (sshd.forward=off)" in log and "refused: port forwarding is off on this server" in log, log


@test
def ssh_forward_any_names_socks():
    """with sshd.forward=any: -L to a Python echo server on the host carries 4 MB each way at once through one channel, intact; a target named by a name is looked
    up through the OS's resolver (a Python name server); ssh -D (SOCKS5, by name and by address) works; and a lookup the name server never answers and a connect
    the target never answers wait in the background: meanwhile an echo on another forward of the same connection keeps answering at once"""
    srv, eport = serve_any_port(_Server, _StreamEcho)
    dns, dport = serve_any_port(_UdpServer, _DnsHandler)
    silent = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)                 # a "name server" that takes the questions and never answers
    silent.bind(("127.0.0.1", 0))
    sport = silent.getsockname()[1]
    try:
        with SshGuest("t-ssh-fwd-any", cmdline_extra="sshd.forward=any") as g:
            g.run(f"printf 'nameserver 10.0.2.2#{dport}\\n' > /etc/resolv.conf")
            p1, p2, pdyn = free_port(), free_port(), free_port()
            with Tunnel(g, "-L", f"127.0.0.1:{p1}:10.0.2.2:{eport}", "-L", f"127.0.0.1:{p2}:kappa.gensokyo:{eport}", "-D", f"127.0.0.1:{pdyn}",
                        extra=("-o", "RekeyLimit=1M")):
                data = os.urandom(4 * 1024 * 1024)
                got = []
                with socket.create_connection(("127.0.0.1", p1), timeout=scaled(120)) as s:
                    def send():
                        s.sendall(data)
                        s.shutdown(socket.SHUT_WR)
                    t0 = time.time()
                    run_threads(send, lambda: got.append(recv_all(s)))
                    print(f"     4 MB each way through one channel to the host in {time.time() - t0:.1f} s")
                assert got[0] == data, len(got[0])
                with socket.create_connection(("127.0.0.1", p2), timeout=scaled(60)) as s:
                    s.sendall(b"by name\n")
                    assert recv_exact(s, 8) == b"by name\n"
                for host, port, hello in (("kappa.gensokyo", eport, b"socks by name\n"), ("10.0.2.2", eport, b"socks by address\n"), ("localhost", 22, None)):
                    with socks5(pdyn, host, port) as s:
                        assert socks_reply(s), host
                        if hello:
                            s.sendall(hello)
                            assert recv_exact(s, len(hello)) == hello, host
                        else:
                            assert recv_exact(s, 16) == b"SSH-2.0-TouhouOS", host

                with socket.create_connection(("127.0.0.1", p1), timeout=scaled(60)) as echo:
                    def worst_round_trip(n=6):
                        worst = 0.0
                        for _ in range(n):
                            t = time.time()
                            echo.sendall(b"ping\n")
                            assert recv_exact(echo, 5) == b"ping\n"
                            worst = max(worst, time.time() - t)
                            time.sleep(0.4)
                        return worst

                    worst_round_trip(1)
                    g.run(f"printf 'nameserver 10.0.2.2#{sport}\\noptions timeout:6\\n' > /etc/resolv.conf")
                    t0 = time.time()
                    with socks5(pdyn, "silent.gensokyo", eport) as slow:          # its lookup waits for the silent name server ...
                        worst = worst_round_trip()                                    # ... and the echo does not
                        slow.settimeout(scaled(60))
                        assert not socks_reply(slow)                                  # the lookup gave up: the open failed, the client closed
                    waited = time.time() - t0
                    assert worst < 3 and waited > 4, (worst, waited)                  # (a loop that waited on the lookup would take its 6 s)
                    g.run(f"printf 'nameserver 10.0.2.2#{dport}\\n' > /etc/resolv.conf")

                    g.run("echo 'lo_drop 100' > /proc/net/tune")                     # a connect nobody answers (the echo goes through the NIC, not the loopback)
                    t0 = time.time()
                    with socks5(pdyn, "127.0.0.1", 22) as hang:
                        worst = worst_round_trip()
                        hang.settimeout(scaled(60))
                        assert not socks_reply(hang)
                    waited = time.time() - t0
                    g.run("echo 'lo_drop 0' > /proc/net/tune")
                    assert worst < 3 and waited > 8, (worst, waited)                  # (... and on the connect, its 10 s)
            log = forward_log(g)
            for what in (r"port forwarding to anywhere \(sshd\.forward=any\)",
                         rf"forward to 10\.0\.2\.2:{eport} \(for [^)]*\): open",
                         rf"forward to kappa\.gensokyo:{eport} = 10\.0\.2\.2 \(for [^)]*\): open",
                         rf"forward to silent\.gensokyo:{eport} \(for [^)]*\): failed: no name server answered",
                         r"forward to 127\.0\.0\.1:22 \(for [^)]*\): failed: the target did not answer in time"):
                assert re.search(what, log), (what, log)
    finally:
        srv.shutdown()
        dns.shutdown()
        silent.close()


@test
def ssh_forward_ends():
    """the ends of a forward: the guest's service killed in the middle of a download and of an upload ends the client's connection at once, a client that resets
    its connection in the middle ends the service's, a client's close that comes while its last bytes still wait for a slow service does not lose them, and
    none of it disturbs the next forward of the same ssh connection"""
    with SshGuest("t-ssh-fwd-ends") as g:
        g.out("echo small-file-content > /tmp/small.txt")
        pa, pb, pc, pd, pe = free_port(), free_port(), free_port(), free_port(), free_port()
        with Tunnel(g, "-L", f"127.0.0.1:{pa}:127.0.0.1:7010", "-L", f"127.0.0.1:{pb}:127.0.0.1:7011", "-L", f"127.0.0.1:{pc}:127.0.0.1:7012",
                    "-L", f"127.0.0.1:{pd}:127.0.0.1:7013", "-L", f"127.0.0.1:{pe}:127.0.0.1:7014"):
            def until_the_end(s):
                """Reads until the connection ends (an EOF, or a reset: as good an end); a timeout is a failure. Returns how long it took."""
                t0 = time.time()
                try:
                    recv_all(s)
                except (ConnectionResetError, ConnectionAbortedError):
                    pass
                return time.time() - t0

            # the service goes away in the middle of a download (its standard input is never the terminal: a background job that reads it is stopped)
            g.run("nc -l 7010 < /dev/zero &")
            wait_listening(g, 7010)
            with socket.create_connection(("127.0.0.1", pa), timeout=scaled(30)) as s:
                assert len(recv_all(s, 1 << 20)) == 1 << 20
                g.run("kill $(pidof nc)")
                took = until_the_end(s)
                assert took < scaled(20), took

            # the client resets its end in the middle of a download: the service's connection ends, so nc does
            g.run("nc -l 7011 < /dev/zero > /dev/null &")
            wait_listening(g, 7011)
            s = socket.create_connection(("127.0.0.1", pb), timeout=scaled(60))
            assert len(recv_all(s, 1 << 20)) == 1 << 20
            s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            s.close()
            wait_no_process(g, "nc")

            # the service goes away in the middle of an upload (its input is a pipe that never says a word: it only receives; usleep, because the
            # tunnel's own session is a sleep)
            g.run("usleep 600000000 | nc -l 7012 > /dev/null &")
            wait_listening(g, 7012)
            with socket.create_connection(("127.0.0.1", pc), timeout=scaled(30)) as s:
                s.sendall(os.urandom(1 << 20))
                g.run("kill $(pidof nc)")
                took = until_the_end(s)
                assert took < scaled(20), took
            g.run("kill $(pidof usleep)")

            # The service ends its side at once (so the client's half of the channel is done) and then reads nothing for four seconds. The client sends
            # its bytes and, right behind the last of them, EOF and CLOSE: the bytes that wait in sshd (the socket and the pipe hold about 276 KB, sshd's
            # window 64 KB more) must still get through after the CLOSE. (sh -c: gapsh does not connect a pipe to a ( ... ) subshell.)
            g.run("nc -l 7014 < /dev/null | sh -c 'usleep 4000000; cat > /tmp/slow.bin' &")
            wait_listening(g, 7014)
            slow = os.urandom(310 * 1024)
            with socket.create_connection(("127.0.0.1", pe), timeout=scaled(30)) as s:
                s.sendall(slow)
                s.shutdown(socket.SHUT_WR)
                assert recv_all(s) == b""
            deadline = time.time() + scaled(60)
            while not re.search(r"forward to 127\.0\.0\.1:7014 .*: closed", forward_log(g)) or g.out("pidof nc; pidof cat").strip():
                assert time.time() < deadline, g.out("ps; ls -l /tmp/slow.bin") + forward_log(g)[-800:]
                time.sleep(1)
            assert hashlib.sha256(slow).hexdigest() in g.out("sha256sum /tmp/slow.bin"), g.out("ls -l /tmp/slow.bin") + forward_log(g)[-800:]

            # and the connection carries the next forward as before
            g.run("nc -l 7013 < /tmp/small.txt &")
            wait_listening(g, 7013)
            with socket.create_connection(("127.0.0.1", pd), timeout=scaled(60)) as s:
                s.shutdown(socket.SHUT_WR)
                assert recv_all(s) == b"small-file-content\n"
        wait_no_process(g, "nc")
        log = forward_log(g)
        for port in (7010, 7011, 7012, 7013, 7014):
            assert re.search(rf"forward to 127\.0\.0\.1:{port} \(for [^)]*\): open", log), (port, log)
            assert re.search(rf"forward to 127\.0\.0\.1:{port} \(for [^)]*\): closed", log), (port, log)
        m = re.search(rf"forward to 127\.0\.0\.1:7014 \(for [^)]*\): closed by the client, {len(slow)} bytes to the target \((\d+) after the close\)", log)
        assert m and int(m.group(1)) > 0, log                                  # the path this is about was taken: bytes went on after the CLOSE


@test
def ssh_forward_caps_and_flood():
    """sshd.forwardmax=4 and sshd.forwardtotal=6: a connection gets four channels and the fifth is refused as a resource shortage, a second connection then gets
    only two (the server's six), a place given back can be taken again; a flood of 150 opens that end at once leaves the server serving, every place given
    back, and a log of bounded length that counts what it did not show"""
    with SshGuest("t-ssh-fwd-caps", cmdline_extra="sshd.forwardmax=4 sshd.forwardtotal=6") as g:
        g.run("mkdir -p /tmp/www; echo forwarded-page > /tmp/www/index.html")
        g.run("httpd -q -p 8080 -d /tmp/www &")                                # one process per connection, and it waits for a request
        wait_listening(g, 8080)
        pa, pb = free_port(), free_port()

        def hold(port, n):
            """Opens n connections at once. Returns (those the server took, those it refused): the client closes a refused one at once."""
            socks = [socket.create_connection(("127.0.0.1", port), timeout=scaled(30)) for _ in range(n)]
            time.sleep(scaled(3))                                              # every open has its answer by now
            kept, closed = [], []
            for s in socks:
                s.settimeout(0.3)
                try:
                    (closed if s.recv(1) == b"" else kept).append(s)
                except socket.timeout:
                    kept.append(s)
                except OSError:
                    closed.append(s)
            return kept, closed

        def page(s):
            s.settimeout(scaled(30))
            s.sendall(b"GET / HTTP/1.0\r\n\r\n")
            return recv_all(s)

        def close_all(socks):
            for s in socks:
                s.close()

        with Tunnel(g, "-L", f"127.0.0.1:{pa}:127.0.0.1:8080"), Tunnel(g, "-L", f"127.0.0.1:{pb}:127.0.0.1:8080"):
            a_kept, a_closed = hold(pa, 6)
            assert (len(a_kept), len(a_closed)) == (4, 2), (len(a_kept), len(a_closed))
            b_kept, b_closed = hold(pb, 4)
            assert (len(b_kept), len(b_closed)) == (2, 2), (len(b_kept), len(b_closed))
            for s in a_kept[:2]:
                assert b"forwarded-page" in page(s)                            # a channel that was held works, and its end gives its place back
            close_all(a_kept[:2] + a_closed + b_closed)
            time.sleep(scaled(2))
            b2_kept, b2_closed = hold(pb, 3)                                   # the server has 2 + 2 now: two more for B, and then B's own cap is reached
            assert (len(b2_kept), len(b2_closed)) == (2, 1), (len(b2_kept), len(b2_closed))
            close_all(a_kept[2:] + b_kept + b2_kept + b2_closed)
            time.sleep(scaled(2))

            for _ in range(6):                                                 # the flood: 150 opens on A that end at once
                close_all([socket.create_connection(("127.0.0.1", pa), timeout=scaled(30)) for _ in range(25)])
            r = g.ssh("echo after-the-flood")
            assert r.returncode == 0 and b"after-the-flood" in r.stdout, (r.returncode, r.stdout, r.stderr)
            time.sleep(scaled(5))
            a_kept, a_closed = hold(pa, 5)                                     # every place came back: A's four ...
            assert (len(a_kept), len(a_closed)) == (4, 1), (len(a_kept), len(a_closed))
            b_kept, b_closed = hold(pb, 3)                                     # ... and the server's last two
            assert (len(b_kept), len(b_closed)) == (2, 1), (len(b_kept), len(b_closed))
            for s in a_kept + b_kept:
                assert b"forwarded-page" in page(s)
            close_all(a_kept + a_closed + b_kept + b_closed)
        deadline = time.time() + scaled(30)
        while "without a line each" not in forward_log(g):                     # the connections are over: their summaries come
            assert time.time() < deadline, forward_log(g)[-3000:]
            time.sleep(1)
        log = forward_log(g)
        assert "refused: too many forwarded channels on this connection (sshd.forwardmax=4)" in log, log
        assert "refused: too many forwarded channels on this server (sshd.forwardtotal=6)" in log, log
        assert "further forwards of this connection are counted, not logged one by one" in log, log
        lines = len(re.findall(r"marisa: forward to", log))
        assert lines <= 2 * 2 * 20, lines                                      # two connections, at most 20 channels each with two lines
        summary = re.findall(r"(\d+) more forwards were opened and (\d+) refused without a line each", log)
        assert summary and sum(int(a) + int(b) for a, b in summary) > 100, summary


@test
def ssh_forward_remote():
    """ssh -R with sshd.remoteforward=local: the server listens on its loopback only (a request for every address gets 127.0.0.1; another address, a port below
    1024, a port in use and a fifth listener are refused), a connection made there in the guest reaches a Python echo server on the host through the client
    (1 MB intact both ways), a port the server picks is announced and works, a channel the client refuses (nothing listens on its side) closes the
    connection that came in, ssh -O cancel (through a control master) closes one listener, and the others end with the connection"""
    srv, eport = serve_any_port(_Server, _StreamEcho)
    ctl = f"/tmp/touhou-fwd-ctl-{os.getpid()}"                                   # the control master's socket (a short path: a Unix socket name has 108 bytes)
    try:
        with SshGuest("t-ssh-fwd-remote", cmdline_extra="sshd.remoteforward=local") as g:
            with Tunnel(g, "-R", f"9001:127.0.0.1:{eport}", "-R", f"0.0.0.0:9002:127.0.0.1:{eport}", "-R", f"0:127.0.0.1:{eport}",
                        "-R", "9004:127.0.0.1:1", extra=("-o", "ControlMaster=yes", "-o", f"ControlPath={ctl}")) as t:
                listening = g.out("netstat -l")
                assert re.search(r"127\.0\.0\.1:9001\b", listening) and re.search(r"127\.0\.0\.1:9002\b", listening), listening
                assert not re.search(r"0\.0\.0\.0:900[12]\b", listening), listening    # "every address" got the loopback only

                out, rc = g.run("echo hello-from-the-guest | nc -w 20 127.0.0.1 9001")
                assert "hello-from-the-guest" in out and rc == 0, (out, rc)
                g.run("head -c 1048576 /dev/urandom > /tmp/r.bin")
                out, rc = g.run("nc -w 20 127.0.0.1 9002 < /tmp/r.bin > /tmp/r.out; sha256sum /tmp/r.bin /tmp/r.out", timeout=180)
                sums = re.findall(r"\b([0-9a-f]{64})\b", out)
                assert len(sums) == 2 and sums[0] == sums[1], out                  # through the client to the host's echo server and back, intact

                m = re.search(rb"Allocated port (\d+) for remote forward", t.err)
                assert m, t.err[-600:]
                picked = int(m.group(1))
                out, rc = g.run(f"echo on-the-picked-port | nc -w 20 127.0.0.1 {picked}")
                assert "on-the-picked-port" in out, (picked, out)
                t0 = time.time()                                                    # nothing listens on the client's side: it refuses the channel, the
                out, rc = g.run("echo to-nobody | nc -w 30 127.0.0.1 9004", timeout=120)   # server closes the connection that came in
                assert "to-nobody" not in out and time.time() - t0 < scaled(30), (out, time.time() - t0)
                assert re.search(r"remote forward 127\.0\.0\.1:9004 \(for 127\.0\.0\.1:\d+\): refused by the client \(2\)", forward_log(g)), forward_log(g)[-1500:]

                for spec, why in ((f"10.0.2.15:9003:127.0.0.1:{eport}", "only the loopback may listen"),
                                  (f"80:127.0.0.1:{eport}", "the ports below 1024 are root's"),
                                  (f"9001:127.0.0.1:{eport}", "cannot listen")):                   # the tunnel has it
                    r = remote_forward_try(g, spec)
                    assert r.returncode == 255 and b"remote port forwarding failed for listen port" in r.stderr, (spec, r.returncode, r.stderr[-400:])
                    assert re.search(rf"remote forward [^\n]*: refused: {re.escape(why)}", forward_log(g)), (spec, why, forward_log(g)[-1500:])

                r = subprocess.run([SSH, *client_args(g, WORK / "known_c"), "-o", f"ControlPath={ctl}", "-O", "cancel", "-R", f"9001:127.0.0.1:{eport}",
                                    "marisa@127.0.0.1"], capture_output=True, timeout=scaled(60))       # cancel-tcpip-forward
                assert r.returncode == 0, (r.returncode, r.stderr[-400:])
                deadline = time.time() + scaled(20)
                while re.search(r":9001\b", g.out("netstat -l")):
                    assert time.time() < deadline, g.out("netstat -l")
                    time.sleep(0.5)
                out, rc = g.run("echo nobody-listens | nc -w 5 127.0.0.1 9001; echo rc=$?")
                assert "rc=1" in out and "nobody-listens" not in out.replace("echo nobody-listens", ""), out
                assert re.search(r":9002\b", g.out("netstat -l")), "a cancel closed the wrong listener"

            # five on one connection (the client goes on without the fifth when ExitOnForwardFailure is not set): four listen, the fifth is refused
            args = [SSH, *client_args(g, WORK / "known_r5")]
            for port in range(9011, 9016):
                args += ["-R", f"{port}:127.0.0.1:{eport}"]
            r = subprocess.run([*args, "marisa@127.0.0.1", "netstat -l"], capture_output=True, timeout=scaled(90))
            assert r.returncode == 0, (r.returncode, r.stderr[-400:])
            assert all(re.search(rf":{p}\b".encode(), r.stdout) for p in range(9011, 9015)) and not re.search(rb":9015\b", r.stdout), r.stdout
            assert b"remote port forwarding failed for listen port 9015" in r.stderr, r.stderr[-400:]

            deadline = time.time() + scaled(30)
            while re.search(r":90(0[12]|1[1-4])\b", g.out("netstat -l")):               # the connections are over, and so are their listeners
                assert time.time() < deadline, g.out("netstat -l")
                time.sleep(1)
            log = forward_log(g)
            for what in (r"remote forwarding \(ssh -R\) on the loopback only \(sshd\.remoteforward=local\)",
                         r"remote forward 127\.0\.0\.1:9001 \(asked for localhost:9001\): listening",
                         r"remote forward 127\.0\.0\.1:9002 \(asked for 0\.0\.0\.0:9002\): listening",
                         rf"remote forward 127\.0\.0\.1:{picked} \(asked for localhost:0\): listening",
                         r"remote forward 127\.0\.0\.1:9001 \(for 127\.0\.0\.1:\d+\): open",
                         r"remote forward 127\.0\.0\.1:9002 \(for 127\.0\.0\.1:(\d+)\): closed[^\n]*1048576 bytes to 127\.0\.0\.1:\1 and 1048576 from it",
                         r"remote forward 127\.0\.0\.1:9001 \(asked for localhost:9001\): closed at the client's request",
                         r"remote forward 127\.0\.0\.1:9002: closed with the connection",
                         r"remote forward localhost:9015: refused: too many remote forwards on this connection"):
                assert re.search(what, log), (what, log[-3000:])
    finally:
        srv.shutdown()
