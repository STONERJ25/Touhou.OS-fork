"""vi on the screen: the status line and ruler, mode indicators, numbers, scrolling, wrapped and very long lines, messages, the
hit-enter prompt, and keys typed over the serial line one byte at a time (Esc, arrows in both forms, unknown sequences), plus the
exit paths (the terminal must be handed back every time). The screen is the serial output replayed through tests/vt.py; vi runs
with LINES=16 COLUMNS=70, so the text area is rows 0-13, the status line row 14 and the message row 15."""
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from vilib import Guest, K, Qemu, check_eq, shq  # noqa: E402
from harness import test  # noqa: E402

TEXT_ROWS = 14
RULER = re.compile(r"\s(\d+),(\S+)\s+(All|Top|Bot|\d+%)\s*$")


def numbered(n, prefix="line "):
    return "".join(f"{prefix}{i}\n" for i in range(1, n + 1))


def ruler(row):
    """(line, column text, position) from a status line, or None."""
    m = RULER.search(row)
    return (int(m.group(1)), m.group(2), m.group(3)) if m else None


def pct(top, total, height=TEXT_ROWS):
    """Vim's position text for a window whose first line is `top`: Top, Bot, All or the percentage above."""
    above = top - 1
    below = max(0, total - (top + height - 1))
    if above == 0 and below == 0:
        return "All"
    if above == 0:
        return "Top"
    if below == 0:
        return "Bot"
    return f"{above * 100 // (above + below)}%"


class Session:
    """vi started on a file; keys() sends keys and waits for the screen to reach a state, then for the output to stop."""

    def __init__(self, g, content, args="/tmp/x", env=""):
        self.g = g
        g.run(f"printf {shq(content)} > /tmp/x")
        self.s = g.start(args, env=env)

    def screen(self):
        return self.g.screen(self.s)

    def keys(self, text, until=None, timeout=40):
        self.g.type(K(text), wait=0.0)
        if until is not None:
            self.g.wait_screen(self.s, until, timeout=timeout)
        return self.settle()

    def settle(self, quiet=0.3, timeout=40):
        """The screen once the output has stopped for a moment."""
        end = time.time() + timeout
        last, since = None, time.time()
        while time.time() < end:
            size = self.g.mark()
            if size != last:
                last, since = size, time.time()
            elif time.time() - since >= quiet:
                break
            time.sleep(0.05)
        return self.screen()

    def quit(self):
        self.g.type(K("<Esc><Esc>:q!<CR>"), wait=0.1)
        self.g.wait_left(self.s)
        self.g.sync()


@test
def vi_screen_basic():
    """vi screen: text, filler rows, status line and ruler, cursor, mode indicators, numbers, list mode, messages."""
    with Qemu(cmdline="text", name="t-vi-screen-basic") as vm:
        g = Guest(vm)
        ss = Session(g, "one\ntwo\n\tthree\n")
        vt = ss.settle()
        rows = vt.rows_text()
        check_eq(rows[0:3], ["one", "two", "        three"], "text rows")
        check_eq(rows[3:14], ["~"] * 11, "filler rows")
        assert re.fullmatch(r" NORMAL  /tmp/x\s+1,1\s+All", rows[14]), rows[14]
        check_eq(rows[15], '"/tmp/x" 3L, 15B', "file message")
        check_eq((vt.y, vt.x), (0, 0), "cursor")
        attrs = vt.row_attrs(14)
        assert all(a & 2 for a in attrs[:60]), "the status line is in reverse video"

        vt = ss.keys("jl", until=lambda r: "2,2" in r[14])
        check_eq((vt.y, vt.x), (1, 1), "cursor after jl")
        check_eq(ruler(vt.rows_text()[14]), (2, "2", "All"))
        vt = ss.keys("j", until=lambda r: "3,1-8" in r[14])
        check_eq((vt.y, vt.x), (2, 7), "on a tab the cursor is at the end of it")
        vt = ss.keys("l", until=lambda r: "3,2-9" in r[14])
        check_eq((vt.y, vt.x), (2, 8), "after the tab")

        # --- modes
        vt = ss.keys("ggi", until=lambda r: "-- INSERT --" in r[15])
        assert " INSERT  /tmp/x" in vt.rows_text()[14], vt.rows_text()[14]
        vt = ss.keys("X", until=lambda r: "[Modified]" in r[14])
        check_eq(vt.rows_text()[0], "Xone")
        vt = ss.keys("<Esc>", until=lambda r: r[15] == "")
        assert vt.rows_text()[14].startswith(" NORMAL  /tmp/x [Modified]"), vt.rows_text()[14]
        vt = ss.keys("R", until=lambda r: "-- REPLACE --" in r[15])
        assert " REPLACE " in vt.rows_text()[14], vt.rows_text()[14]
        vt = ss.keys("<Esc>v", until=lambda r: "-- VISUAL --" in r[15])
        vt = ss.keys("<Esc>V", until=lambda r: "-- VISUAL LINE --" in r[15])
        vt = ss.keys("<Esc><C-v>", until=lambda r: "-- VISUAL BLOCK --" in r[15])
        vt = ss.keys("<Esc>qq", until=lambda r: "recording @q" in r[14] or "recording @q" in r[15])
        vt = ss.keys("q", until=lambda r: "recording" not in r[14] and "recording" not in r[15])

        # --- options that change the picture
        vt = ss.keys(":set nu<CR>", until=lambda r: r[1].startswith("  2 "))
        check_eq(vt.rows_text()[0:3], ["  1 Xone", "  2 two", "  3         three"], "number")
        vt = ss.keys(":set nonu<CR>:set rnu<CR>", until=lambda r: r[0].startswith("  0 "))
        check_eq(vt.rows_text()[0:3], ["  0 Xone", "  1 two", "  2         three"], "relativenumber")
        vt = ss.keys(":set nornu<CR>:set list<CR>", until=lambda r: "Xone$" in r[0])
        check_eq(vt.rows_text()[0:3], ["Xone$", "two$", "^Ithree$"], "list")
        vt = ss.keys(":set nolist<CR>:set ts=4<CR>", until=lambda r: r[2].startswith("    three"))
        check_eq(vt.rows_text()[2], "    three", "tabstop")

        # --- messages
        vt = ss.keys(":foo<CR>", until=lambda r: "E492" in r[15])
        check_eq(vt.rows_text()[15], "E492: Not an editor command: foo")
        assert vt.row_attrs(15)[0] & 1, "errors are bold"
        vt = ss.keys("/zzz<CR>", until=lambda r: "E486" in r[15])
        check_eq(vt.rows_text()[15], "E486: Pattern not found: zzz")
        vt = ss.keys(":q<CR>", until=lambda r: "E37" in r[15])
        check_eq(vt.rows_text()[15], "E37: No write since last change (add ! to override)")
        vt = ss.keys("<C-g>", until=lambda r: "lines" in r[15])
        assert re.fullmatch(r'"/tmp/x" \[Modified\] 3 lines --\d+%--', vt.rows_text()[15]), vt.rows_text()[15]
        vt = ss.keys("u", until=lambda r: "before #1" in r[15])
        assert re.fullmatch(r"1 change; before #1  \d+ seconds? ago", vt.rows_text()[15]), vt.rows_text()[15]
        check_eq(vt.rows_text()[0], "one")
        assert "[Modified]" not in vt.rows_text()[14], "back at the text that was read: not modified"
        vt = ss.keys("<C-r>", until=lambda r: "after #1" in r[15])
        assert re.fullmatch(r"1 change; after #1  \d+ seconds? ago", vt.rows_text()[15]), vt.rows_text()[15]
        vt = ss.keys("uu", until=lambda r: "oldest" in r[15])
        check_eq(vt.rows_text()[15], "Already at oldest change")
        vt = ss.keys("/two<CR>", until=lambda r: "/two" in r[15])
        vt = ss.keys("n", until=lambda r: "search hit BOTTOM" in r[15])
        check_eq(vt.rows_text()[15], "search hit BOTTOM, continuing at TOP")
        vt = ss.keys("<C-c>", until=lambda r: "Type  :qa" in r[15])
        check_eq(vt.rows_text()[15], "Type  :qa  and press <Enter> to exit Vim")

        # --- :reg shows a list over the text and waits
        vt = ss.keys('"ayy:reg<CR>', until=lambda r: any("Press ENTER" in x for x in r))
        assert any(x.startswith("  l  \"a   two") for x in vt.rows_text()), vt.text()
        vt = ss.keys("<CR>", until=lambda r: r[0] == "one" and r[14].startswith(" NORMAL"))
        check_eq(vt.rows_text()[15], "")
        ss.quit()


@test
def vi_screen_scroll():
    """vi screen: Ctrl-E Ctrl-Y Ctrl-D Ctrl-U Ctrl-F Ctrl-B, zt zz zb, G and gg, H M L, and the ruler (Top, Bot, percent)."""
    with Qemu(cmdline="text", name="t-vi-screen-scroll") as vm:
        g = Guest(vm)
        ss = Session(g, numbered(100))
        vt = ss.settle()
        total = 100

        def view(keys, top, cur, what):
            """Types keys and checks the window: its first line, the cursor line and row, and the ruler."""
            vt = ss.keys(keys, until=lambda r: r[0] == f"line {top}" and (ruler(r[14]) or (0,))[0] == cur)
            rows = vt.rows_text()
            check_eq(rows[0:TEXT_ROWS], [f"line {i}" for i in range(top, top + TEXT_ROWS)], f"{what}: the lines shown")
            check_eq(ruler(rows[14]), (cur, "1", pct(top, total)), f"{what}: ruler")
            check_eq((vt.y, vt.x), (cur - top, 0), f"{what}: cursor")

        view("", 1, 1, "start")
        view("<C-e>", 2, 2, "ctrl-e moves the cursor along when it would leave the window")
        view("<C-y>", 1, 2, "ctrl-y")
        view("3<C-e>", 4, 4, "3 ctrl-e")
        view("<C-y><C-y><C-y>", 1, 4, "3 ctrl-y")
        view("<C-d>", 8, 11, "ctrl-d scrolls half a window (7 lines) and moves the cursor as far")
        view("<C-u>", 1, 4, "ctrl-u")
        view("<C-f>", 13, 13, "ctrl-f: two lines of the old page stay")
        view("<C-f>", 25, 25, "ctrl-f again")
        view("<C-b>", 13, 26, "ctrl-b puts the cursor on the last row")
        view("50Gzt", 50, 50, "zt")
        view("zz", 44, 50, "zz")
        view("zb", 37, 50, "zb")
        view("z<CR>", 50, 50, "z<CR>")
        view("z.", 44, 50, "z.")
        view("z-", 37, 50, "z-")
        view("gg", 1, 1, "gg")
        vt = ss.keys("G", until=lambda r: r[0] == "line 87")
        check_eq(vt.rows_text()[13], "line 100")
        check_eq(ruler(vt.rows_text()[14]), (100, "1", "Bot"), "G")
        view("30G", 24, 30, "a jump far away centres the line")
        view("H", 24, 24, "H")
        view("L", 24, 37, "L")
        view("M", 24, 30, "M")
        view("gg15j", 3, 16, "a short way below the window: scroll just enough")
        view("5k", 3, 11, "5k")
        view("20<C-e>", 23, 23, "20 ctrl-e")
        view("gg", 1, 1, "gg again")
        # a count on ctrl-d sets 'scroll'
        view("4<C-d>", 5, 5, "4 ctrl-d: scroll=4")
        view("<C-d>", 9, 9, "ctrl-d keeps that count")
        ss.quit()


@test
def vi_screen_long_lines():
    """vi screen: wrapped lines, :set nowrap, '@' rows, and a line taller than the window (the part with the cursor is shown)."""
    with Qemu(cmdline="text", name="t-vi-screen-long") as vm:
        g = Guest(vm)
        long200 = "".join(chr(ord("a") + i % 26) for i in range(200))
        ss = Session(g, f"first\n{long200}\nlast\n")
        vt = ss.settle()
        rows = vt.rows_text()
        check_eq(rows[0:6], ["first", long200[:70], long200[70:140], long200[140:], "last", "~"], "a wrapped line")
        vt = ss.keys("j$", until=lambda r: "2,200" in r[14])
        check_eq((vt.y, vt.x), (3, 59), "the cursor at the end of the wrapped line")
        vt = ss.keys(":set nowrap<CR>", until=lambda r: r[1] == long200[164:])
        rows = vt.rows_text()
        check_eq(rows[0:4], ["", long200[164:], "", "~"], "nowrap: the window slides so that the cursor is in the middle")
        check_eq((vt.y, vt.x), (1, 35), "the cursor column with nowrap")
        vt = ss.keys("0", until=lambda r: r[1] == long200[:70])
        check_eq(vt.rows_text()[0:3], ["first", long200[:70], "last"], "nowrap: back at the left")
        vt = ss.keys(":set wrap<CR>", until=lambda r: r[2] == long200[70:140])

        # --- a line taller than the window (22 rows of 70 columns)
        huge = "".join(chr(ord("a") + i % 26) for i in range(1500))
        ss.quit()
        ss = Session(g, f"first\n{huge}\nlast\n")
        vt = ss.settle()
        check_eq(vt.rows_text()[0], "first")
        check_eq(vt.rows_text()[1:TEXT_ROWS], ["@"] * 13, "the first screen: the tall line does not fit under the first one")
        vt = ss.keys("j", until=lambda r: "2,1" in r[14])
        rows = vt.rows_text()
        check_eq(rows[0:TEXT_ROWS], [huge[i * 70:(i + 1) * 70] for i in range(TEXT_ROWS)], "the first rows of the long line")
        vt = ss.keys("$", until=lambda r: "2,1500" in r[14])
        rows = vt.rows_text()
        last_rows = [huge[i * 70:(i + 1) * 70] for i in range(22)]
        check_eq(rows[0:TEXT_ROWS], last_rows[22 - TEXT_ROWS:], "the last rows of the long line")
        check_eq(vt.y, 13, "the cursor on the last row")
        vt = ss.keys("0", until=lambda r: r[0] == huge[:70])
        check_eq(vt.y, 0)
        vt = ss.keys("1000|", until=lambda r: "2,1000" in r[14])
        want = huge[(999 // 70) * 70:(999 // 70) * 70 + 70]
        assert want in vt.rows_text()[:TEXT_ROWS], vt.text()
        check_eq(vt.rows_text()[vt.y], want, "the cursor is in the row that has column 1000")
        vt = ss.keys("j", until=lambda r: "3,4" in r[14])
        assert "last" in vt.rows_text()[:TEXT_ROWS], vt.text()
        vt = ss.keys("k", until=lambda r: "2,1000" in r[14])
        assert want in vt.rows_text()[:TEXT_ROWS], vt.text()
        ss.quit()

        # --- a line that does not fit below the others shows '@'
        ss = Session(g, numbered(10) + "y" * 900 + "\nend\n")
        vt = ss.settle()
        rows = vt.rows_text()
        check_eq(rows[0:10], [f"line {i}" for i in range(1, 11)], "the lines that fit")
        check_eq(rows[10:14], ["@"] * 4, "rows of a line that does not fit")
        vt = ss.keys("G", until=lambda r: "end" in r[0:14])
        ss.quit()


@test
def vi_screen_keys_serial():
    """vi: Esc alone, arrows in CSI and SS3 form split over several writes, Home/End/PgUp/PgDn/Delete/Insert, unknown sequences."""
    with Qemu(cmdline="text", name="t-vi-screen-keys") as vm:
        g = Guest(vm)
        ss = Session(g, "".join(f"row {i}\n" for i in range(1, 31)))
        g.wait_screen(ss.s, lambda rows: rows[0] == "row 1")

        def send(parts, gap=0.03):
            for p in parts:
                vm.send(p)
                time.sleep(gap)

        def at(line, col=None):
            """Waits until the ruler shows line (and column)."""
            def ok(r):
                m = RULER.search(r[14])
                return bool(m) and int(m.group(1)) == line and (col is None or m.group(2) == str(col))
            return g.wait_screen(ss.s, ok)

        # a lone Esc is recognised after a short wait; typed text goes in until then
        send(["i", "abc"])
        g.wait_screen(ss.s, lambda r: "-- INSERT --" in r[15] and r[0] == "abcrow 1")
        send(["\x1b"])
        g.wait_screen(ss.s, lambda r: r[15] == "" and r[14].startswith(" NORMAL") and "1,3" in r[14])
        # arrow keys: the bytes arrive one at a time
        send(["\x1b", "[", "B"])
        at(2, 3)
        send(["\x1b", "[", "A"])
        at(1, 3)
        # SS3 form
        send(["\x1bOB", "\x1bOB"])
        at(3, 3)
        send(["\x1b", "O", "A"])
        at(2, 3)
        send(["\x1bOC", "\x1b[C"])
        at(2, 5)
        send(["\x1bOD"])
        at(2, 4)
        send(["\x1b[F"])
        at(2, 5)
        send(["\x1b[H"])
        at(2, 1)
        send(["\x1bOF"])
        at(2, 5)
        send(["\x1bOH"])
        at(2, 1)
        send(["\x1b[4~"])
        at(2, 5)
        send(["\x1b[1~"])
        at(2, 1)
        send(["\x1b[6~"])                                   # PgDn: a page forward
        g.wait_screen(ss.s, lambda r: r[0] == "row 13" and (ruler(r[14]) or (0,))[0] == 13)
        send(["\x1b[5~"])                                   # PgUp: a page back
        g.wait_screen(ss.s, lambda r: r[0] == "abcrow 1" and (ruler(r[14]) or (0,))[0] == 14)
        send(["\x1b[3~"])                                   # Delete removes the character under the cursor
        g.wait_screen(ss.s, lambda r: r[13] == "ow 14")
        # Shift or Ctrl with Left and Right move by words: "abcrow 1" is a word of six letters and then the "1"
        send(["gg"])
        at(1, 1)
        send(["\x1b[1;5C"])                                 # Ctrl-Right
        at(1, 8)
        send(["\x1b[1;5D"])                                 # Ctrl-Left
        at(1, 1)
        send(["\x1b[1;2C"])                                 # Shift-Right
        at(1, 8)
        send(["\x1b[1;2D"])                                 # Shift-Left
        at(1, 1)
        # unknown sequences are swallowed, in normal mode and in insert mode
        send(["\x1b[29~", "\x1bOP", "\x1b[15~", "\x1b[20;2~", "\x1b[99;5~"])
        send(["i", "\x1b[29~", "\x1bOQ", "\x1b[15;5~", "x", "\x1b"])
        g.wait_screen(ss.s, lambda r: r[15] == "" and r[14].startswith(" NORMAL") and r[0] == "xabcrow 1")
        at(1, 1)
        # Esc followed by an unrelated key is two keys: leave insert mode, then the key
        send(["i", "z"])
        g.wait_screen(ss.s, lambda r: r[0] == "zxabcrow 1")
        send(["\x1bx"])
        g.wait_screen(ss.s, lambda r: r[0] == "xabcrow 1" and r[14].startswith(" NORMAL"))
        ss.keys(":w<CR>", until=lambda r: "written" in r[15])
        ss.quit()
        want = "xabcrow 1\n" + "".join(f"row {i}\n" for i in range(2, 14)) + "ow 14\n" + "".join(f"row {i}\n" for i in range(15, 31))
        check_eq(g.out("cat /tmp/x"), want, "the file after the keys")


@test
def vi_screen_interrupt():
    """vi: Ctrl-C stops a macro that calls itself; the editor stays usable afterwards (written, not yet run: see docs/VI.md)."""
    with Qemu(cmdline="text", name="t-vi-intr") as vm:
        g = Guest(vm)
        ss = Session(g, "x\n")
        ss.keys("qaqqaA!<Esc>@aq")                       # register a is empty while it is recorded, so this ends normally
        g.type(K("@a"), wait=1.5)                        # now the macro calls itself for ever
        g.type(K("<C-c>"), wait=0.1)
        vt = g.wait_screen(ss.s, lambda r: "Interrupted" in r[15], timeout=60)
        assert vt.rows_text()[14].startswith(" NORMAL"), vt.rows_text()[14]
        # the editor is not stuck: ordinary commands still work
        vt = ss.keys("0iy<Esc>:w<CR>", until=lambda r: "written" in r[15] and r[0].startswith("yx!"))
        ss.quit()
        out = g.out("cat /tmp/x")
        assert re.fullmatch(r"yx!+\n", out) and len(out) > 5, out[:80]


@test
def vi_exit_paths():
    """vi: the terminal is handed back after :q, :q!, ZZ, :cq, SIGTERM, SIGINT and a run that ends by itself."""
    with Qemu(cmdline="text", name="t-vi-exit") as vm:
        g = Guest(vm)
        leave = b"\x1b[0m\x1b[?25h\x1b[2J\x1b[H\x1b[?1049l"
        for what, keys, status, want_file in (
            (":q", ":q<CR>", 0, "a\nb\n"),
            (":q!", "x:q!<CR>", 0, "a\nb\n"),
            ("ZZ", "xZZ", 0, "\nb\n"),
            (":cq", ":cq<CR>", 1, "a\nb\n"),
            ("ZQ", "xZQ", 0, "a\nb\n"),
            (":x", "x:x<CR>", 0, "\nb\n"),
            (":wq", "x:wq<CR>", 0, "\nb\n"),
            (":cq after a change", "x:cq<CR>", 1, "a\nb\n"),
            ("Ctrl-C and :qa", "<C-c>:qa<CR>", 0, "a\nb\n"),
        ):
            g.run("printf 'a\\nb\\n' > /tmp/x")
            s = g.start("/tmp/x; echo STATUS=$?")
            g.type(K(keys), wait=0.1)
            g.wait_left(s)
            tail = vm.serial_path.read_bytes()[s:]
            assert leave in tail, f"{what}: the terminal is restored (cursor shown, screen cleared, main screen back)\n{tail[-200:]!r}"
            assert f"STATUS={status}".encode() in tail, f"{what}: exit status {status}\n{tail[-200:]!r}"
            check_eq(g.out("echo ok"), "ok\n", f"{what}: the shell works")
            check_eq(g.out("cat /tmp/x"), want_file, f"{what}: file")

        # a signal from another process: the terminal is restored and the status is 128+signal
        for sig, number in (("TERM", 143), ("INT", 130), ("HUP", 129)):
            g.run("printf 'a\\nb\\n' > /tmp/x")
            s = g.mark()
            vm.send(f"(sleep 2; kill -{sig} $(cat /tmp/vi.pid)) & gapsh -c 'echo $$ > /tmp/vi.pid; exec vi /tmp/x'; echo STATUS=$?\n")
            g.wait_screen(s, lambda rows: any("NORMAL" in r for r in rows))
            end = time.time() + 60
            want = f"STATUS={number}".encode()
            while want not in vm.serial_path.read_bytes()[s:]:
                assert time.time() < end, f"vi did not end after SIG{sig}:\n" + g.screen(s).text()
                time.sleep(0.2)
            tail = vm.serial_path.read_bytes()[s:]
            assert leave in tail, (sig, "the terminal is restored", tail[-300:])
            g.sync()
            check_eq(g.out("echo alive"), "alive\n", f"after SIG{sig}")
            check_eq(g.out("cat /tmp/x"), "a\nb\n", f"SIG{sig}: the file is untouched")
