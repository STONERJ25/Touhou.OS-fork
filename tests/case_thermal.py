"""CPU sensors (kernel/arch/x86_64/thermal.c, msrsafe.*, superio.c; docs/SENSORS.md): /proc/thermal, sensors, the guarded rdmsr, the Super I/O probe.

QEMU has no thermal sensor (its CPUID does not offer one), so the readings are tested with the kernel's test hook: thermal.fake=MSR:VALUE[+STEP],...
replaces the CPU by a table, and the numbers and the whole text of `sensors` are checked exactly against values worked out by hand from the Intel SDM:

    1a2:640000      MSR_TEMPERATURE_TARGET: bits 23:16 = 0x64, TjMax 100 C
    19c:883b0000    IA32_THERM_STATUS: valid, resolution 1, digital readout 0x3b = 59 C below TjMax -> 41 C
    1b1:883a0c03    IA32_PACKAGE_THERM_STATUS: readout 0x3a = 58 -> 42 C; bits 0 and 1 (thermal status + its log), bits 10 and 11 (power limit + its log)
    198:2200        IA32_PERF_STATUS: bits 15:8 = 0x22 = 34, times the 100 MHz reference clock = 3400 MHz
    606:a0e03       MSR_RAPL_POWER_UNIT: energy unit 1/2^14 J (bits 12:8 = 14)
    611:1000000+2000  MSR_PKG_ENERGY_STATUS: 0x1000000 and 0x2000 more at every read, so that the power has something to be computed from

The real registers can only be tried on the Dell (docs/SENSORS.md has the plan).
"""
import re
import time

import test_shell
from harness import test, Qemu, scaled

ROOT = r"root@gensokyo \S+ # "
FAKE = "1a2:640000,19c:883b0000,1b1:883a0c03,198:2200,606:a0e03,611:1000000+2000"
ESU = 14
FAN = "not readable here: the fan is not on the CPU (man sensors; as root, sensors --probe-lpc looks for the chip that has it)"


def run(vm, cmd, timeout=90):
    return test_shell.run(vm, cmd, timeout=timeout, prompt=ROOT)


def table(text):
    """The `key: value` lines of /proc/thermal (the first block if the text holds several)."""
    d = {}
    for line in text.splitlines():
        if ": " in line:
            k, v = line.split(": ", 1)
            if k in d:
                break
            d[k] = v
    return d


@test
def thermal_no_sensor_and_the_guarded_read():
    """QEMU has no sensor: /proc/thermal and sensors say so cleanly, the guarded rdmsr returns what a plain one does, a deliberate #GP is caught, and the Super I/O probe finds an empty bus and is root's"""
    with Qemu(cmdline="text user=root thermal.fake=zzz msrsafe.test", name="t-thermal-none") as vm:
        vm.expect(ROOT, timeout=120)
        log = vm.text()
        assert "[Utsuho] thermal.fake=zzz is not understood" in log, log[-1500:]                    # a table that does not parse is ignored, not obeyed
        m = re.search(r"\[Utsuho\] msrsafe: ready yes; EFER by the guarded read (0x[0-9a-f]+) \(direct read (0x[0-9a-f]+)\)", log)
        assert m and m.group(1) == m.group(2) and int(m.group(1), 16) & (1 << 10), (m and m.group(0))  # the long-mode-active bit is set in both
        assert "[Utsuho] msrsafe: a deliberate #GP was caught" in log, log[-1500:]                  # the handler turned a real #GP into a return value
        out, rc = run(vm, "cat /proc/thermal")
        info = table(out)
        if info.get("thermal") == "no thermal sensor":                                               # what QEMU's CPU models do
            assert info["reason"] and "[Utsuho] thermal: no thermal sensor" in vm.text()
            out2, rc2 = run(vm, "sensors")
            assert rc2 == 1 and out2.startswith("no thermal sensor: "), (out2, rc2)
            assert run(vm, "sensors -r")[0] == out
        else:                                                                                        # a hypervisor that does offer one: the shape must still hold
            assert info["thermal"] == "digital thermal sensor" and "tjmax" in info, out
            out2, rc2 = run(vm, "sensors")
            assert rc2 == 0 and "Core 0" in out2, (out2, rc2)
        assert "Call Trace" not in vm.text() and "KERNEL PANIC" not in vm.text()
        # the Super I/O probe: never at boot, root only, and QEMU's bus is empty
        out, rc = run(vm, "cat /proc/superio")
        assert out.startswith("superio: not probed") and "sensors --probe-lpc" in out, out
        out, rc = run(vm, "su reimu -c 'sensors --probe-lpc; echo rc=$?'")
        assert "only root may" in out and "rc=1" in out, out
        out, rc = run(vm, "cat /proc/superio")
        assert out.startswith("superio: not probed"), out                                            # the refused attempt probed nothing
        out, rc = run(vm, "sensors --probe-lpc")
        assert rc == 0 and "superio: nothing answered" in out and "port.0x2e: nothing answered" in out and "port.0x4e: nothing answered" in out, (out, rc)
        assert "fans: not read" in out
        assert run(vm, "cat /proc/superio")[0].startswith("superio: nothing answered")
        assert re.search(r"\[Utsuho\] superio: probed by '[^']+': nothing answered", vm.text()), vm.text()[-800:]
        out, rc = run(vm, "echo nonsense > /proc/superio; echo rc=$?")
        assert "superio: nothing answered" in run(vm, "cat /proc/superio")[0]                       # an unknown command probes nothing and breaks nothing


@test
def thermal_forced_reads_never_fault():
    """thermal.force: the real rdmsr of six registers that QEMU's CPU does not have, through the guard. The emulator answers zeros or raises #GP; either must end as a line of text, never as a panic"""
    with Qemu(cmdline="text user=root thermal.force", name="t-thermal-force") as vm:
        vm.expect(ROOT, timeout=120)
        log = vm.text()
        assert "KERNEL PANIC" not in log, log[-2000:]
        assert "[Utsuho] thermal: " in log and "[forced: thermal.force]" in log, log[-1500:]
        out, rc = run(vm, "cat /proc/thermal")
        t = table(out)
        assert t.get("thermal") == "digital thermal sensor" and t["source"].startswith("cpu, forced"), out
        assert t.get("core0.valid") == "no" or "core0.error" in t, out                               # zeros read as "not valid"; a #GP as "did not answer"
        assert t["tjmax"] == "100" and t["tjmax_source"].startswith("assumed"), t                    # a register that reads 0 is not a throttling temperature
        out2, rc2 = run(vm, "sensors")
        assert rc2 == 0 and "Limit (TjMax)" in out2 and "not valid" in out2, (out2, rc2)
        assert "KERNEL PANIC" not in vm.text()


@test
def thermal_off_touches_nothing():
    """thermal=off, the escape hatch for a machine that dislikes it: no register is read (even with a table and the self-test asked for), the IDT is not patched, the files say why"""
    with Qemu(cmdline=f"text user=root thermal=off msrsafe.test thermal.fake={FAKE}", name="t-thermal-off") as vm:
        vm.expect(ROOT, timeout=120)
        log = vm.text()
        assert "[Utsuho] thermal: off (thermal=off); no register is read" in log, log[-1500:]
        assert "msrsafe:" not in log and "[Utsuho] thermal: core 0" not in log, log[-1500:]        # neither the guarded-read check nor a reading was made
        out, rc = run(vm, "cat /proc/thermal")
        assert out.startswith("thermal: no thermal sensor\nreason: thermal=off on the kernel command line\n"), out
        out, rc = run(vm, "sensors")
        assert rc == 1 and out.startswith("no thermal sensor: thermal=off"), (out, rc)


@test
def thermal_a_gp_outside_the_table_still_panics():
    """the #GP stub is in front of the kernel's own: one at an address the table does not list (msrsafe.crash) is chained to it and panics as before, with the usual report"""
    with Qemu(cmdline="text msrsafe.test msrsafe.crash", name="t-msrsafe-crash") as vm:
        vm.expect(r"INCIDENT: KERNEL PANIC", timeout=60)
        vm.expect(r"backtrace:\n(.*\n){2}", timeout=10)
        text = vm.text()
        assert "[Utsuho] msrsafe: a deliberate #GP was caught" in text, text[-2500:]               # the table's own fault was swallowed first ...
        assert re.search(r"EXCEPTION 13: General Protection Fault \(#GP\) in kernel mode", text), text[-2500:]       # ... and this one was not
        assert "thermal_init" in text, "the backtrace should name the function that faulted"


@test
def thermal_fake_registers():
    """thermal.fake=: every number of /proc/thermal worked out from the registers by hand, the power from two reads of the energy counter, and the whole text of sensors"""
    with Qemu(cmdline=f"text user=root thermal.fake={FAKE}", name="t-thermal-fake") as vm:
        vm.expect(ROOT, timeout=120)
        assert "[Utsuho] thermal: core 0 at 41 C, package at 42 C (TjMax 100 C), 3400 MHz, energy counter present [synthetic: thermal.fake=]" in vm.text(), vm.text()[-1500:]
        out, rc = run(vm, "sleep 1; cat /proc/thermal; sleep 1; cat /proc/thermal")       # (the first sleep: a window needs half a second, and a guest that boots in 700 ms is not that old yet)
        blocks = out.split("thermal: digital thermal sensor\n")
        assert len(blocks) == 3, out
        a, b = table("thermal: digital thermal sensor\n" + blocks[1]), table("thermal: digital thermal sensor\n" + blocks[2])
        for t in (a, b):
            assert t["source"] == "synthetic (thermal.fake= on the kernel command line)", t
            assert (t["vendor"], t["cpu"]) == ("GenuineIntel", "family 6 model 0x9e stepping 9"), t
            assert (t["tjmax"], t["tjmax_source"], t["tjmax_offset"]) == ("100", "msr 0x1a2", "0"), t
            assert [t[k] for k in ("core0.valid", "core0.temp", "core0.margin", "core0.resolution", "core0.status", "core0.log")] == ["yes", "41", "59", "1", "-", "-"], t
            assert [t[k] for k in ("package.valid", "package.temp", "package.margin", "package.resolution")] == ["yes", "42", "58", "1"], t
            assert (t["package.status"], t["package.log"]) == ("hot power_limit", "hot power_limit"), t
            assert (t["ratio"], t["bus_khz"], t["mhz"]) == ("34", "100000", "3400"), t
            assert t["energy_unit"] == "1/2^14 J", t
            assert t["msr.0x19c"] == "0x00000000883b0000" and t["msr.0x1a2"] == "0x0000000000640000" and t["msr.0x1b1"] == "0x00000000883a0c03", t
            assert t["msr.0x198"] == "0x0000000000002200" and t["msr.0x606"] == "0x00000000000a0e03", t
            raw = int(t["energy_raw"], 16)
            assert raw > 0x1000000 and (raw - 0x1000000) % 0x2000 == 0, t                         # one more step for every read of the counter
            assert int(t["energy_uj"]) == raw * 1_000_000 >> ESU, t                                 # the unit conversion, exactly
        # the power: the energy between the two ends of a window, over the window's length, in milliwatts (a window is at least half a second)
        for t in (a, b):
            ms = int(t["power.window_ms"])
            lo, hi = int(t["power.window_from_raw"], 16), int(t["power.window_to_raw"], 16)
            assert ms >= 500 and hi > lo, t
            assert int(t["power.mw"]) == (((hi - lo) & 0xFFFFFFFF) * 1_000_000 >> ESU) // ms, t
        assert a["power.window_from_raw"] == "0x01000000", a                                       # the first window starts at the boot's reading of the counter
        assert b["power.window_from_raw"] == a["energy_raw"], (a, b)                              # and each next one where the last ended
        assert b["power.window_to_raw"] == b["energy_raw"], b
        assert int(b["power.window_ms"]) >= 1000, b                                                # (the sleep between the two reads)
        # sensors: the whole screen
        out, rc = run(vm, "sensors", timeout=120)
        lines = out.strip("\n").splitlines()
        want = ["(synthetic readings: thermal.fake= is on the kernel command line)",
                "%-15s %3d C  (%d C below the limit)" % ("Core 0", 41, 59),
                "%-15s %3d C  (%d C below the limit)  [hot power_limit]" % ("Package", 42, 58),
                "%-15s %3d C" % ("Limit (TjMax)", 100),
                "%-15s %4d MHz  (%d x %d MHz)" % ("Frequency", 3400, 34, 100)]
        assert rc == 0 and lines[:5] == want, (rc, lines)
        assert re.fullmatch(r"Package power   \d+\.\d W  \(average over \d+\.\d s\)", lines[5]), lines[5]
        assert lines[6] == "%-15s %s" % ("Fan", FAN) and len(lines) == 7, lines
        # sensors -r is the kernel's text
        out, rc = run(vm, "sensors -r")
        assert out.startswith("thermal: digital thermal sensor\n") and "msr.0x19c: 0x00000000883b0000" in out, out
        assert "KERNEL PANIC" not in vm.text()


@test
def thermal_fake_partial_table():
    """a table without TjMax, the frequency or the energy counter: TjMax is assumed (and says so), an invalid core reading is reported as invalid, a package reading alone is enough"""
    with Qemu(cmdline="text user=root thermal.fake=19c:003b0000,1b1:883a0000", name="t-thermal-partial") as vm:
        vm.expect(ROOT, timeout=120)
        out, rc = run(vm, "cat /proc/thermal")
        t = table(out)
        assert t["tjmax"] == "100" and t["tjmax_source"] == "assumed (msr 0x1a2 did not answer)", t
        assert (t["core0.valid"], "core0.temp" in t, t["package.valid"], t["package.temp"], t["package.margin"]) == ("no", False, "yes", "42", "58"), t
        assert "ratio" not in t and "mhz" not in t and "energy_raw" not in t and "power.mw" not in t, t        # what the table does not hold is not made up
        assert "msr.0x1a2" not in t and "msr.0x611" not in t, t
        out, rc = run(vm, "sensors")
        lines = out.strip("\n").splitlines()
        assert rc == 0 and lines == ["(synthetic readings: thermal.fake= is on the kernel command line)",
                                     "%-15s the sensor says its reading is not valid" % "Core 0",
                                     "%-15s %3d C  (%d C below the limit)" % ("Package", 42, 58),
                                     "%-15s %3d C" % ("Limit (TjMax)", 100),
                                     "%-15s %s" % ("Fan", FAN)], lines
        assert "KERNEL PANIC" not in vm.text()
