"""IPv6 in the programs (docs/NETWORK.md, "Names and families in the programs"): names with AAAA and A records, IPv6 literals and the fallback
from a broken family to the other, seen through fetch, https_get (libhttp), sntp and ssh, against Python servers on the host.

QEMU's user-mode network: the guest reaches the host's ::1 as fec0::2 and the host's 127.0.0.1 as 10.0.2.2, so one port number on both loopbacks
serves both families, and each server answers with the family it was reached over. The pure parts (interleaving, staggered attempts, URL
literals) are tests/host/test_netaddr.py."""
import re
import socket
import socketserver
import struct
import threading
import time

from harness import test, scaled
from qemu import free_port
from case_net import Guest, _name, _Server
from case_net6 import Server6, UdpServer6, serve6, wait_ready

AAAA_OF = {"dual.gensokyo": ["fec0::2"], "only6.gensokyo": ["fec0::2"], "broken6.gensokyo": ["2001:db8::99"]}
A_OF = {"dual.gensokyo": ["10.0.2.2"], "broken6.gensokyo": ["10.0.2.2"]}


def dns_reply(query):
    ident, flags, qd = struct.unpack(">HHH", query[:6])
    name, pos = _name(query, 12)
    qtype, qclass = struct.unpack(">HH", query[pos:pos + 4])
    question = query[12:pos + 4]
    answers, rcode = [], 0

    def rr(rtype, rdata):
        return b"\xc0\x0c" + struct.pack(">HHIH", rtype, 1, 60, len(rdata)) + rdata

    if name in AAAA_OF or name in A_OF:
        if qtype == 28:
            answers += [rr(28, socket.inet_pton(socket.AF_INET6, a)) for a in AAAA_OF.get(name, [])]
        elif qtype == 1:
            answers += [rr(1, socket.inet_aton(a)) for a in A_OF.get(name, [])]
    else:
        rcode = 3
    return struct.pack(">HHHHHH", ident, 0x8180 | rcode, 1, len(answers), 0, 0) + question + b"".join(answers)


class _Dns(socketserver.BaseRequestHandler):
    def handle(self):
        data, sock = self.request
        try:
            sock.sendto(dns_reply(data), self.client_address)
        except Exception:
            pass


def family_handler(tag):
    class H(socketserver.StreamRequestHandler):
        def handle(self):
            head = b""
            while not head.endswith(b"\r\n\r\n"):
                line = self.rfile.readline()
                if not line:
                    return
                head += line
            host = re.search(rb"\r\nHost: ([^\r]*)", head)
            body = f"family={tag} host={host.group(1).decode() if host else '-'}\n".encode()
            self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\nConnection: close\r\n\r\n" % len(body) + body)
    return H


class _Ntp(socketserver.BaseRequestHandler):
    def handle(self):
        data, sock = self.request
        if len(data) < 48:
            return
        now = time.time()
        reply = bytearray(48)
        reply[0], reply[1] = 0x24, 2
        reply[24:32] = data[40:48]
        for off in (32, 40):
            reply[off:off + 8] = struct.pack(">II", int(now) + 2208988800, int((now - int(now)) * (1 << 32)))
        sock.sendto(bytes(reply), self.client_address)


class _SshBanner(socketserver.BaseRequestHandler):
    seen = []

    def handle(self):
        _SshBanner.seen.append(self.client_address[0])
        self.request.sendall(b"SSH-2.0-net6apps\r\n")
        time.sleep(0.5)


@test
def net6apps_names_literals_fallback():
    """names with AAAA and A records and IPv6 literals through fetch, https_get (libhttp), sntp and ssh; an AAAA address that never answers gives way to IPv4 within 3 s"""
    dns, dport = serve6(UdpServer6, _Dns)
    port = free_port()
    h6, _ = serve6(Server6, family_handler(6), port)
    h4 = _Server(("127.0.0.1", port), family_handler(4))
    threading.Thread(target=h4.serve_forever, daemon=True).start()
    ntp, nport = serve6(UdpServer6, _Ntp)
    ssh, sport = serve6(Server6, _SshBanner)
    try:
        with Guest("t-net6-apps", net="e1000", root=True) as g:
            wait_ready(g)
            g.run(f"printf 'nameserver fec0::2#{dport}\\nsearch gensokyo\\n' > /etc/resolv.conf")
            # libhttp: an IPv6 literal (brackets in the Host header) and a name with an AAAA record only
            out, rc = g.run(f"https_get -q http://[fec0::2]:{port}/; https_get -q http://only6.gensokyo:{port}/", timeout=240)
            assert f"family=6 host=[fec0::2]:{port}" in out and f"family=6 host=only6.gensokyo:{port}" in out, out
            # both families, no global IPv6 address (fec0::/10 has precedence 1): IPv4 first, by RFC 6724
            out, rc = g.run(f"fetch -q http://dual.gensokyo:{port}/; https_get -q http://dual.gensokyo:{port}/", timeout=240)
            assert out.count("family=4") == 2, out
            # an address of 2001:db8::/64 of our own makes 2001:db8::99 the first choice; nobody answers for it on the link, so IPv4 must win
            g.run("ip -6 addr add 2001:db8::5/64 dev eth0; sleep 3", timeout=120)
            out, rc = g.run("nettest6 gai broken6.gensokyo", timeout=120)
            assert re.findall(r"^GAI (\S+) (\S+)$", out, re.M) == [("inet6", "2001:db8::99"), ("inet", "10.0.2.2")], out
            for prog in ("fetch -q", "https_get -q"):
                out, rc = g.run(f"t0=$(date +%s); {prog} http://broken6.gensokyo:{port}/; t1=$(date +%s); echo dt=$((t1-t0))", timeout=240)
                m = re.search(r"dt=(\d+)", out)
                assert "family=4" in out and m and int(m.group(1)) <= 3, (prog, out)
            # sntp: a literal over IPv6, with and without brackets
            out, rc = g.run(f"sntp -p {nport} fec0::2; sntp -p {nport} [fec0::2]", timeout=240)
            assert out.count("server fec0::2, stratum 2") == 2, out
            # ssh: a literal and -6 reach the host over IPv6 (the "server" only sends a banner, so the login itself fails)
            out, rc = g.run(f"ssh -6 -o BatchMode=yes -o StrictHostKeyChecking=no -p {sport} kappa@fec0::2 true; echo rc=$?; ssh -o BatchMode=yes -p {sport} kappa@[fec0::2] true; echo rc=$?", timeout=240)
            assert "Could not resolve" not in out and "connect to host" not in out and "not available" not in out, out
            assert len(_SshBanner.seen) >= 2, (out, _SshBanner.seen)
    finally:
        for srv in (dns, h6, h4, ntp, ssh):
            srv.shutdown()


class _Burst(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.sendall(bytes(range(256)) * 400)                 # 100 KB, then the FIN
        self.request.shutdown(socket.SHUT_WR)
        time.sleep(scaled(15))


@test
def net6apps_no_repeated_acks_after_fin():
    """a reader that drains a full receive ring after the peer's FIN sends no train of identical bare ACKs (one per read() used to leave, because the
    window-update rule compared with the 128 KiB ring while a connection without window scaling can advertise 64 KiB); captured with TOUHOU_PCAP"""
    import os
    import pcapdump
    from harness import BUILD
    srv = _Server(("127.0.0.1", 0), _Burst)
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    pcap = BUILD / "t-net6apps-acks.pcap"
    pcap.unlink(missing_ok=True)
    before = os.environ.get("TOUHOU_PCAP")
    os.environ["TOUHOU_PCAP"] = str(pcap)
    try:
        with Guest("t-net6apps-acks", net="e1000", root=True) as g:
            out, rc = g.run(f"nc -w 30 10.0.2.2 {port} | (sleep 4; wc -c)", timeout=240)     # the pipe stalls nc: the data and the FIN wait in the ring, then go in pieces
            assert "102400" in out, out
    finally:
        srv.shutdown()
        if before is None:
            os.environ.pop("TOUHOU_PCAP", None)
        else:
            os.environ["TOUHOU_PCAP"] = before
    tcp = pcapdump.Tcp(absolute=True)
    lines = [pcapdump.describe(data, tcp)[2] for _, data in pcapdump.frames(pcap)]
    mine = [x for x in lines if x.startswith("TCP   10.0.2.15:") and f"> 10.0.2.2:{port} " in x]
    repeats = sum(1 for a, b in zip(mine, mine[1:]) if a == b and a.endswith("len=0"))
    assert mine and repeats <= 1, f"{repeats} repeated bare ACKs:\n" + "\n".join(mine[-30:])
