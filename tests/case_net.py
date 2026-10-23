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
from harness import test, TESTS, Qemu, BUILD
from qemu import free_port

USER_PROMPT = test_shell.PROMPT
ROOT_PROMPT = r"root@gensokyo \S+ # "
LEASE = r"\[Yukari\] eth0: lease 10\.0\.2\.15/255\.255\.255\.0 from 10\.0\.2\.2, gateway 10\.0\.2\.2, 1 name server"


class Guest:
    """A booted text-console guest with a network card; run() types a command and returns (output, exit status)."""

    def __init__(self, name, net=None, root=False, hostfwd=(), cmdline=""):
        self.prompt = ROOT_PROMPT if root else USER_PROMPT
        cl = ("text " + cmdline + (" user=root" if root else "")).strip()
        self.vm = Qemu(cmdline=cl, name=name, net=net, hostfwd=hostfwd)
        self.net = net

    def __enter__(self):
        self.vm.start()
        # the shell prompt and (with a NIC) the DHCP lease line arrive in either order: wait for both, then move past them
        patterns = [self.prompt] + ([LEASE] if self.net else [])
        deadline = time.time() + 240
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
    out = nettest()
    for check in ("udp", "tcp", "bulk", "refused", "nonblock", "close", "half", "many", "opts", "ping"):
        assert re.search(rf"ok\s+{check}\b", out), f"{check} did not pass:\n{out[-1500:]}"
    assert re.search(r"bulk: 5242880 bytes, checksum [0-9a-f]{8} ok", out), out[-800:]


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


# ------------------------------------------------------------------------------------------------------------ the cards

def nic_case(model, label, driver):
    def run():
        with Guest("t-net-" + label, net=model) as g:
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
nic_case("e1000e", "e1000e", "e1000e")
nic_case("rtl8139", "rtl8139", "rtl8139")


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
