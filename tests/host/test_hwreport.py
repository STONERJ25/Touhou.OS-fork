#!/usr/bin/env python3
"""Host tests for tools/hwreport.py, the reader of the hardware reports (docs/HARDWARE_DB.md), against reports taken from QEMU runs of
different virtual machines (tests/host/hwreports/: Q35 with AHCI, NVMe, xHCI, HDA and e1000e and made-up SMBIOS strings; a bare Q35 with an
RTL8139; an i440fx with xHCI, HDA and virtio-net; a Q35 with NVMe, AC'97 and virtio-net; and a Q35 started with every option that removes a
subsystem). The files are QEMU's own virtual machines: no real identifiers are in them. Expectations that can be read off the files with a
plain regular expression are computed that way here, not by the module under test.

    python -m unittest discover -s tests/host -p "test_hwreport.py" -v       (no C compiler or QEMU needed)
"""
import io
import re
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO / "tools"))
import hwreport  # noqa: E402

FIXTURES = REPO / "tests" / "host" / "hwreports"
NAMES = ["qemu-q35-full", "qemu-q35-bare", "qemu-i440fx-hda-xhci", "qemu-q35-ac97-nvme", "qemu-q35-no-drivers"]


def text(name):
    return (FIXTURES / f"{name}.txt").read_text(encoding="utf-8")


class TestParse(unittest.TestCase):
    def test_every_fixture_is_a_whole_report(self):
        for n in NAMES:
            r = hwreport.load(FIXTURES / f"{n}.txt")
            self.assertEqual(r.format, 1)
            self.assertTrue(r.ended, n)
            self.assertEqual(hwreport.check(r), [], n)
            for s in hwreport.SECTIONS:
                self.assertIn(s, r.sections, f"{n}: [{s}]")

    def test_header(self):
        r = hwreport.load(FIXTURES / "qemu-q35-full.txt")
        self.assertTrue(r.head["build"].startswith("Gensokyo version"))
        self.assertRegex(r.head["date"], r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$")
        self.assertTrue(r.redacted)

    def test_tolerates_windows_line_ends_a_byte_order_mark_comments_and_unknown_parts(self):
        t = "\ufeff" + text("qemu-q35-bare").replace("\n", "\r\n") + "\r\n[from the future]\r\nsomething: 1\r\n# a note\r\n"
        r = hwreport.parse(t)
        self.assertEqual(r.get("from the future", "something"), "1")
        self.assertEqual(r.get("system", "bios.vendor"), "EDK II")
        self.assertTrue(r.ended)

    def test_a_cut_report_is_noticed(self):
        t = text("qemu-q35-bare")
        cut = t[:t.index("[log]")]
        r = hwreport.parse(cut)
        problems = hwreport.check(r)
        self.assertTrue(any("cut short" in p for p in problems), problems)
        self.assertTrue(any("[log] is missing" in p for p in problems), problems)

    def test_not_a_report(self):
        for junk in ("", "hello\nworld\n", "[system]\nbios.vendor: x\n"):
            with self.assertRaises(hwreport.ReportError):
                hwreport.parse(junk)

    def test_pci_lines(self):
        fns = hwreport.pci_functions(hwreport.load(FIXTURES / "qemu-q35-full.txt"))
        raw = re.findall(r"^pci\.([0-9a-f:.]+): ([0-9a-f]{4}):([0-9a-f]{4}) class=([0-9a-f]{4}):([0-9a-f]{2}) driver=(\S+)", text("qemu-q35-full"), re.M)
        self.assertEqual(len(fns), len(raw))
        self.assertEqual(len(fns), 9)
        by = {f.bdf: f for f in fns}
        e1000 = by["00:02.0"]
        self.assertEqual((e1000.id, e1000.driver, e1000.cls, e1000.sub), ("8086:10d3", "e1000", 2, 0))
        self.assertTrue(e1000.msi and e1000.msix and e1000.pcie)
        self.assertEqual((e1000.link, e1000.max_link), ("2.5GT/s:x1", "2.5GT/s:x1"))
        self.assertIn("0:mem:0x20000", e1000.bars)
        self.assertEqual(by["00:1f.2"].driver, "ahci")
        self.assertEqual(by["00:1f.2"].name, "SATA controller (AHCI)")


class TestDrivers(unittest.TestCase):
    def reports(self, names=NAMES):
        return [hwreport.load(FIXTURES / f"{n}.txt") for n in names]

    def expected(self, show_all):
        """Read straight off the text: how many reports hold each id without a driver."""
        count = {}
        for n in NAMES:
            ids = set()
            for bdf, ven, dev, cc, drv in re.findall(r"^pci\.([0-9a-f:.]+): ([0-9a-f]{4}):([0-9a-f]{4}) class=([0-9a-f]{4}):[0-9a-f]{2} driver=(\S+)", text(n), re.M):
                if drv == "none" and (show_all or int(cc, 16) not in (0x0600, 0x0601, 0x0604, 0x0680, 0x0c05)):
                    ids.add(f"{ven}:{dev}")
            for i in ids:
                count[i] = count.get(i, 0) + 1
        return count

    def test_counts_by_id(self):
        for show_all in (False, True):
            rows = hwreport.driver_table(self.reports(), show_all)
            self.assertEqual({r[1]: r[0] for r in rows}, self.expected(show_all), show_all)

    def test_the_backlog_order_is_most_common_first(self):
        rows = hwreport.driver_table(self.reports())
        counts = [r[0] for r in rows]
        self.assertEqual(counts, sorted(counts, reverse=True))
        self.assertEqual(rows[0][1], "1234:1111")                      # the display adapter: no machine has a driver for any GPU
        self.assertEqual(rows[0][0], 5)

    def test_bridges_are_hidden_unless_asked_for(self):
        quiet = {r[1] for r in hwreport.driver_table(self.reports())}
        loud = {r[1] for r in hwreport.driver_table(self.reports(), True)}
        self.assertNotIn("8086:29c0", quiet)                            # the Q35 host bridge
        self.assertIn("8086:29c0", loud)

    def test_the_machine_with_every_subsystem_off_lists_what_it_did_not_drive(self):
        r = hwreport.load(FIXTURES / "qemu-q35-no-drivers.txt")
        rows = {x[1] for x in hwreport.driver_table([r])}
        self.assertTrue({"8086:10d3", "8086:2668", "1b36:0010", "1b36:000d", "8086:2922"} <= rows, rows)

    def test_one_report_counts_an_id_once(self):
        r = hwreport.parse(text("qemu-q35-bare").replace("[storage]", "pci.00:09.0: 1234:5678 class=0200:00 driver=none msi=no msix=no pcie=no link=- max=- bars=- name=x\n"
                                                                    "pci.00:0a.0: 1234:5678 class=0200:00 driver=none msi=no msix=no pcie=no link=- max=- bars=- name=x\n\n[storage]"))
        rows = hwreport.driver_table([r])
        self.assertEqual([x[0] for x in rows if x[1] == "1234:5678"], [1])


class TestSummary(unittest.TestCase):
    def test_full_machine(self):
        s = hwreport.summary(hwreport.load(FIXTURES / "qemu-q35-full.txt"))
        self.assertEqual(s["Machine"], "Touhou Test Reimu Box 9")
        self.assertIn("EDK II", s["Firmware"])
        self.assertIn("UEFI 2.7.0", s["Firmware"])
        self.assertEqual(s["Display"], "1024x768 (GOP)")
        self.assertEqual((s["Keyboard"], s["Mouse"]), ("usb+ps/2", "usb+ps/2"))
        self.assertIn("AHCI", s["Storage"])
        self.assertIn("NVMe", s["Storage"])
        self.assertTrue(s["Storage"].endswith("3 disks"), s["Storage"])
        self.assertEqual(s["Network"], "e1000e up")
        self.assertEqual(s["Audio"], "hda (1af40012)")
        self.assertEqual(s["Shutdown"], "?")                            # only a person can say
        self.assertEqual(s["Date"][:4], "2026")

    def test_a_device_without_a_driver_says_so(self):
        s = hwreport.summary(hwreport.load(FIXTURES / "qemu-q35-no-drivers.txt"))
        self.assertEqual(s["Network"], "no driver for 8086:10d3")
        self.assertEqual(s["Audio"], "no driver for 8086:2668")
        self.assertIn("(no driver)", s["Storage"])

    def test_every_summary_has_every_column(self):
        for n in NAMES:
            s = hwreport.summary(hwreport.load(FIXTURES / f"{n}.txt"))
            self.assertEqual(list(s), hwreport.COLUMNS)
            self.assertEqual(hwreport.table_row(s).count("|"), len(hwreport.COLUMNS) + 1)

    def test_the_table_in_the_documentation_has_the_same_columns(self):
        doc = (REPO / "docs" / "HARDWARE_DB.md").read_text(encoding="utf-8")
        header = [l for l in doc.splitlines() if l.startswith("| Machine | CPU |")]
        self.assertEqual(len(header), 1, "docs/HARDWARE_DB.md should hold one table header")
        self.assertEqual([c.strip() for c in header[0].strip("|").split("|")], hwreport.COLUMNS)


class TestPrivacy(unittest.TestCase):
    def test_default_fixtures_hold_nothing_private(self):
        r = hwreport.load(FIXTURES / "qemu-q35-full.txt")
        t = text("qemu-q35-full")
        for secret in ("SYSSER98765", "BOARDSER4321", "SATASER123", "NVMESER456", "11223344-5566-7788-99aa-bbccddeeff00", "52:54:00:aa:bb:cc", "aa:bb:cc"):
            self.assertNotIn(secret, t)
        self.assertIn("52:54:00:xx:xx:xx", t)
        self.assertEqual(hwreport.check(r), [])

    def test_the_check_notices_what_slipped_through(self):
        t = text("qemu-q35-full")
        for bad in (t.replace("52:54:00:xx:xx:xx", "52:54:00:aa:bb:cc"), t.replace("system.uuid: <uuid>", "system.uuid: 11223344-5566-7788-99aa-bbccddeeff00"),
                    t.replace("system.serial: <redacted>", "system.serial: SYSSER98765"), t.replace("disk.1.serial: <redacted>", "disk.1.serial: SATASER123")):
            self.assertNotEqual(hwreport.check(hwreport.parse(bad)), [])

    def test_a_report_that_says_it_is_unredacted_is_not_blamed_for_it(self):
        t = text("qemu-q35-full").replace("redaction: on", "redaction: off").replace("system.serial: <redacted>", "system.serial: SYSSER98765")
        self.assertEqual(hwreport.check(hwreport.parse(t)), [])


class TestCommandLine(unittest.TestCase):
    def run_main(self, *args):
        out = io.StringIO()
        with redirect_stdout(out):
            rc = hwreport.main(list(map(str, args)))
        return rc, out.getvalue()

    def test_one_report_prints_one_row(self):
        rc, out = self.run_main(FIXTURES / "qemu-q35-full.txt")
        self.assertEqual(rc, 0)
        self.assertEqual(len([l for l in out.splitlines() if l.startswith("| ")]), 1)
        self.assertNotIn("reports  id", out)

    def test_a_folder_prints_a_row_each_and_the_driver_table(self):
        rc, out = self.run_main(FIXTURES, "--header")
        self.assertEqual(rc, 0)
        self.assertEqual(len([l for l in out.splitlines() if l.startswith("| ")]), len(NAMES) + 1)       # the header too
        self.assertIn("reports  id", out)
        self.assertIn("1234:1111", out)

    def test_check_mode(self):
        rc, out = self.run_main("--check", FIXTURES)
        self.assertEqual(rc, 0)
        self.assertIn("5 report(s) checked: fine", out)

    def test_check_fails_on_a_broken_file(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "cut.txt"
            p.write_text(text("qemu-q35-bare")[:500], encoding="utf-8")
            rc, out = self.run_main("--check", p)
            self.assertEqual(rc, 1)
            self.assertIn("cut short", out)


if __name__ == "__main__":
    unittest.main()
