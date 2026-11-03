"""Power: ACPI tables, soft-off and reset through the firmware's own registers, who may close the shrine."""
import re

import test_shell
from harness import test, Qemu, scaled


@test
def acpi_tables():
    """The kernel reads RSDP/FADT/DSDT/MADT: /proc/acpi shows the PM1 control block, the S5 sleep type and the CPU count."""
    with Qemu(cmdline="text", name="t-acpi") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        out, rc = test_shell.run(vm, "cat /proc/acpi")
        assert rc == 0, out
        assert "poweroff:    S5 (acpi)" in out, out
        assert "reset:       acpi register io 0x" in out and "restart order: acpi, pci, kbd, then power off" in out, out
        assert "processors:  1" in out, out
        out, _ = test_shell.run(vm, "dmesg | grep acpi:")
        assert "S5 known" in out, out
        assert re.search(r"acpi: wake status at start: PM1 status [0-9a-f]{4} enable [0-9a-f]{4}; event block 0x[0-9a-f]+ status ([0-9a-f]{2} ?)+, enable ([0-9a-f]{2} ?)+", out), out


@test
def power_off():
    """A resident (group shrine, not root) can run poweroff: the ACPI soft-off ends the machine."""
    with Qemu(cmdline="text", name="t-poweroff") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        vm.send("poweroff\n")               # the farewell line can be lost when QEMU exits at once, so only the exit counts
        assert vm.wait_exit(60) is not None, "the machine did not power off"


@test
def power_restart():
    """reboot goes through the FADT reset register (QEMU runs with -no-reboot, so a reset ends the run)."""
    with Qemu(cmdline="text", name="t-restart") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        vm.send("reboot\n")
        assert vm.wait_exit(60) is not None, "the machine did not reset"


@test
def power_restart_ways():
    """reboot WAY starts with that way (acpi, pci, kbd, efi, triple) and the machine goes at once: no later way is tried; the efi way is the new call into the
    firmware's ResetSystem through a temporary identity map; the screen text and the beeps name the way; the order can be set with reboot= on the command line"""
    names = {"acpi": "the ACPI reset register", "pci": "the PCI reset port", "kbd": "the keyboard controller", "efi": "the firmware's ResetSystem", "triple": "a triple fault"}
    for way, text in names.items():
        with Qemu(cmdline="text", name=f"t-restart-{way}") as vm:
            vm.expect(test_shell.PROMPT, timeout=90)
            vm.send(f"reboot {way}\n")
            assert vm.wait_exit(60) is not None, f"the machine did not reset through {way}"
            log = vm.text()
            assert f"restart, way 1: {text}" in log, (way, log[-800:])
            assert "restart, way 2:" not in log, (way, "a later way was needed:\n" + log[-800:])
    with Qemu(cmdline="text reboot=efi,pci", name="t-restart-order") as vm:           # the command line decides the order for a plain reboot
        vm.expect(test_shell.PROMPT, timeout=90)
        vm.send("reboot\n")
        assert vm.wait_exit(60) is not None
        log = vm.text()
        assert "restart, way 1: the firmware's ResetSystem" in log and "restart, way 2:" not in log, log[-800:]


@test
def power_restart_leaves_out_what_froze_the_dell():
    """The Dell's FADT reset register is the SMI command port (io 0xb2 = 0x73): a software SMI that never came back and froze the machine before the PCI reset port, which
    does restart it, was reached (2026-10-25). Such a register is not in the default order (acpi.resetsmi pretends QEMU's is one), nor are the firmware's ResetSystem (a
    #GP inside it on that Dell) and a triple fault; /proc/acpi says what a plain restart tries; `reboot acpi` still names the register"""
    with Qemu(cmdline="text acpi.resetsmi", name="t-restart-smi") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        out, _ = test_shell.run(vm, "cat /proc/acpi")
        assert "the SMI command port: only when named" in out and "restart order: pci, kbd, then power off" in out, out
        vm.send("reboot\n")
        assert vm.wait_exit(60) is not None, "the machine did not restart through the PCI reset port"
        log = vm.text()
        assert "restart, way 1: the PCI reset port" in log and "restart, way 2:" not in log, log[-800:]
    with Qemu(cmdline="text acpi.resetsmi", name="t-restart-smi-named") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        vm.send("reboot acpi\n")                                   # named: it is tried (QEMU's register is a working one)
        assert vm.wait_exit(60) is not None, "the machine did not restart through the named ACPI register"
        assert "restart, way 1: the ACPI reset register" in vm.text()


@test
def power_restart_that_cannot_restart():
    """When no way restarts the machine (reboot=none stands in for a chipset that ignores them all, which is what the Dell did) the screen says so and the machine
    is switched off instead of being left to freeze with its network dead"""
    with Qemu(cmdline="text reboot=none", name="t-restart-none") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        vm.send("reboot\n")
        assert vm.wait_exit(60) is not None, "the machine neither restarted nor switched off"
        log = vm.text()
        assert "none of the ways to restart the machine worked: switching it off instead" in log, log[-800:]


@test
def power_needs_a_seat():
    """An outsider (sumireko: not root, not in group shrine) is refused, and the machine keeps running."""
    with Qemu(cmdline="text", name="t-power-eperm") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        out, rc = test_shell.run(vm, "su sumireko -c poweroff")
        assert rc != 0 and "not permitted" in out, (rc, out)
        assert "Goodbye" not in out, out                          # no farewell for a request that was refused
        out, rc = test_shell.run(vm, "echo still-here")
        assert "still-here" in out and vm.alive()


@test
def power_button_text():
    """No desktop: the ACPI power button (QMP system_powerdown) is polled by the kernel, which closes the machine itself."""
    with Qemu(cmdline="text", name="t-pwrbtn") as vm:
        vm.expect(test_shell.PROMPT, timeout=90)
        vm.qmp("system_powerdown")
        assert vm.wait_exit(60) is not None, "the machine did not power off"


@test
def power_button_desktop():
    """With the desktop up the button opens the shell's 'Leave the shrine?' dialog; a second press powers off."""
    import time
    with Qemu(cmdline="", name="t-pwrbtn-gui", mem="768M") as vm:
        vm.expect(r"opening the desktop for reimu", timeout=90)
        deadline = time.time() + scaled(40)
        while time.time() < deadline:
            p = vm.pixel(118, 756, vm.screen_ppm())
            if all(abs(a - b) <= 60 for a, b in zip(p, (0xB5, 0x12, 0x2B))):
                break
            time.sleep(0.5)
        else:
            raise AssertionError("the desktop panel never appeared")
        time.sleep(2)
        vm.qmp("system_powerdown")
        vm.expect(r"the power button was pressed", timeout=30)
        dialog = False
        deadline = time.time() + scaled(30)
        while time.time() < deadline and not dialog:                 # the dialog's red title bar sits at the middle of the screen
            p = vm.pixel(400, 301, vm.screen_ppm())
            dialog = all(abs(a - b) <= 50 for a, b in zip(p, (0xA0, 0x10, 0x28)))
            time.sleep(0.5)
        assert dialog, "the shutdown dialog did not open"
        assert vm.alive()
        vm.qmp("system_powerdown")
        assert vm.wait_exit(40) is not None, "the second press should have powered off"
