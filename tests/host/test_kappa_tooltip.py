#!/usr/bin/env python3
"""Host test of Kappa's tooltip placement and wrapping (user/lib/kappa/src/tooltip.h), through tests/host/kappatool_tooltip.c.

A tooltip is drawn inside the window that asked for it, so it must fit in that window whatever its size: the panel (36 pixels high) lost the top 8 pixels
of the Notebook launcher's tip on 2026-11-03. The C program tries every pointer position in windows from the panel's strip to a screen and checks the box
is inside, is where the rules say, and that the text wraps to the window's width without losing words (AddressSanitizer and UBSan are on).

    python tools/hosttest.py discover -s tests/host -p "test_kappa_tooltip.py" -v
"""
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import x86ref  # noqa: E402

HERE = Path(__file__).resolve().parent


class Tooltip(unittest.TestCase):
    def test_placement_and_wrapping(self):
        exe = x86ref.build("kappatool_tooltip", [HERE / "kappatool_tooltip.c"])
        r = subprocess.run([str(exe)], capture_output=True, text=True, errors="replace")
        self.assertEqual(r.returncode, 0, r.stdout[-3000:] + r.stderr[-2000:])
        self.assertTrue(r.stdout.strip().endswith("OK"), r.stdout)


if __name__ == "__main__":
    unittest.main()
