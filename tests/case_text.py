"""Text tools: pnote (editor), patchouli-pager, patchouli (manual viewer, man), and the shrine applets.

The console in the tests is the serial line, so the full-screen programs are driven by sending key bytes (control
characters and escape sequences exactly as the console keyboard produces them, see docs/TOOLS.md) and checked two ways:
by replaying what the guest wrote through a small terminal emulator (tests/vt.py) to see the screen, and by `cat`-ing the
files the editor saved. LINES and COLUMNS make the programs use a 16 x 70 screen whatever the console size is.
"""
import re
import time

from harness import test, Qemu, scaled
from vt import VT

PROMPT = r"reimu@gensokyo \S+ \$ "
LEAVE = b"\x1b[?1049l"                   # what the full-screen programs send when they hand the terminal back
SIZE = "LINES=16 COLUMNS=70 "

CTRL = {c: chr(ord(c) - 64) for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ"}
UP, DOWN, RIGHT, LEFT = "\x1b[A", "\x1b[B", "\x1b[C", "\x1b[D"
HOME, END, DEL, PGUP, PGDN = "\x1b[H", "\x1b[F", "\x1b[3~", "\x1b[5~", "\x1b[6~"
BACKSPACE = "\x7f"


class Guest:
    """A booted system with helpers for shell commands and full-screen programs."""

    def __init__(self, vm):
        self.vm = vm
        vm.expect(PROMPT, timeout=90)

    def run(self, cmd, timeout=40):
        """Runs one shell command line; returns (output, exit status)."""
        self.vm.send(f"echo __BE''GIN__; {cmd}; echo __EN''D__$?\n")
        m = self.vm.expect(r"\n__BEGIN__\n(.*?)__END__(\d+)\n", timeout=timeout)
        self.vm.expect(PROMPT, timeout=15)
        return m.group(1), int(m.group(2))

    def out(self, cmd, timeout=40):
        text, _ = self.run(cmd, timeout)
        return text

    # ---- full-screen programs
    def mark(self):
        return self.vm.serial_path.stat().st_size

    def screen(self, since):
        vt = VT(60, 160)
        vt.feed(self.vm.serial_path.read_bytes()[since:])
        return vt

    def wait_screen(self, since, needle, timeout=20, row=None):
        """Waits until `needle` is on the screen (in row `row` if given). NEEDLE can also be a function of the list of
        screen rows that says when the screen is right. Returns the screen."""
        end = time.time() + scaled(timeout)
        while True:
            vt = self.screen(since)
            rows = vt.rows_text()
            if callable(needle):
                ok = needle(rows)
            elif row is not None:
                ok = needle in rows[row]
            else:
                ok = any(needle in r for r in rows)
            if ok:
                return vt
            if time.time() > end:
                what = "the expected state" if callable(needle) else repr(needle)
                raise TimeoutError(f"{what} never appeared on the screen:\n" + vt.text())
            time.sleep(0.1)

    def wait_left(self, since, timeout=20):
        """Waits until the program has restored the terminal and the shell shows its prompt again."""
        end = time.time() + scaled(timeout)
        while True:
            data = self.vm.serial_path.read_bytes()[since:]
            i = data.rfind(LEAVE)
            if i >= 0 and b"$ " in data[i:]:
                return
            if time.time() > end:
                raise TimeoutError("the program did not leave the screen:\n" + self.screen(since).text())
            time.sleep(0.1)

    def sync(self):
        """Gets the expect() cursor past everything so far, so the next run() starts clean."""
        self.run("true")

    def edit(self, args, keys=(), expect_title=True):
        """Starts pnote with ARGS; returns the mark to read its screen from."""
        since = self.mark()
        self.vm.send(f"{SIZE}pnote {args}\n")
        if expect_title:
            self.wait_screen(since, "Patchouli's Notebook", row=0)
        return since

    def type(self, text, wait=0.35):
        self.vm.send(text)
        time.sleep(wait)


def content(g, path):
    """The bytes of a guest file as a hex string."""
    return g.out(f"od -An -tx1 -v {path} | tr -d ' \\n'; echo").strip()


def check(got, want, what=""):
    """assert got == want, with both values in the message (the test runner prints nothing for a bare assert)."""
    assert got == want, f"{what}\n got: {got!r}\nwant: {want!r}"


def hexof(text):
    return text.encode("utf-8").hex()


# ---------------------------------------------------------------------------------------------------- pnote

@test
def pnote_edit():
    """pnote: typing, saving, the unsaved-changes prompt, no final newline, CRLF, UTF-8, auto-indent, new and read-only files."""
    with Qemu(cmdline="text", name="t-pnote-edit") as vm:
        g = Guest(vm)
        g.run("printf 'one\\ntwo\\nthree\\n' > /tmp/e1.txt; printf 'abc' > /tmp/nonl.txt; printf 'a\\r\\nb\\r\\n' > /tmp/crlf.txt")
        g.run("echo 'café' > /tmp/u8.txt; echo secret > /tmp/ro.txt; chmod 444 /tmp/ro.txt; : > /tmp/empty.txt")

        # --- type, split a line, save
        s = g.edit("/tmp/e1.txt")
        g.wait_screen(s, lambda rows: "/tmp/e1.txt" in rows[0] and "one" in rows[1])     # the title row is painted first
        g.type(DOWN + "X\nmid")
        g.wait_screen(s, "midtwo")
        g.type(CTRL["S"])
        g.wait_screen(s, "Wrote 4 lines")
        g.type(CTRL["X"])
        g.wait_left(s)
        g.sync()
        check(g.out("cat /tmp/e1.txt"), "one\nX\nmidtwo\nthree\n", "typed text and Enter")

        # --- leaving with unsaved changes: N discards, Esc stays, Y saves
        s = g.edit("/tmp/e1.txt")
        g.type("zzz")
        g.wait_screen(s, "Modified", row=0)
        g.type(CTRL["X"])
        g.wait_screen(s, "Save modified buffer?")
        g.type("\x1b", wait=0.6)                                    # a lone Escape cancels the question
        g.wait_screen(s, "Cancelled")
        g.type(CTRL["X"])
        g.wait_screen(s, "Save modified buffer?")
        g.type("n")
        g.wait_left(s)
        g.sync()
        check(g.out("cat /tmp/e1.txt"), "one\nX\nmidtwo\nthree\n", "N must leave the file alone")
        s = g.edit("/tmp/e1.txt")
        g.type("zzz" + CTRL["X"])
        g.wait_screen(s, "Save modified buffer?")
        g.type("y")
        g.wait_left(s)
        g.sync()
        check(g.out("head -1 /tmp/e1.txt"), "zzzone\n", "Y saves before leaving")

        # --- a file without a final newline stays that way; CRLF survives; UTF-8 moves by characters
        s = g.edit("/tmp/nonl.txt")
        g.type("!" + CTRL["S"])
        g.wait_screen(s, "Wrote")
        g.type(CTRL["X"])
        g.wait_left(s)
        g.sync()
        assert content(g, "/tmp/nonl.txt") == hexof("!abc"), "no final newline must not appear"

        s = g.edit("/tmp/crlf.txt")
        vt = g.wait_screen(s, "[CRLF]", row=0)
        g.type("X" + CTRL["S"])
        g.wait_screen(s, "Wrote")
        g.type(CTRL["X"])
        g.wait_left(s)
        g.sync()
        assert content(g, "/tmp/crlf.txt") == hexof("Xa\r\nb\r\n"), "CR LF line endings are kept"

        s = g.edit("/tmp/u8.txt")
        g.wait_screen(s, "café")
        g.type(HOME + RIGHT * 3 + DEL)                               # delete the e-acute: two bytes, one character
        g.wait_screen(s, "caf")
        g.type(END + "あ" + CTRL["S"])
        g.wait_screen(s, "Wrote")
        g.type(CTRL["X"])
        g.wait_left(s)
        g.sync()
        assert content(g, "/tmp/u8.txt") == hexof("cafあ\n"), "UTF-8 editing by code point"

        # --- auto-indent copies the indentation; Tab inserts a tab; -E turns tabs into spaces
        s = g.edit("/tmp/ai.txt")
        g.wait_screen(s, "New file")
        g.type("  x\ny\n\tz" + CTRL["S"])
        g.wait_screen(s, "Wrote")
        g.type(CTRL["X"])
        g.wait_left(s)
        g.sync()
        check(g.out("cat -E /tmp/ai.txt"), "  x$\n  y$\n  \tz$\n", "auto-indent copies the indentation of the line above")
        s = g.edit("-I -E /tmp/ai2.txt")
        g.type("  x\ny\t|" + CTRL["S"])
        g.wait_screen(s, "Wrote")
        g.type(CTRL["X"])
        g.wait_left(s)
        g.sync()
        check(g.out("cat -E /tmp/ai2.txt"), "  x$\ny       |$\n", "-I (no auto-indent) and -E (tabs as spaces)")

        # --- empty file: nothing to save, leaving is immediate; read-only files and -R
        s = g.edit("/tmp/empty.txt")
        g.type(CTRL["X"])
        g.wait_left(s)
        g.sync()
        check(g.out("wc -c < /tmp/empty.txt").strip(), "0")

        s = g.edit("/tmp/ro.txt")
        g.wait_screen(s, "[read-only file]", row=0)
        g.type("x" + CTRL["S"])
        g.wait_screen(s, "Save under another name?")
        g.type("n")
        g.wait_screen(s, "Cannot write")
        g.type(CTRL["X"])
        g.wait_screen(s, "Save modified buffer?")
        g.type("n")
        g.wait_left(s)
        g.sync()
        check(g.out("cat /tmp/ro.txt"), "secret\n")

        s = g.edit("-R /tmp/e1.txt")
        g.wait_screen(s, "[view only]", row=0)
        g.type("hello")
        g.wait_screen(s, "reading only")
        g.type(CTRL["X"])
        g.wait_left(s)                                              # nothing was modified: no question
        g.sync()
        check(g.out("head -1 /tmp/e1.txt"), "zzzone\n")


@test
def pnote_commands():
    """pnote: search and repeat, replace, undo and redo, cut and paste, selection, go to line, help, line numbers."""
    with Qemu(cmdline="text", name="t-pnote-cmds") as vm:
        g = Guest(vm)
        g.run("printf 'alpha\\nbeta\\ngamma\\ndelta\\nalpha two\\n' > /tmp/c1.txt")

        # --- search (smartcase): the cursor lands on the match; ^P goes back; undo and redo are separate steps
        s = g.edit("-n /tmp/c1.txt")
        g.type(CTRL["W"])
        g.wait_screen(s, "Search:")
        g.type("alp\n")                                             # the match under the cursor is skipped: line 5
        g.wait_screen(s, "  5 alpha two")
        g.type("!")
        g.wait_screen(s, "  5 !alpha two")
        g.type(CTRL["P"])                                           # back to line 1
        g.type("?")
        g.wait_screen(s, "  1 ?alpha")
        g.type(CTRL["Z"])
        g.wait_screen(s, "Undone")
        vt = g.wait_screen(s, "  1 alpha")
        assert "  5 !alpha two" in vt.rows_text()[5], vt.text()
        g.type(CTRL["Z"])
        vt = g.wait_screen(s, "  5 alpha two")
        g.type(CTRL["Y"])
        g.wait_screen(s, "Redone")
        vt = g.wait_screen(s, "  5 !alpha two")
        assert "  1 alpha" in vt.rows_text()[1] and "?" not in vt.rows_text()[1], vt.text()

        # --- go to a line, cut a line, paste it above another
        g.type(CTRL["L"])
        g.wait_screen(s, "Go to line")
        g.type("3\n")
        g.wait_screen(s, "Line 3 of 5")
        g.type(CTRL["K"])
        g.wait_screen(s, "  3 delta")
        g.type(DOWN + CTRL["U"])                                    # cut lines are pasted above the current line
        g.wait_screen(s, "  4 gamma")
        g.type(CTRL["S"])
        g.wait_screen(s, "Wrote 5 lines")
        g.type(CTRL["X"])
        g.wait_left(s)
        g.sync()
        check(g.out("cat /tmp/c1.txt"), "alpha\nbeta\ndelta\ngamma\n!alpha two\n")

        # --- replace: N skips a match, A replaces the rest (wrapping around the end), smartcase ignores case
        g.run("printf 'one two\\ntwo three\\nfour\\n' > /tmp/c2.txt")
        s = g.edit("/tmp/c2.txt")
        g.type(CTRL["R"])
        g.wait_screen(s, "Search for")
        g.type("t\n")
        g.wait_screen(s, "Replace with")
        g.type("T\n")
        g.wait_screen(s, "Replace this instance?")
        g.type("n")
        g.wait_screen(s, "Replace this instance?")
        g.type("a")
        g.wait_screen(s, "Replaced 2 occurrences")
        g.type(CTRL["S"])
        g.wait_screen(s, "Wrote 3 lines")
        g.type(CTRL["X"])
        g.wait_left(s)
        g.sync()
        check(g.out("cat /tmp/c2.txt"), "one two\nTwo Three\nfour\n")

        # --- a regular expression (Alt-R in the prompt) with groups in the replacement
        g.run("printf 'x=1\\ny=22\\n' > /tmp/c5.txt")
        s = g.edit("/tmp/c5.txt")
        g.type(CTRL["R"])
        g.wait_screen(s, "Search for")
        g.type("\x1br")
        g.wait_screen(s, "(regex)")
        g.type("([a-z])=([0-9]+)\n")
        g.wait_screen(s, "Replace with")
        g.type("\\2<\\1\n")                                         # a backslash and a digit, not the control characters
        g.wait_screen(s, "Replace this instance?")
        g.type("a")
        g.wait_screen(s, "Replaced 2 occurrences")
        g.type(CTRL["S"])
        g.wait_screen(s, "Wrote")
        g.type(CTRL["X"])
        g.wait_left(s)
        g.sync()
        got = g.out("cat /tmp/c5.txt")
        assert got == "1<x\n22<y\n", repr(got)

        # a word boundary is judged against the whole line, also when the search starts in the middle of it
        g.run("printf 'afoo foo\\n' > /tmp/c6.txt")
        s = g.edit("/tmp/c6.txt")
        g.type(CTRL["W"])
        g.wait_screen(s, "Search:")
        g.type("\x1br")
        g.wait_screen(s, "(regex)")
        g.type("\\<foo\n")                                          # the "foo" inside "afoo" is not a word start
        g.type("!")
        g.wait_screen(s, "afoo !foo")
        g.type(CTRL["S"])
        g.wait_screen(s, "Wrote")
        g.type(CTRL["X"])
        g.wait_left(s)
        g.sync()
        check(g.out("cat /tmp/c6.txt"), "afoo !foo\n")

        # --- a selection: Alt-A, move, copy (Alt-6), paste somewhere else; cut it with ^K
        g.run("printf 'abcdef\\nxyz\\n' > /tmp/c3.txt")
        s = g.edit("/tmp/c3.txt")
        g.type("\x1ba" + RIGHT * 3 + "\x1b6")
        g.wait_screen(s, "Copied 3 bytes")
        g.type(DOWN + END + CTRL["U"])
        g.wait_screen(s, "xyzabc")
        g.type(HOME + "\x1ba" + RIGHT * 2 + CTRL["K"])
        g.wait_screen(s, "Cut 2 bytes")
        g.type(CTRL["S"])
        g.wait_screen(s, "Wrote")
        g.type(CTRL["X"])
        g.wait_left(s)
        g.sync()
        check(g.out("cat /tmp/c3.txt"), "abcdef\nzabc\n")

        # --- undo is many levels deep; a run of typing is one step
        g.run("printf 'base\\n' > /tmp/c4.txt")
        s = g.edit("/tmp/c4.txt")
        g.type(END + " one" + "\n" + "two" + "\n" + "three")
        g.wait_screen(s, "three")
        for _ in range(3):
            g.type(CTRL["Z"], wait=0.3)
        g.wait_screen(s, lambda rows: rows[1] == "base one" and rows[2] == "" and rows[3] == "~")     # all three undone
        g.type(CTRL["Y"] * 3 + CTRL["S"])
        g.wait_screen(s, "Wrote 3 lines")
        g.type(CTRL["X"])
        g.wait_left(s)
        g.sync()
        check(g.out("cat /tmp/c4.txt"), "base one\ntwo\nthree\n")

        # --- help screen and where-am-i
        s = g.edit("/tmp/c1.txt")
        g.type(CTRL["G"])
        g.wait_screen(s, "A grimoire for plain text")
        g.type("q")
        g.wait_screen(s, "^G")
        g.type(CTRL["C"])
        g.wait_screen(s, "line 1/5")
        g.type(CTRL["X"])
        g.wait_left(s)


@test
def pnote_big():
    """pnote: a 5000-line file, a very long line, standard input and the filter mode."""
    with Qemu(cmdline="text", name="t-pnote-big") as vm:
        g = Guest(vm)
        g.run("seq 1 5000 > /tmp/big.txt")
        s = g.edit("-n /tmp/big.txt")
        g.wait_screen(s, "  1 1")
        g.type(PGDN)
        g.wait_screen(s, " 15 15")                                  # a page is the 13 text rows minus one
        g.type("\x1b/")                                             # Alt-/ : the last line
        g.wait_screen(s, "5000 5000")
        g.type(CTRL["L"])
        g.wait_screen(s, "Go to line")
        g.type("2500\n")
        g.wait_screen(s, "2500 2500")
        g.type("<MARK>")
        g.type(CTRL["S"])
        g.wait_screen(s, "Wrote 5000 lines", timeout=60)
        g.type(CTRL["X"])
        g.wait_left(s)
        g.sync()
        check(g.out("sed -n 2500p /tmp/big.txt"), "<MARK>2500\n")
        check(g.out("wc -l < /tmp/big.txt").strip(), "5000")
        check(g.out("sed -n '1p;5000p' /tmp/big.txt"), "1\n5000\n")

        # a line of 3000 characters scrolls sideways and shows markers at the edges
        g.run("l=; i=0; while [ $i -lt 300 ]; do l=${l}0123456789; i=$((i+1)); done; echo \"$l\" > /tmp/long.txt; echo tail >> /tmp/long.txt")
        s = g.edit("/tmp/long.txt")
        g.type(END)
        # at the end of the line the cursor is brought to the middle of the row: "<" marks the hidden start, nothing is hidden on the right
        vt = g.wait_screen(s, lambda rows: rows[1].startswith("<") and rows[1].endswith("23456789"))
        assert ">" not in vt.rows_text()[1] and 20 <= len(vt.rows_text()[1]) < 60, vt.rows_text()[1]
        g.type(HOME)
        vt = g.wait_screen(s, lambda rows: rows[1].startswith("0123") and rows[1].endswith(">"))
        assert len(vt.rows_text()[1]) >= 60, vt.rows_text()[1]
        g.type(CTRL["X"])
        g.wait_left(s)

        # standard input: the keyboard comes from the terminal; filter mode writes the result to the pipe
        g.sync()
        s = g.mark()
        vm.send(f"printf 'p\\nq\\n' | {SIZE}pnote - > /tmp/filter.txt; echo rc=$?\n")
        g.wait_screen(s, "(standard input)", row=0)
        g.type("Z" + CTRL["X"])
        g.wait_screen(s, "down the pipe?")
        g.type("y")
        vm.expect(r"rc=0\n", timeout=20)
        vm.expect(PROMPT, timeout=15)
        check(g.out("cat /tmp/filter.txt"), "Zp\nq\n")
        s = g.mark()
        vm.send(f"echo again | {SIZE}pnote - > /tmp/filter2.txt; echo rc=$?\n")
        g.wait_screen(s, "(standard input)", row=0)
        g.type("Q" + CTRL["X"])
        g.wait_screen(s, "down the pipe?")
        g.type("n")
        vm.expect(r"rc=1\n", timeout=20)
        vm.expect(PROMPT, timeout=15)
        check(g.out("wc -c < /tmp/filter2.txt").strip(), "0")


# ---------------------------------------------------------------------------------------------------- pager

@test
def pager():
    """patchouli-pager (and less, more): paging, search, line numbers, chopping, -F, pipes, restoring the terminal."""
    with Qemu(cmdline="text", name="t-pager") as vm:
        g = Guest(vm)
        g.run("seq 1 100 > /tmp/n.txt; printf 'short\\n%s\\nend\\n' \"$(seq 1 60 | tr '\\n' ' ')\" > /tmp/long.txt")

        # paging, a count, going to the end and back, searching forwards and backwards
        s = g.mark()
        vm.send(f"seq 1 100 | {SIZE}patchouli-pager\n")
        vt = g.wait_screen(s, "lines 1-15")
        rows = vt.rows_text()
        assert rows[0] == "1" and rows[14] == "15", rows[:16]
        assert "(standard input)" in rows[15] and "h:help" in rows[15], rows[15]
        g.type(" ")
        vt = g.wait_screen(s, "lines 15-29")
        assert vt.rows_text()[0] == "15", vt.text()
        g.type("5j")
        vt = g.wait_screen(s, "lines 20-34")
        g.type("G")
        vt = g.wait_screen(s, "(END)")
        assert vt.rows_text()[14] == "100" and "lines 86-100/100" in vt.rows_text()[15], vt.text()
        g.type("g")
        g.wait_screen(s, "lines 1-15")
        g.type("/55\n")
        vt = g.wait_screen(s, "lines 55-69")
        assert vt.rows_text()[0] == "55", vt.text()
        g.type("n")                                                 # 55 is the only line with that number: the message replaces the status
        g.wait_screen(s, "Pattern not found: 55", row=15)
        g.type("?4\n")
        g.wait_screen(s, "lines 54-68")                             # backward from the top line: "54" is the first hit before 55
        g.type("50p")
        g.wait_screen(s, "lines 51-65")
        g.type("d")
        vt = g.wait_screen(s, "lines 58-72")
        g.type("u")
        g.wait_screen(s, "lines 51-65")
        g.type("h")
        g.wait_screen(s, "forward one screen")
        g.type("q")
        g.type("q")
        g.wait_left(s)

        # line numbers, wrapping, chopping and sideways scrolling
        g.sync()
        s = g.mark()
        vm.send(f"{SIZE}patchouli-pager -N /tmp/long.txt\n")
        vt = g.wait_screen(s, "lines 1-")
        rows = vt.rows_text()
        assert rows[0] == "    1 short" and rows[1].startswith("    2 1 2 3"), rows[:5]
        assert rows[2].startswith("      ") and "2 3 4" not in rows[2][:6], rows[:5]      # the wrapped part has a blank gutter
        g.type("q")
        g.wait_left(s)
        g.sync()
        s = g.mark()
        vm.send(f"{SIZE}patchouli-pager -S /tmp/long.txt\n")
        vt = g.wait_screen(s, "lines 1-3")
        rows = vt.rows_text()
        assert rows[0] == "short" and rows[2] == "end" and len(rows[1]) == 70, rows[:4]
        g.type(RIGHT)
        vt = g.wait_screen(s, lambda rows: rows[0] == "" and rows[1].startswith(" 16 17 18"))     # scrolled half a screen (35 columns)
        rows = vt.rows_text()
        assert rows[2] == "", rows[:3]
        g.type("q")
        g.wait_left(s)

        # matches are shown in reverse video; a word boundary also holds for a second match in the same line
        g.sync()
        g.run("printf 'abab\\n' > /tmp/wb.txt")
        s = g.mark()
        vm.send(f"{SIZE}patchouli-pager /tmp/wb.txt\n")
        g.wait_screen(s, "lines 1-1")
        g.type("/\\<ab\n")
        end = time.time() + scaled(20)
        while not any(a & 2 for a in g.screen(s).row_attrs(0)[:4]):
            assert time.time() < end, g.screen(s).text()
            time.sleep(0.1)
        time.sleep(1.0)
        attrs = g.screen(s).row_attrs(0)
        assert [bool(a & 2) for a in attrs[:5]] == [True, True, False, False, False], attrs[:5]
        g.type("q")
        g.wait_left(s)

        # -F prints a short file and returns at once; no terminal on stdout means cat; less and more are the same program
        out = g.out(f"{SIZE}patchouli-pager /tmp/long.txt | head -1; echo plain | less | cat; echo hi | more -N | cat")
        assert out == "short\nplain\n     1  hi\n", out
        out, rc = g.run("printf 'a\\nb\\n' | patchouli-pager -N | cat")
        assert out == "     1  a\n     2  b\n" and rc == 0, out
        out = g.out(f"{SIZE}patchouli-pager -F /tmp/long.txt")                  # stdout is the terminal here, the text fits: no paging
        assert out == "short\n" + " ".join(str(i) for i in range(1, 61)) + " \nend\n", out

        # a pager asked to stop by a signal gives the terminal back
        g.sync()
        s = g.mark()
        vm.send(f"(sleep 3; kill $(pidof patchouli-pager)) & {SIZE}patchouli-pager /tmp/n.txt; echo status=$?\n")
        g.wait_screen(s, "lines 1-15")
        vm.expect(r"status=143\n", timeout=150)                     # the guest clock can run slowly on a busy host: be patient
        vm.expect(PROMPT, timeout=15)
        assert LEAVE in vm.serial_path.read_bytes()[s:], "the pager did not give the screen back"
        check(g.out("echo the terminal works"), "the terminal works\n")

        # more: quits when it runs off the end
        s = g.mark()
        vm.send(f"{SIZE}more /tmp/n.txt\n")
        g.wait_screen(s, "--More--(")
        g.type("G")
        g.wait_screen(s, "--More--(END)")
        g.type(" ")
        g.wait_left(s)


# ---------------------------------------------------------------------------------------------------- manuals

@test
def manual():
    """patchouli / man: pages, search, listing, --check-all, one page for every applet, rendering for terminals."""
    with Qemu(cmdline="text", name="t-manual") as vm:
        g = Guest(vm)

        # plain text when not on a terminal
        out, rc = g.run("patchouli ls | head -12")
        assert rc == 0 and "ls(1)" in out and "Patchouli's Library" in out and "NAME\n       ls - list directory contents" in out, out
        assert "\x1b" not in out, repr(out)
        out, rc = g.run("patchouli -W 40 echo | head -20")
        assert max(len(line) for line in out.splitlines()) <= 40, out
        out, rc = g.run("man 7 regex | grep -c 'BASIC EXPRESSIONS'")
        assert out.strip() == "1" and rc == 0, (out, rc)
        out, rc = g.run("patchouli nosuchpage")
        assert rc == 1 and "no manual entry" in out, out
        out, rc = g.run("patchouli -w ls; patchouli -f cat; patchouli -k 'directory'")
        assert "/ScarletDevilMansion/share/patchouli/1/ls.pw" in out and "cat(1)" in out and "ls(1)" in out, out

        # the whole library parses
        out, rc = g.run("patchouli --check-all", timeout=90)
        assert rc == 0 and re.search(r"\d+ pages checked, 0 problems", out), out

        # every applet has a page, and the index lists the builtins and aliases too
        listing = g.out("kappabox --list")
        assert "applets:" in listing, listing
        applets = [w for w in listing.split("applets:", 1)[1].split() if re.fullmatch(r"[a-z0-9_]+", w)]
        index = g.out("patchouli -l", timeout=60)
        names = set(re.findall(r"^(\S+?)\(\d\)", index, re.M))
        assert len(applets) > 90, applets
        missing = [a for a in applets if a not in names]
        assert not missing, f"applets without a manual page: {missing}"
        for b in ["cd", "export", "alias", "test", "[", ".", ":", "popd", "fg", "less", "more", "man", "gapsh", "gensokyo", "regex", "keys"]:
            assert b in names, f"{b} is not in the manual index"

        # aliases lead to the real page; the section can be given
        out, rc = g.run("man less | head -8; man 8 reboot | head -3; patchouli -a echo | head -3")
        assert "patchouli-pager(1)" in out and "reboot(8)" in out and "echo(1)" in out, out

        # on a terminal the page is formatted with colour and goes to the pager
        g.sync()
        s = g.mark()
        vm.send(f"{SIZE}patchouli echo\n")
        vt = g.wait_screen(s, lambda rows: "echo(1)" in rows[0] and "lines 1-" in rows[15])      # the status line is painted last
        rows = vt.rows_text()
        assert "NAME" in rows[2] and "echo - write its arguments" in rows[3], rows[:5]
        assert "echo(1)" in rows[15], rows[15]
        g.type(" ")
        g.wait_screen(s, "lines 15-")
        g.type("q")
        g.wait_left(s)

        # with PAGER=cat the ANSI-formatted page goes straight to the terminal: headings are bold red
        g.sync()
        s = g.mark()
        vm.send("PAGER=cat patchouli echo; echo __ST''OP__\n")
        vm.expect(r"__STOP__\n", timeout=20)
        vt = VT(160, 200)
        vt.feed(g.vm.serial_path.read_bytes()[s:])
        y = vt.find("NAME")
        assert y >= 0, "the NAME heading is missing from:\n" + vt.text()
        attrs = vt.row_attrs(y)[0]
        assert attrs & 1 and (attrs >> 8) & 0xFF, f"NAME should be bold with a colour, attr={attrs:#x}"
        vm.expect(PROMPT, timeout=15)


# ---------------------------------------------------------------------------------------------------- the shrine

@test
def shrine():
    """omikuji, saisen, reimusay, kappasay: output shapes, the daily seed, the offering box, bubbles."""
    with Qemu(cmdline="text", name="t-shrine") as vm:
        g = Guest(vm)

        out = g.out("omikuji | cat")           # a pipe: plain text
        lines = out.splitlines()
        assert re.fullmatch(r"Omikuji for reimu, \d{4}-\d\d-\d\d", lines[0]), lines[0]
        assert re.fullmatch(r"Fortune: (Dai-kichi|Chu-kichi|Sho-kichi|Kichi|Sue-kichi|Kyo) \(.+\)", lines[1]), lines[1]
        labels = [l.split()[0] if not l.startswith("Lost item") else "Lost" for l in lines[4:]]
        assert labels == ["Wish", "Travel", "Business", "Health", "Lost", "Love"], lines
        assert g.out("omikuji | cat") == out, "the slip is the same all day"
        assert out.isascii(), "the console font has no kanji: they only come with -k"
        kj = g.out("omikuji -k | cat").splitlines()
        assert re.fullmatch(r"Fortune: \S+ \((大吉|中吉|小吉|吉|末吉|凶), .+\)", kj[1]), kj[1]
        draws = {g.out("omikuji -r | cat | tail -n +3") for _ in range(6)}
        assert len(draws) > 1, "omikuji -r should draw fresh slips"
        fl = g.out("fortune | cat").splitlines()
        assert fl[0].startswith("Omikuji for reimu") and fl[1].startswith("Fortune: "), fl[:2]
        # on a terminal: boxed and coloured
        s = g.mark()
        vm.send("omikuji; echo __DO''NE__\n")
        vm.expect(r"__DONE__\n", timeout=20)
        vm.expect(PROMPT, timeout=15)
        text = g.screen(s).text()
        assert "┌" in text and "Omikuji" in text and "└" in text, text
        assert all(ch.isascii() or ch in "┌└│─" for ch in text), "only box-drawing characters, no kanji, by default"

        # the offering box
        out = g.out("rm -f /tmp/saisen-total; saisen 100")
        check(out.splitlines()[0], "You toss 100 yen into the offering box.")
        out = g.out("saisen 0")
        assert "offer... nothing" in out and "(nothing added)" in out and "holds 100 yen" in out, out
        out = g.out("saisen")                                        # the traditional lucky coin
        assert "toss 5 yen" in out and "go-en" in out and "holds 105 yen" in out, out
        check(g.out("saisen -s"), "The offering box holds 105 yen.\n")
        out = g.out("saisen 1000000 | tail -2")
        assert out.endswith("holds 1000105 yen.\n"), out
        out, rc = g.run("saisen abc")
        assert rc == 1 and "not an amount" in out, (out, rc)
        out, rc = g.run("saisen -3")
        assert rc == 1 and "cannot take money out" in out, (out, rc)

        # speech bubbles
        out = g.out("reimusay hi")
        rows = out.splitlines()
        assert rows[0] == " ____" and rows[1] == "< hi >" and rows[2] == " ----", rows[:4]
        assert any("(@ `-..-' @)" in r for r in rows) and rows[3].strip() == "\\", rows[3:6]
        out = g.out("reimusay -W 12 one two three four five six")
        rows = out.splitlines()
        assert rows[1].startswith("/ one two") and rows[1].endswith("\\") and rows[-1] != "", rows[:6]
        assert any(r.startswith("\\ ") and r.endswith("/") for r in rows), rows
        width = len(rows[1])
        assert all(len(r) == width for r in rows[1:5] if r[:1] in "/|\\"), rows[:6]
        out = g.out("echo 'cucumbers' | kappasay -t")
        rows = out.splitlines()
        assert rows[1] == "( cucumbers )" and rows[3].strip() == "o", rows[:6]
        assert any("dish" not in r and "'-._______.-'" in r for r in rows), rows
        out = g.out("printf 'a\\nbb\\n' | kappasay -n")
        assert out.splitlines()[1].startswith("/ a ") and out.splitlines()[2].startswith("\\ bb"), out
