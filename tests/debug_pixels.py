#!/usr/bin/env python3
"""Boot into the desktop, wait, print a few pixels and save a screenshot. For debugging tests/case_desktop.py."""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
from qemu import Qemu, BUILD  # noqa: E402

with Qemu(cmdline="", name="t-pix", mem="768M") as vm:
    vm.expect(r"opening the desktop for reimu", timeout=90)
    for t in range(8):
        time.sleep(3)
        w, h, rgb = vm.screen_ppm()
        print(t, w, h, [vm.pixel(x, y, (w, h, rgb)) for x, y in ((30, 752), (850, 160), (500, 400), (900, 750))])
    vm.screenshot(BUILD / "t-pix.png")
    print(vm.text()[-800:])
