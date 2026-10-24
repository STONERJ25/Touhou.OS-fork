"""Registry for scripted tests. A test module (tests/case_*.py) does:

    from harness import test, Qemu, BUILD

    @test
    def my_feature():
        \"\"\"One-line description shown in the report.\"\"\"
        with Qemu(cmdline="...", name="t-my-feature") as vm:
            ...

run_all.py imports every case_*.py and runs the registered functions; an exception means failure.

A test that cannot pass in some environment for a known reason outside this project (an old QEMU, say) gets an
expected-failure marker under @test:

    @test
    @xfail(reason="QEMU before 9.0 ...", when=lambda: qemu_version() < (9, 0))
    def my_feature(): ...

When `when()` is true the test still runs: a failure is reported as XFAIL and does not fail the run; a pass is reported
as XPASS (the condition is too wide: narrow it) and does not fail the run either unless the marker says strict=True.
When `when()` is false (or there is no `when`: always expected to fail) the marker does nothing. docs/LINUX.md has the rules.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
from qemu import Qemu, BUILD, scaled, qemu_version  # noqa: E402,F401

TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


class XFail:
    """What @xfail attaches to a test function (as fn.xfail); run_all.py reads it."""

    def __init__(self, reason, when=None, strict=False):
        if not reason:
            raise ValueError("@xfail needs a reason")
        self.reason, self.when, self.strict = reason, when, strict

    def applies(self):
        """True when the failure is expected in this environment."""
        return True if self.when is None else bool(self.when())


def xfail(reason, when=None, strict=False):
    """Marks a test as expected to fail (with a reason) when `when()` is true; see the module docstring."""
    mark = XFail(reason, when, strict)

    def deco(fn):
        fn.xfail = mark
        return fn
    return deco
