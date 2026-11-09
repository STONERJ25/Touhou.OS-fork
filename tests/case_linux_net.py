"""Sockets, poll, epoll, eventfd and timerfd for the Linux personality (docs/COMPAT.md, "Sockets, poll and event descriptors"): ordinary C programs, linked statically with glibc,
run under TouhouOS and print what the same program prints on a real Linux.

The programs are in tests/linux/src (net_*.c). What a real Linux prints for them is stored in tests/linux/golden/ by tools/linux-oracle.py (in WSL) and compared
here line by line, like the glibc programs of case_linux.py, whose helpers this file uses: the guest downloads the binaries from a server on the host over QEMU's
user networking. The programs talk over the loopback interface only, except the last test, which talks to a Python server on the host through 10.0.2.2.

    python tools/qemu-slot.py -- python tests/run_all.py linux_net

NET_ONLY=net_tcp,net_udp runs only those programs (for debugging one of them); NET_LIMIT=600 sets the time limit of every program (seconds);
LINUX_TRACE=1 logs every call to the serial console.
"""
import os
import socket
import socketserver
import sys
import threading
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_shell
from harness import test, Qemu
import case_linux as base

# name -> (the command line in the guest, its time limit in seconds)
PROGRAMS = {
    "net_tcp": ("linux /tmp/net_tcp", 300),
    "net_errors": ("linux /tmp/net_errors", 300),
    "net_udp": ("linux /tmp/net_udp", 300),
    "net_unix": ("linux /tmp/net_unix", 300),
    "net_unix2": ("linux /tmp/net_unix2", 300),
    "net_resolve": ("linux /tmp/net_resolve", 300),
    "net_poll": ("linux /tmp/net_poll", 300),
    "net_epoll": ("linux /tmp/net_epoll", 300),
    "net_events": ("linux /tmp/net_events", 300),
}


def run_programs(names, vm_name, setup=None):
    """Downloads the programs into a guest with a network card, runs each and compares its output with the stored output of a real Linux; every difference is
    collected and reported together. `setup(vm, port)` may do more before the programs run."""
    only = os.environ.get("NET_ONLY")
    names = [n for n in names if not only or n in only.split(",")]
    if not names:
        return
    base.build_inputs()
    failures = []
    with base.Inputs() as srv, Qemu(cmdline="text" + base.TRACE, name=vm_name, net="virtio-net-pci") as vm:
        vm.expect(test_shell.PROMPT, timeout=300)
        for n in names:
            base.fetch(vm, srv.port, n)
        if setup:
            setup(vm, srv.port)
        for n in names:
            cmd, limit = PROGRAMS[n]
            limit = int(os.environ.get("NET_LIMIT", limit))
            out, rc = base.run(vm, cmd, timeout=limit)
            try:
                base.compare_with_linux(n, out, rc)
            except AssertionError as e:
                failures.append(str(e))
    assert not failures, "\n\n".join(failures)


@test
def linux_net_sockets_match_linux():
    """TCP, UDP and UNIX-domain sockets through glibc (socket, bind, listen, accept4, connect, the address and option calls, send/recv with the MSG_ flags,
    sendmsg/recvmsg/sendmmsg/recvmmsg, shutdown and end of file, EPIPE and SIGPIPE, ECONNREFUSED, EADDRINUSE, non-blocking connect with poll, abstract names,
    SO_PEERCRED, a server thread and a 1 MB echo): the same output as on a real Linux"""
    run_programs(["net_tcp", "net_errors", "net_udp", "net_unix", "net_unix2", "net_resolve"], "t-linux-net-sockets")


@test
def linux_net_events_match_linux():
    """poll, ppoll, select and pselect (timeouts, EINTR, signal masks), epoll (level-triggered, edge-triggered, one-shot, nested sets, closing a watched
    descriptor, ten TCP clients served by one thread), eventfd (counter, semaphore, wake-ups between threads) and timerfd (intervals, one-shot, absolute
    deadlines, poll and epoll): the same output as on a real Linux"""
    run_programs(["net_poll", "net_epoll", "net_events"], "t-linux-net-events")


# ---- the servers of linux_net_talks_to_the_host: Python on the host, which the guest sees as 10.0.2.2 (QEMU's user networking)

class _TcpServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True


class _Session(socketserver.StreamRequestHandler):
    """greets, answers "ping N", checksums a block announced by "BIG n", says goodbye when the client shuts down its sending side"""

    def handle(self):
        self.request.settimeout(120)
        self.wfile.write(b"hello from the host\n")
        self.wfile.flush()
        while True:
            line = self.rfile.readline()
            if not line:
                break
            if line.startswith(b"ping "):
                self.wfile.write(b"echo:" + line)
            elif line.startswith(b"BIG "):
                data = self.rfile.read(int(line.split()[1]))
                self.wfile.write(b"adler:%08x\n" % (zlib.adler32(data) & 0xffffffff))
            self.wfile.flush()
        self.wfile.write(b"bye\n")
        self.wfile.flush()


def _udp_server(handler):
    srv = socketserver.UDPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


class _UdpEcho(socketserver.BaseRequestHandler):
    def handle(self):
        data, sock = self.request
        sock.sendto(b"UDP:" + data, self.client_address)


class _Dns(socketserver.BaseRequestHandler):
    """answers every A query with 192.0.2.7 (the question is copied, the answer names it by a pointer to offset 12)"""

    def handle(self):
        data, sock = self.request
        question = data[12:]
        reply = data[:2] + bytes([0x81, 0x80]) + data[4:6] + bytes([0, 1, 0, 0, 0, 0]) + question
        reply += bytes([0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 0, 2, 7])
        sock.sendto(reply, self.client_address)


def _closed_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


HOST_EXPECTED = """connect: 0
the source address is a real interface address, not loopback: 1
the peer is the server we named: 1
greeting: hello from the host
answer: echo:ping 1
sent 300000 bytes; the server's checksum equals ours: 1
shutdown(SHUT_WR): 0
last words: bye
then end of file: 0
poll for the answer: 1
answer: 24 bytes 'UDP:dgram from the guest', from the server's port: 1, address: 10.0.2.2
DNS answer: id matches 1, 1 answer, address 192.0.2.7
getaddrinfo of a numeric address: 0, 10.0.2.2 port 80
inet_pton: 1, inet_ntop: 192.0.2.9
connect to a port nobody listens on: -1 errno 111 (ECONNREFUSED 111)
exit status: 0
"""


@test
def linux_net_talks_to_the_host():
    """A glibc program in the guest talks to Python servers on the host through QEMU's user networking (10.0.2.2): a TCP session with a 300000-byte block and a
    checksum, shutdown and end of file, a UDP exchange, a DNS query built by hand and sent over UDP, numeric getaddrinfo, and a refused connect"""
    base.build_inputs()
    tcp = _TcpServer(("127.0.0.1", 0), _Session)
    threading.Thread(target=tcp.serve_forever, daemon=True).start()
    udp, dns = _udp_server(_UdpEcho), _udp_server(_Dns)
    closed = _closed_port()
    try:
        with base.Inputs() as srv, Qemu(cmdline="text" + base.TRACE, name="t-linux-net-host", net="virtio-net-pci") as vm:
            vm.expect(test_shell.PROMPT, timeout=300)
            base.fetch(vm, srv.port, "net_host")
            args = f"10.0.2.2 {tcp.server_address[1]} {udp.server_address[1]} {dns.server_address[1]} {closed}"
            out, rc = base.run(vm, f"linux /tmp/net_host {args}", timeout=300)
            got = out + f"exit status: {rc}\n"
            assert got == HOST_EXPECTED, f"the output differs:\n--- wanted\n{HOST_EXPECTED}--- got\n{got}"
    finally:
        for server in (tcp, udp, dns):
            server.shutdown()
            server.server_close()
