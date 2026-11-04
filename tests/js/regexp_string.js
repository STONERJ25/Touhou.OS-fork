// RegExp objects and the String methods that use them (ECMA-262 22.2.4 the constructor, 22.2.6 RegExp.prototype,
// 22.1.3 String.prototype.match/matchAll/replace/replaceAll/search/split, GetSubstitution).
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

// ---- String.prototype.replace and GetSubstitution
eq("abc".replace(/b/, "[$`|$&|$']"), "a[a|b|c]c", "$` $& $'");
eq("abc".replace(/(b)/, "$2$1$01$10"), "a$2bbb0c", "$n forms with one capture");
eq("abcdefghijkl".replace(/(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)/, "$11-$10-$1"), "k-j-al", "two-digit group references");
eq("x".replace(/x/, "$"), "$", "lone $");
eq("x".replace(/x/, "$$"), "$", "$$");
eq("x".replace(/x/, "a$"), "a$", "trailing $");
eq("x".replace(/x/, "$0"), "$0", "$0 is not a reference");
eq("x".replace(/x/, "$<a>"), "$<a>", "$<name> without named groups is literal");
eq("x".replace(/(?<a>x)/, "[$<a>|$<b>]"), "[x|]", "$<name> with named groups");
eq("x".replace(/(?<a>x)/, "$<a"), "$<a", "$<name without a closing >");
eq("abc".replace(/(?<x>b)/, function (m, p1, off, str, groups) { return groups.x + off + str; }), "ab1abcc", "function replace with groups");
eq("abc".replace(/b/, function () { return arguments.length; }), "a3c", "function replace arguments without captures");
eq("abc".replace(/(b)/, function () { return arguments.length; }), "a4c", "function replace arguments with a capture");
eq("abc".replace(/(?<n>b)/, function () { return arguments.length; }), "a5c", "function replace arguments with named groups");
eq("aaa".replace(/a/g, function (m, off) { return off; }), "012", "function replace offsets");
eq("abc".replace(/(?:)/g, "-"), "-a-b-c-", "empty matches");
eq("aab".replace(/a/y, "x"), "xab", "sticky replace");
eq("aaba".replace(/a/gy, "x"), "xxba", "global sticky replace stops at the first gap");
eq("aXbxc".replace(/x/gi, "-"), "a-b-c", "global ignore-case");
eq("abc".replace(/b/, undefined), "aundefinedc", "replace value is converted to a string");
eq("abc".replace(/(?<=a)b/, "X"), "aXc", "lookbehind replace");
eq("aaa".replace(/a*?/g, "-"), "-a-a-a-", "lazy empty matches");
eq("abc".replace(/$/, "!"), "abc!", "match at the end");
eq("abc".replace(/^/, "!"), "!abc", "match at the start");
eq("a.b".replace(/\./g, "$&$&"), "a..b", "$& twice");
eq("john smith".replace(/(\w+)\s(\w+)/, "$2, $1"), "smith, john", "swap");
eq("aaa".replaceAll(/a/g, "b"), "bbb", "replaceAll with a global regexp");
eq("a.b.c".replaceAll(".", "-"), "a-b-c", "replaceAll with a string");
throws(function () { "a".replaceAll(/a/, "b"); }, TypeError, "replaceAll with a non-global regexp");

// ---- split
eq("a,b;c".split(/[,;]/), ["a", "b", "c"], "split on a class");
eq("a1b2c".split(/(\d)/), ["a", "1", "b", "2", "c"], "captures are spliced in");
eq("a,b,c".split(/,/, 2), ["a", "b"], "limit");
eq("abc".split(/(?:)/), ["a", "b", "c"], "split on the empty pattern");
eq("".split(/a/), [""], "empty string, no match");
eq("".split(/(?:)/), [], "empty string, empty pattern");
eq("\u{1F600}".split(/(?:)/u), ["\u{1F600}"], "u: code points");
eq("\u{1F600}".split(/(?:)/), ["\ud83d", "\ude00"], "no u: code units");
eq("test".split(/(?:)/, 0), [], "limit 0");
eq("A<B>bold</B>and<CODE>coded</CODE>".split(/<(\/)?([^<>]+)>/),
   ["A", undefined, "B", "bold", "/", "B", "and", undefined, "CODE", "coded", "/", "CODE", ""], "the specification's example");
eq("abc".split(/b/y), ["a", "c"], "the y flag does not matter");
eq("a  b".split(/\s*/), ["a", "b"], "a pattern that matches empty");
eq("a-b".split(/(-)|(\+)/), ["a", "-", undefined, "b"], "unmatched captures are undefined");
eq("abc".split(/x/), ["abc"], "no match");
eq("ab".split(/a*?/), ["a", "b"], "lazy empty split");
eq("ab".split(/a*/), ["", "b"], "greedy split at the start");
eq("abab".split(/(?<=a)/), ["a", "ba", "b"], "lookbehind split");
eq("one  two".split(/ /), ["one", "", "two"], "adjacent separators");
eq("aXbxc".split(/x/i), ["a", "b", "c"], "ignore case");
eq("a,b".split(/,/g), ["a", "b"], "the g flag does not matter");

// ---- match, matchAll, search
eq("a1b2".match(/\d/g), ["1", "2"], "global match");
eq("ab".match(/\d/g), null, "global match without matches");
eq(m(/x/, "ab"), null, "no match");
var mm = "xabcx".match(/(?<l>b)(c)/);
eq([mm[0], mm[1], mm[2], mm.index, mm.input, mm.groups.l], ["bc", "b", "c", 2, "xabcx", "b"], "non-global match is exec");
eq("aaa".match(/a*?/g), ["", "", "", ""], "lazy empty matches advance");
eq("abc".search(/c/), 2, "search");
eq("abc".search(/x/), -1, "search without a match");
var sr = /a/g; sr.lastIndex = 5;
eq("ba".search(sr), 1, "search ignores lastIndex");
eq(sr.lastIndex, 5, "search restores lastIndex");
eq("a1b22".match(/\d+/g), ["1", "22"], "multi-digit matches");
eq([..."a1b22".matchAll(/\d+/g)].map(function (x) { return x[0] + "@" + x.index; }), ["1@1", "22@3"], "matchAll");
eq([..."aaa".matchAll(/a/g)].length, 3, "matchAll count");
throws(function () { "a".matchAll(/a/); }, TypeError, "matchAll needs a global regexp");
var ma = "abab".matchAll(/b/g);
eq(Object.prototype.toString.call(ma), "[object RegExp String Iterator]", "iterator tag");
eq(ma.next().value[0], "b", "iterator next");
eq(typeof ma[Symbol.iterator], "function", "the iterator is iterable");
eq(ma[Symbol.iterator]() === ma, true, "the iterator returns itself");
var mr = /a/g; mr.lastIndex = 1;
eq([..."aaa".matchAll(mr)].length, 2, "matchAll starts at lastIndex");
eq(mr.lastIndex, 1, "matchAll leaves the regexp alone");
eq([..."ab".matchAll(/(?:)/g)].length, 3, "matchAll with empty matches");
eq([..."\u{1F600}".matchAll(/(?:)/gu)].length, 2, "matchAll empty matches in u mode");
eq([..."ab".matchAll("b")].length, 1, "matchAll with a string");
throws(function () { RegExp.prototype[Symbol.matchAll].call(1, "a"); }, TypeError, "matchAll on a non-object");

// ---- the Symbol methods and IsRegExp
eq("abc".match({ [Symbol.match](s) { return "M" + s; } }), "Mabc", "custom Symbol.match");
eq("x".replace({ [Symbol.replace](s, r) { return "R:" + s + r; } }, "y"), "R:xy", "custom Symbol.replace");
eq("x".search({ [Symbol.search](s) { return 7; } }), 7, "custom Symbol.search");
eq("abc".split({ [Symbol.split](s, lim) { return [s, lim]; } }, 5), ["abc", 5], "custom Symbol.split");
var notre = /a/; notre[Symbol.match] = false;
eq("/a/".startsWith(notre), true, "Symbol.match false makes startsWith accept it");
throws(function () { "/a/".startsWith(/a/); }, TypeError, "startsWith rejects a regexp");
throws(function () { "/a/".includes(/a/); }, TypeError, "includes rejects a regexp");
eq(RegExp.prototype[Symbol.replace].call({ exec: function () { return null; }, flags: "", lastIndex: 0 }, "abc", "x"), "abc", "generic Symbol.replace");
eq(RegExp.prototype.test.call({ exec: function () { return {}; } }, "a"), true, "test uses exec");
throws(function () { RegExp.prototype.test.call({ exec: function () { return 1; } }, "a"); }, TypeError, "exec returning a non-object");
throws(function () { RegExp.prototype.exec.call({}, "a"); }, TypeError, "exec on a non-regexp");
throws(function () { RegExp.prototype.test.call(1, "a"); }, TypeError, "test on a primitive");
throws(function () { RegExp.prototype[Symbol.split].call(null, "a"); }, TypeError, "split on null");
eq(RegExp.prototype[Symbol.search].call({ lastIndex: 3, exec: function () { return { index: 9 }; } }, "x"), 9, "generic Symbol.search");

// ---- subclasses and species
class MyRE extends RegExp {
  exec(s) { this.calls = (this.calls || 0) + 1; return super.exec(s); }
}
var sub = new MyRE("a", "g");
eq("aaa".replace(sub, "b"), "bbb", "subclass replace");
eq(sub.calls, 4, "subclass exec is called once per match plus the failing one");
eq(sub instanceof MyRE && sub instanceof RegExp, true, "subclass instance");
eq(Object.getPrototypeOf(new MyRE("a")) === MyRE.prototype, true, "subclass prototype");
var spl = new MyRE("-");
eq("a-b".split(spl), ["a", "b"], "subclass split");
class NoSpecies extends RegExp { static get [Symbol.species]() { return RegExp; } }
eq("a-b".split(new NoSpecies("-")), ["a", "b"], "species override");
eq(RegExp[Symbol.species] === RegExp, true, "RegExp[Symbol.species]");
eq(Object.getOwnPropertyDescriptor(RegExp, Symbol.species).get.call(5), 5, "species getter returns this");
class Counter extends RegExp {
  constructor(a, b) { super(a, b); Counter.made = (Counter.made || 0) + 1; }
}
"a-b-c".split(new Counter("-"));
eq(Counter.made, 2, "split constructs a splitter through the species constructor");
Counter.made = 0;
eq([..."ab".matchAll(new Counter(".", "g"))].length, 2, "matchAll through a subclass");
eq(Counter.made, 2, "matchAll constructs its matcher through the species constructor");

// ---- the constructor
var base = /a/g;
eq(RegExp(base) === base, true, "RegExp(re) returns re");
eq(RegExp(base, "i") === base, false, "RegExp(re, flags) makes a new one");
eq(new RegExp(base) === base, false, "new RegExp(re) copies");
eq(new RegExp(base).flags, "g", "copies the flags");
eq(new RegExp(base, "i").flags, "i", "flags override");
eq(new RegExp("a", "gi").flags, "gi", "flags string");
eq(new RegExp().source, "(?:)", "no arguments");
eq(new RegExp(undefined).source, "(?:)", "undefined pattern");
eq(new RegExp(null).source, "null", "null pattern");
eq(new RegExp(123).source, "123", "numeric pattern");
eq(new RegExp("a", undefined).flags, "", "undefined flags");
var like = { source: "x+", flags: "g", constructor: RegExp, [Symbol.match]: true };
eq(RegExp(like) === like, true, "a regexp-like object is returned by RegExp()");
eq(new RegExp(like).source, "x+", "a regexp-like object's source");
eq(new RegExp(like).flags, "g", "a regexp-like object's flags");
eq(new RegExp(like, "i").flags, "i", "flags override for a regexp-like object");
throws(function () { new RegExp("a", "gg"); }, SyntaxError, "repeated flag");
throws(function () { new RegExp("a", "x"); }, SyntaxError, "unknown flag");
throws(function () { new RegExp("["); }, SyntaxError, "bad pattern");
try { new RegExp("(", "u"); } catch (e) { eq(e.message, "Invalid regular expression: /(/u: Unterminated group", "error message"); }
try { new RegExp("a", "gx"); } catch (e) { eq(e.message, "Invalid flags supplied to RegExp constructor 'gx'", "flags error message"); }
eq(RegExp.length, 2, "RegExp.length");
eq(RegExp.name, "RegExp", "RegExp.name");
eq(RegExp.prototype.constructor === RegExp, true, "prototype.constructor");
eq(Object.getPrototypeOf(RegExp.prototype) === Object.prototype, true, "RegExp.prototype inherits from Object.prototype");
eq(Object.prototype.toString.call(RegExp.prototype), "[object Object]", "RegExp.prototype is not a regexp");
eq(Object.prototype.toString.call(/a/), "[object RegExp]", "toString tag");
eq(typeof /a/, "object", "typeof");
eq(/a/ instanceof RegExp, true, "instanceof");
eq(JSON.stringify(/a/), "{}", "JSON");
eq(String(/a/g), "/a/g", "String()");
eq(/a/ + "", "/a/", "concatenation");
function lit() { return /a/g; }
eq(lit() !== lit(), true, "a literal evaluates to a new object each time");
var l1 = lit(); l1.lastIndex = 3;
eq(lit().lastIndex, 0, "lastIndex is per object");

// ---- source, flags, toString, lastIndex, compile
eq(new RegExp("a/b").source, "a\\/b", "source escapes /");
eq(String(new RegExp("a/b")), "/a\\/b/", "toString of an escaped /");
eq(new RegExp("\n").source, "\\n", "source escapes a newline");
eq(new RegExp("\r").source, "\\r", "source escapes a carriage return");
eq(new RegExp(" ").source, "\\u2028", "source escapes U+2028");
eq(new RegExp("[/]").source, "[/]", "a / in a class is left alone");
eq(new RegExp("\\/").source, "\\/", "an escaped / is not escaped twice");
eq(new RegExp("\\\n").source, "\\n", "a backslash-newline pair becomes \\n");
eq(new RegExp("").source, "(?:)", "empty source");
eq(RegExp.prototype.source, "(?:)", "RegExp.prototype.source");
eq(RegExp.prototype.flags, "", "RegExp.prototype.flags");
eq(RegExp.prototype.global, undefined, "RegExp.prototype.global");
eq(RegExp.prototype.toString(), "/(?:)/", "RegExp.prototype.toString()");
eq(RegExp.prototype.toString.call({ source: "x", flags: "y" }), "/x/y", "generic toString");
eq(Object.getOwnPropertyDescriptor(RegExp.prototype, "flags").get.call({ global: true, hasIndices: 1, sticky: 1, unicodeSets: 0 }), "dgy", "generic flags getter");
throws(function () { Object.getOwnPropertyDescriptor(RegExp.prototype, "global").get.call({}); }, TypeError, "flag getter on a plain object");
throws(function () { Object.getOwnPropertyDescriptor(RegExp.prototype, "source").get.call(1); }, TypeError, "source getter on a primitive");
var d = Object.getOwnPropertyDescriptor(/a/, "lastIndex");
eq([d.value, d.writable, d.enumerable, d.configurable], [0, true, false, false], "lastIndex descriptor");
eq(Object.getOwnPropertyNames(/a/), ["lastIndex"], "own properties");
eq(Object.keys(/a/), [], "no enumerable own properties");
eq(Object.freeze(/a/).test("a"), true, "a frozen non-global regexp still tests");
throws(function () { Object.freeze(/a/g).test("a"); }, TypeError, "a frozen global regexp cannot update lastIndex");
var ro = /a/g; Object.defineProperty(ro, "lastIndex", { writable: false });
throws(function () { ro.test("b"); }, TypeError, "a read-only lastIndex cannot be reset after a failure");
var cr = /a/g; cr.lastIndex = 3; cr.compile("b", "i");
eq([cr.source, cr.flags, cr.lastIndex], ["b", "i", 0], "compile reinitialises");
eq(cr.compile(/c/m).source, "c", "compile from a regexp");
throws(function () { cr.compile(/c/, "g"); }, TypeError, "compile(regexp, flags)");
eq(/undefined/.exec()[0], "undefined", "exec() converts undefined");
eq(/null/.test(null), true, "test(null)");
eq(/1/.test(1), true, "test(1)");
eq(/a/.exec("a").input, "a", "input");

// ---- lastIndex and conversion order
var order = [];
var lo = /a/g;
lo.lastIndex = { valueOf: function () { order.push("valueOf"); return 0; } };
lo.exec("a");
eq(order, ["valueOf"], "lastIndex is converted once per exec");
var gl = /a/g; gl.lastIndex = 2.9;
eq(m(gl, "aaaa").length, 1, "fractional lastIndex is truncated"); eq(gl.lastIndex, 3, "after a truncated lastIndex");
var gy = /a/y; gy.lastIndex = "1";
eq(gy.test("ba"), true, "string lastIndex");

// ---- RegExp.escape (ES2025)
eq(RegExp.escape("a.b"), "\\x61\\.b", "escape: first alphanumeric and syntax characters");
eq(RegExp.escape("1+1"), "\\x31\\+1", "escape: leading digit");
eq(RegExp.escape(" -,"), "\\x20\\x2d\\x2c", "escape: space and punctuation");
eq(RegExp.escape("\n\t"), "\\n\\t", "escape: control escapes");
eq(RegExp.escape("/"), "\\/", "escape: slash");
eq(RegExp.escape("é"), "é", "escape: other characters are kept");
eq(RegExp.escape("\ud800"), "\\ud800", "escape: lone surrogates");
eq(RegExp.escape(" "), "\\u2028", "escape: line separator");
eq(RegExp.escape("_x"), "_x", "escape: underscore is not escaped");
eq(new RegExp(RegExp.escape("a.b*c")).test("a.b*c"), true, "escaped text matches itself");
eq(new RegExp(RegExp.escape("a.b*c")).test("axb*c"), false, "escaped text is literal");
throws(function () { RegExp.escape(1); }, TypeError, "escape of a non-string");

print("regexp_string: " + total + " checks, " + fails + " failures");
