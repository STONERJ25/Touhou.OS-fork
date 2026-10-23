#!/usr/bin/env python3
"""Scripted regression tests: boots the kernel in QEMU and checks serial output / guest behaviour.

  python tests/run_all.py            run everything
  python tests/run_all.py selftest   run tests whose name contains the argument
"""
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
from qemu import Qemu, BUILD  # noqa: E402

TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


@test
def selftest():
    """In-kernel self tests: pmm, heap, vmm, scheduler."""
    with Qemu(cmdline="selftest", name="t-selftest") as vm:
        vm.expect(r"SELFTEST (PASS|FAIL)", timeout=60)
        assert "SELFTEST PASS" in vm.text(), vm.text()[-800:]
        assert vm.wait_exit(10) == 0


def crash_case(kind, expected):
    def run():
        with Qemu(cmdline=f"crash={kind}", name=f"t-crash-{kind}") as vm:
            vm.expect(r"INCIDENT: KERNEL PANIC", timeout=40)
            vm.expect(r"backtrace:\n(.*\n){2}", timeout=10)
            text = vm.text()
            assert re.search(expected, text), f"expected /{expected}/ in:\n{text[-1500:]}"
            assert "crash_test" in text or "kmain" in text, "backtrace should name kernel functions"
    run.__name__ = f"crash_{kind}"
    run.__doc__ = f"deliberate kernel fault '{kind}' must produce a diagnosable panic"
    TESTS.append(run)


crash_case("div0", r"Divide Error")
crash_case("pf", r"BOUNDARY VIOLATION: not-present page on write of 0xdead0000 by kernel")
crash_case("null", r"null pointer")
crash_case("gp", r"General Protection")
crash_case("ud", r"Invalid Opcode")
crash_case("nx", r"BOUNDARY VIOLATION: protection violation on instruction fetch")
crash_case("ro", r"BOUNDARY VIOLATION: protection violation on write")
crash_case("early-pf", r"BOUNDARY VIOLATION")
crash_case("panic", r"requested panic")


def main():
    want = sys.argv[1] if len(sys.argv) > 1 else ""
    failed = []
    for fn in TESTS:
        if want not in fn.__name__:
            continue
        t0 = time.time()
        try:
            fn()
            print(f"PASS {fn.__name__:28} {time.time() - t0:5.1f}s  {fn.__doc__ or ''}")
        except Exception as e:  # noqa: BLE001
            failed.append(fn.__name__)
            print(f"FAIL {fn.__name__:28} {time.time() - t0:5.1f}s  {fn.__doc__ or ''}\n     {str(e)[:1200]}")
    print(f"\n{len(TESTS) - len(failed) if not want else 'ran'} tests, {len(failed)} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
