#!/usr/bin/env python3
"""End-to-end checks for gapsh and the userland tools, typing over the serial console.

Each command is wrapped in begin/end markers (written with a quote in the middle so the tty's echo of the command line
does not match), which gives us the exact output and exit status of every command.
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
from qemu import Qemu, BUILD  # noqa: E402

PROMPT = r"reimu@gensokyo \S+ \$ "


def run(vm, cmd, timeout=25):
    """Runs one shell command line in the guest. Returns (output, exit_status)."""
    vm.send(f"echo __BE''GIN__; {cmd}; echo __EN''D__$?\n")
    m = vm.expect(r"\n__BEGIN__\n(.*?)__END__(\d+)\n", timeout=timeout)
    vm.expect(PROMPT, timeout=10)           # let the shell get back to its prompt before the next line is typed
    return m.group(1), int(m.group(2))


# (command, substring that must appear in the output, expected exit status or None)
CHECKS = [
    # --- shell language
    ("echo hello gensokyo", "hello gensokyo", 0),
    ("echo $((6*7)) ${HOME} $USER", "42 /HumanVillage/reimu reimu", 0),
    ("pwd", "/HumanVillage/reimu", 0),
    ("cd /Hakurei && pwd", "/Hakurei", 0),
    ("x=5; if [ $x -gt 3 ]; then echo big; else echo small; fi", "big", 0),
    ("for i in 1 2 3; do printf '%s,' $i; done; echo", "1,2,3,", 0),
    ("echo `echo nested`$(echo sub)", "nestedsub", 0),
    ("false", "", 1),
    ("nosuchcommand", "not found", 127),
    ("f() { echo fn-$1; }; f arg", "fn-arg", 0),
    ("case abc in a*) echo matched;; *) echo no;; esac", "matched", 0),
    ("i=0; while [ $i -lt 3 ]; do i=$((i+1)); done; echo $i", "3", 0),
    ("echo 'single $quoted' \"double $USER\"", "single $quoted double reimu", 0),
    ("echo a; echo b >&2; echo c", "a", 0),
    ("(exit 3); echo rc=$?", "rc=3", 0),
    ("echo ${undefined:-fallback} ${#USER}", "fallback 5", 0),
    # --- files and redirection
    ("echo foo > /tmp/t.txt; cat /tmp/t.txt", "foo", 0),
    ("echo one > /tmp/a; echo two >> /tmp/a; cat /tmp/a", "one\ntwo", 0),
    ("mkdir -p /tmp/d/e/f && ls /tmp/d/e", "f", 0),
    ("cp /tmp/a /tmp/b && mv /tmp/b /tmp/c && cat /tmp/c", "one", 0),
    ("rm /tmp/c; ls /tmp/c", "No such file", None),
    ("ln -s /tmp/a /tmp/lnk && readlink /tmp/lnk", "/tmp/a", 0),
    ("ls /", "Hakurei", 0),
    ("ls -l /Hakurei/etc | grep passwd", "passwd", 0),
    ("touch /tmp/x1 /tmp/x2; ls /tmp/x*", "/tmp/x1", 0),
    ("chmod 600 /tmp/a; stat /tmp/a", "600", 0),
    # --- pipelines and text tools
    ("echo a b c | wc -w", "3", 0),
    ("printf 'b\\na\\nc\\n' | sort", "a\nb\nc", 0),
    ("printf 'b\\na\\nb\\n' | sort | uniq -c", "2 b", 0),
    ("printf '3\\n10\\n2\\n' | sort -n | tail -1", "10", 0),
    ("printf 'a:b:c\\n' | cut -d: -f2", "b", 0),
    ("echo hello | tr a-z A-Z", "HELLO", 0),
    ("printf '1\\n2\\n3\\n4\\n' | head -2", "1\n2", 0),
    ("printf '1\\n2\\n3\\n4\\n' | tail -n 2", "3\n4", 0),
    ("printf 'x\\ny\\n' | tac", "y\nx", 0),
    ("echo abc | rev", "cba", 0),
    ("seq 1 5 | wc -l", "5", 0),
    ("echo hi | tee /tmp/tee.txt; cat /tmp/tee.txt", "hi", 0),
    # --- system tools
    ("id", "uid=1000(reimu) gid=1000(reimu)", 0),
    ("whoami", "reimu", 0),
    ("uname -s", "Gensokyo", 0),
    ("hostname", "gensokyo", 0),
    ("date +%Y | wc -c", "5", 0),
    ("free | head -2", "total", 0),
    ("uptime", "up", 0),
    ("ps", "gapsh", 0),
    ("env | grep ^USER=", "USER=reimu", 0),
    ("dmesg | head -3", "[", 0),
    ("mount", "tmpfs", 0),
    ("which ls", "/bin/ls", 0),
    # --- grep
    ("printf 'apple\\nbanana\\ncherry\\n' | grep an", "banana", 0),
    ("printf 'apple\\nbanana\\ncherry\\n' | grep -v an | wc -l", "2", 0),
    ("printf 'apple\\nbanana\\n' | grep -c a", "2", 0),
    ("printf 'Apple\\n' | grep -i apple", "Apple", 0),
    ("printf 'a1\\nb2\\nc\\n' | grep -E '[a-z][0-9]' | wc -l", "2", 0),
    ("echo 'foo boo' | grep -o 'o*'", "oo", 0),
    ("echo abc | grep -q b; echo q=$?", "q=0", 0),
    ("echo abc | grep -q z; echo q=$?", "q=1", 0),
    ("printf 'a\\nb\\nc\\nd\\ne\\n' | grep -n -C1 c", "2-b", 0),
    ("grep -rl reimu /Hakurei/etc | sort", "/Hakurei/etc/passwd", 0),
    ("printf 'one two\\n' | grep -w two", "one two", 0),
    ("printf 'ab\\nabc\\n' | grep -x ab", "ab", 0),
    # --- sed
    ("echo hello world | sed 's/world/gensokyo/'", "hello gensokyo", 0),
    ("printf 'a\\nb\\nc\\n' | sed -n '2p'", "b", 0),
    ("printf 'a\\nb\\nc\\n' | sed '2d' | wc -l", "2", 0),
    ("echo 'foo bar' | sed -E 's/(foo) (bar)/\\2 \\1/'", "bar foo", 0),
    ("printf 'x\\ny\\n' | sed '$!d'", "y", 0),
    ("printf '1\\n2\\n3\\n4\\n' | sed -n '2,3p'", "2\n3", 0),
    ("echo aaa | sed 's/a/b/2'", "aba", 0),
    ("echo abc | sed 'y/abc/xyz/'", "xyz", 0),
    ("printf 'a\\nb\\n' | sed -n '$='", "2", 0),
    ("echo hello | sed -e 's/h/J/' -e 's/o$/y/'", "Jelly", 0),
    ("echo aXb | sed 's/x/-/I'", "a-b", 0),
    # --- find / xargs
    ("mkdir -p /tmp/ft/sub; touch /tmp/ft/a.txt /tmp/ft/sub/b.txt; find /tmp/ft -name '*.txt' | sort", "/tmp/ft/a.txt\n/tmp/ft/sub/b.txt", 0),
    ("find /tmp/ft -type d | sort", "/tmp/ft/sub", 0),
    ("find /tmp/ft -name b.txt -exec echo found {} \\;", "found /tmp/ft/sub/b.txt", 0),
    ("find /tmp/ft -maxdepth 1 -type f", "/tmp/ft/a.txt", 0),
    ("echo a b c | xargs -n1 echo | wc -l", "3", 0),
    ("printf 'x\\ny\\n' | xargs -I{} echo got-{}", "got-x\ngot-y", 0),
    # --- misc tools
    ("expr 6 '*' 7", "42", 0),
    ("expr length hello", "5", 0),
    ("expr abc : 'a\\(.\\)c'", "b", 0),
    ("echo -n hi | base64", "aGk=", 0),
    ("echo aGk= | base64 -d", "hi", 0),
    ("echo -n abc | md5sum", "900150983cd24fb0d6963f7d28e17f72", 0),
    ("echo -n abc | sha256sum", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", 0),
    ("echo -n abc | od -An -tx1", "61 62 63", 0),
    ("echo -n abc | xxd -p", "616263", 0),
    ("cal 2 2024 | head -1", "February 2024", 0),
    ("basename /a/b/c.txt .txt", "c", 0),
    ("dirname /a/b/c.txt", "/a/b", 0),
    ("sleep 0.2; echo woke", "woke", 0),
    ("echo 'a b' | tr ' ' '\\n' | sort -r | head -1", "b", 0),
    ("printf 'one\\ntwo\\n' | paste -s -d,", "one,two", 0),
    ("echo x > /tmp/trashme; trash /tmp/trashme; ls /Netherworld", "trashme", 0),
]


def run_checks(want="", extra_checks=(), verbose=True, **qemu_args):
    """Boots the system and runs every check. Returns the list of failure descriptions (empty = all passed)."""
    failures = []
    with Qemu(cmdline="text", name="t-shell", **qemu_args) as vm:
        vm.expect(PROMPT, timeout=90)
        for cmd, expect, status in list(CHECKS) + list(extra_checks):
            if want and want not in cmd:
                continue
            try:
                out, rc = run(vm, cmd)
                if expect not in out:
                    raise AssertionError(f"expected {expect!r} in output:\n{out}")
                if status is not None and rc != status:
                    raise AssertionError(f"exit status {rc}, expected {status}; output:\n{out}")
                if verbose:
                    print("PASS", cmd)
            except Exception as e:  # noqa: BLE001
                failures.append(f"{cmd}\n    {str(e)[-900:]}")
                print("FAIL", cmd, "\n   ", str(e)[-900:].replace("\n", "\n    "))
        vm.screenshot(BUILD / "t-shell.png")
    return failures


def main():
    want = sys.argv[1] if len(sys.argv) > 1 else ""
    failures = run_checks(want)
    print(f"{len(failures)} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
