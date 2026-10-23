#!/usr/bin/env python3
"""Scripted regression tests: boots the kernel in QEMU and checks serial output / guest behaviour.

  python tests/run_all.py            run everything
  python tests/run_all.py usb        run tests whose name contains the argument

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
    want = sys.argv[1] if len(sys.argv) > 1 else ""
    failed, ran = [], 0
    for fn in harness.TESTS:
        if want not in fn.__name__:
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
