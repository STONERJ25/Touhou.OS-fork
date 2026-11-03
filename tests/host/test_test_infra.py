#!/usr/bin/env python3
"""Host-side unit tests for the test infrastructure: tests/run_all.py (--retry-failed, @xfail, filters, --json) and
tools/suite.py (grouping, CI parts, summaries).

    python -m unittest discover -s tests/host -p "test_*.py" -v
    python tests/host/test_test_infra.py -v

Plain unittest with fake test functions: no QEMU, no build, no worktrees. Not part of tests/run_all.py.
"""
import contextlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO / "tests"))
sys.path.insert(0, str(REPO / "tools"))
import harness  # noqa: E402
import run_all  # noqa: E402
import suite  # noqa: E402


def fake(name, behaviour, doc="a fake test"):
    """A test function; behaviour() is called on every run and raises to fail."""
    def fn():
        behaviour()
    fn.__name__, fn.__doc__ = name, doc
    return fn


def passes():
    pass


def fails():
    raise AssertionError("always broken")


def flaky_once():
    state = {"runs": 0}

    def behaviour():
        state["runs"] += 1
        if state["runs"] == 1:
            raise TimeoutError("timeout waiting for /gapsh# / (load)")
    return behaviour, state


class RunAllTests(unittest.TestCase):
    def setUp(self):
        self.saved = list(harness.TESTS)

    def tearDown(self):
        harness.TESTS[:] = self.saved

    def run_main(self, tests, argv):
        """Runs run_all.main with exactly these tests registered; returns (exit code, output, json results)."""
        harness.TESTS[:] = tests                       # load_cases() sees a filled registry and loads nothing
        with tempfile.TemporaryDirectory() as tmp:
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = run_all.main(argv + ["--json", str(Path(tmp) / "r.json")])
            data = json.loads((Path(tmp) / "r.json").read_text(encoding="utf-8"))
        return code, out.getvalue(), data

    def test_filters(self):
        names = ["usb_kbd", "usb_kbd_hub", "usbstor_x", "blk_clock_y"]
        pick = lambda f: [n for n in names if run_all.matches(n, f.split(","))]  # noqa: E731
        self.assertEqual(pick("usb_"), ["usb_kbd", "usb_kbd_hub"])
        self.assertEqual(pick("=usb_kbd"), ["usb_kbd"])
        self.assertEqual(pick("~clock,=usbstor_x"), ["usbstor_x", "blk_clock_y"])
        self.assertEqual(pick(""), names)

    def test_flaky_is_reported_and_does_not_fail_the_run(self):
        behaviour, state = flaky_once()
        code, out, data = self.run_main([fake("t_ok", passes), fake("t_flaky", behaviour)], ["--retry-failed"])
        self.assertEqual(code, 0)
        self.assertEqual(state["runs"], 2)
        self.assertIn("FAIL t_flaky", out)                      # the first run prints the usual FAIL line
        self.assertIn("FLAKY t_flaky", out)
        self.assertIn("2 tests, 0 failed, 1 flaky", out)
        self.assertIn("flaky (failed in the run, passed alone): t_flaky", out)
        by = {r["name"]: r for r in data["results"]}
        self.assertEqual(by["t_flaky"]["status"], "FLAKY")
        self.assertIn("timeout", by["t_flaky"]["error"])        # the first failure is kept
        self.assertEqual(data["exit"], 0)

    def test_real_failure_fails_the_run_after_retry(self):
        behaviour, _ = flaky_once()
        code, out, data = self.run_main([fake("t_bad", fails), fake("t_flaky", behaviour)], ["--retry-failed"])
        self.assertEqual(code, 1)
        self.assertIn("2 tests, 1 failed, 1 flaky", out)
        self.assertIn("failed (again when rerun alone): t_bad", out)
        self.assertEqual([r["status"] for r in data["results"]], ["FAIL", "FLAKY"])

    def test_without_retry_the_old_behaviour_stays(self):
        behaviour, state = flaky_once()
        code, out, _ = self.run_main([fake("t_ok", passes), fake("t_flaky", behaviour)], [])
        self.assertEqual(code, 1)
        self.assertEqual(state["runs"], 1)
        self.assertIn("PASS t_ok", out)
        self.assertIn("\n2 tests, 1 failed\n", out)
        self.assertNotIn("Retrying", out)

    def test_filter_and_list(self):
        harness.TESTS[:] = [fake("alpha_one", passes), fake("alpha_two", passes), fake("beta", passes)]
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(run_all.main(["--list", "alpha,=beta"]), 0)
        self.assertEqual(out.getvalue().split(), ["alpha_one", "alpha_two", "beta"])

    def test_xfail_and_xpass(self):
        expected_fail = harness.xfail("old QEMU", when=lambda: True)(fake("t_xfail", fails))
        not_applicable = harness.xfail("old QEMU", when=lambda: False)(fake("t_new_qemu", fails))
        surprise = harness.xfail("old QEMU", when=lambda: True)(fake("t_xpass", passes))
        code, out, data = self.run_main([expected_fail, surprise], ["--retry-failed"])
        self.assertEqual(code, 0)                                # neither XFAIL nor a non-strict XPASS fails the run
        self.assertIn("XFAIL t_xfail", out)
        self.assertIn("XPASS t_xpass", out)
        self.assertIn("2 tests, 0 failed, 1 expected failures, 1 unexpected passes", out)
        self.assertNotIn("Retrying", out)                        # expected failures are not retried
        self.assertEqual([r["status"] for r in data["results"]], ["XFAIL", "XPASS"])
        code, out, data = self.run_main([not_applicable], [])   # condition false: an ordinary failing test
        self.assertEqual(code, 1)
        self.assertEqual(data["results"][0]["status"], "FAIL")

    def test_strict_xpass_fails_the_run(self):
        strict = harness.xfail("old QEMU", when=lambda: True, strict=True)(fake("t_strict", passes))
        code, out, _ = self.run_main([strict], [])
        self.assertEqual(code, 1)
        self.assertIn("strict", out)

    def test_broken_condition_counts_as_ordinary_test(self):
        def boom():
            raise RuntimeError("no qemu")
        marked = harness.xfail("x", when=boom)(fake("t_marked", fails))
        code, out, data = self.run_main([marked], [])
        self.assertEqual(code, 1)
        self.assertEqual(data["results"][0]["status"], "FAIL")

    def test_xfail_needs_a_reason(self):
        with self.assertRaises(ValueError):
            harness.xfail("")

    def test_usbstor_4k_blocks_is_the_only_marked_test(self):
        code = ("import sys; sys.path.insert(0, 'tests'); import run_all, harness; run_all.load_cases(); "
                "print(' '.join(fn.__name__ for fn in harness.TESTS if getattr(fn, 'xfail', None)))")
        out = subprocess.run([sys.executable, "-c", code], cwd=REPO, capture_output=True, text=True, check=True).stdout
        self.assertEqual(out.split(), ["usbstor_4k_blocks"])


class SuiteTests(unittest.TestCase):
    NAMES = ["usb_a", "usb_b", "usb_c", "storage_a", "usbstor_a", "selftest", "notebook_a", "desktop_boots", "net_a",
             "install_x", "brand_new_thing"]

    def test_named_groups_cover_every_test_once(self):
        groups = dict(suite.named_groups(self.NAMES))
        flat = [t for tests in groups.values() for t in tests]
        self.assertEqual(sorted(flat), sorted(self.NAMES))
        self.assertEqual(groups["usb"], ["usb_a", "usb_b", "usb_c"])
        self.assertEqual(groups["usbstor"], ["usbstor_a"])
        self.assertEqual(groups["rest"], ["install_x", "brand_new_thing"])     # nothing is skipped silently

    def test_only_limits_groups(self):
        groups = suite.named_groups(self.NAMES, only={"usb_b", "selftest"})
        self.assertEqual(groups, [("usb", ["usb_b"]), ("kernel", ["selftest"])])

    def test_parts_split_and_cover(self):
        tests = [f"t{i}" for i in range(7)]
        a, b = suite.part_of(tests, "1/2"), suite.part_of(tests, "2/2")
        self.assertEqual(sorted(a + b), sorted(tests))
        self.assertFalse(set(a) & set(b))

    def test_balanced_groups(self):
        names = ["a", "b", "c", "d", "e"]
        groups = suite.balanced_groups(names, 2, {"a": 100, "b": 10, "c": 50, "d": 40, "e": 5})
        totals = sorted(sum({"a": 100, "b": 10, "c": 50, "d": 40, "e": 5}[t] for t in g) for _, g in groups)
        self.assertEqual(totals, [100, 105])
        for _, g in groups:                                       # suite order is kept inside a group
            self.assertEqual(g, sorted(g, key=names.index))

    def test_aggregate_flags_failures_and_missing_groups(self):
        ok = {"exit": 0, "retry_failed": True, "results": [
            {"name": "x", "status": "PASS", "seconds": 1.0}, {"name": "y", "status": "FLAKY", "seconds": 2.0, "error": "t"},
            {"name": "z", "status": "XFAIL", "seconds": 3.0, "reason": "old QEMU"}]}
        md, bad = suite.aggregate_markdown([("one", ok)])
        self.assertFalse(bad)
        self.assertIn("Whole suite: passed", md)
        self.assertIn("`y` (one)", md)
        md, bad = suite.aggregate_markdown([("one", ok), ("two", None)])
        self.assertTrue(bad)
        self.assertIn("| two | did not finish", md)
        failing = {"exit": 1, "results": [{"name": "w", "status": "FAIL", "seconds": 4.0, "error": "boom | `x`"}]}
        md, bad = suite.aggregate_markdown([("one", ok), ("three", failing)])
        self.assertTrue(bad)
        self.assertIn("**Failed (also when rerun alone):** `w` (three)", md)
        self.assertIn("boom \\| 'x'", suite.group_markdown("three", failing))

    def test_aggregate_command_uses_the_plan(self):
        plan = json.dumps({"include": [{"group": "kernel", "part": "1/1", "label": "kernel"},
                                       {"group": "usb", "part": "1/2", "label": "usb-1of2"}]})
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "a").mkdir()
            Path(tmp, "a", "suite-kernel.json").write_text(json.dumps(
                {"label": "kernel", "exit": 0, "results": [{"name": "selftest", "status": "PASS", "seconds": 5.0}]}))
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(suite.main(["--aggregate", tmp, "--expect-plan", plan]), 1)     # usb-1of2 left nothing
                self.assertEqual(suite.main(["--aggregate", tmp]), 0)                            # without the plan: what is there
            self.assertIn("| usb-1of2 | did not finish", out.getvalue())

    def test_ci_plan_matches_the_groups(self):
        plan = suite.ci_plan()
        labels = [e["label"] for e in plan]
        self.assertEqual(len(labels), len(set(labels)))
        self.assertIn("usb-1of2", labels)
        self.assertIn("storage-2of2", labels)
        covered = []
        groups = dict(suite.named_groups(suite.list_tests()))
        for e in plan:
            covered += suite.part_of(groups[e["group"]], e["part"])
        self.assertEqual(sorted(covered), sorted(suite.list_tests()))


if __name__ == "__main__":
    unittest.main()
