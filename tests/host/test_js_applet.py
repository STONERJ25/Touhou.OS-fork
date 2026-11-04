#!/usr/bin/env python3
"""Host tests of the `js` applet (user/bin/kappabox/js.c), built for the host with tests/host/js_applet_host.c.

    python tools/hosttest.py discover -s tests/host -p "test_js_applet.py" -v

The applet is ordinary POSIX C on top of the engine's embedding API, so most of it can be tested without booting the OS: the
real code runs here under AddressSanitizer and UBSan with pipes (programs, standard input, exit statuses, limits, timers, files)
and with a pseudo terminal (the interactive session: line editing, history, multi-line input, Ctrl-C).  tests/case_js.py then
checks what only the OS can show: that the applet is in kappabox, the manual page, the real libgap.
"""
import fcntl
import os
import pty
import re
import select
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_js  # noqa: E402  (the engine's sources and the output directory)

REPO = test_js.REPO
KB = REPO / "user" / "bin" / "kappabox"


def build_applet():
    exe = test_js.OUT / "jsapp"
    srcs = sorted((test_js.JS / "src").glob("*.c")) + [KB / "js.c", REPO / "tests" / "host" / "js_applet_host.c"]
    deps = srcs + sorted((test_js.JS / "src").glob("*.h")) + sorted((test_js.JS / "include").glob("*.h")) + [KB / "js_prelude.h", KB / "kappabox.h"]
    test_js.OUT.mkdir(parents=True, exist_ok=True)
    if exe.exists() and all(d.stat().st_mtime <= exe.stat().st_mtime for d in deps):
        return exe
    cc = test_js.find_cc()
    gcc = "gcc" in Path(cc).name
    flags = ["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"] if gcc else ["-O2", "-g"]
    cmd = [cc, *flags, "-std=gnu17", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-sign-compare", "-Wno-missing-field-initializers",
           "-I", str(test_js.JS / "include"), "-I", str(REPO / "tests" / "host" / "kbshim"), "-I", str(KB), *map(str, srcs), "-lm", "-o", str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("build of the js applet failed:\n%s%s" % (r.stdout, r.stderr[-6000:]))
    return exe


def jsrun(*args, stdin=b"", env=None, timeout=120, cwd=None):
    e = dict(os.environ, ASAN_OPTIONS="detect_leaks=0", TZ="UTC")
    if env:
        e.update(env)
    r = subprocess.run([str(build_applet()), *args], input=stdin, capture_output=True, timeout=timeout, env=e, cwd=cwd)
    return r.returncode, r.stdout.decode("utf-8", "replace"), r.stderr.decode("utf-8", "replace")


class Programs(unittest.TestCase):
    def check(self, args, out=None, rc=0, err=None, stdin=b"", **kw):
        got_rc, got_out, got_err = jsrun(*args, stdin=stdin, **kw)
        self.assertNotIn("AddressSanitizer", got_err, got_err[-2000:])
        self.assertNotIn("runtime error", got_err, got_err[-2000:])
        if out is not None:
            self.assertEqual(got_out, out, got_err[-800:])
        if err is not None:
            self.assertIn(err, got_err)
        self.assertEqual(got_rc, rc, got_out[-300:] + got_err[-800:])
        return got_out, got_err

    def test_eval_and_print(self):
        self.check(["-e", "console.log(1 + 1)"], "2\n")
        self.check(["-p", "6 * 7"], "42\n")
        self.check(["-p", "'x'.repeat(3)"], "xxx\n")
        self.check(["-p", "[1, 2, 3].map(x => x * 2)"], "[ 2, 4, 6 ]\n")
        self.check(["-p", "undefined"], "undefined\n")
        self.check(["-p", "2n ** 100n"], "1267650600228229401496703205376n\n")
        self.check(["-p", "/a(b)/.exec('ab')"], "[ 'ab', 'b', index: 0, input: 'ab', groups: undefined ]\n")
        self.check(["-p", "new Date(0)"], "1970-01-01T00:00:00.000Z\n")
        self.check(["-p", "({ a: 1, b: [1, 2, { c: 3 }] })"], "{ a: 1, b: [ 1, 2, { c: 3 } ] }\n")

    def test_arguments(self):
        self.check(["-e", "console.log(process.argv.length, process.argv.slice(1).join(','))", "a", "b c"], "3 a,b c\n")
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "prog.js"
            p.write_text("console.log(process.argv.slice(1).map(function (a) { return a === process.argv[1] ? 'script' : a; }).join(','))\n")
            self.check([str(p), "x", "-y", "--", "z"], "script,x,-y,--,z\n")
            self.check(["-s", str(p)], "script\n")

    def test_program_from_standard_input(self):
        self.check([], "from stdin\n", stdin=b"console.log('from stdin')")
        self.check(["-"], "again 1\n", stdin=b"console.log('again', process.argv.length)")
        self.check(["-p", "readStdin().length"], "5\n", stdin=b"hello")

    def test_shebang_and_strict(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "s.js"
            p.write_text("#!/usr/bin/env js\nconsole.log('ok')\n")
            self.check([str(p)], "ok\n")
        self.check(["-s", "-e", "x = 1"], err="ReferenceError", rc=1)
        self.check(["-e", "x = 1; console.log(x)"], "1\n")

    def test_exit_status(self):
        self.check(["-e", "process.exit(7)"], rc=7)
        self.check(["-e", "process.exitCode = 3; console.log('x')"], "x\n", rc=3)
        self.check(["-e", "process.exitCode = 3; process.exit()"], rc=3)
        self.check(["-e", "console.log('before'); process.exit(); console.log('after')"], "before\n")
        self.check(["-e", "setTimeout(function () { process.exit(9); }, 10); setTimeout(function () { console.log('no'); }, 500)"], "", rc=9)

    def test_errors(self):
        out, err = self.check(["-e", "function f() { throw new TypeError('bad'); } f()"], rc=1, err="TypeError: bad")
        self.assertIn("at f (", err)
        self.check(["-e", "throw 5"], rc=1, err="5")
        self.check(["-e", "let = ;"], rc=1, err="SyntaxError")
        self.check(["-e", "Promise.reject(new Error('nope'))"], rc=1, err="Uncaught (in promise) Error: nope")
        self.check(["-e", "setTimeout(function () { throw new Error('in timer'); }, 1); setTimeout(function () { console.log('not run'); }, 50)"],
                   "", rc=1, err="in timer")
        self.check(["-e", "console.error('to stderr'); console.log('to stdout')"], "to stdout\n", err="to stderr")
        self.check(["-e", "null.x"], rc=1, err="TypeError")

    def test_usage_errors(self):
        self.check(["-x"], rc=2, err="unknown option")
        self.check(["/nonexistent/prog.js"], rc=2, err="cannot open")
        self.check(["-m", "abc", "-e", "1"], rc=2, err="bad memory limit")
        self.check(["-t", "-1", "-e", "1"], rc=2, err="bad time limit")
        got = self.check(["--help"], rc=0)[0]
        self.assertIn("usage: js", got)
        self.check(["--version"], rc=0)

    def test_time_limit(self):
        t0 = time.time()
        self.check(["-t", "0.3", "-e", "for (;;) {}"], rc=124, err="time limit of 0.3 seconds")
        self.assertLess(time.time() - t0, 30)
        self.check(["-t", "0.2", "-e", "setTimeout(function () { console.log('never'); }, 5000)"], "", rc=124, err="time limit")
        self.check(["-t", "0.2", "-e", "sleep(5000)"], rc=124, err="time limit")
        self.check(["-t", "0.3", "-e", "try { for (;;) {} } catch (e) { console.log('CAUGHT'); }"], "", rc=124)
        self.check(["-t", "5", "-e", "console.log('fast')"], "fast\n")

    def test_memory_limit(self):
        self.check(["-m", "8", "-e", "var a = []; for (;;) a.push(new Array(1e5).fill(1))"], rc=1, err="RangeError: out of memory")
        self.check(["-m", "8", "-e", "var a = []; try { for (;;) a.push(new Array(1e5).fill(1)); } catch (e) { a = null; console.log('caught', e instanceof RangeError); }"], "caught true\n")
        self.check(["-m", "0", "-e", "console.log(new Array(1e6).fill(1).length)"], "1000000\n")

    def test_timers(self):
        self.check(["-e", "setTimeout(() => console.log('late'), 50); setImmediate(() => console.log('imm')); Promise.resolve().then(() => console.log('micro'));"
                          "console.log('first'); var n = 0; var id = setInterval(() => { console.log('tick', ++n); if (n == 3) clearInterval(id); }, 10);"],
                   "first\nmicro\nimm\ntick 1\ntick 2\ntick 3\nlate\n")
        self.check(["-e", "var t = setTimeout(() => console.log('cleared'), 10); clearTimeout(t); setTimeout((a, b) => console.log(a + b), 5, 1, 2);"], "3\n")
        self.check(["-e", "queueMicrotask(() => console.log('qm')); process.nextTick(() => console.log('tick')); console.log('sync')"], "sync\nqm\ntick\n")
        self.check(["-e", "setTimeout(() => console.log('b'), 20); setTimeout(() => console.log('a'), 10); setTimeout(() => console.log('c'), 20);"], "a\nb\nc\n")
        self.check(["-e", "async function f() { await new Promise(r => setTimeout(r, 20)); return 'awaited'; } f().then(console.log)"], "awaited\n")
        self.check(["-e", "setTimeout('x', 1)"], rc=1, err="callback")

    def test_input_and_files(self):
        self.check(["-e", "var l; while ((l = readLine()) !== null) console.log('got', l.toUpperCase())"], "got ONE\ngot TWO\ngot \ngot LAST\n", stdin=b"one\ntwo\r\n\nlast")
        self.check(["-e", "console.log(readLine(), readLine() === null)"], "x true\n", stdin=b"x\n")
        self.check(["-e", "console.log(readLine(), JSON.stringify(readStdin()))"], 'a "b\\nc\\n"\n', stdin=b"a\nb\nc\n")
        with tempfile.TemporaryDirectory() as d:
            f = str(Path(d) / "t.txt")
            self.check(["-e", "writeFile(%r, 'hÃ©llo ã“ã‚“ã«ã¡ã¯\\n'); writeFile(%r, 'more\\n', true); console.log(JSON.stringify(readFile(%r)))" % (f, f, f)],
                       '"hÃ©llo ã“ã‚“ã«ã¡ã¯\\nmore\\n"\n')
            self.assertEqual(Path(f).read_text(encoding="utf-8"), "hÃ©llo ã“ã‚“ã«ã¡ã¯\nmore\n")
            self.check(["-e", "try { readFile(%r) } catch (e) { console.log(e.message.slice(0, 20)) }" % (d + "/missing")], "readFile: " + d[:10] + "\n")
            self.check(["-e", "console.log(process.cwd() === %r)" % d], "true\n", cwd=d)

    def test_environment(self):
        self.check(["-e", "console.log(process.env.JS_TEST_VAR, process.env.JS_TEST_UNSET)"], "hello undefined\n", env={"JS_TEST_VAR": "hello"})
        self.check(["-e", "console.log(new Date(0).toString())"], "Thu Jan 01 1970 09:00:00 GMT+0900 (JST)\n", env={"TZ": "Asia/Tokyo"})
        self.check(["-e", "console.log(new Date(0).getTimezoneOffset(), new Date(2020, 6, 1).getTimezoneOffset())"], "300 240\n", env={"TZ": "America/New_York"})

    def test_output_is_complete_and_ordered(self):
        out, err = self.check(["-e", "for (var i = 0; i < 30000; i++) { console.log(i); if (i % 10000 == 9999) console.error('e' + i); }"])
        lines = out.split("\n")
        self.assertEqual(lines[:3], ["0", "1", "2"])
        self.assertEqual(lines[29999], "29999")
        self.assertEqual(err.split("\n")[:3], ["e9999", "e19999", "e29999"])
        self.check(["-e", "process.stdout.write('a'); process.stderr.write('E'); process.stdout.write('b\\n');"], "ab\n", err="E")

    def test_big_values(self):
        self.check(["-e", "console.log(JSON.stringify(Array.from({ length: 100000 }, (_, i) => i)).length)"], "588891\n")
        self.check(["-e", "var s = ''; for (var i = 0; i < 100000; i++) s += 'x'; console.log(s.length)"], "100000\n")


class Modules(unittest.TestCase):
    """ES modules in the applet: files are modules when they end in .mjs or with --module; imports are read from disk."""

    def run_in(self, files, args, **kw):
        with tempfile.TemporaryDirectory() as d:
            for name, text in files.items():
                p = Path(d) / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(text, encoding="utf-8")
            rc, out, err = jsrun(*args, cwd=d, **kw)
            self.assertNotIn("AddressSanitizer", err, err[-2000:])
            self.assertNotIn("runtime error", err, err[-2000:])
            return rc, out.replace(d, "<dir>"), err.replace(d, "<dir>")

    def test_mjs_file_with_imports(self):
        rc, out, err = self.run_in({
            "main.mjs": "import { add, name } from './lib/math.mjs';\nimport def from './lib/def.mjs';\nconsole.log(add(2, 3), name, def);\n",
            "lib/math.mjs": "export const name = 'math'; export function add(a, b) { return a + b; }\n",
            "lib/def.mjs": "import { name } from './math.mjs';\nexport default `default of ${name}`;\n",
        }, ["main.mjs"])
        self.assertEqual((rc, out), (0, "5 math default of math\n"), err)

    def test_module_flag_and_other_extension(self):
        rc, out, err = self.run_in({"prog.js": "import { x } from './dep.js'; console.log(x, typeof require, this);\n", "dep.js": "export const x = 7;\n"},
                                   ["--module", "prog.js"])
        self.assertEqual((rc, out), (0, "7 undefined undefined\n"), err)
        rc, out, err = self.run_in({"prog.js": "import { x } from './dep.js';\n", "dep.js": "export const x = 7;\n"}, ["prog.js"])
        self.assertEqual(rc, 1)                             # a .js file is a script: import is a syntax error there
        self.assertIn("SyntaxError", err)

    def test_import_meta_and_relative_paths(self):
        rc, out, err = self.run_in({
            "sub/m.mjs": "console.log(import.meta.url, import.meta.filename, import.meta.dirname);\nimport './n.mjs';\nimport '../top.mjs';\n",
            "sub/n.mjs": "console.log('n', import.meta.filename);\n",
            "top.mjs": "console.log('top', import.meta.dirname);\n",
        }, ["sub/m.mjs"])
        self.assertEqual(out.splitlines(), ["n <dir>/sub/n.mjs", "top <dir>", "file://<dir>/sub/m.mjs <dir>/sub/m.mjs <dir>/sub"], err)
        self.assertEqual(rc, 0)

    def test_eval_and_standard_input_as_modules(self):
        rc, out, err = self.run_in({}, ["--module", "-e", "const v = await Promise.resolve(40); console.log(v + 2, import.meta.filename.endsWith('[eval]'));"])
        self.assertEqual((rc, out), (0, "42 true\n"), err)
        rc, out, err = self.run_in({}, ["--module", "-"], stdin=b"export const a = 1; console.log(await 'stdin module', a);")
        self.assertEqual((rc, out), (0, "stdin module 1\n"), err)
        rc, out, err = self.run_in({}, ["--module", "-p", "1"])
        self.assertEqual(rc, 2)

    def test_top_level_await_with_timers(self):
        rc, out, err = self.run_in({"m.mjs": "console.log('start');\nawait new Promise(r => setTimeout(r, 30));\nconsole.log('after the timer');\nsetTimeout(() => console.log('timer after the module'), 10);\n"}, ["m.mjs"])
        self.assertEqual((rc, out), (0, "start\nafter the timer\ntimer after the module\n"), err)

    def test_dynamic_import_from_a_script_and_a_module(self):
        files = {"s.js": "import('./sub/m.mjs').then(ns => console.log('script got', ns.v, Object.keys(ns)));\nconsole.log('script done');\n",
                 "sub/m.mjs": "console.log('m runs');\nexport const v = 5;\nexport default 1;\n",
                 "d.mjs": "const { default: one, v } = await import('./sub/m.mjs');\nconsole.log(one, v);\n"}
        rc, out, err = self.run_in(files, ["s.js"])
        self.assertEqual((rc, out), (0, "script done\nm runs\nscript got 5 [ 'default', 'v' ]\n"), err)
        rc, out, err = self.run_in(files, ["d.mjs"])
        self.assertEqual((rc, out), (0, "m runs\n1 5\n"), err)

    def test_uncaught_error_in_a_module(self):
        rc, out, err = self.run_in({"m.mjs": "import './dep.mjs';\nconsole.log('not reached');\n", "dep.mjs": "console.log('dep');\nthrow new RangeError('boom');\n"}, ["m.mjs"])
        self.assertEqual((rc, out), (1, "dep\n"))
        self.assertIn("RangeError: boom", err)
        self.assertIn("dep.mjs", err)

    def test_rejection_after_a_timer(self):
        rc, out, err = self.run_in({"m.mjs": "setTimeout(() => console.log('timer'), 200);\nawait new Promise(r => setTimeout(r, 20));\nthrow new Error('late failure');\n"}, ["m.mjs"])
        self.assertEqual(rc, 1)
        self.assertEqual(out, "")                          # the failure ends the run: the later timer never runs
        self.assertIn("Error: late failure", err)
        self.assertEqual(err.count("late failure"), 1)    # reported once

    def test_link_and_load_errors(self):
        rc, out, err = self.run_in({"m.mjs": "import { nope } from './dep.mjs';\n", "dep.mjs": "export const yes = 1;\n"}, ["m.mjs"])
        self.assertEqual(rc, 1)
        self.assertIn("does not provide an export named 'nope'", err)
        rc, out, err = self.run_in({"m.mjs": "import './gone.mjs';\n"}, ["m.mjs"])
        self.assertEqual(rc, 1)
        self.assertIn("Cannot load module './gone.mjs'", err)
        rc, out, err = self.run_in({"m.mjs": "import 'bare';\n"}, ["m.mjs"])
        self.assertEqual(rc, 1)
        self.assertIn("Cannot load module 'bare'", err)

    def test_syntax_error_in_the_entry(self):
        rc, out, err = self.run_in({"m.mjs": "let x = ;\n"}, ["m.mjs"])
        self.assertEqual(rc, 1)
        self.assertIn("SyntaxError", err)

    def test_unsettled_top_level_await(self):
        rc, out, err = self.run_in({"m.mjs": "console.log('before');\nawait new Promise(() => {});\nconsole.log('never');\n"}, ["m.mjs"])
        self.assertEqual((rc, out), (13, "before\n"))
        self.assertIn("never finished", err)

    def test_time_limit_in_a_module(self):
        rc, out, err = self.run_in({"m.mjs": "import './spin.mjs';", "spin.mjs": "while (true) {}"}, ["-t", "0.3", "m.mjs"])
        self.assertEqual(rc, 124)
        self.assertIn("time limit", err)
        rc, out, err = self.run_in({"m.mjs": "await null; while (true) {}"}, ["-t", "0.3", "m.mjs"])
        self.assertEqual(rc, 124)

    def test_process_exit_and_exit_code_in_a_module(self):
        rc, out, err = self.run_in({"m.mjs": "process.exitCode = 4; console.log('x');"}, ["m.mjs"])
        self.assertEqual((rc, out), (4, "x\n"))
        rc, out, err = self.run_in({"m.mjs": "await null; process.exit(9); console.log('no');"}, ["m.mjs"])
        self.assertEqual((rc, out), (9, ""))

    def test_the_same_module_runs_once_and_cycles_work(self):
        rc, out, err = self.run_in({
            "main.mjs": "import './a.mjs'; import './b.mjs'; import './a.mjs'; console.log('main');",
            "a.mjs": "import { b } from './b.mjs'; export const a = 'A'; console.log('a runs, b =', b);",
            "b.mjs": "import { a } from './a.mjs'; export const b = 'B'; console.log('b runs', typeof a === 'string');"
        }, ["main.mjs"])
        # b runs first (a is not initialised yet: reading it would throw), then a, then main
        self.assertEqual(rc, 1)
        self.assertIn("ReferenceError", err)
        rc, out, err = self.run_in({
            "main.mjs": "import './a.mjs'; import './b.mjs'; import './a.mjs'; console.log('main');",
            "a.mjs": "import { b } from './b.mjs'; export const a = 'A'; console.log('a runs, b =', b);",
            "b.mjs": "export const b = 'B'; console.log('b runs');"
        }, ["main.mjs"])
        self.assertEqual((rc, out), (0, "b runs\na runs, b = B\nmain\n"), err)


class Terminal:
    """The applet on a pseudo terminal, driven like a person."""

    def __init__(self, *args, env=None, rows=24, cols=80):
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        e = dict(os.environ, ASAN_OPTIONS="detect_leaks=0", TZ="UTC", TERM="xterm", HOME=tempfile.mkdtemp(prefix="jshome"))
        if env:
            e.update(env)
        self.home = e["HOME"]
        self.proc = subprocess.Popen([str(build_applet()), *args], stdin=slave, stdout=slave, stderr=slave, env=e, close_fds=True,
                                     start_new_session=True, preexec_fn=lambda: fcntl.ioctl(0, termios.TIOCSCTTY, 0))
        os.close(slave)
        self.buf = b""

    def read(self, timeout=0.2):
        end = time.time() + timeout
        while time.time() < end:
            r, _, _ = select.select([self.master], [], [], max(0, end - time.time()))
            if not r:
                break
            try:
                data = os.read(self.master, 65536)
            except OSError:
                break
            if not data:
                break
            self.buf += data
        return self.buf

    def expect(self, text, timeout=30):
        if isinstance(text, str):
            text = text.encode()
        end = time.time() + timeout
        while time.time() < end:
            if text in self.buf:
                i = self.buf.index(text) + len(text)
                got, self.buf = self.buf[:i], self.buf[i:]
                return got.decode("utf-8", "replace")
            self.read(0.1)
        raise AssertionError("did not see %r; got %r" % (text, self.buf[-600:]))

    def send(self, data):
        os.write(self.master, data if isinstance(data, bytes) else data.encode())

    def line(self, text):
        self.send(text + "\r")

    def close(self):
        try:
            if self.proc.poll() is None:
                self.proc.kill()
            self.proc.wait(timeout=10)
        finally:
            os.close(self.master)
            shutil.rmtree(self.home, ignore_errors=True)


class Interactive(unittest.TestCase):
    def start(self, *args, **kw):
        t = Terminal(*args, **kw)
        self.addCleanup(t.close)
        t.expect("Type .help")
        t.expect("> ")
        return t

    def test_values_and_errors(self):
        t = self.start()
        t.line("1 + 1")
        t.expect("2\r\n")
        t.expect("> ")
        t.line("'a' + 'b'")
        t.expect("'ab'\r\n")
        t.line("({ x: [1, 2, 3], y: 'z' })")
        t.expect("{ x: [ 1, 2, 3 ], y: 'z' }")
        t.line("undefined")
        t.expect("undefined\r\n")
        t.line("null.f()")
        t.expect("Uncaught TypeError")
        t.expect("> ")
        t.line("var answer = 6 * 7")
        t.line("answer")
        t.expect("42\r\n")
        t.line("_")
        t.expect("42\r\n")
        t.line("let z = 5; const w = 6;")
        t.line("z * w")
        t.expect("30\r\n")
        t.line("class P { constructor() { this.v = 1; } }")
        t.line("new P()")
        t.expect("P { v: 1 }")
        t.line("123n ** 3n")
        t.expect("1860867n")
        t.line("[1, 2, 3].map(x => x * 2)")
        t.expect("[ 2, 4, 6 ]")

    def test_multiline_input(self):
        t = self.start()
        t.line("function add(a, b) {")
        t.expect("... ")
        t.line("  return a + b;")
        t.expect("... ")
        t.line("}")
        t.expect("undefined\r\n")
        t.expect("> ")
        t.line("add(2, 3)")
        t.expect("5\r\n")
        t.line("var o = {")
        t.expect("... ")
        t.line("  a: 1,")
        t.line("  b: [")
        t.line("    2")
        t.line("  ]")
        t.line("}")
        t.expect("> ")
        t.line("o")
        t.expect("{ a: 1, b: [ 2 ] }")
        t.line("`one")
        t.expect("... ")
        t.line("two`")
        t.expect("'one\\ntwo'")
        t.line("if (")
        t.expect("... ")
        t.line("1 +")
        t.expect("... ")
        t.line(")")
        t.expect("Uncaught SyntaxError")
        t.expect("> ")

    def test_line_editing_and_history(self):
        t = self.start()
        t.line("10 + 1")
        t.expect("11\r\n")
        t.line("20 + 2")
        t.expect("22\r\n")
        t.send(b"\x1b[A")                       # up: the last line
        t.send(b"\x1b[A")                       # and the one before
        t.send(b"\r")
        t.expect("11\r\n")
        t.send(b"1 + 2 + 3")
        t.send(b"\x01")                         # Ctrl-A
        t.send(b"(")
        t.send(b"\x05")                         # Ctrl-E
        t.send(b")")
        t.send(b"\x1b[D" * 2)                   # left, left
        t.send(b"\x7f")                         # backspace
        t.send(b"\r")
        t.expect("6\r\n")
        t.send(b"abc")
        t.send(b"\x15")                         # Ctrl-U kills the line
        t.send(b"7 * 6\r")
        t.expect("42\r\n")
        t.send(b"xyz\x03")                      # Ctrl-C abandons the line
        t.expect("^C")
        t.expect("> ")
        t.send(b"'h\xc3\xa9llo'\x01\x1b[C\x1b[C\x1b[C\x7f\r")      # UTF-8: delete the character before the cursor
        t.expect("'hllo'")
        t.send(b"\x04")                         # Ctrl-D on an empty line leaves
        t.proc.wait(timeout=20)
        self.assertEqual(t.proc.returncode, 0)

    def test_history_file(self):
        t = Terminal()
        self.addCleanup(t.close)
        t.expect("> ")
        t.line("40 + 2")
        t.expect("42\r\n")
        t.send(b"\x04")
        t.proc.wait(timeout=20)
        hist = Path(t.home) / ".js_history"
        self.assertEqual(hist.read_text().split("\n")[0], "40 + 2")
        t2 = Terminal(env={"HOME": t.home})                 # a second session finds the line again with Up
        self.addCleanup(t2.close)
        t2.expect("> ")
        t2.send(b"\x1b[A\r")
        t2.expect("42\r\n")

    def test_commands(self):
        t = self.start()
        t.line(".help")
        t.expect("Ctrl-C")
        t.expect("> ")
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "load.js"
            p.write_text("var loaded = 'yes'; console.log('loading');\n")
            t.line(".load " + str(p))
            t.expect("loading")
            t.line("loaded")
            t.expect("'yes'")
        t.line(".exit")
        t.proc.wait(timeout=20)
        self.assertEqual(t.proc.returncode, 0)

    def test_ctrl_c_stops_a_running_program(self):
        t = self.start()
        t.line("for (;;) {}")
        time.sleep(0.5)
        t.send(b"\x03")
        t.expect("interrupted")
        t.expect("> ")
        t.line("'still alive'")
        t.expect("'still alive'")
        t.line("sleep(60000)")
        time.sleep(0.3)
        t.send(b"\x03")
        t.expect("interrupted")
        t.expect("> ")

    def test_time_limit_applies_to_each_line(self):
        t = self.start("-t", "0.3")
        t.line("for (;;) {}")
        t.expect("time limit of 0.3 seconds")
        t.expect("> ")
        t.line("1 + 1")
        t.expect("2\r\n")

    def test_timers_and_promises_in_the_session(self):
        t = self.start()
        t.line("setTimeout(() => console.log('timer fired'), 30)")
        t.expect("timer fired")
        t.line("Promise.resolve(5)")
        t.expect("Promise { 5 }")
        t.line("new Promise(() => {})")
        t.expect("Promise { <pending> }")
        t.line("(async () => { await null; return 'done'; })().then(v => console.log('then', v))")
        t.expect("then done")

    def test_program_then_session(self):
        t = Terminal("-i", "-e", "var preset = 'from -e'; console.log('program ran')")
        self.addCleanup(t.close)
        t.expect("program ran")
        t.expect("> ")
        t.line("preset")
        t.expect("'from -e'")

    def test_a_pipe_is_not_a_terminal(self):
        # -i with a pipe: no prompts, no banner, one result per line
        rc, out, err = jsrun("-i", stdin=b"1 + 1\n'a'\nvar q = 3\nq * 2\nthrow new Error('x')\n")
        self.assertEqual(rc, 0, err[-500:])
        self.assertEqual(out.split("\n")[:4], ["2", "'a'", "undefined", "6"])
        self.assertIn("Uncaught Error: x", err)


if __name__ == "__main__":
    unittest.main()
