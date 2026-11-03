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
    deps = srcs + sorted((JS / "src").glob("*.h")) + sorted((JS / "include").glob("*.h"))
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
        for name in ("language", "builtins", "async"):
            p = CORPUS / (name + ".js")
            with self.subTest(file=p.name):
                rc, out, err = run(["--stress-every", "211", str(p)], timeout=900)
                self.assertNotIn("ERROR: AddressSanitizer", err)
                self.assertEqual(out.splitlines(), p.with_suffix(".expected").read_text().splitlines(), err[-2000:])


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
