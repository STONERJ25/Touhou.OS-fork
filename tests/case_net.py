"""Networking: the Yukari stack (loopback TCP/UDP/ICMP, sockets, robustness), the NIC drivers, DHCP, the resolver and the tools.

The guest clock can run several times slower than wall time, so nothing here sleeps for a fixed time and then expects an
effect: every wait is for serial output with a generous timeout, and the guest-side checks (user/bin/nettest.c) retry.

QEMU's user-mode networking maps the host's loopback to 10.0.2.2, so the "internet" of these tests is a handful of Python
servers on 127.0.0.1 (an echo server, an HTTP server, a DNS responder, an NTP responder) and the guest talks to 10.0.2.2."""
import hashlib
import http.server
import re
import socket
import socketserver
import struct
import threading
import time
import urllib.error
import urllib.request

import test_shell
from harness import test, TESTS, Qemu, BUILD, scaled
from qemu import free_port

USER_PROMPT = test_shell.PROMPT
ROOT_PROMPT = r"root@gensokyo \S+ # "
LEASE = r"\[Yukari\] eth0: lease 10\.0\.2\.15/255\.255\.255\.0 from 10\.0\.2\.2, gateway 10\.0\.2\.2, 1 name server"


class Guest:
    """A booted text-console guest with a network card; run() types a command and returns (output, exit status)."""

    def __init__(self, name, net=None, root=False, hostfwd=(), cmdline="", wait_lease=True):
        self.prompt = ROOT_PROMPT if root else USER_PROMPT
        cl = ("text " + cmdline + (" user=root" if root else "")).strip()
        self.vm = Qemu(cmdline=cl, name=name, net=net, hostfwd=hostfwd)
        self.net = net
        self.wait_lease = wait_lease

    def __enter__(self):
        self.vm.start()
        # the shell prompt and (with a NIC) the DHCP lease line arrive in either order: wait for both, then move past them
        patterns = [self.prompt] + ([LEASE] if self.net and self.wait_lease else [])
        deadline = time.time() + scaled(240)
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
        return test_shell.run(self.vm, cmd, timeout=timeout, prompt=self.prompt)

    def out(self, cmd, timeout=120):
        return self.run(cmd, timeout)[0]


def nettest(name="", timeout=240, root=False, setup=()):
    """Boots a text console, runs the in-guest nettest program (optionally one check) and returns its output."""
    with Guest("t-net-" + (name or "all"), root=root) as g:
        for line in setup:
            g.run(line, timeout=30)
        out, rc = g.run("nettest " + name, timeout=timeout)
        assert "NETTEST PASS" in out and rc == 0, f"nettest failed (exit {rc}):\n{out[-1800:]}"
        return out


# ------------------------------------------------------------------------------------------------------------ loopback

@test
def net_loopback():
    """loopback with no NIC: UDP (fragments, refusals), TCP echo, 5 MB bulk transfer, refused, non-blocking, close, half-close, many sockets, options, ping"""
    with Guest("t-net-all") as g:
        out, rc = g.run("nettest", timeout=360)
        assert "NETTEST PASS" in out and rc == 0, f"nettest failed (exit {rc}):\n{out[-1800:]}"
        for check in ("udp", "tcp", "bulk", "refused", "nonblock", "close", "half", "many", "opts", "ping", "backlog", "bcast", "stall"):
            assert re.search(rf"ok\s+{check}\b", out), f"{check} did not pass:\n{out[-1500:]}"
        assert re.search(r"bulk: 5242880 bytes, checksum [0-9a-f]{8} ok", out), out[-800:]
        # no packet buffer may stay behind once the traffic is over (they would show here as a count that never goes down)
        buffers = None
        for _ in range(20):
            tune = g.out("cat /proc/net/tune")
            buffers = int(re.search(r"buffers (\d+)", tune).group(1))
            if buffers <= 4:
                break
            time.sleep(1)
        assert buffers <= 4, f"{buffers} packet buffers are still in use after the tests:\n{tune}"


@test
def net_robust():
    """3000 malformed frames (truncated, bad checksums, absurd lengths, odd fragments, bogus TCP options and flags) through /proc/net/inject; the stack keeps working"""
    with Guest("t-net-robust", root=True) as g:
        out, rc = g.run("nettest fuzz", timeout=400)
        assert "NETTEST PASS" in out and re.search(r"ok\s+fuzz", out) and rc == 0, (rc, out[-1200:])
        tune = g.out("cat /proc/net/tune")
        ip_bad = int(re.search(r"ip_bad (\d+)", tune).group(1))
        tcp_bad = int(re.search(r"tcp_bad (\d+)", tune).group(1))
        assert ip_bad > 500 and tcp_bad > 0, tune                 # the garbage really reached the parsers and was rejected
        out, rc = g.run("echo alive; ping -c 1 127.0.0.1")
        assert "alive" in out and "1 received" in out, out


@test
def net_loss():
    """a 400 KB TCP transfer over a loopback that drops 5%, duplicates 3% and reorders 5% of the frames arrives intact (retransmission, out-of-order data)"""
    with Guest("t-net-loss", root=True) as g:
        for knob in ("rto_min_ms 40", "lo_drop 5", "lo_dup 3", "lo_reorder 5"):
            g.run(f"echo '{knob}' > /proc/net/tune")
        out, rc = g.run("nettest loss", timeout=600)
        assert "NETTEST PASS" in out and re.search(r"loss: 409600 bytes intact", out) and rc == 0, (rc, out[-1200:])
        dev = g.out("grep 'lo:' /proc/net/dev")
        fields = dev.split("lo:")[1].split()
        tx_packets, tx_dropped = int(fields[9]), int(fields[11])
        assert tx_packets > 200 and tx_dropped > 5, dev            # frames really were thrown away on the way


@test
def net_names_local():
    """no NIC: /etc/hosts answers for getaddrinfo (ping localhost), ifconfig and ip show lo, route and netstat read /proc/net"""
    with Guest("t-net-local") as g:
        out, rc = g.run("ping -c 2 localhost")
        assert rc == 0 and "2 received" in out and "127.0.0.1" in out, out
        out, rc = g.run("ifconfig; ip addr; route -n; netstat -a")
        assert "127.0.0.1  netmask 255.0.0.0" in out and "inet 127.0.0.1/8" in out and "Kernel IP routing table" in out and "Active Internet" in out, out
        out, rc = g.run("nslookup localhost; echo rc=$?")
        assert "no name server is configured" in out, out                  # no DHCP without a NIC: nothing to ask
        out, rc = g.run("ping -c 1 no.such.host.invalid")
        assert rc != 0, out
        out, rc = g.run("ls /proc/net")
        assert all(name in out.split() for name in ("dev", "route", "arp", "tcp", "udp", "tune", "inject")), out
        # nc on both ends over the loopback: a listener writes what it receives into a file (the client retries until it is up)
        g.run("nc -l -w 3 -p 9100 < /dev/null > /tmp/nc.out &")             # stdin must not be the console: a background job would steal the keys
        out, rc = g.run("for i in 1 2 3 4 5 6 7 8 9 10; do echo hello-through-nc | nc -w 5 127.0.0.1 9100 && break; sleep 1; done; wait; cat /tmp/nc.out", timeout=180)
        assert "hello-through-nc" in out, out
        out, rc = g.run("nc -u -l -w 3 -p 9101 < /dev/null > /tmp/ncu.out & sleep 1; echo udp-through-nc | nc -u -w 2 127.0.0.1 9101; wait; cat /tmp/ncu.out", timeout=180)
        assert "udp-through-nc" in out, out


@test
def net_nonet_switch():
    """nonet on the command line: a card that QEMU provides is left alone (one log line, no eth0, no DHCP) and the loopback still works"""
    with Guest("t-net-nonet", net="e1000", cmdline="nonet", wait_lease=False) as g:
        text = g.vm.text()
        assert "nonet: the network cards are left alone" in text, text[-1500:]
        assert "eth0" not in text, "a card was claimed although nonet was given:\n" + text[-1500:]
        out, rc = g.run("cat /proc/net/dev; ping -c 1 127.0.0.1")
        assert "eth0" not in out and "1 received" in out, out


@test
def net_e1000e_transmit_variants():
    """The PCH-style card's transmit variants: e1000.txvar=N starts at variant N (queue switched off first, MAC reset, no errata bits, ring flush, PHY reset, D3 cycle, receive stopped) and DHCP still works; e1000.txstall=2 swallows the doorbell like a hung card, the watchdog tries variant 1 and then 2, and the lease arrives; with txstall=7 the whole ladder is walked."""
    for var in (1, 2, 3, 4, 5, 6, 7):
        with Guest(f"t-net-txvar{var}", net="e1000e", cmdline=f"e1000.pch e1000.txvar={var}") as g:
            text = g.vm.text()
            assert f"transmit variant {var}:" in text, text[-1500:]
            out, rc = g.run("ifconfig eth0; ping -c 1 10.0.2.2")
            assert "10.0.2.15" in out and "1 received" in out, out
    with Guest("t-net-txstall", net="e1000e", cmdline="e1000.pch e1000.txstall=2") as g:
        text = g.vm.text()
        assert "transmit variant 1:" in text and "transmit variant 2:" in text and "transmit works with variant 2" in text, text[-2500:]
        assert "the transmit queue stopped moving" in text and "GPTC" in text, "the register dump should be in the log: " + text[-2500:]
        assert "as the firmware left it" in text and "FEXTNVM6" in text and "LNKCTL" in text, "the full dump should have the power and link registers: " + text[-2500:]
    with Guest("t-net-txladder", net="e1000e", cmdline="e1000.pch e1000.txstall=7") as g:
        text = g.vm.text()
        for v in range(1, 8):
            assert f"transmit variant {v}:" in text, f"variant {v} was not tried:\n" + text[-3000:]
        assert "transmit works with variant 7" in text and "descriptor ring flush" in text, text[-3000:]


def wait_text(g, pattern, timeout=60):
    """The guest's serial text once it matches the pattern (a line and the lease it explains can arrive in either order)."""
    deadline = time.time() + scaled(timeout)
    while True:
        text = g.vm.text()
        if re.search(pattern, text):
            return text
        assert time.time() < deadline, f"never saw {pattern!r}:\n" + text[-3000:]
        time.sleep(0.5)


@test
def net_e1000e_receive_ladder():
    """A card whose receive side is dead or deceived (the Dell's I219 in the third and fourth hardware rounds): e1000.rxstall=N leaves the receive ring without descriptors
    until step N of the receive ladder, e1000.rxgarbage=N makes every received frame arrive blank (a completed descriptor over a buffer of zeros) until step N; the watchdog
    takes a step every few seconds (steps 1 and 2 reset the MAC, the transmit ladder's variants 2 and 4), the log says what it saw and which step brought a frame with
    contents in, and the lease arrives; a card that is reset at probe says so, and e1000.noreset leaves that out"""
    with Guest("t-net-rxstall3", net="e1000e", cmdline="e1000.pch e1000.rxstall=3") as g:
        text = wait_text(g, r"receive works with step 3")
        for step in (1, 2, 3):
            assert f"receive step {step}:" in text, f"step {step} was not taken:\n" + text[-3000:]
        assert "transmit variant 2:" in text and "transmit variant 4:" in text, "steps 1 and 2 reset the MAC:\n" + text[-3000:]
        assert "receive step 4:" not in text and "receive works with step 3" in text, text[-3000:]
        assert "the MAC was reset before its rings were programmed" in text, text[-3000:]
        assert "no frame with contents has reached memory for" in text and "receive FIFO head" in text and ": ring: " in text, "the state of the receive side should be in the log:\n" + text[-3000:]
        out, rc = g.run("ifconfig eth0; ping -c 1 10.0.2.2")
        assert "10.0.2.15" in out and "1 received" in out, out
    with Guest("t-net-rxgarbage3", net="e1000e", cmdline="e1000.pch e1000.rxgarbage=3") as g:
        text = wait_text(g, r"receive works with step 3")
        assert "a frame arrived with a blank Ethernet header" in text and "receive step 1:" in text and "receive step 2:" in text, text[-3000:]
        assert re.search(r"; \d+ blank\n", text) or " blank" in text, text[-3000:]
        out, rc = g.run("ifconfig eth0; ping -c 1 10.0.2.2")
        assert "10.0.2.15" in out and "1 received" in out, out
    with Guest("t-net-rxstall9", net="e1000e", cmdline="e1000.pch e1000.noreset e1000.rxstall=9", wait_lease=False) as g:      # (nine steps: the lease comes a minute of guest time later)
        text = wait_text(g, r"receive works with step 9", timeout=900)
        for step in range(1, 10):
            assert f"receive step {step}:" in text, f"step {step} was not taken:\n" + text[-3500:]
        assert "the MAC was reset before its rings were programmed" not in text, "e1000.noreset must leave the probe-time reset out"
        assert "transmit variant 6:" in text and "receive works with step 9" in text, text[-3500:]
        out, rc = g.run("ifconfig eth0; ping -c 1 10.0.2.2")
        assert "10.0.2.15" in out and "1 received" in out, out


@test
def net_loader_netdiag():
    """'netdiag' on the command line: the loader uses the firmware's own network driver (SNP) before the kernel starts, sends a DHCP DISCOVER, gets the
    OFFER, shuts the adapter down again, and the kernel log carries the report ([Hakurei] netdiag: ...); the kernel's own driver then still gets a lease."""
    text = ""
    for attempt in range(3):          # now and then (about one start in twenty) QEMU's user-mode network answers nobody, firmware and kernel alike: start again then
        try:
            with Guest("t-net-netdiag", net="virtio-net-pci", cmdline="netdiag") as g:
                text = g.vm.text()
        except TimeoutError:
            if attempt == 2:
                raise
            continue
        assert "[Hakurei] netdiag: the firmware's own network driver" in text, text[:3000]
        assert "no EFI_SIMPLE_NETWORK_PROTOCOL" not in text, "the firmware of the tests has no network driver; use an OVMF build with VirtioNetDxe:\n" + text[:2500]
        assert "[Hakurei] netdiag: adapter shut down" in text, text[:4000]
        assert "the firmware's driver transmitted" in text, "the firmware's driver never completed a transmit:\n" + text[:4000]
        if "the network answered" in text:
            break
    assert "[Hakurei] netdiag: result: the firmware's driver transmitted, and the network answered" in text, text[:4000]
    assert "[Hakurei] netdiag: DHCP OFFER from 10.0.2.2 offering 10.0.2.15" in text, text[:4000]


# ------------------------------------------------------------------------------------------------------------ the cards

def nic_case(model, label, driver, cmdline=""):
    def run():
        with Guest("t-net-" + label, net=model, cmdline=cmdline) as g:
            out, rc = g.run("ifconfig eth0")
            assert re.search(r"\(Ethernet\)  " + driver + "\n", out), out
            assert "inet 10.0.2.15  netmask 255.255.255.0  broadcast 10.0.2.255" in out and "UP,BROADCAST,RUNNING" in out, out
            out, rc = g.run("ip route")
            assert "default via 10.0.2.2 dev eth0" in out and "10.0.2.0/24 dev eth0" in out, out
            out, rc = g.run("cat /etc/resolv.conf")
            assert "nameserver 10.0.2.3" in out, out
            out, rc = g.run("ping -c 3 10.0.2.2")
            assert rc == 0 and "3 packets transmitted, 3 received" in out, out
            out, rc = g.run("ip neigh")
            assert "10.0.2.2 dev eth0 lladdr" in out, out
            out, rc = g.run("cat /proc/net/dev | grep eth0")
            assert re.search(r"eth0: \d+ [1-9]\d* ", out), out
    run.__name__ = f"net_nic_{label}"
    run.__doc__ = f"{model}: found, DHCP lease 10.0.2.15 with gateway and DNS, the gateway answers ping, ARP cache and /proc/net/dev fill in"
    TESTS.append(run)


nic_case("virtio-net-pci", "virtio", r"virtio-net\(l\)")
nic_case("virtio-net-pci,disable-legacy=on", "virtio_modern", "virtio-net")
nic_case("e1000", "e1000", "e1000")
nic_case("e1000-82545em", "e1000_82545em", "e1000")
nic_case("e1000e", "e1000e", "e1000e")
nic_case("rtl8139", "rtl8139", "rtl8139")
nic_case("e1000e", "e1000e_pch", r"e1000e\(pch\)", cmdline="e1000.pch")      # QEMU's 82574L driven down the PCH code path (ME, PHY semaphore, errata)


# ------------------------------------------------------------------------------------------------------------ host-side servers

class _Echo(socketserver.BaseRequestHandler):
    def handle(self):
        data = b""
        self.request.settimeout(60)
        while True:
            chunk = self.request.recv(4096)
            if not chunk:
                break
            data += chunk
        self.request.sendall(b"echo:" + data)


class _Server(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True


BIG = bytes((i * 31 + (i >> 8)) & 255 for i in range(200 * 1024))
CHUNKED_PARTS = [b"first chunk, ", b"second chunk, ", b"and the last one\n"]


class _Http(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _send(self, code, body, headers=()):
        self.send_response(code)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/hello.txt":
            self._send(200, b"hello from the other side of the boundary\n", [("Content-Type", "text/plain")])
        elif self.path == "/redir":
            self._send(302, b"", [("Location", "/redir2")])
        elif self.path == "/redir2":
            self._send(301, b"", [("Location", "http://%s/hello.txt" % self.headers["Host"])])
        elif self.path == "/big":
            self._send(200, BIG, [("Content-Type", "application/octet-stream")])
        elif self.path == "/chunked":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Connection", "close")
            self.end_headers()
            for part in CHUNKED_PARTS:
                self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
            self.wfile.write(b"0\r\n\r\n")
        else:
            self._send(404, b"nothing here\n")


def serve(server_cls, handler, port=None):
    port = port or free_port()
    srv = server_cls(("127.0.0.1", port), handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, port


@test
def net_tcp_host():
    """TCP through the NIC to a Python echo server on the host (10.0.2.2): nc sends, half-closes, reads the answer"""
    srv, port = serve(_Server, _Echo)
    try:
        with Guest("t-net-tcphost", net="e1000") as g:
            out, rc = g.run(f"echo konnichiwa | nc -w 20 10.0.2.2 {port}", timeout=180)
            assert "echo:konnichiwa" in out and rc == 0, (out, rc)
            out, rc = g.run(f"nc -z -v 10.0.2.2 {port}; echo rc=$?")
            assert "succeeded" in out and "rc=0" in out, out
            out, rc = g.run("nc -z -v -w 3 10.0.2.2 1; echo rc=$?", timeout=180)
            assert "rc=1" in out, out                                  # the host refuses
            out, rc = g.run("nc -z -v -w 60 10.0.2.77 80; echo rc=$?", timeout=240)
            assert "No route to host" in out and "rc=1" in out, out    # nobody answers ARP for 10.0.2.77: EHOSTUNREACH after about three seconds
    finally:
        srv.shutdown()


@test
def net_fetch_host():
    """fetch and wget get files from a Python HTTP server on the host: redirects, chunked bodies, 200 KB with a checksum, a 404"""
    srv, port = serve(_Server, _Http)
    base = f"http://10.0.2.2:{port}"
    try:
        with Guest("t-net-fetch", net="virtio-net-pci") as g:
            out, rc = g.run(f"fetch -q {base}/hello.txt", timeout=180)
            assert out.strip() == "hello from the other side of the boundary" and rc == 0, (out, rc)
            out, rc = g.run(f"fetch -q {base}/redir")
            assert "hello from the other side" in out and rc == 0, (out, rc)
            out, rc = g.run(f"fetch -q {base}/chunked")
            assert b"".join(CHUNKED_PARTS).decode() in out, repr(out)
            out, rc = g.run(f"fetch -q -O /tmp/big {base}/big; md5sum /tmp/big", timeout=300)
            assert hashlib.md5(BIG).hexdigest() in out, out
            out, rc = g.run(f"cd /tmp && wget {base}/hello.txt && cat hello.txt", timeout=180)
            assert "saved" in out and "hello from the other side" in out and rc == 0, (out, rc)
            out, rc = g.run(f"fetch -q {base}/missing; echo rc=$?")
            assert "rc=8" in out, out
            out, rc = g.run(f"fetch -i -q {base}/hello.txt | head -3")
            assert "HTTP/1.0 200" in out or "HTTP/1.1 200" in out or "200 OK" in out, out
    finally:
        srv.shutdown()


@test
def net_httpd_hostfwd():
    """the guest's httpd answers a request that enters through QEMU's hostfwd: file, index, directory listing, 404, HEAD, path traversal refused"""
    host_port = free_port()
    with Guest("t-net-httpd", net="e1000", hostfwd=[f"tcp:127.0.0.1:{host_port}-:8080"]) as g:
        g.run("mkdir -p /tmp/www/sub; echo gensokyo-index > /tmp/www/index.html; echo deep > /tmp/www/sub/deep.txt")
        g.run("httpd -q -p 8080 -d /tmp/www &")

        def get(path, method="GET", tries=40):
            last = None
            for _ in range(tries):
                try:
                    req = urllib.request.Request(f"http://127.0.0.1:{host_port}{path}", method=method)
                    with urllib.request.urlopen(req, timeout=20) as r:
                        return r.status, r.read(), dict(r.headers)
                except urllib.error.HTTPError as e:
                    return e.code, e.read(), dict(e.headers)
                except (OSError, ConnectionError) as e:        # the guest may still be starting httpd or booting slowly
                    last = e
                    time.sleep(1)
            raise AssertionError(f"no answer for {path}: {last}")

        status, body, hdr = get("/")
        assert status == 200 and body == b"gensokyo-index\n" and hdr.get("Content-Type") == "text/html", (status, body, hdr)
        status, body, _ = get("/sub/deep.txt")
        assert status == 200 and body == b"deep\n", (status, body)
        status, body, _ = get("/sub")
        assert status in (200, 301), status                         # urllib follows the redirect to /sub/ and lists the directory
        assert b"deep.txt" in body, body
        status, body, _ = get("/nothing.html")
        assert status == 404, status
        status, body, hdr = get("/index.html", method="HEAD")
        assert status == 200 and body == b"" and hdr.get("Content-Length") == "15", (status, body, hdr)
        status, body, _ = get("/%2e%2e/etc/passwd")
        assert status in (400, 404), status
        out, rc = g.run("netstat -l")
        assert ":8080" in out, out


# ------------------------------------------------------------------------------------------------------------ DNS and NTP

def _name(data, pos):
    labels = []
    while data[pos]:
        n = data[pos]
        labels.append(data[pos + 1:pos + 1 + n].decode())
        pos += n + 1
    return ".".join(labels).lower(), pos + 1


def _enc(name):
    return b"".join(bytes([len(p)]) + p.encode() for p in name.split(".") if p) + b"\0"


def dns_reply(query, a_target="10.0.2.2"):
    ident, flags, qd = struct.unpack(">HHH", query[:6])
    name, pos = _name(query, 12)
    qtype, qclass = struct.unpack(">HH", query[pos:pos + 4])
    question = query[12:pos + 4]
    answers = []
    rcode = 0

    def rr(rtype, rdata, owner=0xC00C):
        o = struct.pack(">H", owner) if isinstance(owner, int) else _enc(owner)
        return o + struct.pack(">HHIH", rtype, 1, 60, len(rdata)) + rdata

    a_rdata = bytes(int(x) for x in a_target.split("."))
    if name.endswith("gensokyo") or name.endswith("in-addr.arpa"):
        if name == "kappa.gensokyo" and qtype in (1, 255):
            answers.append(rr(1, a_rdata))
        elif name == "kappa.gensokyo":
            pass                                                   # no data for other types
        elif name == "www.gensokyo" and qtype in (1, 5, 255):
            answers.append(rr(5, _enc("kappa.gensokyo")))
            if qtype != 5:
                answers.append(rr(1, a_rdata, "kappa.gensokyo"))
        elif name == "gensokyo" and qtype == 15:
            answers.append(rr(15, struct.pack(">H", 10) + _enc("mx.gensokyo")))
        elif name == "gensokyo" and qtype == 16:
            answers.append(rr(16, bytes([14]) + b"hakurei shrine"))
        elif name == "gensokyo" and qtype == 2:
            answers.append(rr(2, _enc("ns.gensokyo")))
        elif name == "2.2.0.10.in-addr.arpa" and qtype == 12:
            answers.append(rr(12, _enc("kappa.gensokyo")))
        elif name in ("gensokyo", "2.2.0.10.in-addr.arpa", "kappa.gensokyo", "www.gensokyo"):
            pass
        else:
            rcode = 3
    else:
        rcode = 3
    header = struct.pack(">HHHHHH", ident, 0x8180 | rcode, 1, len(answers), 0, 0)
    return header + question + b"".join(answers)


class _DnsHandler(socketserver.BaseRequestHandler):
    def handle(self):
        data, sock = self.request
        try:
            sock.sendto(dns_reply(data), self.client_address)
        except Exception:                                          # a malformed query must not kill the responder
            pass


class _UdpServer(socketserver.ThreadingMixIn, socketserver.UDPServer):
    allow_reuse_address = True
    daemon_threads = True


@test
def net_dns_host():
    """resolver against a Python DNS responder on the host: nslookup, host (A, CNAME, MX, TXT, PTR, NXDOMAIN), the search list, getaddrinfo through a CNAME for ping and fetch"""
    dns, dport = serve(_UdpServer, _DnsHandler)
    http_srv, hport = serve(_Server, _Http)
    try:
        with Guest("t-net-dns", net="e1000e", root=True) as g:
            g.run(f"printf 'nameserver 10.0.2.2#{dport}\\nsearch gensokyo\\n' > /etc/resolv.conf")
            out, rc = g.run("nslookup kappa.gensokyo")
            assert f"Server:\t\t10.0.2.2\nAddress:\t10.0.2.2#{dport}" in out and "Name:\tkappa.gensokyo\nAddress: 10.0.2.2" in out and rc == 0, (out, rc)
            out, rc = g.run("host www.gensokyo")
            assert "www.gensokyo is an alias for kappa.gensokyo." in out and "has address 10.0.2.2" in out and rc == 0, (out, rc)
            out, rc = g.run("nslookup -type=MX gensokyo")
            assert "mail exchanger = 10 mx.gensokyo." in out, out
            out, rc = g.run("nslookup -type=TXT gensokyo; host -t NS gensokyo")
            assert 'text = "hakurei shrine"' in out and "name server ns.gensokyo." in out, out
            out, rc = g.run("host 10.0.2.2")
            assert "domain name pointer kappa.gensokyo." in out, out
            out, rc = g.run("nslookup nosuch.gensokyo; echo rc=$?")
            assert "NXDOMAIN" in out and "rc=1" in out, out
            out, rc = g.run("ping -c 1 www.gensokyo", timeout=180)
            assert "PING www.gensokyo (10.0.2.2)" in out and "1 received" in out and rc == 0, (out, rc)
            out, rc = g.run("ping -c 1 kappa", timeout=180)                      # the search list turns it into kappa.gensokyo
            assert "PING kappa (10.0.2.2)" in out and "1 received" in out, out
            out, rc = g.run(f"fetch -q http://www.gensokyo:{hport}/hello.txt", timeout=180)
            assert "hello from the other side" in out and rc == 0, (out, rc)
            out, rc = g.run("nc -z -w 5 no.such.gensokyo 80; echo rc=$?")
            assert "rc=1" in out, out
    finally:
        dns.shutdown()
        http_srv.shutdown()


class _NtpHandler(socketserver.BaseRequestHandler):
    SHIFT = 7200                                                   # the "server" is two hours ahead of the host

    def handle(self):
        data, sock = self.request
        if len(data) < 48:
            return
        now = time.time() + self.SHIFT
        sec = int(now) + 2208988800
        frac = int((now - int(now)) * (1 << 32))
        reply = bytearray(48)
        reply[0] = 0x24                                            # leap 0, version 4, mode 4 (server)
        reply[1] = 2                                               # stratum 2
        reply[24:32] = data[40:48]                                 # origin timestamp = the client's transmit timestamp
        for off in (32, 40):                                       # receive and transmit timestamps
            reply[off:off + 8] = struct.pack(">II", sec, frac)
        sock.sendto(bytes(reply), self.client_address)


@test
def net_sntp_host():
    """sntp reads a Python NTP responder that is two hours ahead; with -s root steps the clock and date agrees; without root the clock is left alone"""
    ntp, port = serve(_UdpServer, _NtpHandler)
    try:
        with Guest("t-net-sntp", net="virtio-net-pci", root=True) as g:
            out, rc = g.run(f"sntp -p {port} 10.0.2.2")
            m = re.search(r"offset \+(\d+)\.(\d+)", out)
            assert m and abs(int(m.group(1)) - 7200) < 30 and rc == 0, (out, rc)
            before = int(g.out("date +%s").strip().splitlines()[-1])
            out, rc = g.run(f"ntpdate -p {port} 10.0.2.2")
            assert "the clock now reads" in out and rc == 0, (out, rc)
            after = int(g.out("date +%s").strip().splitlines()[-1])
            assert 7100 < after - before < 7300, (before, after)
        with Guest("t-net-sntp-user", net="virtio-net-pci") as g:
            out, rc = g.run(f"ntpdate -p {port} 10.0.2.2")
            assert rc == 1 and "only root may" in out, (out, rc)
    finally:
        ntp.shutdown()


@test
def net_ifconfig_set():
    """root changes an address, a netmask, the MTU and the routes with ifconfig, ip and route; the table follows"""
    with Guest("t-net-ifset", net="e1000", root=True) as g:
        out, rc = g.run("ifconfig eth0 192.168.7.9/24 mtu 1400 && ifconfig eth0 && route -n")
        assert "inet 192.168.7.9  netmask 255.255.255.0" in out and "mtu 1400" in out and "192.168.7.0" in out, out
        out, rc = g.run("ip route add default via 192.168.7.1 && ip route")
        assert "default via 192.168.7.1 dev eth0" in out, out
        out, rc = g.run("ip route del default && ip route add 10.9.0.0/16 via 192.168.7.1 && route -n")
        assert "10.9.0.0" in out and "255.255.0.0" in out and "UG" in out, out
        out, rc = g.run("ip addr del 192.168.7.9/24 dev eth0 && ip addr show dev eth0")
        assert "inet 192.168.7" not in out, out
        out, rc = g.run("ip link set eth0 down && ifconfig; ip link set eth0 up && ifconfig eth0 dhcp && sleep 1 && ifconfig eth0", timeout=240)
        assert "eth0" in out, out
        out, rc = g.run("ifconfig eth0 10.0.2.15 netmask 255.255.255.0 up && route add default gw 10.0.2.2 && ping -c 1 10.0.2.2", timeout=180)
        assert "1 received" in out and rc == 0, (out, rc)
    with Guest("t-net-ifset-user", net="e1000") as g:
        out, rc = g.run("ifconfig eth0 10.1.1.1; echo rc=$?")
        assert "rc=1" in out and "Operation not permitted" in out, out


def link_case(model, label):
    def run():
        with Guest("t-net-link-" + label, net=model) as g:
            vm = g.vm
            out, rc = g.run("ifconfig eth0")
            assert "RUNNING" in out, out
            vm.qmp("set_link", name="net0", up=False)                   # the cable is pulled
            deadline = time.time() + scaled(120)
            while "link down" not in vm.text() and time.time() < deadline:
                time.sleep(0.5)
            assert "link down" in vm.text(), vm.text()[-800:]
            out, rc = g.run("ifconfig eth0; ping -c 1 -W 2 10.0.2.2; echo rc=$?", timeout=120)     # nothing hangs, the ping simply fails
            assert "RUNNING" not in out.split("PING")[0] and "rc=1" in out, out
            lease_lines = len(re.findall(LEASE, vm.text()))
            vm.qmp("set_link", name="net0", up=True)                    # and plugged in again: DHCP starts over by itself
            deadline = time.time() + scaled(180)
            while len(re.findall(LEASE, vm.text())) <= lease_lines and time.time() < deadline:
                time.sleep(0.5)
            assert len(re.findall(LEASE, vm.text())) > lease_lines, vm.text()[-1200:]
            out, rc = g.run("ping -c 2 10.0.2.2", timeout=180)
            assert rc == 0 and "2 received" in out, out
    run.__name__ = f"net_link_{label}"
    run.__doc__ = f"{model}: pulling the cable closes the gap without hanging anything; plugging it in again brings a new DHCP lease and a working network"
    TESTS.append(run)


link_case("e1000", "e1000")
link_case("virtio-net-pci", "virtio")
link_case("rtl8139", "rtl8139")


@test
def net_static_config_at_boot():
    """a `static` line in network.conf (kept on the HUMANVILLAGE volume) gives the card its address, default route and name server before the first session: no
    DHCP lease is asked for, the gateway answers a ping, resolv.conf has the server, and `netconf -n` says what a line would do; a `dhcp` line leaves the card to DHCP"""
    from case_storage import Image, Guest as StorageGuest, hv_part, MIB
    conf = b"# the test network of QEMU's user-mode stack, with an address of our own\neth0 static 10.0.2.77/24 gateway 10.0.2.2 dns 10.0.2.3 10.0.2.4\n"
    with Image("net-static", 64 * MIB, [hv_part({"/.system/etc/network.conf": conf})]) as img:
        with Qemu(cmdline="text bootlog=off user=root", disk=img.path, net="e1000", name="t-net-static") as vm:
            g = StorageGuest(vm, True)
            vm.expect(r"root@gensokyo \S+ # ", timeout=120)
            log = vm.text()
            assert "[netconf] eth0: static 10.0.2.77/24, gateway 10.0.2.2, dns 10.0.2.3, 10.0.2.4" in log, log[-1500:]
            out = g.ok("ifconfig eth0; route -n; cat /etc/resolv.conf")
            assert "inet 10.0.2.77  netmask 255.255.255.0" in out and re.search(r"0\.0\.0\.0\s+10\.0\.2\.2\s+0\.0\.0\.0\s+UG", out), out
            assert "nameserver 10.0.2.3" in out and "nameserver 10.0.2.4" in out, out
            out, rc = g.run("ping -c 2 10.0.2.2", timeout=120)
            assert rc == 0 and "2 received" in out, out
            time.sleep(scaled(8))                                         # a DHCP client would have said something by now
            assert "lease 10.0.2.15" not in vm.text(), "the card asked DHCP although it has a static address"
            out = g.ok("netconf -n")
            assert "netconf: would set eth0: static 10.0.2.77/24" in out, out
            g.ok("printf 'eth0 dhcp\n' > /tmp/dhcp.conf; netconf -n /tmp/dhcp.conf")      # a dhcp line asks for nothing to be set
            assert g.ok("netconf -n /tmp/dhcp.conf").strip() == "", "a dhcp line must not be applied"


@test
def net_nicdebug_trace():
    """nicdebug on the command line: the card's driver traces the first frames in both directions (the DHCP exchange with its type and xid), the DHCP client says what it
    sent and saw, and the card's counter lines appear after the link came up (frames sent and received, damaged frames, descriptor error bits)"""
    with Guest("t-net-nicdebug", net="e1000e", cmdline="e1000.pch nicdebug") as g:
        deadline = time.time() + scaled(90)
        log = ""
        while time.time() < deadline:
            log = g.out("dmesg")
            if "counters since link up" in log and "descriptor error bits" in log:
                break
            time.sleep(2)
        assert re.search(r"tx #1 \(\d+ bytes, ring 0/0, TDH 0\): .* IPv4 proto 17 0\.0\.0\.0 > 255\.255\.255\.255 udp 68>67 .* DHCP op 1 type 1 xid [0-9a-f]{8}", log), log[-2500:]
        assert re.search(r"rx #\d+ \(\d+ bytes, status \w+ errors 00\): .* udp 67>68 .* DHCP op 2 type 2 xid [0-9a-f]{8} yiaddr 10\.0\.2\.15", log), log[-2500:]
        assert re.search(r"DHCP DISCOVER sent: xid [0-9a-f]{8}", log) and "DHCP reply seen: " in log, log[-2500:]
        m = re.search(r"counters since link up:([^\n]*)", log)
        assert m and re.search(r"\bgptc [1-9]", m.group(1)) and re.search(r"\bgprc [1-9]", m.group(1)), (m and m.group(1))
        assert re.search(r"driver: \d+ transmit descriptors finished by the card; sent \d+ \(0 errors\)", log), log[-1500:]
        assert "receive step" not in log, "a card that receives must not walk the receive ladder:\n" + log[-2500:]
        assert "the MAC was reset before its rings were programmed" in log and ", 0 blank" in log, log[-2500:]
        assert re.search(r"receive side: RXDCTL \w+ RCTL \w+ RDBAL \w+ RDLEN \d+ RDH \d+ RDT \d+", log) and "receive side: receive FIFO head" in log, log[-2500:]
        assert re.search(r"registers 3 s after link up: regs 2800: \w{8} \w{8} \w{8}", log) and "registers as the firmware left them: regs 3800:" in log, log[-2500:]


@test
def net_wake_on_lan_armed_at_poweroff():
    """poweroff arms the network card to wake the machine for a magic packet (Wake on LAN) and tells the chipset to listen: the PCH code path (QEMU's 82574L with e1000.pch) opens the
    PHY's wake-up page, writes the receive address, the filter control and the wake-up registers, reads them back and puts the card to sleep; the plain path sets the MAC's wake-up
    registers; the lines reach the log before the machine goes, the chipset's side says what it found, and e1000.wol=off leaves it all alone (the machine still powers off)"""
    mac = "52:54:00:12:34:56"
    with Guest("t-net-wol-pch", net="e1000e", cmdline="e1000.pch", root=True) as g:
        log = g.out("dmesg")
        assert re.search(r"\S+: wake-up status at start: WUS [0-9a-f]{8}, PHY status (?:unreadable )?[0-9a-f]{4}, WUC [0-9a-f]{8} WUFC [0-9a-f]{8}, PMCSR [0-9a-f]{4}; cleared", log), log[-2500:]
        g.vm.send("poweroff\n")
        g.vm.expect(rf"eth0: wake-up armed(?: in part)?: magic packets for {mac} in the MAC and the PHY \(WUFC \w{{8}} WUC \w{{8}} CTRL \w{{8}} STATUS \w{{8}}, RCTL was \w{{8}}\); the card sleeps as the last step", timeout=scaled(120))
        g.vm.expect(r"wake-up: (?:event 0x6d|the firmware describes no event block)", timeout=scaled(60))
        assert g.vm.wait_exit(scaled(90)) is not None, "the machine did not power off after arming the card"
        log = g.vm.text()
        assert re.search(r"eth0: wake-up, PHY side (?:written|FAILED)", log) or "eth0: wake-up, PHY side:" in log, log[-2500:]
        assert "OEM bits" in log, log[-2500:]
    with Guest("t-net-wol-mac", net="e1000", root=True) as g:
        g.vm.send("poweroff\n")
        g.vm.expect(rf"eth0: wake-up armed: magic packets for {mac} \(WUFC 00000002 WUC \w{{8}} CTRL \w{{8}} STATUS \w{{8}}, RCTL was \w{{8}}\); the card sleeps as the last step", timeout=scaled(120))
        assert g.vm.wait_exit(scaled(90)) is not None, "the machine did not power off after arming the card"
        assert "PHY side" not in g.vm.text(), "a plain card has no PHY wake-up page"
    with Guest("t-net-wol-off", net="e1000", cmdline="e1000.wol=off", root=True) as g:
        g.vm.send("poweroff\n")
        assert g.vm.wait_exit(scaled(120)) is not None, "the machine did not power off"
        text = g.vm.text()
        assert "wake-up armed" not in text and "wake-up status at the last step" not in text and "the card sleeps" not in text, "e1000.wol=off must leave the card alone:\n" + text[-1500:]
