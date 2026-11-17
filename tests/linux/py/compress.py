"""The zlib, gzip, bz2, lzma, zipfile, tarfile and shutil modules: compressing and decompressing in memory and through files, streaming, the formats' headers and checksums, archives of
a small tree with its modes, links and times, and the errors of damaged data."""
import bz2
import gzip
import io
import lzma
import os
import shutil
import stat
import tarfile
import zipfile
import zlib


def show(label, value=""):
    print(f"{label}: {value}")


text = (b"It was the best of times, it was the worst of times. " * 200) + bytes(range(256)) * 20
show("input", (len(text), zlib.crc32(text)))

print("--- zlib")
for level in (0, 1, 6, 9):
    c = zlib.compress(text, level)
    show(f"level {level}", (len(c), zlib.decompress(c) == text))
show("known output", zlib.compress(b"hello hello hello hello").hex())
show("decompress", zlib.decompress(zlib.compress(b"x" * 100000)) == b"x" * 100000)
co = zlib.compressobj(level=9, wbits=-15)
parts = [co.compress(text[i:i + 1000]) for i in range(0, 5000, 1000)] + [co.flush()]
do = zlib.decompressobj(wbits=-15)
show("raw deflate streaming", (do.decompress(b"".join(parts)) == text[:5000], do.eof))
do = zlib.decompressobj()
c = zlib.compress(text)
out = b"".join(do.decompress(c[i:i + 7]) for i in range(0, len(c), 7))
show("decompress in 7-byte pieces", (out == text, do.eof, do.unused_data))
show("max_length", len(zlib.decompressobj().decompress(c, 100)))
for bad in (b"not zlib data", c[:-4], c[:10] + b"\x00" + c[11:]):
    try:
        zlib.decompress(bad)
    except zlib.error as e:
        show("zlib.error", str(e).split(":")[0] if ":" in str(e) else str(e)[:40])
show("version", (zlib.ZLIB_VERSION[:2], zlib.Z_BEST_COMPRESSION))
big = bytes(range(251)) * 40000
show("10 MB", (len(zlib.compress(big, 1)), zlib.decompress(zlib.compress(big, 1)) == big))

print("--- gzip")
g = gzip.compress(text, mtime=0)
show("gzip bytes", (len(g), g[:4].hex(), g[-8:-4].hex(), gzip.decompress(g) == text))
with gzip.open("file.gz", "wb", compresslevel=6) as f:
    f.write(b"line one\n")
    f.write(b"line two\n")
with gzip.open("file.gz", "rt") as f:
    show("gzip file", f.read())
with gzip.GzipFile("file.gz") as f:
    show("readline and seek", (f.readline(), f.tell(), f.seek(0), f.read(4)))
with open("file.gz", "ab") as f:
    f.write(gzip.compress(b"third\n"))
with gzip.open("file.gz", "rb") as f:
    show("two members", f.read())
try:
    gzip.decompress(g[:20])
except (EOFError, OSError) as e:
    show("truncated gzip", type(e).__name__)
try:
    gzip.decompress(b"plain text")
except OSError as e:
    show("not gzip", e)
show("size on disk", os.path.getsize("file.gz") > 20)

print("--- bz2 and lzma")
b = bz2.compress(text)
show("bz2", (len(b), b[:3], bz2.decompress(b) == text))
bo = bz2.BZ2Decompressor()
show("bz2 streaming", (b"".join(bo.decompress(b[i:i + 50]) for i in range(0, len(b), 50)) == text, bo.eof))
with bz2.open("file.bz2", "wt") as f:
    f.write("bzip2 text\n" * 100)
with bz2.open("file.bz2", "rt") as f:
    show("bz2 file", (len(f.read()), os.path.getsize("file.bz2") < 200))
x = lzma.compress(text)
show("lzma", (len(x), x[:6].hex(), lzma.decompress(x) == text))
show("lzma raw", lzma.decompress(lzma.compress(text, format=lzma.FORMAT_RAW, filters=[{"id": lzma.FILTER_LZMA2, "preset": 3}]), format=lzma.FORMAT_RAW, filters=[{"id": lzma.FILTER_LZMA2}]) == text)
with lzma.open("file.xz", "wb") as f:
    f.write(text)
with lzma.open("file.xz", "rb") as f:
    show("xz file", f.read() == text)
for mod, bad in ((bz2, b"BZh9 nothing"), (lzma, b"\xfd7zXZ\x00 broken")):
    try:
        mod.decompress(bad)
    except (OSError, ValueError, lzma.LZMAError) as e:
        show(f"{mod.__name__} error", type(e).__name__)

print("--- zip")
os.makedirs("src/sub/deeper")
files = {"src/a.txt": b"alpha\n" * 100, "src/sub/b.bin": bytes(range(256)) * 10, "src/sub/deeper/c.txt": b"gamma", "src/empty": b""}
for name, data in files.items():
    with open(name, "wb") as f:
        f.write(data)
os.chmod("src/a.txt", 0o640)
os.chmod("src/sub/b.bin", 0o755)
os.utime("src/a.txt", (1_700_000_000, 1_700_000_000))
with zipfile.ZipFile("test.zip", "w", zipfile.ZIP_DEFLATED) as z:
    for name in files:
        z.write(name)
    z.writestr("generated.txt", "made in memory\n" * 10)
    z.writestr(zipfile.ZipInfo("fixed-date.txt", date_time=(2020, 2, 29, 12, 34, 56)), "date\n")
    z.mkdir("dir-entry")
with zipfile.ZipFile("test.zip") as z:
    show("namelist", z.namelist())
    show("sizes", [(i.filename, i.file_size, i.compress_type) for i in z.infolist()][:4])
    show("read", (z.read("src/sub/deeper/c.txt"), z.read("generated.txt")[:14]))
    show("testzip", z.testzip())
    info = z.getinfo("fixed-date.txt")
    show("date_time", info.date_time)
    show("unix mode", oct((z.getinfo("src/sub/b.bin").external_attr >> 16) & 0o7777))
    z.extractall("unzipped")
show("extracted", sorted(os.path.relpath(os.path.join(d, f), "unzipped") for d, _, fs in os.walk("unzipped") for f in fs))
show("extracted content", open("unzipped/src/a.txt", "rb").read() == files["src/a.txt"])
with zipfile.ZipFile("stored.zip", "w", zipfile.ZIP_STORED) as z:
    z.writestr("x", b"stored " * 10)
show("stored size", (os.path.getsize("stored.zip"), zipfile.is_zipfile("stored.zip"), zipfile.is_zipfile("file.gz")))
with zipfile.ZipFile("bz.zip", "w", zipfile.ZIP_BZIP2) as z:
    z.writestr("x", b"bzip " * 100)
with zipfile.ZipFile("xz.zip", "w", zipfile.ZIP_LZMA) as z:
    z.writestr("x", b"lzma " * 100)
show("bzip2 and lzma members", (zipfile.ZipFile("bz.zip").read("x")[:5], zipfile.ZipFile("xz.zip").read("x")[:5]))
bad = bytearray(open("test.zip", "rb").read())
bad[60] ^= 0xFF
open("bad.zip", "wb").write(bad)
try:
    with zipfile.ZipFile("bad.zip") as z:
        z.testzip()
        for n in z.namelist():
            z.read(n)
    show("damaged zip", "no error")
except (zipfile.BadZipFile, zlib.error) as e:
    show("damaged zip", type(e).__name__)
buf = io.BytesIO()
with zipfile.ZipFile(buf, "w") as z:
    z.writestr("in/memory.txt", "no file involved")
show("zip in memory", (len(buf.getvalue()) > 50, zipfile.ZipFile(io.BytesIO(buf.getvalue())).namelist()))

print("--- tar")
os.symlink("a.txt", "src/link")
os.link("src/a.txt", "src/hard")
with tarfile.open("test.tar.gz", "w:gz") as t:
    t.add("src")
with tarfile.open("test.tar.gz") as t:
    names = t.getnames()
    show("names", sorted(names))
    members = {m.name: m for m in t.getmembers()}
    show("modes", [(n, oct(members[n].mode)) for n in ("src/a.txt", "src/sub/b.bin", "src/sub")])
    show("types", [(n, members[n].isreg(), members[n].isdir(), members[n].issym(), members[n].islnk()) for n in ("src/a.txt", "src/sub", "src/link", "src/hard")])
    show("link names", (members["src/link"].linkname, members["src/hard"].linkname))
    show("mtime", members["src/a.txt"].mtime)
    show("extractfile", t.extractfile("src/sub/deeper/c.txt").read())
    t.extractall("untarred", filter="data")
show("untarred tree", sorted(os.path.relpath(os.path.join(d, f), "untarred") for d, _, fs in os.walk("untarred") for f in fs))
show("extracted modes", (oct(os.stat("untarred/src/a.txt").st_mode & 0o777), oct(os.stat("untarred/src/sub/b.bin").st_mode & 0o777)))
show("extracted link", (os.readlink("untarred/src/link"), os.stat("untarred/src/hard").st_nlink, int(os.stat("untarred/src/a.txt").st_mtime)))
for mode, name in (("w:", "plain.tar"), ("w:bz2", "t.tar.bz2"), ("w:xz", "t.tar.xz")):
    with tarfile.open(name, mode) as t:
        t.add("src/sub")
    with tarfile.open(name) as t:
        show(f"{name}", (sorted(t.getnames()), os.path.getsize(name) > 0))
buf = io.BytesIO()
with tarfile.open(fileobj=buf, mode="w") as t:
    ti = tarfile.TarInfo("virtual/file.txt")
    ti.size = 11
    ti.mtime = 1_000_000_000
    ti.mode = 0o600
    ti.uname = "nobody"
    t.addfile(ti, io.BytesIO(b"hello world"))
show("tar in memory", (len(buf.getvalue()), buf.getvalue()[:16], tarfile.open(fileobj=io.BytesIO(buf.getvalue())).extractfile("virtual/file.txt").read()))
with tarfile.open("stream.tar.gz", "w|gz") as t:
    t.add("src/a.txt")
with tarfile.open("stream.tar.gz", "r|gz") as t:
    show("streaming", [m.name for m in t])
try:
    tarfile.open("file.gz")
except tarfile.ReadError as e:
    show("not a tar file", type(e).__name__)
with tarfile.open("evil.tar", "w") as t:
    ti = tarfile.TarInfo("../../escape")
    ti.size = 1
    t.addfile(ti, io.BytesIO(b"x"))
try:
    with tarfile.open("evil.tar") as t:
        t.extractall("safe", filter="data")
except tarfile.FilterError as e:
    show("the data filter refuses ..", type(e).__name__)

print("--- shutil")
shutil.make_archive("made", "gztar", root_dir="src")
show("make_archive", tarfile.open("made.tar.gz").getnames()[:3])
shutil.make_archive("madezip", "zip", root_dir="src")
show("zip archive", sorted(zipfile.ZipFile("madezip.zip").namelist()))
shutil.unpack_archive("made.tar.gz", "unpacked", filter="data")
show("unpack_archive", sorted(os.listdir("unpacked")))
shutil.copytree("src", "copy")
show("copytree", (sorted(os.listdir("copy")), oct(os.stat("copy/a.txt").st_mode & 0o777), os.path.islink("copy/link")))
shutil.copy2("src/a.txt", "stamped")
show("copy2 keeps the time", int(os.stat("stamped").st_mtime))
shutil.move("stamped", "moved")
show("move", (os.path.exists("stamped"), os.path.exists("moved")))
shutil.rmtree("copy")
show("rmtree", os.path.exists("copy"))
show("disk_usage", shutil.disk_usage(".").total > 0)
show("which", (shutil.which("sh"), shutil.which("no-such-program")))
show("get_terminal_size", tuple(shutil.get_terminal_size((80, 24))))
print("--- done")
