#!/usr/bin/env python3
"""Runs the kernel's system call fuzzer (user/test/sysfuzz.c, docs/HARDENING.md) once per seed and prints one line per run.

  python tools/fuzz.py 1 2 3                      three seeds, 25 s each, as an ordinary user
  python tools/fuzz.py --secs 60 --workers 3 7    one longer run with three workers
  python tools/fuzz.py --extra heapdebug 11 12    with heap poisoning (finds writes through stale pointers)
  python tools/fuzz.py --root 5                   the workers keep uid 0 (what they may change stays in the scratch directory)
  python tools/fuzz.py --only 17 5                seed 5, worker number 17 alone, every call printed before it is made

How it decides: the guest boots with `kerneltest=sysfuzz ktargs=...`; init runs the fuzzer as root and powers off with its verdict as
the exit status. A run passes when that status is 0 and the log has no panic and no [WARNING] line. A failing run prints the end of the
log and the last "worker N seed S starts" lines (as many as there were workers at a time); after a panic or a hang one of them is the
worker to replay with --only N.

Not part of the default test suite: `TOUHOU_HARDENING_FUZZ=1 python tests/run_all.py hardening_sysfuzz` runs three seeds as a test."""
import argparse
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qemu import Qemu  # noqa: E402

# Kernel lines that say nothing about a failure (every worker that dies of a signal is reported there).
NOISE = re.compile(r"exorcised|BOUNDARY VIOLATION|^\[sysfuzz\]|^\[fuzz\]|^\[\] |incident:|^\[Eiki\] process group")


def run_one(config, extra, mem, secs):
    """Boots and waits for the end. Returns (status, guest exit status, serial text); status is PASS, FAIL, PANIC or TIMEOUT."""
    cmdline = f"text kerneltest=sysfuzz ktargs={config}" + (" " + extra if extra else "")
    verdict = None
    with Qemu(cmdline=cmdline, mem=mem, name="fuzz") as vm:
        end = time.time() + secs + 150
        while time.time() < end:
            if "INCIDENT: KERNEL PANIC" in vm.text():
                time.sleep(1.5)                      # let the rest of the panic screen arrive
                verdict = "PANIC"
                break
            if not vm.alive():
                verdict = "done"
                break
            time.sleep(0.5)
        rc = vm.wait_exit(10) if verdict == "done" else None
        text = vm.text()
    if verdict == "PANIC":
        return "PANIC", rc, text
    if verdict is None:
        return "TIMEOUT", rc, text
    return ("PASS" if rc == 0 and "[WARNING]" not in text else "FAIL"), rc, text


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("seeds", nargs="+", type=int, help="one run per seed")
    ap.add_argument("--secs", type=int, default=25, help="how long the workers run (default 25)")
    ap.add_argument("--workers", type=int, default=2, help="workers at a time, 1 to 4 (default 2)")
    ap.add_argument("--root", action="store_true", help="the workers keep uid 0")
    ap.add_argument("--extra", default="", help="more kernel command line words, e.g. heapdebug")
    ap.add_argument("--mem", default="512M", help="guest memory (default 512M)")
    ap.add_argument("--only", type=int, help="replay only the worker with this number, tracing every call")
    ap.add_argument("--tail", type=int, default=30, help="lines of a failing log to show (default 30)")
    a = ap.parse_args()
    failures = 0
    for seed in a.seeds:
        cfg = f"seed:{seed},time:{a.secs},workers:{a.workers}" + (",root:1" if a.root else "")
        if a.only is not None:
            cfg += f",only:{a.only},trace:1"
        status, rc, text = run_one(cfg, a.extra, a.mem, a.secs)
        lines = text.splitlines()
        shown = [l for l in lines if not NOISE.search(l)]
        summary = ""
        for l in shown:
            if l.startswith("sysfuzz: ") and "workers," in l:
                summary = l[9:]
        print(f"seed {seed:6d}: {status:7s} (guest status {rc}) {summary}", flush=True)
        if status != "PASS":
            failures += 1
            started = [l.strip() for l in lines if l.startswith("sysfuzz: worker") and l.rstrip().endswith("starts")]
            for l in started[-a.workers:]:
                print(f"  last workers started: {l}")
            for l in shown[-a.tail:]:
                sys.stdout.buffer.write((l + "\n").encode("utf-8", "replace"))
            sys.stdout.flush()
    print(f"{failures} of {len(a.seeds)} runs failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
