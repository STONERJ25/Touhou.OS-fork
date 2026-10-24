"""Helpers for the vi tests (tests/case_vi*.py).

Two ways of driving the editor in the guest:

* Script mode (fast, exact): `vi -s keys file` takes its keys from a file, so a check is one short shell line and needs no sleeps.
  `ScriptRunner.check()` writes the text and the keys, runs vi, and compares the file afterwards byte for byte (via `od`).
* Typed mode (the real path): `Guest` types into a running `vi` over the serial console and reads the screen through
  tests/vt.py; used for the screen checks, Esc handling and the exit paths.

Keys are written in vim's notation: <Esc> <CR> <BS> <Tab> <C-x> <Up> <Down> <Left> <Right> <Home> <End> <Del> <Ins> <PageUp>
<PageDown> <lt> <Nul>; everything else is literal.

A "cursor row" has `|` in front of the character the cursor is on, both in the text and in the expected text; the runner moves
the cursor there before the keys and appends a probe afterwards, so the cursor position is compared along with the text.
"""
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from harness import Qemu  # noqa: E402,F401
from vt import VT  # noqa: E402
import test_shell  # noqa: E402

PROMPT = test_shell.PROMPT
LEAVE = b"\x1b[?1049l"
SIZE = "LINES=16 COLUMNS=70 "

ESC, CR = "\x1b", "\r"
UP, DOWN, RIGHT, LEFT = "\x1b[A", "\x1b[B", "\x1b[C", "\x1b[D"
HOME, END, DEL, INS, PGUP, PGDN = "\x1b[H", "\x1b[F", "\x1b[3~", "\x1b[2~", "\x1b[5~", "\x1b[6~"

_NAMES = {
    "esc": ESC, "cr": CR, "enter": CR, "return": CR, "bs": "\x7f", "tab": "\t", "space": " ", "lt": "<", "bar": "|",
    "nul": "\x00", "up": UP, "down": DOWN, "left": LEFT, "right": RIGHT, "home": HOME, "end": END, "del": DEL,
    "ins": INS, "pageup": PGUP, "pagedown": PGDN, "bslash": "\\",
}
_KEY = re.compile(r"<(?:([Cc])-(.)|([A-Za-z]+))>")


def K(s):
    """Expands <Esc>, <CR>, <C-r> ... in a key string."""
    def sub(m):
        if m.group(1):
            ch = m.group(2)
            if ch == "?":
                return "\x7f"
            return chr(ord(ch.upper()) & 0x1F)
        name = m.group(3).lower()
        return _NAMES.get(name, m.group(0))
    return _KEY.sub(sub, s)


def ctrl(c):
    return chr(ord(c.upper()) - 64)


def to_bytes(s):
    return s if isinstance(s, (bytes, bytearray)) else s.encode("utf-8", "surrogateescape")


def shq(data):
    """A single-quoted shell word that printf turns back into exactly `data` (bytes or str)."""
    out = []
    for b in to_bytes(data):
        c = chr(b)
        if c == "\\":
            out.append("\\\\")
        elif c == "%":
            out.append("%%")
        elif c == "'":
            out.append("'\\''")
        elif c == "\n":
            out.append("\\n")
        elif c == "\t":
            out.append("\\t")
        elif b < 0x20 or b >= 0x7F:
            out.append("\\0%03o" % b)           # gapsh's printf knows \0ddd (a leading 0), not the POSIX \ddd
        else:
            out.append(c)
    return "'" + "".join(out) + "'"


def cursor_row(text):
    """Splits text with a `|` cursor marker into (text, line (1-based), column (0-based))."""
    i = text.index("|")
    line = text.count("\n", 0, i) + 1
    col = i - (text.rfind("\n", 0, i) + 1)
    return text[:i] + text[i + 1:], line, col


class Row:
    """One scripted edit: text before, keys, text after (or None to skip the comparison), extra vi arguments.

    args:   the vi arguments after `-s keys` (typed into the shell as they are; the default is the file /tmp/x)
    rc:     expected exit status of vi (None: not checked)
    save:   append <Esc>:wq<CR> to the keys (False for rows that quit by themselves)
    setup:  a shell command run first (to create other files)
    post:   a shell command run afterwards; its output must be exactly post_want
    env:    window size as (lines, columns); the default is 24 x 80
    """

    def __init__(self, name, before, keys, after, args="/tmp/x", rc=None, cursor=False, save=True, setup="", env=None,
                 post="", post_want=""):
        self.name, self.before, self.keys, self.after = name, before, keys, after
        self.args, self.rc, self.cursor, self.save, self.setup, self.env = args, rc, cursor, save, setup, env
        self.post, self.post_want = post, post_want


def R(name, before, keys, after, **kw):
    return Row(name, before, keys, after, **kw)


def C(name, before, keys, after, **kw):
    """Cursor row: `|` marks the cursor in both texts."""
    kw["cursor"] = True
    return Row(name, before, keys, after, **kw)


# t TEXT KEYS ARGS...: writes /tmp/x and /tmp/k, runs `vi -s /tmp/k ARGS...` (the runner always passes at least the file name,
# because gapsh expands "$@" with no parameters to one empty word) and prints the exit status and /tmp/x as hex
SHELL_FUNC = (
    "t(){ printf \"$1\" >/tmp/x; printf \"$2\" >/tmp/k; shift 2; vi -s /tmp/k \"$@\" >/dev/null 2>&1; "
    "echo \"rc=$?\"; od -An -v -tx1 /tmp/x; echo @@@; }"
)


def parse_t(out):
    m = re.search(r"rc=(\d+)\n(.*?)@@@", out, re.S)
    if not m:
        raise AssertionError(f"no result from the guest: {out[-300:]!r}")
    return int(m.group(1)), bytes(int(h, 16) for h in re.findall(r"\b[0-9a-f]{2}\b", m.group(2)))


def show(b):
    return b.decode("utf-8", "backslashreplace")


class ScriptRunner:
    def __init__(self, vm):
        self.vm = vm
        self.failures = []
        self.count = 0
        self.restarts = 0
        self.boot()

    def boot(self):
        self.vm.expect(PROMPT, timeout=120)
        test_shell.run(self.vm, SHELL_FUNC)
        self.size = None
        self.set_size((24, 80))

    def set_size(self, size):
        if size != self.size:
            test_shell.run(self.vm, f"export LINES={size[0]} COLUMNS={size[1]}")
            self.size = size

    def sh(self, cmd, timeout=60):
        return test_shell.run(self.vm, cmd, timeout=timeout)

    def run_vi(self, before, keys, args="", setup=""):
        """Runs `vi -s` on `before` with the keys; returns (exit status, file contents as bytes)."""
        if setup:
            self.sh(setup)
        cmd = f"t {shq(before)} {shq(keys)} {args}"
        out, _ = test_shell.run(self.vm, cmd, timeout=60)
        return parse_t(out)

    def run_vi_file(self, path, keys, timeout=60, args="", repeat=None):
        """Runs `vi -s keys path` on an existing file (no output capture). `repeat` = (unit, n) makes the key file start with the
        unit repeated n times (built in the guest, so a long run needs no long command line). Returns (exit status, seconds)."""
        keys = K(keys) + K("<Esc>:wq<CR>")
        if repeat:
            unit, n = repeat
            pre = f"yes {unit} | head -n {n} | tr -d '\\n' >/tmp/k; printf {shq(keys)} >>/tmp/k"
        else:
            pre = f"printf {shq(keys)} >/tmp/k"
        t0 = time.time()
        out, _ = test_shell.run(self.vm, f"{pre}; vi -s /tmp/k {args} {path} >/dev/null 2>&1; echo \"rc=$?\"", timeout=timeout)
        m = re.search(r"rc=(\d+)", out)
        if not m:
            raise AssertionError(f"vi gave no exit status: {out[-200:]!r}")
        return int(m.group(1)), time.time() - t0

    def check_row(self, r):
        before, keys, after = r.before, r.keys, r.after
        if r.cursor:
            before, line, col = cursor_row(before)
            keys = f"{line}G0" + (f"{col}l" if col else "") + keys + "<Esc>i|<Esc>"
        keys = K(keys)
        if r.save:
            keys += K("<Esc>:wq<CR>")
        self.set_size(r.env or (24, 80))
        rc, got = self.run_vi(to_bytes(before), keys, r.args, r.setup)
        want = to_bytes(after) if after is not None else None
        problems = []
        if want is not None and got != want:
            problems.append(f"file {show(got)!r}, expected {show(want)!r}")
        if r.rc is not None and rc != r.rc:
            problems.append(f"exit status {rc}, expected {r.rc}")
        if r.post:
            out, _ = self.sh(r.post)
            if out != r.post_want:
                problems.append(f"`{r.post}` printed {out!r}, expected {r.post_want!r}")
        return problems

    def run_rows(self, rows):
        for r in rows:
            self.count += 1
            try:
                problems = self.check_row(r)
            except Exception as e:  # noqa: BLE001
                head = str(e).strip().splitlines()[0][:200] if str(e).strip() else ""
                problems = [f"{type(e).__name__}: {head}"]
                self.recover()
            if problems:
                self.failures.append(f"[{r.name}] keys {r.keys!r} on {r.before!r}: " + "; ".join(problems))

    def recover(self):
        """After a timeout vi may still be running (or hung): quit it and resynchronise with the shell, or reboot."""
        self.restarts += 1
        token = f"SYNC{self.restarts}ED"
        try:
            self.vm.send("\x1b\x1b:q!\r")
            time.sleep(1.0)
            self.vm.send(f"echo {token[:4]}''{token[4:]}\n")
            self.vm.expect(rf"\n{token}\n", timeout=20)
            self.vm.expect(PROMPT, timeout=15)
            self.size = None
            self.set_size((24, 80))
            return
        except Exception:  # noqa: BLE001
            pass
        self.vm.stop()
        self.vm.start()
        self.boot()

    def finish(self, what):
        assert not self.failures, f"{len(self.failures)} of {self.count} {what} checks failed:\n" + "\n".join(self.failures)


# ---------------------------------------------------------------------------------------------------------- typed mode

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

    def screen(self, since, rows=60, cols=160):
        vt = VT(rows, cols)
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

    def start(self, args="/tmp/x", size=SIZE, env=""):
        """Starts vi and waits for the first screen; returns the mark to read the screen from."""
        s = self.mark()
        self.vm.send(f"{size}{env}vi {args}\n")
        self.wait_screen(s, lambda rows: any("NORMAL" in r for r in rows))
        return s


def check_eq(got, want, what=""):
    assert got == want, f"{what}\n got: {got!r}\nwant: {want!r}"
