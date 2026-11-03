#!/usr/bin/env python3
"""Runs the same tests with the clang-built userland and with the userland built by mcc, and prints where they differ.

  python tools/mcc_compare.py notebook,sketchbook                 both runs (each through tools/qemu-slot.py), then the table
  python tools/mcc_compare.py notebook --reuse                    use build/mcc-compare/{clang,mcc}.json where they exist
  python tools/mcc_compare.py shell,utest --dir build/mcc-user    another output directory of tools/mcc_userland.py
  python tools/mcc_compare.py --table build/mcc-compare/clang.json build/mcc-compare/mcc.json     only print the table of two result files

The runs are sequential (they share files under build/).  A test that fails with the mcc build and passes with the clang build is
a regression of the compiler until proved otherwise: rerun it alone with  tools/mcc_userland_suites.py NAME  first, because the
mouse-driven GUI tests flake on a busy PC (the same test can fail in the baseline too, which the table shows).
Exit status: 0 when no test fails with mcc that passes with clang, else 1.
"""
import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "build" / "mcc-compare"


def run(label, filt, mcc_dir, json_path, slot, retry):
    cmd = [sys.executable, str(ROOT / "tools" / "mcc_userland_suites.py"), filt, "--dir", mcc_dir, "--json", str(json_path)]
    if label == "clang":
        cmd.append("--none")
    if retry:
        cmd.append("--retry-failed")
    if slot:
        cmd = [sys.executable, str(ROOT / "tools" / "qemu-slot.py"), "--", *cmd]
    print(f"== {label}: {' '.join(cmd[-6:])}", flush=True)
    subprocess.call(cmd, cwd=ROOT)


def load(path):
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    return {r["name"]: r for r in data["results"]}


def table(clang, mcc):
    names = sorted(set(clang) | set(mcc))
    bad, same_fail, better = [], [], []
    print(f"\n{'test':36} {'clang':>12} {'mcc':>12}   note")
    for n in names:
        a, b = clang.get(n), mcc.get(n)
        sa = a["status"] if a else "-"
        sb = b["status"] if b else "-"
        ta = f"{a['seconds']:.0f}s" if a else ""
        tb = f"{b['seconds']:.0f}s" if b else ""
        note = ""
        ok = ("PASS", "FLAKY", "XFAIL", "XPASS")
        if sa in ok and sb == "FAIL":
            note = "REGRESSION"
            bad.append(n)
        elif sa == "FAIL" and sb == "FAIL":
            note = "fails in both"
            same_fail.append(n)
        elif sa == "FAIL" and sb in ok:
            note = "passes with mcc only"
            better.append(n)
        elif a and b and a["seconds"] > 0 and b["seconds"] > 2.5 * a["seconds"] and b["seconds"] > 10:
            note = f"{b['seconds'] / a['seconds']:.1f}x slower"
        print(f"{n:36} {sa + ' ' + ta:>12} {sb + ' ' + tb:>12}   {note}")
    total = len(names)
    print(f"\n{total} tests: {len(bad)} regressions with mcc, {len(same_fail)} failing in both, {len(better)} passing only with mcc")
    for n in bad:
        print(f"--- {n}: {(mcc[n].get('error') or '')[:600]}")
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("filter", nargs="?", default="", help="test filter of tests/run_all.py (comma separated)")
    ap.add_argument("--dir", default="build/mcc-user", help="output directory of tools/mcc_userland.py")
    ap.add_argument("--reuse", action="store_true", help="do not rerun a side whose json file exists")
    ap.add_argument("--no-slot", action="store_true", help="do not go through tools/qemu-slot.py (only when you hold a slot yourself)")
    ap.add_argument("--retry-failed", action="store_true", help="rerun each failed test alone once in each run")
    ap.add_argument("--table", nargs=2, metavar=("CLANG_JSON", "MCC_JSON"), help="print the table of two existing result files and exit")
    args = ap.parse_args()
    if args.table:
        return 1 if table(load(args.table[0]), load(args.table[1])) else 0
    OUT.mkdir(parents=True, exist_ok=True)
    paths = {"clang": OUT / "clang.json", "mcc": OUT / "mcc.json"}
    for label, p in paths.items():
        if args.reuse and p.exists():
            continue
        if p.exists():
            p.unlink()
        run(label, args.filter, args.dir, p, not args.no_slot, args.retry_failed)
    missing = [str(p) for p in paths.values() if not p.exists()]
    if missing:
        print("no results in:", ", ".join(missing))
        return 2
    return 1 if table(load(paths["clang"]), load(paths["mcc"])) else 0


if __name__ == "__main__":
    sys.exit(main())
