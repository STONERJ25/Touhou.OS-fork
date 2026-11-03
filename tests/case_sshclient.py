"""The SSH client (docs/SSH_CLIENT.md): ssh, ssh-keygen, ssh-keyscan, scp and sftp running INSIDE the guest.

Two servers are used. The guest's own sshd (docs/SSH.md), reached over the loopback at 127.0.0.1 (the guest is started with `sshd`, and `sshd.throttle=0` because
several tests make a connection that ends without a login on purpose), and a Python server on the host (tests/sshsrv.py, written separately from both the client
and the sshd), reached through QEMU's user-mode network at 10.0.2.2: that one has passwords, keyboard-interactive, banners, forced re-keys and the other things the
sshd does not do. Keys are made inside the guest by the client's own ssh-keygen.

The pieces that need no guest (known_hosts, key files, the protocol against OpenSSH's own tools and the Python server) are in tests/host/test_sshclient.py."""
import base64
import hashlib
import re
import shutil
import subprocess
import threading
import time

from harness import test, scaled, BUILD
from qemu import free_port
from case_net import Guest

LOG_LINE = re.compile(r"^\[\w[\w-]*\] .*\n", re.M)
PROMPT_MARISA = r"marisa@gensokyo \S+ \$ "
ROOT_PROMPT = r"root@gensokyo \S+ # "
KEYGEN = shutil.which("ssh-keygen")


class ClientGuest(Guest):
    """A root console with sshd running (loopback only unless net is given). setup_keys() makes a key in the guest and authorizes it for marisa."""

    def __init__(self, name, net=None, cmdline_extra="", **kw):
        super().__init__(name, net=net, root=True, cmdline=("sshd sshd.throttle=0 sshd.peraddr=0 " + cmdline_extra).strip(), **kw)

    def __enter__(self):
        super().__enter__()
        deadline = time.time() + scaled(90)
        while ":22" not in self.out("netstat -l"):
            assert time.time() < deadline, "sshd never started listening:\n" + self.out("dmesg | grep -i sshd")
            time.sleep(1)
        return self

    def setup_keys(self, name="k", passphrase="", authorize=True):
        out, rc = self.run(f"ssh-keygen -q -t ed25519 -N '{passphrase}' -C 'test key {name}' -f /tmp/{name}")
        assert rc == 0, out
        if authorize:
            self.run("mkdir -p /etc/ssh/authorized_keys")
            self.run(f"cat /tmp/{name}.pub >> /etc/ssh/authorized_keys/marisa; chmod 644 /etc/ssh/authorized_keys/marisa")

    def ssh(self, args, timeout=120, kh="/tmp/kh", batch=True):
        """Runs `ssh ARGS` in the guest (no config file; known_hosts in `kh`; BatchMode unless batch=False) and returns (output, status). The kernel log lines
        that the console shows (sshd logs there) are taken out of the output."""
        out, rc = self.run(f"ssh -F /dev/null -o UserKnownHostsFile={kh} {'-o BatchMode=yes ' if batch else ''}{args}", timeout=timeout)
        return LOG_LINE.sub("", out), rc


@test
def sshclient_login_and_exec():
    """ssh-keygen makes a key in the guest, ssh logs in to the guest's own sshd with it: output, exit status, stderr, an unknown key refused, host key policies and a changed host key"""
    with ClientGuest("t-sshc-exec") as g:
        g.setup_keys("k")
        g.setup_keys("other", authorize=False)
        out, rc = g.ssh("-i /tmp/k -o StrictHostKeyChecking=accept-new marisa@127.0.0.1 'echo hello-from-client; id -un'")
        assert rc == 0 and "hello-from-client" in out and "marisa" in out, (rc, out)
        assert "Permanently added '127.0.0.1' (ED25519) to the list of known hosts." in out, out
        out, rc = g.run("cat /tmp/kh")
        assert re.fullmatch(r"127\.0\.0\.1 ssh-ed25519 AAAA\S+\s*", out), out                 # the host key was remembered
        out, rc = g.ssh("-i /tmp/k marisa@127.0.0.1 'exit 3'")
        assert rc == 3, (rc, out)                                                              # no prompt: the host is known now
        out, rc = g.ssh("-i /tmp/k marisa@127.0.0.1 'echo to-out; echo to-err >&2; exit 0' 2>/dev/null")
        assert rc == 0 and "to-out" in out and "to-err" not in out, (rc, out)
        out, rc = g.ssh("-i /tmp/other marisa@127.0.0.1 'echo should-not-run'")
        assert rc == 255 and "should-not-run" not in out and "Permission denied (publickey)" in out, (rc, out)
        out, rc = g.ssh("-i /tmp/k -o StrictHostKeyChecking=yes marisa@127.0.0.1 'echo no'", kh="/tmp/kh-empty")
        assert rc == 255 and "No ED25519 host key is known for 127.0.0.1 and you have requested strict checking" in out and "Host key verification failed." in out, (rc, out)
        out, rc = g.ssh("-i /tmp/k marisa@127.0.0.1 'echo no'", kh="/tmp/kh-empty")
        assert rc == 255 and "can't be established" in out and "ED25519 key fingerprint is SHA256:" in out, (rc, out)       # BatchMode: no terminal to ask on
        g.run("ssh-keygen -q -t ed25519 -N '' -f /tmp/wrong && sed 's/^/127.0.0.1 /' /tmp/wrong.pub > /tmp/kh-changed")
        for strict in ("accept-new", "no", "yes", "ask"):
            out, rc = g.ssh(f"-i /tmp/k -o StrictHostKeyChecking={strict} marisa@127.0.0.1 'echo should-not-run'", kh="/tmp/kh-changed")
            assert rc == 255 and "REMOTE HOST IDENTIFICATION HAS CHANGED" in out and "should-not-run" not in out, (strict, rc, out)
        out, rc = g.run("cat /tmp/kh-changed | wc -l")
        assert out.strip() == "1", out                                                         # the file was not touched


@test
def sshclient_pty_shell():
    """ssh on the console gets a login shell on the server's pty: prompt, environment, Ctrl+C reaching the far program, the exit status of `exit N`, `~.` and `~?`, and the console is cooked again afterwards"""
    with ClientGuest("t-sshc-pty") as g:
        g.setup_keys("k")
        vm = g.vm
        base = "ssh -F /dev/null -o UserKnownHostsFile=/tmp/kh -o StrictHostKeyChecking=accept-new -o BatchMode=yes -i /tmp/k marisa@127.0.0.1"

        def type_line(s):
            vm.send(s + "\n")

        def remote(cmd, timeout=60):
            """Types one command at the remote prompt; returns what it printed (the markers have a quote in the middle so the echo does not match)."""
            vm.send(f"echo __BE''GIN__; {cmd}; echo __EN''D__\n")
            out = vm.expect(r"\n__BEGIN__\n(.*?)__END__\n", timeout=timeout).group(1)
            vm.expect(PROMPT_MARISA, timeout=30)
            return out

        type_line(base)
        vm.expect(PROMPT_MARISA, timeout=90)
        out = remote("echo pty-ok-$USER; tty; echo term=$TERM; echo ssh=$SSH_CONNECTION")
        assert "pty-ok-marisa" in out and "/dev/pts/" in out and "term=xterm" in out and re.search(r"ssh=127\.0\.0\.1 \d+ 127\.0\.0\.1 22", out), out
        type_line("sleep 100")
        time.sleep(scaled(2))
        vm.send("\x03")                                                    # raw mode: Ctrl+C is a character, and the far pty turns it into SIGINT
        vm.expect(PROMPT_MARISA, timeout=30)
        assert "alive" in remote("echo alive")
        type_line("exit 5")
        vm.expect(r"Connection to 127\.0\.0\.1 closed\.", timeout=30)
        vm.expect(ROOT_PROMPT, timeout=30)
        out, rc = g.run("echo cooked-again")                               # the console accepts a line again: the terminal modes were restored
        assert "cooked-again" in out and rc == 0, (out, rc)

        # the escape character: ~? lists the escapes (and sends nothing), ~. ends the session at once, with the status 255
        type_line(base + "; echo status=$?")
        vm.expect(PROMPT_MARISA, timeout=90)
        vm.send("\n~?")
        vm.expect(r"Supported escape sequences:.*~\.\s+- terminate connection.*~\?\s+- this message", timeout=15)
        vm.send("\r~.")
        vm.expect(r"Connection to 127\.0\.0\.1 closed\.", timeout=30)
        vm.expect(r"status=255", timeout=30)
        vm.expect(ROOT_PROMPT, timeout=30)
        time.sleep(scaled(1))
        deadline = time.time() + scaled(30)
        while "gapsh" in g.out("ps | grep marisa"):                        # the far shell was hung up when the connection went
            assert time.time() < deadline, g.out("ps")
            time.sleep(1)


def host_server(**kw):
    """The Python SSH server of tests/sshsrv.py on the host's loopback; the guest reaches it as 10.0.2.2."""
    import sshsrv
    return sshsrv.Server(**kw)


def guest_public_key(g, name="k"):
    """The 32 bytes of the public key that the guest's ssh-keygen wrote to /tmp/NAME.pub."""
    out, rc = g.run(f"cat /tmp/{name}.pub")
    assert rc == 0, out
    return base64.b64decode(out.split()[1])[-32:]


@test
def sshclient_passwords_and_the_python_server():
    """against the independent Python server: a password asked for without echo (a wrong one first), keyboard-interactive, the banner, a refusal, BatchMode"""
    with host_server(users={"bob": "s3cret pw"}, kbdint={"carol": [("Password: ", False, "hunter2"), ("Code: ", True, "4711")]},
                     banner="Welcome to the test server.\n", max_tries=20) as srv, Guest("t-sshc-pw", net="e1000", root=True, cmdline="sshd.throttle=0") as g:
        vm = g.vm
        base = f"ssh -F /dev/null -o UserKnownHostsFile=/tmp/kh2 -o StrictHostKeyChecking=accept-new -o PubkeyAuthentication=no -p {srv.port}"

        mark = len(vm.text())
        vm.send(base + " bob@10.0.2.2 'echo password-ok'; echo status=$?\n")
        vm.expect(r"Welcome to the test server\.", timeout=60)
        vm.expect(r"bob@10\.0\.2\.2's password: ", timeout=30)
        vm.send("wrong\n")
        vm.expect(r"Permission denied, please try again\.\s+bob@10\.0\.2\.2's password: ", timeout=30)
        vm.send("s3cret pw\n")
        vm.expect(r"password-ok\s+status=0", timeout=30)
        vm.expect(g.prompt, timeout=30)
        shown = vm.text()[mark:]
        assert "s3cret pw" not in shown, shown                              # nothing typed at a password prompt is echoed
        assert "Permanently added '[10.0.2.2]:%d' (ED25519)" % srv.port in shown, shown
        assert ("auth", "bob", "password", False) in srv.events and ("auth", "bob", "password", True) in srv.events, srv.events

        vm.send(base + " -o NumberOfPasswordPrompts=2 bob@10.0.2.2 'echo never'; echo status=$?\n")
        vm.expect(r"password: ", timeout=30)
        vm.send("no1\n")
        vm.expect(r"password: ", timeout=30)
        vm.send("no2\n")
        vm.expect(r"bob@10\.0\.2\.2: Permission denied \(keyboard-interactive,password\)\.\s+status=255", timeout=30)
        vm.expect(g.prompt, timeout=30)

        vm.send(base + " carol@10.0.2.2 'echo kbd-ok'; echo status=$?\n")
        vm.expect(r"Password: ", timeout=60)
        vm.send("hunter2\n")
        vm.expect(r"Code: ", timeout=30)
        vm.send("4711\n")                                                    # this prompt asked for echo: the digits are shown
        vm.expect(r"4711\s+kbd-ok\s+status=0", timeout=30)
        vm.expect(g.prompt, timeout=30)

        out, rc = g.run(f"ssh -F /dev/null -o UserKnownHostsFile=/tmp/kh2 -o PubkeyAuthentication=no -o BatchMode=yes -p {srv.port} bob@10.0.2.2 'echo x'")
        assert rc == 255 and "Permission denied" in out, (rc, out)          # BatchMode: no password is asked for
        out, rc = g.run(f"ssh -F /dev/null -q -o UserKnownHostsFile=/tmp/kh2 -o PubkeyAuthentication=no -o BatchMode=yes -p {srv.port} carol@10.0.2.2 'echo x'")
        assert "Welcome" not in out, out                                      # -q silences the banner


@test
def sshclient_rekeys_and_bulk_data_against_the_python_server():
    """a key made in the guest logs in to the Python server; 100000 lines go up through `sum` with the client re-keying every 128 KB, the server starts a re-key mid-session, 600 KB come down intact"""
    with host_server(keys={"frank": []}) as srv, Guest("t-sshc-bulk", net="e1000", root=True, cmdline="sshd.throttle=0") as g:
        g.run("ssh-keygen -q -t ed25519 -N '' -f /tmp/k")
        srv.keys["frank"] = [guest_public_key(g)]
        base = f"ssh -F /dev/null -i /tmp/k -o UserKnownHostsFile=/tmp/kh2 -o StrictHostKeyChecking=accept-new -o BatchMode=yes -p {srv.port} frank@10.0.2.2"
        data = b"".join(b"%d\n" % i for i in range(1, 100001))
        out, rc = g.run(f"seq 1 100000 | {base.replace('-o BatchMode=yes', '-o BatchMode=yes -o RekeyLimit=128K')} sum", timeout=300)
        assert rc == 0 and out.split()[-2:] == [str(len(data)), hashlib.sha256(data).hexdigest()], (rc, out[-300:])
        assert len(srv.find("kex")) >= 3, srv.events                         # the client started re-keys of its own
        out, rc = g.run(f"{base} rekey", timeout=60)
        assert rc == 0 and "before-rekey" in out and "after-rekey" in out, (rc, out)
        out, rc = g.run(f"{base} gen 600000 | sha256sum", timeout=300)
        gen = bytes((i * 7 + 3) & 255 for i in range(600000))
        assert hashlib.sha256(gen).hexdigest() in out, out
        out, rc = g.run(f"{base} status 7")
        assert rc == 7, (rc, out)
        out, rc = g.run(f"{base} signal TERM")
        assert rc == 255 and "Killed by signal TERM." in out, (rc, out)


@test
def sshclient_scp_and_sftp():
    """scp and sftp in the guest against the guest's own sftp-server: a 1.3 MB file each way intact, a directory tree each way, a wildcard, -p, an unreadable file, a missing one, and an sftp batch"""
    with ClientGuest("t-sshc-scp") as g:
        g.setup_keys("k")
        opts = "-F /dev/null -i /tmp/k -o UserKnownHostsFile=/tmp/kh -o StrictHostKeyChecking=accept-new -o BatchMode=yes"
        g.run("seq 1 200000 > /tmp/src.txt && chmod 644 /tmp/src.txt")
        want, _ = g.run("sha256sum < /tmp/src.txt")
        t0 = time.time()
        out, rc = g.run(f"scp {opts} /tmp/src.txt marisa@127.0.0.1:/tmp/up.txt", timeout=300)
        took = time.time() - t0
        assert rc == 0, (rc, out)
        out, rc = g.run("sha256sum < /tmp/up.txt && ls -l /tmp/up.txt")
        assert want.split()[0] in out and "marisa" in out, out                             # intact, and it belongs to the resident who logged in
        print(f"scp up: {1_288_895 / 1024 / took:.0f} KiB/s (guest clock)")
        out, rc = g.run(f"scp {opts} marisa@127.0.0.1:/tmp/up.txt /tmp/down.txt", timeout=300)
        assert rc == 0, (rc, out)
        out, rc = g.run("sha256sum < /tmp/down.txt")
        assert want.split()[0] in out, out

        g.run("mkdir -p /tmp/tree/a/b && echo top > /tmp/tree/top.txt && echo mid > /tmp/tree/a/mid.txt && seq 1 3000 > /tmp/tree/a/b/deep.txt")
        out, rc = g.run(f"scp {opts} -r -p /tmp/tree marisa@127.0.0.1:/tmp/tree2", timeout=120)
        assert rc == 0, (rc, out)
        out, rc = g.run(f"ssh {opts} marisa@127.0.0.1 'cd /tmp/tree2 && find . -type f | sort && cat top.txt a/mid.txt'")
        assert "./a/b/deep.txt" in out and "./a/mid.txt" in out and "./top.txt" in out and "top" in out and "mid" in out, out
        out, rc = g.run(f"scp {opts} -r marisa@127.0.0.1:/tmp/tree2 /tmp/tree3", timeout=120)
        assert rc == 0, (rc, out)
        out, rc = g.run("cd /tmp/tree3 && find . -type f | sort && sha256sum a/b/deep.txt && cd /tmp/tree && sha256sum a/b/deep.txt")
        lines = out.split("\n")
        sums = [l.split()[0] for l in lines if re.fullmatch(r"[0-9a-f]{64}\s+a/b/deep\.txt", l.strip())]
        assert len(sums) == 2 and sums[0] == sums[1], out

        g.run("echo one > /tmp/w1.log; echo two > /tmp/w2.log; mkdir -p /tmp/logs")
        out, rc = g.run(f"scp {opts} 'marisa@127.0.0.1:/tmp/w*.log' /tmp/logs && ls /tmp/logs", timeout=120)
        assert rc == 0 and "w1.log" in out and "w2.log" in out, (rc, out)
        out, rc = g.run(f"scp {opts} marisa@127.0.0.1:/tmp/does-not-exist /tmp/x")
        assert rc == 1 and "does-not-exist: No such file" in out, (rc, out)
        out, rc = g.run(f"scp {opts} marisa@127.0.0.1:/etc/shadow /tmp/x")
        assert rc == 1 and "Permission denied" in out, (rc, out)                             # the server's rules apply: /etc/shadow is root's
        out, rc = g.run(f"scp {opts} /tmp/tree marisa@127.0.0.1:/tmp/tree4")
        assert rc == 1 and "not a regular file" in out, (rc, out)                           # a directory needs -r

        g.run("printf 'pwd\\nmkdir /tmp/sf\\ncd /tmp/sf\\npwd\\nput /tmp/src.txt moved.txt\\nls -l\\nrename moved.txt renamed.txt\\nchmod 600 renamed.txt\\nget renamed.txt /tmp/sf-back.txt\\nrm renamed.txt\\ncd /tmp\\nrmdir sf\\nbye\\n' > /tmp/batch.txt")
        out, rc = g.run(f"sftp {opts} -b /tmp/batch.txt marisa@127.0.0.1", timeout=300)
        assert rc == 0 and "Remote working directory: /HumanVillage/marisa" in out and "Remote working directory: /tmp/sf" in out and "moved.txt" in out, (rc, out)
        out, rc = g.run("sha256sum < /tmp/sf-back.txt; ls /tmp/sf")
        assert want.split()[0] in out and "No such file" in out, out
        g.run("printf 'mkdir /tmp/nodir/x\\nmkdir /tmp/never\\n' > /tmp/batch2.txt")
        out, rc = g.run(f"sftp {opts} -b /tmp/batch2.txt marisa@127.0.0.1; ls /tmp/never")
        assert "No such file" in out, out                                                   # the batch stopped at the failure


@test
def sshclient_manual_pages():
    """the manual pages of the client (ssh, ssh-keygen, ssh-keyscan, scp, sftp, ssh_config, known_hosts) exist, parse, and the whole library still checks"""
    with Guest("t-sshc-man") as g:
        out, rc = g.run("patchouli --check-all", timeout=120)
        assert re.search(r"\d+ pages checked, \d+ problems?", out), out
        mine = ("/ssh.pw", "/ssh-keygen.pw", "/ssh-keyscan.pw", "/scp.pw", "/sftp.pw", "/ssh_config.pw", "/known_hosts.pw")
        ours = [line for line in out.splitlines() if re.search(r"\.pw:\d+:", line) and line.split(":")[0].endswith(mine)]
        assert not ours, ours                                                 # (a problem in somebody else's page is theirs: the `manual` test of case_text.py counts those)
        index = g.out("patchouli -l", timeout=60)
        for name, section in (("ssh", 1), ("ssh-keygen", 1), ("ssh-keyscan", 1), ("scp", 1), ("sftp", 1), ("ssh_config", 5), ("known_hosts", 5)):
            assert re.search(rf"^{re.escape(name)}\({section}\)", index, re.M), (name, index[-600:])
        out, rc = g.run("patchouli ssh | head -12")
        assert rc == 0 and "ssh(1)" in out and "log in to another machine" in out, out
        out, rc = g.run("patchouli 5 ssh_config | grep -c StrictHostKeyChecking")
        assert out.strip().isdigit() and int(out.strip()) >= 1, out


@test
def sshclient_keygen_keyscan_and_passphrases():
    """ssh-keygen in the guest: the fingerprint and the picture are what the host's OpenSSH computes for the same key; a passphrase is asked for (without echo) only when the server would take the key, wrong then right; ssh-keyscan returns the fingerprint that sshd logged, and its line works as a known_hosts entry"""
    with ClientGuest("t-sshc-keys") as g:
        out, rc = g.run("ssh-keygen -t ed25519 -N 'pass phrase 1' -C 'guest key' -f /tmp/pk", timeout=120)
        assert rc == 0 and "Your identification has been saved in /tmp/pk" in out, (rc, out)
        shown_fp = re.search(r"The key fingerprint is:\s+(SHA256:\S+) guest key", out).group(1)
        pub, rc = g.run("cat /tmp/pk.pub")
        assert pub.startswith("ssh-ed25519 ") and pub.strip().endswith("guest key"), pub
        work = BUILD / "sshclient-test"
        work.mkdir(parents=True, exist_ok=True)
        (work / "pk.pub").write_text(pub.strip() + "\n")
        mine = subprocess.run([KEYGEN, "-lv", "-f", str(work / "pk.pub")], capture_output=True, text=True, check=True).stdout
        assert shown_fp in mine, (shown_fp, mine)                                          # the guest computed the same fingerprint as OpenSSH
        out, rc = g.run("ssh-keygen -lv -f /tmp/pk.pub")
        assert rc == 0 and out.strip().replace("\r", "") == mine.strip(), (out, mine)        # ... and drew the same picture
        out, rc = g.run("ssh-keygen -l -f /tmp/pk")                                         # the private file's public half needs no passphrase
        assert shown_fp in out, out

        g.run("mkdir -p /etc/ssh/authorized_keys; cat /tmp/pk.pub >> /etc/ssh/authorized_keys/marisa; chmod 644 /etc/ssh/authorized_keys/marisa")
        g.run("ssh-keygen -q -t ed25519 -N '' -f /tmp/unlisted")
        vm = g.vm
        base = "ssh -F /dev/null -o UserKnownHostsFile=/tmp/kh -o StrictHostKeyChecking=accept-new -o PasswordAuthentication=no -i /tmp/unlisted -i /tmp/pk marisa@127.0.0.1"
        mark = len(vm.text())
        vm.send(base + " 'echo unlocked-ok'; echo status=$?\n")
        vm.expect(r"Enter passphrase for key '/tmp/pk': ", timeout=90)                    # (the unlisted key, tried first, asked for nothing: it has none, and the server refused it)
        vm.send("not the one\n")
        vm.expect(r"Bad passphrase, try again for /tmp/pk\s+Enter passphrase for key '/tmp/pk': ", timeout=60)
        vm.send("pass phrase 1\n")
        vm.expect(r"unlocked-ok\s+status=0", timeout=90)
        vm.expect(g.prompt, timeout=30)
        shown = vm.text()[mark:]
        assert "pass phrase 1" not in shown and "not the one" not in shown, shown           # nothing typed at the passphrase prompts was echoed

        out, rc = g.run("ssh-keyscan 127.0.0.1 2>/dev/null > /tmp/scanned; cat /tmp/scanned; ssh-keygen -l -f /tmp/scanned")
        scanned_fp = re.search(r"(SHA256:\S+)", out).group(1)
        log = g.out("dmesg | grep 'sshd.*host key'")
        assert scanned_fp in log, (scanned_fp, log)                                         # the key sshd announced in its log is the one ssh-keyscan fetched
        out, rc = g.run("ssh -F /dev/null -i /tmp/pk -o UserKnownHostsFile=/tmp/scanned -o StrictHostKeyChecking=yes -o BatchMode=yes marisa@127.0.0.1 'echo scanned-key-trusted' 2>&1 | tail -3")
        assert "Enter passphrase" not in out and ("scanned-key-trusted" in out or "passphrase" in out or "Permission" in out), out
        out, rc = g.run("ssh -F /dev/null -i /tmp/unlisted -o UserKnownHostsFile=/tmp/scanned -o StrictHostKeyChecking=yes -o BatchMode=yes marisa@127.0.0.1 'echo no' 2>&1 | tail -2")
        assert "Host key verification failed" not in out, out                              # the scanned line is accepted as the host's entry (the login itself is refused for the unlisted key)
        out, rc = g.run("ssh-keygen -F 127.0.0.1 -f /tmp/scanned | head -2; ssh-keygen -R 127.0.0.1 -f /tmp/scanned; cat /tmp/scanned | wc -c")
        assert "Host 127.0.0.1 found: line 1" in out and "updated" in out, out
