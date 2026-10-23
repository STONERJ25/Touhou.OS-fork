#!/usr/bin/env python3
"""Debug helper: inject keys over QMP and look at what the guest echoes."""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
from qemu import Qemu  # noqa: E402

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
text = sys.argv[1].encode().decode("unicode_escape") if len(sys.argv) > 1 else "echo Reimu is on duty\n"
with Qemu(cmdline="", name="debug-kbd") as vm:
    vm.expect(r"kshell:# ", timeout=60)
    t0 = time.time()
    vm.type(text)
    print("typed in %.2fs" % (time.time() - t0))
    time.sleep(2)
    print(repr(vm.text()[-400:]))
