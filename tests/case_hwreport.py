"""hwreport (docs/HARDWARE_DB.md): the hardware report, run inside QEMU machines.

The reference machine is a Q35 with an AHCI disk, an NVMe drive, xHCI with a USB keyboard and mouse, an Intel HDA codec and an e1000e card,
with made-up SMBIOS strings, serial numbers and a MAC address, so that the test can look for them: the report has to agree with `lspci`,
`lsblk` and `lsusb` about what is there, must hold none of those identifiers by default and all of them with --full, and must land where
it is told to. Two more boots remove subsystems (every kernel option that switches one off) and a machine with no disk, and require a whole
report all the same. The reading side is tools/hwreport.py (tests/host/test_hwreport.py).

Run just these with:  python tools/qemu-slot.py -- python tests/run_all.py hwreport
"""
import re
import sys
from pathlib import Path

import test_shell
from case_storage import Image, hv_part, boot, MIB
from harness import test, Qemu, BUILD, scaled

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import hwreport  # noqa: E402

SECRETS = {
    "system serial": "SYSSER98765", "board serial": "BOARDSER4321", "SATA disk serial": "SATASER123", "NVMe serial": "NVMESER456",
    "system UUID": "11223344-5566-7788-99aa-bbccddeeff00", "MAC address": "52:54:00:aa:bb:cc", "the last three bytes of the MAC": "aa:bb:cc",
}


def img(name, mib=64):
    p = BUILD / name
    if not p.exists():
        p.write_bytes(bytes(mib << 20))
    return p


def reference_machine():
    """The extra QEMU arguments of the reference machine."""
    return [
        "-uuid", "11223344-5566-7788-99aa-bbccddeeff00",
        "-smbios", "type=1,manufacturer=Touhou Test,product=Reimu Box 9,version=1.0,serial=SYSSER98765,sku=RB9-SKU,family=Shrine",
        "-smbios", "type=2,manufacturer=Touhou Test,product=Hakurei Board,version=2.0,serial=BOARDSER4321",
        "-drive", f"if=none,id=sata0,format=raw,file={img('t-hwreport-sata.img')}", "-device", "ide-hd,drive=sata0,serial=SATASER123",
        "-drive", f"if=none,id=nvm0,format=raw,file={img('t-hwreport-nvme.img')}", "-device", "nvme,drive=nvm0,serial=NVMESER456",
        "-device", "qemu-xhci,id=xhci", "-device", "usb-kbd,bus=xhci.0", "-device", "usb-mouse,bus=xhci.0",
    ]


def sh(vm, cmd, timeout=90):
    out, rc = test_shell.run(vm, cmd, timeout=timeout)
    return out, rc


def whole(text):
    r = hwreport.parse(text)
    assert r.ended, "the report has no end line:\n" + text[-400:]
    for s in hwreport.SECTIONS:
        assert s in r.sections, f"section [{s}] is missing:\n" + text[:600]
    return r


@test
def hwreport_reference_machine():
    """On the Q35 reference machine the report agrees with lspci/lsblk/lsusb, keeps the machine's identifiers out by default and in with --full."""
    with Qemu(cmdline="text blk.ro", name="t-hwreport", mem="512M", net="e1000e,mac=52:54:00:aa:bb:cc", audio="hda", extra=reference_machine()) as vm:
        vm.expect(test_shell.PROMPT, timeout=150)
        out, rc = sh(vm, "hwreport")
        assert rc == 0, out[-600:]
        r = whole(out)
        assert r.head["format"] == "hwreport 1"
        assert r.head["build"].startswith("Gensokyo version"), r.head["build"]
        assert r.redacted
        problems = hwreport.check(r)
        assert not problems, problems

        # --- the numbers agree with the other tools
        lspci, _ = sh(vm, "lspci")
        want = {}
        for m in re.finditer(r"^([0-9a-f]{2}:[0-9a-f]{2}\.[0-9a-f]) \S+ ([0-9a-f]{4}:[0-9a-f]{4}) .*?(?:  \[(\S+)\])?$", lspci, re.M):
            want[m.group(1)] = (m.group(2), m.group(3) or "none")
        assert len(want) >= 9, lspci
        got = {f.bdf: (f.id, f.driver) for f in hwreport.pci_functions(r)}
        assert got == want, f"[pci] differs from lspci:\nreport {got}\nlspci  {want}"
        assert r.get("pci", "count").startswith(f"{len(want)} functions"), r.get("pci", "count")
        by = {f.bdf: f for f in hwreport.pci_functions(r)}
        for bdf, (ident, drv) in want.items():
            if drv in ("nvme", "xhci", "e1000", "ahci"):
                assert by[bdf].pcie or drv == "ahci", f"{bdf} {drv} should show a PCIe capability or be the Q35 AHCI function"
        assert any(f.msix for f in by.values()) and any(f.msi for f in by.values()), "the NVMe/xHCI MSI-X and the AHCI/HDA MSI capabilities should show"

        lsblk, _ = sh(vm, "lsblk")
        disks = {}
        for line in lsblk.splitlines():
            m = re.match(r"(\S+)\s+(\S+)\s+\d+\s+disk\s", line)
            if m:
                disks[m.group(1)] = m.group(2)
        assert len(disks) >= 3 and "nvme0n1" in disks, lsblk
        assert int(r.get("storage", "disks")) == len(disks), (r.get("storage", "disks"), disks)
        in_report = {}
        for k, v in r.section("storage"):
            m = re.match(r"disk\.\d+$", k) and re.match(r"(\S+) driver=\S+ size=\d+ \((\S+)\)", v)
            if m:
                in_report[m.group(1)] = m.group(2)
        assert in_report == disks, f"[storage] differs from lsblk:\n{in_report}\n{disks}"
        ctl = [f for f in hwreport.pci_functions(r) if f.cls == 1 or (f.cls == 8 and f.sub == 5)]
        assert int(r.get("storage", "controllers")) == len(ctl) == 2, (r.get("storage", "controllers"), [f.id for f in ctl])

        lsusb, _ = sh(vm, "lsusb")
        assert int(r.get("usb", "devices")) == len(re.findall(r" Port ", lsusb)) == 2, lsusb
        assert r.get("usb", "hosts") == "1" and "xHCI" in r.get("usb", "host.0")
        assert r.get("input", "keyboards").startswith("1 usb")

        ifc, _ = sh(vm, "ifconfig -a")
        assert "52:54:00:aa:bb:cc" in ifc, ifc
        nic = r.get("network", "interface.0")
        assert nic.startswith("eth0 driver=e1000e mac=52:54:00:xx:xx:xx link=up"), nic
        assert re.search(r"address=(10\.0\.2\.x|-)$", nic), nic                    # (the lease may not have arrived yet); a private address loses its last byte

        sysinfo = {k: v for k, v in r.section("system")}
        assert sysinfo["system.manufacturer"] == "Touhou Test" and sysinfo["system.product"] == "Reimu Box 9", sysinfo
        assert sysinfo["system.serial"] == "<redacted>" and sysinfo["system.uuid"] == "<uuid>", sysinfo
        assert r.get("cpu", "hypervisor").startswith("yes")
        assert r.get("firmware", "firmware.vendor") == "EDK II", r.section("firmware")[:4]
        assert re.match(r"1024x768, ", r.get("firmware", "gop.in_use")), r.get("firmware", "gop.in_use")
        assert int(r.get("firmware", "gop.modes").split()[0]) >= 3
        assert any(k == "acpi.table.1" for k, _ in r.section("acpi")) and r.get("acpi", "acpi.madt.ioapics") == "1"
        assert r.get("audio", "codec_id")

        # --- nothing private in the default report; all of it with --full
        for what, s in SECRETS.items():
            assert s not in out, f"the default report holds {what} ({s})"
        full, rc = sh(vm, "hwreport --full")
        assert rc == 0
        rf = whole(full)
        assert not rf.redacted
        for what, s in SECRETS.items():
            assert s in full, f"--full should keep {what} ({s})"
        assert re.search(r"address=(10\.0\.2\.15|-)\s", rf.get("network", "interface.0") + " "), rf.get("network", "interface.0")

        # --- where it goes
        w, rc = sh(vm, "hwreport -o")
        assert rc == 0 and "written to /tmp/hwreport.txt" in w, w                 # no HUMANVILLAGE volume here
        txt, _ = sh(vm, "cat /tmp/hwreport.txt")
        assert whole(txt).get("system", "system.product") == "Reimu Box 9"
        w, rc = sh(vm, "hwreport /tmp/other.txt")
        assert "written to /tmp/other.txt" in w, w
        w, rc = sh(vm, "hwreport --no-such-thing")
        assert rc == 2 and "usage: hwreport" in w, (rc, w)

        # --- firstlook carries the report at its end, and the secrets stay out of that part
        fl, rc = sh(vm, "su marisa -c 'sudo firstlook /tmp/fl.txt'; grep -c '^format: hwreport 1' /tmp/fl.txt; tail -n 3 /tmp/fl.txt", timeout=180)
        assert re.search(r"^1$", fl, re.M), fl[-600:]
        assert "end: hwreport 1" in fl, fl[-600:]
        tail, _ = sh(vm, "sed -n '/^===== hwreport/,$p' /tmp/fl.txt")
        for what, s in SECRETS.items():
            assert s not in tail, f"firstlook's hwreport part holds {what}"


def check_degraded(text, expect_controllers_without_driver=True):
    r = whole(text)
    problems = hwreport.check(r)
    assert not problems, problems
    if expect_controllers_without_driver:
        ctl = [re.match(r"\S+ \S+ (.*) driver=(\S+)$", v).groups() for k, v in r.section("storage") if re.match(r"controller\.\d+$", k)]
        assert ctl, r.section("storage")
        for kind, drv in ctl:                                                             # the SATA and NVMe controllers are there; nothing drives them
            assert drv == "none" or kind == "virtio-blk", (kind, drv, ctl)               # (the stick of the other test is virtio-blk, which is not switched off)
        assert "not available" in r.get("usb", "devices"), r.section("usb")
        assert "not available" in r.get("audio", "codec"), r.section("audio")
        assert r.get("network", "interfaces") == "0", r.section("network")
    return r


OFF = "ahci=off nvme=off usb=off usbstorage=off noaudio nonet"


@test
def hwreport_every_subsystem_off_writes_to_the_stick():
    """With every option that removes a subsystem the report is still whole; `hwreport -o` goes to /HumanVillage/hwreport.txt when that volume is mounted."""
    with Image("hwreport-hv", 64 * MIB, [hv_part()]) as image:
        with boot(image, cmdline=OFF, name="t-hwreport-off", extra=reference_machine()) as (g, vm):
            out = g.ok("hwreport")
            check_degraded(out)
            w = g.ok("hwreport -o")
            m = re.search(r"written to /HumanVillage(/\S*?hwreport\.txt) ", w)         # the top of the volume if the user may write there, else the user's home on it
            assert m, w
            where = m.group(1)
            g.poweroff()
        v = image.volume(1)
        try:
            data = v.read_file(where).decode("utf-8")
        finally:
            v.close()
        r = check_degraded(data)
        assert r.get("storage", "disks") == "1", r.section("storage")            # only the stick's own volume (virtio-blk): the SATA and NVMe drives are not looked at
        for what, s in SECRETS.items():
            assert s not in data, f"the file on the stick holds {what}"


@test
def hwreport_machine_without_a_disk():
    """No data disk, SATA and NVMe left alone: no disk at all, and the report says so and goes to /tmp."""
    with Qemu(cmdline="text blk.ro " + OFF, name="t-hwreport-nodisk", mem="512M") as vm:
        vm.expect(test_shell.PROMPT, timeout=150)
        out, rc = sh(vm, "hwreport")
        assert rc == 0, out[-400:]
        r = check_degraded(out)
        assert r.get("storage", "disks") == "0", r.section("storage")
        w, _ = sh(vm, "hwreport -o")
        assert "written to /tmp/hwreport.txt" in w, w
