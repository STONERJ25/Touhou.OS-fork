"""The select, selectors and epoll interfaces: pipes, sockets and listening sockets with time-outs, level- and edge-triggered epoll, poll flags, the selectors classes, and a small
event loop that serves several clients from one thread."""
import os
import select
import selectors
import socket
import threading
import time


def show(label, value=""):
    print(f"{label}: {value}")


def flags(mask):
    names = [n for n in ("POLLIN", "POLLPRI", "POLLOUT", "POLLERR", "POLLHUP", "POLLNVAL", "POLLRDHUP") if mask & getattr(select, n)]
    return "|".join(names) or "0"


print("--- select")
r, w = os.pipe()
show("nothing to read", select.select([r], [], [], 0))
show("writable", select.select([], [w], [], 0) == ([], [w], []))
os.write(w, b"data")
show("readable", select.select([r], [], [], 5) == ([r], [], []))
t0 = time.monotonic()
show("time-out", select.select([os.pipe()[0]], [], [], 0.2) == ([], [], []))
show("time-out took a while", time.monotonic() - t0 >= 0.15)
os.read(r, 10)
os.close(w)
show("EOF is readable", select.select([r], [], [], 5) == ([r], [], []))
try:
    select.select([999], [], [], 0)
except (OSError, ValueError) as e:
    show("bad descriptor", type(e).__name__)
try:
    select.select([r], [], [], -1)
except ValueError as e:
    show("negative time-out", e)
os.close(r)
a, b = socket.socketpair()
show("socket select", (select.select([a], [], [], 0)[0], select.select([], [a], [], 0)[1] == [a]))
b.send(b"x")
show("socket readable", select.select([a, b], [], [], 5)[0] == [a])
rl, wl, xl = select.select([a, b], [a, b], [a, b], 5)
show("three lists", (a in rl, b in rl, len(wl), xl))
a.close()
b.close()

print("--- poll")
p = select.poll()
r, w = os.pipe()
p.register(r, select.POLLIN)
p.register(w, select.POLLOUT)
show("poll pipe", sorted((fd == r, flags(m)) for fd, m in p.poll(1000)))
os.write(w, b"hello")
show("poll after a write", sorted((fd == r, flags(m)) for fd, m in p.poll(1000)))
p.modify(w, select.POLLIN)
show("modified", sorted((fd == r, flags(m)) for fd, m in p.poll(1000)))
os.close(w)
show("hangup", sorted((fd == r, flags(m)) for fd, m in p.poll(1000)))
os.read(r, 100)
show("hangup after reading", [(flags(m)) for fd, m in p.poll(1000) if fd == r])
p.unregister(r)
try:
    p.unregister(r)
except KeyError:
    show("unregister twice", "KeyError")
p = select.poll()
p.register(995, select.POLLIN)
show("POLLNVAL", [(flags(m)) for fd, m in p.poll(100)])
os.close(r)
l = socket.socket()
l.bind(("127.0.0.1", 0))
l.listen(1)
p = select.poll()
p.register(l, select.POLLIN)
show("listener idle", p.poll(50))
c = socket.create_connection(l.getsockname())
show("listener with a pending connection", [flags(m) for fd, m in p.poll(5000)])
s, _ = l.accept()
p = select.poll()
p.register(c, select.POLLIN | select.POLLRDHUP)
s.close()
show("peer closed", [flags(m) for fd, m in p.poll(5000)])
c.close()
l.close()

print("--- epoll")
ep = select.epoll()
show("epoll object", (ep.fileno() > 2, ep.closed))
r, w = os.pipe()
ep.register(r, select.EPOLLIN)
ep.register(w, select.EPOLLOUT)
show("level-triggered", sorted((fd == w, flags(m)) for fd, m in ep.poll(5)))
os.write(w, b"abc")
ev = ep.poll(5)
show("readable too", sorted(flags(m) for fd, m in ev))
show("level-triggered repeats", len(ep.poll(5)))
os.read(r, 1)
show("still data left", sorted(flags(m) for fd, m in ep.poll(5) if fd == r))
os.read(r, 10)
show("drained", [flags(m) for fd, m in ep.poll(0.05) if fd == r])
ep.modify(w, select.EPOLLOUT | select.EPOLLET)
show("edge-triggered, first", [flags(m) for fd, m in ep.poll(5) if fd == w])
show("edge-triggered, second", [flags(m) for fd, m in ep.poll(0.05) if fd == w])
ep.unregister(w)
os.close(w)
show("EOF", [flags(m) for fd, m in ep.poll(5)])
ep.unregister(r)
os.close(r)
a, b = socket.socketpair()
ep.register(a, select.EPOLLIN | select.EPOLLET)
b.send(b"1")
show("edge-triggered socket", [flags(m) for fd, m in ep.poll(5)])
show("no new edge", ep.poll(0.05))
b.send(b"2")
show("a new edge after more data", [flags(m) for fd, m in ep.poll(5)])
ep.modify(a, select.EPOLLIN | select.EPOLLONESHOT)
show("one-shot", [flags(m) for fd, m in ep.poll(5)])
show("one-shot is disarmed", ep.poll(0.05))
ep.modify(a, select.EPOLLIN | select.EPOLLONESHOT)
show("re-armed", [flags(m) for fd, m in ep.poll(5)])
try:
    ep.register(a, select.EPOLLIN)
except FileExistsError as e:
    show("registered twice", e.errno)
try:
    ep.modify(b, select.EPOLLIN)
except FileNotFoundError as e:
    show("modify of an unregistered descriptor", e.errno)
ep.unregister(a)
a.close()
b.close()
show("epoll with a time-out", ep.poll(0.05))
show("maxevents", ep.poll(0.05, 1))
ep.close()
show("closed", ep.closed)
try:
    ep.poll(0)
except ValueError as e:
    show("poll on a closed epoll", e)
with select.epoll(flags=select.EPOLL_CLOEXEC) as ep:
    show("CLOEXEC", os.get_inheritable(ep.fileno()))
    other = select.epoll()
    ep.register(other.fileno(), select.EPOLLIN)
    show("an epoll inside an epoll", ep.poll(0.05))
    other.close()

print("--- selectors")
show("default selector", selectors.DefaultSelector.__name__)
for cls in (selectors.SelectSelector, selectors.PollSelector, selectors.EpollSelector):
    a, b = socket.socketpair()
    with cls() as sel:
        key = sel.register(a, selectors.EVENT_READ, data="payload")
        show(f"{cls.__name__} idle", sel.select(0))
        b.send(b"x")
        ready = sel.select(5)
        show(f"{cls.__name__} ready", [(k.data, e) for k, e in ready])
        sel.modify(a, selectors.EVENT_READ | selectors.EVENT_WRITE, data="changed")
        show(f"{cls.__name__} read and write", sorted(e for k, e in sel.select(5)))
        show(f"{cls.__name__} get_key", sel.get_key(a).data)
        sel.unregister(a)
        try:
            sel.get_key(a)
        except KeyError:
            show(f"{cls.__name__} unregistered", "KeyError")
    a.close()
    b.close()

print("--- an event loop serving ten clients from one thread")
server = socket.socket()
server.bind(("127.0.0.1", 0))
server.listen(20)
server.setblocking(False)
sel = selectors.DefaultSelector()
sel.register(server, selectors.EVENT_READ, "accept")
address = server.getsockname()
replies = {}


def client(n):
    with socket.create_connection(address) as c:
        c.sendall(f"client {n}\n".encode())
        replies[n] = c.makefile().readline()


threads = [threading.Thread(target=client, args=(n,)) for n in range(10)]
for t in threads:
    t.start()
served = 0
buffers = {}
deadline = time.monotonic() + 60
while served < 10 and time.monotonic() < deadline:
    for key, mask in sel.select(1):
        if key.data == "accept":
            conn, _ = server.accept()
            conn.setblocking(False)
            sel.register(conn, selectors.EVENT_READ, "serve")
            buffers[conn] = b""
        else:
            conn = key.fileobj
            data = conn.recv(100)
            if not data:
                sel.unregister(conn)
                conn.close()
                continue
            buffers[conn] += data
            if buffers[conn].endswith(b"\n"):
                conn.sendall(buffers[conn].upper())
                sel.unregister(conn)
                conn.close()
                served += 1
for t in threads:
    t.join()
show("served", (served, [replies[n] for n in range(10)]))
sel.close()
server.close()
print("--- done")
