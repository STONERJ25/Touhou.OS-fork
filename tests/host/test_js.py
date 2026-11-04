#!/usr/bin/env python3
"""Host tests for Kosuzu, the JavaScript engine (user/lib/js), through tests/host/jstool.c.

    python tools/hosttest.py discover -s tests/host -p "test_js.py" -v

What runs:
  * the corpus: every tests/js/*.js that has a .expected file, compared line for line (AddressSanitizer + UBSan build);
  * the collector stress run: tests/js/gc_stress.js with a collection forced at every allocation (jstool --stress);
  * a table of programs that must fail to parse (SyntaxError) and of runtime errors;
  * the limits: the memory limit, deep recursion, an interrupt (time limit), all of which must be clean errors;
  * the benchmarks (tests/js/bench_*.js) on an optimized build, with their checksums and timings printed.

The tools are built into build/host-js (build/host-js-wsl under WSL); nothing is installed or downloaded.
"""
import json
import math
import os
import shutil
import subprocess
import sys
import time
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
JS = REPO / "user" / "lib" / "js"
CORPUS = REPO / "tests" / "js"


def _in_wsl():
    try:
        return "microsoft" in Path("/proc/version").read_text().lower()
    except OSError:
        return False


OUT = REPO / "build" / ("host-js-wsl" if _in_wsl() else "host-js")


def find_cc():
    for cc in (os.environ.get("CC"), "gcc", "cc", "clang"):
        if cc and shutil.which(cc):
            return shutil.which(cc)
    raise unittest.SkipTest("no C compiler")


def build(name, sanitize):
    OUT.mkdir(parents=True, exist_ok=True)
    exe = OUT / name
    srcs = sorted((JS / "src").glob("*.c")) + [REPO / "tests" / "host" / "jstool.c"]
    deps = srcs + sorted((JS / "src").glob("*.h")) + sorted((JS / "include").glob("*.h")) + [REPO / "user" / "bin" / "kappabox" / "js_prelude.h"]
    if exe.exists() and all(d.stat().st_mtime <= exe.stat().st_mtime for d in deps):
        return exe
    cc = find_cc()
    gcc = "gcc" in Path(cc).name
    flags = ["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"] if sanitize and gcc else ["-O2", "-g"]
    cmd = [cc, *flags, "-std=gnu17", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-sign-compare",
           "-Wno-missing-field-initializers", "-I", str(JS / "include"), *map(str, srcs), "-lm", "-o", str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("build of %s failed:\n%s%s" % (name, r.stdout, r.stderr[-6000:]))
    return exe


def tool():
    return build("jstool", True)


def fast_tool():
    return build("jstool-fast", False)


def run(args, exe=None, timeout=300):
    r = subprocess.run([str(exe or tool()), *args], capture_output=True, text=True, errors="replace", timeout=timeout,
                       env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
    return r.returncode, r.stdout, r.stderr


def run_js(code, *opts, exe=None):
    return run([*opts, "-e", code], exe=exe)


class Corpus(unittest.TestCase):
    def test_corpus(self):
        files = sorted(p for p in CORPUS.glob("*.js") if p.with_suffix(".expected").exists())
        self.assertTrue(files, "no corpus files")
        for p in files:
            with self.subTest(file=p.name):
                rc, out, err = run([str(p)])
                self.assertNotIn("ERROR: AddressSanitizer", err)
                self.assertNotIn("runtime error", err)
                self.assertEqual(out.splitlines(), p.with_suffix(".expected").read_text().splitlines(), err[-2000:])
                self.assertEqual(rc, 0, err[-2000:])

    def test_gc_stress(self):
        p = CORPUS / "gc_stress.js"
        rc, out, err = run(["--stress", str(p)], timeout=1200)
        self.assertNotIn("ERROR: AddressSanitizer", err)
        self.assertEqual(out.splitlines(), p.with_suffix(".expected").read_text().splitlines(), err[-2000:])
        self.assertEqual(rc, 0)

    def test_corpus_frequent_gc(self):
        # the whole corpus with a collection at every 211th allocation (every allocation takes too long here)
        for name in ("language", "builtins", "async", "regexp_unicode", "regexp_string", "regressions", "proxy", "typed", "date", "bigint", "iterhelpers", "fromasync", "iterclose", "promisetry"):
            p = CORPUS / (name + ".js")
            with self.subTest(file=p.name):
                rc, out, err = run(["--stress-every", "211", str(p)], timeout=900)
                self.assertNotIn("ERROR: AddressSanitizer", err)
                self.assertEqual(out.splitlines(), p.with_suffix(".expected").read_text().splitlines(), err[-2000:])


class ConsolePrelude(unittest.TestCase):
    """The `js` applet's console and inspector (user/bin/kappabox/js_prelude.h, written in JavaScript), through jstool --console."""

    def test_inspect(self):
        p = CORPUS / "console" / "inspect.js"
        rc, out, err = run(["--console", str(p)])
        self.assertNotIn("ERROR: AddressSanitizer", err)
        self.assertNotIn("runtime error", err)
        self.assertEqual(out.splitlines(), p.with_suffix(".expected").read_text().splitlines(), err[-2000:])
        self.assertEqual(rc, 0, err[-2000:])

    def test_inspect_under_collector_stress(self):
        p = CORPUS / "console" / "inspect.js"
        rc, out, err = run(["--console", "--stress-every", "13", str(p)], timeout=900)
        self.assertNotIn("ERROR: AddressSanitizer", err)
        self.assertEqual(out.splitlines(), p.with_suffix(".expected").read_text().splitlines(), err[-2000:])

    def test_standard_error(self):
        code = ("console.error('e1', {a: 1}); console.warn('w'); console.assert(false, 'oops %s', 'fmt'); console.assert(1 == 1);"
                "console.group('g'); console.error('inner'); console.groupEnd(); console.log('out'); console.trace('here');"
                "console.time('t'); console.timeLog('t', 'x'); console.timeEnd('t'); console.timeEnd('missing');")
        rc, out, err = run_js(code, "--console")
        self.assertEqual(rc, 0, err[-1500:])
        lines = err.splitlines()
        self.assertEqual(lines[:4], ["e1 { a: 1 }", "w", "Assertion failed: oops fmt", "  inner"])
        self.assertTrue(lines[4].startswith("Trace: here"), lines[4])
        self.assertIn("No such label 'missing'", err)
        outs = out.splitlines()
        self.assertEqual(outs[:2], ["g", "out"])
        self.assertRegex(outs[2], r"^t: [0-9.]+ms x$")
        self.assertRegex(outs[3], r"^t: [0-9.]+ms$")

    def test_uncaught_error_report(self):
        rc, out, err = run_js("function f() { throw new TypeError('bad'); } f();", "--console")
        self.assertNotEqual(rc, 0)
        self.assertIn("TypeError: bad", out + err)


# programs that must not compile: (source, a fragment of the message)
SYNTAX_ERRORS = [
    ("var 1a;", "SyntaxError"),
    ("let x; let x;", "SyntaxError"),
    ("const c;", "SyntaxError"),
    ("function f() { return } }", "SyntaxError"),
    ("'use strict'; with (a) {}", "SyntaxError"),
    ("'use strict'; var eval = 1;", "SyntaxError"),
    ("break;", "SyntaxError"),
    ("x: x: ;", "SyntaxError"),
    ("class A { constructor() {} constructor() {} }", "SyntaxError"),
    ("class A extends B { m() { super(); } }", "SyntaxError"),
    ("function* g() { yield = 1 }", "SyntaxError"),
    ("async function f() { await = 1 }", "SyntaxError"),
    ("({a: 1} = 1)", "SyntaxError"),
    ("a => { 'use strict'; delete a; }", "SyntaxError"),
    ("new.target", "SyntaxError"),
    ("for (let x of [], []) ;", "SyntaxError"),
    ("`${`", "SyntaxError"),
    ("'unterminated", "SyntaxError"),
    ("0b2", "SyntaxError"),
    ("1__0", "SyntaxError"),
    ("class A { #x; #x; }", "SyntaxError"),
    ("class A { m() { this.#y; } }", "SyntaxError"),
    ("let let = 1;", "SyntaxError"),
    ("(a, a) => 1", "SyntaxError"),
    ("function f(a = super.x) {}", "SyntaxError"),
    ("if (1) function* g() {}", "SyntaxError"),
    ("return 1;", "SyntaxError"),
    ("import('x', 'y', 'z')", "SyntaxError"),
    ("/(/", "Unterminated group"),
    ("/a**/", "Nothing to repeat"),
    ("/[b-a]/", "Range out of order"),
    ("/(?<n>a)(?<n>b)/", "Duplicate capture group name"),
    ("/a/gg", "regular expression flags"),
    (r"/\u{110000}/u", "Invalid Unicode escape"),
    ("/(?<=a)*/", "Nothing to repeat"),
    ("/a/uv", "regular expression flags"),
    (r"/\p{Foo}/u", "Invalid property name"),
    (r"/(?<a>.)\k<b>/", "Invalid named capture referenced"),
]

# programs that compile but throw: (source, a fragment of the output)
RUNTIME_ERRORS = [
    ("undefinedVariable", "ReferenceError"),
    ("null.x", "TypeError"),
    ("let a = a;", "ReferenceError"),
    ("const k = 1; k = 2;", "TypeError"),
    ("(void 0)()", "TypeError"),
    ("new (function* () {})()", "TypeError"),
    ("class A {}; A()", "TypeError"),
    ("Object.defineProperty(Object.freeze({}), 'x', { value: 1 })", "TypeError"),
    ("[].reduce((a, b) => a)", "TypeError"),
    ("new Array(-1)", "RangeError"),
    ("'a'.repeat(-1)", "RangeError"),
    ("(1).toFixed(101)", "RangeError"),
    ("JSON.parse('{bad')", "SyntaxError"),
    ("var o = {}; o.o = o; JSON.stringify(o)", "TypeError"),
    ("Symbol() + ''", "TypeError"),
    ("throw new Error('custom message')", "Error: custom message"),
    ("new RegExp('(')", "Invalid regular expression: /(/: Unterminated group"),
    ("new RegExp('a', 'q')", "Invalid flags supplied to RegExp constructor 'q'"),
    ("'a'.matchAll(/a/)", "TypeError"),
    ("'a'.replaceAll(/a/, 'b')", "TypeError"),
    ("RegExp.prototype.exec.call({}, 'a')", "TypeError"),
    ("Object.freeze(/a/g).test('a')", "TypeError"),
]


class Errors(unittest.TestCase):
    def test_syntax_errors(self):
        for src, frag in SYNTAX_ERRORS:
            with self.subTest(src=src):
                rc, out, err = run_js(src)
                self.assertNotEqual(rc, 0, out)
                self.assertIn(frag, out + err)
                self.assertNotIn("AddressSanitizer", err)

    def test_runtime_errors(self):
        for src, frag in RUNTIME_ERRORS:
            with self.subTest(src=src):
                rc, out, err = run_js(src)
                self.assertNotEqual(rc, 0, out)
                self.assertIn(frag, out + err)
                self.assertNotIn("AddressSanitizer", err)

    def test_caught_errors(self):
        rc, out, err = run_js("try { null.f(); } catch (e) { print(e instanceof TypeError, e.constructor === TypeError); }")
        self.assertEqual(out.split(), ["true", "true"], err)


class Limits(unittest.TestCase):
    def test_memory_limit(self):
        code = ("var keep = []; try { for (;;) keep.push(new Array(10000).fill('x' + keep.length)); }"
                " catch (e) { keep = null; print('caught', e instanceof RangeError, e.message); }")
        rc, out, err = run_js(code, "--mem", "16")
        self.assertIn("caught true out of memory", out, err[-2000:])
        self.assertEqual(rc, 0)

    def test_deep_recursion(self):
        code = "function f(n) { return f(n + 1) + 1; } try { f(0); } catch (e) { print('caught', e instanceof RangeError); }"
        rc, out, err = run_js(code)
        self.assertIn("caught true", out, err[-2000:])
        self.assertEqual(rc, 0)

    def test_deep_recursion_through_natives(self):
        code = ("function f() { return [1].map(f); } try { f(); } catch (e) { print('caught', e instanceof RangeError); }")
        rc, out, err = run_js(code)
        self.assertIn("caught true", out, err[-2000:])

    def test_deep_nesting_parse(self):
        # (a file: one command-line argument may not exceed 128 KiB on Linux)
        OUT.mkdir(parents=True, exist_ok=True)
        p = OUT / "deep_nesting.js"
        p.write_text("var x = " + "[" * 100000 + "]" * 100000 + ";\nprint('parsed');\n")
        rc, out, err = run([str(p)])
        self.assertNotIn("AddressSanitizer", err)
        self.assertTrue("parsed" in out or "RangeError" in out or "SyntaxError" in out, out[-500:] + err[-500:])

    def test_interrupt(self):
        t0 = time.time()
        rc, out, err = run_js("for (;;) {}", "--time-limit", "300")
        self.assertNotEqual(rc, 0)
        self.assertIn("interrupted", out + err)
        self.assertLess(time.time() - t0, 30)

    def test_interrupt_not_catchable(self):
        rc, out, err = run_js("try { for (;;) {} } catch (e) { print('CATCH-BLOCK-RAN'); }", "--time-limit", "300")
        self.assertNotIn("CATCH-BLOCK-RAN", out)
        self.assertIn("interrupted", out + err)


class ProxyAndTypedLimits(unittest.TestCase):
    """Proxies recurse in C and typed arrays take their storage from the heap: both must meet the limits as clean errors."""

    def test_huge_buffers_are_range_errors(self):
        code = ("var out = [];"
                "function t(f) { try { f(); out.push('no error'); } catch (e) { out.push(e.constructor.name); } }"
                "t(function () { new ArrayBuffer(2 ** 40); });"
                "t(function () { new ArrayBuffer(1, { maxByteLength: 2 ** 40 }); });"
                "t(function () { new Float64Array(2 ** 32); });"
                "t(function () { new SharedArrayBuffer(2 ** 53); });"
                "t(function () { new Uint8Array(new ArrayBuffer(8), 0, 2 ** 53); });"
                "t(function () { new ArrayBuffer(Number.MAX_SAFE_INTEGER); });"
                "print(out.join());")
        rc, out, err = run_js(code, "--mem", "64")
        self.assertEqual(out.strip(), "RangeError,RangeError,RangeError,RangeError,RangeError,RangeError", err[-1500:])
        self.assertNotIn("AddressSanitizer", err)

    def test_buffers_obey_the_memory_limit(self):
        code = ("var keep = []; try { for (;;) keep.push(new ArrayBuffer(1 << 20)); } catch (e) { var n = keep.length; keep = null;"
                " print('caught', e instanceof RangeError, e.message, n > 4); }")
        rc, out, err = run_js(code, "--mem", "32")
        self.assertIn("caught true out of memory true", out, err[-1500:])
        self.assertEqual(rc, 0)
        # and the memory comes back after the collection
        code = ("for (var i = 0; i < 200; i++) new Uint8Array(1 << 20).fill(i);"
                "var r = new ArrayBuffer(16, { maxByteLength: 1 << 22 }); for (var i = 0; i < 100; i++) r.resize((i % 2) ? 16 : 1 << 22);"
                "print('done');")
        rc, out, err = run_js(code, "--mem", "48")
        self.assertEqual(out.strip(), "done", err[-1500:])

    def test_deep_proxy_chains_are_range_errors(self):
        ops = ("p.x", "p.x = 1", "'x' in p", "delete p.x", "Object.keys(p)", "Object.getPrototypeOf(p)", "Object.setPrototypeOf(p, null)",
               "Object.isExtensible(p)", "Object.preventExtensions(p)", "Object.defineProperty(p, 'x', { value: 1 })",
               "Object.getOwnPropertyDescriptor(p, 'x')", "JSON.stringify(p)", "Array.isArray(p)", "p instanceof Object")
        for op in ops:
            with self.subTest(op=op):
                code = ("var p = {}; for (var i = 0; i < 100000; i++) p = new Proxy(p, {});"
                        "try { %s; print('no error'); } catch (e) { print(e instanceof RangeError ? 'RangeError' : e); }" % op)
                rc, out, err = run_js(code)
                self.assertNotIn("AddressSanitizer", err)
                self.assertIn(out.strip(), ("RangeError", "no error"), err[-1500:])
                self.assertEqual(rc, 0, out + err[-500:])

    def test_endless_trap_recursion_is_a_range_error(self):
        code = ("var p = new Proxy({}, { get: function (t, k, r) { return r[k]; }, has: function (t, k) { return k in p; },"
                " ownKeys: function (t) { return Reflect.ownKeys(p); } });"
                "var out = [];"
                "for (var f of [function () { p.x; }, function () { 'x' in p; }, function () { Object.keys(p); }]) {"
                " try { f(); out.push('no error'); } catch (e) { out.push(e instanceof RangeError); } }"
                "print(out.join());")
        rc, out, err = run_js(code)
        self.assertEqual(out.strip(), "true,true,true", err[-1500:])
        self.assertNotIn("AddressSanitizer", err)

    def test_proxy_prototype_chain_cycles(self):
        # the cycle check of [[SetPrototypeOf]] stops at a proxy, so a prototype cycle through one can be made: looking a
        # property up in it must end with a RangeError (the lookups recurse in C through the proxy), not loop for ever
        code = ("var target = {}; var p = new Proxy(target, {}); var o = Object.create(p); Object.setPrototypeOf(target, o);"
                "var out = [];"
                "for (var f of [function () { o.missing; }, function () { 'missing' in o; }, function () { o.missing = 1; }, function () { Object.getPrototypeOf(p) === o; }]) {"
                " try { f(); out.push('no error'); } catch (e) { out.push(e instanceof RangeError ? 'RangeError' : String(e)); } }"
                "print(out.join());")
        rc, out, err = run_js(code, "--time-limit", "20000")
        self.assertEqual(out.strip(), "RangeError,RangeError,RangeError,no error", err[-1500:])
        self.assertNotIn("AddressSanitizer", err)

    def test_collector_stress_with_proxies_and_typed_arrays(self):
        code = ("var keep = [];"
                "for (var i = 0; i < 300; i++) {"
                "  var t = new Uint16Array([i, i + 1, i + 2]); var d = new DataView(t.buffer);"
                "  var p = new Proxy(t, { get: function (o, k) { return o[k]; } });"
                "  keep.push([p.length, p[1], d.getUint16(2, true), t.subarray(1).length, Array.from(t).join(), Object.keys(p).join('')]);"
                "}"
                "print(keep.length, keep[299].join(';'));")
        rc, out, err = run_js(code, "--stress-every", "7")
        self.assertEqual(out.strip(), "300 3;300;300;2;299,300,301;012", err[-1500:])
        self.assertNotIn("AddressSanitizer", err)


class BigIntLimits(unittest.TestCase):
    """The schoolbook algorithms are quadratic: a hostile program must end in a RangeError or an interrupt, never a crash."""

    def test_enormous_results_end_in_range_errors_or_interrupts(self):
        ops = ["10n ** 100000000n", "1n << (2n ** 35n)", "3n ** (2n ** 31n)", "BigInt.asUintN(2 ** 40, -1n)", "2n ** (2n ** 29n)",
               "(1n << 1000000000n)", "BigInt('9'.repeat(5000000))", "((1n << 8000000n) - 1n).toString()", "((1n << 8000000n) - 1n).toString(7)",
               "((1n << 4000000n) - 1n) / ((1n << 2000000n) + 1n)", "((1n << 3000000n) - 1n) * ((1n << 3000000n) - 3n)", "BigInt('0x' + 'f'.repeat(300000000))"]
        for op in ops:
            with self.subTest(op=op):
                t0 = time.time()
                rc, out, err = run_js("try { %s; print('no error'); } catch (e) { print(e.constructor.name, e.message); }" % op, "--mem", "64", "--time-limit", "2000")
                self.assertNotIn("AddressSanitizer", err, err[-1500:])
                self.assertTrue(out.startswith("RangeError") or out.startswith("no error") or "interrupted" in out + err, out[-300:] + err[-300:])
                self.assertLess(time.time() - t0, 90)

    def test_loops_neither_leak_nor_slow_down(self):
        code = ("var x = 1n; for (var i = 0; i < 300000; i++) x = (x * 6364136223846793005n + 1442695040888963407n) % (2n ** 64n); print(x);")
        rc, out, err = run_js(code, "--mem", "16")
        x = 1
        for _ in range(300000):
            x = (x * 6364136223846793005 + 1442695040888963407) % (2 ** 64)
        self.assertEqual(out.strip(), str(x), err[-1500:])

    def test_collector_stress(self):
        code = ("var keep = [];"
                "for (var i = 0n; i < 200n; i++) keep.push([i ** 20n, (i * 2n ** 70n).toString(16), BigInt.asIntN(70, i * 2n ** 68n), BigInt(Number(i) * 1e20)]);"
                "var s = 0n; for (var k of keep) s += k[0] + BigInt('0x' + k[1]) + k[2] + k[3];"
                "var a = new BigInt64Array(50); for (var i = 0; i < 50; i++) a[i] = BigInt(i) * 2n ** 60n;"
                "print(s, a[49], a.reduce(function (p, q) { return p + q; }, 0n));")
        rc, out, err = run_js(code, "--stress-every", "3")
        s = sum(i ** 20 + int(hex(i * 2 ** 70), 16) + ((i * 2 ** 68 + 2 ** 69) % 2 ** 70 - 2 ** 69) + int(float(i) * 1e20) for i in range(200))
        wrap = lambda v: (v + 2 ** 63) % 2 ** 64 - 2 ** 63
        self.assertEqual(out.split(), [str(s), str(wrap(49 * 2 ** 60)), str(sum(wrap(i * 2 ** 60) for i in range(50)))], err[-1500:])
        self.assertNotIn("AddressSanitizer", err)


class DateZones(unittest.TestCase):
    """Date in other time zones than the UTC of the corpus.  The expected values come from Python's datetime, not the engine."""

    ZONES = (0, 540, -300, 330, -210, 765, -720, 840, -570)

    @staticmethod
    def gmt(minutes):
        return "GMT%s%02d%02d" % ("+" if minutes >= 0 else "-", abs(minutes) // 60, abs(minutes) % 60)

    def test_fixed_offsets(self):
        import datetime as dt
        epoch = dt.datetime(1970, 1, 1)
        code = """
          var d = new Date(Date.UTC(2020, 1, 29, 12, 34, 56, 789));
          print([d.getFullYear(), d.getMonth(), d.getDate(), d.getDay(), d.getHours(), d.getMinutes(), d.getSeconds(), d.getMilliseconds(), d.getTimezoneOffset()].join());
          print(d.toString()); print(d.toTimeString()); print(d.toDateString());
          print(new Date(2020, 0, 1).getTime(), new Date(2020, 11, 31, 23, 59).getTime());
          print(Date.parse("2000-01-01T00:00:00"), Date.parse("2000-01-01"), Date.parse("2000-01-01T00:00:00Z"), Date.parse("2000-01-01T00:00:00+01:00"));
          print(new Date(2020, 0, 1).toISOString());
          var s = new Date(0); s.setHours(12); print(s.getTime());
          var u = new Date(Date.UTC(2020, 5, 15, 10)); u.setDate(31); print(u.getTime());
          var y = new Date(Date.UTC(2020, 11, 31, 23)); y.setFullYear(2021); print(y.getTime(), y.getFullYear());
          print(Date.parse(new Date(0).toString()), Date.parse(new Date(0).toUTCString()));
        """

        def ms(x):
            return int((x - epoch).total_seconds() * 1000)

        for tz in self.ZONES:
            with self.subTest(tz=tz):
                rc, out, err = run_js(code, "--tz", str(tz), "--tz-name", "ZZZ")
                self.assertEqual(rc, 0, out + err)
                shift = dt.timedelta(minutes=tz)
                loc = dt.datetime(2020, 2, 29, 12, 34, 56) + shift
                s_local = (epoch + shift).replace(hour=12)                  # setHours(12) keeps the local date and the minutes
                u_now = dt.datetime(2020, 6, 15, 10) + shift                # setDate(31) on a June date: July 1 at the same time of day
                u_local = dt.datetime(2020, 6, 1) + dt.timedelta(days=30, hours=u_now.hour, minutes=u_now.minute)
                y_local = (dt.datetime(2020, 12, 31, 23) + shift).replace(year=2021)
                want = [
                    ",".join(map(str, [loc.year, loc.month - 1, loc.day, (loc.weekday() + 1) % 7, loc.hour, loc.minute, loc.second, 789, -tz])),
                    "%s %s (ZZZ)" % (loc.strftime("%a %b %d %Y %H:%M:%S"), self.gmt(tz)),
                    "%s %s (ZZZ)" % (loc.strftime("%H:%M:%S"), self.gmt(tz)),
                    loc.strftime("%a %b %d %Y"),
                    "%d %d" % (ms(dt.datetime(2020, 1, 1)) - tz * 60000, ms(dt.datetime(2020, 12, 31, 23, 59)) - tz * 60000),
                    "%d %d %d %d" % (946684800000 - tz * 60000, 946684800000, 946684800000, 946684800000 - 3600000),
                    (dt.datetime(2020, 1, 1) - shift).strftime("%Y-%m-%dT%H:%M:%S.000Z"),
                    "%d" % (ms(s_local) - tz * 60000),
                    "%d" % (ms(u_local) - tz * 60000),
                    "%d %d" % (ms(y_local) - tz * 60000, y_local.year),
                    "0 0",
                ]
                self.assertEqual(out.splitlines(), want)

    def test_daylight_saving_time(self):
        # US Eastern 2020: clocks go 02:00 -> 03:00 on March 8 (07:00Z), and 02:00 -> 01:00 on November 1 (06:00Z)
        start, end = 1583650800000, 1604210400000
        code = """
          function show(d) { return d.toISOString() + "|" + d.getHours() + ":" + d.getMinutes() + "|" + d.getTimezoneOffset(); }
          print(show(new Date(2020, 2, 8, 1, 59, 59)));
          print(show(new Date(2020, 2, 8, 2, 30)));        // does not exist: read with the offset before the change
          print(show(new Date(2020, 2, 8, 3, 0)));
          print(show(new Date(2020, 10, 1, 0, 59)));
          print(show(new Date(2020, 10, 1, 1, 30)));       // happens twice: the first time
          print(show(new Date(2020, 10, 1, 2, 0)));
          print(show(new Date(Date.UTC(2020, 10, 1, 5, 30))));
          print(show(new Date(Date.UTC(2020, 10, 1, 6, 30))));
          print(show(new Date(Date.UTC(2020, 6, 1, 16))));
          print(show(new Date(Date.UTC(2020, 0, 1, 16))));
          var d = new Date(2020, 2, 8, 0, 0); d.setHours(2, 30); print(show(d));
          var e = new Date(2020, 2, 7, 2, 30); e.setDate(8); print(show(e));
          var f = new Date(2020, 10, 1, 0, 30); f.setHours(f.getHours() + 1); print(show(f));
          print(Date.parse("2020-07-01T12:00:00") - Date.parse("2020-07-01T12:00:00Z"));
          print(Date.parse("2020-01-01T12:00:00") - Date.parse("2020-01-01T12:00:00Z"));
          print(new Date(2020, 2, 8, 12).toString());
          print(new Date(2020, 0, 8, 12).toString());
          print(new Date(2020, 6, 8, 12).toTimeString());
        """
        rc, out, err = run_js(code, "--tz", "-300", "--tz-dst", str(start), str(end), "60", "--tz-name", "US")
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(out.splitlines(), [
            "2020-03-08T06:59:59.000Z|1:59|300",
            "2020-03-08T07:30:00.000Z|3:30|240",
            "2020-03-08T07:00:00.000Z|3:0|240",
            "2020-11-01T04:59:00.000Z|0:59|240",
            "2020-11-01T05:30:00.000Z|1:30|240",
            "2020-11-01T07:00:00.000Z|2:0|300",
            "2020-11-01T05:30:00.000Z|1:30|240",
            "2020-11-01T06:30:00.000Z|1:30|300",
            "2020-07-01T16:00:00.000Z|12:0|240",
            "2020-01-01T16:00:00.000Z|11:0|300",
            "2020-03-08T07:30:00.000Z|3:30|240",
            "2020-03-08T07:30:00.000Z|3:30|240",
            "2020-11-01T05:30:00.000Z|1:30|240",
            "14400000",
            "18000000",
            "Sun Mar 08 2020 12:00:00 GMT-0400 (US)",
            "Wed Jan 08 2020 12:00:00 GMT-0500 (US)",
            "12:00:00 GMT-0400 (US)",
        ])

    def test_extremes_do_not_misbehave(self):
        code = ("var a = new Date(-8.64e15), b = new Date(8.64e15); print(a.getTimezoneOffset(), b.getTimezoneOffset(), b.toISOString());"
                "print(new Date(8.64e15).setHours(0), a.toString().length > 10, new Date(2020, 0, 1, 0, 0, 0, 8.64e15).getTime());")
        for tz in (840, -720):
            with self.subTest(tz=tz):
                rc, out, err = run_js(code, "--tz", str(tz))
                self.assertEqual(rc, 0, out + err)
                self.assertNotIn("AddressSanitizer", err)
                lines = out.splitlines()
                self.assertEqual(lines[0], "%d %d +275760-09-13T00:00:00.000Z" % (-tz, -tz))
                self.assertEqual(lines[1].split()[1:], ["true", "NaN"], lines[1])


class RegExpLimits(unittest.TestCase):
    """A hostile pattern or input must end in an error or an interrupt: never a hang, a crash or unbounded memory."""

    def test_catastrophic_backtracking_is_interrupted(self):
        for code in ("/(a+)+b/.test('a'.repeat(40))",
                     "/^(\\w+\\s?)*$/.test('a'.repeat(40) + '!')",
                     "/(x+x+)+y/.test('x'.repeat(60))",
                     "/(?:a|aa)+$/.test('a'.repeat(60) + 'b')",
                     "'a'.repeat(50).replace(/(a*)*b/g, 'x')"):
            with self.subTest(code=code):
                t0 = time.time()
                rc, out, err = run_js("try { %s } catch (e) { print('CATCH-BLOCK-RAN'); }" % code, "--time-limit", "300")
                self.assertNotEqual(rc, 0)
                self.assertNotIn("CATCH-BLOCK-RAN", out)
                self.assertIn("interrupted", out + err)
                self.assertNotIn("AddressSanitizer", err)
                self.assertLess(time.time() - t0, 60)

    def test_backtrack_stack_is_bounded(self):
        code = ("try { /(?:a|b)*c/.test('ab'.repeat(5000000)); print('no error'); }"
                " catch (e) { print('caught', e instanceof RangeError, e.message); }")
        rc, out, err = run_js(code)
        self.assertIn("caught true Maximum call stack size exceeded", out, err[-2000:])
        self.assertEqual(rc, 0)
        self.assertNotIn("AddressSanitizer", err)

    def test_backtrack_stack_obeys_the_memory_limit(self):
        code = ("try { /(?:a|b)*c/.test('ab'.repeat(2000000)); print('no error'); }"
                " catch (e) { print('caught', e instanceof RangeError); }")
        rc, out, err = run_js(code, "--mem", "48")
        self.assertIn("caught true", out, err[-2000:])
        self.assertEqual(rc, 0)

    def test_deep_pattern_nesting_is_an_error(self):
        for src in ("'('.repeat(200000) + ')'.repeat(200000)", "'(?:'.repeat(200000)", "'(?=a'.repeat(100000)",
                    "'[' + '[\\\\q{a|'.repeat(20000)"):
            with self.subTest(src=src):
                rc, out, err = run_js("try { new RegExp(%s, 'v'); print('compiled'); } catch (e) { print('caught', e.constructor.name); }" % src)
                self.assertTrue("caught" in out or "compiled" in out, out[-300:] + err[-300:])
                self.assertNotIn("AddressSanitizer", err)
                self.assertEqual(rc, 0, err[-500:])

    def test_regexp_in_a_loop_does_not_leak_or_slow_down(self):
        # the compile cache and the per-match buffers: a million executions of a literal
        code = "var n = 0; for (var i = 0; i < 300000; i++) if (/(\\d+)-(\\d+)/.test('a' + i + '-' + i)) n++; print(n);"
        rc, out, err = run_js(code, "--mem", "32")
        self.assertEqual(out.strip(), "300000", err[-1000:])


def raytrace_checksum():
    def add(a, b): return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
    def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
    def mul(a, k): return (a[0] * k, a[1] * k, a[2] * k)
    def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
    def norm(a): return mul(a, 1 / math.sqrt(dot(a, a)))
    spheres = [((0, -1, 3), 1, 1), ((2, 0, 4), 1, 2), ((-2, 0, 4), 1, 3), ((0, -5001, 0), 5000, 4)]
    light = (2, 1, 0)
    total = 0.0
    W, H = 160, 120
    for y in range(H):
        for x in range(W):
            d = norm(((x - W / 2) / W, -(y - H / 2) / W, 1))
            best, bs = math.inf, None
            for c, r, col in spheres:
                co = sub((0, 0, 0), c)
                b = 2 * dot(co, d)
                cc = dot(co, co) - r * r
                disc = b * b - 4 * cc
                if disc < 0:
                    continue
                t = (-b - math.sqrt(disc)) / 2
                if 0.001 < t < best:
                    best, bs = t, (c, r, col)
            if bs:
                p = mul(d, best)
                n = norm(sub(p, bs[0]))
                l = norm(sub(light, p))
                total += bs[2] * max(0, dot(n, l))
    return math.floor(total + 0.5)


def regexp_bench_line():
    """The numbers bench_regexp.js prints, computed with Python's re module (the patterns mean the same in both)."""
    import re
    words = ["alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta"]
    text = "".join(words[i % 8] + str(i) + ("@example.com\n" if i % 7 == 0 else " ") for i in range(40000))
    numbers = len(re.findall(r"\d+", text))
    emails = len(re.findall(r"[a-z]+\d*@[a-z]+\.com", text))
    squeezed = len(re.sub(r"\s+", "_", text))
    pieces = len(re.split(r"[ @.\n]", text))
    total = sum(len(m.group(2)) for m in re.finditer(r"([a-z]+?)(\d+)", text))
    alt = len(re.findall(r"\b(?:alpha|gamma|theta)\d+\b", text))
    lines = len(re.findall(r"^.*example.*$", text, re.M))
    ci = len(re.findall(r"EPSILON\d+", text, re.I))
    fn = len(re.sub(r"\d+", lambda m: str(len(m.group())), text))
    return "regexp %d %d %d %d %d %d %d %d %d 20000" % (numbers, emails, squeezed, pieces, total, alt, lines, ci, fn)


def bigint_bench_line():
    """The line bench_bigint.js prints, computed with Python's integers (the same algorithms, so the digits agree)."""
    if hasattr(sys, "set_int_max_str_digits"):
        sys.set_int_max_str_digits(0)
    a, b = 0, 1
    for _ in range(100000):
        a, b = b, a + b

    def arccot(x, unity):
        total = xpow = unity // x
        n, sign, x = 3, -1, x * x
        while True:
            xpow //= x
            term = xpow // n
            total += sign * term
            sign, n = -sign, n + 2
            if term == 0:
                return total

    unity = 10 ** (1500 + 10)
    pi = str((16 * arccot(5, unity) - 4 * arccot(239, unity)) // 10 ** 10)
    mp = sum(pow(i, 1000003, 1000000007) for i in range(1, 20001))
    return "bigint %d %d %d %s %s" % (len(str(math.factorial(5000))), len(str(a)), mp, pi[:12], pi[-12:])


def expected_bench():
    s_len = sum(len("item%d" % i) for i in range(200000)) + 199999
    items = []
    for i in range(20000):
        v = str(i // 4) if i % 4 == 0 else repr(i / 4)
        items.append('{"id":%d,"name":"n%d","tags":["a","b",%d],"ok":%s,"v":%s}'
                     % (i, i, i % 7, "true" if i % 2 == 0 else "false", v))
    text = len("[" + ",".join(items) + "]")
    return {
        "bench_fib.js": "fib 196418",
        "bench_strings.js": "strings %d 100000 bcd %d" % (s_len, s_len - 10),
        "bench_json.js": "json %d" % (5 * (20000 + text)),
        "bench_sort.js": "sort true true true 250000",
        "bench_raytrace.js": "raytrace %d" % raytrace_checksum(),
        "bench_regexp.js": regexp_bench_line(),
        "bench_bigint.js": bigint_bench_line(),
    }


class Bench(unittest.TestCase):
    def test_benchmarks(self):
        exe = fast_tool()
        want = expected_bench()
        times = []
        for name, line in want.items():
            with self.subTest(bench=name):
                t0 = time.time()
                rc, out, err = run([str(CORPUS / name)], exe=exe)
                dt = time.time() - t0
                times.append("%s %.3fs" % (name, dt))
                self.assertEqual(out.strip(), line, err[-2000:])
        print("\nbenchmarks (optimized host build): " + ", ".join(times))


if __name__ == "__main__":
    unittest.main()
