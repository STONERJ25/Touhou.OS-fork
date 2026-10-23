#!/usr/bin/env python3
"""Boots with the given command line, waits a few seconds and dumps the cleaned serial text (debug helper)."""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
from qemu import Qemu  # noqa: E402

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
cmdline = sys.argv[1] if len(sys.argv) > 1 else ""
wait = float(sys.argv[2]) if len(sys.argv) > 2 else 8
with Qemu(cmdline=cmdline, name="debug") as vm:
    time.sleep(wait)
    print(repr(vm.text()[-1500:]))
    print("alive:", vm.alive())
