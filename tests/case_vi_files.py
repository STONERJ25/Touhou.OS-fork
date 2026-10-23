"""vi: start-up options, odd files (empty, no final newline, CRLF, tabs, UTF-8, invalid bytes), read-only files, and a
100,000-line file."""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from vilib import C, R, Qemu, ScriptRunner  # noqa: E402
from harness import test  # noqa: E402

STARTUP = [
    R("+N", "a\nb\nc\n", "x", "a\nb\n\n", args="+3 /tmp/x"),
    R("+ alone starts on the last line", "a\nb\nc\n", "x", "a\nb\n\n", args="+ /tmp/x"),
    R("+N past the end", "a\nb\n", "x", "a\n\n", args="+99 /tmp/x"),
    R("+/pattern", "a\nfoo\nc\n", "x", "a\noo\nc\n", args="+/foo /tmp/x"),
    R("+/pattern not found", "a\nb\n", "x", "\nb\n", args="+/zzz /tmp/x"),
    R("-c command", "a\n", "", "b\n", args="-c 's/a/b/' /tmp/x"),
    R("two -c commands", "a\nb\n", "", "x\nb\n", args="-c 'set ts=4' -c 's/a/x/' /tmp/x"),
    R("+command", "a\nb\n", "", "a\nc\n", args="'+2s/b/c/' /tmp/x"),
    R("-R: editing is allowed, writing is not", "a\n", "x:w<CR>:q!<CR>", "a\n", args="-R /tmp/x", save=False, rc=0),
    R("-R: :q refuses after a change", "a\n", "x:q<CR>:q!<CR>", "a\n", args="-R /tmp/x", save=False, rc=0),
    R("-R: :w! writes", "a\n", "x:w!<CR>:q<CR>", "\n", args="-R /tmp/x", save=False, rc=0),
    R("-R: :set noro allows :w", "a\n", ":set noro<CR>x:wq<CR>", "\n", args="-R /tmp/x", save=False),
    R("a nonexistent file is created", "", "ihello<Esc>:wq<CR>", None, args="/tmp/new", save=False, setup="rm -f /tmp/new",
      post="cat /tmp/new", post_want="hello\n"),
    R("a nonexistent file is not created by :q", "", ":q<CR>", None, args="/tmp/new", save=False, setup="rm -f /tmp/new",
      post="ls /tmp/new 2>/dev/null; echo done", post_want="done\n"),
    R("a directory is not edited", "a\n", ":q<CR>", "a\n", args="/tmp/", save=False, rc=0),
    R("two files: the second is not touched", "a\n", "x:wq<CR>:q<CR>", "\n", args="/tmp/x /tmp/y", save=False, setup="echo y >/tmp/y",
      post="cat /tmp/y", post_want="y\n"),
    R("--version", "a\n", "", "a\n", args="--version", save=False, rc=0),
    R("-s with an unreadable script", "a\n", "", "a\n", args="-s /tmp/nonexistent-script /tmp/x", save=False, rc=2),
    R("-- ends the options", "a\n", "x", "\n", args="-- /tmp/x"),
    R(":cq exit status", "a\n", ":cq<CR>", "a\n", args="/tmp/x", save=False, rc=1),
    R("the end of the script quits with status 1", "a\n", "x", "a\n", args="/tmp/x", save=False, rc=1),
    R("vi - reads the text from standard input", "", ":w /tmp/y<CR>:q!<CR>", None, args="- </tmp/in", save=False, rc=0,
      setup="printf 'one\\ntwo\\n' >/tmp/in; rm -f /tmp/y", post="cat /tmp/y", post_want="one\ntwo\n"),
    R("vi - : the text can be edited", "", "jddix<Esc>:w /tmp/y<CR>:q!<CR>", None, args="- </tmp/in", save=False,
      setup="printf 'one\\ntwo\\nthree\\n' >/tmp/in; rm -f /tmp/y", post="cat /tmp/y", post_want="one\nxthree\n"),
    R("vi - : plain :w has no file name", "", ":w<CR>:w /tmp/y<CR>:q!<CR>", None, args="- </tmp/in", save=False,
      setup="printf 'one\\n' >/tmp/in; rm -f /tmp/y", post="ls /tmp/- 2>/dev/null; cat /tmp/y", post_want="one\n"),
    R("vi - : an empty stdin", "", "ia<Esc>:w /tmp/y<CR>:q!<CR>", None, args="- </dev/null", save=False,
      setup="rm -f /tmp/y", post="cat /tmp/y", post_want="a\n"),
]

ODD_FILES = [
    R("a read-only file cannot be written", "a\n", "x:wq<CR>:q!<CR>", None, args="/tmp/ro", save=False,
      setup="printf 'a\\n' >/tmp/ro; chmod 444 /tmp/ro", post="cat /tmp/ro", post_want="a\n"),
    R("a read-only file: :w! fails cleanly", "a\n", "x:w!<CR>:q!<CR>", None, args="/tmp/ro", save=False,
      setup="printf 'a\\n' >/tmp/ro; chmod 444 /tmp/ro", post="cat /tmp/ro", post_want="a\n"),
    R("an empty file stays empty", "", "", ""),
    R("an empty file: :q", "", ":q<CR>", "", save=False, rc=0),
    R("an empty file gets text", "", "ia<Esc>", "a\n"),
    R("an empty file: dd keeps it empty", "", "dd", ""),
    R("a file of one empty line stays", "\n", "", "\n"),
    R("no final newline: x", "abc", "x", "bc\n"),
    R("no final newline: o", "abc", "ox<Esc>", "abc\nx\n"),
    R("no final newline: dd on the last line", "a\nb", "jdd", "a\n"),
    R("no final newline kept with nofixeol", "abc", ":set nofixeol<CR>x", "bc"),
    R("no final newline: an unchanged file is completed too", "abc", "", "abc\n"),
    R("no final newline: p after the last line", "a\nb", "yyGp", "a\nb\na\n"),
    R("CRLF is hidden and written back", "a\r\nb\r\n", "x", "\r\nb\r\n"),
    R("CRLF: A appends before the CR", "a\r\nb\r\n", "A!<Esc>", "a!\r\nb\r\n"),
    R("CRLF: new lines get CRLF", "a\r\nb\r\n", "ox<Esc>", "a\r\nx\r\nb\r\n"),
    R("CRLF: :set ff=unix", "a\r\nb\r\n", ":set ff=unix<CR>", "a\nb\n"),
    R("LF file: :set ff=dos", "a\nb\n", ":set ff=dos<CR>", "a\r\nb\r\n"),
    R("mixed line endings keep their CR", "a\r\nb\n", "A!<Esc>", "a\r!\nb\n"),
    R("tab with ts=8", "\tab\n", "ix<Esc>", "x\tab\n"),
    C("tabs with ts=4", "|\tab\n", ":set ts=4<CR>5|", "\t|ab\n"),
    C("| on a tab with ts=4", "|a\tb\n", ":set ts=4<CR>5|", "a\t|b\n"),
    R("UTF-8 text: x removes a character", "日本語\n", "x", "本語\n"),
    R("UTF-8 text: l and x", "日本語\n", "lx", "日語\n"),
    R("UTF-8 text: $ and x", "日本語\n", "$x", "日本\n"),
    R("UTF-8 text: yank and put", "日本\n", "ylp", "日日本\n"),
    R("UTF-8 text: r", "日本\n", "ra", "a本\n"),
    R("UTF-8 text: ~ on accents", "éà\n", "~~", "ÉÀ\n"),
    R("UTF-8 text: cw", "日本語 text\n", "cwX<Esc>", "X text\n"),
    R("UTF-8 text: search", "日本語\n", "/本<CR>x", "日語\n"),
    R("UTF-8 text: f", "日本語\n", "f語x", "日本\n"),
    R("UTF-8 text: dw over a multibyte word", "日本語 abc\n", "dw", "abc\n"),
    R("UTF-8 text: Ctrl-V u", "x\n", "i<C-v>u65e5<Esc>", "日x\n"),
    R("UTF-8 text: typing", "", "i日本<Esc>", "日本\n"),
    R("invalid UTF-8 bytes survive", b"a\xff\xfeb\n", "x", b"\xff\xfeb\n"),
    R("deleting one invalid byte", b"a\xff\xfeb\n", "lx", b"a\xfeb\n"),
    R("yank and put an invalid byte", b"a\xffb\n", "lylP", b"a\xff\xffb\n"),
    R("replacing an invalid byte", b"a\xffb\n", "lrX", b"aXb\n"),
    R("a lone continuation byte", b"\x80a\n", "lx", b"\x80\n"),
    R("a truncated sequence at the end", b"ab\xe6\x97\n", "x", b"b\xe6\x97\n"),
    R("a NUL byte in the file", b"a\x00b\n", "x", b"\x00b\n"),
    R("a line of control characters", b"\x01\x02\n", "x", b"\x02\n"),
    R("trailing blanks are kept", "a  \nb\n", "", "a  \nb\n"),
]


def run(name, *tables):
    with Qemu(cmdline="text", name=f"t-vi-{name}") as vm:
        sr = ScriptRunner(vm)
        for t in tables:
            sr.run_rows(t)
        sr.finish(name)


@test
def vi_startup_and_files():
    """vi: start-up arguments, exit status, odd files (empty, no final newline, CRLF, tabs, UTF-8, invalid bytes)."""
    run("files", STARTUP, ODD_FILES)


@test
def vi_big_file():
    """vi: a 100,000-line file opens at once; G, :%s, :g, counts, undo, macros and a 100,000 character line stay quick."""
    with Qemu(cmdline="text", name="t-vi-big") as vm:
        sr = ScriptRunner(vm)
        sr.sh("seq 1 100000 >/tmp/big.orig; head -c 100000 /dev/zero | tr '\\0' x >/tmp/long.orig; echo >>/tmp/long.orig", timeout=120)
        orig = "wc -l </tmp/big.orig"
        checks = [
            # (name, file, keys, repeat, shell command printing the result, wanted output, seconds allowed)
            ("G and an edit on the last line", "big", "Gx", None, "tail -n 1 /tmp/big; wc -l </tmp/big", ["00000", "100000"], 60),
            ("1G and an edit on the first line", "big", "1Gx", None, "head -n 1 /tmp/big; wc -l </tmp/big", ["", "100000"], 60),
            ("500G and dd", "big", "500Gdd", None, "wc -l </tmp/big; sed -n 500p /tmp/big", ["99999", "501"], 60),
            ("a count of 100000 for dd", "big", "100000dd", None, "wc -c </tmp/big", ["0"], 60),
            (":%s on every line", "big", ":%s/1/X/g<CR>", None,
             "echo $(grep -c X /tmp/big) $(grep -c 1 /tmp/big.orig); sed -n 100000p /tmp/big", None, 180),
            (":g/9/d", "big", ":g/9/d<CR>", None, "echo $(grep -c 9 /tmp/big) $(wc -l </tmp/big) $(grep -vc 9 /tmp/big.orig)", None, 180),
            (":%s then u", "big", ":%s/1/X/g<CR>u", None, "cmp /tmp/big /tmp/big.orig && echo same", ["same"], 240),
            (":g/5/m0", "big", ":g/5/m0<CR>", None, "head -n 1 /tmp/big; wc -l </tmp/big", ["99995", "100000"], 240),
            (":g/5/d then u", "big", ":g/5/d<CR>u", None, "cmp /tmp/big /tmp/big.orig && echo same", ["same"], 240),
            ("a macro run 2000 times", "big", "qaA!<Esc>jq1999@a", None, "grep -c '!$' /tmp/big", ["2000"], 120),
            ("5000j and dd", "big", "5000jdd", None, "sed -n 5001p /tmp/big", ["5002"], 60),
            ("5000 separate j keys", "big", "dd", ("j", 5000), "sed -n 5001p /tmp/big", ["5002"], 120),
            ("a search through the file", "big", "/99999<CR>x", None, "sed -n 99999p /tmp/big", ["9999"], 60),
            (":sort n", "big", ":sort n<CR>", None, "head -n 2 /tmp/big", ["1", "2"], 240),
            (":sort", "big", ":sort<CR>", None, "head -n 3 /tmp/big", ["1", "10", "100"], 240),
            ("a 100,000 character line: $A", "long", "$A!<Esc>", None, "tail -c 3 /tmp/long; wc -c </tmp/long", ["x!", "100003"], 60),
            ("a 100,000 character line: 0x", "long", "0x", None, "wc -c </tmp/long", ["100000"], 60),
            ("a 100,000 character line: dw", "long", "dw", None, "wc -c </tmp/long", ["1"], 60),
        ]
        failures = []
        for name, f, keys, repeat, cmd, want, limit in checks:
            sr.sh(f"cp /tmp/{f}.orig /tmp/{f}", timeout=120)
            try:
                rc, took = sr.run_vi_file(f"/tmp/{f}", keys, limit, repeat=repeat)
                out, _ = sr.sh(cmd, timeout=60)
            except Exception as e:  # noqa: BLE001
                failures.append(f"[{name}] {type(e).__name__}: {str(e).strip().splitlines()[0][:200]}")
                sr.recover()
                continue
            got = out.split()
            print(f"  {name}: {took:.1f} s", flush=True)
            if want is not None and got != want:
                failures.append(f"[{name}] printed {got!r}, expected {want!r}")
            if want is None:
                if len(got) >= 2 and got[0] != got[1] and name.startswith(":%s"):
                    failures.append(f"[{name}] {got!r}: the counts differ")
                if name.startswith(":g/9") and (got[0] != "0" or got[1] != got[2]):
                    failures.append(f"[{name}] {got!r}")
            if took > limit:
                failures.append(f"[{name}] took {took:.0f} s, more than {limit} s")
        assert not failures, "\n".join(failures)
