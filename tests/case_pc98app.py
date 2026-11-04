"""The PC-98 application on the desktop (user/bin/pc98app, docs/PC98_APP.md), in QEMU, with disk images that the tests make themselves:
a boot sector (tests/host/pc98app/appdemo.asm, assembled here with nasm) that draws eight colour bars, a text line, a box that follows the
Z key, a frame counter, a 2 kHz beep and a disk write on the W key. Nothing from a real PC-98 is used.

What is checked: the window's own pixels (the machine's picture at 1x, bar by bar), keys sent through the window reaching the guest, pause and
reset from the F12 menu, the disk overlay (a guest write changes neither the image file nor anything else until "Save changes as...", which
writes a new file with the change), a missing image giving a message box, the buzzer reaching the sound card at the right pitch, and the
application's own benchmark. The guest's clock can run slowly under TCG, so every wait is for a line the program prints with --trace or for pixels.

    python tools/qemu-slot.py -- python tests/run_all.py pc98app
"""
import base64
import re
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent / "host"))
import audiotools as at  # noqa: E402
import pc98apphost as H  # noqa: E402
from guitools import Gui  # noqa: E402
from harness import test, Qemu, BUILD, scaled  # noqa: E402

# The eight bars of the demo, from the colour number (blue = bit 0, red = bit 1, green = bit 2 of the number) as the display module draws them.
BARS = [(0, 0, 0), (0, 0, 255), (255, 0, 0), (255, 0, 255), (0, 255, 0), (0, 255, 255), (255, 255, 0), (255, 255, 255)]
RED, GREEN = (255, 0, 0), (0, 255, 0)
HEAD = "pc98app: "


def demo_image():
    return H.boot_d88_small(H.assemble("appdemo"))


def put_file(g, path, data):
    """Types a file into the guest: base64 in lines of 800 characters over the console, decoded there.
    (The guest's line editor redraws the rest of the line after every character it takes, so a long line is slow to type under TCG, and slower
    still when the host has other work to do: the wait for the end of each line is generous, because a slow machine is not a failing test.)"""
    enc = base64.b64encode(data).decode()
    g.sh(f"rm -f {path}.b64")
    for i in range(0, len(enc), 800):
        g.sh(f"printf %s {enc[i:i + 800]} >> {path}.b64", timeout=180)
    out = g.sh(f"base64 -d {path}.b64 > {path}; echo size=$(wc -c < {path})", timeout=90)
    assert f"size={len(data)}" in out, out


def get_file(g, path):
    out = g.sh(f"base64 {path}")
    # (the application prints its --trace lines on the same console: only lines made of base64 characters are the file)
    return base64.b64decode("".join(l.strip() for l in out.splitlines() if re.fullmatch(r"[A-Za-z0-9+/=]+", l.strip())))


def screen(vm):
    w, h, rgb = vm.screen_ppm()
    return np.frombuffer(rgb, dtype=np.uint8).reshape(h, w, 3).astype(int)


def find_picture(arr):
    """The top-left corner of the machine's picture in the screenshot, from the magenta bar (80 x 200 pixels at x = 240): None when it is not there."""
    mask = (arr == np.array([255, 0, 255])).all(axis=2)
    ys, xs = np.nonzero(mask)
    if len(xs) < 80 * 150:
        return None
    x0, y0, x1, y1 = xs.min(), ys.min(), xs.max(), ys.max()
    if x1 - x0 != 79 or y1 - y0 != 199:
        return None
    return int(x0) - 3 * 80, int(y0)


def wait_picture(vm, timeout=90):
    end = time.time() + scaled(timeout)
    while time.time() < end:
        at_ = find_picture(screen(vm))
        if at_:
            return at_
        time.sleep(0.5)
    raise TimeoutError("the machine's picture never showed in the window:\n" + vm.text()[-1500:])


def px(arr, origin, x, y):
    return tuple(int(v) for v in arr[origin[1] + y, origin[0] + x])


def box_colour(vm, origin):
    return px(screen(vm), origin, 40, 236)


def wait_box(vm, origin, colour, timeout=40):
    end = time.time() + scaled(timeout)
    while time.time() < end:
        if box_colour(vm, origin) == colour:
            return True
        time.sleep(0.3)
    return False


def start_app(name, extra="", image=None, audio=None, mem="768M", wait_for_picture=True):
    """Boots the desktop, types the demo image into the guest and starts the application on it. Returns (vm, gui, origin of the picture)."""
    vm = Qemu(cmdline="text", name=name, mem=mem, audio=audio)
    vm.start()
    g = Gui(vm)
    g.start_desktop()
    put_file(g, "/tmp/demo.d88", image if image is not None else demo_image())
    g.sh(f"pc98app --trace {extra} /tmp/demo.d88 &")
    vm.expect(re.escape(HEAD + "boot 90"), timeout=60)
    origin = wait_picture(vm) if wait_for_picture else None
    return vm, g, origin


def crop(arr, origin, x, y, w, h):
    return arr[origin[1] + y:origin[1] + y + h, origin[0] + x:origin[0] + x + w]


# ---------------------------------------------------------------------------------------------------------------- the picture and the keys

@test
def pc98app_draws_the_machines_picture_and_takes_keys():
    """The window shows the demo's eight bars, its text and its box pixel for pixel; Z held down turns the box green in the guest and released red."""
    vm, g, origin = start_app("t-pc98-picture")
    try:
        arr = screen(vm)
        for k, colour in enumerate(BARS):
            for y in (5, 100, 195):
                assert px(arr, origin, k * 80 + 40, y) == colour, f"bar {k} at row {y}: {px(arr, origin, k * 80 + 40, y)}, expected {colour}"
        assert px(arr, origin, 79, 100) == BARS[0] and px(arr, origin, 80, 100) == BARS[1], "the first bar ends at pixel 79 exactly"
        assert px(arr, origin, 300, 210) == (0, 0, 0), "below the bars the picture is black"
        assert px(arr, origin, 40, 236) == RED, "the box starts red"
        text = crop(arr, origin, 0, 320, 96, 16)                          # the banner's row: "PC98APP DEMO" in white on black
        assert (text == 255).all(axis=2).sum() > 80, "the text layer drew nothing"
        # keys: Z down, the guest's key status table says so, the box turns green; Z up, red again
        vm._input([vm._key_event("z", True)])
        assert wait_box(vm, origin, GREEN), "the box did not turn green while Z was held"
        vm._input([vm._key_event("z", False)])
        assert wait_box(vm, origin, RED), "the box did not turn red again after Z was released"
        text = vm.text()
        assert re.search(HEAD + r"key 2C down", text) and re.search(HEAD + r"key 2C up", text), "the window did not trace the key"
        g.shot("picture")
    finally:
        vm.stop()


def big_desktop(name, video, extra_cmdline=""):
    """A desktop on a bigger screen (Gui.start_desktop looks for the panel at the 1024 x 768 position, so this starts the pieces itself)."""
    vm = Qemu(cmdline=f"text video={video} {extra_cmdline}".strip(), name=name, mem="768M")
    vm.start()
    g = Gui(vm)
    g.wait_prompt()
    vm.send("fairyd &\n")
    vm.expect(r"fairyd: \d+x\d+", timeout=60)
    vm.send("hakurei-shell &\n")
    time.sleep(4)
    return vm, g


def wait_scaled_picture(vm, f, timeout=90):
    """The picture of the machine shown f times as large (each pixel an f x f block): returns its origin once it is on the screen, checking the size exactly."""
    end = time.time() + scaled(timeout)
    while time.time() < end:
        arr = screen(vm)
        ys, xs = np.nonzero((arr == np.array([255, 0, 255])).all(axis=2))
        if len(xs) > 80 * f * 150 * f:
            assert xs.max() - xs.min() == 80 * f - 1 and ys.max() - ys.min() == 200 * f - 1, (xs.min(), xs.max(), ys.min(), ys.max())
            return int(xs.min()) - 3 * 80 * f, int(ys.min())
        time.sleep(0.5)
    raise TimeoutError(f"no {f}x picture on the screen")


@test
def pc98app_keeps_the_aspect_and_scales_by_whole_numbers():
    """--scale 2 makes the picture 1280 x 800 in a window that fits it: the same bars, each pixel a 2 x 2 block."""
    vm, g = big_desktop("t-pc98-scale", "1920x1080")
    try:
        put_file(g, "/tmp/demo.d88", demo_image())
        g.sh("pc98app --trace --scale 2 /tmp/demo.d88 &")
        vm.expect(re.escape(HEAD + "boot 90"), timeout=60)
        origin = wait_scaled_picture(vm, 2)
        arr = screen(vm)
        for k, colour in enumerate(BARS):
            assert tuple(arr[origin[1] + 200, origin[0] + k * 160 + 80]) == colour, k
        g.shot("scale2")
    finally:
        vm.stop()


@test
def pc98app_is_enlarged_by_the_window_server_on_a_hidpi_screen():
    """At 2560 x 1440 the ui scale is 2: the application opens its 1x window and the server shows every picture pixel as a 2 x 2 block, bars and all."""
    vm, g = big_desktop("t-pc98-hidpi", "2560x1440")
    try:
        put_file(g, "/tmp/demo.d88", demo_image())
        g.sh("pc98app --trace /tmp/demo.d88 &")
        vm.expect(re.escape(HEAD + "boot 90"), timeout=60)
        origin = wait_scaled_picture(vm, 2)
        arr = screen(vm)
        for k, colour in enumerate(BARS):
            assert tuple(arr[origin[1] + 200, origin[0] + k * 160 + 80]) == colour, k
        g.shot("hidpi")
    finally:
        vm.stop()


# ---------------------------------------------------------------------------------------------------------------- the F12 menu: pause and reset

@test
def pc98app_pauses_and_resets_from_the_f12_menu():
    """F12 opens the menu and holds the game; P pauses (the frame counter stops, the sound too), P again resumes; R restarts the machine."""
    vm, g, origin = start_app("t-pc98-menu")
    try:
        counter = lambda: crop(screen(vm), origin, 0, 352, 40, 16).copy()          # the four hex digits of the frame counter (row 22)
        first = counter()
        time.sleep(2.5)
        assert (counter() != first).any(), "the counter does not advance: the demo is not running"
        g.key("f12")
        vm.expect(re.escape(HEAD + "menu open"), timeout=20)
        g.key("p")                                                                    # pause (the menu closes, then the machine pauses)
        vm.expect(re.escape(HEAD + "menu closed"), timeout=20)
        vm.expect(re.escape(HEAD + "paused"), timeout=20)
        time.sleep(1.0)
        frozen = counter()
        time.sleep(3.0)
        assert (counter() == frozen).all(), "the counter changed while the machine was paused"
        g.key("f12")
        g.key("p")                                                                    # resume
        vm.expect(re.escape(HEAD + "resumed"), timeout=20)
        end = time.time() + scaled(40)
        while time.time() < end and (counter() == frozen).all():
            time.sleep(0.5)
        assert (counter() != frozen).any(), "the counter did not advance after resuming"
        boots = len(re.findall(re.escape(HEAD + "boot 90"), vm.text()))
        g.key("f12")
        g.key("r")                                                                    # reset: the machine starts over, the counter from 0000
        end = time.time() + scaled(40)
        while time.time() < end and len(re.findall(re.escape(HEAD + "boot 90"), vm.text())) == boots:
            time.sleep(0.3)
        assert len(re.findall(re.escape(HEAD + "boot 90"), vm.text())) == boots + 1, "no restart"
        time.sleep(1.0)
        zero = crop(screen(vm), origin, 0, 352, 40, 16)
        digits_lit = (zero == 255).all(axis=2).sum()
        assert digits_lit > 20, "the counter is not shown after the reset"
        g.shot("after-reset")
    finally:
        vm.stop()


# ---------------------------------------------------------------------------------------------------------------- the disk overlay and "Save changes as..."

@test
def pc98app_guest_writes_go_to_memory_and_save_as_makes_a_new_file():
    """The W key makes the demo write a sector. The image file is not touched; 'Save changes as...' writes a NEW file whose only difference is that sector."""
    img = demo_image()
    vm, g, origin = start_app("t-pc98-save", image=img)
    try:
        before = get_file(g, "/tmp/demo.d88")
        assert before == img
        vm._input([vm._key_event("w", True)])
        time.sleep(0.6)
        vm._input([vm._key_event("w", False)])
        m = vm.expect(HEAD + r"disk changed: (\d+) blocks", timeout=60)
        assert int(m.group(1)) in (2, 3), m.group(0)                                  # 1024 bytes: two or three 512-byte blocks
        assert get_file(g, "/tmp/demo.d88") == img, "the image file changed"
        # a file with the name of "image-saved" exists already (the dialog proposes the next free name, -2, but a typed name is the user's): it must be refused, and say so
        g.sh("cp /tmp/demo.d88 /tmp/demo-saved.d88")
        g.key("f12")
        vm.expect(re.escape(HEAD + "menu open"), timeout=20)
        g.key("s")                                                                    # Save changes as...
        time.sleep(2.0)
        g.shot("save-dialog")
        g.key("a", ctrl=True)
        g.type("/tmp/demo-saved.d88")
        g.key("ret")
        vm.expect(HEAD + r"message: .*demo-saved\.d88.*exists already", timeout=60)
        time.sleep(1.0)
        g.key("ret")                                                                  # close the message
        assert get_file(g, "/tmp/demo-saved.d88") == img, "an existing file was overwritten"
        g.key("f12")                                                                  # the changes are still there to save: a new name
        vm.expect(re.escape(HEAD + "menu open"), timeout=20)
        g.key("s")
        time.sleep(2.0)
        g.key("a", ctrl=True)                                                         # the name field has the focus: replace its text
        g.type("/tmp/saved.d88")
        g.key("ret")
        m = vm.expect(HEAD + r"saved /tmp/saved\.d88 \((\d+) blocks changed\)", timeout=60)
        saved = get_file(g, "/tmp/saved.d88")
        assert saved != img and len(saved) == len(img)
        diff = [i for i in range(len(img)) if img[i] != saved[i]]
        assert diff[-1] - diff[0] < 1024, "more than one sector differs"
        assert img.count(b"WRITTEN BY THE DEMO") == 1, "the text is in the program only"
        assert saved.count(b"WRITTEN BY THE DEMO") == 2, "and in the saved image a second time: in the sector the guest wrote"
        assert get_file(g, "/tmp/demo.d88") == img, "the original was never touched"
        assert get_file(g, "/tmp/demo-saved.d88") == img, "the decoy is still as it was"
    finally:
        vm.stop()


@test
def pc98app_asks_before_quitting_with_unsaved_changes():
    """Closing the window with the guest's writes unsaved asks first; 'Keep running' keeps the machine, 'Discard and quit' ends the program."""
    vm, g, origin = start_app("t-pc98-quit")
    try:
        vm._input([vm._key_event("w", True)])
        time.sleep(0.6)
        vm._input([vm._key_event("w", False)])
        vm.expect(HEAD + r"disk changed: \d+ blocks", timeout=60)
        g.key("f12")
        g.key("q")
        vm.expect(HEAD + r"asking before quit: \d+ blocks changed", timeout=30)
        time.sleep(1.5)
        g.shot("quit-question")
        assert px(screen(vm), origin, 40, 236) == RED, "the machine's window is still there behind the question"
        assert g.sh("ps | grep pc98app | grep -v grep | wc -l").strip().endswith("1"), "the program is gone"
        counter = lambda: crop(screen(vm), origin, 0, 352, 40, 16).copy()
        held = counter()
        time.sleep(2.0)
        assert (counter() == held).all(), "the machine runs behind the question"
        g.key("esc")                                                                  # Keep running
        end = time.time() + scaled(40)
        while time.time() < end and (counter() == held).all():
            time.sleep(0.5)
        assert (counter() != held).any(), "the machine did not go on after 'Keep running'"
        assert g.sh("ps | grep pc98app | grep -v grep | wc -l").strip().endswith("1")
        g.key("f12")
        g.key("q")
        vm.expect(HEAD + r"asking before quit: \d+ blocks changed", timeout=30)
        time.sleep(1.5)
        g.click(543, 445)                                                             # Discard and quit
        end = time.time() + scaled(30)
        while time.time() < end and not g.sh("ps | grep pc98app | grep -v grep | wc -l").strip().endswith("0"):
            time.sleep(1)
        assert g.sh("ps | grep pc98app | grep -v grep | wc -l").strip().endswith("0"), "the program did not quit"
    finally:
        vm.stop()


# ---------------------------------------------------------------------------------------------------------------- inserting an image, the recent list, the font ROM

@test
def pc98app_inserts_an_image_through_the_file_dialog_and_remembers_it():
    """Started with no image, the application shows that; F12, 1 and a file name in the dialog boot the image, and the recent list keeps its path."""
    vm = Qemu(cmdline="text", name="t-pc98-insert", mem="768M")
    vm.start()
    g = Gui(vm)
    try:
        g.start_desktop()
        put_file(g, "/tmp/demo.d88", demo_image())
        g.sh("pc98app --trace &")
        vm.expect(HEAD + r"settings: 386 16 MHz, FM 86, sound on, volume 80, scale 1x, speed 100%", timeout=60)
        time.sleep(2)
        assert find_picture(screen(vm)) is None
        g.key("f12")
        vm.expect(re.escape(HEAD + "menu open"), timeout=20)
        g.key("1")                                                                    # Insert floppy 1...
        time.sleep(2.0)
        g.shot("insert-dialog")
        g.key("a", ctrl=True)
        g.type("/tmp/demo.d88")
        g.key("ret")
        vm.expect(re.escape(HEAD + "floppy 1: /tmp/demo.d88 (D88)"), timeout=60)
        vm.expect(re.escape(HEAD + "boot 90"), timeout=60)
        origin = wait_picture(vm)
        assert px(screen(vm), origin, 4 * 80 + 40, 100) == BARS[4]
        assert "/tmp/demo.d88" in g.sh("cat ~/.config/pc98/recent"), "the image is not in the recent list"
    finally:
        vm.stop()


@test
def pc98app_reads_the_font_rom_named_in_the_settings_file():
    """~/.config/pc98/config names a font ROM; the text layer then draws from it (a made-up one whose glyphs are solid blocks) and the status line says which font it is."""
    vm = Qemu(cmdline="text", name="t-pc98-font", mem="768M")
    vm.start()
    g = Gui(vm)
    try:
        g.start_desktop()
        put_file(g, "/tmp/demo.d88", demo_image())
        # a 288,768-byte font ROM made in the guest: all zeros, then every ANK glyph above the space (codes 21h-FFh) filled with ones
        g.sh("head -c 288768 /dev/zero > /tmp/FONT.ROM; head -c 3568 /dev/zero | tr '\\000' '\\377' > /tmp/solid; "
             "dd if=/tmp/solid of=/tmp/FONT.ROM bs=1 seek=2576 conv=notrunc 2>/dev/null; ls -l /tmp/FONT.ROM")
        g.sh("mkdir -p ~/.config/pc98; printf 'font=/tmp/FONT.ROM\\nvolume=40\\nscale=1\\n' > ~/.config/pc98/config")
        g.sh("pc98app --trace /tmp/demo.d88 &")
        vm.expect(HEAD + r"settings: .*volume 40", timeout=60)
        vm.expect(re.escape(HEAD + "font: FONT.ROM"), timeout=20)
        vm.expect(re.escape(HEAD + "boot 90"), timeout=60)
        origin = wait_picture(vm)
        time.sleep(1.0)
        arr = screen(vm)
        banner = crop(arr, origin, 0, 320, 96, 16)                                     # "PC98APP DEMO": eleven letters and digits and a space
        lit = int((banner == 255).all(axis=2).sum())
        assert lit == 11 * 8 * 16, f"{lit} white pixels in the banner row, expected eleven solid 8 x 16 blocks"
        g.shot("font")
    finally:
        vm.stop()


# ---------------------------------------------------------------------------------------------------------------- the desktop's own: start menu, icon, About, a program that stops

@test
def pc98app_is_in_the_start_menu_and_on_the_desktop():
    """The start menu has "Yukari's Gap to PC-98" (Games) and the desktop has a PC-98 icon: each starts the program."""
    vm = Qemu(cmdline="text", name="t-pc98-start", mem="768M")
    vm.start()
    g = Gui(vm)
    try:
        g.start_desktop()
        g.click(118, 756)                                                             # the start button
        time.sleep(1.5)
        g.click(140, 541)                                                             # Yukari's Gap to PC-98, under Games
        end = time.time() + scaled(60)
        while time.time() < end and not g.sh("ps | grep pc98app | grep -v grep | wc -l").strip().endswith("1"):
            time.sleep(1)
        assert g.sh("ps | grep pc98app | grep -v grep | wc -l").strip().endswith("1"), "the start menu entry did not start the program"
        time.sleep(3)
        g.shot("from-the-menu")
        assert (screen(vm) == np.array([0xEC, 0xE6, 0xD6])).all(axis=2).sum() > 1000, "no window"
        g.sh("kill $(pidof pc98app) 2>/dev/null; sleep 1")
        g.double_click(58, 690)                                                       # the PC-98 icon on the desktop: the eighth of the column (y 660..744, the panel covers its lower edge), after the Browser's
        end = time.time() + scaled(60)
        while time.time() < end and not g.sh("ps | grep pc98app | grep -v grep | wc -l").strip().endswith("1"):
            time.sleep(1)
        assert g.sh("ps | grep pc98app | grep -v grep | wc -l").strip().endswith("1"), "the desktop icon did not start the program"
    finally:
        vm.stop()


@test
def pc98app_a_program_that_stops_gets_a_message_with_what_and_where():
    """A boot sector that executes an instruction the CPU model does not have stops the machine; a box says which opcode and where, and the window stays."""
    vm, g, origin = start_app("t-pc98-stop", image=H.boot_d88_small(bytes([0x0F, 0x07])), wait_for_picture=False)
    try:
        vm.expect(HEAD + r"stopped: The program ran an instruction the emulator does not have: opcode 0F 07 at 1FC0:0000", timeout=60)
        vm.expect(HEAD + r"message: The program ran an instruction", timeout=30)
        time.sleep(2)
        g.shot("stopped")
        arr = screen(vm)
        assert (arr == np.array([0xEC, 0xE6, 0xD6])).all(axis=2).sum() > 20000, "no message box"
        assert g.sh("ps | grep pc98app | grep -v grep | wc -l").strip().endswith("1"), "the program is gone"
    finally:
        vm.stop()


@test
def pc98app_has_an_about_box():
    """F12, A opens the About box: the gap, the name, the notice that nothing from a PC-98 comes with it."""
    vm, g, origin = start_app("t-pc98-about")
    try:
        g.key("f12")
        g.key("a")
        vm.expect(re.escape(HEAD + "about box"), timeout=30)
        time.sleep(2)
        g.shot("about")
        arr = screen(vm)
        violet = ((abs(arr - np.array([0x4B, 0x2A, 0x78])) < 4).all(axis=2)).sum()
        assert violet > 800, f"the gap of the About box is not drawn ({violet} violet pixels)"
        g.key("ret")                                                                  # close it again
        time.sleep(1.5)
        assert (abs(screen(vm) - np.array([0x4B, 0x2A, 0x78])) < 4).all(axis=2).sum() < 100, "the box did not close"
    finally:
        vm.stop()


# ---------------------------------------------------------------------------------------------------------------- errors in the open

@test
def pc98app_missing_and_damaged_images_give_a_message_and_a_live_window():
    """A missing image and a damaged one each give a message box that says what; the window stays open."""
    vm = Qemu(cmdline="text", name="t-pc98-errors", mem="768M")
    vm.start()
    g = Gui(vm)
    try:
        g.start_desktop()
        img = bytearray(demo_image())
        img[0x20:0x24] = bytes([0xF0, 0xFF, 0xFF, 0x00])                           # the first track starts far beyond the end of the file
        put_file(g, "/tmp/damaged.d88", bytes(img))
        g.sh("echo this is not a disk image > /tmp/junk.d88")
        g.sh("pc98app --trace /tmp/nonexistent.d88 /tmp/damaged.d88 /tmp/junk.d88 &")
        m = vm.expect(HEAD + r"message: ", timeout=60)
        time.sleep(1.0)
        text = vm.text()[m.start():m.start() + 1500]                                  # the message has several lines
        assert "nonexistent.d88" in text and "No such file" in text, text
        assert "damaged.d88" in text and "damaged" in text and "junk.d88" in text and "not a PC-98 disk image" in text, text
        time.sleep(2)
        arr = screen(vm)
        paper = (arr == np.array([0xEC, 0xE6, 0xD6])).all(axis=2).sum()
        assert paper > 20000, "no dialog window on the screen"
        g.shot("error-box")
        assert g.sh("ps | grep pc98app | grep -v grep | wc -l").strip().endswith("1"), "the program is gone"
    finally:
        vm.stop()


# ---------------------------------------------------------------------------------------------------------------- sound

@test
def pc98app_the_buzzer_reaches_the_sound_card():
    """The demo's 2 kHz buzzer comes out of /dev/audio: the sound card's capture has a tone at 2 kHz."""
    vm, g, origin = start_app("t-pc98-sound", audio="hda")
    try:
        assert HEAD + "sound: /dev/audio" in vm.text(), "the application did not open the sound device"
        time.sleep(scaled(10))                                                        # 120 frames of beeping take about 2 s of machine time (the demo halts between frames: real time even under TCG)
    finally:
        vm.stop()
    rate, x = at.load(vm.audio_path)
    x = at.mono(x)
    segs = at.segments(x, rate, threshold=0.005)
    assert segs, "nothing was played"
    a, b = max(segs, key=lambda s: s[1] - s[0])
    tone = at.trimmed(x[a:b], rate)
    f = at.dominant_freq(tone, rate)
    assert abs(f - 2000) < 25, f"dominant frequency {f:.1f} Hz, expected about 2000"
    assert 0.02 < at.peak(tone) < 0.3, f"peak {at.peak(tone):.3f}: a buzzer is quiet"
    assert (b - a) / rate > 1.0, f"the beep lasted {(b - a) / rate:.2f} s of audio, about 2 s expected"


@test
def pc98app_the_fm_board_reaches_the_sound_card():
    """A boot sector programs the PC-9801-86's SSG for a 1006.5 Hz tone: the sound card's capture has it, clean, at the right pitch (55,466 Hz chip frames resampled to 48 kHz)."""
    vm = Qemu(cmdline="text", name="t-pc98-fm", mem="768M", audio="hda")
    vm.start()
    g = Gui(vm)
    try:
        g.start_desktop()
        put_file(g, "/tmp/fm.d88", H.boot_d88_small(H.assemble("fmtone")))
        g.sh("pc98app --trace /tmp/fm.d88 &")
        vm.expect(re.escape(HEAD + "boot 90"), timeout=60)
        assert HEAD + "sound: /dev/audio" in vm.text()
        time.sleep(scaled(10))
    finally:
        vm.stop()
    rate, x = at.load(vm.audio_path)
    x = at.mono(x)
    segs = at.segments(x, rate, threshold=0.005)
    assert segs, "nothing was played"
    a, b = max(segs, key=lambda s: s[1] - s[0])
    tone = at.trimmed(x[a:b], rate)
    f = at.dominant_freq(tone, rate)
    assert abs(f - 7987200 / 128 / 62) < 5, f"dominant frequency {f:.1f} Hz"
    assert at.band_power(tone, rate, 1006.5, 60) > 0.5, "the tone is not the main part of the sound"


# ---------------------------------------------------------------------------------------------------------------- speed

@test
def pc98app_benchmark_measures_the_speed_and_what_the_idle_skip_saves():
    """`pc98 --bench` runs a game-style frame wait headless with the idle skip off and on and prints how many times real time that was."""
    with Qemu(cmdline="text", name="t-pc98-bench", mem="768M") as vm:
        g = Gui(vm)
        g.wait_prompt()
        put_file(g, "/tmp/wait.d88", H.boot_d88_small(H.assemble("idle", ["MODE=1"])))
        out = g.sh("pc98 --bench 2 --no-sound /tmp/wait.d88", timeout=600)
        print("   " + out.strip().replace("\n", "\n   "))
        off = re.search(r"bench idle=off: ([\d.]+) s of machine time in ([\d.]+) s of host time = ([\d.]+) x real time", out)
        on = re.search(r"bench idle=on : ([\d.]+) s of machine time in ([\d.]+) s of host time = ([\d.]+) x real time.*took (\d+)% of the clocks and (\d+)% of the instructions", out)
        assert off and on, out
        assert float(off.group(1)) >= 1.9 and float(on.group(1)) >= 1.9
        assert int(on.group(4)) > 80, "the frame wait of the demo is almost all of it"
        assert float(on.group(3)) > 2 * float(off.group(3)), "the skip must pay: " + out
