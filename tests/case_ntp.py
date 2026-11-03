"""Network time at boot (kernel/net/ntp.c, docs/NTP.md): the kernel asks NTP servers by itself and steps the clock.

The "internet" of these tests is Python on the host's loopback, which QEMU's user-mode networking shows to the guest as 10.0.2.2 (the
same trick as tests/case_net.py). The servers here can lie in the ways a real one might (stratum 0, the alarm flag, a reply to another
request, a clock in 2019, a root distance of seconds), two of them can disagree, and a name can be looked up through a DNS responder.

Every guest in this file sets ntp.builtin=127.0.0.1 unless it is testing the built-in group: the built-in group names real public servers,
and a test must not talk to the internet (or be moved by what it answers). The harness gives every OTHER guest ntp=off for the same reason.

The guest clock can run several times slower than wall time, so nothing here sleeps for a fixed time and then expects an effect: every wait
polls /proc/ntp with a generous timeout, and the comparisons with the host's clock allow a minute (the offsets tested are an hour or more).
"""
import re
import socket
import struct
import threading
import time

import test_shell
from harness import test, Qemu, scaled

ROOT_PROMPT = r"root@gensokyo \S+ # "
LEASE = r"\[Yukari\] eth0: lease 10\.0\.2\.15/255\.255\.255\.0 from 10\.0\.2\.2"
NTP_DELTA = 2208988800
NOWHERE = "ntp.builtin=127.0.0.1"                        # an address the client refuses at once ("not an address a time server can have"): no packet leaves, and no seconds are spent waiting for a silent port


# ------------------------------------------------------------------------------------------------------------ servers on the host

def ts_bytes(t):
    sec = int(t)
    return struct.pack(">II", (sec + NTP_DELTA) & 0xFFFFFFFF, int((t - sec) * (1 << 32)) & 0xFFFFFFFF)


class NtpServer(threading.Thread):
    """An NTP server whose clock is `shift` seconds ahead of the host's, and which can misbehave. `mode`:
    ok, stratum0 (kiss-o'-death RATE), unsync (leap indicator 3), stratum16, wrong_origin (copies the wrong timestamp back),
    ancient (a clock in 2019), big_root (a root delay of ten seconds), silent (never answers)."""

    def __init__(self, shift=0.0, mode="ok", stratum=2):
        super().__init__(daemon=True)
        self.shift, self.mode, self.stratum = shift, mode, stratum
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(0.2)
        self.port = self.sock.getsockname()[1]
        self.hits = []
        self.running = True
        self.start()

    def stop(self):
        self.running = False
        self.join(2)
        self.sock.close()

    def run(self):
        while self.running:
            try:
                data, addr = self.sock.recvfrom(512)
            except socket.timeout:
                continue
            except OSError:
                return
            self.hits.append((time.time(), data))
            if len(data) < 48 or self.mode == "silent":
                continue
            now = time.time() + self.shift
            if self.mode == "ancient":
                now = 1_560_000_000.0                                     # 2019-06
            p = bytearray(48)
            li, stratum, root_delay, refid = 0, self.stratum, 0, b"\x7f\x00\x00\x01"
            if self.mode == "stratum0":
                li, stratum, refid = 3, 0, b"RATE"
            elif self.mode == "unsync":
                li = 3
            elif self.mode == "stratum16":
                stratum = 16
            elif self.mode == "big_root":
                root_delay = 10 << 16
            p[0] = li << 6 | 4 << 3 | 4                                   # version 4, mode 4 (server)
            p[1] = stratum
            struct.pack_into(">I", p, 4, root_delay)
            p[12:16] = refid
            p[16:24] = ts_bytes(now - 5)                                  # reference timestamp
            p[24:32] = data[40:48] if self.mode != "wrong_origin" else bytes(b ^ 0x5A for b in data[40:48])
            p[32:40] = ts_bytes(now)
            p[40:48] = ts_bytes(now)
            try:
                self.sock.sendto(bytes(p), addr)
            except OSError:
                pass


def _enc(name):
    return b"".join(bytes([len(p)]) + p.encode() for p in name.split(".") if p) + b"\0"


class DnsServer(threading.Thread):
    """Answers A queries from a table {name: ip}; a name in `cnames` {alias: name} gets a CNAME and then the A record; the rest is NXDOMAIN."""

    def __init__(self, table, cnames=None):
        super().__init__(daemon=True)
        self.table, self.cnames = table, cnames or {}
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(0.2)
        self.port = self.sock.getsockname()[1]
        self.queries = []
        self.running = True
        self.start()

    def stop(self):
        self.running = False
        self.join(2)
        self.sock.close()

    def run(self):
        while self.running:
            try:
                data, addr = self.sock.recvfrom(512)
            except socket.timeout:
                continue
            except OSError:
                return
            try:
                ident = data[:2]
                pos = 12
                labels = []
                while data[pos]:
                    n = data[pos]
                    labels.append(data[pos + 1:pos + 1 + n].decode().lower())
                    pos += n + 1
                name = ".".join(labels)
                qtype = struct.unpack(">H", data[pos + 1:pos + 3])[0]
                question = data[12:pos + 5]
                self.queries.append((name, qtype))
                answers, target = b"", self.cnames.get(name, name)
                if name in self.cnames:
                    rd = _enc(target)
                    answers += b"\xc0\x0c" + struct.pack(">HHIH", 5, 1, 60, len(rd)) + rd
                owner = b"\xc0\x0c" if target == name else _enc(target)
                count = 1 if name in self.cnames else 0
                if qtype == 1 and target in self.table:
                    answers += owner + struct.pack(">HHIH", 1, 1, 60, 4) + bytes(int(x) for x in self.table[target].split("."))
                    count += 1
                    rcode = 0
                else:
                    rcode = 0 if (target in self.table) else 3
                reply = ident + struct.pack(">HHHHH", 0x8180 | rcode, 1, count, 0, 0) + question + answers
                self.sock.sendto(reply, addr)
            except Exception:                                            # a malformed query must not end the responder
                pass


# ------------------------------------------------------------------------------------------------------------ the guest

class Guest:
    """A booted root text console with a NIC on QEMU's user-mode network and `cmdline` added to the kernel's."""

    def __init__(self, name, cmdline="", net="virtio-net-pci"):
        self.vm = Qemu(cmdline=("text user=root " + cmdline).strip(), name=name, net=net)

    def __enter__(self):
        self.vm.start()
        patterns = [ROOT_PROMPT, LEASE]
        deadline = time.time() + scaled(300)
        while True:
            text = self.vm.text()
            found = [re.search(p, text, re.S) for p in patterns]
            if all(found):
                break
            if not self.vm.alive():
                raise RuntimeError("QEMU exited while booting\n" + self.vm.diagnose())
            if time.time() > deadline:
                raise TimeoutError(f"the guest never showed {patterns}\n" + self.vm.diagnose())
            time.sleep(0.2)
        self.vm._pos = max(m.end() for m in found)
        return self

    def __exit__(self, *exc):
        self.vm.stop()

    def run(self, cmd, timeout=120):
        return test_shell.run(self.vm, cmd, timeout=timeout, prompt=ROOT_PROMPT)

    def out(self, cmd, timeout=120):
        return self.run(cmd, timeout)[0]

    def ntp(self):
        """/proc/ntp as a dict of key: value."""
        d = {}
        for line in self.out("cat /proc/ntp").splitlines():
            if ": " in line:
                k, v = line.split(": ", 1)
                d[k.strip()] = v.strip()
            elif line.endswith(":"):
                d[line[:-1]] = ""
        return d

    def now(self):
        return int(self.out("date +%s").strip().splitlines()[-1])

    def log(self):
        return self.vm.text()

    def wait(self, pred, what, timeout=240, step=1.0):
        deadline = time.time() + scaled(timeout)
        last = None
        while time.time() < deadline:
            last = pred()
            if last:
                return last
            time.sleep(step)
        raise AssertionError(f"timed out waiting for {what}; last value {last!r}\n--- serial tail ---\n{self.vm.text()[-2500:]}")

    def wait_log(self, regex, timeout=240, since=0):
        def look():
            m = re.search(regex, self.vm.text()[since:])
            return m
        return self.wait(look, f"the log line /{regex}/", timeout)

    def sync(self, timeout=240):
        """Asks the kernel's client for a round now and waits until it has finished one. Returns /proc/ntp after it."""
        before = int(self.ntp()["cycles"])
        mark = len(self.vm.text())
        self.run("echo sync > /proc/ntp")
        st = self.wait(lambda: (lambda d: d if int(d["cycles"]) > before and d["ntp"] != "syncing" else None)(self.ntp()), "a finished round", timeout, step=1.0)
        st["_log"] = self.vm.text()[mark:]
        return st


def host_now():
    return time.time()


def near(guest_seconds, expect_shift, slack=150):
    """The guest's clock reads the host's plus `expect_shift` seconds, give or take `slack`."""
    return abs(guest_seconds - (host_now() + expect_shift)) < slack


# ------------------------------------------------------------------------------------------------------------ tests

@test
def ntp_boot_sync():
    """ntp=HOST:PORT: the clock is stepped soon after the lease (+2 h from a Python server), /proc/ntp and the log say so, sntp -S shows it, the chip is untouched, and a clock set by hand early is put right again"""
    srv = NtpServer(shift=7200)
    try:
        with Guest("t-ntp-boot", f"ntp=10.0.2.2:{srv.port} {NOWHERE}") as g:
            m = g.wait_log(rf"\[Sakuya\] clock set from 10\.0\.2\.2:{srv.port}: \+(\d+)\.(\d{{3}}) s \(round trip (\d+) ms, stratum 2\)")
            assert 7140 <= int(m.group(1)) <= 7290, m.group(0)
            assert near(g.now(), 7200), g.now() - host_now()
            st = g.ntp()
            assert st["ntp"] == "synced" and st["server"] == f"10.0.2.2:{srv.port}" and st["source"] == "cmdline" and st["stratum"] == "2", st
            assert int(st["syncs"]) == 1 and int(st["failures"]) == 0 and st["fail_streak"] == "0", st
            assert 7140_000 <= int(st["offset_ms"]) <= 7290_000, st
            assert st["rtc"].startswith("untouched") and st["interval_s"] == "14400", st
            assert re.fullmatch(r"20\d\d-\d\d-\d\d \d\d:\d\d:\d\d UTC", st["last_sync"]), st
            rtc = int(g.out("cat /proc/rtc").strip().splitlines()[-1])
            assert abs(rtc - host_now()) < 300, f"the clock chip was moved: {rtc - host_now()}"         # the chip still holds the host's time, not +2 h
            out, rc = g.run("sntp -S")
            assert rc == 0 and "ntp: synced" in out and f"server: 10.0.2.2:{srv.port}" in out, out
            out, rc = g.run("sntp -n", timeout=180)                          # root asks the kernel's client to go again, and sees the result of that round
            assert rc == 0 and "ntp: synced" in out and "syncs: 2" in out and "cycles: 2" in out, (out, rc)
            # the request on the wire: version 4, mode 3, a transmit time close to the host's, the rest zero
            assert len(srv.hits) >= 1
            when, pkt = srv.hits[0]
            assert len(pkt) == 48 and pkt[0] == 0x23 and pkt[1:40] == bytes(39), pkt.hex()
            sent = struct.unpack(">I", pkt[40:44])[0] - NTP_DELTA
            assert abs(sent - when) < 120, (sent, when)
            # somebody sets the clock by hand during the first minutes (init's rtc=local correction does the same): the servers are asked again
            out, rc = g.run("date -u -s '2030-01-01 00:00:00'")
            assert rc == 0, out
            g.wait_log(r"\[Sakuya\] the clock was set by somebody else \([+-]\d+\.\d{3} s\): asking the servers again", timeout=120)
            g.wait(lambda: near(g.now(), 7200), "the clock to come back from 2030", timeout=240)
            assert int(g.ntp()["external_steps"]) >= 1
    finally:
        srv.stop()


@test
def ntp_liars_are_not_believed():
    """a server that says stratum 0, leap indicator 3, stratum 16, answers another request, lives in 2019 or is seconds from a real clock is ignored; two servers 2 h apart cancel each other; two that agree are believed"""
    liar = NtpServer(shift=7200, mode="stratum0")
    a, b = NtpServer(shift=0), NtpServer(shift=7200)
    try:
        with Guest("t-ntp-liars", f"ntp=10.0.2.2:{liar.port} {NOWHERE}") as g:
            # the first round started by itself at boot and the liar is in its first mode
            st = g.wait(lambda: (lambda d: d if int(d["cycles"]) >= 1 and d["ntp"] != "syncing" else None)(g.ntp()), "the first round")
            scenarios = [("stratum0", r"it asks us to stay away \(kiss-o'-death\): RATE"), ("unsync", r"its clock is not synchronised \(leap indicator 3\)"),
                         ("stratum16", r"its clock is not synchronised \(stratum 16\)"), ("wrong_origin", r"an answer to another request"),
                         ("ancient", r"impossible timestamps"), ("big_root", r"too far from a real clock \(root distance\)")]
            for i, (mode, text) in enumerate(scenarios):
                if i:
                    liar.mode = mode
                    st = g.sync()
                log = g.log()
                assert re.search(rf"\[Sakuya\] network time: 10\.0\.2\.2:{liar.port}: {text}", log), (mode, log[-1500:])
                assert int(st["syncs"]) == 0 and near(g.now(), 0, slack=600), (mode, st, g.now() - host_now())      # the clock did not move (+2 h would be seen)
                assert st["ntp"] == "failing" and st["last_error"] == "no server answered", (mode, st)
            assert int(st["failures"]) >= len(scenarios) and int(st["fail_streak"]) >= len(scenarios), st

            # two servers 2 h apart: neither is believed
            g.run(f"echo 'server 10.0.2.2:{a.port},10.0.2.2:{b.port}' > /proc/ntp")
            st = g.sync()
            assert re.search(rf"\[Sakuya\] network time: the servers disagree \(10\.0\.2\.2:{a.port} says [+-]\d+\.\d{{3}} s, 10\.0\.2\.2:{b.port} says \+7\d{{3}}\.\d{{3}} s\): neither is believed", st["_log"]), st["_log"]
            assert int(st["syncs"]) == 0 and near(g.now(), 0, slack=600), st
            assert "disagree" in st["last_error"], st

            # two that agree: believed, the better of the two sets the clock, and the failure streak ends
            b.shift = 3600
            a.shift = 3600
            st = g.sync()
            m = re.search(rf"\[Sakuya\] clock set from 10\.0\.2\.2:(\d+): \+(\d+)\.\d{{3}} s \(round trip \d+ ms, stratum 2; 10\.0\.2\.2:(\d+) agrees within \d+ ms\)", st["_log"])
            assert m and {int(m.group(1)), int(m.group(3))} == {a.port, b.port} and 3540 <= int(m.group(2)) <= 3690, st["_log"]
            assert int(st["syncs"]) == 1 and st["fail_streak"] == "0" and st["ntp"] == "synced" and st["last_error"] == "-", st
            assert near(g.now(), 3600), g.now() - host_now()
            # the liar was asked every time, and the built-in port (nothing there) never answered
            assert len(liar.hits) >= len(scenarios)
    finally:
        for s in (liar, a, b):
            s.stop()


@test
def ntp_off_does_nothing():
    """ntp=off: no thread, no packets to a server that is there, /proc/ntp says off, the commands that talk to the client fail cleanly"""
    srv = NtpServer(shift=7200)
    try:
        with Guest("t-ntp-off", f"ntp=off ntp.builtin=10.0.2.2:{srv.port}") as g:
            time.sleep(15)                                                  # (a client that was going to ask would have done it by now)
            out, rc = g.run("cat /proc/ntp")
            assert rc == 0 and out.startswith("ntp: off\n") and "ntp=off" in out, out
            assert not srv.hits, "a packet reached the server although ntp=off"
            assert near(g.now(), 0, slack=600)
            assert not re.search(r" ntp +ticks=", g.out("cat /proc/tasks"))   # no kernel thread of that name
            out, rc = g.run("echo sync > /proc/ntp; echo rc=$?")
            assert "rc=" in out, out                                         # nothing to talk to: refused, not a hang or a crash
            assert g.out("cat /proc/ntp").startswith("ntp: off\n")
            out, rc = g.run("sntp -n")
            assert rc == 1 and "/proc/ntp" in out, (out, rc)
            assert "[Sakuya] network time is off (ntp=off)" in g.log()
    finally:
        srv.stop()


@test
def ntp_dead_server_falls_back_and_never_blocks():
    """the command-line server is dead: the shell prompt comes as usual, the round falls through to the next group (here the built-in list, a Python server), and the failure was logged"""
    live = NtpServer(shift=7200)
    dead = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)                 # a port that nobody answers on
    dead.bind(("127.0.0.1", 0))
    dead_port = dead.getsockname()[1]
    try:
        with Guest("t-ntp-dead", f"ntp=10.0.2.2:{dead_port} ntp.builtin=10.0.2.2:{live.port}") as g:
            # (Guest.__enter__ returned: the prompt and the lease were both seen, whatever the client was doing)
            out, rc = g.run("echo still-here; uptime")
            assert rc == 0 and "still-here" in out
            assert re.search(r" ntp +ticks=", g.out("cat /proc/tasks"))      # the client is a thread of its own
            m = g.wait_log(rf"\[Sakuya\] clock set from 10\.0\.2\.2:{live.port}: \+(\d+)\.\d{{3}} s", timeout=300)
            assert 7140 <= int(m.group(1)) <= 7290, m.group(0)
            log = g.log()
            assert re.search(rf"\[Sakuya\] network time: 10\.0\.2\.2:{dead_port}: no answer", log), log[-1500:]
            st = g.ntp()
            assert st["ntp"] == "synced" and st["source"] == "builtin" and st["server"] == f"10.0.2.2:{live.port}", st
            assert st["cmdline_servers"] == f"10.0.2.2:{dead_port}", st
    finally:
        dead.close()
        live.stop()


@test
def ntp_rtc_local_and_chip():
    """rtc=local tz=JST-9 ntp.rtc=1: init's own correction of a local-time chip and the client's step meet in the right answer, and the chip is then written as local time"""
    srv = NtpServer(shift=7200)
    try:
        with Guest("t-ntp-rtc", f"rtc=local tz=JST-9 ntp.rtc=1 ntp=10.0.2.2:{srv.port} {NOWHERE}") as g:
            g.wait_log(rf"\[Sakuya\] clock set from 10\.0\.2\.2:{srv.port}", timeout=300)
            g.wait_log(r"\[Sakuya\] clock chip written too: local time, \+9 h 00 min from UTC", timeout=60)
            # init shifts the system clock by the zone's offset exactly once; whichever of the two came first, the clock ends where the server says
            g.wait(lambda: near(g.now(), 7200), "the system clock to read the server's time", timeout=300)
            g.wait(lambda: abs(int(g.out("cat /proc/rtc").strip().splitlines()[-1]) - (host_now() + 7200 + 9 * 3600)) < 120, "the chip to hold the local time of the server's clock", timeout=120)
            st = g.ntp()
            assert st["rtc"].startswith("written") and "local time" in st["rtc"] and st["rtc_local"].startswith("yes"), st
    finally:
        srv.stop()


@test
def ntp_names_hosts_and_dns():
    """the built-in group is a list of names: /etc/hosts answers for one, a DNS responder (read from /etc/resolv.conf, with #PORT) for another, a name that does not exist is skipped; two servers that agree are used"""
    one, two = NtpServer(shift=3600), NtpServer(shift=3600)
    dns = DnsServer({"ntp2.gensokyo": "10.0.2.2", "real.gensokyo": "10.0.2.2"}, cnames={"alias.gensokyo": "real.gensokyo"})
    try:
        names = f"ntp1.gensokyo:{one.port},nosuch.gensokyo:1,ntp2.gensokyo:{two.port}"
        with Guest("t-ntp-dns", f"ntp.builtin={names} ntp=127.0.0.1") as g:
            g.run("printf '10.0.2.2 ntp1.gensokyo\\n' >> /etc/hosts")
            g.run(f"printf 'nameserver 10.0.2.2#{dns.port}\\n' > /etc/resolv.conf")
            g.run("echo sync > /proc/ntp")
            # The round that starts by itself at the lease can run before, during or after the two files above are written: it may find only ntp1 (from /etc/hosts) and step the clock by itself,
            # and then the round asked for finds both and sees the clock already right. So the line to wait for is the one with two servers in it, whatever offset it shows.
            m = g.wait_log(r"\[Sakuya\] clock set from ([^:]+) \(10\.0\.2\.2:(\d+)\): [+-]\d+\.\d{3} s \(round trip \d+ ms, stratum 2; ([^ ]+) \(10\.0\.2\.2:(\d+)\) agrees", timeout=300)
            log = g.log()
            assert {m.group(1), m.group(3)} == {"ntp1.gensokyo", "ntp2.gensokyo"}, log[-2500:]
            assert re.search(r"nosuch\.gensokyo: the name could not be looked up", log), log[-2500:]
            assert near(g.now(), 3600), g.now() - host_now()
            st = g.wait(lambda: (lambda d: d if d["ntp"] == "synced" else None)(g.ntp()), "a finished round", timeout=120)
            assert st["source"] == "builtin" and int(st["syncs"]) >= 1, st
            asked = [q for q in dns.queries if q[0] in ("ntp1.gensokyo", "ntp2.gensokyo", "nosuch.gensokyo")]
            assert ("ntp2.gensokyo", 1) in asked and ("nosuch.gensokyo", 1) in asked and not any(q[0] == "ntp1.gensokyo" for q in asked), asked   # (/etc/hosts came first)
            # a CNAME in front of the address is followed by the server, and the answer's A record is used
            g.run(f"echo 'builtin alias.gensokyo:{one.port}' > /proc/ntp")
            st = g.sync()
            assert re.search(rf"\[Sakuya\] clock set from alias\.gensokyo \(10\.0\.2\.2:{one.port}\)", st["_log"]), st["_log"]
    finally:
        for s in (one, two):
            s.stop()
        dns.stop()


@test
def ntp_resyncs_by_itself():
    """ntp.interval=16: the client asks again by itself, follows a server whose clock jumps, and ntp.rtc=1 writes the chip as UTC"""
    srv = NtpServer(shift=7200)
    try:
        with Guest("t-ntp-resync", f"ntp.interval=16 ntp.rtc=1 ntp=10.0.2.2:{srv.port} {NOWHERE}") as g:
            g.wait(lambda: int(g.ntp()["syncs"]) >= 1, "the first sync", timeout=300)
            assert near(g.now(), 7200)
            st = g.ntp()
            assert st["interval_s"] == "16" and st["rtc"].startswith("written") and "UTC" in st["rtc"], st
            assert abs(int(g.out("cat /proc/rtc").strip().splitlines()[-1]) - (host_now() + 7200)) < 300     # (a chip that keeps UTC gets the UTC time)
            srv.shift = 10800                                                # the server's clock jumps ahead by an hour (far more than a slow guest's lag can explain)
            g.wait(lambda: near(g.now(), 10800, slack=400) and int(g.ntp()["syncs"]) >= 2, "the next sync to follow the jump", timeout=900)       # (a sync every 16 s of GUEST time: a busy PC makes that a long wait)
            assert len(srv.hits) >= 2
            assert abs(int(g.out("cat /proc/rtc").strip().splitlines()[-1]) - (host_now() + 10800)) < 400    # the chip followed too
    finally:
        srv.stop()


@test
def ntp_asks_again_when_the_cable_comes_back():
    """the cable is pulled and plugged in again while the server's clock moves on by an hour: the client does not wait for its next interval (4 hours), it asks again soon after the new lease"""
    srv = NtpServer(shift=3600)
    try:
        with Guest("t-ntp-cable", f"ntp=10.0.2.2:{srv.port} {NOWHERE}") as g:
            vm = g.vm
            g.wait(lambda: int(g.ntp()["syncs"]) >= 1, "the first sync", timeout=300)
            assert near(g.now(), 3600)
            leases = len(re.findall(LEASE, vm.text()))
            vm.qmp("set_link", name="net0", up=False)                         # the cable is pulled ...
            g.wait_log(r"link down", timeout=120)
            g.wait(lambda: g.ntp()["network"] == "down", "the client to see the network gone", timeout=120)      # (it looks twice a second of guest time)
            srv.shift = 7200                                                 # ... and while it is out, the truth moves on
            vm.qmp("set_link", name="net0", up=True)                          # ... and plugged in again: a new lease
            g.wait(lambda: len(re.findall(LEASE, vm.text())) > leases, "a new lease", timeout=300)
            g.wait(lambda: near(g.now(), 7200, slack=90) and int(g.ntp()["syncs"]) >= 2, "the clock to follow the server again", timeout=300)
    finally:
        srv.stop()


# ------------------------------------------------------------------------------------------------------------ DHCP option 42, on a network of our own

def _csum(b):
    if len(b) % 2:
        b += b"\0"
    s = sum(struct.unpack(">%dH" % (len(b) // 2), b))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF


def _aton(ip):
    return bytes(int(x) for x in ip.split("."))


class MiniNet(threading.Thread):
    """The other end of a virtio-net card wired to QEMU's `stream` netdev: frames come and go over a TCP connection, each behind a 4-byte length.
    It is a router of one address, 10.9.0.1/24, that answers ARP for the addresses of `servers`, hands out 10.9.0.15 by DHCP with option 42 naming
    `servers` (user-mode networking's DHCP never sends one), and runs an NTP server on port 123 at each of them whose clock is its `shift` seconds ahead."""

    ROUTER = "10.9.0.1"
    MAC = bytes.fromhex("525400aabb01")

    def __init__(self, servers):
        super().__init__(daemon=True)
        self.servers = dict(servers)                       # {ip: shift seconds}
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]
        self.requested = []                                # the parameter request list of every DHCP message the guest sent
        self.ntp_hits = []                                 # (server ip, source port) of every NTP request
        self.conn = None
        self.start()

    def stop(self):
        try:
            self.listener.close()
            if self.conn:
                self.conn.close()
        except OSError:
            pass

    def send(self, frame):
        frame = frame + bytes(max(0, 60 - len(frame)))     # (an Ethernet frame is at least 60 bytes before its checksum; real cards pad, so this does too)
        self.conn.sendall(struct.pack(">I", len(frame)) + frame)

    def run(self):
        try:
            self.conn, _ = self.listener.accept()
            buf = b""
            while True:
                while len(buf) < 4:
                    d = self.conn.recv(65536)
                    if not d:
                        return
                    buf += d
                n = struct.unpack(">I", buf[:4])[0]
                while len(buf) < 4 + n:
                    d = self.conn.recv(65536)
                    if not d:
                        return
                    buf += d
                frame, buf = buf[4:4 + n], buf[4 + n:]
                try:
                    self.handle(frame)
                except Exception:                            # a frame we do not understand is not a reason to stop
                    pass
        except OSError:
            return

    def ip_frame(self, dst_mac, src_ip, dst_ip, sport, dport, data):
        udp = struct.pack(">HHHH", sport, dport, 8 + len(data), 0) + data
        ip = struct.pack(">BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), 0, 0, 64, 17, 0, _aton(src_ip), _aton(dst_ip))
        ip = ip[:10] + struct.pack(">H", _csum(ip)) + ip[12:]
        return dst_mac + self.MAC + b"\x08\x00" + ip + udp

    def handle(self, f):
        src_mac, ethertype = f[6:12], struct.unpack(">H", f[12:14])[0]
        if ethertype == 0x0806 and struct.unpack(">H", f[20:22])[0] == 1:          # ARP request
            target = ".".join(str(b) for b in f[38:42])
            if target == self.ROUTER or target in self.servers:
                reply = struct.pack(">HHBBH", 1, 0x0800, 6, 4, 2) + self.MAC + f[38:42] + f[22:28] + f[28:32]
                self.send(src_mac + self.MAC + b"\x08\x06" + reply)
            return
        if ethertype != 0x0800 or f[23] != 17:
            return
        ihl = (f[14] & 15) * 4
        udp = f[14 + ihl:]
        sport, dport = struct.unpack(">HH", udp[:4])
        data = udp[8:]
        dst_ip = ".".join(str(b) for b in f[30:34])
        if dport == 67:
            self.dhcp(src_mac, data)
        elif dport == 123 and dst_ip in self.servers and len(data) >= 48:
            self.ntp_hits.append((dst_ip, sport))
            now = time.time() + self.servers[dst_ip]
            p = bytearray(48)
            p[0], p[1] = 0x24, 2
            p[24:32] = data[40:48]
            p[32:40] = p[40:48] = ts_bytes(now)
            self.send(self.ip_frame(src_mac, dst_ip, ".".join(str(b) for b in f[26:30]), 123, sport, bytes(p)))

    def dhcp(self, src_mac, req):
        if len(req) < 241 or req[0] != 1:
            return
        mtype, wanted, i = 0, [], 240
        while i < len(req) and req[i] != 255:
            code = req[i]
            if code == 0:
                i += 1
                continue
            n = req[i + 1]
            if code == 53:
                mtype = req[i + 2]
            elif code == 55:
                wanted = list(req[i + 2:i + 2 + n])
            i += 2 + n
        self.requested.append(wanted)
        if mtype not in (1, 3):
            return
        opts = bytes([53, 1, 2 if mtype == 1 else 5]) + bytes([54, 4]) + _aton(self.ROUTER) + bytes([51, 4]) + struct.pack(">I", 3600)
        opts += bytes([1, 4, 255, 255, 255, 0]) + bytes([3, 4]) + _aton(self.ROUTER) + bytes([6, 4]) + _aton(self.ROUTER)
        opts += bytes([42, 4 * len(self.servers)]) + b"".join(_aton(ip) for ip in self.servers) + b"\xff"
        bootp = bytes([2, 1, 6, 0]) + req[4:8] + b"\0\0" + req[10:12] + b"\0" * 4 + _aton("10.9.0.15") + _aton(self.ROUTER) + b"\0" * 4
        bootp += req[28:44] + b"\0" * 192 + b"\x63\x82\x53\x63" + opts
        self.send(self.ip_frame(b"\xff" * 6, self.ROUTER, "255.255.255.255", 67, 68, bootp))


@test
def ntp_dhcp_option_42():
    """a lease that names two NTP servers (option 42, on a network made by the test): the client asked for the option, uses the servers, cross-checks them on port 123, and the command line's server is not asked at all"""
    net = MiniNet({"10.9.0.1": 3600, "10.9.0.2": 3600})
    cmdline = "ntp=10.9.0.99:123 " + NOWHERE
    extra = ["-netdev", f"stream,id=n0,addr.type=inet,addr.host=127.0.0.1,addr.port={net.port},server=off", "-device", "virtio-net-pci,netdev=n0"]
    vm = Qemu(cmdline=("text user=root " + cmdline).strip(), name="t-ntp-dhcp42", net=False, extra=extra)
    try:
        vm.start()
        deadline = time.time() + scaled(300)
        while not (re.search(ROOT_PROMPT, vm.text()) and re.search(r"\[Yukari\] eth0: lease 10\.9\.0\.15/255\.255\.255\.0 from 10\.9\.0\.1", vm.text())):
            assert vm.alive() and time.time() < deadline, "the guest never showed a prompt and a lease\n" + vm.diagnose()
            time.sleep(0.2)
        vm._pos = len(vm.text())
        g = Guest.__new__(Guest)
        g.vm = vm
        assert net.requested and all(42 in w for w in net.requested), net.requested                         # every DISCOVER and REQUEST asked for option 42
        m = g.wait_log(r"\[Sakuya\] clock set from 10\.9\.0\.(\d): \+(\d+)\.\d{3} s \(round trip \d+ ms, stratum 2; 10\.9\.0\.(\d) agrees within \d+ ms\)", timeout=240)
        assert {m.group(1), m.group(3)} == {"1", "2"} and 3540 <= int(m.group(2)) <= 3690, m.group(0)       # no ":port": the standard port 123
        st = g.ntp()
        assert st["source"] == "dhcp" and st["dhcp_servers"] == "2" and st["ntp"] == "synced", st
        assert st["server"] in ("10.9.0.1", "10.9.0.2"), st
        assert {ip for ip, _ in net.ntp_hits} == {"10.9.0.1", "10.9.0.2"}, net.ntp_hits
        assert near(g.now(), 3600), g.now() - host_now()
        assert not re.search(r"\[Sakuya\][^\n]*10\.9\.0\.99", g.log())                                      # the command line's server never came into it (the lease's servers answered)
    finally:
        vm.stop()
        net.stop()
