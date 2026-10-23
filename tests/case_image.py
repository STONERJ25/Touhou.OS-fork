"""The stick image: tools/mkimage.py builds a GPT disk image that firmware boots, the way a PC boots a USB stick.

Each test makes a small image from the current build/esp (so run tools/build.py first) and boots QEMU from it alone, with
no fat: directory in sight, on the bus a real machine would use: a USB stick behind xHCI, SATA, NVMe."""
import re
import subprocess
import sys
import time
from pathlib import Path

import test_shell
from guitools import START_BUTTON, SHRINE_RED, near
from harness import test, Qemu, BUILD

ROOT = Path(__file__).resolve().parent.parent
IMG = BUILD / "t-stick.img"


def make_image(cmdline):
    out = subprocess.run([sys.executable, str(ROOT / "tools" / "mkimage.py"), "--no-build", "--out", str(IMG), "--size", "256M",
                          "--cmdline", cmdline], capture_output=True, text=True)
    assert out.returncode == 0, out.stdout[-800:] + out.stderr[-800:]


@test
def image_usb_stick_boots_desktop():
    """The image on a USB stick behind xHCI boots the way a PC does: firmware loads the loader from the stick, the kernel
    takes the controller the firmware just used, and reimu's desktop opens."""
    make_image("video=1024x768 blk.ro")
    with Qemu(cmdline="", name="t-image-usb", mem="768M", boot_image=IMG, boot_bus="usb") as vm:
        vm.expect(r"opening the desktop for reimu", timeout=120)
        deadline = time.time() + 60
        ok = False
        while time.time() < deadline and not ok:
            ok = near(vm.pixel(*START_BUTTON, vm.screen_ppm()), SHRINE_RED, 60)
            time.sleep(0.5)
        assert ok, "the desktop panel never appeared; serial tail:\n" + vm.text()[-600:]
        txt = vm.text()
        assert re.search(r"cmdline 'video=1024x768 blk\.ro'", txt), "the image's own cmdline.txt must be used"
        assert re.search(r"\[Kappa\] .*xhci|\[Kappa\] usb", txt), "the kernel should have started its own xHCI driver:\n" + txt[-900:]


@test
def image_sata_disk_is_read_only():
    """On SATA the image shows up as sda with its two partitions; with blk.ro (the image's default) nothing can write to it,
    not even root."""
    make_image("text blk.ro")
    with Qemu(cmdline="", name="t-image-sata", boot_image=IMG, boot_bus="sata") as vm:
        vm.expect(test_shell.PROMPT, timeout=120)
        out, _ = test_shell.run(vm, "cat /proc/partitions")
        assert "sda" in out and "sda1" in out and "sda2" in out, out
        out, rc = test_shell.run(vm, "su marisa -c 'sudo dd if=/dev/zero of=/dev/sda2 bs=512 count=1 2>&1'")
        assert re.search(r"[Rr]ead-only|EROFS|not permitted", out) and "1 records out" not in out, (rc, out)
        out, _ = test_shell.run(vm, "su marisa -c 'sudo dd if=/dev/sda of=/dev/null bs=512 count=1 2>&1'")
        assert "512" in out or "1+0" in out, out                  # reading is fine


@test
def image_nvme_and_virtio_boot():
    """The same image boots from an NVMe drive and from virtio-blk, and the kernel logs which volume became /HumanVillage."""
    make_image("text")
    for bus, dev in (("nvme", "nvme0n1"), ("virtio", "vda")):
        with Qemu(cmdline="", name=f"t-image-{bus}", boot_image=IMG, boot_bus=bus) as vm:
            vm.expect(test_shell.PROMPT, timeout=120)
            out, _ = test_shell.run(vm, "cat /proc/partitions; mount | grep -i humanvillage")
            assert dev in out, (bus, out)
            assert "HumanVillage" in out, (bus, out)            # mounted by the policy: the stick's own HUMANVILLAGE partition
