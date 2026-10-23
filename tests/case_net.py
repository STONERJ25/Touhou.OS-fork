"""Networking: the Yukari stack (loopback TCP/UDP/ICMP, sockets, robustness) and the NIC drivers.

The guest clock can run several times slower than wall time, so nothing here sleeps for a fixed time and then expects an
effect: every wait is for serial output with a generous timeout, and the guest-side checks (user/bin/nettest.c) retry."""
import re

import test_shell
from harness import test, Qemu, BUILD


def nettest(name="", timeout=240, setup=()):
    """Boots a text console, runs the in-guest nettest program (optionally one check) and returns its output."""
    with Qemu(cmdline="text", name="t-net-" + (name or "all")) as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        for line in setup:
            test_shell.run(vm, line, timeout=30)
        out, rc = test_shell.run(vm, "nettest " + name, timeout=timeout)
        assert "NETTEST PASS" in out and rc == 0, f"nettest failed (exit {rc}):\n{out[-1800:]}"
        return out


@test
def net_loopback():
    """loopback with no NIC: UDP (fragments, refusals), TCP echo, 5 MB bulk transfer, refused, non-blocking, close, half-close, many sockets, options, ping"""
    out = nettest()
    for check in ("udp", "tcp", "bulk", "refused", "nonblock", "close", "half", "many", "opts", "ping"):
        assert re.search(rf"ok\s+{check}\b", out), f"{check} did not pass:\n{out[-1500:]}"
    assert re.search(r"bulk: 5242880 bytes, checksum [0-9a-f]{8} ok", out), out[-800:]
