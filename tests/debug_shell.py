#!/usr/bin/env python3
"""Boot the shell and feed it the lines of a file (one command per line); prints everything the console says.
Usage: debug_shell.py lines.txt [seconds-to-wait-per-line]. For debugging."""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
from qemu import Qemu  # noqa: E402

lines = Path(sys.argv[1]).read_text(encoding="utf-8").splitlines()
wait = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0
with Qemu(cmdline="text", name="t-dbg") as vm:
    vm.expect(r"reimu@gensokyo \S+ \$ ", timeout=90)
    for line in lines:
        start = len(vm.text())
        vm.send(line + "\n")
        time.sleep(wait)
        print(f"--- sent {line!r}")
        out = vm.text()[start:]
        print(out[-1500:] if len(out) > 1500 else out)
