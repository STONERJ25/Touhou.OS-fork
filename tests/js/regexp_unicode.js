// RegExp: Unicode mode, case-insensitive matching, property escapes, the v flag and modifiers
// (ECMA-262 22.2.2.7 Canonicalize, 22.2.1 UnicodeSets, the u and v flags).
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

// ---- code points versus code units
eq(/^.$/u.test("\u{1F600}"), true, "u: dot matches an astral character");
eq(/^.$/.test("\u{1F600}"), false, "no u: dot matches one code unit");
eq(/^..$/.test("\u{1F600}"), true, "no u: two units");
eq(/\ud83d/u.test("\u{1F600}"), false, "u: a lone lead surrogate does not match half a pair");
eq(/\ud83d/.test("\u{1F600}"), true, "no u: a lone lead surrogate matches the unit");
eq(/\ude00/u.test("\u{1F600}"), false, "u: a lone trail surrogate does not match half a pair");
eq(/\ud83d/u.test("\ud83dx"), true, "u: a lone surrogate matches itself");
eq(/^[\u{1F600}]$/u.test("\u{1F600}"), true, "u: class with an astral member");
eq(/^[\u{1F600}]$/.test("\u{1F600}"), false, "no u: the class is two code units");
eq(/^[^x]$/u.test("\u{1F600}"), true, "u: negated class matches a pair");
eq(/^[^x]$/.test("\u{1F600}"), false, "no u: negated class matches a unit");
eq(/\u{1F600}/u.test("\u{1F600}"), true, "\\u{} escape");
eq(/\ud83d\ude00/u.test("\u{1F600}"), true, "u: surrogate escapes form one code point");
eq(/^[\ud83d\ude00]$/u.test("\u{1F600}"), true, "u: surrogate escapes in a class form one code point");
eq(/[\u{1F600}-\u{1F64F}]/u.test("\u{1F603}"), true, "astral range");
eq(/[\u{1F600}-\u{1F64F}]/u.test("\u{1F650}"), false, "astral range end");
eq(/\u{1F600}+/u.exec("\u{1F600}\u{1F600}")[0].length, 4, "quantifier on an astral character");
eq(/\ud83d\ude00+/.exec("\u{1F600}\ude00\ude00")[0].length, 4, "no u: the quantifier applies to the trail surrogate only");
eq("\u{1F600}".match(/./g).length, 2, "no u: two matches");
eq("\u{1F600}".match(/./gu).length, 1, "u: one match");
eq("\u{1F600}x".replace(/(?:)/gu, "-"), "-\u{1F600}-x-", "u: empty matches advance by code point");
eq("\u{1F600}x".replace(/(?:)/g, "-").length, "\u{1F600}x".length + 4, "no u: empty matches between the units");
var r = /./gu; r.exec("\u{1F600}a");
eq(r.lastIndex, 2, "u: lastIndex after an astral match");
r = /./yu; r.lastIndex = 1;
eq(m(r, "\u{1F600}a"), ["\u{1F600}"], "u: a start inside a pair backs up to the pair");
eq(/(?<=\u{1F600})a/u.test("\u{1F600}a"), true, "u: lookbehind over an astral character");
eq(/(?<=^.)a/u.test("\u{1F600}a"), true, "u: lookbehind dot reads a whole code point");
eq(/(?<=^.)a/.test("\u{1F600}a"), false, "no u: lookbehind dot reads one unit");
eq(/\u{10000}/u.source, "\\u{10000}", "source keeps the escape");
eq(/^\S$/u.test("\u{1F600}"), true, "u: \\S matches a pair");
eq(m(/.*?(?=\ude00)/, "\u{1F600}"), ["\ud83d"], "no u: lookahead sees the trail unit");
eq(m(/\u{61}/u, "a"), ["a"], "\\u{61} with u");
eq(/[\u{1F600}-\u{1F64F}]+/u.exec("a\u{1F600}\u{1F601}b")[0], "\u{1F600}\u{1F601}", "greedy class loop over pairs");
eq(/^(?:[\u{1F600}-\u{1F64F}])+$/u.test("\u{1F600}\u{1F601}\u{1F602}"), true, "backtracking over code points");
eq(/^.+\u{1F602}$/u.test("ab\u{1F600}\u{1F602}"), true, "greedy dot backs up by code points");
eq(/^.+.$/u.test("\u{1F600}\u{1F601}"), true, "greedy dot gives one pair back");
eq(/^[^a]+$/u.exec("\u{1F600}\u{1F601}")[0].length, 4, "negated class loop over pairs");

// ---- case-insensitive matching
eq(/abc/i.test("ABC"), true, "ASCII");
eq(/[a-c]+/i.exec("xxABCabc")[0], "ABCabc", "class range");
eq(/[^a]/i.test("A"), false, "negated class folds");
eq(/[^a]/i.test("b"), true, "negated class, other letter");
eq(/\u00e4/i.test("\u00c4"), true, "Latin-1 letters");
eq(/\u03c3/i.test("\u03c2"), true, "Greek sigma and final sigma");
eq(/\u00df/i.test("SS"), false, "sharp s has no simple case folding");
eq(/\u00b5/i.test("\u039c"), true, "micro sign and capital mu: uppercase equal");
eq(/\u212a/i.test("k"), false, "no u: Kelvin sign is not k");
eq(/\u212a/iu.test("k"), true, "u: Kelvin sign folds to k");
eq(/k/iu.test("\u212a"), true, "u: k matches Kelvin sign");
eq(/s/i.test("\u017f"), false, "no u: long s is not s");
eq(/s/iu.test("\u017f"), true, "u: long s folds to s");
eq(/\u017f/i.test("S"), false, "no u: long s does not match S");
eq(/[a-z]/i.test("\u017f"), false, "no u: class does not reach long s");
eq(/[a-z]/iu.test("\u017f"), true, "u: class reaches long s");
eq(/[a-z]/iu.test("\u212a"), true, "u: class reaches the Kelvin sign");
eq(/\w/iu.test("\u017f"), true, "/iu: \\w includes long s");
eq(/\W/iu.test("\u017f"), false, "/iu: \\W excludes long s");
eq(/\w/i.test("\u017f"), false, "/i: \\w excludes long s");
eq(/\W/i.test("\u017f"), true, "/i: \\W includes long s");
eq(/\bs/iu.test("\u017f"), true, "/iu: \\b sees long s as a word character");
eq(/\u1e9e/iu.test("\u00df"), true, "u: capital sharp s folds to sharp s");
eq(/\u1e9e/i.test("\u00df"), false, "no u: capital sharp s");
eq(/\u0130/i.test("i"), false, "dotted capital I is not i");
eq(/\u0131/i.test("I"), false, "no u: dotless i does not match I");
eq(/\u0131/iu.test("I"), false, "u: dotless i does not match I");
eq(/\u{10400}/iu.test("\u{10428}"), true, "u: Deseret letters fold");
eq(/[\u{10400}-\u{10410}]/iu.test("\u{10428}"), true, "u: Deseret in a class");
eq(/(a)\1/i.test("aA"), true, "backreference compares case-insensitively");
eq(/(\u212a)\1/iu.test("\u212ak"), true, "u: backreference folds");
eq(/(\u212a)\1/i.test("\u212ak"), false, "no u: backreference does not fold Kelvin");
eq(/(?:(a)|b)(?<=\1)/i.test("A"), true, "folding inside lookbehind");
eq(/\p{Lu}/iu.test("a"), true, "u: \\p{Lu} with i matches lowercase");
eq(/\P{Lu}/iu.test("A"), true, "u: \\P{Lu} with i matches uppercase (complement before folding)");
eq(/[^\P{Lu}]/iu.test("a"), false, "u: negated \\P{Lu} class");
eq(/\p{Ll}/u.test("A"), false, "u: \\p{Ll} without i");

// ---- property escapes
eq(/\p{Lu}/u.test("A"), true, "\\p{Lu}");
eq(/\p{Lu}/u.test("a"), false, "\\p{Lu} negative");
eq(/\p{Letter}/u.test("\u00e9"), true, "long general category name");
eq(/\p{gc=Ll}/u.test("a"), true, "gc=");
eq(/\p{General_Category=Decimal_Number}/u.test("7"), true, "General_Category=");
eq(/\p{Nd}/u.test("\u0663"), true, "Arabic-Indic digit");
eq(/\p{Script=Greek}/u.test("\u03b1"), true, "Script=Greek");
eq(/\p{sc=Latn}/u.test("a"), true, "sc=Latn");
eq(/\p{Script=Greek}/u.test("a"), false, "Script=Greek negative");
eq(/\p{scx=Grek}/u.test("\u03b1"), true, "Script_Extensions");
eq(/\p{Script_Extensions=Han}/u.test("\u4e2d"), true, "Han");
eq(/\p{Emoji}/u.test("\u{1F600}"), true, "Emoji");
eq(/\p{ASCII}/u.test("a"), true, "ASCII");
eq(/\p{ASCII}/u.test("\u00e9"), false, "ASCII negative");
eq(/\p{Any}/u.test("\u{10FFFF}"), true, "Any");
eq(/\p{Alphabetic}/u.test("x"), true, "Alphabetic");
eq(/\p{White_Space}/u.test("\u2003"), true, "White_Space");
eq(/\p{ID_Start}/u.test("a"), true, "ID_Start");
eq(/\p{Uppercase}/u.test("A"), true, "Uppercase");
eq(/\p{Lowercase}/u.test("A"), false, "Lowercase");
eq(/\P{L}/u.test("1"), true, "\\P{L}");
eq(/\P{L}/u.test("a"), false, "\\P{L} negative");
eq(/[\p{L}\d]+/u.exec("--ab12--")[0], "ab12", "properties in a class");
eq(/[^\p{L}]/u.exec("ab1")[0], "1", "negated property class");
eq(/\p{Lu}\p{Ll}+/u.exec("xx Hello")[0], "Hello", "properties in sequence");
eq(/\p{Cn}/u.test("\u0378"), true, "unassigned code point");
eq(/\p{Co}/u.test("\ue000"), true, "private use");
eq(/\p{Cs}/u.test("\ud800"), true, "a lone surrogate");
["\\p{Foo}", "\\p{L", "\\p", "\\p{=L}", "\\p{gc=}", "\\p{Script}", "\\p{Lu=x}", "\\P{Foo}", "[\\p{Foo}]", "\\p{lu}", "\\p{ Lu}"].forEach(function (s) { syntaxError(s, "u"); });
eq(/\p{L}/.test("p{L}"), true, "no u: \\p is a literal p");

// ---- the v flag
eq(/[\p{L}--[a-z]]/v.test("a"), false, "v: subtraction removes");
eq(/[\p{L}--[a-z]]/v.test("A"), true, "v: subtraction keeps the rest");
eq(/[[a-z]&&[aeiou]]/v.test("e"), true, "v: intersection");
eq(/[[a-z]&&[aeiou]]/v.test("b"), false, "v: intersection excludes");
eq(/[\p{L}&&\p{ASCII}]/v.test("\u00e9"), false, "v: intersection of properties");
eq(/[\p{L}&&\p{ASCII}]/v.test("e"), true, "v: intersection of properties, member");
eq(/[[a-z]&&[^aeiou]]/v.exec("aeiobx")[0], "b", "v: intersection with a negated nested class");
eq(/[\w--\d]+/v.exec("12ab34")[0], "ab", "v: subtraction of a class escape");
eq(/[[a-c][x-z]]+/v.exec("--abxz--")[0], "abxz", "v: nested union");
eq(/[\q{abc|d}]/v.exec("xabcx")[0], "abc", "v: string alternative");
eq(/^[\q{abc|ab|a}]+$/v.test("abcaba"), true, "v: strings, longest first");
eq(m(/[\q{ab|a}]/v, "ab"), ["ab"], "v: the longer string wins");
eq(m(/[\q{a|ab}c]/v, "abc"), ["ab"], "v: longest string is tried first regardless of order");
eq(/^[\q{}]$/v.test(""), true, "v: the empty string is a member");
eq(/[\q{abc}--\q{abc}]/v.test("abc"), false, "v: string subtraction");
eq(/[[a-z]--\q{x}]/v.test("x"), false, "v: subtraction of a single-character string");
eq(/[\q{ab}&&\q{ab}]/v.test("ab"), true, "v: string intersection");
eq(/[^a-z]/v.test("1"), true, "v: negated class");
eq(/[^\d]/v.test("5"), false, "v: negated class escape");
eq(/[\p{Lu}--[A]]/vi.test("a"), false, "vi: subtraction applies after folding");
eq(/[\p{Lu}--[A]]/vi.test("b"), true, "vi: other letters stay");
eq(/[^\p{Lu}]/vi.test("A"), false, "vi: negation applies after folding");
eq(/\p{Lu}/vi.test("a"), true, "vi: property folds");
eq(/[a-z]/vi.test("K"), true, "vi: range folds");
eq(/[\w]/vi.test("\u212a"), true, "vi: \\w includes the Kelvin sign");
eq(/[\&\-\!]+/v.exec("a&-!b")[0], "&-!", "v: escaped reserved punctuators");
eq(/[a-z]/v.flags, "v", "v flag");
["[(]", "[)]", "[[]", "[{]", "[}]", "[/]", "[|]", "[a&&&b]", "[a--b&&c]", "[a&&b--c]", "[&&a]", "[a&&]", "[^\\q{ab}]", "[\\q{ab]", "[\\q{a}", "[a-z", "[!!]", "[##]", "[a--]", "[[a]", "[\\p{Foo}]", "[\\P{RGI_Emoji}]", "[z-a]", "[a-\\d]"]
  .forEach(function (s) { syntaxError(s, "v"); });
["[\\(]", "[\\)]", "[\\[\\]]", "[\\{\\}]", "[\\/]", "[\\|]", "[\\-]", "[^a]", "[a-z]", "[\\q{ab|c}]", "[[a-z]&&[b-d]]", "[a--b]", "[^\\q{a}]", "[\\q{ab}--a]"]
  .forEach(function (s) {
    total++;
    try { new RegExp(s, "v"); } catch (e) { fails++; print("FAIL valid /" + s + "/v: " + e); }
  });
syntaxError("a", "uv");

// ---- modifiers (ES2025)
eq(/(?i:a)b/.test("Ab"), true, "(?i:)");
eq(/(?i:a)b/.test("AB"), false, "(?i:) scope ends");
eq(/(?-i:a)b/i.test("aB"), true, "(?-i:)");
eq(/(?-i:a)b/i.test("AB"), false, "(?-i:) is case-sensitive inside");
eq(/(?s:.)/.test("\n"), true, "(?s:)");
eq(/(?-s:.)/s.test("\n"), false, "(?-s:)");
eq(/(?m:^a)/.test("x\na"), true, "(?m:)");
eq(/(?-m:^a)/m.test("x\na"), false, "(?-m:)");
eq(/(?im:^a)/.test("x\nA"), true, "(?im:)");
eq(/(?i-s:a.)/s.test("A\n"), false, "(?i-s:)");
eq(/(?i:[a-c])/.test("B"), true, "modifier applies to classes");
eq(/(?i:(a)\1)/.test("aA"), true, "modifier applies to backreferences inside the group");
eq(/(?i:(a))\1/.test("aA"), false, "a backreference outside the modifier group is case-sensitive");
eq(/(?i:\u212a)/u.test("k"), true, "modifier with u");
eq(/(?:(?i:a)b)c/.test("AbC"), false, "modifiers do not leak");
eq(/(?i:a|b)+/.exec("aBAb")[0], "aBAb", "modifier over an alternation");

print("regexp_unicode: " + total + " checks, " + fails + " failures");
