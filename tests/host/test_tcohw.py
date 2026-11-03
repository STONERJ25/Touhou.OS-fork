#!/usr/bin/env python3
"""Host tests for shared/tcohw.h, the TCO watchdog code that the loader and the kernel share (docs/RELIABILITY.md), through tests/host/tcohw_sim.c.

QEMU's q35 is an ICH9, so the QEMU tests (tests/case_reliability.py) only ever run the older chipsets' path. The 100/200-series path, the one the
Dell OptiPlex 7050's Q270 takes (TCOBASE in the SMBus controller, NO_REBOOT behind the hidden P2SB bridge), runs here against a simulated
configuration space: where the registers are found, that the P2SB is hidden again afterwards with no bit written that was not set, that NO_REBOOT
is cleared without touching the bits around it, and the timer arithmetic.

    python3 -m unittest discover -s tests/host -p "test_tcohw.py" -v
"""
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import x86ref  # noqa: E402

HERE = Path(__file__).resolve().parent


def build():
    out = x86ref.OUT / "tcohw_sim"
    src, hdr = HERE / "tcohw_sim.c", x86ref.REPO / "shared" / "tcohw.h"
    if out.exists() and all(p.stat().st_mtime <= out.stat().st_mtime for p in (src, hdr)):
        return out
    x86ref.OUT.mkdir(parents=True, exist_ok=True)
    cc = x86ref.find_cc()
    gcc = "gcc" in Path(cc).name.lower()
    flags = ["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-Werror"] if gcc else ["-O2"]
    r = subprocess.run([cc, *flags, "-std=gnu17", "-Wall", "-Wextra", "-I", str(x86ref.REPO / "shared"), str(src), "-o", str(out)],
                       capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("building tcohw_sim failed:\n" + r.stdout + r.stderr)
    return out


def run(scenario):
    r = subprocess.run([str(build()), scenario], capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stdout + r.stderr
    return dict(line.split("=", 1) for line in r.stdout.splitlines() if "=" in line)


@unittest.skipUnless(x86ref.have_gcc(), "needs a C compiler")
class TcoHw(unittest.TestCase):
    def test_q35_ich9(self):
        o = run("q35")
        self.assertEqual((o["kind"], o["abase"], o["tcobase"], o["nr_phys"], o["nr_bit"]), ("1", "0x600", "0x660", "0xfed1f410", "0x20"))
        self.assertEqual(o["writes"], "", "the ICH path writes nothing to configuration space")

    def test_q270_the_dell(self):
        o = run("q270")
        self.assertEqual((o["kind"], o["abase"], o["tcobase"], o["nr_phys"], o["nr_bit"]), ("2", "0x1800", "0x400", "0xfdc6000c", "0x2"))
        self.assertEqual(o["writes"], "31:1:0xe0=0;31:1:0xe0=0x100", "the P2SB is unhidden and hidden again, nothing else")
        self.assertEqual(o["p2sbc"], "0x100")
        self.assertEqual(o["why"], "")

    def test_q270_hidden_p2sb_reads_all_ones(self):
        o = run("q270-ones")
        self.assertEqual((o["kind"], o["nr_phys"]), ("2", "0xfdc6000c"))
        self.assertEqual(o["writes"], "31:1:0xe0=0;31:1:0xe0=0x100", "no bit is written that was not set: only the hide bit comes back")

    def test_q270_p2sb_already_visible(self):
        o = run("q270-visible")
        self.assertEqual((o["kind"], o["nr_phys"], o["writes"], o["p2sbc"]), ("2", "0xfdc6000c", "", "0"))

    def test_q270_tco_not_decoded(self):
        o = run("q270-nodecode")
        self.assertEqual(o["kind"], "0")
        self.assertIn("decoding is switched off", o["why"])

    def test_not_intel(self):
        o = run("amd")
        self.assertEqual(o["kind"], "0")
        self.assertIn("not an Intel chipset", o["why"])

    def test_noreboot_and_the_timer(self):
        for scenario, bit in (("q35", 0x20), ("q270", 0x2)):
            o = run(scenario)
            self.assertEqual((o["nr_before"], o["nr_after"]), ("1", "0"), scenario)
            self.assertEqual(int(o["nr_mem"], 16), 0x201, f"{scenario}: only NO_REBOOT changes")
            self.assertEqual(int(o["tmr"], 16) & 0x3FF, 100, "120 s from the last feed is two countdowns of 100 ticks of 0.6 s")
            cnt1 = int(o["cnt1"], 16)
            self.assertFalse(cnt1 & 0x0800, "the counter runs")
            self.assertEqual(o["running"], "1")
            self.assertEqual(int(o["sts1"], 16) & 0x8, 0, "an old first timeout is forgotten")
            self.assertEqual(int(o["sts2"], 16) & 0x2, 0x2, "the previous start's SECOND_TO_STS is left for the kernel to read")
            self.assertGreaterEqual(int(o["rld_writes"]), 1)
            self.assertTrue(int(o["halted_cnt1"], 16) & 0x0800)

    def test_ticks(self):
        o = run("q35")
        self.assertEqual(o["ticks"], "1:2,2:2,3:3,60:50,120:100,600:500,1227:1023,5000:1023")
        self.assertEqual(o["seconds_of_100"], "120")


if __name__ == "__main__":
    unittest.main()
