"""Power: ACPI tables, soft-off and reset through the firmware's own registers, who may close the shrine."""
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
        assert "reset:       acpi register" in out, out
        assert "processors:  1" in out, out
        out, _ = test_shell.run(vm, "dmesg | grep acpi:")
        assert "S5 known" in out, out


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
