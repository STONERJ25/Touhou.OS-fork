"""The graphical session: boots into the desktop, then drives it with the pointer and keyboard."""
import time

from harness import test, Qemu, BUILD, scaled


def near(p, q, tol=40):
    return all(abs(a - b) <= tol for a, b in zip(p, q))


@test
def desktop_boots():
    """Booting with no options opens fairyd, the wallpaper and the panel (the start button is shrine red)."""
    with Qemu(cmdline="", name="t-desktop", mem="768M") as vm:
        vm.expect(r"opening the desktop for reimu", timeout=90)
        deadline = time.time() + scaled(25)
        ok = False
        while time.time() < deadline:
            shot = vm.screen_ppm()
            if near(vm.pixel(118, 756, shot), (0xB5, 0x12, 0x2B), 60):          # the start button
                ok = True
                break
            time.sleep(0.5)
        assert ok, "the panel never appeared; serial tail:\n" + vm.text()[-500:]
        shot = vm.screen_ppm()
        assert near(vm.pixel(850, 160, shot), (0xF3, 0xEE, 0xDA), 60), "the moon should be in the sky"
        vm.screenshot(BUILD / "t-desktop.png")


@test
def desktop_terminal():
    """Ctrl+Alt+T opens a terminal window; commands typed into it run and print their output."""
    with Qemu(cmdline="", name="t-desktop-term", mem="768M") as vm:
        vm.expect(r"opening the desktop for reimu", timeout=90)
        time.sleep(7)
        vm.key("t", ctrl=True, alt=True)
        time.sleep(4)
        vm.type("echo gensokyo-$((6*7))\n")
        time.sleep(2.5)
        shot = vm.screen_ppm()
        vm.screenshot(BUILD / "t-desktop-term.png")
        # the terminal paints text in its own window: look for light pixels well inside where it opens
        light = sum(1 for x in range(60, 400, 3) for y in range(60, 140, 3) if sum(vm.pixel(x, y, shot)) > 600)
        assert light > 30, "no terminal text on screen"
