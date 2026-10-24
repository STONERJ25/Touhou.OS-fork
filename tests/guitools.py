"""Helpers for GUI tests: boot to the text console, start the desktop by hand, run shell lines over the serial port,
position the pointer exactly, click, drag, and look at pixels.

    from guitools import Gui

    with Qemu(cmdline="text", name="t-myapp", mem="768M") as vm:
        g = Gui(vm)
        g.start_desktop()                    # fairyd + hakurei-shell, waits until the panel is painted
        g.sh("mkdir /tmp/x; echo hi > /tmp/x/a.txt")
        g.sh("myapp /tmp/x &")               # the first normal window opens with its content at (44, 56)
        g.sleep(2)
        g.click(120, 90)
        g.shot("after-click")                # build/<name>-after-click.png
        assert g.pixel_near(100, 100, (0xEC, 0xE6, 0xD6))

The pointer only accepts small relative steps (the PS/2 mouse drops packets with a delta over 255), so move_to() homes
the pointer in the top-left corner first and then walks to the target in steps of at most 90 pixels. The pointer is
exact afterwards as long as every move goes through this class.
"""
import time

from harness import Qemu, BUILD, scaled  # noqa: F401  (re-exported for the test modules)

PROMPT = r"reimu@gensokyo \S+ \$ "
START_BUTTON = (118, 756)         # the start button of the panel; shrine red once the panel is up
SHRINE_RED = (0xB5, 0x12, 0x2B)
PAPER = (0xEC, 0xE6, 0xD6)        # window background (KC_PAPER)
FIRST_WINDOW_CONTENT = (44, 56)   # content origin of the first decorated window (cascade position 40,30 + frame)


def near(p, q, tol=40):
    return all(abs(a - b) <= tol for a, b in zip(p, q))


class Gui:
    def __init__(self, vm, prefix=None):
        self.vm = vm
        self.prefix = prefix or vm.name
        self.px = self.py = None          # where we believe the pointer is
        self._marker = 0
        self.buttons = set()

    # ------------------------------------------------------------ boot and shell
    def wait_prompt(self, timeout=90):
        self.vm.expect(PROMPT, timeout=timeout)

    def start_desktop(self, timeout=40):
        """Boots with cmdline 'text' must already be at the prompt; starts fairyd and hakurei-shell."""
        self.wait_prompt()
        self.vm.send("fairyd &\n")
        time.sleep(1.5)
        self.vm.send("hakurei-shell &\n")
        deadline = time.time() + scaled(timeout)
        while time.time() < deadline:
            if near(self.vm.pixel(*START_BUTTON), SHRINE_RED, 60):
                time.sleep(0.5)
                self.px = self.py = None
                return
            time.sleep(0.5)
        raise TimeoutError("the panel never appeared; serial tail:\n" + self.vm.text()[-600:])

    def sh(self, line, timeout=30):
        """Runs one shell line on the console and returns what it printed. The line editor's echo (prompt redraws) is
        filtered out; the begin/end markers use arithmetic so that only real output matches them, not the typed echo."""
        self._marker += 1
        n = 7000 + self._marker
        self.vm.send(f"echo ==B$(({n}))==\n{line}\necho ==E$(({n}))==\n")
        self.vm.expect(rf"==E{n}==", timeout=timeout)
        text = self.vm.text()
        b = text.rfind(f"==B{n}==")
        e = text.rfind(f"==E{n}==")
        body = text[b + len(f"==B{n}==") if b >= 0 else 0:e]
        return "\n".join(l for l in body.split("\n") if "reimu@gensokyo" not in l)

    def sleep(self, s):
        time.sleep(s)

    # ------------------------------------------------------------ pointer
    def home(self):
        for _ in range(12):
            self.vm.mouse_move(-90, -90)
            time.sleep(0.01)
        time.sleep(0.05)
        self.px = self.py = 0

    def move_to(self, x, y):
        if self.px is None:
            self.home()
        while (self.px, self.py) != (x, y):
            dx = max(-90, min(90, x - self.px))
            dy = max(-90, min(90, y - self.py))
            self.vm.mouse_move(dx, dy)
            self.px += dx
            self.py += dy
            time.sleep(0.012)
        time.sleep(0.06)

    def click(self, x=None, y=None, button="left", count=1):
        if x is not None:
            self.move_to(x, y)
        for i in range(count):
            self.vm.mouse_button(button, True)
            time.sleep(0.05)
            self.vm.mouse_button(button, False)
            time.sleep(0.05 if i + 1 < count else 0.12)

    def double_click(self, x=None, y=None):
        self.click(x, y, count=2)

    def right_click(self, x=None, y=None):
        self.click(x, y, button="right")

    def press(self, button="left"):
        self.vm.mouse_button(button, True)
        time.sleep(0.08)

    def release(self, button="left"):
        self.vm.mouse_button(button, False)
        time.sleep(0.12)

    def drag(self, x0, y0, x1, y1, steps=8, button="left"):
        """Press at (x0, y0), move to (x1, y1) in small steps, release."""
        self.move_to(x0, y0)
        self.press(button)
        for i in range(1, steps + 1):
            self.move_to(x0 + (x1 - x0) * i // steps, y0 + (y1 - y0) * i // steps)
        self.release(button)

    # ------------------------------------------------------------ keyboard
    def key(self, qcode, ctrl=False, alt=False, shift=False):
        self.vm.key(qcode, ctrl=ctrl, alt=alt, shift=shift, hold=0.04)
        time.sleep(0.08)

    def type(self, text):
        self.vm.type(text, delay=0.03)
        time.sleep(0.1)

    # ------------------------------------------------------------ pixels
    def screen(self):
        return self.vm.screen_ppm()

    def pixel(self, x, y, shot=None):
        return self.vm.pixel(x, y, shot)

    def pixel_near(self, x, y, rgb, tol=24, shot=None):
        return near(self.vm.pixel(x, y, shot), rgb, tol)

    def count_pixels(self, rect, rgb, tol=24, shot=None, step=1):
        """How many pixels inside rect = (x, y, w, h) are within tol of rgb."""
        shot = shot or self.vm.screen_ppm()
        x0, y0, w, h = rect
        n = 0
        for yy in range(y0, y0 + h, step):
            for xx in range(x0, x0 + w, step):
                if near(self.vm.pixel(xx, yy, shot), rgb, tol):
                    n += 1
        return n

    def wait_pixel(self, x, y, rgb, tol=24, timeout=10):
        deadline = time.time() + scaled(timeout)
        while time.time() < deadline:
            if self.pixel_near(x, y, rgb, tol):
                return True
            time.sleep(0.25)
        return False

    def shot(self, name):
        return self.vm.screenshot(BUILD / f"{self.prefix}-{name}.png")
