"""Registry for scripted tests. A test module (tests/case_*.py) does:

    from harness import test, Qemu, BUILD

    @test
    def my_feature():
        \"\"\"One-line description shown in the report.\"\"\"
        with Qemu(cmdline="...", name="t-my-feature") as vm:
            ...

run_all.py imports every case_*.py and runs the registered functions; an exception means failure.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
from qemu import Qemu, BUILD, scaled  # noqa: E402,F401

TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn
