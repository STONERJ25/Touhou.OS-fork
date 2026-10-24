#!/usr/bin/env python3
"""Optional cross-check of the x86emu CPU core (user/lib/pc98) against the SingleStepTests 8088 test vectors.

    python3 tests/host/x86sst.py --fetch            # download what is missing (network), then run 200 tests per file
    python3 tests/host/x86sst.py --limit 0          # every test of every cached file (about 3 million)
    python3 tests/host/x86sst.py --ops 00,D0.6,F7.7 # some files only

The vectors (https://github.com/SingleStepTests/8088, version 2, MIT licence, recorded from a real AMD D8088) are NOT part
of this repository: they are downloaded at test time into build/sst-8088/ (git-ignored) and only with --fetch. Without
--fetch and without a cache the script says so and exits 0 (skipped). Undefined flags are masked with the suite's own
metadata.json; opcodes marked "undefined" there are skipped unless --undefined is given.

Each vector is one instruction: registers and the bytes of RAM it touches before and after. The runner feeds them to
build/host-x86/x86tool (tests/host/x86tool.c, built here with AddressSanitizer) as an 8086 and compares registers, the
flags under the mask, and every byte the emulator wrote or the vector says changed.
"""
import argparse
import gzip
import http.client
import json
import os
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
OUT = REPO / "build" / "host-x86"
CACHE = REPO / "build" / "sst-8088"
BASE = "https://raw.githubusercontent.com/SingleStepTests/8088/main/v2/"
REGS = ["ax", "cx", "dx", "bx", "sp", "bp", "si", "di", "es", "cs", "ss", "ds", "ip", "flags"]


def build_tool():
    OUT.mkdir(parents=True, exist_ok=True)
    tool = OUT / "x86tool"
    srcs = [REPO / "tests/host/x86tool.c", REPO / "user/lib/pc98/src/x86cpu.c"]
    if not tool.exists() or any(s.stat().st_mtime > tool.stat().st_mtime for s in srcs + [REPO / "user/lib/pc98/include/x86emu.h"]):
        cmd = ["gcc", "-O2", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-Wall", "-Wextra",
               "-Wno-unused-parameter", "-I", str(REPO / "user/lib/pc98/include"), *map(str, srcs), "-o", str(tool)]
        subprocess.run(cmd, check=True)
    return tool


def fetch(name):
    dst = CACHE / name
    if dst.exists():
        return dst
    CACHE.mkdir(parents=True, exist_ok=True)
    for delay in (2, 4, 8, 16, None):
        try:
            with urllib.request.urlopen(BASE + name, timeout=120) as r:
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
    r = subprocess.run([str(tool)], input="\n".join(script) + "\n", capture_output=True, text=True)
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
        mem_before = {a: v for a, v in ini["ram"]}
        mem_after = dict(mem_before)
        mem_after.update({a: v for a, v in fin["ram"]})
        for a, v in res["w"].items():
            want = mem_after.get(a, 0)
            if v != want:
                bad.append(f"[{a:05x}]={v:02x} want {want:02x}")
        for a, v in fin["ram"]:
            if a not in res["w"] and mem_before.get(a, 0) != v:
                bad.append(f"[{a:05x}] not written, want {v:02x}")
        if bad:
            fails.append(f"{t['name']} ({bytes(t['bytes']).hex()}): " + ", ".join(bad))
    return len(tests), fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fetch", action="store_true", help="download missing files (network)")
    ap.add_argument("--limit", type=int, default=200, help="tests per file, 0 = all")
    ap.add_argument("--ops", help="comma-separated file stems (e.g. 00,D0.6)")
    ap.add_argument("--undefined", action="store_true", help="also run opcodes the suite marks undefined")
    ap.add_argument("-v", action="store_true", help="print every failure, not the first five per file")
    a = ap.parse_args()
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
    for name, mask in files:
        fn = name + ".json.gz"
        path = fetch(fn) if a.fetch else (CACHE / fn if (CACHE / fn).exists() else None)
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


if __name__ == "__main__":
    sys.exit(main())
