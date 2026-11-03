"""Kappa's tooltips stay whole (user/lib/kappa/src/tooltip.h; the placement rules are also checked over every pointer position by tests/host/test_kappa_tooltip.py).

    python tools/qemu-slot.py -- python tests/run_all.py tooltip
"""
import time

from harness import test, Qemu, BUILD, scaled
from guitools import Gui

TIP = (0xFF, 0xF8, 0xD0)                         # the fill of a tooltip
SHRINE_RED = (0xB5, 0x12, 0x2B)


def near(p, q, tol=40):
    return all(abs(a - b) <= tol for a, b in zip(p, q))


@test
def tooltip_of_the_panel_is_whole():
    """The tip of a panel launcher is drawn inside the 36 pixel high panel window and is whole: the 22 rows of the box, not cut off at the top (the
    Notebook launcher's tip lost its top 8 rows on the Dell on 2026-11-03)"""
    with Qemu(cmdline="", name="t-tooltip-panel", mem="768M") as vm:
        vm.expect(r"opening the desktop for reimu", timeout=90)
        deadline = time.time() + scaled(40)
        while not near(vm.pixel(118, 756), SHRINE_RED, 60):                       # the start button: the panel is painted
            assert time.time() < deadline, "the panel never appeared:\n" + vm.text()[-500:]
            time.sleep(0.5)
        time.sleep(1.5)
        # the panel is 36 pixels high at the bottom of the 1024x768 screen (y 732..767); the Notebook launcher is the third icon: its centre is at x 215, y 750
        Gui(vm).move_to(215, 750)
        deadline = time.time() + scaled(20)
        rows = []
        while time.time() < deadline:
            shot = vm.screen_ppm()
            # the tip is a 74 pixel wide box right of the pointer (x 229..302): a column left of the text is all fill, frame at the ends
            rows = [y for y in range(700, 768) if near(vm.pixel(231, y, shot), TIP, 14)]
            if len(rows) >= 18:
                break
            time.sleep(0.4)
        vm.screenshot(BUILD / "t-tooltip-panel.png")
        assert len(rows) >= 18, f"the tooltip is cut off: {len(rows)} fill rows at x 231 (a whole box has 20), at y {rows}"
        assert min(rows) >= 732 and max(rows) <= 767, f"the tooltip must stay inside the panel's window (y 732..767): {rows}"
        assert rows == list(range(rows[0], rows[0] + len(rows))), f"the fill rows are not one block: {rows}"


@test
def tooltip_popup_stays_on_the_screen():
    """A popup a client asks for beyond the bottom right corner of the screen (what a menu opened near the pointer is) is moved back inside by the window
    server: all of it is on the screen, none of it is cut off"""
    with Qemu(cmdline="text", name="t-popup-clamp", mem="768M") as vm:
        g = Gui(vm)
        g.start_desktop()
        g.sh("popuptest 1000 700 120 80 &")
        # the 120 x 80 popup asked for (1000, 700) on the 1024 x 768 screen: it belongs at (904, 688)
        deadline = time.time() + scaled(30)
        ok = False
        while time.time() < deadline:
            shot = vm.screen_ppm()
            ok = all(near(vm.pixel(x, y, shot), (0xFF, 0x00, 0x00), 30) for x, y in ((905, 690), (1020, 690), (905, 764), (1020, 764)))
            if ok:
                break
            time.sleep(0.4)
        vm.screenshot(BUILD / "t-popup-clamp.png")
        assert ok, "the popup is not whole on the screen: its corners at (905,690) (1020,690) (905,764) (1020,764) should be red"
        shot = vm.screen_ppm()
        assert not near(vm.pixel(900, 700, shot), (0xFF, 0x00, 0x00), 30), "the popup starts left of x 904: it was not moved"
