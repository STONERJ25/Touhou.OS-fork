#!/usr/bin/env python3
"""Host tests for kernel/include/ktime_elapsed.h: the elapsed-time helpers that replace the kernel's unsigned `now - stamp` where the stamp can be the newer reading
(docs/RELIABILITY.md, "Elapsed time and unsigned subtraction"; the bug that header exists for is docs/NETWORK.md, "The first-connection stall").

The header is compiled as it is with the host's C compiler into a small program that prints the helpers' results for a table of (a, b, limit) triples; the expected
values are computed here with Python's unbounded integers, so a wrap cannot hide on both sides. The boundary cases: equal readings, a stamp one millisecond newer (the
ARP case: the old subtraction gave 2^64-1), the ends of the 64-bit range, and the comparison limits one below, at and one above the elapsed time.

    python tools/hosttest.py discover -s tests/host -p "test_ktime_elapsed.py" -v
"""
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import x86ref  # noqa: E402

HEADER = x86ref.REPO / "kernel" / "include" / "ktime_elapsed.h"
M = 2 ** 64 - 1

PROGRAM = r"""
#include <stdio.h>
#include <stdlib.h>
#include "ktime_elapsed.h"
int main(int argc, char **argv) {
    for (int i = 1; i + 2 < argc; i += 3) {
        uint64_t a = strtoull(argv[i], 0, 0), b = strtoull(argv[i + 1], 0, 0), ms = strtoull(argv[i + 2], 0, 0);
        printf("%llu %llu %d %d\n", (unsigned long long)ktime_elapsed(a, b), (unsigned long long)ktime_left(a, b), ktime_older_than(a, b, ms), ktime_at_least(a, b, ms));
    }
    return 0;
}
"""


def build():
    out = x86ref.OUT / "ktime_elapsed_test"
    src = x86ref.OUT / "ktime_elapsed_test.c"
    x86ref.OUT.mkdir(parents=True, exist_ok=True)
    if out.exists() and src.exists() and src.read_text() == PROGRAM and HEADER.stat().st_mtime <= out.stat().st_mtime:
        return out
    src.write_text(PROGRAM)
    cc = x86ref.find_cc()
    gcc = "gcc" in Path(cc).name.lower()
    flags = ["-O1", "-fsanitize=undefined", "-fno-sanitize-recover=undefined", "-Werror"] if gcc else ["-O2"]
    r = subprocess.run([cc, *flags, "-std=gnu17", "-Wall", "-Wextra", "-I", str(HEADER.parent), str(src), "-o", str(out)], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("building the ktime_elapsed test failed:\n" + r.stdout + r.stderr)
    return out


def ask(cases):
    args = [str(x) for case in cases for x in case]
    r = subprocess.run([str(build()), *args], capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stdout + r.stderr
    return [tuple(int(x) for x in line.split()) for line in r.stdout.splitlines()]


def expect(a, b, ms):
    """What the header promises, in whole numbers: (elapsed(a, b), left(a, b), older_than, at_least)."""
    d = a - b if a > b else 0
    return d, d, int(d > ms), int(d >= ms)


@unittest.skipUnless(x86ref.have_gcc(), "needs a C compiler")
class KtimeElapsed(unittest.TestCase):
    def check(self, cases):
        for case, got in zip(cases, ask(cases), strict=True):
            self.assertEqual(got, expect(*case), f"(a, b, ms) = {case}")

    def test_the_stamp_is_older(self):
        self.check([(1000, 400, 599), (1000, 400, 600), (1000, 400, 601), (1, 0, 0), (1, 0, 1), (300001, 0, 300000), (300000, 0, 300000)])

    def test_equal_readings_are_zero(self):
        got = ask([(0, 0, 0), (5, 5, 0), (5, 5, 1), (M, M, 0)])
        self.assertEqual(got, [(0, 0, 0, 1), (0, 0, 0, 1), (0, 0, 0, 0), (0, 0, 0, 1)])

    def test_a_stamp_one_millisecond_newer_is_just_now_not_2_to_the_64(self):
        """The ARP case: `now` 4999 read before the poll, the entry stamped 5000 inside it, the five-minute limit. The plain subtraction said 2^64-1 > 300000."""
        got = ask([(4999, 5000, 300000), (4999, 5000, 0), (0, 1, 0), (0, M, 1), (M - 1, M, M - 1)])
        self.assertEqual(got, [(0, 0, 0, 0), (0, 0, 0, 1), (0, 0, 0, 1), (0, 0, 0, 0), (0, 0, 0, 0)])
        self.assertEqual((4999 - 5000) % 2 ** 64, M, "what the unsigned subtraction gave")

    def test_the_ends_of_the_range(self):
        self.check([(M, 0, M), (M, 0, M - 1), (M, 1, M - 1), (M, M - 1, 0), (M, M - 1, 1), (2 ** 63, 2 ** 63 - 1, 1), (2 ** 63 - 1, 2 ** 63, 0), (0, 0, M), (M, 0, 0)])

    def test_left_of_a_deadline(self):
        got = ask([(1500, 1000, 0), (1000, 1000, 0), (999, 1000, 0), (M, 0, 0), (0, M, 0)])       # ktime_left(deadline, now) is the second number
        self.assertEqual([g[1] for g in got], [500, 0, 0, M, 0])

    def test_a_sweep_against_whole_numbers(self):
        edge = [0, 1, 2, 999, 1000, 1001, 300000, 2 ** 32 - 1, 2 ** 32, 2 ** 63 - 1, 2 ** 63, M - 1, M]
        self.check([(a, b, ms) for a in edge for b in edge for ms in (0, 1, 1000, 300000, M)])


if __name__ == "__main__":
    unittest.main()
