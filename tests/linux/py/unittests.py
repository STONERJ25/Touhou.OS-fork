"""The unittest module: a small suite with passing, failing, erroring, skipped and expected-to-fail tests, subtests, fixtures, mocks, assertion messages, a test loader and a runner whose
report is printed without times or paths."""
import io
import unittest
from unittest import mock


def show(label, value=""):
    print(f"{label}: {value}")


class Calculator:
    def add(self, a, b):
        return a + b

    def divide(self, a, b):
        return a / b


class CalcTests(unittest.TestCase):
    log = []

    @classmethod
    def setUpClass(cls):
        cls.log.append("setUpClass")

    @classmethod
    def tearDownClass(cls):
        cls.log.append("tearDownClass")

    def setUp(self):
        self.calc = Calculator()
        self.log.append(f"setUp {self._testMethodName}")

    def tearDown(self):
        self.log.append(f"tearDown {self._testMethodName}")

    def test_add(self):
        self.assertEqual(self.calc.add(2, 3), 5)

    def test_divide(self):
        self.assertAlmostEqual(self.calc.divide(1, 3), 0.3333333, places=6)

    def test_zero(self):
        with self.assertRaises(ZeroDivisionError):
            self.calc.divide(1, 0)

    def test_fails(self):
        self.assertEqual(self.calc.add(2, 2), 5, "arithmetic is broken")

    def test_errors(self):
        self.calc.nothing()

    @unittest.skip("not today")
    def test_skipped(self):
        self.fail("must not run")

    @unittest.skipIf(True, "condition holds")
    def test_skipped_if(self):
        self.fail("must not run")

    @unittest.expectedFailure
    def test_known_bug(self):
        self.assertEqual(1, 2)

    @unittest.expectedFailure
    def test_fixed_bug(self):
        self.assertEqual(1, 1)

    def test_subtests(self):
        for n in range(5):
            with self.subTest(n=n):
                self.assertNotEqual(n % 3, 2, f"{n} is 2 mod 3")

    def test_assertions(self):
        self.assertTrue(1)
        self.assertIn(2, [1, 2, 3])
        self.assertIsNone(None)
        self.assertListEqual([1, 2], [1, 2])
        self.assertDictEqual({"a": 1}, {"a": 1})
        self.assertRegex("hello world", r"w\w+d")
        self.assertIsInstance(3, int)
        self.assertCountEqual([1, 2, 2], [2, 1, 2])
        self.assertGreater(3, 2)
        with self.assertRaisesRegex(ValueError, "invalid literal"):
            int("x")
        with self.assertLogs("demo", level="WARNING") as cm:
            import logging
            logging.getLogger("demo").warning("watch out")
        self.assertEqual(cm.output, ["WARNING:demo:watch out"])

    def test_diff(self):
        self.assertEqual([1, 2, 3, 4], [1, 2, 4, 3])

    def test_mocks(self):
        m = mock.Mock(return_value=42)
        self.assertEqual(m(1, key="v"), 42)
        m.assert_called_once_with(1, key="v")
        with mock.patch.object(Calculator, "add", return_value=99) as patched:
            self.assertEqual(Calculator().add(1, 1), 99)
        patched.assert_called_once()
        self.assertEqual(Calculator().add(1, 1), 2)
        mm = mock.MagicMock()
        mm.thing.return_value = 7
        self.assertEqual(mm.thing(), 7)
        self.assertEqual(mm.mock_calls[0], mock.call.thing())
        with mock.patch.dict("os.environ", {"UNITTEST_VARIABLE": "set"}):
            import os
            self.assertEqual(os.environ["UNITTEST_VARIABLE"], "set")
        self.assertNotIn("UNITTEST_VARIABLE", __import__("os").environ)


suite = unittest.defaultTestLoader.loadTestsFromTestCase(CalcTests)
show("tests found", (suite.countTestCases(), sorted(t._testMethodName for t in suite)[:3]))
stream = io.StringIO()
result = unittest.TextTestRunner(stream=stream, verbosity=2).run(suite)
show("ran", result.testsRun)
show("outcome", (len(result.failures), len(result.errors), len(result.skipped), len(result.expectedFailures), len(result.unexpectedSuccesses), result.wasSuccessful()))
for line in stream.getvalue().splitlines():
    if line.startswith(("test_", "setUpClass")) or " ... " in line:
        print("  " + line.split(" (")[0] + " ... " + line.rsplit(" ... ", 1)[-1])
for test, tb in result.failures:
    lines = [l for l in tb.splitlines() if l.startswith(("AssertionError", "  - ", "  + ", "? ", "- ", "+ "))]
    show(f"failure of {test._testMethodName}", lines)
for test, tb in result.errors:
    show(f"error of {test._testMethodName}", tb.splitlines()[-1])
show("skip reasons", sorted(reason for _, reason in result.skipped))
show("fixture order", CalcTests.log[:3] + ["..."] + CalcTests.log[-2:])
show("setUp and tearDown pairs", (sum(1 for l in CalcTests.log if l.startswith("setUp ")), sum(1 for l in CalcTests.log if l.startswith("tearDown "))))


class Quick(unittest.TestCase):
    def test_one(self):
        pass

    def test_two(self):
        self.assertEqual(1 + 1, 2)


s2 = io.StringIO()
r2 = unittest.TextTestRunner(stream=s2, verbosity=0).run(unittest.defaultTestLoader.loadTestsFromTestCase(Quick))
show("a passing suite", (r2.wasSuccessful(), r2.testsRun, s2.getvalue().splitlines()[0], s2.getvalue().splitlines()[-1]))
show("FunctionTestCase", unittest.TextTestRunner(stream=io.StringIO()).run(unittest.FunctionTestCase(lambda: None)).wasSuccessful())
print("--- done")
