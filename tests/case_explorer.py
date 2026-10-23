"""Gensokyo Explorer (`explorer`, the file manager): the job engine and model self test, navigation, sorting, selection, the keyboard,
the Netherworld (trash, restore, delete for good), copy / move / drag and drop between folders and across volumes, name conflicts,
renaming and new folders, properties, hidden files, big folders, and opening files in the other applications.

The Explorer prints "ex: ..." progress lines on the console when EXPLORER_TRACE is set; the tests wait for those, look at the file
system through the shell, and check pixels. Geometry (window content coordinates, default size 800 x 540; the first window of a
session has its content at (44, 56) on the screen):

    menu bar y 0..23, toolbar y 26..59 (back 20,43; forward 52,43; up 84,43; refresh 116,43; list 707,43; icons 737,43; new folder
    778,43), places x 0..149 (Home 75,100; Root 75,126; Netherworld 75,152; first volume 75,206), file view from x 152, y 62; the list
    has its header at y 64..85 and rows of 20 pixels from y 86; the icon view has cells of 96 x 84 pixels from (158, 64).

GUI tests poll for what they expect and run once more on a fresh VM before they fail: the guest clock lags when the host is busy and
the PS/2 pointer drops packets.
"""
import re
import time
from contextlib import contextmanager

from harness import test, Qemu, BUILD
from guitools import Gui, PROMPT, PAPER, near

OX, OY = 44, 56
W = 800


def win(x, y):
    """Window content coordinates -> screen coordinates (the first window of a session)."""
    return OX + x, OY + y


BACK, FWD, UP, REFRESH = (20, 43), (52, 43), (84, 43), (116, 43)
LIST_BTN, ICONS_BTN, NEWF_BTN = (W - 108 + 15, 43), (W - 78 + 15, 43), (W - 38 + 16, 43)
ADDRESS = (400, 43)
HOME_PLACE, ROOT_PLACE, NW_PLACE, VOL_PLACE = (75, 100), (75, 126), (75, 152), (75, 206)
HEAD_NAME, HEAD_SIZE, HEAD_MOD, HEAD_TYPE = (243, 75), (374, 75), (490, 75), (626, 75)
BODY_EMPTY = (500, 86 + 20 * 14)           # empty space below the rows of a short list


def row(i, x=240, banner=False):
    """Centre of list row i (window coordinates); inside the Netherworld the banner pushes the list down by 32 pixels."""
    return x, 86 + (32 if banner else 0) + 20 * i + 10


def cell(col, rw):
    """Centre of the icon of icon-view cell (col, rw)."""
    return 158 + 96 * col + 48, 64 + 84 * rw + 22


# ---------------------------------------------------------------------------------------------------------------------

def mark(vm):
    return len(vm.text())


def wait_after(vm, m, pattern, timeout=25):
    """Waits until the console text after offset m contains pattern; returns the match."""
    rx = re.compile(pattern, re.S)
    end = time.time() + timeout
    while time.time() < end:
        found = rx.search(vm.text()[m:])
        if found:
            return found
        time.sleep(0.1)
    raise TimeoutError(f"timeout waiting for /{pattern}/ after the action\n--- console tail ---\n{vm.text()[m:][-1800:]}")


def never_after(vm, m, pattern, wait=1.0):
    time.sleep(wait)
    found = re.search(pattern, vm.text()[m:], re.S)
    assert not found, f"unexpected {found.group(0)!r} on the console"


def order_of(vm, m):
    """The names of the last 'ex: order a|b|c' line after offset m."""
    lines = re.findall(r"ex: order ([^\n]*)", vm.text()[m:])
    assert lines, "no order line"
    return lines[-1].strip().split("|")


def make_tree(g):
    g.sh("mkdir -p /tmp/x/Alpha /tmp/x/Beta /tmp/x/.secret; echo hello > /tmp/x/a.txt; echo 'int main(void){return 0;}' > /tmp/x/prog.c; echo data > /tmp/x/Beta/inner.txt")
    g.sh("printf 'P3\\n2 2\\n255\\n255 0 0 0 255 0 0 0 255 255 255 0\\n' > /tmp/x/pic.ppm; printf '#!/bin/sh\\necho hi\\n' > /tmp/x/run.sh; chmod 755 /tmp/x/run.sh; ln -s /tmp/x/Alpha /tmp/x/link-to-alpha; seq 1 2000 > /tmp/x/numbers.dat")


@contextmanager
def session(name, tree=True, mem="768M"):
    """Boots to the console, starts the desktop, makes the test folder /tmp/x and turns the traces on."""
    with Qemu(cmdline="text", name=name, mem=mem) as vm:
        g = Gui(vm, prefix=name)
        g.start_desktop()
        g.sh("export EXPLORER_TRACE=1 NOTEBOOK_TRACE=1")
        if tree:
            make_tree(g)
        yield vm, g


def gui_test(name, body, **kw):
    """Runs body(vm, g) in a fresh session; once more on a fresh VM if it fails."""
    last = None
    for attempt in range(2):
        try:
            with session(name if attempt == 0 else name + "-retry", **kw) as (vm, g):
                body(vm, g)
            return
        except (AssertionError, TimeoutError, RuntimeError) as e:
            last = e
            print(f"  [{name}] attempt {attempt + 1} failed: {str(e)[:500]}", flush=True)
    raise last


def launch(vm, g, path="/tmp/x", items=None):
    m = mark(vm)
    vm.send(f"explorer {path} &\n")
    wait_after(vm, m, rf"ex: dir {re.escape(path)} items={items if items is not None else r'\d+'}", timeout=60)
    time.sleep(3.0)          # let the first frames and the one settling rescan (a folder touched just before the scan) pass: a busy guest drops pointer packets
    return m


def click_wait(vm, g, pos, pattern, button="left", timeout=10, count=1, tries=3):
    """Clicks at a window position and waits for the console line that proves it worked. The PS/2 pointer drops packets when the
    host is busy, so a click without effect is repeated after homing the pointer again (the clicks used here are safe to repeat)."""
    last = None
    m0 = mark(vm)                                                        # a trace from an earlier attempt that arrives late counts too
    for attempt in range(tries):
        if attempt:
            g.home()
        if button == "right":
            g.right_click(*win(*pos))
        else:
            g.click(*win(*pos), count=count)
        try:
            return wait_after(vm, m0, pattern, timeout)
        except TimeoutError as e:
            last = e
    raise last


def hold(vm, mods, fn):
    """Runs fn with the modifier keys (qcodes: ctrl, shift, alt) held down."""
    for k in mods:
        vm._input([vm._key_event(k, True)])
    time.sleep(0.08)
    try:
        return fn()
    finally:
        for k in reversed(mods):
            vm._input([vm._key_event(k, False)])
        time.sleep(0.08)


def mod_click(vm, g, mods, pos, pattern, timeout=10, tries=3):
    last = None
    m0 = mark(vm)
    for attempt in range(tries):
        if attempt:
            g.home()
        hold(vm, mods, lambda: g.click(*win(*pos)))
        try:
            return wait_after(vm, m0, pattern, timeout)
        except TimeoutError as e:
            last = e
    raise last


def drag_wait(vm, g, a, b, pattern, timeout=12, tries=3, mods=(), steps=8):
    """Drags from window position a to b (holding the modifier keys, if any) and waits for the console line that shows it worked;
    like click_wait it homes the pointer and tries again when nothing happened."""
    last = None
    m0 = mark(vm)
    for attempt in range(tries):
        if attempt:
            g.home()
        if mods:
            hold(vm, list(mods), lambda: g.drag(*win(*a), *win(*b), steps=steps))
        else:
            g.drag(*win(*a), *win(*b), steps=steps)
        try:
            return wait_after(vm, m0, pattern, timeout)
        except TimeoutError as e:
            last = e
    raise last


def ctrl_click(vm, g, pos, pattern, timeout=10):
    return mod_click(vm, g, ["ctrl"], pos, pattern, timeout)


def shift_click(vm, g, pos, pattern, timeout=10):
    return mod_click(vm, g, ["shift"], pos, pattern, timeout)


def key_wait(vm, g, qcode, pattern, timeout=20, **mods):
    m = mark(vm)
    g.key(qcode, **mods)
    return wait_after(vm, m, pattern, timeout)


def sh_lines(g, cmd):
    return [l.strip() for l in g.sh(cmd).split("\n") if l.strip()]


# ---------------------------------------------------------------------------------------------------------------------

@test
def explorer_selftest():
    """explorer --selftest: paths, formats, the folder model and the whole job engine (copy, move across volumes, conflicts, the Netherworld, delete, cancel)."""
    with Qemu(cmdline="text", name="t-explorer-self", mem="768M") as vm:
        vm.expect(PROMPT, timeout=90)
        vm.send("explorer --selftest\n")
        m = vm.expect(r"EXPLORER SELFTEST (PASS|FAIL)[^\n]*", timeout=300)
        assert "PASS" in m.group(0), vm.text()[-2500:]


@test
def explorer_navigate_sort():
    """A double click opens a folder; Up, Back, Forward, the address field and the places navigate; a bad address is reported; headings sort."""
    def body(vm, g):
        m0 = launch(vm, g)
        assert order_of(vm, m0) == ["Alpha", "Beta", "link-to-alpha", "a.txt", "numbers.dat", "pic.ppm", "prog.c", "run.sh"], order_of(vm, m0)
        g.shot("start")
        # sort by size, then reverse, then by name again
        m1 = mark(vm)
        click_wait(vm, g, HEAD_SIZE, r"ex: sort 1 desc=0")
        wait_after(vm, m1, r"ex: order [^\n]*")
        o = order_of(vm, m1)
        assert o[:4] == ["Alpha", "Beta", "link-to-alpha", "a.txt"] and o[-1] == "numbers.dat", o         # smallest files first, biggest last
        m2 = mark(vm)
        click_wait(vm, g, HEAD_SIZE, r"ex: sort 1 desc=1")
        wait_after(vm, m2, r"ex: order [^\n]*")
        time.sleep(0.4)
        o = order_of(vm, m2)
        assert o[3] == "numbers.dat" and o[:3] == ["link-to-alpha", "Beta", "Alpha"], o
        click_wait(vm, g, HEAD_NAME, r"ex: sort 0 desc=0")
        # open Beta by double click
        click_wait(vm, g, row(1), r"ex: sel 1 first=Beta")
        click_wait(vm, g, row(1), r"ex: nav /tmp/x/Beta", count=2)
        wait_after(vm, 0, r"ex: dir /tmp/x/Beta items=1")
        # Up returns to /tmp/x with Beta selected
        click_wait(vm, g, UP, r"ex: sel 1 first=Beta")
        click_wait(vm, g, BACK, r"ex: nav /tmp/x/Beta")
        click_wait(vm, g, FWD, r"ex: nav /tmp/x\n")
        # the address field
        g.click(*win(*ADDRESS))
        g.key("a", ctrl=True)
        g.type("/tmp/x/Alpha")
        m4 = mark(vm)
        g.key("ret")
        wait_after(vm, m4, r"ex: nav /tmp/x/Alpha")
        click_wait(vm, g, ROOT_PLACE, r"ex: nav /\n")
        click_wait(vm, g, HOME_PLACE, r"ex: nav /HumanVillage/reimu")
        # a bad address says so in a dialog (and does not leave the folder)
        g.click(*win(*ADDRESS))
        g.key("a", ctrl=True)
        g.type("/no/such/place")
        m5 = mark(vm)
        g.key("ret")
        wait_after(vm, m5, r"ex: dialog No such place")
        time.sleep(1.0)
        g.shot("bad-address")
        g.key("esc")
        time.sleep(0.5)
        never_after(vm, m5, r"ex: nav /no/such")
        # a folder that may not be read says so (Eiki), and the window stays where it was
        g.sh("mkdir /tmp/x/locked; chmod 000 /tmp/x/locked")
        g.click(*win(*ADDRESS))
        g.key("a", ctrl=True)
        g.type("/tmp/x/locked")
        m7 = mark(vm)
        g.key("ret")
        wait_after(vm, m7, r"ex: dialog Cannot open the folder")
        time.sleep(1.0)
        g.shot("locked")
        g.key("esc")
        time.sleep(0.5)
        # a file typed into the address field shows its folder with the file selected
        g.click(*win(*ADDRESS))
        g.key("a", ctrl=True)
        g.type("/tmp/x/prog.c")
        m6 = mark(vm)
        g.key("ret")
        wait_after(vm, m6, r"ex: nav /tmp/x\n")
        wait_after(vm, m6, r"ex: sel 1 first=prog.c")
    gui_test("t-explorer-nav", body)


@test
def explorer_select_keys():
    """Click, Ctrl+click, Shift+click, a rubber band and Ctrl+A select; arrows, Home/End, type-ahead, Enter and Backspace work from the keyboard."""
    def body(vm, g):
        launch(vm, g)
        click_wait(vm, g, row(3), r"ex: sel 1 first=a.txt")
        ctrl_click(vm, g, row(5), r"ex: sel 2 first=a.txt")
        shift_click(vm, g, row(7), r"ex: sel 3 first=pic.ppm")             # a range from the last clicked row (5) to 7
        key_wait(vm, g, "a", r"ex: sel 8", ctrl=True)
        click_wait(vm, g, BODY_EMPTY, r"ex: sel 0")                          # a click on empty space clears the selection
        # rubber band from empty space up and to the left over rows 3..7
        m = mark(vm)
        g.drag(*win(500, 86 + 20 * 12), *win(200, 86 + 20 * 3 + 4), steps=6)
        wait_after(vm, m, r"ex: sel 5 first=a.txt")
        g.shot("band")
        # keyboard: arrows move the selection, Home and End jump
        click_wait(vm, g, row(3), r"ex: sel 1 first=a.txt")
        key_wait(vm, g, "down", r"ex: sel 1 first=numbers.dat")
        key_wait(vm, g, "end", r"ex: sel 1 first=run.sh")
        key_wait(vm, g, "home", r"ex: sel 1 first=Alpha")
        m = mark(vm)
        hold(vm, ["shift"], lambda: g.key("down"))
        wait_after(vm, m, r"ex: sel 2 first=Alpha")
        # type-ahead
        click_wait(vm, g, row(0), r"ex: sel 1 first=Alpha")
        m = mark(vm)
        g.type("pr")
        wait_after(vm, m, r"ex: find pr -> prog.c")
        wait_after(vm, m, r"ex: sel 1 first=prog.c")
        g.key("esc")                                                         # clears the typed letters (they also expire after a second)
        time.sleep(0.3)
        m = mark(vm)
        g.type("l")
        wait_after(vm, m, r"ex: find l -> link-to-alpha")
        # Enter opens a folder, Backspace goes up and selects what we left
        key_wait(vm, g, "ret", r"ex: nav /tmp/x/link-to-alpha")
        key_wait(vm, g, "backspace", r"ex: sel 1 first=link-to-alpha")
        # Alt+Left goes back, Alt+Right forward
        key_wait(vm, g, "left", r"ex: nav /tmp/x/link-to-alpha", alt=True)
        key_wait(vm, g, "right", r"ex: nav /tmp/x\n", alt=True)
    gui_test("t-explorer-keys", body)


# Restore / Empty buttons of the Netherworld banner (window coordinates) and the confirmation dialogs' default keys.
RESTORE_BTN, EMPTY_BTN = (638, 77), (W - 2 - 56, 77)


def shell_has(g, path):
    return g.sh(f"test -e '{path}' && echo YES || echo NO").strip().endswith("YES")


def file_size(g, path):
    """The size of a file in bytes, from `ls -l` (the fifth column)."""
    parts = g.sh(f"ls -l '{path}'").split()
    return int(parts[4])


def job_done(vm, m, kind, ok, total, errors=0, timeout=40):
    return wait_after(vm, m, rf"ex: job done kind={kind} ok={ok} of {total} errors={errors}", timeout)


@test
def explorer_netherworld():
    """Delete sends things to /Netherworld with a note of where they came from; Restore puts them back; Delete for good and Empty ask first."""
    def body(vm, g):
        launch(vm, g)
        # Delete: a.txt goes to the Netherworld, with a sidecar note
        click_wait(vm, g, row(3), r"ex: sel 1 first=a.txt")
        m = mark(vm)
        g.key("delete")
        job_done(vm, m, 2, 1, 1)
        wait_after(vm, m, r"ex: dir /tmp/x items=7")
        assert not shell_has(g, "/tmp/x/a.txt") and shell_has(g, "/Netherworld/a.txt")
        note = g.sh("cat /Netherworld/.trashinfo/a.txt.info")
        assert "path=/tmp/x/a.txt" in note and "time=" in note, note
        # the Netherworld shows it, with its banner; Restore puts it back
        click_wait(vm, g, NW_PLACE, r"ex: dir /Netherworld items=1")
        time.sleep(0.5)
        g.shot("netherworld")
        click_wait(vm, g, row(0, banner=True), r"ex: sel 1 first=a.txt")
        m = mark(vm)
        g.click(*win(*RESTORE_BTN))
        job_done(vm, m, 4, 1, 1)
        assert shell_has(g, "/tmp/x/a.txt") and not shell_has(g, "/Netherworld/a.txt") and not shell_has(g, "/Netherworld/.trashinfo/a.txt.info")
        # a folder with content: trash, then delete it for good (asks first; Esc keeps it)
        click_wait(vm, g, (75, 100), r"ex: nav ")                                  # somewhere else first
        launch_path = "/tmp/x"
        g.click(*win(*ADDRESS))
        g.key("a", ctrl=True)
        g.type(launch_path)
        m = mark(vm)
        g.key("ret")
        wait_after(vm, m, r"ex: dir /tmp/x items=8")
        click_wait(vm, g, row(1), r"ex: sel 1 first=Beta")
        m = mark(vm)
        g.key("delete")
        job_done(vm, m, 2, 1, 1)
        assert shell_has(g, "/Netherworld/Beta/inner.txt")
        click_wait(vm, g, NW_PLACE, r"ex: dir /Netherworld items=1")
        click_wait(vm, g, row(0, banner=True), r"ex: sel 1 first=Beta")
        m = mark(vm)
        g.key("delete", shift=True)
        wait_after(vm, m, r"ex: dialog Delete for good\?")
        time.sleep(1.0)
        g.shot("confirm")
        g.key("esc")
        time.sleep(0.8)
        assert shell_has(g, "/Netherworld/Beta/inner.txt"), "Esc must keep it"
        m = mark(vm)
        g.key("delete", shift=True)
        wait_after(vm, m, r"ex: dialog Delete for good\?")
        time.sleep(1.0)
        g.key("ret")
        job_done(vm, m, 3, 1, 1)
        assert not shell_has(g, "/Netherworld/Beta") and not shell_has(g, "/Netherworld/.trashinfo/Beta.info")
        # Empty the Netherworld: two things in, the banner button asks, then it is empty
        g.sh("echo one > /Netherworld/ex-one; echo two > /Netherworld/ex-two")
        m = mark(vm)
        g.key("f5")
        wait_after(vm, m, r"ex: dir /Netherworld items=2")
        m = mark(vm)
        g.click(*win(*EMPTY_BTN))
        wait_after(vm, m, r"ex: dialog Empty the Netherworld\?")
        time.sleep(1.0)
        g.key("ret")
        job_done(vm, m, 3, 3, 3)                                            # the two things and the folder of notes
        assert g.sh("ls -A /Netherworld").strip() == "", "the Netherworld should be empty"
        # the folder of notes is made again by the next deletion
        click_wait(vm, g, HOME_PLACE, r"ex: nav /HumanVillage/reimu")
        g.sh("echo again > ~/again.txt")
        key_wait(vm, g, "f5", r"ex: order again.txt")
        click_wait(vm, g, row(0), r"ex: sel 1 first=again.txt")
        m = mark(vm)
        g.key("delete")
        job_done(vm, m, 2, 1, 1)
        assert g.sh("cat /Netherworld/.trashinfo/again.txt.info").count("path=/HumanVillage/reimu/again.txt") == 1
    gui_test("t-explorer-trash", body)


@test
def explorer_copy_move():
    """Copy, cut and paste of files and folders (also across volumes), copies in the same folder, name conflicts, and drag and drop."""
    def body(vm, g):
        launch(vm, g)
        # copy the folder Beta (row 1) into Alpha (row 0)
        click_wait(vm, g, row(1), r"ex: sel 1 first=Beta")
        key_wait(vm, g, "c", r"ex: clipboard copy 1", ctrl=True)
        click_wait(vm, g, row(0), r"ex: sel 1 first=Alpha")
        click_wait(vm, g, row(0), r"ex: dir /tmp/x/Alpha items=0", count=2)
        m = mark(vm)
        g.key("v", ctrl=True)
        job_done(vm, m, 0, 1, 1)
        wait_after(vm, m, r"ex: dir /tmp/x/Alpha items=1")
        assert g.sh("cat /tmp/x/Alpha/Beta/inner.txt").strip() == "data" and shell_has(g, "/tmp/x/Beta/inner.txt")
        # pasting the same folder again merges it and finds inner.txt in the way: the question appears, Enter keeps both
        m = mark(vm)
        g.key("v", ctrl=True)
        wait_after(vm, m, r"ex: dialog Name already taken")
        time.sleep(1.0)
        g.shot("conflict")
        g.key("ret")
        job_done(vm, m, 0, 1, 1)
        assert shell_has(g, "/tmp/x/Alpha/Beta/inner (2).txt"), g.sh("ls /tmp/x/Alpha/Beta")
        # cut a.txt in /tmp/x and paste it into Alpha: a move
        key_wait(vm, g, "backspace", r"ex: sel 1 first=Alpha")
        click_wait(vm, g, row(3), r"ex: sel 1 first=a.txt")
        key_wait(vm, g, "x", r"ex: clipboard cut 1", ctrl=True)
        click_wait(vm, g, row(0), r"ex: sel 1 first=Alpha")
        click_wait(vm, g, row(0), r"ex: dir /tmp/x/Alpha items=1", count=2)
        m = mark(vm)
        g.key("v", ctrl=True)
        job_done(vm, m, 1, 1, 1)
        assert shell_has(g, "/tmp/x/Alpha/a.txt") and not shell_has(g, "/tmp/x/a.txt")
        # a copy in the same folder gets a new name
        key_wait(vm, g, "backspace", r"ex: sel 1 first=Alpha")
        click_wait(vm, g, row(3), r"ex: sel 1 first=numbers.dat")
        key_wait(vm, g, "c", r"ex: clipboard copy 1", ctrl=True)
        m = mark(vm)
        g.key("v", ctrl=True)
        job_done(vm, m, 0, 1, 1)
        assert shell_has(g, "/tmp/x/numbers (copy).dat"), g.sh("ls /tmp/x")
        # move across volumes: /tmp is a different volume than the home folder (copy, then the original is removed)
        time.sleep(0.5)
        o = order_of(vm, 0)
        idx = o.index("prog.c")
        click_wait(vm, g, row(idx), r"ex: sel 1 first=prog.c")
        key_wait(vm, g, "x", r"ex: clipboard cut 1", ctrl=True)
        click_wait(vm, g, HOME_PLACE, r"ex: nav /HumanVillage/reimu")
        m = mark(vm)
        g.key("v", ctrl=True)
        job_done(vm, m, 1, 1, 1)
        assert g.sh("cat /HumanVillage/reimu/prog.c").strip() == "int main(void){return 0;}" and not shell_has(g, "/tmp/x/prog.c")
        # a paste that cannot work (a folder nobody may write to) does not stop the job: one list of problems at the end
        g.sh("mkdir /tmp/x/ro; chmod 555 /tmp/x/ro; echo q > /tmp/x/q.txt")
        g.click(*win(*ADDRESS))
        g.key("a", ctrl=True)
        g.type("/tmp/x")
        m = mark(vm)
        g.key("ret")
        wait_after(vm, m, r"ex: order [^\n]*q\.txt")
        o = order_of(vm, m)
        click_wait(vm, g, row(o.index("q.txt")), r"ex: sel 1 first=q.txt")
        key_wait(vm, g, "c", r"ex: clipboard copy 1", ctrl=True)
        g.click(*win(*ADDRESS))
        g.key("a", ctrl=True)
        g.type("/tmp/x/ro")
        m = mark(vm)
        g.key("ret")
        wait_after(vm, m, r"ex: dir /tmp/x/ro items=0")
        m = mark(vm)
        g.key("v", ctrl=True)
        wait_after(vm, m, r"ex: job done kind=0 ok=0 of 1 errors=1")
        wait_after(vm, m, r"ex: dialog Something went wrong")
        time.sleep(1.0)
        g.shot("errors")
        g.key("ret")
        time.sleep(0.6)
        assert not shell_has(g, "/tmp/x/ro/q.txt")
    gui_test("t-explorer-copy", body)


@test
def explorer_drag_drop():
    """Dragging onto a folder moves (copies with Ctrl, or to another volume), onto the Netherworld deletes, onto nothing does nothing."""
    def body(vm, g):
        launch(vm, g)
        # numbers.dat (row 4) onto the folder Beta (row 1): moved, same volume
        m = mark(vm)
        drag_wait(vm, g, row(4), row(1), r"ex: drop 1 items on /tmp/x/Beta")
        job_done(vm, m, 1, 1, 1)
        assert shell_has(g, "/tmp/x/Beta/numbers.dat") and not shell_has(g, "/tmp/x/numbers.dat")
        # now: Alpha Beta link-to-alpha a.txt pic.ppm prog.c run.sh. With Ctrl held a drag copies: pic.ppm (row 4) onto Alpha (row 0)
        wait_after(vm, m, r"ex: order Alpha\|Beta\|link-to-alpha\|a.txt\|pic.ppm")
        m = mark(vm)
        drag_wait(vm, g, row(4), row(0), r"ex: drop 1 items on /tmp/x/Alpha", mods=["ctrl"])
        job_done(vm, m, 0, 1, 1)
        assert shell_has(g, "/tmp/x/Alpha/pic.ppm") and shell_has(g, "/tmp/x/pic.ppm")
        # two selected items go together: a.txt and pic.ppm onto Alpha
        click_wait(vm, g, row(3), r"ex: sel 1 first=a.txt")
        ctrl_click(vm, g, row(4), r"ex: sel 2 first=a.txt")
        m = mark(vm)
        drag_wait(vm, g, row(3), row(0), r"ex: drop 2 items on /tmp/x/Alpha")
        # pic.ppm is already in Alpha (the copy above): the job asks, Enter keeps both
        wait_after(vm, m, r"ex: dialog Name already taken")
        time.sleep(0.8)
        g.key("ret")
        job_done(vm, m, 1, 2, 2)
        assert shell_has(g, "/tmp/x/Alpha/a.txt") and not shell_has(g, "/tmp/x/a.txt")
        assert shell_has(g, "/tmp/x/Alpha/pic (2).ppm") and not shell_has(g, "/tmp/x/pic.ppm")
        # onto the Netherworld place: a delete
        wait_after(vm, m, r"ex: order Alpha\|Beta\|link-to-alpha\|prog.c\|run.sh")
        m = mark(vm)
        drag_wait(vm, g, row(3), NW_PLACE, r"ex: drop 1 items on /Netherworld")
        job_done(vm, m, 2, 1, 1)
        assert shell_has(g, "/Netherworld/prog.c") and not shell_has(g, "/tmp/x/prog.c")
        # dropped on empty space or on a file: nothing happens
        m = mark(vm)
        drag_wait(vm, g, row(3), BODY_EMPTY, r"ex: drag start")
        never_after(vm, m, r"ex: job queued")
        # to another volume the default is a copy: run.sh onto Home
        m = mark(vm)
        drag_wait(vm, g, row(3), HOME_PLACE, r"ex: drop 1 items on /HumanVillage/reimu")
        job_done(vm, m, 0, 1, 1)
        assert shell_has(g, "/HumanVillage/reimu/run.sh") and shell_has(g, "/tmp/x/run.sh")
    gui_test("t-explorer-dnd", body)


def dialog_origin(w, h):
    """Screen position of the content of a dialog of w x h pixels: the server centres it in the work area (1024 x 732)."""
    return (1024 - (w + 8)) // 2 + 4, (732 - (h + 30)) // 2 + 26


@test
def explorer_rename_properties():
    """New Folder and F2 rename in place (the extension stays, a taken name is refused, Esc cancels); Properties changes permissions; Ctrl+H shows hidden files; icons; the context menu."""
    def body(vm, g):
        launch(vm, g)
        # New Folder makes "New Folder" and starts renaming it
        m = mark(vm)
        click_wait(vm, g, NEWF_BTN, r"ex: created /tmp/x/New Folder")
        wait_after(vm, m, r"ex: rename start New Folder")
        time.sleep(0.6)
        g.shot("newfolder")
        g.type("Gate")
        key_wait(vm, g, "ret", r"ex: renamed New Folder -> Gate")
        assert shell_has(g, "/tmp/x/Gate") and not shell_has(g, "/tmp/x/New Folder")
        # now: Alpha Beta Gate link-to-alpha a.txt numbers.dat pic.ppm prog.c run.sh
        click_wait(vm, g, row(4), r"ex: sel 1 first=a.txt")
        key_wait(vm, g, "f2", r"ex: rename start a.txt")
        time.sleep(0.4)
        g.type("doc")                                                        # only "a" was selected: the extension stays
        key_wait(vm, g, "ret", r"ex: renamed a.txt -> doc.txt")
        assert shell_has(g, "/tmp/x/doc.txt") and not shell_has(g, "/tmp/x/a.txt")
        # a name that is taken is refused with a dialog
        click_wait(vm, g, row(7), r"ex: sel 1 first=prog.c")
        key_wait(vm, g, "f2", r"ex: rename start prog.c")
        time.sleep(0.4)
        g.key("a", ctrl=True)
        g.type("Beta")
        m = mark(vm)
        g.key("ret")
        wait_after(vm, m, r"ex: dialog Cannot rename")
        time.sleep(0.8)
        g.key("esc")
        time.sleep(0.5)
        assert shell_has(g, "/tmp/x/prog.c") and shell_has(g, "/tmp/x/Beta/inner.txt")
        # Esc cancels a rename
        click_wait(vm, g, row(8), r"ex: sel 1 first=run.sh")
        key_wait(vm, g, "f2", r"ex: rename start run.sh")
        time.sleep(0.4)
        g.type("zzz")
        m = mark(vm)
        g.key("esc")
        never_after(vm, m, r"ex: renamed", wait=1.0)
        assert shell_has(g, "/tmp/x/run.sh")
        # Properties of run.sh (mode 755, still selected): take away the owner's execute bit
        m = mark(vm)
        g.key("ret", alt=True)
        wait_after(vm, m, r"ex: dialog Properties")
        time.sleep(1.2)
        g.shot("properties")
        ox, oy = dialog_origin(440, 442)                                     # a file on a volume: 420 + the volume row
        m = mark(vm)
        g.click(ox + 285, oy + 242)                                          # Owner / Execute
        time.sleep(0.4)
        g.key("ret")
        wait_after(vm, m, r"ex: properties applied mode=0?[0-7]*")
        assert g.sh("ls -l /tmp/x/run.sh").strip().startswith("-rw-r-xr-x"), g.sh("ls -l /tmp/x/run.sh")
        # hidden files
        m = mark(vm)
        g.key("h", ctrl=True)
        wait_after(vm, m, r"ex: hidden 1 items=10")
        time.sleep(0.4)
        assert ".secret" in order_of(vm, m)
        m = mark(vm)
        g.key("h", ctrl=True)
        wait_after(vm, m, r"ex: hidden 0 items=9")
        # the icon view and the list view again
        click_wait(vm, g, ICONS_BTN, r"ex: view icons")
        time.sleep(0.4)
        g.shot("icons")
        click_wait(vm, g, cell(1, 0), r"ex: sel 1 first=Beta")
        # an icon can be renamed too
        key_wait(vm, g, "f2", r"ex: rename start Beta")
        time.sleep(0.4)
        g.shot("icon-rename")
        g.key("esc")
        time.sleep(0.4)
        key_wait(vm, g, "1", r"ex: view list", ctrl=True)
        # the context menu: right click a file, Up wraps to the last entry (Properties), Enter picks it
        m = mark(vm)
        g.right_click(*win(*row(4)))
        wait_after(vm, m, r"ex: context menu 12 items")
        time.sleep(0.5)
        g.shot("context")
        g.key("up")
        m = mark(vm)
        g.key("ret")
        wait_after(vm, m, r"ex: dialog Properties")
        time.sleep(0.8)
        g.key("esc")
    gui_test("t-explorer-rename", body)


@test
def explorer_progress_cancel():
    """A long copy gets a progress window; Cancel stops it and removes the half-written file; the same copy left alone completes."""
    def body(vm, g):
        g.sh("dd if=/dev/zero of=/tmp/x/big.bin bs=1048576 count=160 2>/dev/null; ls -l /tmp/x/big.bin", timeout=240)
        launch(vm, g)
        o = order_of(vm, 0)
        assert "big.bin" in o, o
        click_wait(vm, g, row(o.index("big.bin")), r"ex: sel 1 first=big.bin")
        key_wait(vm, g, "c", r"ex: clipboard copy 1", ctrl=True)
        click_wait(vm, g, HOME_PLACE, r"ex: nav /HumanVillage/reimu")
        # paste and cancel while the progress window is up (160 MB take a few seconds even on a fast host)
        m = mark(vm)
        g.key("v", ctrl=True)
        wait_after(vm, m, r"ex: dialog File operation", timeout=60)
        time.sleep(0.5)
        g.shot("progress")
        g.key("esc")
        wait_after(vm, m, r"ex: job done kind=0 ok=0 of 1 errors=0 skipped=0 cancelled", timeout=60)
        time.sleep(0.5)
        assert not shell_has(g, "/HumanVillage/reimu/big.bin"), "the half-written copy must be removed"
        # paste again and let it finish
        m = mark(vm)
        g.key("v", ctrl=True)
        job_done(vm, m, 0, 1, 1, timeout=240)
        size_a, size_b = file_size(g, "/tmp/x/big.bin"), file_size(g, "/HumanVillage/reimu/big.bin")
        assert size_a == size_b == 160 * 1048576, (size_a, size_b)
    gui_test("t-explorer-progress", body, mem="1024M")


@test
def explorer_big_folder():
    """A folder of 3000 files is read in slices and navigated at once: select all, End, Page Down and type-ahead work."""
    def body(vm, g):
        g.sh("mkdir -p /tmp/big; cd /tmp/big; seq 1 3000 | xargs -n 250 touch; ls | wc -l")
        m = mark(vm)
        vm.send("explorer /tmp/big &\n")
        found = wait_after(vm, m, r"ex: dir /tmp/big items=3000 hidden=0 ms=(\d+)", timeout=120)
        print(f"  3000 entries read in {found.group(1)} ms", flush=True)
        o = order_of(vm, m)
        assert o[:5] == ["1", "2", "3", "4", "5"] and o[9] == "10", o               # natural order, not 1, 10, 100
        time.sleep(3.0)
        g.shot("big")
        key_wait(vm, g, "a", r"ex: sel 3000", ctrl=True)
        click_wait(vm, g, row(2), r"ex: sel 1 first=3")
        key_wait(vm, g, "end", r"ex: sel 1 first=3000")
        key_wait(vm, g, "home", r"ex: sel 1 first=1\n")
        m = mark(vm)
        g.key("pgdn")
        wait_after(vm, m, r"ex: sel 1 first=2[0-9]\n")                       # about a page further down (row 21)
        time.sleep(0.5)
        m = mark(vm)
        g.type("2999")
        wait_after(vm, m, r"ex: find 2999 -> 2999")
        time.sleep(0.6)
        g.shot("big-end")
        # the window still draws while it is at the end of the list: the row of 2999 is highlighted somewhere in the view
        assert g.count_pixels((OX + 154, OY + 86, 640, 440), (0xB5, 0x12, 0x2B), tol=8, step=3) > 50
    gui_test("t-explorer-big", body, mem="1024M")


@test
def explorer_open_apps():
    """Enter opens text in the Notebook, pictures in the Gallery and programs in a terminal window."""
    def body(vm, g):
        g.sh("printf '#!/bin/sh\\necho ran > /tmp/x/ran.flag\\n' > /tmp/x/run.sh; chmod 755 /tmp/x/run.sh")
        launch(vm, g)
        # a program: a terminal opens, runs it, and waits for Enter
        click_wait(vm, g, row(7), r"ex: sel 1 first=run.sh")
        m = mark(vm)
        g.key("ret")
        wait_after(vm, m, r"ex: open /tmp/x/run.sh with terminal")
        deadline = time.time() + 40
        while time.time() < deadline and not shell_has(g, "/tmp/x/ran.flag"):
            time.sleep(1.0)
        assert shell_has(g, "/tmp/x/ran.flag"), "the program did not run in the terminal"
        time.sleep(1.0)
        g.shot("terminal")
        g.key("ret")                                                         # the terminal closes when Enter is pressed
        time.sleep(1.5)
        # text goes to the Notebook
        click_wait(vm, g, row(3), r"ex: sel 1 first=a.txt")
        m = mark(vm)
        g.key("ret")
        wait_after(vm, m, r"ex: open /tmp/x/a.txt with notebook")
        wait_after(vm, m, r"nb: open /tmp/x/a.txt lines=\d+ bytes=6", timeout=40)
        time.sleep(1.0)
        g.shot("notebook")
        m = mark(vm)
        g.key("q", ctrl=True)
        wait_after(vm, m, r"nb: quit")
        time.sleep(1.0)
        # a picture goes to the Gallery
        click_wait(vm, g, row(5), r"ex: sel 1 first=pic.ppm")
        m = mark(vm)
        g.key("ret")
        wait_after(vm, m, r"ex: open /tmp/x/pic.ppm with gallery")
        time.sleep(4.0)
        assert "gallery" in g.sh("ps"), "the Gallery is not running"
    gui_test("t-explorer-open", body)
