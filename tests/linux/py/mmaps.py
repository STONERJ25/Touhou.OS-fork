"""The mmap module: anonymous and file-backed mappings, reading and writing through a mapping and through the file, private copies, partial maps at an offset, resizing, searching,
and memory shared between a process and its children (anonymous and from a file in the working directory)."""
import mmap
import os
import struct
import time


def show(label, value=""):
    print(f"{label}: {value}")


show("page size", (mmap.PAGESIZE, mmap.ALLOCATIONGRANULARITY))

print("--- anonymous")
m = mmap.mmap(-1, 4096)
show("size", (len(m), m.closed, m.tell()))
m.write(b"hello")
show("write and read back", (m.tell(), m[:5], m.read(3), m.tell()))
m.seek(0)
m.write(b"line one\nline two\n")
m.seek(0)
show("read", (m.read(5), m.readline(), m.readline(), m.tell()))
m[100:105] = b"world"
show("slice assignment", (m[98:107], m.find(b"world", 0), m.find(b"nothing", 0), m.rfind(b"o", 0, 4096), m.find(b"line", 5)))
m[200] = 65
show("item", (m[200], m[199]))
show("tell/seek", (m.seek(20), m.tell(), m.seek(-1, os.SEEK_END), m.seek(-5, os.SEEK_CUR)))
try:
    m.seek(5000)
except ValueError as e:
    show("seek beyond the end", e)
try:
    m[4096] = 1
except IndexError as e:
    show("index out of range", e)
try:
    m[0:3] = b"toolong"
except IndexError as e:
    show("slice of another size", type(e).__name__)
m.seek(0)
m.write(b"ab" * 2048)
show("written to the end", m.tell())
try:
    m.write(b"x")
except ValueError as e:
    show("write past the end", e)
m.move(0, 4, 8)
show("move", m[:12])
show("zero page", mmap.mmap(-1, 8192)[4000:4005])
m.close()
m = mmap.mmap(-1, 4096, flags=mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS)       # a shared anonymous map cannot grow (the pages beyond its first size are not backed)
m[:4] = b"abcd"
m[4090:4096] = b"123456"
m.resize(8192)
show("resize", (len(m), m[:4], m[4090:4100], m[8000:8004]))
m.resize(2048)
show("shrink", (len(m), m[:4]))
m.close()
show("closed", m.closed)
try:
    m[0]
except ValueError as e:
    show("use after close", e)
try:
    mmap.mmap(-1, 0)
except (ValueError, OSError) as e:
    show("zero length", (type(e).__name__, getattr(e, "errno", None)))
a = mmap.mmap(-1, 1 << 20)
a[0:4] = b"head"
a[(1 << 20) - 4:] = b"tail"
show("1 MB", (a[:4], a[-4:], sum(a[1 << 19:(1 << 19) + 4096])))
a.close()

print("--- a file")
with open("data.bin", "wb") as f:
    f.write(b"0123456789abcdef" * 512)
with open("data.bin", "rb") as f:
    r = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    show("read-only", (len(r), r[:16], r[8190:8200], r.find(b"cdef0")))
    try:
        r[0] = 1
    except TypeError as e:
        show("write to a read-only map", e)
    f.seek(5)
    show("the file position is not used", (r.tell(), f.read(3)))
    r.close()
with open("data.bin", "r+b") as f:
    w = mmap.mmap(f.fileno(), 0)
    w[0:6] = b"CHANGE"
    w.flush()
    f.seek(0)
    show("write through the map, read through the file", f.read(10))
    f.seek(100)
    f.write(b"BYFILE")
    f.flush()
    show("write through the file, read through the map", w[96:110])
    w.close()
with open("data.bin", "r+b") as f:
    c = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_COPY)
    c[0:4] = b"COPY"
    show("copy-on-write", (c[:8], os.pread(f.fileno(), 8, 0)))
    c.close()
    off = mmap.ALLOCATIONGRANULARITY
    p = mmap.mmap(f.fileno(), 100, offset=off)
    show("offset", (p[:20] == (b"0123456789abcdef" * 512)[off:off + 20], len(p)))
    p.close()
    try:
        mmap.mmap(f.fileno(), 100, offset=100)
    except (OSError, ValueError) as e:
        show("unaligned offset", type(e).__name__)
    try:
        mmap.mmap(f.fileno(), 1 << 20, access=mmap.ACCESS_READ)
    except ValueError as e:
        show("longer than the file", e)
    g = mmap.mmap(f.fileno(), 0)
    f.truncate(4096)
    show("file shrunk below the map", "ok")
    g.close()
    f.truncate(2 * 8192)
    f.seek(0)
    show("file grown", (len(f.read()), os.fstat(f.fileno()).st_size))
with open("empty.bin", "wb") as f:
    pass
try:
    with open("empty.bin", "r+b") as f:
        mmap.mmap(f.fileno(), 0)
except ValueError as e:
    show("empty file", e)
with open("grow.bin", "w+b") as f:
    f.truncate(10000)
    g = mmap.mmap(f.fileno(), 10000)
    struct.pack_into("<IQ", g, 4000, 0xCAFEBABE, 2 ** 40 + 7)
    show("struct", struct.unpack_from("<IQ", g, 4000)[0] == 0xCAFEBABE)
    g.flush(0, 8192)
    g.madvise(mmap.MADV_NORMAL)
    g.madvise(mmap.MADV_SEQUENTIAL)
    g.madvise(mmap.MADV_WILLNEED)
    show("madvise", "ok")
    g.close()
    f.seek(4000)
    show("written to the file", struct.unpack("<IQ", f.read(12)) == (0xCAFEBABE, 2 ** 40 + 7))

print("--- shared with a child")
shared = mmap.mmap(-1, 4096)
shared[:8] = b"parent  "
pid = os.fork()
if pid == 0:
    shared[:8] = b"child   "
    shared[100:104] = struct.pack("<I", 1234)
    os._exit(0)
os.waitpid(pid, 0)
show("anonymous memory is shared", (shared[:8], struct.unpack("<I", shared[100:104])[0]))
private = mmap.mmap(-1, 4096, flags=mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS)
private[:8] = b"parent  "
pid = os.fork()
if pid == 0:
    private[:8] = b"child   "
    os._exit(0)
os.waitpid(pid, 0)
show("MAP_PRIVATE memory is copied", private[:8])
with open("shared.bin", "w+b") as f:
    f.truncate(4096)
    fm = mmap.mmap(f.fileno(), 4096)
    pid = os.fork()
    if pid == 0:
        fm[:5] = b"child"
        os._exit(0)
    os.waitpid(pid, 0)
    show("a mapped file is shared with the child", (fm[:5], os.pread(f.fileno(), 5, 0)))
    counter = mmap.mmap(-1, 4096)
    pids = []
    for n in range(4):
        pid = os.fork()
        if pid == 0:
            for _ in range(100):
                # a crude atomic increment is not available: each child owns its own slot
                counter[n * 8:(n + 1) * 8] = struct.pack("<Q", struct.unpack("<Q", counter[n * 8:(n + 1) * 8])[0] + 1)
            os._exit(0)
        pids.append(pid)
    for pid in pids:
        os.waitpid(pid, 0)
    show("four children, four slots", [struct.unpack("<Q", counter[n * 8:(n + 1) * 8])[0] for n in range(4)])
flag = mmap.mmap(-1, 16)
pid = os.fork()
if pid == 0:
    deadline = time.monotonic() + 60
    while flag[0] == 0 and time.monotonic() < deadline:
        time.sleep(0.01)
    flag[1] = flag[0] + 1
    os._exit(0)
time.sleep(0.1)
flag[0] = 41
os.waitpid(pid, 0)
show("a flag set after the fork reaches the child", flag[1])
print("--- done")
