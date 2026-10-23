"""vi: the modal editor. Keys are typed into a freshly started `vi /tmp/x`, the file is written with :wq and compared with cat;
the screen checks go through tests/vt.py like the pnote tests (see case_text.py and docs/TOOLS.md).

The guest clock runs slowly on a busy host, so nothing here sleeps and hopes: every step waits for something on the screen.
"""
import re
import time

from harness import test, Qemu
from vt import VT

PROMPT = r"reimu@gensokyo \S+ \$ "
LEAVE = b"\x1b[?1049l"
SIZE = "LINES=16 COLUMNS=70 "

ESC = "\x1b"
CR = "\r"
UP, DOWN, RIGHT, LEFT = "\x1b[A", "\x1b[B", "\x1b[C", "\x1b[D"
HOME, END, DEL, PGUP, PGDN = "\x1b[H", "\x1b[F", "\x1b[3~", "\x1b[5~", "\x1b[6~"


def ctrl(c):
    return chr(ord(c.upper()) - 64)


class Guest:
    def __init__(self, vm):
        self.vm = vm
        vm.expect(PROMPT, timeout=120)

    def run(self, cmd, timeout=40):
        self.vm.send(f"echo __BE''GIN__; {cmd}; echo __EN''D__$?\n")
        m = self.vm.expect(r"\n__BEGIN__\n(.*?)__END__(\d+)\n", timeout=timeout)
        self.vm.expect(PROMPT, timeout=15)
        return m.group(1), int(m.group(2))

    def out(self, cmd, timeout=40):
        return self.run(cmd, timeout)[0]

    def mark(self):
        return self.vm.serial_path.stat().st_size

    def screen(self, since):
        vt = VT(60, 160)
        vt.feed(self.vm.serial_path.read_bytes()[since:])
        return vt

    def wait_screen(self, since, needle, timeout=40, row=None):
        end = time.time() + timeout
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

    def wait_left(self, since, timeout=40):
        end = time.time() + timeout
        while True:
            data = self.vm.serial_path.read_bytes()[since:]
            i = data.rfind(LEAVE)
            if i >= 0 and b"$ " in data[i:]:
                return
            if time.time() > end:
                raise TimeoutError("vi did not leave the screen:\n" + self.screen(since).text())
            time.sleep(0.1)

    def sync(self):
        self.run("true")

    def type(self, text, wait=0.3):
        # small chunks: the console input buffer is not large
        for i in range(0, len(text), 48):
            self.vm.send(text[i:i + 48])
            time.sleep(0.05)
        time.sleep(wait)


def check(got, want, what=""):
    assert got == want, f"{what}\n got: {got!r}\nwant: {want!r}"


def printf_arg(text):
    """A string for printf '...' that produces exactly TEXT."""
    return (text.replace("\\", "\\\\").replace("%", "%%").replace("'", "'\\''").replace("\n", "\\n").replace("\t", "\\t"))


def start(g, args="/tmp/x", size=SIZE, env=""):
    """Starts vi and waits for the first screen; returns the mark to read the screen from."""
    s = g.mark()
    g.vm.send(f"{size}{env}vi {args}\n")
    g.wait_screen(s, lambda rows: any("NORMAL" in r for r in rows))
    return s


def edit(g, content, keys, path="/tmp/x", args=None, quit_keys=ESC + ":wq\n"):
    """Writes CONTENT to PATH, starts vi, types KEYS then ':wq', and returns the file afterwards."""
    g.run(f"printf '{printf_arg(content)}' > {path}")
    s = start(g, args or path)
    g.type(keys)
    g.type(quit_keys, wait=0.1)
    g.wait_left(s)
    g.sync()
    return g.out(f"cat {path}")


def cases(g, rows):
    """Runs a list of (name, content, keys, want) edits."""
    for name, content, keys, want in rows:
        got = edit(g, content, keys)
        check(got, want, name)


@test
def vi_basic():
    """vi: insert, delete, change, yank and put, undo and redo, search, counts, saving."""
    with Qemu(cmdline="text", name="t-vi-basic") as vm:
        g = Guest(vm)
        cases(g, [
            ("insert text", "", "ihello world" + ESC, "hello world\n"),
            ("append and open", "one\n", "A two" + ESC + "othree" + ESC, "one two\nthree\n"),
            ("x and dd", "abc\ndef\nghi\n", "x" + "jdd", "bc\nghi\n"),
            ("dw and cw", "foo bar baz\n", "dwcwQUX" + ESC, "QUX baz\n"),
            ("yy p", "a\nb\n", "yyjp", "a\nb\na\n"),
            ("undo redo", "a b c\n", "xxxuu" + ctrl("r"), "b c\n"),
            ("search", "alpha\nbeta\ngamma\n", "/gam" + CR + "x", "alpha\nbeta\namma\n"),
            ("count insert", "", "3ihi" + ESC, "hihihi\n"),
            ("dot repeat", "a b c d\n", "dw.", "c d\n"),
            ("D and J", "ab cd\nef\n", "wDJ", "ab ef\n"),
        ])
