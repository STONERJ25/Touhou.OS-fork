# Host-side differential and fuzz test for libz (NOT part of tests/run_all.py: it needs a Linux gcc, e.g. WSL, with
# AddressSanitizer). Run it from Linux:  ASAN_OPTIONS=detect_leaks=0 python3 -u tests/host/ztest.py
# It builds tests/host/ztool.c against user/lib/z with -fsanitize=address,undefined, then compares with Python's zlib/gzip.
import os, random, subprocess, sys, zlib, gzip, io

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
SCR = os.path.join(REPO, "build", "host-z")
os.makedirs(SCR, exist_ok=True)
ZT = SCR + "/ztool"


def build():
    cmd = ["gcc", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-Wall", "-Wextra",
           "-Wno-unused-parameter", "-I", REPO + "/user/lib/z/include", REPO + "/tests/host/ztool.c",
           REPO + "/user/lib/z/src/crc.c", REPO + "/user/lib/z/src/inflate.c", REPO + "/user/lib/z/src/deflate.c", "-o", ZT]
    r = subprocess.run(cmd, capture_output=True, text=True)
    print(r.stdout, r.stderr)
    assert r.returncode == 0


def run(args, data):
    try:
        r = subprocess.run([ZT] + [str(a) for a in args], input=data, capture_output=True, timeout=40)
    except subprocess.TimeoutExpired:
        print("TIMEOUT", args, len(data), flush=True)
        return -999, b"", "timeout"
    return r.returncode, r.stdout, r.stderr.decode(errors="replace")


def samples():
    rnd = random.Random(7)
    yield b""
    yield b"a"
    yield b"hello hello hello hello hello"
    yield bytes(range(256)) * 3
    yield b"\x00" * 100000
    yield bytes(rnd.getrandbits(8) for _ in range(70000))
    words = [b"gensokyo", b"reimu", b"marisa", b"touhou", b"kappa", b"fairy", b"shrine", b"\n", b" "]
    yield b"".join(rnd.choice(words) for _ in range(40000))
    yield open(REPO + "/tools/build.py", "rb").read()
    yield open(REPO + "/user/lib/z/src/deflate.c", "rb").read() * 4
    yield bytes(rnd.choice(b"ab") for _ in range(100000))
    yield bytes((i * i) & 255 for i in range(200000))


def main():
    build()
    bad = 0
    for i, data in enumerate(samples()):
        for level in (0, 1, 3, 6, 9):
            for fmt, name in ((0, "raw"), (1, "zlib"), (2, "gzip")):
                for ic, oc in ((1 << 20, 1 << 20), (1000, 77), (1, 1) if len(data) < 300 else (4093, 5000)):
                    rc, out, err = run(["deflate", level, fmt, ic, oc], data)
                    if rc != 0:
                        print("DEFLATE FAIL", i, level, name, ic, oc, rc, err[-300:]); bad += 1; continue
                    # python must read it
                    try:
                        if fmt == 0:
                            back = zlib.decompress(out, -15)
                        elif fmt == 1:
                            back = zlib.decompress(out)
                        else:
                            back = gzip.decompress(out)
                    except Exception as e:
                        print("PY FAIL", i, level, name, ic, oc, e); bad += 1; continue
                    if back != data:
                        print("MISMATCH py", i, level, name); bad += 1; continue
                    # and our inflater must read it with small chunks
                    rc, back2, err = run(["inflate", 3 if fmt else 0, ic if ic < 5000 else 4096, oc if oc < 5000 else 4096], out)
                    if rc != 0 or back2 != data:
                        print("INFLATE FAIL", i, level, name, ic, oc, rc, err[-300:]); bad += 1
            # ratio report
        rc, out, err = run(["deflate", 6, 1, 65536, 65536], data)
        py = zlib.compress(data, 6)
        print(f"sample {i}: {len(data)} -> ours {len(out)}  zlib {len(py)}")
    # python-made streams through our inflater
    for i, data in enumerate(samples()):
        for level in (0, 1, 6, 9):
            for fmt, z in ((0, lambda d, l: zlib.compressobj(l, zlib.DEFLATED, -15).compress(d) + b""), ):
                co = zlib.compressobj(level, zlib.DEFLATED, -15)
                raw = co.compress(data) + co.flush()
                for ic in (1, 7, 4096, 1 << 20):
                    if ic == 1 and len(raw) > 5000: continue
                    rc, back, err = run(["inflate", 0, ic, 33 if ic < 100 else 1000], raw)
                    if rc != 0 or back != data:
                        print("INFLATE py-raw FAIL", i, level, ic, rc, err[-200:]); bad += 1
            zz = zlib.compress(data, level)
            rc, back, err = run(["inflate", 3, 4096, 4096], zz)
            if rc != 0 or back != data: print("INFLATE py-zlib FAIL", i, level, rc, err[-200:]); bad += 1
            gz = gzip.compress(data, min(level, 9) if level else 1)
            rc, back, err = run(["inflate", 3, 4096, 4096], gz)
            if rc != 0 or back != data: print("INFLATE py-gzip FAIL", i, level, rc, err[-200:]); bad += 1
    # fuzz: corrupt/truncate
    rnd = random.Random(99)
    data = open(REPO + "/user/lib/z/src/deflate.c", "rb").read()
    for fmt, blob in ((0, zlib.compressobj(6, zlib.DEFLATED, -15).compress(data)), (1, zlib.compress(data)), (2, gzip.compress(data))):
        co = zlib.compressobj(6, zlib.DEFLATED, -15)
        raw = co.compress(data) + co.flush()
        blob = raw if fmt == 0 else blob
        for t in range(150):
            b = bytearray(blob)
            mode = rnd.choice(("flip", "trunc", "junk"))
            if mode == "flip":
                for _ in range(rnd.randint(1, 4)):
                    b[rnd.randrange(len(b))] ^= 1 << rnd.randrange(8)
            elif mode == "trunc":
                del b[rnd.randrange(len(b)):]
            else:
                b[rnd.randrange(len(b)):] = os.urandom(rnd.randint(1, 50))
            rc, out, err = run(["inflate", 3 if fmt else 0, rnd.choice((1, 100, 4096)), rnd.choice((1, 50, 4096))], bytes(b))
            if rc not in (0, 1, 3) or "Sanitizer" in err or "runtime error" in err:
                print("FUZZ CRASH", fmt, t, mode, rc, err[-500:]); bad += 1
    print("bad:", bad)
    return bad


sys.exit(1 if main() else 0)
