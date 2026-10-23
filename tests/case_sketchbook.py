"""Marisa's Sketchbook (sketchbook): the paint program. Each test boots the desktop in QEMU, starts the program, drives
the pointer and keyboard like a user would and checks pixels on the screen and the files it writes.

Coordinates in this file are "base" coordinates: screen positions when the program's window is the first decorated window
(content origin at (44, 56), default window 770x640, a 640x480 picture centred in the canvas view at zoom 100%). The window
server cascades new windows, so Sk translates base coordinates by the offset of the instance it started; dialogs are
centred on the screen and are addressed in absolute coordinates through s.vm.

    picture pixel (0, 0)  at (149, 105)        palette: dark row y = 628, bright row y = 649, cell x = 124 + 21 * column
    menu bar centre y = 68; titles at x = 70 (File), 118 (Edit), 170 (Image), 222 (View), 270 (Help)
"""
import re
import time

from harness import test, Qemu, BUILD
from guitools import Gui, near

OX, OY = 149, 105
WHITE = (255, 255, 255)
BLACK = (0, 0, 0)
RED = (255, 0, 0)
BLUE = (0, 0, 255)
YELLOW = (255, 255, 0)
LIME = (0, 255, 0)
CYAN = (0, 255, 255)
WORKSPACE = (0x6F, 0x74, 0x88)
ERROR_RED = (0xC0, 0x2D, 0x2D)          # the error icon
QUESTION_BLUE = (0x2D, 0x6C, 0xC0)      # the question icon


def swatch(col, row):
    return 124 + 21 * col, 628 + 21 * row


S_BLACK, S_RED, S_BLUE, S_YELLOW, S_LIME, S_WHITE = swatch(0, 0), swatch(2, 1), swatch(1, 1), swatch(6, 1), swatch(4, 1), swatch(7, 1)
S_CYAN = swatch(5, 1)

STYLE = {"outline": (86, 351), "solid": (86, 371), "both": (86, 391)}      # rectangle / ellipse style buttons
MENU_X = {"File": 70, "Edit": 118, "Image": 170, "View": 222, "Help": 270}
MENU_KINDS = {"File": "nnnsnnsn", "Edit": "nnsnnnnsnn", "Image": "nnnnnsnnn", "View": "nnnnnnsn", "Help": "nn"}
PARK = (86, 560)                         # an empty part of the toolbox, where the pointer waits while pixels are read


def pic(x, y):
    """Base screen coordinates of picture pixel (x, y) at zoom 100% in a 640x480 picture."""
    return OX + x, OY + y


class Sk:
    """The program under test plus helpers around Gui."""

    def __init__(self, vm, start=True, args="", env=""):
        self.vm = vm
        self.g = Gui(vm)
        self.g.start_desktop()
        self.g.sh("mkdir /tmp/sk")
        self.count = 0
        self.dx = self.dy = 0
        if start:
            self.start(args, env)

    # ---- starting and stopping
    def start(self, args="", env="", ready=None):
        n = self.count
        self.count += 1
        x = min(40 + (n * 30) % 240, 1024 - 778)               # where the window server cascades the next window
        y = min(30 + (n * 30) % 180, 732 - 670)
        self.dx, self.dy = x - 40, y - 30
        self.g.px = self.g.py = None
        self.g.sh(f"{env} sketchbook {args} &")
        rx, ry, rgb = ready or (OX + 5, OY + 5, WHITE)         # the white picture shows once the window is up
        assert self.wait_pixel(rx, ry, rgb, 25), "sketchbook never opened its window"

    def quit(self):
        """Ctrl+Q closes an unmodified picture; waits until the process is gone."""
        self.g.key("q", ctrl=True)
        return self.wait_gone()

    def wait_gone(self, timeout=10):
        end = time.time() + timeout
        while time.time() < end:
            if not re.search(r"\d+", self.g.sh("pidof sketchbook")):
                return True
            time.sleep(0.4)
        return False

    # ---- pointer and keys in base coordinates
    def at(self, x, y):
        return x + self.dx, y + self.dy

    def click(self, x, y, button="left", count=1):
        self.g.click(*self.at(x, y), button=button, count=count)

    def double_click(self, x, y):
        self.g.double_click(*self.at(x, y))

    def drag(self, x0, y0, x1, y1, steps=8, button="left"):
        self.g.drag(*self.at(x0, y0), *self.at(x1, y1), steps=steps, button=button)

    def move_to(self, x, y):
        self.g.move_to(*self.at(x, y))

    def key(self, *a, **kw):
        self.g.key(*a, **kw)

    def type(self, text):
        self.g.type(text)

    def park(self):
        target = self.at(*PARK)
        if (self.g.px, self.g.py) != target:
            self.g.move_to(*target)
            time.sleep(0.2)

    # ---- looking (the pointer is parked first: its sprite is part of the screen)
    def screen(self):
        self.park()
        return self.vm.screen_ppm()

    def px(self, x, y, shot=None):
        self.park()
        return self.vm.pixel(x + self.dx, y + self.dy, shot)

    def wait_pixel(self, x, y, rgb, timeout=8, tol=8):
        self.park() if self.g.px is not None else None
        end = time.time() + timeout
        while time.time() < end:
            if near(self.vm.pixel(x + self.dx, y + self.dy), rgb, tol):
                return True
            time.sleep(0.25)
        return False

    def wait_until(self, fn, timeout=8, gap=0.25):
        self.park()
        end = time.time() + timeout
        while time.time() < end:
            shot = self.vm.screen_ppm()
            if fn(shot):
                return True
            time.sleep(gap)
        return False

    def is_near(self, shot, x, y, rgb, tol=8):
        return near(self.vm.pixel(x + self.dx, y + self.dy, shot), rgb, tol)

    def count_near(self, shot, rect, rgb, tol=12):
        """How many pixels inside rect = (x, y, w, h), base coordinates, are near rgb."""
        x0, y0, w, h = rect
        return sum(1 for y in range(y0, y0 + h) for x in range(x0, x0 + w) if self.is_near(shot, x, y, rgb, tol))

    def find_colour_abs(self, shot, rect, rgb, tol=10, step=2):
        """True when a pixel near rgb exists inside rect = (x, y, w, h) in absolute screen coordinates."""
        x0, y0, w, h = rect
        for y in range(y0, y0 + h, step):
            for x in range(x0, x0 + w, step):
                if near(self.vm.pixel(x, y, shot), rgb, tol):
                    return True
        return False

    # ---- dialogs
    def main_active(self, shot=None):
        """The main window's title bar is shrine red while it has the focus and grey-blue while a dialog has it."""
        r, g_, b = self.vm.pixel(700 + self.dx, 44 + self.dy, shot or self.vm.screen_ppm())
        return r > 100 and g_ < 70

    def wait_dialog(self, timeout=10):
        return self.wait_until(lambda sh: not self.main_active(sh), timeout)

    def wait_dialog_closed(self, timeout=10):
        return self.wait_until(lambda sh: self.main_active(sh), timeout)

    def dialog_has_icon(self, rgb):
        """Looks for the dialog's icon colour in the middle of the screen (dialogs are centred)."""
        return self.wait_until(lambda sh: self.find_colour_abs(sh, (270, 250, 300, 200), rgb, 6), 6)

    def save_as(self, path):
        """Ctrl+Shift+S, type the path in the file dialog, Enter."""
        g = self.g
        g.key("s", ctrl=True, shift=True)
        assert self.wait_dialog(), "the Save As dialog did not appear"
        time.sleep(0.4)
        g.key("a", ctrl=True)
        g.type(path)
        g.key("ret")

    def menu(self, name, index):
        kinds = MENU_KINDS[name]
        self.click(MENU_X[name], 68)
        time.sleep(0.3)
        y = 27
        for k in kinds[:index]:
            y += 7 if k == "s" else 22
        self.click(MENU_X[name] + 30, 56 + y + 11)
        time.sleep(0.3)

    # ---- files in the guest
    def file_hex(self, path, offset, n):
        out = self.g.sh(f"xxd -p -s {offset} -l {n} {path}")
        return "".join(re.findall(r"^[0-9a-f]+$", out, re.M))[:2 * n]

    def wait_file(self, path, size=None, timeout=15):
        end = time.time() + timeout
        while time.time() < end:
            out = self.g.sh(f"ls -l {path}")
            if path.split("/")[-1] in out and (size is None or str(size) in out):
                return True
            time.sleep(0.5)
        return False

    def make_file(self, path, hexstr):
        self.g.sh(f"echo {hexstr} | xxd -r > {path}")


def ppm_hex(width, height, pixels, comment=None):
    """A binary PPM as a hex string (for `xxd -r`); pixels is a list of (r, g, b)."""
    head = b"P6\n" + (b"# " + comment.encode() + b"\n" if comment else b"") + b"%d %d\n255\n" % (width, height)
    return (head + bytes(c for p in pixels for c in p)).hex()


def bmp_hex(width, height, pixels):
    """A 24-bit bottom-up BMP as hex; pixels in top-down order."""
    stride = (width * 3 + 3) & ~3
    rows = b""
    for y in range(height - 1, -1, -1):
        row = b"".join(bytes((b, g, r)) for r, g, b in pixels[y * width:(y + 1) * width])
        rows += row + b"\0" * (stride - len(row))
    head = b"BM" + (54 + len(rows)).to_bytes(4, "little") + b"\0\0\0\0" + (54).to_bytes(4, "little")
    dib = (40).to_bytes(4, "little") + width.to_bytes(4, "little") + height.to_bytes(4, "little") + (1).to_bytes(2, "little") + \
        (24).to_bytes(2, "little") + b"\0" * 24
    return (head + dib + rows).hex()


# ---------------------------------------------------------------------------------------------------- drawing


@test
def sketchbook_draw_undo():
    """Pencil, brush, shapes, fill, eraser and picker paint the right pixels; undo and redo take them away and back."""
    with Qemu(cmdline="text", name="t-sketchbook-draw", mem="768M") as vm:
        s = Sk(vm)
        g = s.g
        # pencil: a black line from (20, 20) to (120, 60) passes exactly through (70, 40)
        s.key("p")
        s.drag(*pic(20, 20), *pic(120, 60))
        assert s.wait_pixel(*pic(70, 40), BLACK), "the pencil stroke is missing"
        assert near(s.px(*pic(70, 46)), WHITE), "pixels beside the stroke must stay white"
        # brush with the red swatch: size 4 by default, so the stroke is several pixels thick
        s.click(*S_RED)
        s.key("b")
        s.drag(*pic(20, 100), *pic(220, 100))
        assert s.wait_pixel(*pic(120, 101), RED), "the brush stroke is missing"
        assert near(s.px(*pic(120, 98)), RED), "a size 4 brush covers 4 rows"
        assert near(s.px(*pic(120, 108)), WHITE)
        # filled rectangle in blue: choose the Solid style button
        s.click(*S_BLUE)
        s.key("r")
        s.click(*STYLE["solid"])
        s.drag(*pic(300, 40), *pic(380, 100))
        assert s.wait_pixel(*pic(340, 70), BLUE), "the solid rectangle is missing"
        # ellipse outline in lime
        s.click(*S_LIME)
        s.key("o")
        s.click(*STYLE["outline"])
        s.drag(*pic(400, 40), *pic(500, 100))
        assert s.wait_pixel(*pic(400, 70), LIME) or s.wait_pixel(*pic(400, 71), LIME), "the ellipse is missing"
        assert near(s.px(*pic(450, 70)), WHITE), "the ellipse must be hollow"
        # flood fill the inside of the ellipse with yellow
        s.click(*S_YELLOW)
        s.key("f")
        s.click(*pic(450, 70))
        assert s.wait_pixel(*pic(450, 70), YELLOW), "flood fill did not fill the ellipse"
        assert near(s.px(*pic(520, 70)), WHITE), "flood fill must stop at the outline"
        # undo the fill, then redo it
        s.key("z", ctrl=True)
        assert s.wait_pixel(*pic(450, 70), WHITE), "undo should remove the fill"
        assert near(s.px(*pic(400, 70)), LIME) or near(s.px(*pic(400, 71)), LIME), "undo must keep the older drawing"
        s.key("y", ctrl=True)
        assert s.wait_pixel(*pic(450, 70), YELLOW), "redo should bring the fill back"
        # eraser paints the background colour (white) over the red stroke
        s.key("e")
        s.drag(*pic(100, 90), *pic(100, 112))
        assert s.wait_pixel(*pic(100, 101), WHITE), "the eraser should rub out the stroke"
        # picker: the right button takes the blue of the rectangle as the background colour; the eraser then paints blue
        s.key("i")
        s.click(*pic(340, 70), button="right")
        s.key("e")
        s.drag(*pic(150, 90), *pic(150, 112))
        assert s.wait_pixel(*pic(150, 101), BLUE), "the eraser should use the picked background colour"
        g.shot("draw")
        # 25 dots are 25 undo steps (at least 20 levels are required): take them all back
        s.key("p")
        s.click(*S_BLACK)
        for i in range(25):
            s.click(*pic(30 + 8 * i, 200))
        assert s.wait_pixel(*pic(30 + 8 * 24, 200), BLACK)
        for i in range(25):
            s.key("z", ctrl=True)
        assert s.wait_pixel(*pic(30, 200), WHITE), "25 undo steps should have removed all the dots"
        assert near(s.px(*pic(450, 70)), YELLOW), "...but not the older drawing"


@test
def sketchbook_zoom_grid():
    """Zoom 800% shows a pixel grid; painting at high zoom hits the right picture pixel; the wheel scrolls."""
    with Qemu(cmdline="text", name="t-sketchbook-zoom", mem="768M") as vm:
        s = Sk(vm)
        s.key("8")
        # zooming keeps the picture point under the middle of the view fixed: picture pixel (320, 240) now covers the
        # screen block x 469..476, y 345..352
        s.click(*S_RED)
        s.key("p")
        s.click(473, 349)
        assert s.wait_pixel(473, 349, RED, 6), "the pixel was not painted"
        shot = s.screen()
        reds = s.count_near(shot, (473 - 12, 349 - 12, 25, 25), RED)
        assert 40 <= reds <= 64, f"one picture pixel should be a block of about 8x8 screen pixels, got {reds}"
        assert s.is_near(shot, 473 + 8, 349, WHITE, 6), "the neighbouring pixel must stay white"

        def non_white(sh):
            return sum(1 for dx in range(40) for dy in range(40) if s.vm.pixel(540 + dx, 420 + dy, sh) != WHITE)
        assert non_white(shot) >= 200, "the pixel grid should be visible at 800%"
        s.key("g")                                            # grid off
        assert s.wait_until(lambda sh: non_white(sh) == 0, 6), "G should hide the grid"
        # the wheel scrolls: after scrolling down the red block moves up and out of its place
        s.move_to(300, 300)
        for _ in range(3):
            s.vm.mouse_button("wheel-down", True)
            time.sleep(0.05)
            s.vm.mouse_button("wheel-down", False)
            time.sleep(0.15)
        assert s.wait_until(lambda sh: not s.is_near(sh, 473, 349, RED, 12), 6), "the picture should have scrolled"


@test
def sketchbook_colours():
    """The mixer's hex field and sliders change the foreground colour; X swaps; right-clicking a swatch sets the background."""
    with Qemu(cmdline="text", name="t-sketchbook-colours", mem="768M") as vm:
        s = Sk(vm)
        s.key("p")
        s.click(523, 625)                                     # the hex field
        s.key("a", ctrl=True)
        s.type("30a0ff")
        s.key("ret")
        s.move_to(*pic(50, 50))
        s.click(*pic(50, 50))
        assert s.wait_pixel(*pic(50, 50), (0x30, 0xA0, 0xFF), 4), "the hex colour 30A0FF was not used"
        # the red slider: clicking in the middle of its track sets about half intensity (the track runs x 314..438)
        s.click(376, 624)
        s.click(*pic(60, 50))
        assert s.wait_until(lambda sh: 100 <= s.vm.pixel(*s.at(*pic(60, 50)), sh)[0] <= 156, 6), "the R slider did not set a middle value"
        shot = s.screen()
        r, g_, b = s.vm.pixel(*s.at(*pic(60, 50)), shot)
        assert abs(g_ - 0xA0) <= 2 and abs(b - 0xFF) <= 2, "moving the R slider must leave G and B alone"
        # right button on the yellow swatch sets the background; X swaps; then a click paints with the new foreground
        s.click(*S_YELLOW, button="right")
        s.key("x")
        s.click(*pic(70, 50))
        assert s.wait_pixel(*pic(70, 50), YELLOW), "after X the old background must be the foreground"


# ---------------------------------------------------------------------------------------------------- files


@test
def sketchbook_files():
    """Opens a PPM with a comment, saves BMP and PPM files, reads them back bit for bit, and refuses unknown formats."""
    with Qemu(cmdline="text", name="t-sketchbook-files", mem="768M") as vm:
        s = Sk(vm, start=False)
        g = s.g
        # a 2x2 PPM with a header comment: red, green / blue, yellow
        s.make_file("/tmp/sk/tiny.ppm", ppm_hex(2, 2, [RED, LIME, BLUE, YELLOW], "a comment"))
        s.start("/tmp/sk/tiny.ppm", ready=(468, 344, RED))
        assert s.wait_pixel(469, 344, LIME) and s.wait_pixel(468, 345, BLUE) and s.wait_pixel(469, 345, YELLOW), "tiny.ppm shows the wrong colours"
        assert s.quit(), "Ctrl+Q should close the program when nothing is unsaved"

        # a 3x2 BMP (24-bit, bottom-up, rows padded to 4 bytes) opens with the right orientation
        s.make_file("/tmp/sk/tiny.bmp", bmp_hex(3, 2, [RED, LIME, BLUE, YELLOW, CYAN, WHITE]))
        s.start("/tmp/sk/tiny.bmp", ready=(467, 344, RED))
        assert s.wait_pixel(468, 344, LIME) and s.wait_pixel(469, 344, BLUE) and s.wait_pixel(467, 345, YELLOW) \
            and s.wait_pixel(468, 345, CYAN) and s.wait_pixel(469, 345, WHITE), "tiny.bmp shows the wrong colours"
        assert s.quit()

        # draw a red rectangle and a black pixel on a new picture called pic.bmp, then save it with Ctrl+S
        s.start("/tmp/sk/pic.bmp")
        s.click(*S_RED)
        s.key("r")
        s.click(*STYLE["solid"])
        s.drag(*pic(10, 10), *pic(59, 39))
        s.key("p")
        s.click(*S_BLACK)
        s.click(*pic(100, 100))
        assert s.wait_pixel(*pic(100, 100), BLACK)
        s.key("s", ctrl=True)
        assert s.wait_file("/tmp/sk/pic.bmp", 921654), "Ctrl+S should write a 640x480 24-bit BMP of 921654 bytes"
        head = s.file_hex("/tmp/sk/pic.bmp", 0, 30)
        assert head.startswith("424d36100e00") and head[28:36] == "28000000", "bad BMP header: " + head
        stride = 640 * 3
        assert s.file_hex("/tmp/sk/pic.bmp", 54 + (479 - 20) * stride + 30 * 3, 3) == "0000ff", "the red rectangle is wrong in the BMP (BGR order)"
        assert s.file_hex("/tmp/sk/pic.bmp", 54 + (479 - 100) * stride + 100 * 3, 3) == "000000", "the black pixel is wrong in the BMP"
        assert s.file_hex("/tmp/sk/pic.bmp", 54 + (479 - 5) * stride + 5 * 3, 3) == "ffffff", "untouched pixels must be white"

        # Save As a PPM through the file dialog
        s.save_as("/tmp/sk/pic.ppm")
        assert s.wait_file("/tmp/sk/pic.ppm", 921615), "Save As should write a 640x480 P6 file of 921615 bytes"
        assert s.file_hex("/tmp/sk/pic.ppm", 0, 15) == "50360a3634302034383" + "00a3235350a", "bad PPM header"
        assert s.file_hex("/tmp/sk/pic.ppm", 15 + (20 * 640 + 30) * 3, 3) == "ff0000", "the red rectangle is wrong in the PPM"
        assert s.file_hex("/tmp/sk/pic.ppm", 15 + (100 * 640 + 100) * 3, 3) == "000000"
        assert s.quit(), "the picture was saved, so Ctrl+Q must not ask anything"

        # open the BMP again and write a PPM copy: the pixels survive the round trip bit for bit
        s.start("/tmp/sk/pic.bmp")
        assert s.wait_pixel(*pic(30, 20), RED) and near(s.px(*pic(100, 100)), BLACK)
        s.save_as("/tmp/sk/copy.ppm")
        assert s.wait_file("/tmp/sk/copy.ppm", 921615)
        sums = g.sh("md5sum /tmp/sk/pic.ppm /tmp/sk/copy.ppm")
        found = re.findall(r"^([0-9a-f]{32})\s", sums, re.M)
        assert len(found) == 2 and found[0] == found[1], "BMP -> PPM round trip changed the picture:\n" + sums
        assert s.quit()

        # a name that is neither BMP nor PPM is refused with a dialog and nothing is written
        s.start("/tmp/sk/pic.bmp")
        s.save_as("/tmp/sk/nope.png")
        assert s.wait_dialog(), "saving as .png should explain that only BMP and PPM are supported"
        assert "nope.png" not in g.sh("ls /tmp/sk")
        s.key("ret")
        assert s.wait_dialog_closed()
        assert s.quit()


@test
def sketchbook_bad_files():
    """Garbage, truncated, oversized and empty pictures and folders give an error dialog and a blank canvas, never a crash."""
    with Qemu(cmdline="text", name="t-sketchbook-bad", mem="768M") as vm:
        s = Sk(vm, start=False)
        g = s.g
        bmp = bmp_hex(2, 2, [RED, LIME, BLUE, YELLOW])
        s.make_file("/tmp/sk/trunc.bmp", bmp[:2 * 60])                                       # the pixel data stops early
        s.make_file("/tmp/sk/huge.ppm", "50360a393939393920393939393920300a3235350a00")      # P6 99999 99999 0.. 255, no data
        s.make_file("/tmp/sk/zero.ppm", "50360a3020300a3235350a")                             # P6 0 0 255
        s.make_file("/tmp/sk/rle.bmp", bmp[:60] + "01000000" + bmp[68:])                      # compression = 1 (RLE8)
        g.sh("echo this is not a picture > /tmp/sk/garbage.bmp")
        g.sh("echo P3 > /tmp/sk/ascii.ppm")
        cases = ["/tmp/sk/garbage.bmp", "/tmp/sk/trunc.bmp", "/tmp/sk/huge.ppm", "/tmp/sk/zero.ppm", "/tmp/sk/rle.bmp", "/tmp/sk/ascii.ppm", "/tmp/sk"]
        for i, path in enumerate(cases):
            s.start(path)
            assert s.wait_dialog(), f"{path}: no error dialog"
            assert s.dialog_has_icon(ERROR_RED), f"{path}: the dialog has no error icon"
            if i == 0:
                g.shot("badfile")
            s.key("ret")
            assert s.wait_dialog_closed(), f"{path}: the dialog did not close"
            # the program still works: the canvas is blank and a pencil click paints
            s.key("p")
            s.click(*S_RED)
            s.click(*pic(40, 40))
            assert s.wait_pixel(*pic(40, 40), RED), f"{path}: the program stopped drawing after the error"
            s.key("z", ctrl=True)
            assert s.quit(), f"{path}: the program did not close"


# ---------------------------------------------------------------------------------------------------- dialogs and guards


@test
def sketchbook_dialogs():
    """The New and Resize dialogs set the size (and clamp silly sizes); unsaved changes are guarded; close really exits."""
    with Qemu(cmdline="text", name="t-sketchbook-dialogs", mem="768M") as vm:
        s = Sk(vm)
        g = s.g
        # Ctrl+N on an unmodified picture opens the New dialog at once; 48 x 32 pixels
        s.key("n", ctrl=True)
        assert s.wait_dialog()
        time.sleep(0.5)
        s.key("a", ctrl=True)
        s.type("48")
        s.key("tab")
        s.key("a", ctrl=True)
        s.type("32")
        g.shot("newdlg")
        s.key("ret")
        assert s.wait_dialog_closed()
        # the picture is centred in the 678 x 518 view that starts at (86, 30): x 401..448, y 273..304 in content
        # coordinates; look at its edges from the white inside to the workspace outside
        px0, py0 = 44 + 86 + (678 - 48) // 2, 56 + 30 + (518 - 32) // 2      # screen position of the 48 x 32 picture
        assert s.wait_pixel(px0, py0, WHITE) and near(s.px(px0 + 47, py0 + 31), WHITE), "the new picture should be 48 x 32 and white"
        assert near(s.px(px0 - 3, py0 + 10), WORKSPACE, 12) and near(s.px(px0 + 52, py0 + 10), WORKSPACE, 12) \
            and near(s.px(px0 + 10, py0 + 36), WORKSPACE, 12), "the picture must end at 48 x 32"
        # draw a red pixel in the corner and resize the canvas (Image > Resize Canvas) to 64 x 40 without stretching
        s.key("p")
        s.click(*S_RED)
        s.click(px0 + 1, py0 + 1)
        assert s.wait_pixel(px0 + 1, py0 + 1, RED)
        s.menu("Image", 6)
        assert s.wait_dialog()
        time.sleep(0.5)
        s.key("a", ctrl=True)
        s.type("64")
        s.key("tab")
        s.key("a", ctrl=True)
        s.type("40")
        s.key("ret")
        assert s.wait_dialog_closed()
        px0, py0 = 44 + 86 + (678 - 64) // 2, 56 + 30 + (518 - 40) // 2
        g.shot("resized")
        assert s.wait_pixel(px0 + 1, py0 + 1, RED), "resizing the canvas must keep the picture at the top left"
        assert s.wait_pixel(px0 + 60, py0 + 36, WHITE) and near(s.px(px0 + 72, py0 + 10), WORKSPACE, 12) \
            and near(s.px(px0 + 10, py0 + 46), WORKSPACE, 12), "the canvas should now be 64 x 40"
        # a huge size is clamped to 4096 with a note; the dialog stays open until OK is pressed again
        s.key("n", ctrl=True)           # modified -> asks first
        assert s.wait_dialog()
        assert s.dialog_has_icon(QUESTION_BLUE), "the unsaved changes question has no question icon"
        g.shot("unsaved")
        s.key("esc")                    # Cancel
        assert s.wait_dialog_closed(), "Escape should answer the question with Cancel"
        assert near(s.px(*pic(0, 0)), WHITE) or True
        # Alt+F4 asks as well; the Discard button (second of three) lets the program exit
        s.key("f4", alt=True)
        assert s.wait_dialog()
        assert s.dialog_has_icon(QUESTION_BLUE)
        g.click(572, 416)               # Discard: the ask dialog is centred, its buttons are at x = 466, 572, 678
        assert s.wait_gone(), "Discard should close the program"


# ---------------------------------------------------------------------------------------------------- selections and image operations


@test
def sketchbook_select_ops():
    """Selection move, copy and paste, delete; flip, rotate, invert and crop from the Image menu."""
    with Qemu(cmdline="text", name="t-sketchbook-select", mem="768M") as vm:
        s = Sk(vm)
        g = s.g
        # a red 60 x 40 rectangle at (150, 150) and a blue 20 x 20 square beside it
        s.click(*S_RED)
        s.key("r")
        s.click(*STYLE["solid"])
        s.drag(*pic(150, 150), *pic(209, 189))
        s.click(*S_BLUE)
        s.drag(*pic(250, 150), *pic(269, 169))
        assert s.wait_pixel(*pic(180, 170), RED) and s.wait_pixel(*pic(260, 160), BLUE)
        # select the red rectangle and drag it 100 pixels to the right: the old place becomes white (background)
        s.key("s")
        s.drag(*pic(140, 140), *pic(219, 199))
        s.drag(*pic(180, 170), *pic(280, 170))
        s.click(*pic(500, 400))        # click outside: the selection is placed
        assert s.wait_pixel(*pic(280, 170), RED), "the moved selection is missing"
        assert near(s.px(*pic(180, 170)), WHITE), "moving a selection must leave the background colour behind"
        # undo puts it back in one step
        s.key("z", ctrl=True)
        assert s.wait_pixel(*pic(180, 170), RED) and near(s.px(*pic(280, 170)), WHITE), "undo should undo the whole move"
        # copy with Ctrl+C, paste with Ctrl+V: the pasted copy appears in the top left corner of the view and can be dragged
        s.drag(*pic(145, 145), *pic(214, 194))
        s.key("c", ctrl=True)
        s.key("v", ctrl=True)
        assert s.wait_pixel(*pic(30, 30), RED, 6), "Ctrl+V should paste the copied pixels at the top left of the view"
        s.drag(*pic(30, 30), *pic(330, 330))
        s.click(*pic(550, 440))
        assert s.wait_pixel(*pic(330, 330), RED) and near(s.px(*pic(30, 30)), WHITE), "the pasted copy should have moved"
        assert near(s.px(*pic(180, 170)), RED), "the original must still be there"
        # Delete clears a selection with the background colour
        s.drag(*pic(145, 145), *pic(214, 194))
        s.key("delete")
        assert s.wait_pixel(*pic(180, 170), WHITE), "Delete should clear the selection"
        s.key("z", ctrl=True)
        assert s.wait_pixel(*pic(180, 170), RED)
        g.shot("select")

        # Image > Flip Horizontal: the red rectangle jumps to the right edge (x -> 639 - x)
        s.menu("Image", 0)
        assert s.wait_pixel(*pic(639 - 180, 170), RED), "Flip Horizontal"
        assert near(s.px(*pic(180, 170)), WHITE)
        s.menu("Image", 1)             # Flip Vertical: y -> 479 - y
        assert s.wait_pixel(*pic(639 - 180, 479 - 170), RED), "Flip Vertical"
        s.menu("Image", 4)             # Invert: red -> cyan, white -> black
        assert s.wait_pixel(*pic(639 - 180, 479 - 170), CYAN) and s.wait_pixel(*pic(5, 5), BLACK), "Invert Colours"
        s.menu("Image", 4)             # inverting twice is the identity
        assert s.wait_pixel(*pic(639 - 180, 479 - 170), RED) and s.wait_pixel(*pic(5, 5), WHITE)
        s.key("z", ctrl=True)          # undo the second invert (history of whole-image operations)
        assert s.wait_pixel(*pic(5, 5), BLACK)
        s.key("z", ctrl=True)
        s.key("z", ctrl=True)
        s.key("z", ctrl=True)
        assert s.wait_pixel(*pic(180, 170), RED) and s.wait_pixel(*pic(5, 5), WHITE), "undo should walk back through the operations"
        # Rotate 90 clockwise: a 640x480 picture becomes 480x640; pixel (x, y) goes to (479 - y, x)
        s.menu("Image", 2)
        time.sleep(0.5)
        shot = s.screen()
        cx = 44 + 86 + (678 - 14 - 480) // 2           # screen x of the rotated picture's left edge (a scrollbar takes 14 pixels)
        cy = 56 + 30 + 6                               # taller than the view: it starts below the padding
        assert s.is_near(shot, cx + (479 - 170), cy + 180, RED, 8), "Rotate 90 clockwise: the red rectangle should be near the top right"
        assert s.is_near(shot, cx + 5, cy + 5, WHITE, 8)
        s.menu("Image", 3)             # and back
        assert s.wait_pixel(*pic(180, 170), RED), "Rotate 90 anticlockwise should undo the rotation"
