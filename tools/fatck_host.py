#!/usr/bin/env python3
"""fatck_host -- the kernel's FAT checker (shared/fatck.c) and the fsck repair engine (shared/fatfix.c) on the host.

The same C source that the kernel and /bin/fsck use is built into a freestanding Windows DLL (a shared object on Linux) with the
clang/lld that tools/build.py already needs, and driven through ctypes. That makes it cheap to hold the C checker against tools/mkdisk.py's independent Python
checker over hundreds of damaged images (tests/case_fsck.py does so), without booting a VM for each.

  python tools/fatck_host.py IMAGE[:N]            scan partition N of an image (default: the first FAT volume) and print the findings
  python tools/fatck_host.py -a IMAGE[:N]         repair what is safe, then scan again       (-y: repair everything)
"""
import ctypes
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SHARED = ROOT / "shared"
OUT = ROOT / "build" / "fatck-host"
LLVM = Path(r"C:\Program Files\LLVM\bin")
WINDOWS = os.name == "nt"

FS_NOTE, FS_WARN, FS_LEAK, FS_ERR = range(4)
SEV_NAMES = ["note", "warning", "leftover", "error"]


class HostBuildError(RuntimeError):
    pass


def _tool(name):
    # the same order as tools/build.py: TOUHOU_LLVM_DIR, then the PATH, then the Windows install folder
    dirs = [Path(os.environ["TOUHOU_LLVM_DIR"])] if os.environ.get("TOUHOU_LLVM_DIR") else []
    for d in dirs:
        for ext in ("", ".exe"):
            p = d / (name + ext)
            if p.is_file():
                return str(p)
    found = shutil.which(name)
    if found:
        return found
    for ext in ("", ".exe"):
        p = LLVM / (name + ext)
        if p.is_file():
            return str(p)
    raise HostBuildError(f"cannot find {name}")


def _sources():
    srcs = [SHARED / "fatck.c", ROOT / "tools" / "fatck_host_rt.c"]
    if (SHARED / "fatfix.c").exists():
        srcs.append(SHARED / "fatfix.c")
    return srcs


def build_dll():
    """Compiles and links the DLL (Windows) or shared object (elsewhere) if any source is newer than it; returns its path."""
    OUT.mkdir(parents=True, exist_ok=True)
    dll = OUT / ("fatck_host.dll" if WINDOWS else "fatck_host.so")
    srcs = _sources()
    headers = sorted(SHARED.glob("fat*.h"))
    newest = max(p.stat().st_mtime for p in srcs + headers + [Path(__file__)])
    if dll.exists() and dll.stat().st_mtime >= newest:
        return dll
    clang, lld = _tool("clang"), _tool("lld-link" if WINDOWS else "ld.lld")
    target = ["-target", "x86_64-unknown-windows"] if WINDOWS else ["-target", "x86_64-unknown-linux-gnu", "-fPIC"]
    objs = []
    for s in srcs:
        o = OUT / (s.stem + ".obj")
        cmd = [clang, *target, "-ffreestanding", "-fno-stack-protector", "-mno-red-zone", "-O2", "-g0",
               "-std=gnu17", "-Wall", "-Wextra", "-Wno-unused-parameter", "-DFATCK_REPAIR", "-DFATCK_HOST", "-I", str(SHARED),
               "-I", str(ROOT / "tools")]
        if s.name == "fatck_host_rt.c":
            cmd.append("-fno-builtin")
        cmd += ["-c", str(s), "-o", str(o)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            raise HostBuildError(f"compiling {s.name} failed:\n{r.stdout}{r.stderr}")
        if r.stderr.strip():
            sys.stderr.write(r.stderr)
        objs.append(o)
    exports = set()
    for h in headers:
        exports.update(re.findall(r"\b((?:fatck|fatfix)_\w+)\s*\(", h.read_text()))
    exports.update(["fatck_host_sizes"])
    if WINDOWS:
        cmd = [lld, "/dll", "/noentry", "/nodefaultlib", "/machine:x64", f"/out:{dll}"] + [f"/export:{e}" for e in sorted(exports)]
    else:
        # -Bsymbolic: the object's own mem*/strlen stay its own, whatever the host process has loaded
        cmd = [lld, "-shared", "-Bsymbolic", "-o", str(dll)]
    cmd += [str(o) for o in objs]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise HostBuildError(f"linking the DLL failed:\n{r.stdout}{r.stderr}")
    return dll


# ---------------------------------------------------------------------------------------------------------------------
# ctypes mirrors of shared/fatck.h

FK_NAMES = ["none", "bpb", "size", "fatcopy", "media", "dirty", "fsinfo", "backup", "dirchain", "dirbig", "dots", "garbage", "dirsize",
            "lfn", "lfnfield", "lfnpad", "name", "namecase", "attr", "date", "dup", "label", "range", "nocluster", "chain", "cross",
            "short", "long", "lost", "incomplete", "io"]
FK = {n: i for i, n in enumerate(FK_NAMES)}
FK_COUNT = len(FK_NAMES)


class Finding(ctypes.Structure):
    _fields_ = [("code", ctypes.c_uint16), ("sev", ctypes.c_uint8), ("fixable", ctypes.c_uint8), ("dir", ctypes.c_uint32),
                ("slot", ctypes.c_uint32), ("nslots", ctypes.c_uint32), ("cluster", ctypes.c_uint32), ("first", ctypes.c_uint32),
                ("size", ctypes.c_uint32), ("aux", ctypes.c_uint32), ("chain_len", ctypes.c_uint32), ("last", ctypes.c_uint32),
                ("attr", ctypes.c_uint8)]


class Result(ctypes.Structure):
    _fields_ = [("opened", ctypes.c_bool), ("incomplete", ctypes.c_bool), ("dirty", ctypes.c_bool), ("hard_error", ctypes.c_bool),
                ("bits", ctypes.c_int), ("nclusters", ctypes.c_uint32), ("cluster_bytes", ctypes.c_uint32),
                ("errors", ctypes.c_uint32), ("leaks", ctypes.c_uint32), ("warnings", ctypes.c_uint32), ("notes", ctypes.c_uint32),
                ("count", ctypes.c_uint32 * FK_COUNT), ("files", ctypes.c_uint32), ("dirs", ctypes.c_uint32),
                ("used", ctypes.c_uint32), ("free_clusters", ctypes.c_uint32), ("bad_clusters", ctypes.c_uint32),
                ("lost_clusters", ctypes.c_uint32), ("first_lost", ctypes.c_uint32 * 8), ("fat_diff", ctypes.c_uint32 * 4),
                ("max_depth", ctypes.c_uint32), ("file_bytes", ctypes.c_uint64)]


class Geom(ctypes.Structure):
    _fields_ = [("bits", ctypes.c_int), ("bps", ctypes.c_uint32), ("spc", ctypes.c_uint32), ("cluster_bytes", ctypes.c_uint32),
                ("reserved", ctypes.c_uint32), ("nfats", ctypes.c_uint32), ("fat_sectors", ctypes.c_uint32),
                ("root_entries", ctypes.c_uint32), ("nclusters", ctypes.c_uint32), ("root_cluster", ctypes.c_uint32),
                ("root_dir", ctypes.c_uint32), ("active_fat", ctypes.c_uint32), ("fsinfo_sector", ctypes.c_uint32),
                ("backup_sector", ctypes.c_uint32), ("fat_off", ctypes.c_uint64 * 4), ("fat_bytes", ctypes.c_uint64),
                ("root_off", ctypes.c_uint64), ("root_bytes", ctypes.c_uint64), ("data_off", ctypes.c_uint64),
                ("vol_bytes", ctypes.c_uint64), ("media", ctypes.c_uint8), ("mirrored", ctypes.c_bool)]


C = ctypes
READ_T = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_uint64, C.c_void_p, C.c_size_t)
WRITE_T = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_uint64, C.c_void_p, C.c_size_t)
FLUSH_T = C.CFUNCTYPE(C.c_int, C.c_void_p)
ALLOC_T = C.CFUNCTYPE(C.c_void_p, C.c_void_p, C.c_size_t)
RELEASE_T = C.CFUNCTYPE(None, C.c_void_p, C.c_void_p, C.c_size_t)
NOW_T = C.CFUNCTYPE(C.c_uint64, C.c_void_p)
REPORT_T = C.CFUNCTYPE(None, C.c_void_p, C.POINTER(Finding), C.c_char_p)


class Io(ctypes.Structure):
    _fields_ = [("read", READ_T), ("write", WRITE_T), ("flush", FLUSH_T), ("alloc", ALLOC_T), ("release", RELEASE_T), ("now_ms", NOW_T),
                ("report", REPORT_T), ("ctx", C.c_void_p), ("bytes", C.c_uint64), ("align", C.c_uint32), ("deadline_ms", C.c_uint64),
                ("max_windows", C.c_uint32), ("arena_bytes", C.c_uint32), ("max_depth", C.c_uint32), ("keep", C.c_uint32)]


FIX_NONE, FIX_SAFE, FIX_ALL = 0, 1, 2
SAY_T = C.CFUNCTYPE(None, C.c_void_p, C.c_char_p)
ASK_T = C.CFUNCTYPE(C.c_bool, C.c_void_p, C.c_int, C.c_char_p)


class FixOpts(ctypes.Structure):
    _fields_ = [("mode", C.c_int), ("free_lost", C.c_bool), ("ask", ASK_T), ("say", SAY_T), ("ctx", C.c_void_p),
                ("unix_time", C.c_uint64), ("max_passes", C.c_uint32)]


class FixStats(ctypes.Structure):
    _fields_ = [("passes", C.c_uint32), ("repaired", C.c_uint32), ("declined", C.c_uint32), ("failed", C.c_uint32),
                ("lost_chains", C.c_uint32), ("lost_clusters", C.c_uint32), ("freed_clusters", C.c_uint32),
                ("copied_clusters", C.c_uint32), ("wrote", C.c_bool), ("clean_now", C.c_bool), ("flags_cleared", C.c_bool)]


_lib = None


def lib():
    global _lib
    if _lib is None:
        dll = build_dll()
        _lib = C.CDLL(str(dll))
        sizes = (C.c_uint32 * 4)()
        _lib.fatck_host_sizes(sizes)
        want = (C.sizeof(Finding), C.sizeof(Result), C.sizeof(Geom), C.sizeof(Io))
        if tuple(sizes) != want:
            raise HostBuildError(f"ctypes mirrors disagree with the C structures: C {tuple(sizes)} vs Python {want}")
        _lib.fatck_open.argtypes = [C.POINTER(C.c_void_p), C.POINTER(Io)]
        _lib.fatck_open.restype = C.c_int
        for name in ("fatck_close",):
            getattr(_lib, name).argtypes = [C.c_void_p]
            getattr(_lib, name).restype = None
        _lib.fatck_scan.argtypes = [C.c_void_p]
        _lib.fatck_scan.restype = C.c_int
        _lib.fatck_result.argtypes = [C.c_void_p]
        _lib.fatck_result.restype = C.POINTER(Result)
        _lib.fatck_geom.argtypes = [C.c_void_p]
        _lib.fatck_geom.restype = C.POINTER(Geom)
        _lib.fatck_flush.argtypes = [C.c_void_p]
        _lib.fatck_flush.restype = C.c_int
        if hasattr(_lib, "fatfix_run"):
            _lib.fatfix_run.argtypes = [C.c_void_p, C.POINTER(FixOpts), C.POINTER(FixStats)]
            _lib.fatfix_run.restype = C.c_int
    return _lib


class ScanOutcome:
    """What one scan found. findings: [(Finding copy, text)]; result: the Result structure (copied)."""

    def __init__(self, rc, result, findings, geom):
        self.rc, self.result, self.findings, self.geom = rc, result, findings, geom

    @property
    def errors(self):
        return [t for f, t in self.findings if f.sev == FS_ERR]

    @property
    def leaks(self):
        return [t for f, t in self.findings if f.sev == FS_LEAK]

    @property
    def warnings(self):
        return [t for f, t in self.findings if f.sev == FS_WARN]

    @property
    def ok(self):
        """No damage (leftovers and warnings allowed) and the scan ran to the end."""
        return self.result.opened and not self.result.incomplete and self.result.errors == 0

    def codes(self):
        return {FK_NAMES[f.code] for f, _ in self.findings}

    def text(self):
        return "\n".join(f"{SEV_NAMES[f.sev]:8} {FK_NAMES[f.code]:12} {t}" for f, t in self.findings)


class Session:
    """One FAT volume of an image file (or any seekable binary file) opened for the C engine."""

    def __init__(self, path, offset=0, length=None, align=512, writable=False, deadline_ms=0, max_windows=64, arena=1 << 20, keep=0):
        self.f = open(path, "r+b" if writable else "rb")
        self.offset = offset
        self.f.seek(0, 2)
        self.length = (self.f.tell() - offset) if length is None else length
        self.align = align
        self.writable = writable
        self.findings = []
        self._mem = {}
        self._ck = C.c_void_p()
        self.reads = self.writes = 0
        L = lib()

        def read(_ctx, off, buf, n):
            try:
                self.f.seek(self.offset + off)
                data = self.f.read(n)
                if len(data) < n:
                    data += bytes(n - len(data))
                C.memmove(buf, data, n)
                self.reads += 1
                return 0
            except Exception:                               # noqa: BLE001 -- must not unwind through C
                return -5

        def write(_ctx, off, buf, n):
            try:
                self.f.seek(self.offset + off)
                self.f.write(C.string_at(buf, n))
                self.writes += 1
                return 0
            except Exception:                               # noqa: BLE001
                return -5

        def flush(_ctx):
            self.f.flush()
            return 0

        def alloc(_ctx, n):
            b = (C.c_char * n)()
            addr = C.addressof(b)
            self._mem[addr] = b
            return addr

        def release(_ctx, p, n):
            self._mem.pop(p, None)

        def report(_ctx, fp, text):
            fnd = Finding()
            C.memmove(C.addressof(fnd), fp, C.sizeof(Finding))
            self.findings.append((fnd, text.decode("utf-8", "replace")))

        self._cb = (READ_T(read), WRITE_T(write) if writable else WRITE_T(), FLUSH_T(flush), ALLOC_T(alloc), RELEASE_T(release),
                    NOW_T(), REPORT_T(report))
        self.io = Io(self._cb[0], self._cb[1], self._cb[2], self._cb[3], self._cb[4], self._cb[5], self._cb[6], None,
                     self.length, align, deadline_ms, max_windows, arena, 0, keep)
        self.rc_open = L.fatck_open(C.byref(self._ck), C.byref(self.io))

    @property
    def opened(self):
        return self.rc_open == 0

    def scan(self):
        L = lib()
        if not self.opened:
            return ScanOutcome(self.rc_open, Result(), list(self.findings), None)
        self.findings.clear()
        rc = L.fatck_scan(self._ck)
        res = Result()
        C.memmove(C.addressof(res), L.fatck_result(self._ck), C.sizeof(Result))
        geom = Geom()
        C.memmove(C.addressof(geom), L.fatck_geom(self._ck), C.sizeof(Geom))
        return ScanOutcome(rc, res, list(self.findings), geom)

    def fix(self, mode=FIX_SAFE, free_lost=False, unix_time=1767225600, max_passes=0):
        """Runs the repair engine (shared/fatfix.c). Returns (rc, FixStats, [lines it said], ScanOutcome of the last scan)."""
        L = lib()
        lines = []
        if not self.opened:                                    # the boot sector is unusable: nothing can be repaired
            return self.rc_open, FixStats(), lines, self.scan()

        def say(_ctx, text):
            lines.append(text.decode("utf-8", "replace"))

        say_cb = SAY_T(say)
        opts = FixOpts(mode, free_lost, ASK_T(), say_cb, None, unix_time, max_passes)
        stats = FixStats()
        rc = L.fatfix_run(self._ck, C.byref(opts), C.byref(stats))
        return rc, stats, lines, self.scan()                    # (one more scan, so that the findings are those of the repaired volume only)

    def close(self):
        if self._ck.value:
            lib().fatck_close(self._ck)
            self._ck = C.c_void_p()
        self._mem.clear()
        self.f.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def scan_volume(path, index=None, **kw):
    """Scans partition `index` (1-based; default: first FAT volume) of an image; returns a ScanOutcome."""
    import mkdisk
    ss, parts = mkdisk.partitions(path)
    pick = [p for p in parts if index is None or p["index"] == index]
    if not pick:
        raise HostBuildError(f"no partition {index} in {path}")
    off, length = pick[0]["offset"], pick[0]["length"]
    with Session(path, off, length, align=ss, **kw) as s:
        out = s.scan() if s.opened else ScanOutcome(s.rc_open, Result(), list(s.findings), None)
    return out


def fix_volume(path, index=None, mode=FIX_SAFE, free_lost=False, **kw):
    """Repairs partition `index` of an image in place (the file is modified!). Returns (rc, FixStats, lines, ScanOutcome of the last scan)."""
    import mkdisk
    ss, parts = mkdisk.partitions(path)
    pick = [p for p in parts if index is None or p["index"] == index]
    if not pick:
        raise HostBuildError(f"no partition {index} in {path}")
    with Session(path, pick[0]["offset"], pick[0]["length"], align=ss, writable=mode != FIX_NONE, keep=4096, **kw) as s:
        return s.fix(mode, free_lost)


def main(argv=None):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    args = list(sys.argv[1:] if argv is None else argv)
    if not args:
        print(__doc__)
        return 2
    mode = None
    while args and args[0] in ("-a", "-y", "-n"):
        mode = {"-a": FIX_SAFE, "-y": FIX_ALL, "-n": FIX_NONE}[args.pop(0)]
    spec = args[-1]
    m = re.match(r"^(.*):(\d+)$", spec)
    path, idx = (m.group(1), int(m.group(2))) if m and len(m.group(1)) > 1 else (spec, None)
    if mode:
        rc, stats, lines, out = fix_volume(path, idx, mode)
        print("\n".join(lines))
        print(f"passes {stats.passes}, repaired {stats.repaired}, declined {stats.declined}, failed {stats.failed}, clean now: {stats.clean_now}")
        print(out.text())
        return 0 if stats.clean_now else 1
    out = scan_volume(path, idx)
    print(out.text())
    r = out.result
    print(f"{r.errors} error(s), {r.leaks} leftover(s), {r.warnings} warning(s); {r.files} files, {r.dirs} directories, "
          f"{r.free_clusters} free / {r.lost_clusters} lost / {r.bad_clusters} bad clusters")
    return 0 if out.ok else 1


if __name__ == "__main__":
    sys.exit(main())
