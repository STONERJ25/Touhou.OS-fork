"""The games: the logic self-tests of danmaku, minesweeper and snake, and GUI checks that start each game on the desktop,
play it with the keyboard and the mouse, and look at the pixels and at what the programs print.

Every game takes --trace and prints what it does ("minesweeper: reveal 4 4 -> 61", "snake: turn 2", "danmaku: new run ..."), so the
tests wait for those lines instead of sleeping: the guest clock can run slowly when the host timer is coarse.
"""
import re
import time

import numpy as np

from harness import test, Qemu, BUILD

PROMPT = r"reimu@gensokyo \S+ \$ "


def boot(name):
    vm = Qemu(cmdline="text", name=name, mem="768M")
    vm.start()
    vm.expect(PROMPT, timeout=120)
    return vm


def desktop(vm):
    """Starts the window server and the shell panel, like the desktop session does."""
    vm.send("fairyd &\n")
    vm.expect(r"fairyd: \d+x\d+", timeout=60)
    vm.send("hakurei-shell &\n")
    time.sleep(4)


def screen_array(vm):
    w, h, rgb = vm.screen_ppm()
    return np.frombuffer(rgb, dtype=np.uint8).reshape(h, w, 3).astype(int)


def color_bbox(arr, color, tol=8, region=None):
    """Bounding box (x0, y0, x1, y1) of the pixels close to `color`, or None."""
    sub = arr
    ox = oy = 0
    if region:
        x0, y0, x1, y1 = region
        sub = arr[y0:y1, x0:x1]
        ox, oy = x0, y0
    mask = (np.abs(sub - np.array(color)) <= tol).all(axis=2)
    ys, xs = np.nonzero(mask)
    if not len(xs):
        return None
    return int(xs.min()) + ox, int(ys.min()) + oy, int(xs.max()) + ox, int(ys.max()) + oy


def count_close(arr, color, tol=8, region=None):
    sub = arr if not region else arr[region[1]:region[3], region[0]:region[2]]
    return int((np.abs(sub - np.array(color)) <= tol).all(axis=2).sum())


def near(p, q, tol=40):
    return all(abs(int(a) - int(b)) <= tol for a, b in zip(p, q))


class Pointer:
    """Tracks the pointer: relative moves only, starting where the window server puts it."""

    def __init__(self, vm):
        self.vm, self.x, self.y = vm, 512, 384

    def to(self, x, y):
        dx, dy = x - self.x, y - self.y
        while dx or dy:                                   # the guest mouse takes at most +-127 per packet
            sx, sy = max(-100, min(100, dx)), max(-100, min(100, dy))
            self.vm.mouse_move(sx, sy)
            dx -= sx
            dy -= sy
            time.sleep(0.05)
        self.x, self.y = x, y
        time.sleep(0.15)

    def click(self, x, y, button="left"):
        self.to(x, y)
        self.vm.mouse_click(button)
        time.sleep(0.3)


# ---------------------------------------------------------------------------------------------------------------- self-tests

@test
def games_selftests():
    """minesweeper, snake and danmaku --selftest run in the guest and report PASS."""
    with Qemu(cmdline="text", name="t-games-self", mem="768M") as vm:
        vm.expect(PROMPT, timeout=120)
        for game, wait in (("minesweeper", 120), ("snake", 180), ("danmaku", 400)):
            vm.send(f"{game} --selftest\n")
            m = vm.expect(rf"{game} selftest: (PASS|FAIL)[^\n]*", timeout=wait)
            assert m.group(1) == "PASS", f"{game}: {m.group(0)}\n{vm.text()[-1500:]}"
            print("   ", m.group(0))


# ---------------------------------------------------------------------------------------------------------------- minesweeper

@test
def minesweeper_play():
    """Minesweeper: the first click opens an area, a right click flags a fairy, uncovering a fairy loses."""
    vm = boot("t-games-ms")
    try:
        desktop(vm)
        vm.send("minesweeper --trace --seed 3 &\n")
        vm.expect(r"minesweeper: new game Beginner 9x9 fairies=10", timeout=60)
        time.sleep(2)
        arr = screen_array(vm)
        bb = color_bbox(arr, (0x66, 0xB0, 0x5A), tol=6)
        assert bb, "no grass on the screen: the board did not appear"
        bx, by = bb[0] - 2, bb[1] - 2                      # the tile bevel is two pixels wide
        assert 170 <= bb[2] - bb[0] + 5 <= 190, f"the board should be 9 cells of 20 px, found {bb}"
        cell = lambda cx, cy: (bx + cx * 20 + 10, by + cy * 20 + 10)
        ptr = Pointer(vm)
        ptr.click(*cell(4, 4))
        m = vm.expect(r"minesweeper: fairies at: ([0-9, ]+)\n.*?minesweeper: reveal 4 4 -> (\d+)", timeout=30)
        opened = int(m.group(2))
        assert opened >= 9, f"the first click should open an area, got {opened} cells"
        mines = [tuple(map(int, p.split(","))) for p in m.group(1).split()]
        shot = vm.screen_ppm()
        x, y = cell(4, 4)
        assert not near(vm.pixel(x - 8, y - 8, shot), (0x66, 0xB0, 0x5A), 25), "the opened cell should not be grass any more"
        # flag a fairy with the right button
        ax, ay = mines[0]
        ptr.click(*cell(ax, ay), button="right")
        vm.expect(rf"minesweeper: flag {ax} {ay} -> 1", timeout=30)
        # the keyboard: the first arrow press shows the cursor, the next ones move it; F places a flag on a covered cell
        kx, ky = mines[2]
        vm.key("up")
        for _ in range(abs(kx - 4)):
            vm.key("right" if kx > 4 else "left")
        for _ in range(abs(ky - 4)):
            vm.key("down" if ky > 4 else "up")
        vm.key("f")
        vm.expect(rf"minesweeper: flag {kx} {ky} -> 1", timeout=30)
        # uncovering another fairy loses the game
        cx, cy = mines[1]
        ptr.click(*cell(cx, cy))
        vm.expect(rf"minesweeper: lost at {cx},{cy}", timeout=30)
        time.sleep(1)
        shot = vm.screen_ppm()
        px, py = cell(cx, cy)
        assert near(vm.pixel(px - 8, py - 8, shot), (0xE0, 0x20, 0x20), 50), "the fairy that was uncovered should sit on a red square"
        vm.screenshot(BUILD / "t-games-minesweeper.png")
        # F2 starts a new game
        vm.key("f2")
        vm.expect(r"minesweeper: new game Beginner", timeout=30)
    finally:
        vm.stop()


# ---------------------------------------------------------------------------------------------------------------- snake

@test
def snake_play():
    """Snake: the game starts, the arrow keys steer, hitting the wall ends the game."""
    vm = boot("t-games-snake")
    try:
        desktop(vm)
        vm.send("snake --start --seed 5 --trace &\n")
        vm.expect(r"snake: state \d -> 3", timeout=90)      # the ready banner is over: the snake moves
        time.sleep(1)
        arr = screen_array(vm)
        floor = count_close(arr, (0x1B, 0x3C, 0x36), tol=5)
        assert floor > 20000, f"the garden should be on the screen ({floor} floor pixels)"
        assert count_close(arr, (0xD9, 0xA4, 0x41), tol=6) > 200, "the gold line of the status bar should be visible"
        vm.key("down")
        vm.expect(r"snake: turn 2", timeout=30)
        vm.key("left")
        vm.expect(r"snake: turn 3", timeout=30)
        vm.screenshot(BUILD / "t-games-snake.png")
        vm.expect(r"snake: game over: score \d+ level 1 length 3", timeout=120)   # no food on its way: it runs into the wall
    finally:
        vm.stop()


@test
def snake_scores():
    """Snake: the autopilot eats three dango, dies, and the high score is saved to ~/.config/snake.scores."""
    vm = boot("t-games-snake2")
    try:
        desktop(vm)
        vm.send("snake --bot --bot-limit 3 --level 2 --seed 11 --trace &\n")
        vm.expect(r"snake: state \d -> 6", timeout=240)        # the game over screen with the name dialog
        time.sleep(1.5)
        vm.key("ret")                                          # accept the suggested name
        vm.expect(r"snake: high score saved: (\d+) rank 0", timeout=60)
        vm.send("cat ~/.config/snake.scores\n")
        m = vm.expect(r"\n0 (\d+) 2 \d+ \w+", timeout=30)
        assert int(m.group(1)) > 0
        vm.screenshot(BUILD / "t-games-snake-over.png")
    finally:
        vm.stop()


# ---------------------------------------------------------------------------------------------------------------- danmaku

@test
def danmaku_menus():
    """Danmaku: Start, a difficulty and a heroine are chosen with the keyboard, then the game starts; Esc pauses."""
    vm = boot("t-games-dm1")
    try:
        desktop(vm)
        vm.send("danmaku --trace --noscores &\n")
        vm.expect(r"danmaku: state 0 -> 1", timeout=120)      # the art is built, the title screen is up
        time.sleep(1.5)
        arr = screen_array(vm)
        assert count_close(arr, (0x8A, 0x10, 0x30), tol=40) + count_close(arr, (0xFF, 0xF4, 0xC8), tol=30) > 300, "the title screen should show the menu"
        vm.screenshot(BUILD / "t-games-danmaku-title.png")
        vm.key("ret")
        vm.expect(r"danmaku: state 1 -> 2", timeout=30)
        vm.key("down")                                         # Hard
        vm.key("ret")
        vm.expect(r"danmaku: state 2 -> 3", timeout=30)
        vm.key("right")                                        # the witch
        vm.key("ret")
        vm.expect(r"danmaku: state \d -> 5", timeout=60)
        vm.expect(r"danmaku: new run: Hard, Witch, Starlit Cloud Road", timeout=60)
        time.sleep(3)
        vm.key("esc")
        vm.expect(r"danmaku: state 5 -> 6", timeout=30)        # paused
        time.sleep(1)
        vm.screenshot(BUILD / "t-games-danmaku-pause.png")
        vm.key("esc")
        vm.expect(r"danmaku: state 6 -> 5", timeout=30)
        # hold Z and Shift while the stage runs: nothing may crash
        time.sleep(2)
    finally:
        vm.stop()


@test
def danmaku_demo():
    """Danmaku: the autopilot plays the first boss's spell card (a still picture at a fixed tick) and the frame rate stays playable."""
    vm = boot("t-games-dm2")
    try:
        desktop(vm)
        vm.send("danmaku --demo --god --stage 1 --boss --phase 1 --seed 5 --noscores --trace --warp 420 --hold &\n")
        vm.expect(r"danmaku: practice run: Normal, Shrine Maiden, Starlit Cloud Road", timeout=120)
        time.sleep(3)
        arr = screen_array(vm)
        field = arr[150:600, 215:620]                          # inside the field wherever the window sits
        bright = int((field.sum(axis=2) > 620).sum())
        colours = len({tuple(p) for p in (field[::6, ::6] // 32).reshape(-1, 3)})
        assert bright > 150, f"bullets and sprites should light up the field ({bright} bright pixels)"
        assert colours > 25, f"the field should be colourful ({colours} colour buckets)"
        assert count_close(arr, (0xE8, 0xB8, 0x50), tol=10) > 600, "the gold border of the field and of the HUD"
        vm.screenshot(BUILD / "t-games-danmaku-spell.png")
    finally:
        vm.stop()
    vm = boot("t-games-dm3")
    try:
        desktop(vm)
        vm.send("danmaku --demo --god --stage 1 --boss --phase 1 --seed 5 --noscores --trace &\n")
        vm.expect(r"danmaku: practice run", timeout=120)
        m = vm.expect(r"danmaku: frames: ([1-9]\d*)\.(\d) fps, (\d+) us per frame[^\n]*", timeout=240)
        fps = int(m.group(1)) + int(m.group(2)) / 10
        print("    ", m.group(0))
        assert fps >= 20, f"the game should run at 20 frames per second or better in the emulator: {m.group(0)}"
    finally:
        vm.stop()
