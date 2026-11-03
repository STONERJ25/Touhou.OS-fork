#!/usr/bin/env python3
"""Optional cross-check of the x86emu CPU core (user/lib/pc98) against the SingleStepTests 8088 and 80386 test vectors.

    python3 tests/host/x86sst.py --fetch            # download what is missing (network), then run 200 tests per file
    python3 tests/host/x86sst.py --limit 0          # every test of every cached file (about 3 million)
    python3 tests/host/x86sst.py --ops 00,D0.6,F7.7 # some files only
    python3 tests/host/x86sst.py --cpu 386 --fetch --limit 0       # the 80386 real-mode suite, all of it
    python3 tests/host/x86sst.py --cpu 386 --ops 6601,0FA4 --strict  # also compare the flags the suite calls undefined

On the owner's Windows PC the tool must be built and run inside WSL; tools/hosttest.py does that for unittest modules, so
this file also has a test case that runs main() with the arguments in X86SST_ARGS (WSLENV forwards the variable):

    X86SST_ARGS="--cpu 386 --limit 0" WSLENV=X86SST_ARGS python tools/hosttest.py tests.host.x86sst

The 8088 vectors (https://github.com/SingleStepTests/8088, version 2, MIT licence, recorded from a real AMD D8088) and the
80386 vectors (https://github.com/SingleStepTests/80386, v1 real mode, MIT licence for the software and public domain for
the data, recorded from an Intel 80386EX) are NOT part of this repository: they are downloaded at test time into
build/sst-8088/ and build/sst-80386/ (git-ignored) and only with --fetch. Without --fetch and without a cache the script
says so and exits 0 (skipped). Undefined flags are masked with the suites' own metadata (metadata.json for the 8088, the
register masks inside each 80386 file); 8088 opcodes marked "undefined" are skipped unless --undefined is given, and
80386 tests on the suite's revocation list are skipped.

Each vector is one instruction: registers and the bytes of RAM it touches before and after. The runner feeds them to
build/host-x86/x86tool (tests/host/x86tool.c, built with AddressSanitizer by tests/host/x86ref.py) as an 8086 or a 386
and compares registers, the flags under the mask, and every byte the emulator wrote or the vector says changed. The 386
files are in the suite's binary MOO format (chunks: https://github.com/dbalsom/moo, doc/moo_format_v1.md), read here by
moo_tests(), written from that specification.
"""
import argparse
import gzip
import http.client
import json
import os
import shlex
import struct
import subprocess
import sys
import time
import unittest
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
CACHE = REPO / "build" / "sst-8088"
BASE = "https://raw.githubusercontent.com/SingleStepTests/8088/main/v2/"
REGS = ["ax", "cx", "dx", "bx", "sp", "bp", "si", "di", "es", "cs", "ss", "ds", "ip", "flags"]

CACHE386 = REPO / "build" / "sst-80386"
BASE386 = "https://raw.githubusercontent.com/SingleStepTests/80386/main/"
TREE386 = "https://api.github.com/repos/SingleStepTests/80386/git/trees/main?recursive=1"
DIR386 = "v1_ex_real_mode"
# RG32 / RM32 register order (MOO format 1.1), and the order of x86tool's "x" command and "X" output line.
MOO_R32 = ["cr0", "cr3", "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp", "cs", "ds", "es", "fs", "gs", "ss", "eip",
           "eflags", "dr6", "dr7"]
X_REGS = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "fs", "gs", "eflags", "cr0"]


def build_tool():
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import x86ref
    return x86ref.tool()


def fetch_url(url, dst):
    """Downloads url to dst unless dst exists; None when the server has no such file."""
    if dst.exists():
        return dst
    dst.parent.mkdir(parents=True, exist_ok=True)
    for delay in (2, 4, 8, 16, None):
        try:
            with urllib.request.urlopen(url, timeout=120) as r:
                data = r.read()
            tmp = dst.with_suffix(".part")
            tmp.write_bytes(data)
            tmp.rename(dst)
            return dst
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return None
            if delay is None:
                raise
        except (OSError, http.client.HTTPException):    # resets, cut-off transfers: retry with backoff
            if delay is None:
                raise
        time.sleep(delay)
    return None


def fetch(name):
    return fetch_url(BASE + name, CACHE / name)


def file_list(meta, undefined):
    """[(file name, flags mask)] for every opcode file the metadata describes."""
    out = []
    for op, ent in meta["opcodes"].items():
        if "reg" in ent:
            for r, sub in ent["reg"].items():
                if sub.get("status") == "undefined" and not undefined:
                    continue
                out.append((f"{op}.{r}", sub.get("flags-mask", 0xFFFF)))
        else:
            if ent.get("status") in ("prefix",) or (ent.get("status") == "undefined" and not undefined):
                continue
            out.append((op, ent.get("flags-mask", 0xFFFF)))
    return out


def run_file(tool, path, mask, limit):
    tests = json.load(gzip.open(path))
    if limit:
        tests = tests[:limit]
    script = ["model 8086"]
    for t in tests:
        ini = t["initial"]
        script.append("r " + " ".join("%x" % ini["regs"][k] for k in REGS))
        for a, v in ini["ram"]:
            script.append("m %x %x" % (a, v))
        script.append("step 1")
        script.append("clear")
    r = subprocess.run([str(tool)], input="\n".join(script) + "\n", capture_output=True, text=True,
                       env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
    if r.returncode != 0:
        return len(tests), [f"tool exited with {r.returncode}: {r.stderr[-800:]}"]
    results, cur = [], None
    for line in r.stdout.splitlines():
        if line.startswith("R "):
            cur = {"regs": [int(x, 16) for x in line.split()[1:]], "w": {}}
            results.append(cur)
        elif line.startswith("W "):
            _, a, v = line.split()
            cur["w"][int(a, 16)] = int(v, 16)
    if len(results) != len(tests):
        return len(tests), [f"got {len(results)} results for {len(tests)} tests; stderr: {r.stderr[-800:]}"]
    fails = []
    for t, res in zip(tests, results):
        ini, fin = t["initial"], t["final"]
        exp = dict(ini["regs"])
        exp.update(fin["regs"])
        got = dict(zip(REGS, res["regs"][:14]))
        bad = []
        for k in REGS:
            e, g = exp[k], got[k]
            if k == "flags":
                e, g = e & mask, g & mask
            if e != g:
                bad.append(f"{k}={g:04x} want {e:04x}")
        # A divide error pushes FLAGS; the bits the suite calls undefined are as undefined on the stack as in the register.
        fmask = {}
        if exp["cs"] == 0 and exp["ip"] == 0x400:
            lo, hi = ((exp["ss"] << 4) + ((exp["sp"] + 4) & 0xFFFF)) & 0xFFFFF, ((exp["ss"] << 4) + ((exp["sp"] + 5) & 0xFFFF)) & 0xFFFFF
            fmask = {lo: mask & 0xFF, hi: mask >> 8}
        mem_before = {a: v for a, v in ini["ram"]}
        mem_after = dict(mem_before)
        mem_after.update({a: v for a, v in fin["ram"]})
        for a, v in res["w"].items():
            want = mem_after.get(a, 0)
            m = fmask.get(a, 0xFF)
            if v & m != want & m:
                bad.append(f"[{a:05x}]={v:02x} want {want:02x}")
        for a, v in fin["ram"]:
            if a not in res["w"] and mem_before.get(a, 0) != v:
                bad.append(f"[{a:05x}] not written, want {v:02x}")
        if bad:
            fails.append(f"{t['name']} ({bytes(t['bytes']).hex()}): " + ", ".join(bad))
    return len(tests), fails


# ------------------------------------------------------------------ 80386 (MOO files)

def _chunks(buf, off, end):
    """(chunk id, payload offset, payload length) for the chunks between off and end; the length field decides where
    the next one starts, so unknown chunks and longer future payloads are skipped."""
    while off + 8 <= end:
        cid = buf[off:off + 4].decode("ascii", "replace")
        ln = struct.unpack_from("<I", buf, off + 4)[0]
        yield cid, off + 8, ln
        off += 8 + ln


def _regs32(buf, off):
    """An RG32 or RM32 payload: a bitmask, then one uint32 per set bit, in MOO_R32 order."""
    mask = struct.unpack_from("<I", buf, off)[0]
    out, p = {}, off + 4
    for i, name in enumerate(MOO_R32):
        if mask >> i & 1:
            out[name] = struct.unpack_from("<I", buf, p)[0]
            p += 4
    return out


def _state(buf, off, ln):
    st = {"regs": {}, "ram": [], "mask": {}}
    for cid, o, _ in _chunks(buf, off, off + ln):
        if cid == "RG32":
            st["regs"] = _regs32(buf, o)
        elif cid == "RM32":
            st["mask"] = _regs32(buf, o)
        elif cid == "RAM ":
            n = struct.unpack_from("<I", buf, o)[0]
            st["ram"] = [struct.unpack_from("<IB", buf, o + 4 + 5 * i) for i in range(n)]
    return st


def moo_tests(path):
    """Reads a gzipped MOO file: (the file's register masks, [tests]); a test is a dict with name, bytes, initial and
    final ({regs, ram, mask}), exception ((number, flags address) when one was taken) and hash."""
    buf = gzip.open(path).read()
    masks, tests = {}, []
    for cid, o, ln in _chunks(buf, 0, len(buf)):
        if cid == "RM32":
            masks = _regs32(buf, o)
        elif cid == "TEST":
            t = {"idx": struct.unpack_from("<I", buf, o)[0]}
            for sid, so, sl in _chunks(buf, o + 4, o + ln):
                if sid == "NAME":
                    n = struct.unpack_from("<I", buf, so)[0]
                    t["name"] = buf[so + 4:so + 4 + n].decode("ascii", "replace")
                elif sid == "BYTS":
                    n = struct.unpack_from("<I", buf, so)[0]
                    t["bytes"] = bytes(buf[so + 4:so + 4 + n])
                elif sid == "INIT":
                    t["initial"] = _state(buf, so, sl)
                elif sid == "FINA":
                    t["final"] = _state(buf, so, sl)
                elif sid == "EXCP":
                    t["exception"] = struct.unpack_from("<BI", buf, so)
                elif sid == "HASH":
                    t["hash"] = buf[so:so + 20].hex()
            tests.append(t)
    return masks, tests


def list386(fetch_ok):
    """The stems of the real-mode files (00, 6601, 0FBA.4, ...), from the repository listing cached in files.json."""
    idx = CACHE386 / "files.json"
    if not idx.exists():
        if not fetch_ok:
            return None
        with urllib.request.urlopen(TREE386, timeout=120) as r:
            tree = json.load(r)
        names = sorted(e["path"].split("/")[-1][:-len(".MOO.gz")] for e in tree["tree"]
                       if e["type"] == "blob" and e["path"].startswith(DIR386 + "/") and e["path"].endswith(".MOO.gz"))
        CACHE386.mkdir(parents=True, exist_ok=True)
        idx.write_text(json.dumps(names))
    if fetch_ok:
        fetch_url(BASE386 + "revocation_list.txt", CACHE386 / "revocation_list.txt")
        fetch_url(BASE386 + "80386.csv", CACHE386 / "80386.csv")
    return json.loads(idx.read_text())


def revoked386():
    p = CACHE386 / "revocation_list.txt"
    if not p.exists():
        return set()
    return {ln.strip().lower() for ln in p.read_text().splitlines() if ln.strip() and not ln.startswith("#")}


def csv_masks386():
    """{(opcode, extension or None): mask of the defined flags} from the suite's 80386.csv (f_umask), for the files that
    carry no mask of their own."""
    import csv
    p = CACHE386 / "80386.csv"
    out = {}
    if p.exists():
        with p.open() as f:
            for row in csv.DictReader(f):
                if row.get("f_umask"):
                    out[(row["op"].upper(), row["ex"] or None)] = int(row["f_umask"], 16)
    return out


def csv_key386(stem):
    """File stem (6766C1.4, 0FBA.5, 01) -> (opcode, extension) as 80386.csv spells them: the size prefixes removed."""
    s = stem.upper()
    while len(s) > 2 and s[:2] in ("66", "67"):
        s = s[2:]
    op, _, ex = s.partition(".")
    return op, ex or None


def run_file386(tool, path, limit, strict, revoked, csv_masks):
    """Runs one 80386 file. Every test is the instruction under test followed by a HLT (the suite puts one at the target
    of a jump or an exception, and lists it in the initial RAM); the final state is taken after that HLT, so the test
    runs until HLT, as the suite says to (a jump into its own bytes can make that two instructions). The defined flags:
    the file's own mask, else the CSV's."""
    masks, tests = moo_tests(path)
    tests = [t for t in tests if t.get("hash") not in revoked]
    if limit:
        tests = tests[:limit]
    stem = path.name.split(".MOO")[0]
    fmask = masks.get("eflags", csv_masks.get(csv_key386(stem), 0xFFFF)) & 0xFFFF
    if strict:
        fmask = 0xFFFF
    script = ["model 386", "a20 1"]
    for t in tests:
        ini = t["initial"]["regs"]
        lo = [ini[k] & 0xFFFF for k in ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "es", "cs", "ss", "ds", "eip")]
        script.append("r " + " ".join("%x" % v for v in lo) + " %x" % ((ini["eflags"] & 0x7FD7) | 2))
        script.append("x " + " ".join("%x" % ini[k] for k in ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi")) +
                      " %x %x %x" % (ini["fs"] & 0xFFFF, ini["gs"] & 0xFFFF, (ini["eflags"] & 0x7FD7) | 2))
        script.append("cr 0 %x" % (ini["cr0"] & 0x8000001F))
        for a, v in t["initial"]["ram"]:
            script.append("m %x %x" % (a, v))
        script.append("run 8")
        script.append("clear")
    r = subprocess.run([str(tool)], input="\n".join(script) + "\n", capture_output=True, text=True,
                       env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
    if r.returncode != 0:
        return len(tests), [f"tool exited with {r.returncode}: {r.stderr[-800:]}"], 0
    results, cur = [], None
    for line in r.stdout.splitlines():
        if line.startswith("R "):
            v = [int(x, 16) for x in line.split()[1:]]
            cur = {"r": dict(zip(REGS, v[:14])), "stop": v[16], "w": {}}
            results.append(cur)
        elif line.startswith("X "):
            cur["x"] = dict(zip(X_REGS, (int(x, 16) for x in line.split()[1:])))
        elif line.startswith("W "):
            _, a, v = line.split()
            cur["w"][int(a, 16)] = int(v, 16)
    if len(results) != len(tests):
        return len(tests), [f"got {len(results)} results for {len(tests)} tests; stderr: {r.stderr[-800:]}"], 0
    fails, flag_only = [], 0
    for t, res in zip(tests, results):
        ini, fin = t["initial"], t["final"]
        exp = dict(ini["regs"])
        exp.update(fin["regs"])
        tmask = fin["mask"]
        bad, flag_bad = [], False
        if res["stop"]:
            bad.append(f"CPU stopped ({res['stop']})")
        if res["r"]["ip"] != exp["eip"] & 0xFFFF:
            bad.append(f"ip={res['r']['ip']:04x} want {exp['eip']:04x}")
        for k in ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"):
            m = tmask.get(k, 0xFFFFFFFF)
            if res["x"][k] & m != exp[k] & m:
                bad.append(f"{k}={res['x'][k]:08x} want {exp[k]:08x}")
        for k in ("es", "cs", "ss", "ds"):
            if res["r"][k] != exp[k] & 0xFFFF:
                bad.append(f"{k}={res['r'][k]:04x} want {exp[k] & 0xFFFF:04x}")
        for k in ("fs", "gs"):
            if res["x"][k] != exp[k] & 0xFFFF:
                bad.append(f"{k}={res['x'][k]:04x} want {exp[k] & 0xFFFF:04x}")
        m = fmask & tmask.get("eflags", 0xFFFF)
        got_f, want_f = res["x"]["eflags"], exp["eflags"] & 0xFFFF
        if got_f >> 16:
            bad.append(f"eflags={got_f:08x}: upper half set")
        if got_f & m != want_f & m:
            bad.append(f"flags={got_f & 0xFFFF:04x} want {want_f:04x} (mask {m:04x})")
            flag_bad = True
        # The FLAGS image an exception pushed holds the same undefined bits as the register.
        fm = {}
        if "exception" in t:
            fa = t["exception"][1]
            fm = {fa: m & 0xFF, fa + 1: m >> 8}
        before = {a: v for a, v in ini["ram"]}
        after = dict(before)
        after.update({a: v for a, v in fin["ram"]})
        for a, v in res["w"].items():
            if a not in after:
                bad.append(f"[{a:06x}]={v:02x} written, not in the test")
                continue
            mm = fm.get(a, 0xFF)
            if v & mm != after[a] & mm:
                bad.append(f"[{a:06x}]={v:02x} want {after[a]:02x}")
                flag_bad = flag_bad or a in fm
        for a, v in fin["ram"]:
            if a not in res["w"] and before.get(a) != v:
                bad.append(f"[{a:06x}] not written, want {v:02x}")
        if bad:
            only_flags = flag_bad and all(b.startswith("flags=") or ("want" in b and b.startswith("[") and int(b[1:7], 16) in fm)
                                          for b in bad)
            flag_only += only_flags
            exc = f" exc {t['exception'][0]}" if "exception" in t else ""
            fails.append(f"#{t['idx']} {t.get('name', '?')} ({t['bytes'].hex()}){exc}: " + ", ".join(bad))
    return len(tests), fails, flag_only


def main386(a):
    names = list386(a.fetch)
    if names is None:
        print("x86sst: no cached SingleStepTests 80386 vectors (build/sst-80386); run with --cpu 386 --fetch to download them. SKIPPED")
        return 0
    if a.ops:
        want = {o.upper() for o in a.ops.split(",")}
        names = [n for n in names if n.upper() in want]
    tool = build_tool()
    revoked = revoked386()
    csv_masks = csv_masks386()
    total = failed = flag_only = 0
    skipped = []
    for name in names:
        dst = CACHE386 / DIR386 / (name + ".MOO.gz")
        path = fetch_url(BASE386 + DIR386 + "/" + name + ".MOO.gz", dst) if a.fetch else (dst if dst.exists() else None)
        if path is None:
            skipped.append(name)
            continue
        n, fails, fo = run_file386(tool, path, a.limit, a.strict, revoked, csv_masks)
        total += n
        failed += len(fails)
        flag_only += fo
        status = "ok" if not fails else f"{len(fails)} FAILED" + (f" ({fo} only in flags)" if fo else "")
        print(f"{name:12s} {n:6d} tests  {status}", flush=True)
        for f in fails if a.v else fails[:5]:
            print("    " + f)
    print(f"x86sst 386: {total} tests, {failed} failed ({flag_only} of them only in the flags), "
          f"{len(skipped)} files not available: {' '.join(skipped)}")
    return 1 if failed else 0


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--cpu", choices=("8088", "386"), default="8088", help="which suite (default 8088)")
    ap.add_argument("--fetch", action="store_true", help="download missing files (network)")
    ap.add_argument("--limit", type=int, default=200, help="tests per file, 0 = all")
    ap.add_argument("--ops", help="comma-separated file stems (e.g. 00,D0.6; for the 386 e.g. 6601,0FBA.4)")
    ap.add_argument("--undefined", action="store_true", help="8088: also run opcodes the suite marks undefined")
    ap.add_argument("--strict", action="store_true", help="386: compare the flags the suite calls undefined as well")
    ap.add_argument("-v", action="store_true", help="print every failure, not the first five per file")
    a = ap.parse_args(argv)
    if a.cpu == "386":
        return main386(a)
    meta_path = CACHE / "metadata.json"
    if not meta_path.exists():
        if not a.fetch:
            print("x86sst: no cached SingleStepTests vectors (build/sst-8088); run with --fetch to download them. SKIPPED")
            return 0
        if not fetch("metadata.json"):
            print("x86sst: metadata.json not found upstream")
            return 1
    meta = json.loads(meta_path.read_text())
    files = file_list(meta, a.undefined)
    if a.ops:
        want = {o.upper() for o in a.ops.split(",")}
        files = [(n, m) for n, m in files if n.upper() in want]
    tool = build_tool()
    total = failed = 0
    skipped = []
    done_bases = set()
    for name, mask in files:
        fn = name + ".json.gz"
        path = fetch(fn) if a.fetch else (CACHE / fn if (CACHE / fn).exists() else None)
        if path is None and "." in name:
            # Opcodes whose reg field is ignored (8F, C6, C7) come as one file for all reg values.
            base = name.split(".")[0]
            if base in done_bases:
                continue
            fn = base + ".json.gz"
            path = fetch(fn) if a.fetch else (CACHE / fn if (CACHE / fn).exists() else None)
            if path is not None:
                done_bases.add(base)
                name = base
        if path is None:
            skipped.append(name)
            continue
        n, fails = run_file(tool, path, mask, a.limit)
        total += n
        failed += len(fails)
        status = "ok" if not fails else f"{len(fails)} FAILED"
        print(f"{name:6s} {n:6d} tests  {status}", flush=True)
        for f in fails if a.v else fails[:5]:
            print("    " + f)
    print(f"x86sst: {total} tests, {failed} failed, {len(skipped)} files not available: {' '.join(skipped)}")
    return 1 if failed else 0


class SingleStepTests(unittest.TestCase):
    """The cross-check as a test, for tools/hosttest.py (python tools/hosttest.py tests.host.x86sst): it runs main() with
    the arguments in X86SST_ARGS, and is skipped when that variable is not set (the discovery pattern test_*.py does not
    pick this file up)."""

    @unittest.skipUnless(os.environ.get("X86SST_ARGS"), "set X86SST_ARGS to run the SingleStepTests cross-check")
    def test_vectors(self):
        self.assertEqual(main(shlex.split(os.environ["X86SST_ARGS"])), 0)


if __name__ == "__main__":
    sys.exit(main())
