// Promise.try (ECMA-262 2025, 27.2.4.8).  Self-checking; the checks run one after the other and the summary comes last.
var total = 0, fails = 0;
function eq(actual, expected, label) {
  total++;
  if (actual !== expected) { fails++; print("FAIL " + label + ": got " + String(actual) + ", want " + String(expected)); }
}
var tests = [];
function test(label, f) { tests.push([label, f]); }

test("shape", function () {
  eq(typeof Promise.try, "function", "Promise.try is a function");
  eq(Promise.try.length + "/" + Promise.try.name, "1/try", "length and name");
  var d = Object.getOwnPropertyDescriptor(Promise, "try");
  eq([d.writable, d.enumerable, d.configurable].join(), "true,false,true", "attributes");
  eq(Promise.try(function () {}) instanceof Promise, true, "returns a promise");
});
test("runs at once, with the arguments, this undefined", function () {
  var seen = [];
  var p = Promise.try(function (a, b) { "use strict"; seen.push(a, b, this); return a + b; }, 1, 2);
  eq(seen.join() + "|" + seen.length, "1,2,|3", "the callback ran before try returned");
  return p.then(function (v) { eq(v, 3, "resolved with the result"); });
});
test("a throw rejects", function () {
  var e = new Error("x");
  return Promise.try(function () { throw e; }).then(function () { eq("fulfilled", "rejected", "throw"); }, function (r) { eq(r, e, "rejected with the thrown value"); });
});
test("a callback that is not callable rejects with a TypeError", function () {
  return Promise.try(5).then(function () { eq("fulfilled", "rejected", "non-callable"); }, function (r) { eq(r instanceof TypeError, true, "TypeError"); });
});
test("a returned promise is adopted", function () {
  var p = Promise.try(function () { return Promise.resolve(7); });
  return p.then(function (v) { eq(v, 7, "adopted"); });
});
test("a returned rejected promise stays rejected", function () {
  return Promise.try(function () { return Promise.reject("no"); }).then(function () { eq("fulfilled", "rejected", "rejected promise"); }, function (r) { eq(r, "no", "reason"); });
});
test("this must be an object, and is the constructor of the result", function () {
  var threw = false;
  try { Promise.try.call(undefined, function () {}); } catch (e) { threw = e instanceof TypeError; }
  eq(threw, true, "undefined this is a TypeError");
  class MyPromise extends Promise {}
  var p = MyPromise.try(function () { return 1; });
  eq(p instanceof MyPromise, true, "a subclass is used");
  return p.then(function (v) { eq(v, 1, "subclass result"); });
});

var chain = Promise.resolve();
tests.forEach(function (t) {
  chain = chain.then(function () { return t[1](); }).catch(function (e) { fails++; total++; print("FAIL " + t[0] + ": " + e); });
});
chain.then(function () { print("promisetry: " + total + " checks, " + fails + " failures"); });
