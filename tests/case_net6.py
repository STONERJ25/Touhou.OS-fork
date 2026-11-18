"""IPv6: the Yukari stack's IPv6 side, on the loopback (::1, no card) and through QEMU's user-mode network.

QEMU's user-mode network has an IPv6 side (docs/NETWORK.md, "IPv6"): the prefix fec0::/64, the gateway fec0::2 (which is also the host: a connection or datagram to fec0::2
reaches the host's own ::1, so the Python servers of these tests listen on ::1), the name server fec0::3, and router advertisements (the prefix with the on-link and autonomous
flags, a default router with a lifetime of 1800 s, an RDNSS option naming fec0::3) that the guest gets when it asks for them with a router solicitation.

The guest clock can run several times slower than wall time, so nothing here sleeps for a fixed time and then expects an effect: every wait is for output with a generous timeout.
The in-guest checks are user/bin/nettest6.c; the pure parts (address text, extension header walker, reassembly) are tests/host/test_net6.py."""
import re
import socket
import socketserver
import struct
import threading
import time

from harness import test, TESTS, Qemu, scaled
from qemu import free_port
from case_net import Guest, ROOT_PROMPT, LEASE, wait_text, dns_reply, _name, _enc, _Http, _Echo, BIG, CHUNKED_PARTS  # noqa: F401

MAC_TAIL = "5054:ff:fe12:3456"                 # QEMU's default MAC address 52:54:00:12:34:56 as an EUI-64 interface identifier
LINK_LOCAL = f"fe80::{MAC_TAIL}"
GLOBAL = f"fec0::{MAC_TAIL}"                   # the SLAAC address from the prefix of the router advertisement


class Server6(socketserver.ThreadingMixIn, socketserver.TCPServer):
    address_family = socket.AF_INET6
    allow_reuse_address = True
    daemon_threads = True


class UdpServer6(socketserver.ThreadingMixIn, socketserver.UDPServer):
    address_family = socket.AF_INET6
    allow_reuse_address = True
    daemon_threads = True


def serve6(server_cls, handler, port=None):
    """A server on the host's IPv6 loopback, which the guest reaches as fec0::2."""
    port = port or free_port()
    srv = server_cls(("::1", port), handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, port


def wait_ready(g, timeout=90):
    """Waits until the card has its link-local address and the one from the router advertisement, both past duplicate address detection."""
    deadline = time.time() + scaled(timeout)
    while True:
        out = g.out("cat /proc/net/if_inet6")
        mine = [ln for ln in out.splitlines() if ln.endswith("eth0")]
        flags = {ln.split()[0]: ln.split()[4] for ln in mine}
        ll = "fe80000000000000505400fffe123456"
        gl = "fec0000000000000505400fffe123456"
        if ll in flags and gl in flags and not (int(flags[ll], 16) & 0x40) and not (int(flags[gl], 16) & 0x40):
            return out
        assert time.time() < deadline, "the card never got both its addresses:\n" + out + "\n" + g.vm.text()[-1500:]
        time.sleep(1)


# ------------------------------------------------------------------------------------------------------------ loopback, no card

@test
def net6_loopback():
    """loopback with no card: UDP over ::1 (fragments up to 65527 bytes), TCP echo, 2 MB bulk transfer, refused connections, half-close and reset, the dual-stack socket (v4-mapped peers, IPV6_V6ONLY), v4-mapped destinations, options and address errors, an ICMPv6 ping socket; afterwards no packet buffer may be left over"""
    with Guest("t-net6-all") as g:
        def inet_sockets():
            return int(re.search(r"inet_sockets (\d+)", g.out("cat /proc/kstats")).group(1))
        base = inet_sockets()
        out, rc = g.run("nettest6", timeout=360)
        assert "NETTEST6 PASS" in out and rc == 0, f"nettest6 failed (exit {rc}):\n{out[-1800:]}"
        for check in ("udp6", "frag", "tcp6", "bulk6", "refused6", "close6", "dual", "mapped", "opts6", "ping6"):
            assert re.search(rf"ok\s+{check}\b", out), f"{check} did not pass:\n{out[-1500:]}"
        assert re.search(r"bulk6: 2097152 bytes, checksum [0-9a-f]{8} ok", out), out[-800:]
        buffers = None
        for _ in range(20):
            tune = g.out("cat /proc/net/tune")
            buffers = int(re.search(r"buffers (\d+)", tune).group(1))
            if buffers <= 4:
                break
            time.sleep(1)
        assert buffers <= 4, f"{buffers} packet buffers are still in use after the tests:\n{tune}"
        deadline = time.time() + scaled(120)                                      # the closed connections wait in TIME_WAIT for 30 seconds, then every socket object must be gone
        while inet_sockets() > base and time.time() < deadline:
            time.sleep(2)
        assert inet_sockets() <= base, f"{inet_sockets()} socket objects, there were {base} before: some were not freed"


@test
def net6_loopback_tools():
    """no card: ::1 is an address of lo (ifconfig, ip, /proc/net/if_inet6, ipv6_route), `localhost` is in /etc/hosts for both families, ping6 and ping -6 work, nc -6 and httpd -6 listen on :: (tcp6 in /proc/net and netstat), fetch speaks to http://[::1]:PORT/"""
    with Guest("t-net6-tools") as g:
        out, rc = g.run("ifconfig lo; ip -6 addr; ip -6 route; cat /proc/net/if_inet6 /proc/net/ipv6_route")
        assert "inet6 ::1  prefixlen 128  scopeid 0x10<host>" in out, out
        assert "inet6 ::1/128 scope host" in out and "valid_lft forever preferred_lft forever" in out, out
        assert "::1 dev lo proto kernel metric 256" in out, out
        assert re.search(r"^00000000000000000000000000000001 01 80 10 80\s+lo$", out, re.M), out                       # Linux's /proc/net/if_inet6 line for lo (the index is the scope id: 1 here, lo is the only card)
        assert re.search(r"^00000000000000000000000000000001 80 0{32} 00 0{32} 00000100 00000001 00000000 00000005\s+lo$", out, re.M), out
        out, rc = g.run("ping6 -c 2 ::1; ping -6 -c 1 localhost; ping -c 1 localhost")
        assert "2 packets transmitted, 2 received" in out and "PING localhost(::1) 56 data bytes" in out and "PING localhost (127.0.0.1)" in out, out    # ping -6 asks for the IPv6 address, plain ping for the IPv4 one
        out, rc = g.run("route -A inet6 -n; netstat -rn -A inet6; ip -6 neigh; ip -6 addr show dev lo")
        assert "Kernel IPv6 routing table" in out and re.search(r"::1/128\s+::\s+U\s+256", out), out
        out, rc = g.run("nc -6 -l -w 5 -p 9300 > /tmp/nc6.out < /dev/null & sleep 1; netstat -ln | grep 9300; grep -c ':2454' /proc/net/tcp6; "
                        "for i in 1 2 3 4 5 6 7 8 9 10; do echo hello-over-ipv6 | nc -6 -w 5 ::1 9300 && break; sleep 1; done; wait; cat /tmp/nc6.out", timeout=240)
        assert re.search(r"tcp6\s+0\s+0 :::9300\s+:::\*\s+LISTEN", out) and "hello-over-ipv6" in out, out
        # the same listener answers IPv4 clients too (a dual-stack socket); its tcp6 line has the unspecified address
        out, rc = g.run("nc -6 -l -w 8 -p 9301 > /tmp/nc4.out < /dev/null & sleep 1; grep ':2455' /proc/net/tcp6; "
                        "for i in 1 2 3 4 5 6 7 8 9 10; do echo hello-from-ipv4 | nc -w 5 127.0.0.1 9301 && break; sleep 1; done; wait; cat /tmp/nc4.out", timeout=240)
        assert re.search(r"^\s*\d+: 0{32}:2455 0{32}:0000 0A ", out, re.M) and "hello-from-ipv4" in out, out
        g.run("mkdir -p /tmp/www6; echo gensokyo-six > /tmp/www6/index.html")
        g.run("httpd -q -6 -p 8686 -d /tmp/www6 &")
        out, rc = g.run("for i in 1 2 3 4 5 6 7 8 9 10; do fetch -q http://[::1]:8686/ && break; sleep 1; done; fetch -q http://localhost:8686/; fetch -q http://127.0.0.1:8686/index.html", timeout=240)
        assert out.count("gensokyo-six") == 3, out
        out, rc = g.run("netstat -a | grep 8686; ss -l | grep 8686")
        assert re.search(r"tcp6\s+0\s+0 :::8686\s+:::\*\s+LISTEN", out) and "[::]:8686" in out, out


@test
def net6_robust():
    """3000 malformed IPv6 packets (truncated headers, bad lengths, odd extension header chains, fragments, ICMPv6 of every type, UDP and TCP with wrong checksums) through /proc/net/inject: ip6_bad counts what was thrown away, no packet buffer leaks, the stack still works"""
    with Guest("t-net6-robust", root=True) as g:
        out, rc = g.run("nettest6 fuzz6", timeout=400)
        assert "NETTEST6 PASS" in out and re.search(r"ok\s+fuzz6", out) and rc == 0, (rc, out[-1200:])
        tune = g.out("cat /proc/net/tune")
        assert int(re.search(r"ip6_bad (\d+)", tune).group(1)) > 500, tune
        for _ in range(20):
            buffers = int(re.search(r"buffers (\d+)", g.out("cat /proc/net/tune")).group(1))
            if buffers <= 4:
                break
            time.sleep(1)
        assert buffers <= 4, f"{buffers} packet buffers are still in use after the fuzzing"
        out, rc = g.run("echo alive; ping6 -c 1 ::1; ping -c 1 127.0.0.1; grep -E 'RateLimit|OutEchoReplies|InEchos|OutParmProblems' /proc/net/snmp6")
        assert "alive" in out and out.count("1 received") == 2, out                          # (the counters are there for the message when it fails)


@test
def net6_loss():
    """a 300 KB TCP transfer over ::1 on a loopback that drops 5%, duplicates 3% and reorders 5% of the frames arrives intact; fragmented datagrams through the same weather arrive whole or not at all (a duplicate fragment is dropped alone)"""
    with Guest("t-net6-loss", root=True) as g:
        for knob in ("rto_min_ms 40", "lo_drop 5", "lo_dup 3", "lo_reorder 5"):
            g.run(f"echo '{knob}' > /proc/net/tune")
        out, rc = g.run("nettest6 loss6", timeout=600)
        assert "NETTEST6 PASS" in out and re.search(r"loss6: 307200 bytes intact", out) and rc == 0, (rc, out[-1200:])
        assert re.search(r"loss6: \d+ whole datagrams of 40, none damaged", out), out
        dev = g.out("grep 'lo:' /proc/net/dev")
        fields = dev.split("lo:")[1].split()
        assert int(fields[9]) > 200 and int(fields[11]) > 5, dev               # frames really were thrown away on the way


# ------------------------------------------------------------------------------------------------------------ a card, QEMU's router advertisements

def check_card(g, label):
    """What a card must have once its link-local address and the one from the router advertisement are past duplicate address detection."""
    out = wait_ready(g)
    log = g.out("dmesg")                                                           # (what happens to the addresses is in the kernel log, not on the console: it would break into whatever runs there)
    assert f"{label}: IPv6 address {LINK_LOCAL}/64 is ready" in log, log[-2500:]
    assert f"{label}: IPv6 address {GLOBAL}/64 was made from a router advertisement (tentative)" in log, log[-2500:]
    assert f"{label}: IPv6 address {GLOBAL}/64 is ready" in log, log[-2500:]
    assert re.search(r"^fe80000000000000505400fffe123456 0[12] 40 20 80\s+eth0$", out, re.M), out                          # link-local: scope 0x20, permanent
    assert re.search(r"^fec0000000000000505400fffe123456 0[12] 40 40 00\s+eth0$", out, re.M), out                          # site scope 0x40 (fec0::/10), no flags: preferred, from the advertisement
    out, rc = g.run("ifconfig eth0; ip -6 addr show dev eth0; ip -6 route; ip -6 neigh; cat /proc/net/ndp")
    assert f"inet6 {LINK_LOCAL}  prefixlen 64  scopeid 0x20<link>" in out and f"inet6 {GLOBAL}  prefixlen 64  scopeid 0x40<site>" in out, out
    assert f"inet6 {LINK_LOCAL}/64 scope link\n       valid_lft forever preferred_lft forever" in out, out
    assert re.search(rf"inet6 {GLOBAL}/64 scope site dynamic\n       valid_lft \d+sec preferred_lft \d+sec", out), out
    assert "fe80::/64 dev eth0 proto kernel metric 256" in out and "fec0::/64 dev eth0 proto kernel metric 256" in out, out
    assert re.search(r"default via fe80::2 dev eth0 proto ra metric 1024 expires 1[78]\d\dsec", out), out                 # the router advertised a lifetime of 1800 s
    out, rc = g.run("ping6 -c 3 fec0::2; ping6 -c 2 fe80::2%eth0; ip -6 neigh")
    assert "3 packets transmitted, 3 received" in out and "2 packets transmitted, 2 received" in out and rc == 0, out
    assert re.search(r"fe80::2 dev eth0 lladdr 52:56:00:00:00:02 router (REACHABLE|STALE|DELAY|PROBE)", out) and "fec0::2 dev eth0 lladdr 52:56:00:00:00:02 router" in out, out
    out, rc = g.run("cat /etc/resolv.conf; ping6 -c 1 2001:db8::1; echo rc=$?")
    assert "nameserver 10.0.2.3\nnameserver fec0::3 # RDNSS" in out, out                                                   # RDNSS beside the DHCP server
    assert "From fe80::2%eth0 icmp_seq=1 Destination unreachable: No route" in out and "rc=1" in out, out                  # a router's error comes back to the ping


def nic6_case(model, label, cmdline=""):
    def run():
        with Guest("t-net6-" + label, net=model, cmdline=cmdline, root=True) as g:
            check_card(g, "eth0")
    run.__name__ = f"net6_nic_{label}"
    run.__doc__ = (f"{model}: a link-local address with duplicate address detection, the global address and the default route from QEMU's router advertisement, RDNSS in resolv.conf, ping6 of the router "
                   f"(link-local and global), the neighbour cache, a router's error message; the card lets the multicast of neighbour discovery through")
    TESTS.append(run)


nic6_case("virtio-net-pci", "virtio")
nic6_case("e1000", "e1000")
nic6_case("e1000e", "e1000e")
nic6_case("rtl8139", "rtl8139")
nic6_case("e1000e", "e1000e_pch", cmdline="e1000.pch")


@test
def net6_host_tcp_udp_http():
    """TCP, UDP and HTTP through the card to Python servers on the host's ::1, which the guest reaches as fec0::2: nc echoes, a 200 KB download with a checksum, redirects and a 404 through fetch, wget"""
    tcp, tport = serve6(Server6, _Echo)
    udp_srv = None

    class UdpEcho(socketserver.BaseRequestHandler):
        def handle(self):
            data, sock = self.request
            sock.sendto(b"udp6:" + data, self.client_address)

    udp_srv, uport = serve6(UdpServer6, UdpEcho)
    http, hport = serve6(Server6, _Http)
    try:
        with Guest("t-net6-host", net="e1000", root=True) as g:
            wait_ready(g)
            out, rc = g.run(f"echo konnichiwa | nc -w 20 fec0::2 {tport}", timeout=180)
            assert "echo:konnichiwa" in out and rc == 0, (out, rc)
            out, rc = g.run(f"echo hello-udp | nc -u -w 5 fec0::2 {uport}", timeout=180)
            assert "udp6:hello-udp" in out, out
            base = f"http://[fec0::2]:{hport}"
            out, rc = g.run(f"fetch -q {base}/hello.txt", timeout=180)
            assert out.strip() == "hello from the other side of the boundary" and rc == 0, (out, rc)
            out, rc = g.run(f"fetch -q {base}/redir")                                          # the redirect keeps the IPv6 address in the Location it follows
            assert "hello from the other side" in out and rc == 0, (out, rc)
            out, rc = g.run(f"fetch -q -O /tmp/big {base}/big; md5sum /tmp/big", timeout=300)
            import hashlib
            assert hashlib.md5(BIG).hexdigest() in out, out
            out, rc = g.run(f"cd /tmp && wget {base}/hello.txt && cat hello.txt", timeout=180)
            assert "saved" in out and "hello from the other side" in out and rc == 0, (out, rc)
            out, rc = g.run(f"fetch -q {base}/missing; echo rc=$?")
            assert "rc=8" in out, out
            out, rc = g.run(f"fetch -q https://[fec0::2]/; echo rc=$?")
            assert "not supported" in out and "rc=1" in out, out                               # libhttp (https) is IPv4 only
            out, rc = g.run("ping6 -c 1 fec0::2; ss -t | head -3")
            assert "1 received" in out, out
    finally:
        for srv in (tcp, udp_srv, http):
            srv.shutdown()


# ------------------------------------------------------------------------------------------------------------ names

AAAA_OF = {"kappa6.gensokyo": ["fec0::2"], "both.gensokyo": ["fec0::2"], "order.gensokyo": ["fec0::2", "2001:db8::2"]}
A_OF = {"both.gensokyo": ["10.0.2.2"], "order.gensokyo": ["10.0.2.2"]}
PTR_NAME = ".".join(reversed("".join(f"{b:02x}" for b in socket.inet_pton(socket.AF_INET6, "fec0::2")))) + ".ip6.arpa"


def dns6_reply(query):
    """A responder for A, AAAA, CNAME and PTR (ip6.arpa) of a few names; everything else does not exist."""
    ident, flags, qd = struct.unpack(">HHH", query[:6])
    name, pos = _name(query, 12)
    qtype, qclass = struct.unpack(">HH", query[pos:pos + 4])
    question = query[12:pos + 4]
    answers, rcode = [], 0

    def rr(rtype, rdata, owner=b"\xc0\x0c"):
        return owner + struct.pack(">HHIH", rtype, 1, 60, len(rdata)) + rdata

    if name == "www6.gensokyo":                                                                   # an alias of a name that has an AAAA record only
        if qtype in (1, 5, 28):
            answers.append(rr(5, _enc("kappa6.gensokyo")))
            if qtype == 28:
                answers.append(rr(28, socket.inet_pton(socket.AF_INET6, "fec0::2"), _enc("kappa6.gensokyo")))
    elif name in AAAA_OF or name in A_OF:
        if qtype == 28:
            answers += [rr(28, socket.inet_pton(socket.AF_INET6, a)) for a in AAAA_OF.get(name, [])]
        elif qtype == 1:
            answers += [rr(1, bytes(int(x) for x in a.split("."))) for a in A_OF.get(name, [])]
    elif name == PTR_NAME:
        if qtype == 12:
            answers.append(rr(12, _enc("kappa6.gensokyo")))
    else:
        rcode = 3
    header = struct.pack(">HHHHHH", ident, 0x8180 | rcode, 1, len(answers), 0, 0)
    return header + question + b"".join(answers)


class _Dns6Handler(socketserver.BaseRequestHandler):
    def handle(self):
        data, sock = self.request
        try:
            sock.sendto(dns6_reply(data), self.client_address)
        except Exception:                                                                         # a malformed query must not kill the responder
            pass


@test
def net6_dns_host():
    """a Python DNS responder on the host's ::1 reached over IPv6 as fec0::2: nslookup and host for AAAA (the answer in its shortest form), PTR under ip6.arpa, a CNAME to a name with only an AAAA record, ping6 and fetch of a name that has no A record, getaddrinfo's order by RFC 6724 (an address of 2001:db8::/64 first, then IPv4, then the site-local one), AI_V4MAPPED and AI_ALL"""
    dns, dport = serve6(UdpServer6, _Dns6Handler)
    http, hport = serve6(Server6, _Http)
    try:
        with Guest("t-net6-dns", net="e1000e", root=True) as g:
            wait_ready(g)
            g.run(f"printf 'nameserver fec0::2#{dport}\\nsearch gensokyo\\n' > /etc/resolv.conf")
            out, rc = g.run("nslookup -type=AAAA kappa6.gensokyo")
            assert f"Server:\t\tfec0::2\nAddress:\tfec0::2#{dport}" in out and "Name:\tkappa6.gensokyo\nAddress: fec0::2" in out and rc == 0, (out, rc)
            out, rc = g.run("host -t AAAA both.gensokyo; host -t A both.gensokyo; host -t AAAA www6.gensokyo")
            assert "both.gensokyo has IPv6 address fec0::2" in out and "both.gensokyo has address 10.0.2.2" in out, out
            assert "www6.gensokyo is an alias for kappa6.gensokyo." in out and "kappa6.gensokyo has IPv6 address fec0::2" in out, out
            out, rc = g.run("host fec0::2; nslookup fec0::2")                                     # an address asks for the name under ip6.arpa (RFC 3596)
            assert out.count("kappa6.gensokyo.") == 2, out
            out, rc = g.run("nslookup nosuch.gensokyo; echo rc=$?")
            assert "NXDOMAIN" in out and "rc=1" in out, out
            out, rc = g.run("ping6 -c 1 kappa6.gensokyo; ping -6 -c 1 www6", timeout=240)
            assert "PING kappa6.gensokyo(fec0::2)" in out and out.count("1 received") == 2, out
            out, rc = g.run(f"fetch -q http://kappa6.gensokyo:{hport}/hello.txt", timeout=240)    # the name has an AAAA record only: the connection goes to fec0::2 over IPv6
            assert "hello from the other side" in out and rc == 0, (out, rc)
            out, rc = g.run(f"fetch -q http://both.gensokyo:{hport}/hello.txt", timeout=240)      # both families: whichever the order puts first, it works
            assert "hello from the other side" in out, out
            # RFC 6724: with an address of 2001:db8::/64 of our own, that destination is the best, IPv4 next, the site-local fec0::2 last
            out, rc = g.run("ip -6 addr add 2001:db8::5/64 dev eth0; sleep 3; nettest6 gai order.gensokyo", timeout=240)
            got = re.findall(r"^GAI (\S+) (\S+)$", out, re.M)
            assert got == [("inet6", "2001:db8::2"), ("inet", "10.0.2.2"), ("inet6", "fec0::2")], (got, out)
            out, rc = g.run(f"fetch -q http://order.gensokyo:{hport}/hello.txt", timeout=240)   # the first address is on-link and nobody answers for it, nothing listens on the second: the third works
            assert "hello from the other side" in out and rc == 0, (out, rc)
            out, rc = g.run("nettest6 gai order.gensokyo inet6; echo --; nettest6 gai order.gensokyo inet; echo --; nettest6 gai order.gensokyo inet6 v4mapped; echo --; nettest6 gai order.gensokyo inet6 v4mapped all")
            parts = out.split("--\n")
            assert re.findall(r"^GAI (\S+) (\S+)$", parts[0], re.M) == [("inet6", "2001:db8::2"), ("inet6", "fec0::2")], parts[0]
            assert re.findall(r"^GAI (\S+) (\S+)$", parts[1], re.M) == [("inet", "10.0.2.2")], parts[1]
            assert "::ffff:" not in parts[2], parts[2]                                           # AI_V4MAPPED alone: mapped addresses only when there is no IPv6 one
            assert re.findall(r"^GAI (\S+) (\S+)$", parts[3], re.M)[:3] == [("inet6", "2001:db8::2"), ("inet6", "::ffff:10.0.2.2"), ("inet6", "fec0::2")], parts[3]
            out, rc = g.run("nettest6 gai localhost; echo --; nettest6 gai ::1 numerichost; echo --; nettest6 gai fe80::1%eth0 numerichost; echo --; nettest6 gai 10.0.2.2 inet6 numerichost; echo --; nettest6 gai - passive; echo --; nettest6 gai -")
            parts = out.split("--\n")
            assert re.findall(r"^GAI (\S+) (\S+)$", parts[0], re.M) == [("inet6", "::1"), ("inet", "127.0.0.1")], parts[0]       # /etc/hosts has both; ::1 has the higher precedence
            assert "GAI inet6 ::1" in parts[1] and "GAI inet6 fe80::1%eth0" in parts[2] and "GAI error" in parts[3], out
            assert re.findall(r"^GAI (\S+) (\S+)$", parts[4], re.M) == [("inet6", "::"), ("inet", "0.0.0.0")], parts[4]
            assert re.findall(r"^GAI (\S+) (\S+)$", parts[5], re.M) == [("inet6", "::1"), ("inet", "127.0.0.1")], parts[5]
    finally:
        dns.shutdown()
        http.shutdown()


@test
def net6_addrconfig_without_a_router():
    """a network without IPv6 router advertisements (QEMU's ipv6=off): the card only has its link-local address, so AI_ADDRCONFIG leaves the AAAA records out (they are not even asked for), and without the flag a destination that has no route sorts after the IPv4 one"""
    seen = []

    class Dns4(socketserver.BaseRequestHandler):
        def handle(self):
            data, sock = self.request
            _, pos = _name(data, 12)
            seen.append(struct.unpack(">H", data[pos:pos + 2])[0])
            sock.sendto(dns6_reply(data), self.client_address)

    class Udp4(socketserver.ThreadingMixIn, socketserver.UDPServer):
        allow_reuse_address = True
        daemon_threads = True

    port = free_port()
    srv = Udp4(("127.0.0.1", port), Dns4)                                                           # reached over IPv4 as 10.0.2.2
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        g = Guest("t-net6-nora", net="e1000", root=True)
        g.vm.netdev_opts = "ipv6=off"
        with g:
            time.sleep(scaled(4))
            out, rc = g.run("cat /proc/net/if_inet6; ip -6 route; ifconfig eth0")
            assert re.search(r"^fe80000000000000505400fffe123456 0[12] 40 20 80\s+eth0$", out, re.M) and "fec0" not in out and "default via" not in out, out
            g.run(f"printf 'nameserver 10.0.2.2#{port}\\nsearch gensokyo\\n' > /etc/resolv.conf")
            seen.clear()
            out, rc = g.run("nettest6 gai order.gensokyo addrconfig")
            assert re.findall(r"^GAI (\S+) (\S+)$", out, re.M) == [("inet", "10.0.2.2")], out
            assert 28 not in seen and 1 in seen, seen                                               # no AAAA query went out
            seen.clear()
            out, rc = g.run("nettest6 gai order.gensokyo")
            got = re.findall(r"^GAI (\S+) (\S+)$", out, re.M)
            assert got[0] == ("inet", "10.0.2.2") and {a for _, a in got} == {"10.0.2.2", "fec0::2", "2001:db8::2"}, (got, out)    # no route to the IPv6 ones: they come last
            assert 28 in seen, seen
    finally:
        srv.shutdown()


# ------------------------------------------------------------------------------------------------------------ robustness and the rules of the RFCs

@test
def net6_reassembly_and_headers():
    """fragments made by hand through /proc/net/inject: in order, backwards, repeated (the datagram arrives once), overlapping (thrown away, even if the rest comes later), an atomic fragment, a length that is no multiple of 8, one that would exceed 65535, more than 64 fragments, ten datagrams for four queues, and the timeout with "time exceeded"; extension headers: options by the top two bits of their type, the router alert, routing header type 0 (refused) and the others with and without segments left, a hop-by-hop header in the wrong place, an unknown protocol, nine headers in a row, with the parameter problems checked by code and pointer"""
    with Guest("t-net6-rfc", root=True) as g:
        for name in ("reasm", "exthdr"):
            out, rc = g.run(f"nettest6 {name}", timeout=300)
            assert "NETTEST6 PASS" in out and re.search(rf"ok\s+{name}\b", out) and rc == 0, f"{name} failed (exit {rc}):\n{out[-1500:]}"
        for _ in range(20):
            buffers = int(re.search(r"buffers (\d+)", g.out("cat /proc/net/tune")).group(1))
            if buffers <= 4:
                break
            time.sleep(1)
        assert buffers <= 4, f"{buffers} packet buffers are still in use"
        assert "ip6_reasm_ms 60000" in g.out("cat /proc/net/tune")                      # the test shortened it and put it back; reading the file shows it
        out, rc = g.run("grep -E 'Ip6Reasm|Icmp6OutParm|Icmp6OutTimeExcds|Ip6InHdrErrors|Ip6InUnknown' /proc/net/snmp6")
        assert re.search(r"Ip6ReasmOKs\s+[1-9]", out) and re.search(r"Ip6ReasmFails\s+[1-9]", out) and re.search(r"Icmp6OutTimeExcds\s+[1-9]", out) and re.search(r"Ip6InUnknownProtos\s+[1-9]", out), out


@test
def net6_neighbour_discovery_robust():
    """with a card: duplicate address detection by hand (tentative for about a second, an advertisement or another node's probe makes the address a duplicate, a neighbour's ordinary solicitation does not), 4000 solicitations from forged addresses (the 64-entry cache holds, the advertisements are rate limited), 3000 neighbour discovery messages that break the rules, 2500 router advertisements with wild values (the tables stay bounded), and a router's "packet too big" that lowers the size of fragments to that destination but never raises it; the card still talks afterwards"""
    with Guest("t-net6-nd", net="e1000", root=True) as g:
        wait_ready(g)
        for name in ("dad", "pmtu", "ndfuzz"):
            out, rc = g.run(f"nettest6 {name}", timeout=600)
            assert "NETTEST6 PASS" in out and re.search(rf"ok\s+{name}\b", out) and rc == 0, f"{name} failed (exit {rc}):\n{out[-1800:]}"
        assert re.search(r"ndfuzz: \d+ advertisements for 4000 solicitations, \d+ neighbours in the cache", out), out
        log = g.out("dmesg | grep -c 'duplicate address detected'")
        assert int(log.strip().splitlines()[-1]) >= 2, log
        for _ in range(20):
            buffers = int(re.search(r"buffers (\d+)", g.out("cat /proc/net/tune")).group(1))
            if buffers <= 4:
                break
            time.sleep(1)
        assert buffers <= 4, f"{buffers} packet buffers are still in use after the fuzzing"
        out, rc = g.run("ping6 -c 3 fec0::2; ping6 -c 1 fe80::2%eth0; ping -c 1 10.0.2.2", timeout=240)
        assert out.count("received") == 3 and "3 packets transmitted, 3 received" in out and "0% packet loss" in out, out


@test
def net6_static_config_and_link():
    """ip, ifconfig and route set IPv6 addresses (checked by duplicate address detection first), routes and neighbours and show them in Linux's forms; an address that was set by hand survives a pulled cable and is checked again, what the router taught goes with the cable and comes back with the next advertisement"""
    with Guest("t-net6-static", net="e1000", root=True) as g:
        wait_ready(g)
        out, rc = g.run("ip -6 addr add fec0::77/64 dev eth0; ip -6 addr show dev eth0 | grep -c tentative; ifconfig eth0 inet6 add fec0::78/64; ifconfig eth0 | grep -c 'inet6 fec0::7'")
        assert out.split() == ["1", "2"] or out.split()[-1] == "2", out
        deadline = time.time() + scaled(60)
        while "tentative" in g.out("ip -6 addr show dev eth0") and time.time() < deadline:
            time.sleep(1)
        out, rc = g.run("ip -6 addr show dev eth0; ping6 -c 1 fec0::77; ip -6 addr add fec0::77/64 dev eth0; echo rc=$?")
        assert re.search(r"inet6 fec0::77/64 scope site\n", out) and re.search(r"inet6 fec0::78/64 scope site\n", out), out         # permanent: no "dynamic", no lifetimes that run out
        assert "1 received" in out and "File exists" in out and "rc=1" in out, out
        out, rc = g.run("ip -6 route add 2001:db8:5::/48 via fe80::2 dev eth0 metric 700; ip -6 route add 2001:db8:6::/48 dev eth0; ip -6 route; route -A inet6 -n; ping6 -c 1 2001:db8:5::1")
        assert "2001:db8:5::/48 via fe80::2 dev eth0 proto boot metric 700" in out and "2001:db8:6::/48 dev eth0 proto boot metric 1024" in out, out
        assert re.search(r"2001:db8:5::/48\s+fe80::2\s+UG\s+700", out), out
        assert "From fe80::2%eth0 icmp_seq=1 Destination unreachable: No route" in out, out                                       # the packet went to the router that the route names
        out, rc = g.run("ip -6 route del 2001:db8:5::/48; ip -6 route del 2001:db8:6::/48; ip -6 route; ip -6 route del 2001:db8:7::/48; echo rc=$?")
        assert "2001:db8:5" not in out.split("rc=")[0] and "2001:db8:6" not in out.split("rc=")[0] and "rc=1" in out, out
        out, rc = g.run("ip -6 neigh add fe80::dead dev eth0 lladdr 52:56:00:00:00:77; ip -6 neigh; ip -6 neigh del fe80::dead dev eth0; ip -6 neigh | grep -c dead")
        assert "fe80::dead dev eth0 lladdr 52:56:00:00:00:77 PERMANENT" in out and out.strip().endswith("0"), out
        out, rc = g.run("ip -6 addr del fec0::77/64 dev eth0; ifconfig eth0 inet6 del fec0::78/64; ip -6 addr show dev eth0 | grep -c fec0::7; ip -6 addr del fec0::77/64 dev eth0; echo rc=$?")
        assert re.search(r"^0$", out, re.M) and "Cannot assign requested address" in out and "rc=1" in out, out
        g.run("ip -6 addr add fec0::79/64 dev eth0")
        # the cable is pulled: what the router taught goes (its address, its default route, its neighbour entry); what was set by hand and the link-local address stay
        vm = g.vm
        vm.qmp("set_link", name="net0", up=False)
        deadline = time.time() + scaled(120)
        while "link down" not in vm.text() and time.time() < deadline:
            time.sleep(0.5)
        out, rc = g.run("ip -6 addr show dev eth0; ip -6 route; ip -6 neigh")
        assert GLOBAL not in out and "default via" not in out and "fe80::2" not in out and LINK_LOCAL in out and "fec0::79/64" in out, out
        vm.qmp("set_link", name="net0", up=True)
        out = wait_ready(g, timeout=180)                                          # the address from the router comes back (it was gone: the check above), past duplicate address detection
        assert re.search(r"^fec0000000000000000000000000079 0[12] 40 40 80\s+eth0$", out.replace("fec0000000000000000000000000079", "fec0000000000000000000000000079"), re.M) or "0079" in out, out
        out, rc = g.run("ip -6 route; ping6 -c 2 fec0::2; ip -6 addr show dev eth0", timeout=240)
        assert re.search(r"default via fe80::2 dev eth0 proto ra", out) and "2 packets transmitted, 2 received" in out and "fec0::79/64" in out, out
        # a card that is brought down and up again starts IPv6 over
        out, rc = g.run("ifconfig eth0 down; ip -6 addr show dev eth0 | grep -c dynamic; ifconfig eth0 up; echo up")
        assert "up" in out, out
        wait_ready(g)


@test
def net6_static6_in_network_conf():
    """a `static6` line in network.conf (kept on the HUMANVILLAGE volume): netconf gives the card the address and the default route over IPv6 before the first session while DHCP still
    gives the IPv4 lease (a static6 line holds nothing back), the address passes duplicate address detection, the route stays a static one when the router's advertisement names the same
    router, ping6 works, netconf -n says what a line would do (and nothing for another card's hardware address), running netconf again changes nothing, and the checks of the file refuse
    each way a static6 line can be wrong"""
    from case_storage import Image, Guest as StorageGuest, hv_part, MIB
    conf = b"# an IPv6 address by hand; the card still asks DHCP for IPv4\neth0 static6 fec0::77/64 gateway fe80::2\n"
    with Image("net6-static6", 64 * MIB, [hv_part({"/.system/etc/network.conf": conf})]) as img:
        with Qemu(cmdline="text bootlog=off user=root", disk=img.path, net="e1000", name="t-net6-static6") as vm:
            g = StorageGuest(vm, True)
            vm.expect(ROOT_PROMPT, timeout=120)
            log = vm.text()
            assert "[netconf] eth0: static6 fec0::77/64, gateway fe80::2" in log, log[-1500:]
            assert "DHCP waits until netconf has run" not in log, "a static6 line must not hold the DHCP client back"
            deadline = time.time() + scaled(120)                                 # the address passes the duplicate check, and the router's advertisement has come
            while True:
                out = g.ok("cat /proc/net/if_inet6")
                flags = {ln.split()[0]: int(ln.split()[4], 16) for ln in out.splitlines() if ln.endswith("eth0")}
                mine, ra, ll = "fec0" + "0" * 24 + "0077", "fec0000000000000505400fffe123456", "fe80000000000000505400fffe123456"
                if all(a in flags and not flags[a] & 0x40 for a in (mine, ra, ll)):
                    break
                assert time.time() < deadline, "the addresses never became ready:\n" + out + "\n" + vm.text()[-1500:]
                time.sleep(1)
            assert flags[mine] & 0x80, flags                                     # 0x80: permanent, as the router's addresses are not
            assert not flags[ra] & 0x80, flags
            out = g.ok("ip -6 addr show dev eth0; ip -6 route; cat /etc/resolv.conf")
            assert re.search(r"inet6 fec0::77/64 scope site\n       valid_lft forever preferred_lft forever", out), out
            assert "default via fe80::2 dev eth0 proto boot metric 1024" in out, out
            assert "proto ra" not in out, out                                      # the advertisement did not turn the route that was set by hand into one that expires
            deadline = time.time() + scaled(120)
            while not re.search(LEASE, vm.text()) and time.time() < deadline:
                time.sleep(0.5)
            assert re.search(LEASE, vm.text()), vm.text()[-1500:]
            out, rc = g.run("ping6 -c 2 fec0::2; ping -c 1 10.0.2.2", timeout=240)
            assert rc == 0 and "2 packets transmitted, 2 received" in out and "1 packets transmitted, 1 received" in out, out
            out = g.ok("netconf -n; netconf; ip -6 addr show dev eth0 | grep -c 'inet6 fec0::77'; ip -6 route | grep -c '^default'")
            assert "netconf: would set eth0: static6 fec0::77/64, gateway fe80::2" in out, out
            assert "netconf: eth0: static6 fec0::77/64, gateway fe80::2" in out, out
            assert out.split()[-2:] == ["1", "1"], out                             # the second run added nothing: one address, one default route
            g.ok("printf 'eth0 static 10.0.2.77/24 gateway 10.0.2.2\\neth0 static6 2001:db8::5/64\\neth1 static6 2001:db8::6/64\\n' > /tmp/both.conf")
            out = g.ok("netconf -n /tmp/both.conf")
            assert "netconf: would set eth0: static6 2001:db8::5/64" in out and "netconf: would set eth0: static 10.0.2.77/24, gateway 10.0.2.2" in out, out
            assert "eth1" not in out, out                                          # the card does not exist
            g.ok("printf 'eth0 static6 2001:db8::5/64 mac 52:54:00:99:99:99\\n' > /tmp/other.conf")
            assert g.ok("netconf -n /tmp/other.conf").strip() == "", "a line for another hardware address must not be applied"
            # the file's checks (the same ones refuse a damaged file at boot and in etckeep)
            wrong = [
                ("eth0 static6 fec0::77", "'fec0::77' has no /PREFIX"),
                ("eth0 static6", "static6 needs ADDR/PREFIX"),
                ("eth0 static6 fec0::77/0", "the prefix must be 1 to 128"),
                ("eth0 static6 fec0::77/129", "the prefix must be 1 to 128"),
                ("eth0 static6 ::1/128", "'::1' is not a usable IPv6 address"),
                ("eth0 static6 ::/64", "'::' is not a usable IPv6 address"),
                ("eth0 static6 fe80::5/64", "'fe80::5' is not a usable IPv6 address"),
                ("eth0 static6 ff02::5/64", "'ff02::5' is not a usable IPv6 address"),
                ("eth0 static6 not-an-address/64", "'not-an-address' is not a usable IPv6 address"),
                ("eth0 static6 fec0::77/64 gateway 2001:db8::1", "the gateway 2001:db8::1 is neither link-local nor inside the card's prefix"),
                ("eth0 static6 fec0::77/64 gateway fec0::77", "the gateway is the card's own address"),
                ("eth0 static6 fec0::77/64 gateway nonsense", "the gateway nonsense is not an IPv6 address"),
                ("eth0 static6 fec0::77/64 dns 1.1.1.1", "unexpected 'dns'"),
                ("eth0 static6 fec0::77/64 mac 52:54:00:12:34", "'52:54:00:12:34' is not a MAC address"),
                ("eth0 static6 fec0::77/64\\neth0 static6 fec0::78/64", "eth0 has two static6 lines"),
                ("eth0 sttic6 fec0::77/64", "'sttic6' is neither dhcp, static nor static6"),
            ]
            for text, why in wrong:
                out, rc = g.run(f"printf '{text}\\n' > /etc/network.conf; etckeep check network.conf")
                assert re.search(r"^/etc/network\.conf: damaged: line \d+: " + re.escape(why), out, re.M) and rc == 1, (text, out)
            right = ["eth0 static6 fec0::77/64", "eth0 static6 2001:db8::5/128 gateway fe80::1", "eth0 static6 fec0::77/64 gateway fec0::1 mac 52:54:00:12:34:56",
                     "eth0 static6 fec0::77/64 mac 52:54:00:12:34:56\\neth0 static6 fec0::78/64", "eth0 static 10.0.2.77/24 gateway 10.0.2.2\\neth0 static6 fec0::77/64\\neth1 dhcp"]
            for text in right:
                out, rc = g.run(f"printf '{text}\\n' > /etc/network.conf; etckeep check network.conf")
                assert "/etc/network.conf: fine" in out and rc == 0, (text, out)
