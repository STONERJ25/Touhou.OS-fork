#!/usr/bin/env python3
"""Runs suites of tests/run_all.py with the userland built by mcc (build/mcc-user, made by tools/mcc_userland.py).

  python tools/mcc_userland.py                                # after every change of the compiler (parallel, about 10 s)
  python tools/mcc_userland_suites.py notebook,sketchbook     # the filter is the one of tests/run_all.py
  python tools/mcc_userland_suites.py vi --only bin/vi        # replace only some programs with their mcc builds
  python tools/mcc_userland_suites.py notebook --none         # replace nothing: the baseline with the clang build
  python tools/mcc_userland_suites.py shell --json r.json     # also write the results (see tests/run_all.py --json)

Every guest the tests start boots a private copy of the ESP (build/mcc-userland-esp) whose initrd holds the mcc-built programs.
tools/mcc_compare.py runs the baseline and the mcc build one after the other and prints where they differ.
Do not run it at the same time as another test run in this checkout (both use QEMU and build/ files); QEMU goes through
tools/qemu-slot.py:   python tools/qemu-slot.py -- python tools/mcc_userland_suites.py notebook"""
import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))
import mcc_util as U  # noqa: E402
import qemu  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("filter", nargs="?", default="", help="test name filter of tests/run_all.py (comma separated)")
    ap.add_argument("--only", help="comma separated programs to replace, such as bin/notebook,sbin/init")
    ap.add_argument("--none", action="store_true", help="replace nothing")
    ap.add_argument("--dir", default="build/mcc-user", help="where tools/mcc_userland.py put the programs (default build/mcc-user)")
    ap.add_argument("--json", help="write the results of the run to this file")
    ap.add_argument("--retry-failed", action="store_true", help="rerun each failed test alone once (run_all.py --retry-failed)")
    args = ap.parse_args()

    repl = {}
    for kind in ("bin", "sbin"):
        for p in sorted((ROOT / args.dir / kind).glob("*")):
            if p.is_file() and not p.name.endswith(".err"):
                repl[f"{kind}/{p.name}"] = p
    if not repl:
        sys.exit(f"{args.dir} is empty: run  python tools/mcc_userland.py  first")
    if args.only:
        keep = set(args.only.split(","))
        repl = {k: v for k, v in repl.items() if k in keep}
    if args.none:
        repl = {}
    print(len(repl), "programs replaced by their mcc builds", flush=True)

    esp = U.make_esp("mcc-userland", repl)
    orig = qemu.Qemu.__init__

    def patched(self, *a, **kw):
        kw["esp"] = esp
        orig(self, *a, **kw)

    qemu.Qemu.__init__ = patched
    argv = [args.filter]
    if args.json:
        argv = ["--json", args.json, *argv]
    if args.retry_failed:
        argv = ["--retry-failed", *argv]
    import run_all
    sys.exit(run_all.main(argv))


if __name__ == "__main__":
    main()
