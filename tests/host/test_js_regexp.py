#!/usr/bin/env python3
"""Differential test of Kosuzu's RegExp engine against Python's re module (tests/host/test_js_regexp.py).

    python tools/hosttest.py discover -s tests/host -p "test_js_regexp.py" -v

Random patterns over a small grammar (literals, classes, groups, alternation, greedy and lazy quantifiers, anchors,
word boundaries, lookahead and fixed-width lookbehind) are matched against random short inputs by both engines; the
span of the match must agree always, and the captures when no group sits inside a quantifier (the one place where
ECMAScript and Python differ on purpose: JavaScript clears a group's capture at the start of every iteration).
Backreferences are left out (an unset group fails in Python, matches the empty string in JavaScript).

The fixed seed makes failures reproducible: the failing pattern, flags and input are printed.
"""
import json
import os
import random
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_js  # noqa: E402  (the tool builder and runner)


class Gen:
    """Random patterns.  Every piece is returned with whether it can match the empty string: a quantifier is only
    put on pieces that cannot, because an iteration that matches nothing is handled differently by ECMAScript (the
    iteration is rejected and the other alternatives are tried) and by Python (the loop just ends)."""

    def __init__(self, rng):
        self.rng = rng
        self.in_quant = 0
        self.captures_in_quant = False

    def literal(self):
        return self.rng.choice("abc"), False

    def cls(self):
        return self.rng.choice(["[ab]", "[bc]", "[^a]", "[a-c]", "[^bc]", r"\w", r"\d", r"\s", r"\W", "[abc ]"]), False

    def atom(self, depth):
        r = self.rng.random()
        if depth <= 0 or r < 0.30:
            return self.literal()
        if r < 0.38:
            return ".", False
        if r < 0.50:
            return self.cls()
        if r < 0.66:
            t, n = self.alt(depth - 1)
            return "(?:" + t + ")", n
        if r < 0.80:
            if self.in_quant:
                self.captures_in_quant = True
            t, n = self.alt(depth - 1)
            return "(" + t + ")", n
        if r < 0.86:
            t, _ = self.alt(depth - 1)
            return "(?=" + t + ")", True
        if r < 0.91:
            t, _ = self.alt(depth - 1)
            return "(?!" + t + ")", True
        if r < 0.95:
            return "(?<=" + self.fixed() + ")", True
        return "(?<!" + self.fixed() + ")", True

    def fixed(self):
        return "".join(self.rng.choice(["a", "b", "c", "[ab]", ".", r"\w"]) for _ in range(self.rng.randint(1, 2)))

    def quant(self):
        r = self.rng.random()
        if r < 0.55:
            return "", False
        q = self.rng.choice(["*", "+", "?", "{2}", "{1,2}", "{0,2}", "{2,}", "*", "+", "?"])
        if self.rng.random() < 0.3:
            q += "?"
        return q, q[0] in "*?" or q.startswith("{0")

    def term(self, depth):
        if self.rng.random() < 0.06:
            return self.rng.choice(["^", "$", r"\b", r"\B"]), True
        q, qnull = self.quant()
        self.in_quant += 1 if q else 0
        a, anull = self.atom(depth)
        self.in_quant -= 1 if q else 0
        if a.startswith("(?<=") or a.startswith("(?<!"):
            return a, True                  # a lookbehind cannot be quantified in JavaScript
        if anull:
            return a, True                  # never quantify a piece that can match nothing (see above)
        return a + q, qnull

    def seq(self, depth):
        parts = [self.term(depth) for _ in range(self.rng.randint(1, 3))]
        return "".join(t for t, _ in parts), all(n for _, n in parts)

    def alt(self, depth):
        parts = [self.seq(depth) for _ in range(1 if self.rng.random() < 0.7 else self.rng.randint(2, 3))]
        return "|".join(t for t, _ in parts), any(n for _, n in parts)


def to_python(pattern, multiline):
    # JavaScript's $ matches only at the very end (without m); Python's also before a final newline
    if not multiline:
        pattern = pattern.replace("$", "\\Z")
    return pattern


def py_result(pattern, flags, text):
    if text == "" and r"\B" in pattern:
        return "ERR"                        # Python: "\B does not match an empty string"; ECMAScript: it does
    try:
        f = re.ASCII | (re.MULTILINE if "m" in flags else 0) | (re.IGNORECASE if "i" in flags else 0)
        m = re.compile(to_python(pattern, "m" in flags), f).search(text)
    except (re.error, RecursionError, OverflowError):
        return "ERR"
    if m is None:
        return None
    return [m.start(), m.end()] + list(m.groups())


class Differential(unittest.TestCase):
    def test_against_python_re(self):
        # JS_REGEXP_FUZZ=60000 JS_REGEXP_FUZZ_SEED=7 runs a bigger one
        rng = random.Random(int(os.environ.get("JS_REGEXP_FUZZ_SEED", "20261103")))
        cases = []
        for _ in range(int(os.environ.get("JS_REGEXP_FUZZ", "6000"))):
            g = Gen(rng)
            pattern, _ = g.alt(3)
            flags = rng.choice(["", "", "", "m", "i"])
            text = "".join(rng.choice("abc  \n") for _ in range(rng.randint(0, 10)))
            cases.append((pattern, flags, text, g.captures_in_quant))
        js = ("var cases = %s;\nvar out = [];\n"
              "for (var i = 0; i < cases.length; i++) {\n"
              "  try { var m = new RegExp(cases[i][0], cases[i][1]).exec(cases[i][2]);\n"
              "    out.push(m === null ? null : [m.index, m.index + m[0].length].concat(m.slice(1).map(function (x) { return x === undefined ? null : x; })));\n"
              "  } catch (e) { out.push('ERR'); }\n}\n"
              "print(JSON.stringify(out));\n") % json.dumps([[p, f, t] for p, f, t, _ in cases])
        path = test_js.OUT / "regexp_fuzz.js"
        test_js.OUT.mkdir(parents=True, exist_ok=True)
        path.write_text(js, encoding="utf-8")
        rc, out, err = test_js.run([str(path)], exe=test_js.fast_tool(), timeout=600)
        self.assertEqual(rc, 0, err[-2000:])
        got = json.loads(out)
        checked = bad = 0
        for (pattern, flags, text, quant), g in zip(cases, got):
            want = py_result(pattern, flags, text)
            if want == "ERR":
                continue                    # Python does not accept it (or differs in syntax): nothing to compare
            self.assertNotEqual(g, "ERR", "our engine rejected /%s/%s" % (pattern, flags))
            checked += 1
            if want is None or g is None:
                ok = want == g
            elif quant:
                ok = want[:2] == g[:2]      # only the span: captures inside quantifiers legitimately differ
            else:
                ok = want == g
            if not ok:
                bad += 1
                if bad <= 10:
                    print("MISMATCH /%s/%s on %r: ours %r, re %r" % (pattern, flags, text, g, want))
        self.assertGreater(checked, len(cases) * 2 // 3)
        self.assertEqual(bad, 0, "%d of %d patterns disagree (first ones printed above)" % (bad, checked))


if __name__ == "__main__":
    unittest.main()
