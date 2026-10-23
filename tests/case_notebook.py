"""Patchouli's Notebook (the `notebook` text editor): model self test, typing and saving, undo/redo, find and replace, line
endings, the unsaved-changes question, big files, bad input, preferences and syntax colours.

The editor prints "nb: ..." progress lines to the console when NOTEBOOK_TRACE is set; the tests wait for those and check the
bytes that reach the disk. Window geometry assumed: the first normal window opens with its content at (44, 56); the menu bar
titles File/Edit/View/Help sit at x = 69/118/167/213, y = 68 on the screen."""
import hashlib
import re
import time
from contextlib import contextmanager

from harness import test, Qemu, BUILD
from guitools import Gui, PROMPT

DEMO = [
    "#include <stdio.h>",
    "/* Patchouli's first spell card */",
    "static int spell(int power) {",
    "    if (power > 9000) return 1;   // over nine thousand",
    "    printf(\"Master Spark! %d\\n\", power);",
    "    return 0;",
    "}",
    "",
    "int main(void) {",
    "    for (int i = 0; i < 3; i++) spell(i * 4500);",
    "    return 0;",
    "}",
]
DEMO_BYTES = ("\n".join(DEMO) + "\n").encode()
TEXT_CLICK = (44 + 300, 56 + 200)            # a spot inside the text area of the default window


@contextmanager
def session(name):
    """Boots to the console, starts the desktop and turns the editor's trace on."""
    with Qemu(cmdline="text", name=name, mem="768M") as vm:
        g = Gui(vm, prefix=name)
        g.start_desktop()
        g.sh("export NOTEBOOK_TRACE=1")
        yield vm, g


def launch(vm, args="", ready=r"nb: pos 1 1 "):
    vm.send(f"notebook {args} &\n")
    vm.expect(ready, timeout=30)
    time.sleep(0.8)


def make_demo(g, path="/tmp/demo.c"):
    for i, line in enumerate(DEMO):
        quoted = line.replace("'", "'\\''")
        g.sh(f"echo '{quoted}' {'>>' if i else '>'} {path}")


def md5_of(g, path):
    out = g.sh(f"md5sum {path}")
    m = re.search(r"\b([0-9a-f]{32})\b", out)
    assert m, f"md5sum printed nothing useful for {path}: {out!r}"
    return m.group(1)


def expect_file(g, path, data, what=""):
    got = md5_of(g, path)
    want = hashlib.md5(data).hexdigest()
    if got != want:
        dump = g.sh(f"od -An -c {path}")
        raise AssertionError(f"{what}: {path} differs from the expected {len(data)} bytes ({data[:80]!r}...).\nod -c says:\n{dump[:900]}")


def trace_lines(vm):
    return re.findall(r"nb: [^\n]*", vm.text())


# ---------------------------------------------------------------------------------------------------------------------

@test
def notebook_selftest():
    """notebook --selftest: gap buffer, line index, undo/redo, wrap rules, highlighters and file layer, editor operations and fuzz (34000+ checks)."""
    with Qemu(cmdline="text", name="t-notebook-self", mem="512M") as vm:
        vm.expect(PROMPT, timeout=90)
        vm.send("notebook --selftest\n")
        m = vm.expect(r"NOTEBOOK SELFTEST (PASS|FAIL)[^\n]*", timeout=240)
        assert "PASS" in m.group(0), vm.text()[-1800:]


@test
def notebook_edit_save():
    """Typing, auto-indent, Tab/Shift+Tab, undo and redo, cut/copy/paste and Save write exactly the expected bytes."""
    with session("t-notebook-edit") as (vm, g):
        launch(vm, "/tmp/t1.txt")                        # a path that does not exist yet: an empty page with that name
        g.type("Hello Gensokyo\n  indented")
        g.key("ret")                                     # auto-indent copies the two spaces
        g.type("next")
        vm.expect(r"nb: title Notebook - \*t1.txt", timeout=10)
        g.key("s", ctrl=True)
        vm.expect(r"nb: saved /tmp/t1.txt bytes=32", timeout=10)
        vm.expect(r"nb: title Notebook - t1.txt", timeout=10)
        expect_file(g, "/tmp/t1.txt", b"Hello Gensokyo\n  indented\n  next", "after typing")

        g.key("z", ctrl=True)                            # undo "next"
        g.key("s", ctrl=True)
        vm.expect(r"nb: saved /tmp/t1.txt bytes=28", timeout=10)
        expect_file(g, "/tmp/t1.txt", b"Hello Gensokyo\n  indented\n  ", "after undo of the typed word")
        g.key("z", ctrl=True)                            # undo the Enter together with its indentation
        g.key("s", ctrl=True)
        vm.expect(r"nb: saved /tmp/t1.txt bytes=25", timeout=10)
        expect_file(g, "/tmp/t1.txt", b"Hello Gensokyo\n  indented", "after undo of the line break")
        g.key("y", ctrl=True)
        g.key("z", ctrl=True, shift=True)                # both redo spellings
        g.key("s", ctrl=True)
        vm.expect(r"nb: saved /tmp/t1.txt bytes=32", timeout=10)
        expect_file(g, "/tmp/t1.txt", b"Hello Gensokyo\n  indented\n  next", "after redo")

        g.key("tab")                                     # to the next tab stop (column 8): two spaces
        g.type("x")
        g.key("tab", shift=True)                         # removes the line's indentation (two spaces)
        g.key("s", ctrl=True)
        vm.expect(r"nb: saved /tmp/t1.txt bytes=33", timeout=10)
        expect_file(g, "/tmp/t1.txt", b"Hello Gensokyo\n  indented\nnext  x", "after tab / shift+tab")

        g.key("a", ctrl=True)
        g.key("c", ctrl=True)
        g.key("end", ctrl=True)
        g.key("v", ctrl=True)
        g.key("s", ctrl=True)
        vm.expect(r"nb: saved /tmp/t1.txt bytes=66", timeout=10)
        both = b"Hello Gensokyo\n  indented\nnext  x"
        expect_file(g, "/tmp/t1.txt", both + both, "after select all, copy, paste")


@test
def notebook_find_replace():
    """Find counts and selects matches, F3 steps through them, Replace All is one undo step, Match case works."""
    with session("t-notebook-find") as (vm, g):
        make_demo(g)
        launch(vm, "/tmp/demo.c")
        g.click(*TEXT_CLICK)
        g.key("f", ctrl=True)
        g.type("spell")
        vm.expect(r"nb: find count=3", timeout=10)
        g.key("ret")
        vm.expect(r"nb: found at \d+", timeout=10)
        g.key("f3")
        vm.expect(r"nb: found at \d+", timeout=10)
        g.key("f3", shift=True)
        vm.expect(r"nb: found at \d+", timeout=10)
        shot = g.screen()
        assert g.count_pixels((44 + 90, 56 + 26, 400, 16 * 12), (0xF3, 0xDC, 0x8C), tol=6, shot=shot) > 100, "other matches are not highlighted"
        # Match case: "SPELL" has no match; turning the option on makes the count drop to 0
        g.key("a", ctrl=True)
        g.type("SPELL")
        vm.expect(r"nb: find count=3", timeout=10)
        g.click(44 + 489, 56 + 482)                      # the "Match case" check box
        vm.expect(r"nb: find count=0", timeout=10)
        g.key("a", ctrl=True)
        g.type("spell")
        vm.expect(r"nb: find count=3", timeout=10)
        # replace all
        g.key("h", ctrl=True)
        g.key("tab")
        g.type("charm")
        g.click(44 + 664, 56 + 481)                      # Replace All
        vm.expect(r"nb: replaced 3", timeout=10)
        g.key("esc")
        g.key("s", ctrl=True)
        vm.expect(r"nb: saved /tmp/demo.c", timeout=10)
        expect_file(g, "/tmp/demo.c", DEMO_BYTES.replace(b"spell", b"charm"), "after Replace All")
        g.key("z", ctrl=True)                            # one undo reverts all three replacements
        g.key("s", ctrl=True)
        vm.expect(r"nb: saved /tmp/demo.c", timeout=10)
        expect_file(g, "/tmp/demo.c", DEMO_BYTES, "after undoing Replace All")


@test
def notebook_line_endings():
    """CRLF files stay CRLF (and LF files LF) after editing; a UTF-8 BOM is kept; invalid UTF-8 bytes survive a save."""
    with session("t-notebook-eol") as (vm, g):
        g.sh("printf 'line one\\r\\nline two\\r\\nline three\\r\\n' > /tmp/crlf.txt")
        g.sh("printf '\\0357\\0273\\0277bom line\\n' > /tmp/bom.txt")
        g.sh("printf 'caf\\0351 ok\\n' > /tmp/bad.txt")
        launch(vm, "/tmp/crlf.txt")
        g.key("end", ctrl=True)
        g.type("NEW")
        g.key("ret")
        g.type("LINE")
        g.key("s", ctrl=True)
        vm.expect(r"nb: saved /tmp/crlf.txt bytes=41", timeout=10)
        expect_file(g, "/tmp/crlf.txt", b"line one\r\nline two\r\nline three\r\nNEW\r\nLINE", "CRLF preserved")
        g.key("q", ctrl=True)
        vm.expect(r"nb: quit", timeout=10)

        launch(vm, "/tmp/bom.txt")
        g.key("end", ctrl=True)
        g.type("more")
        g.key("s", ctrl=True)
        vm.expect(r"nb: saved /tmp/bom.txt", timeout=10)
        expect_file(g, "/tmp/bom.txt", b"\xef\xbb\xbfbom line\nmore", "BOM preserved")
        g.key("q", ctrl=True)
        vm.expect(r"nb: quit", timeout=10)

        launch(vm, "/tmp/bad.txt")
        g.type("X")
        g.key("s", ctrl=True)
        vm.expect(r"nb: saved /tmp/bad.txt", timeout=10)
        expect_file(g, "/tmp/bad.txt", b"Xcaf\xe9 ok\n", "invalid UTF-8 byte kept")
        g.key("q", ctrl=True)
        vm.expect(r"nb: quit", timeout=10)

        g.sh("printf 'a\\r\\nb\\nc\\r\\n' > /tmp/mixed.txt")           # mostly CRLF: the odd line ending is normalised
        launch(vm, "/tmp/mixed.txt", ready=r"nb: open /tmp/mixed.txt lines=4 bytes=6 eol=CRLF")
        g.type("X")
        g.key("s", ctrl=True)
        vm.expect(r"nb: saved /tmp/mixed.txt", timeout=10)
        expect_file(g, "/tmp/mixed.txt", b"Xa\r\nb\r\nc\r\n", "mixed line endings become the majority style")
        g.key("q", ctrl=True)
        vm.expect(r"nb: quit", timeout=10)

        g.sh("printf 'one\\rtwo\\r' > /tmp/mac.txt")                   # classic Mac line endings
        launch(vm, "/tmp/mac.txt", ready=r"nb: open /tmp/mac.txt lines=3 bytes=8 eol=CR")
        g.key("end", ctrl=True)
        g.type("end")
        g.key("s", ctrl=True)
        vm.expect(r"nb: saved /tmp/mac.txt", timeout=10)
        expect_file(g, "/tmp/mac.txt", b"one\rtwo\rend", "CR line endings preserved")


@test
def notebook_eol_switch():
    """Clicking the line ending in the status bar switches LF to CRLF; the page counts as modified and saves with CRLF."""
    with session("t-notebook-eolsw") as (vm, g):
        make_demo(g)
        launch(vm, "/tmp/demo.c")
        g.click(714, 566)                                # the "LF" segment of the status bar
        vm.expect(r"nb: msg Line endings will be CRLF", timeout=10)
        vm.expect(r"nb: title Notebook - \*demo.c", timeout=10)
        g.key("s", ctrl=True)
        vm.expect(r"nb: saved /tmp/demo.c bytes=292", timeout=10)
        expect_file(g, "/tmp/demo.c", DEMO_BYTES.replace(b"\n", b"\r\n"), "converted to CRLF")
        g.click(714, 566)                                # and back
        vm.expect(r"nb: msg Line endings will be LF", timeout=10)
        g.key("s", ctrl=True)
        vm.expect(r"nb: saved /tmp/demo.c bytes=280", timeout=10)
        expect_file(g, "/tmp/demo.c", DEMO_BYTES, "converted back to LF")


def file_menu(g, y):
    g.click(69, 68)                                      # File
    time.sleep(0.4)
    g.click(95, y)


@test
def notebook_menus_and_recent():
    """File menu with the mouse: Save As, Close, a recent file, Open... and Quit; New with unsaved text asks first."""
    with session("t-notebook-menus") as (vm, g):
        launch(vm, "")                                   # no argument: an untitled page
        g.type("menu text")
        vm.expect(r"nb: title Notebook - \*untitled", timeout=10)
        file_menu(g, 182)                                # Save As...
        vm.expect(r"nb: dialog save-as", timeout=10)
        time.sleep(0.8)
        g.key("a", ctrl=True)
        g.type("/tmp/m2.txt")
        g.key("ret")
        vm.expect(r"nb: saved /tmp/m2.txt", timeout=10)
        expect_file(g, "/tmp/m2.txt", b"menu text", "Save As from the menu")
        file_menu(g, 226)                                # Close: nothing unsaved, so no question
        vm.expect(r"nb: msg Page closed", timeout=10)
        vm.expect(r"nb: title Notebook - untitled", timeout=10)
        file_menu(g, 277)                                # the recent file list: entry 1
        vm.expect(r"nb: open /tmp/m2.txt lines=1 bytes=9", timeout=10)
        g.type("!")
        file_menu(g, 94)                                 # New: the page is modified, so it asks
        vm.expect(r"nb: dialog Unsaved changes", timeout=10)
        time.sleep(0.8)
        g.click(585, 413)                                # Don't save
        vm.expect(r"nb: msg A fresh page", timeout=10)
        assert md5_of(g, "/tmp/m2.txt") == hashlib.md5(b"menu text").hexdigest(), "Don't save changed the file"
        file_menu(g, 138)                                # Open...
        vm.expect(r"nb: dialog open", timeout=10)
        time.sleep(0.8)
        g.key("esc")
        time.sleep(0.4)
        g.key("esc")
        time.sleep(0.5)
        file_menu(g, 248)                                # Quit
        vm.expect(r"nb: quit", timeout=10)


@test
def notebook_confirm_dialogs():
    """Saving over another file and over a file that changed on disk asks first; Esc keeps the other file, Enter replaces."""
    with session("t-notebook-confirm") as (vm, g):
        g.sh("echo AAA > /tmp/a.txt")
        g.sh("echo BBB > /tmp/b.txt")
        launch(vm, "/tmp/a.txt")
        for answer in ("esc", "ret"):
            g.key("s", ctrl=True, shift=True)
            vm.expect(r"nb: dialog save-as", timeout=10)
            time.sleep(0.8)
            g.key("a", ctrl=True)
            g.type("/tmp/b.txt")
            g.key("ret")
            vm.expect(r"nb: dialog replace", timeout=10)
            time.sleep(0.8)
            g.key(answer)
            if answer == "esc":
                time.sleep(0.8)
                expect_file(g, "/tmp/b.txt", b"BBB\n", "Cancel must keep the other file")
            else:
                vm.expect(r"nb: saved /tmp/b.txt", timeout=10)
                expect_file(g, "/tmp/b.txt", b"AAA\n", "Replace overwrites the other file")
        g.sh("sleep 2")                                  # modification times have a resolution of one second (of guest time)
        g.sh("echo external > /tmp/b.txt")
        g.type("x")
        for answer in ("esc", "ret"):
            g.key("s", ctrl=True)
            vm.expect(r"nb: dialog changed-on-disk", timeout=10)
            time.sleep(0.8)
            g.key(answer)
            if answer == "esc":
                time.sleep(0.8)
                expect_file(g, "/tmp/b.txt", b"external\n", "Cancel must keep what is on disk")
            else:
                vm.expect(r"nb: saved /tmp/b.txt", timeout=10)
                expect_file(g, "/tmp/b.txt", b"xAAA\n", "Overwrite writes the page")
        # "Save" in the unsaved-changes question goes through the same check, and quits only after the save really happened
        g.type("y")
        g.sh("sleep 2")
        g.sh("echo ext2 > /tmp/b.txt")
        for answer in ("esc", "ret"):
            g.key("f4", alt=True)
            vm.expect(r"nb: dialog Unsaved changes", timeout=10)
            time.sleep(0.8)
            g.key("ret")                                 # Save
            vm.expect(r"nb: dialog changed-on-disk", timeout=10)
            time.sleep(0.8)
            g.key(answer)
            if answer == "esc":
                time.sleep(0.8)
                expect_file(g, "/tmp/b.txt", b"ext2\n", "Cancel keeps what is on disk and the editor stays open")
            else:
                vm.expect(r"nb: saved /tmp/b.txt", timeout=10)
                vm.expect(r"nb: quit", timeout=10)
                expect_file(g, "/tmp/b.txt", b"xyAAA\n", "Overwrite, then quit")


@test
def notebook_reload():
    """Reload from Disk (F5) picks up changes made elsewhere and asks before throwing away unsaved text."""
    with session("t-notebook-reload") as (vm, g):
        g.sh("echo one > /tmp/r.txt")
        launch(vm, "/tmp/r.txt")
        g.sh("echo two-lines > /tmp/r.txt")
        g.sh("echo second >> /tmp/r.txt")
        g.key("f5")
        vm.expect(r"nb: open /tmp/r.txt lines=3 bytes=17", timeout=10)
        g.type("X")
        vm.expect(r"nb: title Notebook - \*r.txt", timeout=10)
        g.key("f5")
        vm.expect(r"nb: dialog reload", timeout=10)
        time.sleep(0.8)
        g.key("esc")                                     # Cancel: the page keeps its text
        time.sleep(0.8)
        g.key("s", ctrl=True)
        vm.expect(r"nb: saved /tmp/r.txt", timeout=10)
        expect_file(g, "/tmp/r.txt", b"Xtwo-lines\nsecond\n", "Cancel kept the typed text")
        g.type("Y")
        g.key("f5")
        vm.expect(r"nb: dialog reload", timeout=10)
        time.sleep(0.8)
        g.key("ret")                                     # Reload
        vm.expect(r"nb: open /tmp/r.txt lines=3 bytes=18", timeout=10)
        vm.expect(r"nb: title Notebook - r.txt", timeout=10)


@test
def notebook_paths_and_windows():
    """Relative paths, spaces and dots in paths, New Window starts a second editor."""
    with session("t-notebook-paths") as (vm, g):
        g.sh("mkdir -p '/tmp/dir with space'")
        g.sh("echo spaced > '/tmp/dir with space/my notes.txt'")
        g.sh("echo rel > /tmp/rel.txt")
        g.sh("cd /tmp")
        vm.send("notebook rel.txt &\n")
        vm.expect(r"nb: open /tmp/rel.txt lines=2 bytes=4", timeout=20)
        time.sleep(1.0)
        g.key("n", ctrl=True, shift=True)                # New Window
        vm.expect(r"nb: start", timeout=20)
        time.sleep(1.5)
        pids = g.sh("pidof notebook").split()
        assert len([p for p in pids if p.isdigit()]) == 2, f"expected two notebook processes, pidof said {pids}"
        g.key("q", ctrl=True)                            # the new, empty window
        vm.expect(r"nb: quit", timeout=10)
        time.sleep(0.8)
        g.key("q", ctrl=True)                            # the first one
        vm.expect(r"nb: quit", timeout=10)
        vm.send('notebook "/tmp/dir with space/../dir with space/./my notes.txt" &\n')
        vm.expect(r"nb: open /tmp/dir with space/my notes.txt lines=2 bytes=7", timeout=20)
        vm.expect(r"nb: title Notebook - my notes.txt", timeout=10)


@test
def notebook_unsaved_close():
    """Closing a modified page asks Save / Don't save / Cancel; Esc keeps editing, Don't save leaves the file alone."""
    with session("t-notebook-close") as (vm, g):
        make_demo(g)
        original = md5_of(g, "/tmp/demo.c")
        launch(vm, "/tmp/demo.c")
        g.type("X")
        vm.expect(r"nb: title Notebook - \*demo.c", timeout=10)
        g.key("f4", alt=True)
        vm.expect(r"nb: dialog Unsaved changes", timeout=10)
        shot = g.screen()
        assert g.count_pixels((277 + 60, 314 + 80, 300, 30), (0xE4, 0xDE, 0xCB), tol=4, shot=shot) > 200, "no dialog buttons on screen"
        g.shot("unsaved")
        g.key("esc")                                     # Cancel: the page stays
        time.sleep(0.8)
        g.type("Y")
        vm.expect(r"nb: pos 1 3 mod=1", timeout=10)
        g.key("f4", alt=True)
        vm.expect(r"nb: dialog Unsaved changes", timeout=10)
        time.sleep(0.5)
        g.click(585, 413)                                # Don't save
        vm.expect(r"nb: quit", timeout=10)
        assert md5_of(g, "/tmp/demo.c") == original, "Don't save must leave the file untouched"

        launch(vm, "/tmp/demo.c")
        g.type("Z")
        g.key("f4", alt=True)
        vm.expect(r"nb: dialog Unsaved changes", timeout=10)
        time.sleep(0.5)
        g.key("ret")                                     # the default button is Save
        vm.expect(r"nb: saved /tmp/demo.c", timeout=10)
        vm.expect(r"nb: quit", timeout=10)
        expect_file(g, "/tmp/demo.c", b"Z" + DEMO_BYTES, "Save from the unsaved-changes dialog")


@test
def notebook_big_file():
    """A 100,000 line file opens and scrolls without slow frames; Ctrl+End, PageDown and typing stay quick."""
    with session("t-notebook-big") as (vm, g):
        g.sh("seq 1 100000 > /tmp/big.txt")
        t0 = time.time()
        vm.send("notebook /tmp/big.txt &\n")
        vm.expect(r"nb: open /tmp/big.txt lines=100001", timeout=60)
        vm.expect(r"nb: pos 1 1 ", timeout=30)
        assert time.time() - t0 < 25, "opening took too long"
        time.sleep(1)
        t1 = time.time()
        g.key("end", ctrl=True)
        vm.expect(r"nb: pos 100001 1 ", timeout=15)
        assert time.time() - t1 < 10, "Ctrl+End was slow"
        g.shot("end")
        # the last page shows the last line numbers: the gutter must be 6 digits wide and the status bar says so
        g.key("home", ctrl=True)
        vm.expect(r"nb: pos 1 1 ", timeout=15)
        for _ in range(5):
            g.key("pgdn")
        vm.expect(r"nb: pos 141 1 ", timeout=15)
        for _ in range(3):                               # the mouse wheel scrolls three rows per notch (the PS/2 driver of the kernel
            vm.mouse_button("wheel-down", True)          # reports the direction the wrong way round, so accept either direction)
            vm.mouse_button("wheel-down", False)
        vm.expect(r"nb: top (132|150)\b", timeout=15)
        g.drag(44 + 711, 56 + 36, 44 + 711, 56 + 236)    # the scroll bar thumb, at the top of its track
        vm.expect(r"nb: top \d{5}\b", timeout=15)
        g.key("end", ctrl=True)
        g.type("tail")
        vm.expect(r"nb: pos 100001 5 mod=1", timeout=15)
        g.key("s", ctrl=True)
        vm.expect(r"nb: saved /tmp/big.txt", timeout=20)
        out = g.sh("wc -l /tmp/big.txt")
        assert "100000" in out, out
        slow = [int(x) for x in re.findall(r"nb: slow-frame (\d+) ms", vm.text())]
        assert not slow or max(slow) < 1500, f"slow frames: {slow}"


@test
def notebook_bad_input():
    """Binary files are refused, folders and read-only files are handled, odd inputs never crash the editor."""
    with session("t-notebook-bad") as (vm, g):
        g.sh("printf 'ABC\\000DEF\\001\\002' > /tmp/bin.dat")
        g.sh(": > /tmp/empty.txt")
        g.sh("echo 'read only text' > /tmp/ro.txt")
        g.sh("chmod 444 /tmp/ro.txt")
        g.sh("yes abcdefghij | head -c 1000000 | tr -d '\\n' > /tmp/oneline.txt")
        launch(vm, "/tmp/bin.dat", ready=r"nb: refused binary /tmp/bin.dat")
        vm.expect(r"nb: dialog Not a text file", timeout=10)
        time.sleep(0.8)
        g.shot("binary")
        g.key("esc")
        g.type("typed in the untitled page")
        vm.expect(r"nb: title Notebook - \*untitled", timeout=10)
        g.key("q", ctrl=True)                            # modified: asks; Don't save
        vm.expect(r"nb: dialog Unsaved changes", timeout=10)
        time.sleep(0.5)
        g.click(585, 413)
        vm.expect(r"nb: quit", timeout=10)

        launch(vm, "/tmp", ready=r"nb: error This is a folder")
        vm.expect(r"nb: dialog Cannot open folder", timeout=10)
        time.sleep(0.8)
        g.key("esc")
        g.key("q", ctrl=True)
        vm.expect(r"nb: quit", timeout=10)

        launch(vm, "/tmp/ro.txt")
        g.type("x")
        vm.expect(r"nb: msg Read-only file", timeout=10)
        g.key("s", ctrl=True)                            # saving a read-only file offers Save As
        vm.expect(r"nb: dialog save-as", timeout=10)
        time.sleep(0.8)
        g.key("esc")                                     # the first Esc only leaves the file name field of the stock dialog
        time.sleep(0.4)
        g.key("esc")
        time.sleep(0.5)
        assert md5_of(g, "/tmp/ro.txt") == hashlib.md5(b"read only text\n").hexdigest()
        g.key("q", ctrl=True)
        vm.expect(r"nb: quit", timeout=10)

        launch(vm, "/tmp/empty.txt")
        g.key("end", ctrl=True)
        g.key("backspace")
        g.key("q", ctrl=True)
        vm.expect(r"nb: quit", timeout=10)

        g.sh("echo secret > /tmp/noread.txt")
        g.sh("chmod 000 /tmp/noread.txt")
        launch(vm, "/tmp/noread.txt", ready=r"nb: error Cannot read:")
        vm.expect(r"nb: dialog Cannot open file", timeout=10)
        time.sleep(0.8)
        g.key("esc")
        g.key("q", ctrl=True)
        vm.expect(r"nb: quit", timeout=10)

        g.sh("mkdir /tmp/rodir")
        g.sh("chmod 555 /tmp/rodir")
        launch(vm, "/tmp/rodir/new.txt")
        g.type("cannot be saved")
        g.key("s", ctrl=True)
        vm.expect(r"nb: error The page could not be written", timeout=10)
        vm.expect(r"nb: dialog Cannot save", timeout=10)
        time.sleep(0.8)
        g.key("esc")
        time.sleep(0.5)
        g.key("q", ctrl=True)                            # still modified: asks, Don't save
        vm.expect(r"nb: dialog Unsaved changes", timeout=10)
        time.sleep(0.8)
        g.click(585, 413)
        vm.expect(r"nb: quit", timeout=10)

        t0 = time.time()
        vm.send("notebook /tmp/oneline.txt &\n")
        vm.expect(r"nb: pos 1 1 ", timeout=60)
        time.sleep(1)
        g.key("end")
        vm.expect(r"nb: pos 1 9\d{5} mod=0", timeout=30)
        g.type("!")
        vm.expect(r"nb: pos 1 9\d{5} mod=1", timeout=30)
        assert "BOUNDARY VIOLATION" not in vm.text(), "the editor crashed on a 1 MB line"
        slow = [int(x) for x in re.findall(r"nb: slow-frame (\d+) ms", vm.text())]
        assert not slow or max(slow) < 3000, f"slow frames on the long line: {slow}"
        g.key("q", ctrl=True)
        vm.expect(r"nb: dialog Unsaved changes", timeout=10)
        time.sleep(0.5)
        g.click(585, 413)
        vm.expect(r"nb: quit", timeout=10)


@test
def notebook_context_menu():
    """A right click opens the context menu next to the pointer; keys or the mouse choose; Esc closes it."""
    with session("t-notebook-ctx") as (vm, g):
        make_demo(g)
        launch(vm, "/tmp/demo.c")
        for attempt in range(2):                         # a loaded machine can drop mouse packets: re-home the pointer once
            g.right_click(44 + 200, 56 + 100)
            try:
                vm.expect(r"nb: context menu", timeout=8)
                break
            except TimeoutError:
                if attempt:
                    raise
                g.home()
        time.sleep(0.5)
        g.shot("ctx")
        assert g.pixel_near(44 + 230, 56 + 106, (0xEC, 0xE6, 0xD6), tol=10), "no menu on screen"
        g.key("esc")
        time.sleep(0.5)
        assert not g.pixel_near(44 + 230, 56 + 106, (0xEC, 0xE6, 0xD6), tol=10), "the menu is still there after Esc"
        g.right_click(44 + 200, 56 + 100)
        vm.expect(r"nb: context menu", timeout=10)
        time.sleep(0.4)
        g.key("down")                                    # Cut, Copy and the history are disabled: Paste first, then Select All
        g.key("down")
        g.key("ret")
        vm.expect(r"nb: pos 13 1 mod=0 lines=13 sel=280", timeout=10)


@test
def notebook_menu_keys():
    """Menus work from the keyboard: Alt+letter opens, arrows move, Enter picks, Esc closes, Right switches menus."""
    with session("t-notebook-menukeys") as (vm, g):
        make_demo(g)
        launch(vm, "/tmp/demo.c")
        g.key("e", alt=True)
        vm.expect(r"nb: menu 2 item -1", timeout=10)
        g.key("down")                                    # Undo, Redo, Cut and Copy are disabled: the first stop is Paste
        vm.expect(r"nb: menu 2 item 5", timeout=10)
        g.key("down")
        vm.expect(r"nb: menu 2 item 6", timeout=10)      # Select All
        g.key("ret")
        vm.expect(r"nb: pos 13 1 mod=0 lines=13 sel=280", timeout=10)
        g.key("f", alt=True)
        vm.expect(r"nb: menu 1 item -1", timeout=10)
        g.key("right")
        vm.expect(r"nb: menu 2 item -1", timeout=10)
        g.key("left")
        g.key("left")
        vm.expect(r"nb: menu 4 item -1", timeout=10)     # wraps around to Help
        g.key("up")
        vm.expect(r"nb: menu 4 item 1", timeout=10)      # About (the last entry)
        g.key("esc")
        vm.expect(r"nb: menu 0 item -1", timeout=10)
        g.key("v", alt=True)                             # View: Line Numbers is first; pick Word Wrap
        g.key("down")
        g.key("down")
        g.key("ret")
        time.sleep(0.8)
        assert "word_wrap=1" in g.sh("cat ~/.config/notebook.conf")


@test
def notebook_wrap_and_mouse():
    """Word wrap moves the caret by visual rows; double click selects a word, triple click a line, the gutter a whole line."""
    with session("t-notebook-wrap") as (vm, g):
        words = "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua."
        g.sh(f"echo '{words} {words}' > /tmp/wrap.txt")
        g.sh("echo 'second line' >> /tmp/wrap.txt")
        launch(vm, "/tmp/wrap.txt")
        g.click(167, 68)                                 # View
        time.sleep(0.4)
        g.click(170, 116)                                # Word Wrap
        time.sleep(0.8)
        g.click(*TEXT_CLICK)
        g.key("home", ctrl=True)
        vm.expect(r"nb: pos 1 1 mod=0 lines=3 sel=0", timeout=10)
        g.key("down")                                    # one visual row down, still inside line 1
        m = vm.expect(r"nb: pos 1 (\d+) mod=0", timeout=10)
        assert 40 < int(m.group(1)) < 120, f"Down moved to column {m.group(1)}"
        g.key("home", ctrl=True)
        g.double_click(44 + 90 + 8 * 7, 56 + 36)         # inside "dolor"
        vm.expect(r"nb: pos 1 \d+ mod=0 lines=3 sel=5", timeout=10)
        g.click(44 + 600, 56 + 28 + 16 * 8)              # empty area below the text: caret to the end
        g.click(44 + 300, 56 + 36, count=3)              # triple click: the whole first line (one line = 255 chars + newline)
        vm.expect(r"nb: pos \d+ \d+ mod=0 lines=3 sel=\d{3}", timeout=10)
        g.shot("wrap")
        g.click(44 + 25, 56 + 28 + 16 * 4 + 8)           # the number of line 2 in the gutter (line 1 takes four rows)
        vm.expect(r"nb: pos 3 1 mod=0 lines=3 sel=12", timeout=10)      # "second line" and its line break


@test
def notebook_gui_fuzz():
    """A hundred and twenty random keys, clicks, drags, wheel turns and View/Edit menu picks: the editor survives and still saves."""
    import os
    import random
    rng = random.Random(int(os.environ.get("NB_FUZZ_SEED", "1990")))
    with session("t-notebook-fuzz") as (vm, g):
        make_demo(g)
        g.sh("echo 'a very long line to give the horizontal scroll bar something to do, and word wrap too: " + "word " * 40 + "' >> /tmp/demo.c")
        launch(vm, "/tmp/demo.c")
        keys = ["left", "right", "up", "down", "home", "end", "pgup", "pgdn", "backspace", "delete", "ret", "tab", "spc"]
        for step in range(120):
            r = rng.random()
            if r < 0.30:
                g.type("".join(rng.choice("abcdefgh {}();\"'") for _ in range(rng.randint(1, 4))))
            elif r < 0.55:
                g.key(rng.choice(keys), shift=rng.random() < 0.3, ctrl=rng.random() < 0.15)
            elif r < 0.62:
                g.key(rng.choice("zyaxcvdk"), ctrl=True, shift=rng.random() < 0.3)
            elif r < 0.78:
                g.click(44 + rng.randint(0, 715), 56 + rng.randint(26, 495), count=rng.choice([1, 1, 2, 3]))
            elif r < 0.86:
                x, y = 44 + rng.randint(40, 700), 56 + rng.randint(30, 480)
                g.drag(x, y, x + rng.randint(-150, 150), y + rng.randint(-120, 120), steps=4)
            elif r < 0.92:
                vm.mouse_button(rng.choice(["wheel-up", "wheel-down"]), True)
                vm.mouse_button("wheel-down", False)
            elif r < 0.97:
                g.click(rng.choice([118, 167]), 68)           # Edit or View menu
                time.sleep(0.2)
                g.click(100 if rng.random() < 0.5 else 170, 56 + rng.choice([38, 60, 82, 104, 126, 150, 172, 194]))
                time.sleep(0.2)
            else:
                g.key("esc")
        g.key("esc")
        g.key("esc")
        time.sleep(0.5)
        g.shot("after")
        assert "BOUNDARY VIOLATION" not in vm.text(), "the editor crashed:\n" + vm.text()[-1500:]
        g.key("s", ctrl=True)
        vm.expect(r"nb: (saved|dialog save-as)", timeout=20)                 # a View menu pick may have switched things around
        slow = [int(x) for x in re.findall(r"nb: slow-frame (\d+) ms", vm.text())]
        assert not slow or max(slow) < 3000, f"slow frames: {slow}"
        out = g.sh("pidof notebook")
        assert any(p.isdigit() for p in out.split()), "the editor is gone: " + out


@test
def notebook_prefs_and_recent():
    """View toggles and the recent files list are stored in ~/.config, and syntax colours switch on and off."""
    with session("t-notebook-prefs") as (vm, g):
        make_demo(g)
        g.sh("echo plain > /tmp/other.txt")
        launch(vm, "/tmp/demo.c")
        time.sleep(0.5)
        shot = g.screen()
        kw = g.count_pixels((44 + 50, 56 + 26, 400, 16 * 12), (0xA3, 0x12, 0x2A), tol=10, shot=shot)
        ty = g.count_pixels((44 + 50, 56 + 26, 400, 16 * 12), (0x2A, 0x55, 0xA8), tol=10, shot=shot)
        assert kw > 30 and ty > 30, f"no syntax colours for C (keywords {kw}, types {ty})"
        g.click(167, 68)                                 # View
        time.sleep(0.4)
        g.click(170, 160)                                # Syntax Colours (off)
        time.sleep(0.8)
        g.shot("nocolours")
        shot = g.screen()
        kw2 = g.count_pixels((44 + 50, 56 + 26, 400, 16 * 12), (0xA3, 0x12, 0x2A), tol=10, shot=shot)
        assert kw2 == 0, f"colours still on after switching them off ({kw2})"
        g.click(167, 68)
        time.sleep(0.4)
        g.click(170, 116)                                # Word Wrap (on)
        time.sleep(0.5)
        g.key("o", ctrl=True)                            # Open...: type a path in the file name field
        vm.expect(r"nb: dialog open", timeout=10)
        time.sleep(0.8)
        g.type("/tmp/other.txt")
        g.key("ret")
        vm.expect(r"nb: open /tmp/other.txt", timeout=10)
        conf = g.sh("cat ~/.config/notebook.conf")
        assert "word_wrap=1" in conf and "highlight=0" in conf, conf
        recent = g.sh("cat ~/.config/notebook.recent")
        assert "/tmp/other.txt" in recent and "/tmp/demo.c" in recent, recent
        assert recent.index("/tmp/other.txt") < recent.index("/tmp/demo.c"), "most recent first: " + recent
        g.click(213, 68)                                 # Help
        time.sleep(0.4)
        g.click(220, 116)                                # About Patchouli's Notebook
        vm.expect(r"nb: dialog about", timeout=10)
        time.sleep(0.6)
        g.shot("about")
        g.key("esc")
        g.key("f1")
        vm.expect(r"nb: dialog shortcuts", timeout=10)
        time.sleep(0.6)
        g.shot("shortcuts")
        g.key("esc")
