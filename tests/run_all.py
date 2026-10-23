#!/usr/bin/env python3
"""Scripted regression tests: boots the kernel in QEMU and checks serial output / guest behaviour.

  python tests/run_all.py                    run everything
  python tests/run_all.py usb                run the tests whose name starts with "usb"
  python tests/run_all.py usb,blk_,~clock    comma-separated filters; a leading ~ means "contains" instead of "starts with"

Tests live in tests/case_*.py and register themselves with the @test decorator from harness.py.
"""
import importlib
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import harness  # noqa: E402

for module in sorted(HERE.glob("case_*.py")):
    importlib.import_module(module.stem)


def main():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except AttributeError:
            pass
    wants = [w for w in (sys.argv[1] if len(sys.argv) > 1 else "").split(",") if w] or [""]
    failed, ran = [], 0
    for fn in harness.TESTS:
        if not any((w[1:] in fn.__name__) if w.startswith("~") else fn.__name__.startswith(w) for w in wants):
            continue
        ran += 1
        t0 = time.time()
        try:
            fn()
            print(f"PASS {fn.__name__:28} {time.time() - t0:5.1f}s  {fn.__doc__ or ''}", flush=True)
        except Exception as e:  # noqa: BLE001
            failed.append(fn.__name__)
            print(f"FAIL {fn.__name__:28} {time.time() - t0:5.1f}s  {fn.__doc__ or ''}\n     {str(e)[:1500]}", flush=True)
    print(f"\n{ran} tests, {len(failed)} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
