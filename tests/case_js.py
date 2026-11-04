"""js: the JavaScript applet of kappabox (Kosuzu, user/lib/js).  Each check types one command into gapsh over the serial console
(see test_shell.py).  The applet's behaviour is covered on the host by tests/host/test_js_applet.py (under AddressSanitizer,
with pipes and a pseudo terminal); what is checked here is what only the OS can show: the program links against the real libgap
and runs in a real process, the clock, standard input from a pipe, exit statuses through the shell, the tty (Ctrl-C, the
interactive session), the memory and time limits, and the manual page.

An expected string starting with "=" must equal the whole output (trailing newline ignored); any other string only has to appear in
the output.  The exit status is checked when it is not None.
"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_shell
from harness import test, Qemu

# the commands use single quotes for the shell, so the JavaScript inside them uses double quotes and no backslashes
TABLE = [
    # --- the language and the standard library
    ("js -p '6 * 7'", "=42", 0),
    ("""js -e 'console.log(1 + 1, [1, 2], { a: "x" })'""", "=2 [ 1, 2 ] { a: 'x' }", 0),
    ("js -p '2n ** 100n'", "=1267650600228229401496703205376n", 0),
    ("js -p '[...Array(5).keys()].map(x => x * x)'", "=[ 0, 1, 4, 9, 16 ]", 0),
    ("""js -p '"The Quick Brown".replace(/(?<first>\\w)\\w*/g, "$<first>")'""", "=T Q B", 0),
    ("""js -p '/(?<=[$])\\d+/.exec("cost $42")[0]'""", "=42", 0),
    ("js -p 'new Date(Date.UTC(2024, 1, 29, 12)).toISOString()'", "=2024-02-29T12:00:00.000Z", 0),
    ("js -p 'new Date(0).getUTCFullYear() + typeof Date.now()'", "=1970number", 0),
    ("js -p 'new Uint8Array([1, 2, 300]).join()'", "=1,2,44", 0),
    ("js -p 'typeof Proxy + typeof Reflect + typeof BigInt64Array + typeof SharedArrayBuffer'", "=functionobjectfunctionfunction", 0),
    ("""js -p 'JSON.stringify({ a: [1, { b: 2 }], c: "d" })'""", """={"a":[1,{"b":2}],"c":"d"}""", 0),
    ("js -p '[1, 2, 3, 4].values().map(x => x * 2).filter(x => x > 2).toArray()'", "=[ 4, 6, 8 ]", 0),
    ("js -e 'Array.fromAsync([Promise.resolve(1), 2]).then(a => console.log(a))'", "=[ 1, 2 ]", 0),
    ("js -e 'Promise.try(() => 5).then(v => console.log(\"try\", v))'", "=try 5", 0),
    ("js -p 'String(async (a) => a)'", "=async (a) => a", 0),
    ("js -p 'Math.max(...[3, 9, 4])'", "=9", 0),
    ("js -p '[...Array(100000).keys()].reduce((a, b) => a + b)'", "=4999950000", 0),
    ("js -p '\"\\u65e5\\u672c\".length + \"\\ud83d\\ude00\".length'", "=4", 0),
    ("""js -e 'class A { #x = 1; get x() { return this.#x; } } console.log(new A().x, new A())'""", "=1 A {}", 0),
    ("js -e 'async function f() { await null; return 5 } f().then(v => console.log(\"got\", v))'", "=got 5", 0),
    ("js -e 'function* g() { yield 1; yield 2 } console.log([...g()], Object.entries({ a: 1 }))'", "=[ 1, 2 ] [ [ 'a', 1 ] ]", 0),
    # --- programs, arguments, standard input and output
    ("""printf 'console.log("hi", process.argv.slice(2).join());\\n' > /tmp/a.js; js /tmp/a.js x y""", "=hi x,y", 0),
    ("""printf '#!/bin/js\\nconsole.log("shebang");\\n' > /tmp/sb.js; js /tmp/sb.js""", "=shebang", 0),
    ("js -p 'process.argv.length' a b c", "=4", 0),
    ("""printf 'one\\ntwo\\n' | js -e 'var l; while ((l = readLine()) !== null) print(l.toUpperCase())'""", "=ONE\nTWO", 0),
    ("""printf 'a b c' | js -p 'readStdin().split(" ").length'""", "=3", 0),
    ("""echo 'console.log("from stdin")' | js""", "=from stdin", 0),
    ("""js -e 'console.error("e"); console.log("o")' 2>&1""", "=e\no", 0),
    ("""js -e 'writeFile("/tmp/j.txt", "abc"); writeFile("/tmp/j.txt", "def", true); console.log(readFile("/tmp/j.txt"))'""", "=abcdef", 0),
    ("js -e 'for (var i = 0; i < 3000; i++) console.log(i)' | tail -1", "=2999", 0),
    # --- exit statuses and errors
    ("js -e 'process.exit(7)'", "", 7),
    ("js -e 'process.exitCode = 3'", "", 3),
    ("js -e 'null.x'", "TypeError", 1),
    ("""js -e 'function f() { throw new Error("boom") } f()'""", "at f", 1),
    ("js -e 'Promise.reject(new Error(\"nope\"))'", "Uncaught (in promise) Error: nope", 1),
    ("js -e 'let = ;'", "SyntaxError", 1),
    ("js -s -e 'x = 1'", "ReferenceError", 1),
    ("""js -e 'readFile("/nonexistent")'""", "readFile", 1),
    ("js -x", "unknown option", 2),
    ("js /nonexistent.js", "cannot open", 2),
    ("js --version", "Kosuzu", 0),
    # --- timers
    ("""js -e 'setTimeout(() => console.log("late"), 100); console.log("first")'""", "=first\nlate", 0),
    ("""js -e 'var n = 0, id = setInterval(() => { console.log("tick", ++n); if (n == 3) clearInterval(id); }, 20)'""", "=tick 1\ntick 2\ntick 3", 0),
    # --- modules: files are read through the OS, relative to the importing module
    ("""mkdir -p /tmp/jm; printf 'export const n = 41; export function inc(x) { return x + 1 }\\n' > /tmp/jm/lib.mjs; """
     """printf 'import { n, inc } from "./lib.mjs";\\nconsole.log(inc(n), import.meta.filename);\\n' > /tmp/jm/main.mjs; js /tmp/jm/main.mjs""",
     "=42 /tmp/jm/main.mjs", 0),
    ("""js --module -e 'const v = await Promise.resolve(5); console.log(v * 2)'""", "=10", 0),
    ("""js -e 'import("/tmp/jm/lib.mjs").then(m => console.log(m.n, Object.keys(m)))'""", "=41 [ 'inc', 'n' ]", 0),
    ("""printf 'await null; throw new Error("late");\\n' > /tmp/jm/bad.mjs; js /tmp/jm/bad.mjs""", "Error: late", 1),
    ("""printf 'await new Promise(() => {});\\n' > /tmp/jm/hang.mjs; js /tmp/jm/hang.mjs""", "never finished", 13),
    ("""printf 'import { x } from "./lib.mjs";\\n' > /tmp/jm/missing.mjs; js /tmp/jm/missing.mjs""", "does not provide an export named", 1),
    ("""printf 'import "./nowhere.mjs";\\n' > /tmp/jm/gone.mjs; js /tmp/jm/gone.mjs""", "Cannot load module", 1),
    # --- limits
    ("js -t 1 -e 'for (;;) {}'", "time limit", 124),
    ("js -t 1 -e 'setTimeout(() => console.log(\"never\"), 20000)'", "time limit", 124),
    ("js -t 1 -e 'sleep(20000)'", "time limit", 124),
    ("js -t 1 -e '/(a+)+b/.test(\"a\".repeat(60))'", "time limit", 124),
    ("js -m 8 -e 'var a = []; for (;;) a.push(new Array(1e5).fill(1))'", "out of memory", 1),
    ("""js -m 8 -e 'var a = []; try { for (;;) a.push(new Array(1e5).fill(1)); } catch (e) { a = null; console.log("caught", e.name); }'""", "=caught RangeError", 0),
    ("js -e 'function f() { return f() + 1 } try { f() } catch (e) { console.log(e.name) }'", "=RangeError", 0),
    # --- the toolbox and the manual
    ("ls -l /bin/js", "kappabox", 0),
    ("kappabox --list | grep -w js", "js", 0),
    ("patchouli js | head -6", "JavaScript", 0),
    ("patchouli --check-all", "0 problems", 0),
]


def check(vm, table):
    failures = []
    for cmd, expect, status in table:
        try:
            out, rc = test_shell.run(vm, cmd, timeout=120)
            out = out.replace("\r", "")
            if expect.startswith("="):
                assert out.rstrip("\n") == expect[1:], f"expected {expect[1:]!r}, got {out!r}"
            else:
                assert expect in out, f"expected {expect!r} in {out!r}"
            assert status is None or rc == status, f"exit {rc}, expected {status}; output {out!r}"
        except Exception as e:  # noqa: BLE001
            failures.append(f"{cmd}: {str(e)[-300:]}")
    return failures


@test
def js_checks():
    """js: the language and library, programs and arguments, standard input, exit statuses, errors, timers, limits, the manual."""
    with Qemu(cmdline="text", name="t-js") as vm:
        vm.expect(test_shell.PROMPT, timeout=300)
        failures = check(vm, TABLE)
    assert not failures, "\n".join(failures)


@test
def js_session():
    """js: the interactive session on the serial console: prompt, values, multi-line input, errors, history, Ctrl-C, Ctrl-D."""
    with Qemu(cmdline="text", name="t-js-repl") as vm:
        vm.expect(test_shell.PROMPT, timeout=300)
        vm.send("js\n")
        vm.expect(r"Type \.help", timeout=60)
        vm.expect(r"> ")
        vm.send("6 * 7\n")
        vm.expect(r"\n42\r?\n", timeout=30)
        vm.expect(r"> ")
        vm.send("function f(n) {\n")
        vm.expect(r"\.\.\. ")
        vm.send("  return n + 1;\n")
        vm.expect(r"\.\.\. ")
        vm.send("}\n")
        vm.expect(r"undefined\r?\n", timeout=30)
        vm.expect(r"> ")
        vm.send("f(41)\n")
        vm.expect(r"\n42\r?\n", timeout=30)
        vm.expect(r"> ")
        vm.send("({ list: [1, 2, 3], text: 'x' })\n")
        vm.expect(r"\{ list: \[ 1, 2, 3 \], text: 'x' \}", timeout=30)
        vm.expect(r"> ")
        vm.send("null.f()\n")
        vm.expect(r"Uncaught TypeError", timeout=30)
        vm.expect(r"> ")
        vm.send("\x1b[A\x1b[A\n")                        # Up twice: the line before the last one, run again
        vm.expect(r"\{ list: \[ 1, 2, 3 \], text: 'x' \}", timeout=30)
        vm.expect(r"> ")
        vm.send("for (;;) {}\n")
        time.sleep(1.5)
        vm.send("\x03")                                  # Ctrl-C stops the running program, not the session
        vm.expect(r"interrupted", timeout=30)
        vm.expect(r"> ")
        vm.send("'alive'\n")
        vm.expect(r"'alive'", timeout=30)
        vm.expect(r"> ")
        vm.send("\x04")                                  # Ctrl-D leaves
        vm.expect(test_shell.PROMPT, timeout=30)
