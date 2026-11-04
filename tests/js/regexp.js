// RegExp: core matching semantics (ECMA-262 22.2.2 Pattern Semantics, the worked examples of the specification, and
// Annex B.1.2).  Self-checking: every check states the expected value; the file prints its failures and a summary.
var total = 0, fails = 0;
function repr(v) {
  if (typeof v === "string") return JSON.stringify(v);
  if (Array.isArray(v)) return "[" + v.map(repr).join(",") + "]";
  return String(v);
}
function eq(actual, expected, label) {
  total++;
  var a = repr(actual), e = repr(expected);
  if (a !== e) { fails++; print("FAIL " + label + ": got " + a + ", want " + e); }
}
function m(re, s) { var r = re.exec(s); return r === null ? null : Array.from(r); }
function throws(f, ctor, label) {
  total++;
  try { f(); } catch (e) { if (e instanceof ctor) return; fails++; print("FAIL " + label + ": wrong error " + e); return; }
  fails++; print("FAIL " + label + ": no error");
}
function syntaxError(src, flags) { throws(function () { new RegExp(src, flags); }, SyntaxError, "SyntaxError for /" + src + "/" + (flags || "")); }
function valid(src, flags) {
  total++;
  try { new RegExp(src, flags); } catch (e) { fails++; print("FAIL valid /" + src + "/" + (flags || "") + ": " + e); }
}

// ---- basics
eq(m(/abc/, "xabcx"), ["abc"], "literal");
eq(m(/a.c/, "abc"), ["abc"], "dot");
eq(m(/a.c/, "a\nc"), null, "dot does not match \\n");
eq(m(/a.c/, "a c"), null, "dot does not match U+2028");
eq(m(/a|b|c/, "xc"), ["c"], "alternation");
eq(m(/^abc$/, "abc"), ["abc"], "anchors");
eq(m(/^abc$/, "abc\n"), null, "$ without m does not match before a final newline");
eq(m(/x*/, "abc"), [""], "empty match at 0");
eq(m(/(a)(b)?/, "ac"), ["a", "a", undefined], "unmatched optional group is undefined");
eq(m(/a{3}/, "aaaa"), ["aaa"], "{n}");
eq(m(/a{2,}/, "aaaa"), ["aaaa"], "{n,}");
eq(m(/a{0,1}/, "aa"), ["a"], "{0,1}");
eq(m(/a{1,2}?/, "aa"), ["a"], "lazy {1,2}");
eq(m(/a??/, "a"), [""], "lazy ?");
eq(m(/a*?$/, "aaa"), ["aaa"], "lazy star must reach the anchor");
eq(m(/\d+/, "ab123cd"), ["123"], "\\d+");
eq(m(/\D+/, "12ab34"), ["ab"], "\\D+");
eq(m(/\s+/, "a \t\n ﻿b"), [" \t\n ﻿"], "\\s covers NBSP and BOM");
eq(m(/\S+/, "  ab "), ["ab"], "\\S+");
eq(m(/\w+/, "-ab_9-"), ["ab_9"], "\\w+");
eq(m(/\W+/, "ab-+cd"), ["-+"], "\\W+");
eq(m(/[^\n]+/, "ab\ncd"), ["ab"], "negated class");
eq(m(/a\.b/, "a.b"), ["a.b"], "escaped dot");
eq(m(/\x41B\103/, "ABC"), ["ABC"], "hex, unicode and octal escapes");
eq(m(/\cJ/, "\n"), ["\n"], "control escape");
eq(m(/[\b]/, "\b"), ["\b"], "backspace in a class");
eq(m(/\t\n\v\f\r/, "\t\n\v\f\r"), ["\t\n\v\f\r"], "control escapes");
eq(m(/\0/, "\0"), ["\0"], "NUL escape");

// ---- the examples in the specification
eq(m(/(z)((a+)?(b+)?(c))*/, "zaacbbbcac"), ["zaacbbbcac", "z", "ac", "a", undefined, "c"], "RepeatMatcher example: captures reset on every iteration");
eq(m(/(a|ab)(c|bcd)(d*)/, "abcd"), ["abcd", "a", "bcd", ""], "backtracking example");
eq(m(/a[a-z]{2,4}/, "abcdefghi"), ["abcde"], "greedy bounded class");
eq(m(/a[a-z]{2,4}?/, "abcdefghi"), ["abc"], "lazy bounded class");
eq(m(/(aa|aabaac|ba|b|c)*/, "aabaac"), ["aaba", "ba"], "ordered alternatives inside a loop");
eq(m(/(?=(a+))/, "baaabac"), ["", "aaa"], "lookahead keeps its captures");
eq(m(/(?=(a+))a*b\1/, "baaabac"), ["aba", "a"], "lookahead capture used by a backreference");
eq(m(/(.*?)a(?!(a+)b\2c)\2(.*)/, "baaabaac"), ["baaabaac", "ba", undefined, "abaac"], "negative lookahead leaves its group undefined");
eq(m(/(?<=(\d+)(\d+))$/, "1053"), ["", "1", "053"], "lookbehind matches right to left");
eq(m(/(?<=\1(a))b/, "aab"), ["b", "a"], "backreference inside a lookbehind");
eq(m(/(?<=([abc])\1)d/, "aad"), ["d", "a"], "backreference before its group in a lookbehind is satisfied by the group on its right");

// ---- quantifier details
eq(m(/(a*)*/, "b"), ["", undefined], "(a*)* on b: the empty iteration is rejected");
eq(m(/(a*)+/, "b"), ["", ""], "(a*)+ on b: the first iteration may be empty");
eq(m(/(?:a?)*?b/, "aab"), ["aab"], "lazy loop over an optional body");
eq(m(/(a|(b))+/, "ba"), ["ba", "a", undefined], "group 2 is reset when the second iteration starts");
eq(m(/(?:(a)|b)+/, "ab"), ["ab", undefined], "capture from an earlier iteration is cleared");
eq(m(/(\2a)(b)/, "ab"), ["ab", "a", "b"], "reference to a later group matches the empty string");
eq(m(/(?:(a)|b)\1/, "b"), ["b", undefined], "backreference to an unset group matches empty");
eq(m(/\1(a)/, "aa"), ["a", "a"], "forward reference");
eq(m(/(a)\1/, "aa"), ["aa", "a"], "backreference");
eq(m(/(a+)\1/, "aaaa"), ["aaaa", "aa"], "backtracking into a captured group");
eq(m(/(a)|b\1/, "b"), ["b", undefined], "backreference in another alternative");
eq(m(/(?:x{2}){3}/, "xxxxxxxx"), ["xxxxxx"], "nested counted loops");
eq(m(/(?:ab){2,3}c/, "ababababc"), ["abababc"], "counted loop with a suffix");
eq(m(/(a{1,2}){2}/, "aaa"), ["aaa", "a"], "counted loop capture is the last iteration");
eq(m(/(?:a*)*b/, "aaac"), null, "nested stars fail cleanly");
eq(m(/(a?)*?c/, "aac"), ["aac", "a"], "lazy star of a nullable group");
eq(m(/x{0}y/, "xy"), ["y"], "x{0}");
eq(m(/(a){0}b/, "ab"), ["b", undefined], "group in x{0} stays undefined");
eq(m(/a{2147483648}/, "aaa"), null, "huge counts are accepted");
eq(m(/a{1,2}{/, "aa{"), ["aa{"], "Annex B: a lone { after a quantifier");

// ---- anchors, word boundaries, lookaround
eq(m(/^a/m, "b\na"), ["a"], "^ with m");
eq(m(/a$/m, "a\nb"), ["a"], "$ with m");
eq(m(/^$/m, "a\n\nb"), [""], "empty line with m");
eq(m(/\bfoo\b/, "a foo b"), ["foo"], "\\b");
eq(m(/\Bfoo/, "afoo"), ["foo"], "\\B");
eq(m(/\b/, ""), null, "\\b on the empty string");
eq(m(/\B/, ""), [""], "\\B on the empty string");
eq(m(/(?=a)a/, "a"), ["a"], "positive lookahead");
eq(m(/(?!a)b/, "b"), ["b"], "negative lookahead");
eq(m(/a(?=b)/, "ab"), ["a"], "lookahead consumes nothing");
eq(m(/(?<=\$)\d+/, "cost $42"), ["42"], "lookbehind");
eq(m(/(?<!\$)\b\d+/, "cost $42 or 17"), ["17"], "negative lookbehind");
eq(m(/(?<=a|bc)d/, "bcd"), ["d"], "lookbehind with alternatives of different lengths");
eq(m(/(?<=^|,)\w/g, "a,b"), ["a"], "lookbehind with an anchor");
eq(m(/(?<![a-z])\d/, "a1 2"), ["2"], "negative lookbehind with a class");
eq(m(/(?=(?<x>a))\k<x>/, "a"), ["a", "a"], "named group in a lookahead");
eq(m(/(?!(a))\1b/, "b"), ["b", undefined], "group inside a negative lookahead stays undefined");
eq(m(/(?=(a))?/, "a"), ["", undefined], "Annex B quantified lookahead: the optional lookahead is skipped when empty");
eq(m(/(?<=(?<a>\w){3})f/, "abcdef"), ["f", "c"], "lookbehind captures the right-most match");
eq(m(/(?<=\b)a/, "a"), ["a"], "lookbehind with an assertion");
eq(m(/(?<=a(?=b))b/, "ab"), ["b"], "lookahead nested in a lookbehind");

// ---- groups and names
var nm = /(?<year>\d{4})-(?<mon>\d{2})/.exec("on 2024-05-17");
eq(nm.groups.year, "2024", "named group year");
eq(nm.groups.mon, "05", "named group mon");
eq(Object.getPrototypeOf(nm.groups), null, "groups has a null prototype");
eq(Object.keys(nm), ["0", "1", "2", "index", "input", "groups"], "exec result property order");
eq(nm.index, 3, "index");
eq(/(?<a>x)|(?<b>y)/.exec("y").groups.a, undefined, "unmatched named group");
eq(Object.keys(/(?<a>x)|(?<b>y)/.exec("y").groups), ["a", "b"], "all names are present");
eq(/a/.exec("a").groups, undefined, "groups is undefined without names");
eq(m(/(?<a>.)\k<a>/, "xaab"), ["aa", "a"], "\\k<name>");
eq(m(/\k<a>(?<a>x)/, "x"), ["x", "x"], "forward \\k<name>");
eq(m(/(?<𝒜>.)/u, "z"), ["z", "z"], "astral identifier in a group name");
eq(/(?<ab>x)/.exec("x").groups.ab, "x", "unicode escape in a group name");
eq(/(?<\u{62}c>x)/.exec("x").groups.bc, "x", "braced escape in a group name");
eq(/(?<$_x1>x)/.exec("x").groups.$_x1, "x", "$ and _ in a group name");
// ES2025: duplicate named groups in different alternatives
eq(/(?<n>a)|(?<n>b)/.exec("b").groups.n, "b", "duplicate name, second alternative");
eq(/(?<n>a)|(?<n>b)/.exec("a").groups.n, "a", "duplicate name, first alternative");
eq(m(/(?:(?<n>a)|(?<n>b))\k<n>/, "bb"), ["bb", undefined, "b"], "\\k with duplicate names uses the group that took part");
eq("xay".replace(/(?:(?<n>a)|(?<n>b))/, "[$<n>]"), "x[a]y", "$<n> with duplicate names");

// ---- flags and lastIndex
var r = /a/g;
eq(r.test("aaa"), true, "g test 1"); eq(r.lastIndex, 1, "lastIndex 1");
eq(r.test("aaa"), true, "g test 2"); eq(r.lastIndex, 2, "lastIndex 2");
eq(r.test("aaa"), true, "g test 3"); eq(r.lastIndex, 3, "lastIndex 3");
eq(r.test("aaa"), false, "g test 4"); eq(r.lastIndex, 0, "lastIndex reset");
r = /a/y; r.lastIndex = 1;
eq(r.test("ba"), true, "sticky at lastIndex"); eq(r.lastIndex, 2, "sticky lastIndex");
r.lastIndex = 0;
eq(r.test("ba"), false, "sticky fails off lastIndex"); eq(r.lastIndex, 0, "sticky failure resets lastIndex");
r = /a/; r.lastIndex = 5;
eq(r.test("a"), true, "non-global ignores lastIndex"); eq(r.lastIndex, 5, "non-global does not touch lastIndex");
r = /a/g; r.lastIndex = 9;
eq(r.test("a"), false, "lastIndex beyond the length"); eq(r.lastIndex, 0, "reset after failure");
r = /a/g; r.lastIndex = -1;
eq(r.test("a"), true, "negative lastIndex is clamped to 0 by ToLength");
r = /a/g; r.lastIndex = { valueOf: function () { return 1; } };
eq(r.test("aa"), true, "lastIndex goes through ToLength"); eq(r.lastIndex, 2, "after valueOf");
r = /a/y;
eq(m(r, "ba"), null, "sticky: no search");
eq(/^b/y.test("ab"), false, "sticky with ^ at lastIndex 0");
r = /^b/my; r.lastIndex = 2;
eq(r.test("a\nb"), true, "sticky multiline ^ at a line start");
eq(/a/gimsuyd.flags, "dgimsuy", "flags in canonical order");
eq(/a/v.flags, "v", "v flag");
eq(/a/g.global && /a/i.ignoreCase && /a/m.multiline && /a/s.dotAll && /a/u.unicode && /a/v.unicodeSets && /a/y.sticky && /a/d.hasIndices, true, "flag getters");
eq(/a/.global || /a/.ignoreCase || /a/.multiline || /a/.dotAll || /a/.unicode || /a/.unicodeSets || /a/.sticky || /a/.hasIndices, false, "flag getters off");
eq(/a.c/s.test("a\nc"), true, "dotAll");
eq(/a.c/s.test("a c"), true, "dotAll with a paragraph separator");

// ---- indices (the d flag)
var d = /a(b)?(?<x>c)/d.exec("zac");
eq(d.indices.length, 3, "indices length");
eq(Array.from(d.indices[0]), [1, 3], "indices[0]");
eq(d.indices[1], undefined, "indices of an unmatched group");
eq(Array.from(d.indices[2]), [2, 3], "indices[2]");
eq(Array.from(d.indices.groups.x), [2, 3], "indices.groups.x");
eq(Object.keys(d), ["0", "1", "2", "index", "input", "groups", "indices"], "key order with d");
eq(/a/d.exec("a").indices.groups, undefined, "indices.groups without names");
eq(/a/.exec("a").indices, undefined, "no indices without d");

// ---- classes
eq(m(/[a-c]+/, "xxabcxx"), ["abc"], "range");
eq(m(/[^a-c]+/, "abxyc"), ["xy"], "negated range");
eq(m(/[\d-x]+/, "5-x"), ["5-x"], "Annex B: a class escape next to - is a union");
eq(m(/[a-\d]+/, "a-5"), ["a-5"], "Annex B: - after a class atom");
eq(m(/[]/, "a"), null, "empty class matches nothing");
eq(m(/[^]/, "\n"), ["\n"], "[^] matches anything");
eq(m(/[\]]/, "]"), ["]"], "escaped ]");
eq(m(/[a\-z]+/, "a-z"), ["a-z"], "escaped -");
eq(m(/[-a]+/, "-a"), ["-a"], "leading -");
eq(m(/[a-]+/, "a-"), ["a-"], "trailing -");
eq(m(/[\w-]+/, "a-b c"), ["a-b"], "- after a class escape at the end");
eq(m(/[\s\S]/, "\n"), ["\n"], "[\\s\\S]");
eq(m(/[.]/, "a."), ["."], "dot in a class is literal");
eq(m(/[\1]/, "\x01"), ["\x01"], "Annex B: octal escape in a class");
eq(m(/[\c]/, "\\"), ["\\"], "Annex B: \\c in a class is a backslash");
eq(m(/[\c_]/, "\x1f"), ["\x1f"], "Annex B: \\c_ in a class");
eq(m(/[\cA]/, "\x01"), ["\x01"], "\\cA in a class");
eq(m(/[/]/, "/"), ["/"], "slash in a class");
eq(m(/[A-C]+/, "ABCD"), ["ABC"], "unicode escapes as range ends");
eq(m(/[\x41-\x43]+/, "ABCD"), ["ABC"], "hex escapes as range ends");

// ---- Annex B legacy syntax
eq(m(/\8/, "8"), ["8"], "\\8 is an identity escape");
eq(m(/\18/, "\x018"), ["\x018"], "\\18 is octal 1 then 8");
eq(m(/(a)\2/, "a\x02"), ["a\x02", "a"], "\\2 beyond the group count is octal");
eq(m(/\10/, "\x08"), ["\x08"], "\\10 without ten groups is octal");
eq(m(/\377/, "\xff"), ["\xff"], "\\377");
eq(m(/\400/, "\x200"), ["\x200"], "\\400 is \\40 and 0");
eq(m(/a{/, "a{"), ["a{"], "a lone { is a literal");
eq(m(/a{1/, "a{1"), ["a{1"], "an incomplete quantifier is literal");
eq(m(/a{,5}/, "a{,5}"), ["a{,5}"], "{,n} is literal");
eq(m(/{/, "{"), ["{"], "{ at the start");
eq(m(/}/, "}"), ["}"], "} is literal");
eq(m(/]/, "]"), ["]"], "] is literal");
eq(m(/x{1}{/, "x{"), ["x{"], "{ after a quantifier");
eq(m(/\c/, "\\c"), ["\\c"], "\\c without a letter is a backslash and c");
eq(m(/\cA/, "\x01"), ["\x01"], "\\cA");
eq(m(/\a/, "a"), ["a"], "unknown escape is the character");
eq(m(/\k/, "k"), ["k"], "\\k without named groups");
eq(m(/\p{L}/, "p{L}"), ["p{L}"], "\\p without u is p");
eq(m(/\u{61}/, "u".repeat(61)), ["u".repeat(61)], "\\u{61} without u is u{61}: 61 u's");
eq(m(/\u12/, "u12"), ["u12"], "invalid \\u is u");
eq(m(/\x1/, "x1"), ["x1"], "invalid \\x is x");
eq(m(/(?=a)*/, ""), [""], "quantified lookahead");
eq(m(/(?!a)+b/, "b"), ["b"], "quantified negative lookahead");
eq(m(/a|/, "b"), [""], "empty alternative");
eq(m(/(?:)/, "ab"), [""], "empty group");

// ---- syntax errors
["(", ")", "[", "a**", "a{2,1}", "*a", "+", "?", "(?", "(?<", "(?<a", "(?<a>x)(?<a>y)", "(?<1a>x)", "(?:", "(?<=a)*", "(?<!a)+",
 "\\", "a{1,2}{3}", "{1}", "(?<a>.)\\k<b>", "(?<a>.)\\k", "\\k<a>(?<b>.)", "(?<a", "(?<a>", "(?=", "x{2,1}", "[b-a]", "(?z)", "a)", "(?<=", "(?i", "(?-:a)", "(?ii:a)", "(?i-i:a)", "(?<>x)", "(?<a\\u>x)"]
  .forEach(function (s) { syntaxError(s); });
["{", "(", "\\u{110000}", "\\p{}", "\\p{Foo}", "[\\d-a]", "\\-", "\\a", "\\k", "\\1", "(?=a)*", "a{", "}", "]", "\\00", "[\\1]", "\\c", "\\u12", "\\x1", "(?<a>.)\\k<b>", "\\u{}", "\\u{1", "\\ud83d\\u12", "[z-a]", "\\_"]
  .forEach(function (s) { syntaxError(s, "u"); });
["a", "gg", "gu".concat("v"), "x", "G"].forEach(function (f) { syntaxError("a", f); });
throws(function () { new RegExp("a", "uv"); }, SyntaxError, "u and v together");
valid("\\/"); valid("[/]"); valid("(?:)"); valid("^*".slice(0, 1)); valid("a|"); valid("(?<a>x)|(?<b>x)"); valid("\\u{1F600}", "u");
valid("[\\-]", "u"); valid("\\^\\$\\\\\\.\\*\\+\\?\\(\\)\\[\\]\\{\\}\\|\\/", "u"); valid("(?<=a)(?<!b)(?=c)(?!d)", "u"); valid("\\0", "u");
valid("(?<a>.)\\k<a>", "u"); valid("\\p{Lu}", "u"); valid("[\\p{L}\\d_]", "u"); valid("a{1,2}?", "u");
syntaxError("(?<=a)*", "u");
syntaxError("(?<a>x)(?<a>y)", "u");
syntaxError("(?<a>x)|(?<a>y)(?<a>z)", "");

// ---- deep and large patterns do not crash
throws(function () { new RegExp("(".repeat(100000) + ")".repeat(100000)); }, Error, "deeply nested groups end in an error, not a crash");
throws(function () { new RegExp("(?:".repeat(100000)); }, Error, "unterminated deep nesting");
eq(new RegExp("a".repeat(100000)).test("a".repeat(100000)), true, "a long literal pattern");
eq(new RegExp("(?:a|b)".repeat(2000)).test("ab".repeat(1000)), true, "many alternations");
eq(/^(?:a?){16}a{16}$/.test("a".repeat(16)), true, "the classic a?^n a^n pattern at a size that backtracking handles");

// ---- long inputs
var big = "ab".repeat(200000);
eq(big.match(/b/g).length, 200000, "global match on a long string");
eq(/^(?:ab)*$/.test(big), true, "a long loop");
eq(/^[ab]*$/.test(big), true, "a long class loop");
eq(/c/.test(big), false, "no match scans linearly");
eq(/(?:ab)*c/.test("ab".repeat(2000)), false, "a failing loop retried from every start (quadratic, but small)");
eq(big.replace(/a/g, "").length, 200000, "global replace on a long string");
eq(/(.*)b/.exec(big)[1].length, 399999, "greedy dot with backtracking over a long string");

print("regexp: " + total + " checks, " + fails + " failures");
