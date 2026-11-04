#!/usr/bin/env python3
"""Host tests for ES modules in Kosuzu (user/lib/js/src/module.c), through tests/host/jstool.c --module.

    python tools/hosttest.py discover -s tests/host -p "test_js_modules.py" -v

What runs (AddressSanitizer + UBSan build, as in test_js.py):
  * the corpus: every tests/js/modules/*.mjs that has a .expected file is run as the entry module (its imports are read
    from disk, relative to the importing file) and its output compared line for line; again with a collection forced at
    every 7th allocation;
  * entry modules that must fail, with the error they end in: a missing export, an ambiguous star export, a throw, a
    file that is not there, a syntax error in a dependency, a top-level await that never settles;
  * big graphs: a chain of 2000 modules (must work), a chain of 100000 (a RangeError, never a crash), a cycle of 500,
    a ladder of star exports whose name resolution would take 2^40 steps without the visited set, a deep chain of
    async modules, thousands of imports of one module.
The test262 module directories are run by tests/host/test262.py (see docs/JS.md for the numbers).
"""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_js  # noqa: E402  (the tool builder)

REPO = test_js.REPO
MODULES = REPO / "tests" / "js" / "modules"


def run_module(path, *opts, timeout=600, exe=None):
    r = subprocess.run([str(exe or test_js.tool()), *opts, "--module", str(path)], capture_output=True, text=True,
                       errors="replace", timeout=timeout, cwd=str(REPO), env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
    return r.returncode, r.stdout, r.stderr


def clean(err):
    return "ERROR: AddressSanitizer" not in err and "runtime error" not in err


class Corpus(unittest.TestCase):
    def entries(self):
        files = sorted(p for p in MODULES.glob("*.mjs") if p.with_suffix(".expected").exists())
        self.assertTrue(files, "no module corpus")
        return files

    def test_corpus(self):
        for p in self.entries():
            with self.subTest(file=p.name):
                rc, out, err = run_module(p.relative_to(REPO).as_posix())
                self.assertTrue(clean(err), err[-2000:])
                self.assertEqual(out.splitlines(), p.with_suffix(".expected").read_text().splitlines(), err[-2000:])
                self.assertEqual(rc, 0, err[-2000:])

    def test_corpus_under_collector_stress(self):
        for p in self.entries():
            with self.subTest(file=p.name):
                rc, out, err = run_module(p.relative_to(REPO).as_posix(), "--stress-every", "7", timeout=1800)
                self.assertTrue(clean(err), err[-2000:])
                self.assertEqual(out.splitlines(), p.with_suffix(".expected").read_text().splitlines(), err[-2000:])
                self.assertEqual(rc, 0, err[-2000:])


class Failures(unittest.TestCase):
    """Entry modules that end in an error: the report is "Uncaught <error>" and the exit status 1."""

    def run_files(self, files, entry="main.mjs", *opts):
        with tempfile.TemporaryDirectory() as d:
            for name, text in files.items():
                (Path(d) / name).write_text(text, encoding="utf-8")
            return run_module(Path(d) / entry, *opts)

    def expect(self, files, text, entry="main.mjs"):
        rc, out, err = self.run_files(files, entry)
        self.assertTrue(clean(err), err[-2000:])
        self.assertEqual(rc, 1, out + err[-1500:])
        self.assertIn(text, out)
        return out

    def test_missing_export(self):
        self.expect({"main.mjs": "import { x } from './dep.mjs'; print('not reached');", "dep.mjs": "export const y = 1;"},
                    "SyntaxError: The requested module './dep.mjs' does not provide an export named 'x'")

    def test_missing_export_in_a_cycle(self):
        self.expect({"main.mjs": "import { b } from './b.mjs'; export const a = 1;",
                     "b.mjs": "import { a, nothing } from './main.mjs'; export const b = 2;"},
                    "does not provide an export named 'nothing'")

    def test_indirect_export_of_nothing(self):
        self.expect({"main.mjs": "export { x } from './dep.mjs';", "dep.mjs": "export const y = 1;"}, "SyntaxError")

    def test_ambiguous_star_export(self):
        self.expect({"main.mjs": "import { v } from './hub.mjs';", "hub.mjs": "export * from './l.mjs'; export * from './r.mjs';",
                     "l.mjs": "export const v = 1;", "r.mjs": "export const v = 2;"},
                    "conflicting star exports for name 'v'")

    def test_error_thrown_by_a_module(self):
        out = self.expect({"main.mjs": "import './dep.mjs'; print('not reached');",
                           "dep.mjs": "print('dep runs'); throw new Error('boom');"}, "Uncaught Error: boom")
        self.assertNotIn("not reached", out)
        self.assertIn("dep runs", out)

    def test_error_thrown_by_the_entry(self):
        self.expect({"main.mjs": "import './dep.mjs'; print('dep done'); null.x;", "dep.mjs": "export {};"}, "TypeError")

    def test_missing_file(self):
        self.expect({"main.mjs": "import './nowhere.mjs';"}, "Cannot load module './nowhere.mjs'")

    def test_bare_specifier(self):
        self.expect({"main.mjs": "import 'lodash';"}, "Cannot load module 'lodash'")

    def test_syntax_error_in_the_entry(self):
        self.expect({"main.mjs": "let = = 1;"}, "SyntaxError")

    def test_syntax_error_in_a_dependency(self):
        self.expect({"main.mjs": "import './dep.mjs'; print('not reached');", "dep.mjs": "export const x = ;"}, "SyntaxError")

    def test_top_level_await_that_never_settles(self):
        self.expect({"main.mjs": "await new Promise(() => {}); print('not reached');"}, "never finished")

    def test_dependency_that_never_settles(self):
        self.expect({"main.mjs": "import './dep.mjs'; print('not reached');", "dep.mjs": "await new Promise(() => {});"},
                    "never finished")

    def test_rejection_of_a_top_level_await(self):
        self.expect({"main.mjs": "await Promise.reject(new RangeError('late'));"}, "RangeError: late")

    def test_uncaught_in_a_job_after_the_module_ended(self):
        rc, out, err = self.run_files({"main.mjs": "Promise.resolve().then(() => { throw new Error('in a job'); }); print('module done');"})
        self.assertIn("module done", out)
        self.assertIn("in a job", out)
        self.assertEqual(rc, 1)

    def test_a_script_cannot_import_statically(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "s.js"
            p.write_text("import x from './m.mjs';", encoding="utf-8")
            r = subprocess.run([str(test_js.tool()), str(p)], capture_output=True, text=True, errors="replace",
                               env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
            self.assertEqual(r.returncode, 1)
            self.assertIn("SyntaxError", r.stdout)

    def test_a_script_can_import_dynamically(self):
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "m.mjs").write_text("export const v = 7; print('m ran');", encoding="utf-8")
            p = Path(d) / "s.js"
            p.write_text("import('./m.mjs').then(ns => print('v', ns.v)); print('script done');", encoding="utf-8")
            r = subprocess.run([str(test_js.tool()), str(p)], capture_output=True, text=True, errors="replace",
                               env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
            self.assertEqual(r.stdout.splitlines(), ["script done", "m ran", "v 7"], r.stderr[-1500:])
            self.assertEqual(r.returncode, 0)


class Graphs(unittest.TestCase):
    """Big and hostile module graphs: they work or fail cleanly, in bounded time and memory."""

    def build(self, d, files):
        for name, text in files.items():
            (Path(d) / name).write_text(text, encoding="utf-8")

    def test_chain_of_2000(self):
        with tempfile.TemporaryDirectory() as d:
            n = 2000
            files = {"m%d.mjs" % i: "import { w as next } from './m%d.mjs'; export const w = next + 1;" % (i + 1) for i in range(n)}
            files["m%d.mjs" % n] = "export const w = 0;"
            files["main.mjs"] = "import { w } from './m0.mjs'; print(w);"
            self.build(d, files)
            rc, out, err = run_module(Path(d) / "main.mjs", "--time-limit", "60000")
            self.assertTrue(clean(err), err[-2000:])
            self.assertEqual(out.strip(), str(n), err[-1000:])

    def test_chain_of_100000_is_a_range_error(self):
        with tempfile.TemporaryDirectory() as d:
            n = 100000
            for i in range(n):
                (Path(d) / ("m%d.mjs" % i)).write_text("import './m%d.mjs';" % (i + 1), encoding="utf-8")
            (Path(d) / ("m%d.mjs" % n)).write_text("print('end of the chain');", encoding="utf-8")
            rc, out, err = run_module(Path(d) / "m0.mjs", "--time-limit", "120000", timeout=900)
            self.assertTrue(clean(err), err[-2000:])
            self.assertEqual(rc, 1, out[-300:] + err[-1500:])
            self.assertIn("RangeError", out)
            self.assertNotIn("end of the chain", out)

    def test_cycle_of_500(self):
        with tempfile.TemporaryDirectory() as d:
            n = 500
            files = {}
            for i in range(n):
                j = (i + 1) % n
                files["c%d.mjs" % i] = "import { f%d } from './c%d.mjs'; export function f%d() { return %d; } export const s%d = f%d();" % (j, j, i, i, i, j)
            files["main.mjs"] = "import { s0 } from './c0.mjs'; import { s499 } from './c499.mjs'; print(s0, s499);"
            self.build(d, files)
            rc, out, err = run_module(Path(d) / "main.mjs", "--time-limit", "60000")
            self.assertTrue(clean(err), err[-2000:])
            self.assertEqual(out.strip(), "1 0", out + err[-1000:])

    def test_ladder_of_star_exports(self):
        # every level exports * from two modules that each export * from the next level: 2^40 paths to the bottom,
        # all of them reaching the one binding (so the name is not ambiguous)
        with tempfile.TemporaryDirectory() as d:
            n = 40
            files = {}
            for i in range(n):
                nxt = "./bottom.mjs" if i == n - 1 else None
                for side in "ab":
                    if nxt:
                        files["l%d%s.mjs" % (i, side)] = "export * from '%s'; export * from '%s';" % (nxt, nxt)
                    else:
                        files["l%d%s.mjs" % (i, side)] = "export * from './l%da.mjs'; export * from './l%db.mjs';" % (i + 1, i + 1)
            files["bottom.mjs"] = "export const bottom = 'reached';"
            files["main.mjs"] = "import * as ns from './l0a.mjs'; import { bottom } from './l0b.mjs'; print(bottom, Object.keys(ns).join());"
            self.build(d, files)
            rc, out, err = run_module(Path(d) / "main.mjs", "--time-limit", "30000")
            self.assertTrue(clean(err), err[-2000:])
            self.assertEqual(rc, 0, out + err[-1000:])
            self.assertEqual(out.strip(), "reached bottom")

    def test_chain_of_async_modules(self):
        with tempfile.TemporaryDirectory() as d:
            n = 1500
            files = {"a%d.mjs" % i: "import { log } from './log.mjs'; import './a%d.mjs'; await null; log.push(%d);" % (i + 1, i) for i in range(n)}
            files["a%d.mjs" % n] = "import { log } from './log.mjs'; await null; log.push(%d);" % n
            files["log.mjs"] = "export const log = [];"
            files["main.mjs"] = ("import { log } from './log.mjs'; import './a0.mjs';"
                                 "print(log.length, log[0], log[log.length - 1], log.every((v, i) => v === %d - i));" % n)
            self.build(d, files)
            rc, out, err = run_module(Path(d) / "main.mjs", "--time-limit", "60000")
            self.assertTrue(clean(err), err[-2000:])
            self.assertEqual(out.strip(), "%d %d 0 true" % (n + 1, n), out + err[-1000:])

    def test_many_importers_of_one_module(self):
        with tempfile.TemporaryDirectory() as d:
            n = 3000
            files = {"u%d.mjs" % i: "import { shared } from './shared.mjs'; export const x%d = shared + %d;" % (i, i) for i in range(n)}
            files["shared.mjs"] = "print('shared runs once'); export const shared = 1;"
            files["main.mjs"] = "".join("import { x%d } from './u%d.mjs';\n" % (i, i) for i in range(n)) + "print(x0 + x%d);" % (n - 1)
            self.build(d, files)
            rc, out, err = run_module(Path(d) / "main.mjs", "--time-limit", "60000")
            self.assertTrue(clean(err), err[-2000:])
            self.assertEqual(out.splitlines(), ["shared runs once", str(1 + 3000)], out + err[-1000:])

    def test_a_namespace_of_a_thousand_names(self):
        with tempfile.TemporaryDirectory() as d:
            n = 2000
            self.build(d, {"big.mjs": "".join("export const n%d = %d;\n" % (i, i) for i in range(n)),
                           "main.mjs": "import * as ns from './big.mjs'; let s = 0; for (const k of Object.keys(ns)) s += ns[k]; print(Object.keys(ns).length, s, ns.n1999, 'n2000' in ns);"})
            rc, out, err = run_module(Path(d) / "main.mjs")
            self.assertTrue(clean(err), err[-2000:])
            self.assertEqual(out.strip(), "2000 %d 1999 false" % (n * (n - 1) // 2), err[-1000:])

    def test_memory_limit_while_loading(self):
        with tempfile.TemporaryDirectory() as d:
            n = 6000
            files = {"m%d.mjs" % i: "export const v%d = %r; import './m%d.mjs';" % (i, "x" * 4000, i + 1) for i in range(n)}
            files["m%d.mjs" % n] = "export {};"
            self.build(d, files)
            rc, out, err = run_module(Path(d) / "m0.mjs", "--mem", "8")
            self.assertTrue(clean(err), err[-2000:])
            self.assertEqual(rc, 1, out[-300:] + err[-500:])
            self.assertIn("out of memory", out)

    def test_time_limit_in_a_module(self):
        with tempfile.TemporaryDirectory() as d:
            self.build(d, {"main.mjs": "import './spin.mjs';", "spin.mjs": "while (true) {}"})
            rc, out, err = run_module(Path(d) / "main.mjs", "--time-limit", "300")
            self.assertTrue(clean(err), err[-2000:])
            self.assertEqual(rc, 1)
            self.assertIn("interrupted", out)

    def test_time_limit_after_an_await(self):
        with tempfile.TemporaryDirectory() as d:
            self.build(d, {"main.mjs": "await null; while (true) {}"})
            rc, out, err = run_module(Path(d) / "main.mjs", "--time-limit", "300")
            self.assertTrue(clean(err), err[-2000:])
            self.assertEqual(rc, 1)


if __name__ == "__main__":
    unittest.main()
