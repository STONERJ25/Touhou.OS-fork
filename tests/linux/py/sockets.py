"""The socket module over the loopback interface: TCP with a server thread, UDP, socketpair, UNIX-domain sockets (a path in the working directory and the abstract namespace),
descriptor passing, options, time-outs and non-blocking mode, the address functions and getaddrinfo, socketserver, and an HTTP server with a client in the same program."""
import errno
import hashlib
import http.server
import os
import socket
import socketserver
import struct
import threading
import time
import urllib.request


def show(label, value=""):
    print(f"{label}: {value}")


def serve_once(listener, handler):
    def run():
        conn, addr = listener.accept()
        with conn:
            handler(conn, addr)
    t = threading.Thread(target=run)
    t.start()
    return t


print("--- TCP")
listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind(("127.0.0.1", 0))
listener.listen(5)
host, port = listener.getsockname()
show("bound to", (host, port > 0, listener.family.name, listener.type.name))


def echo(conn, addr):
    while True:
        data = conn.recv(4096)
        if not data:
            break
        conn.sendall(data.upper())


t = serve_once(listener, echo)
c = socket.create_connection((host, port))
show("peer", (c.getpeername() == (host, port), c.getsockname()[0]))
c.sendall(b"hello server")
show("echo", c.recv(100))
c.sendall(b"second")
c.sendall(b" message")
buf = b""
while len(buf) < 14:
    buf += c.recv(100)
show("two sends", buf)
payload = bytes(range(256)) * 4096                     # 1 MB: the server echoes it in pieces, so we read and write at the same time
sent = []
w = threading.Thread(target=lambda: (c.sendall(payload), sent.append(True)))
w.start()
got = bytearray()
while len(got) < len(payload):
    chunk = c.recv(65536)
    if not chunk:
        break
    got += chunk
w.join()
show("1 MB echo", (len(got), hashlib.sha256(bytes(got)).hexdigest()[:16] == hashlib.sha256(payload.upper()).hexdigest()[:16]))
c.shutdown(socket.SHUT_WR)
show("end of file after shutdown", c.recv(10))
c.close()
t.join()
listener.close()

print("--- TCP errors and options")
s = socket.socket()
try:
    s.connect(("127.0.0.1", port))
except ConnectionRefusedError as e:
    show("refused", e.errno)
s.close()
s = socket.socket()
s.settimeout(0.1)
l2 = socket.socket()
l2.bind(("127.0.0.1", 0))
l2.listen(1)
l2.settimeout(0.1)
try:
    l2.accept()
except TimeoutError as e:
    show("accept time-out", type(e).__name__)
s.connect(l2.getsockname())
srv, _ = l2.accept()
try:
    s.recv(10)
except TimeoutError:
    show("recv time-out", "TimeoutError")
s.setblocking(False)
try:
    s.recv(10)
except BlockingIOError as e:
    show("non-blocking recv", e.errno)
show("gettimeout", (s.gettimeout(), srv.gettimeout()))
s.setblocking(True)
srv.sendall(b"abcdef")
show("MSG_PEEK", (s.recv(3, socket.MSG_PEEK), s.recv(10)))
try:
    s.recv(10, socket.MSG_DONTWAIT)
except BlockingIOError as e:
    show("MSG_DONTWAIT", e.errno)
srv.sendall(b"0123456789")
show("MSG_WAITALL", s.recv(10, socket.MSG_WAITALL))
buf = bytearray(8)
srv.sendall(b"into a buffer")
n = s.recv_into(buf)
show("recv_into", (n, bytes(buf[:n]), s.recv(100)))
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
show("TCP_NODELAY", s.getsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY) != 0)
s.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
show("SO_KEEPALIVE", s.getsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE) != 0)
show("SO_TYPE", (s.getsockopt(socket.SOL_SOCKET, socket.SO_TYPE) == socket.SOCK_STREAM, s.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)))
show("fileno", (s.fileno() > 2, s.get_inheritable()))
f = s.makefile("rwb")
srv.sendall(b"line one\nline two\n")
show("makefile readline", (f.readline(), f.readline()))
f.write(b"from makefile\n")
f.flush()
show("makefile write", srv.recv(100))
srv.close()
show("peer closed", s.recv(10))
try:
    for _ in range(100):                                            # the first send is accepted; the peer's reset arrives a moment later
        s.send(b"to nobody")
        time.sleep(0.05)
except (BrokenPipeError, ConnectionResetError) as e:
    show("send to a closed peer", type(e).__name__)
f.close()
s.close()
l2.close()
try:
    s.send(b"x")
except OSError as e:
    show("closed socket", e.errno)
a = socket.socket()
b = a.dup()
show("dup", (a.fileno() != b.fileno(), b.family == a.family))
a.close()
b.close()

print("--- UDP")
u1 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
u1.bind(("127.0.0.1", 0))
u2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
u2.settimeout(30)
u2.sendto(b"datagram one", u1.getsockname())
data, addr = u1.recvfrom(100)
show("recvfrom", (data, addr[0], addr[1] == u2.getsockname()[1]))
u1.sendto(b"reply", addr)
show("reply", u2.recvfrom(100)[0])
u2.sendto(b"x" * 10, u1.getsockname())
show("truncated", u1.recvfrom(4)[0])
u2.connect(u1.getsockname())
u2.send(b"connected udp")
show("connected udp", u1.recv(100))
u1.setblocking(False)
try:
    u1.recv(10)
except BlockingIOError as e:
    show("empty datagram queue", e.errno)
show("broadcast option", u2.getsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST))
u1.close()
u2.close()

print("--- socketpair and UNIX-domain sockets")
p, q = socket.socketpair()
show("socketpair", (p.family.name, p.type.name, p.getsockname() == "" or p.getsockname() == b""))
p.sendall(b"ping")
show("pair recv", q.recv(10))
q.sendall(b"pong")
show("pair reply", p.recv(10))
p.close()
show("pair eof", q.recv(10))
q.close()
p, q = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
p.send(b"dgram 1")
p.send(b"dgram 2")
show("unix datagrams", (q.recv(100), q.recv(100)))
p.close()
q.close()
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind("unix.sock")
srv.listen(1)
show("socket file", (os.path.exists("unix.sock"), oct(os.stat("unix.sock").st_mode & 0o170000)))
show("getsockname", srv.getsockname())
cl = socket.socket(socket.AF_UNIX)
cl.connect("unix.sock")
conn, addr = srv.accept()
conn.sendall(b"over a path")
show("unix stream", (cl.recv(100), repr(addr)))
cred = cl.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i"))
pid, uid, gid = struct.unpack("3i", cred)
show("SO_PEERCRED", (pid == os.getpid(), uid == os.getuid(), gid == os.getgid()))
conn.close()
cl.close()
srv.close()
try:
    socket.socket(socket.AF_UNIX).connect("unix.sock")
except (ConnectionRefusedError, FileNotFoundError) as e:
    show("connect to a stale socket file", type(e).__name__)
os.unlink("unix.sock")
try:
    socket.socket(socket.AF_UNIX).connect("unix.sock")
except FileNotFoundError as e:
    show("connect to a missing path", e.errno)
srv = socket.socket(socket.AF_UNIX)
srv.bind("\0abstract-name-for-the-test")
srv.listen(1)
show("abstract name", srv.getsockname())
cl = socket.socket(socket.AF_UNIX)
cl.connect("\0abstract-name-for-the-test")
conn, _ = srv.accept()
cl.sendall(b"abstract")
show("abstract stream", conn.recv(100))
conn.close()
cl.close()
srv.close()
try:
    s2 = socket.socket(socket.AF_UNIX)
    s2.bind("unix2.sock")
    s3 = socket.socket(socket.AF_UNIX)
    s3.bind("unix2.sock")
except OSError as e:
    show("bind twice", errno.errorcode[e.errno])
os.unlink("unix2.sock")

print("--- passing descriptors")
p, q = socket.socketpair()
r, w = os.pipe()
socket.send_fds(p, [b"here are two descriptors"], [r, w])
msg, fds, flags, addr = socket.recv_fds(q, 100, 4)
show("send_fds", (msg, len(fds), fds[0] != r))
os.write(fds[1], b"through the passed pipe")
show("the passed descriptor works", os.read(r, 100))
for fd in (r, w, *fds):
    os.close(fd)
p.close()
q.close()

print("--- addresses and names")
show("getaddrinfo", sorted(set(ai[4] for ai in socket.getaddrinfo("localhost", 8080, family=socket.AF_INET, type=socket.SOCK_STREAM))))
show("gethostbyname", socket.gethostbyname("localhost"))
show("services and protocols", (socket.getservbyname("http"), socket.getservbyname("https", "tcp"), socket.getservbyport(25), socket.getprotobyname("tcp"), socket.getprotobyname("udp")))
show("getaddrinfo numeric", socket.getaddrinfo("127.0.0.1", "80", family=socket.AF_INET, type=socket.SOCK_STREAM, flags=socket.AI_NUMERICHOST)[0][4])
show("AI_PASSIVE", socket.getaddrinfo(None, 9, family=socket.AF_INET, type=socket.SOCK_DGRAM, flags=socket.AI_PASSIVE)[0][4])
try:
    socket.getaddrinfo("no-such-host.invalid", 80, family=socket.AF_INET)
except socket.gaierror as e:
    show("gaierror", type(e).__name__)
show("inet_aton/ntoa", (socket.inet_aton("10.1.2.3"), socket.inet_ntoa(b"\xc0\xa8\x00\x01")))
show("inet_pton/ntop", (socket.inet_pton(socket.AF_INET6, "::1"), socket.inet_ntop(socket.AF_INET6, b"\x00" * 15 + b"\x01")))
show("byte order", (socket.htons(1), socket.ntohs(256), socket.htonl(1), socket.ntohl(1 << 24)))
show("constants", (socket.AF_INET, socket.AF_UNIX, socket.SOCK_STREAM, socket.SOCK_DGRAM, socket.SOMAXCONN > 0, socket.has_ipv6))
show("hostname", isinstance(socket.gethostname(), str))
try:
    socket.inet_aton("not an address")
except OSError as e:
    show("bad address", type(e).__name__)
show("create_server", socket.create_server(("127.0.0.1", 0)).getsockname()[0])

print("--- sendfile and files")
with open("payload.bin", "wb") as f:
    f.write(bytes(range(256)) * 1000)
p, q = socket.socketpair()
sent = []


def send_the_file():                                                # more than a socket holds: the reader below must run at the same time
    with open("payload.bin", "rb") as f:
        sent.append(p.sendfile(f))
    p.close()


st = threading.Thread(target=send_the_file)
st.start()
data = b""
while True:
    chunk = q.recv(65536)
    if not chunk:
        break
    data += chunk
st.join()
show("sendfile", (sent, len(data), data == open("payload.bin", "rb").read()))
q.close()

print("--- socketserver and an HTTP server")


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        for line in self.rfile:
            self.wfile.write(line.upper())


with socketserver.ThreadingTCPServer(("127.0.0.1", 0), Handler) as server:
    server.daemon_threads = True
    st = threading.Thread(target=server.serve_forever)
    st.start()
    with socket.create_connection(server.server_address) as cc:
        cc.sendall(b"line one\nline two\n")
        cc.shutdown(socket.SHUT_WR)
        show("socketserver", cc.makefile().read())
    server.shutdown()
    st.join()


class Web(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = f"you asked for {self.path}".encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"]))
        self.send_response(201)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body[::-1])

    def log_message(self, *args):
        pass


httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Web)
st = threading.Thread(target=httpd.serve_forever)
st.start()
base = f"http://127.0.0.1:{httpd.server_address[1]}"
with urllib.request.urlopen(base + "/hello/world?x=1") as resp:
    show("GET", (resp.status, resp.headers["Content-Type"], resp.read()))
req = urllib.request.Request(base + "/post", data=b"reverse me", method="POST")
with urllib.request.urlopen(req) as resp:
    show("POST", (resp.status, resp.read()))
big = b"y" * 300000
req = urllib.request.Request(base + "/big", data=big, method="POST")
with urllib.request.urlopen(req) as resp:
    show("big POST", (resp.status, len(resp.read())))
httpd.shutdown()
httpd.server_close()
st.join()
print("--- done")
