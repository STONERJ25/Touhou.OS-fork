"""Hi-dpi: the ui scale of Fairies, Kappa and the desktop (docs/DESKTOP.md, "Hi-dpi screens").

At 2560x1440 the window server picks scale 2 by itself: the logical screen is 1280x720, every logical pixel is a 2x2 block of
screen pixels. The tests look at the panel, the start button and its label (the 8x16 font must come out exactly twice as wide and
high: 63x14 lit pixels at 1x, 126x28 at 2x, four times as many lit pixels), open the start menu and a terminal with real mouse
clicks in screen pixels, click a cell of Minesweeper, which draws its own pixels and is enlarged by the server, and press keypad
buttons of the Calculator with the mouse at 1920x1080, where only ui.scale=2 gives scale 2. ui.scale=N on the command line forces
a scale; a scale that would leave less than 640x480 is lowered.

That scale 1 is unchanged pixel for pixel is checked by tests/pixel_identity.py (two builds, bit-exact screenshots), not here.

Positions: the panel and the start menu sit at the bottom of the logical screen, so on a screen of logical height H the start
button label starts at logical y H - 24 and the start button's centre is at logical (118, H - 12), the panel's gold top line at H - 36.
"""
import time

import numpy as np

from harness import test, Qemu, BUILD, scaled
from guitools import Gui, near

SHRINE_RED = (0xB5, 0x12, 0x2B)
PAPER = (0xEC, 0xE6, 0xD6)
GRASS = (0x66, 0xB0, 0x5A)
LABEL_1X = (63, 14, 263)          # the start button's "Gensokyo": width, height and number of lit pixels at scale 1


class ScreenGui(Gui):
    """Gui whose home() reaches the corner of a large screen (the pointer starts in the middle and moves in screen pixels).

    The homing moves overshoot and are absorbed at the screen edge; they must have arrived before the moves to the target are
    sent, or QEMU adds both into one pending motion and the overshoot eats part of the way to the target. So home() waits until
    the pointer's white arrow is seen in the top-left corner."""
    def home(self):
        for _ in range(24):
            self.vm.mouse_move(-90, -90)
            time.sleep(0.01)
        deadline = time.time() + scaled(15)
        while time.time() < deadline:
            a = arr(self.vm)
            if int((a[0:40, 0:30] == 255).all(axis=2).sum()) >= 10:
                break
            time.sleep(0.3)
        time.sleep(0.3)
        self.px = self.py = 0


def arr(vm):
    w, h, rgb = vm.screen_ppm()
    return np.frombuffer(rgb, dtype=np.uint8).reshape(h, w, 3).astype(int)


def start_desktop(vm, s, logical_h, timeout=90):
    """fairyd and hakurei-shell by hand; waits until the start button is painted at its place for scale s."""
    g = ScreenGui(vm)
    g.wait_prompt()
    vm.send("fairyd &\n")
    m = vm.expect(r"fairyd: ui scale (\d+) \((\w+|ui\.scale)\)", timeout=60)
    vm.send("hakurei-shell &\n")
    bx, by = 118 * s, (logical_h - 12) * s
    deadline = time.time() + scaled(timeout)
    while time.time() < deadline:
        if near(vm.pixel(bx, by), SHRINE_RED, 60):
            time.sleep(1)
            return g, int(m.group(1)), m.group(2)
        time.sleep(0.5)
    raise AssertionError("the panel never appeared; serial tail:\n" + vm.text()[-600:])


def label_metrics(a, s, logical_h):
    """Width, height and count of the light pixels of the start button label, in screen pixels."""
    x0, y0, x1, y1 = 36 * s, (logical_h - 28) * s, 124 * s, (logical_h - 8) * s
    light = a[y0:y1, x0:x1].min(axis=2) > 200
    ys, xs = np.nonzero(light)
    assert len(xs), "no label on the start button"
    return int(xs.max() - xs.min() + 1), int(ys.max() - ys.min() + 1), int(light.sum()), light, (int(xs.min()) + x0, int(ys.min()) + y0)


def check_label(a, s, logical_h):
    w, h, n, light, origin = label_metrics(a, s, logical_h)
    assert (w, h, n) == (LABEL_1X[0] * s, LABEL_1X[1] * s, LABEL_1X[2] * s * s), \
        f"the start button label should be {LABEL_1X[0] * s}x{LABEL_1X[1] * s} with {LABEL_1X[2] * s * s} lit pixels at scale {s}, found {w}x{h} with {n}"
    if s > 1:                     # every font pixel is an s x s block aligned to the logical grid
        x0, y0 = 36 * s, (logical_h - 28) * s
        ys, xs = np.nonzero(light)
        for y, x in zip(ys, xs):
            by, bx = (y + y0) // s * s - y0, (x + x0) // s * s - x0
            assert light[by:by + s, bx:bx + s].all(), f"glyph pixel at {x + x0},{y + y0} is not part of a full {s}x{s} block"


@test
def hidpi_scale1_unchanged():
    """At 1024x768 the scale stays 1 (automatic) and the start button and its label are where and as large as they always were."""
    with Qemu(cmdline="text", name="t-hidpi-1x", mem="768M") as vm:
        g, s, why = start_desktop(vm, 1, 768)
        assert (s, why) == (1, "automatic"), (s, why)
        a = arr(vm)
        assert a.shape[:2] == (768, 1024), a.shape
        check_label(a, 1, 768)
        assert "ui scale 1" in g.sh("cat /tmp/fairyd.stat"), "fairyd.stat should name the scale"


@test
def hidpi_auto_2560():
    """At 2560x1440 the scale is 2 by itself: panel, start button and font twice as large, the start menu and a terminal open with
    clicks in screen pixels, the window title bar sits where the 1x layout puts it, doubled."""
    with Qemu(cmdline="text", name="t-hidpi-2560", mem="768M", video="2560x1440") as vm:
        g, s, why = start_desktop(vm, 2, 720)
        assert (s, why) == (2, "automatic"), (s, why)
        a = arr(vm)
        assert a.shape[:2] == (1440, 2560), f"the firmware did not give 2560x1440: {a.shape}"
        check_label(a, 2, 720)
        gold = [y for y in range(1300, 1440) if near(tuple(a[y, 1200]), (0xD9, 0xA4, 0x41), 30)]
        assert gold == [(720 - 36) * 2, (720 - 36) * 2 + 1], f"the panel's gold top line should be two screen pixels at y 1368, found {gold}"
        vm.screenshot(BUILD / "t-hidpi-2560-desktop.png")
        # the start menu: a Kappa popup of the shell, opened with a click in screen pixels
        g.click(118 * 2, (720 - 12) * 2)
        menu_ok = False
        deadline = time.time() + scaled(20)
        while time.time() < deadline and not menu_ok:
            a = arr(vm)
            menu_ok = near(tuple(a[(720 - 300) * 2, 150 * 2]), PAPER, 30) and near(tuple(a[(720 - 300) * 2 + 1, 150 * 2 + 1]), PAPER, 30)
            time.sleep(0.5)
        assert menu_ok, "the start menu did not open"
        vm.screenshot(BUILD / "t-hidpi-2560-menu.png")
        # its first entry, Terminal: at 1x it is 515 logical pixels above the menu's bottom edge, which is 38 above the screen's
        g.click(108 * 2, (720 - 38 - 515) * 2)
        title_ok = False
        deadline = time.time() + scaled(40)
        while time.time() < deadline and not title_ok:
            a = arr(vm)
            # the first window: outer frame at logical (40, 30), title bar from y 34 to 55; its text starts at logical x 68
            title_ok = near(tuple(a[44 * 2, 200 * 2]), (0xC1, 0x1A, 0x38), 40) or near(tuple(a[44 * 2, 300 * 2]), (0xC1, 0x1A, 0x38), 60)
            time.sleep(0.5)
        assert title_ok, "no active title bar where the first window opens"
        time.sleep(2)
        vm.type("echo hidpi-$((6*7))\n")
        lit = 0
        deadline = time.time() + scaled(20)
        while time.time() < deadline and lit <= 60:
            a = arr(vm)
            lit = int((a[60 * 2:140 * 2:3, 60 * 2:400 * 2:3].sum(axis=2) > 600).sum())
            time.sleep(0.5)
        vm.screenshot(BUILD / "t-hidpi-2560-terminal.png")
        assert lit > 60, f"no terminal text on screen ({lit} light pixels)"
        stat = g.sh("cat /tmp/fairyd.stat")
        assert "screen 2560x1440" in stat and "ui scale 2" in stat, stat


@test
def hidpi_forced_scale2_and_enlarged_client():
    """ui.scale=2 at 2048x1536 gives the 1024x768 layout doubled; Minesweeper (a 1x buffer the server enlarges) answers a click on
    a cell given in screen pixels."""
    with Qemu(cmdline="text ui.scale=2", name="t-hidpi-2048", mem="768M", video="2048x1536") as vm:
        g, s, why = start_desktop(vm, 2, 768)
        assert (s, why) == (2, "ui.scale"), (s, why)
        a = arr(vm)
        assert a.shape[:2] == (1536, 2048), f"the firmware did not give 2048x1536: {a.shape}"
        check_label(a, 2, 768)
        g.sh("minesweeper --trace --seed 3 > /tmp/ms.log 2>&1 &")
        bb = None
        deadline = time.time() + scaled(60)
        while time.time() < deadline and not bb:
            if "new game" in g.sh("cat /tmp/ms.log"):
                time.sleep(2)
                a = arr(vm)
                mask = (np.abs(a - np.array(GRASS)) <= 6).all(axis=2)
                ys, xs = np.nonzero(mask)
                if len(xs) > 1000:
                    bb = (int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max()))
            time.sleep(1)
        assert bb, "the Minesweeper board did not appear"
        vm.screenshot(BUILD / "t-hidpi-2048-minesweeper.png")
        assert 350 <= bb[2] - bb[0] + 10 <= 380, f"the board should be 9 cells of 20 logical = 40 screen pixels, found {bb}"
        bx, by = bb[0] - 4, bb[1] - 4                      # the tile bevel is two logical pixels
        cell = lambda cx, cy: (bx + cx * 40 + 20, by + cy * 40 + 20)
        g.click(*cell(2, 6))
        out = ""
        deadline = time.time() + scaled(30)
        while time.time() < deadline and "reveal" not in out:
            out = g.sh("cat /tmp/ms.log")
            time.sleep(1)
        assert "minesweeper: reveal 2 6 -> " in out, "the click should land on cell 2,6:\n" + out[-600:]
        time.sleep(1)
        x, y = cell(2, 6)
        assert not near(vm.pixel(x - 14, y - 14), GRASS, 25), "the opened cell should not be grass any more"


CALC_LCD = (0x14, 0x1B, 0x42)      # the Calculator's display (COL_LCD)


@test
def hidpi_forced_scale2_1920_button_click():
    """ui.scale=2 at 1920x1080, where the automatic choice is 1: a 960x540 desktop drawn twice as large. The Calculator (a Kappa
    program drawing at full resolution) is driven with mouse clicks on its keypad buttons given in screen pixels: 7 + 2 = must
    give 9."""
    with Qemu(cmdline="text ui.scale=2", name="t-hidpi-1920", mem="768M", video="1920x1080") as vm:
        g, s, why = start_desktop(vm, 2, 540)
        assert (s, why) == (2, "ui.scale"), (s, why)
        a = arr(vm)
        assert a.shape[:2] == (1080, 1920), f"the firmware did not give 1920x1080: {a.shape}"
        check_label(a, 2, 540)
        gold = [y for y in range(940, 1080) if near(tuple(a[y, 900]), (0xD9, 0xA4, 0x41), 30)]
        assert gold == [(540 - 36) * 2, (540 - 36) * 2 + 1], f"the panel's gold top line should be two screen pixels at y 1008, found {gold}"
        g.sh("calculator --trace > /tmp/calc.log 2>&1 &")
        lcd = None
        deadline = time.time() + scaled(60)
        while time.time() < deadline and not lcd:
            if "calc: ready" in g.sh("cat /tmp/calc.log"):
                time.sleep(2)
                a = arr(vm)
                mask = (a[:1000] == np.array(CALC_LCD)).all(axis=2)
                xs = np.nonzero(mask.sum(axis=0) > 100)[0]           # columns and rows that cross the display (the night sky has a
                ys = np.nonzero(mask.sum(axis=1) > 300)[0]           # few pixels of the same colour)
                if len(xs) > 300 and len(ys) > 100:
                    lcd = (int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max()))
            time.sleep(1)
        assert lcd, "the Calculator did not appear"
        vm.screenshot(BUILD / "t-hidpi-1920-calculator.png")
        # the display is 396 x 86 logical pixels at content (8, 28) of a 680 x 420 window; the keypad starts at content y 122 with
        # keys of 76 x 45 every 80 x 49 logical pixels
        ox, oy = lcd[0] // 2 - 8, lcd[1] // 2 - 28
        assert abs((lcd[2] - lcd[0] + 1) - 396 * 2) <= 8, f"the display should be about {396 * 2} screen pixels wide: {lcd}"
        key = lambda col, row: ((ox + 8 + col * 80 + 38) * 2, (oy + 122 + row * 49 + 22) * 2)
        title = a[(oy - 12) * 2, (ox + 200) * 2]
        assert near(tuple(title), (0xC1, 0x1A, 0x38), 70), f"no active title bar above the Calculator: {tuple(title)}"
        for col, row in ((0, 2), (3, 5), (1, 4), (4, 5)):          # 7 + 2 =
            g.click(*key(col, row))
            time.sleep(0.4)
        out = ""
        deadline = time.time() + scaled(30)
        while time.time() < deadline and "calc: 7+2 = 9" not in out:
            out = g.sh("cat /tmp/calc.log")
            time.sleep(1)
        vm.screenshot(BUILD / "t-hidpi-1920-calculator-result.png")
        assert "calc: 7+2 = 9" in out, "the clicks should press 7, +, 2 and =:\n" + out[-600:]


@test
def hidpi_forced_scale1_and_too_small():
    """ui.scale=1 keeps a 2560x1440 screen at 1x (the panel at the very bottom, the font at its 1x size); ui.scale=2 on 1280x800
    would leave 640x400 and is lowered to 1."""
    with Qemu(cmdline="text ui.scale=1", name="t-hidpi-force1", mem="768M", video="2560x1440") as vm:
        g, s, why = start_desktop(vm, 1, 1440)
        assert (s, why) == (1, "ui.scale"), (s, why)
        check_label(arr(vm), 1, 1440)
    with Qemu(cmdline="text ui.scale=2", name="t-hidpi-small", mem="768M", video="1280x800") as vm:
        g, s, why = start_desktop(vm, 1, 800)
        assert (s, why) == (1, "ui.scale"), (s, why)
        check_label(arr(vm), 1, 800)
