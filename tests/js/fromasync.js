// Array.fromAsync (ECMA-262 23.1.2.2).  Self-checking; the checks run one after the other and the summary comes last.
var total = 0, fails = 0;
function repr(v) {
  if (typeof v === "string") return JSON.stringify(v);
  if (typeof v === "symbol") return v.toString();
  if (Array.isArray(v)) return "[" + v.map(repr).join(",") + "]";
  return String(v);
}
function eq(actual, expected, label) {
  total++;
  var a = repr(actual), e = repr(expected);
  if (a !== e) { fails++; print("FAIL " + label + ": got " + a + ", want " + e); }
}
// resolves to the rejection reason (or "no rejection")
function rejection(p) { return p.then(function () { return "no rejection"; }, function (e) { return e; }); }

var tests = [];
function test(label, f) { tests.push([label, f]); }

test("an array of plain values", function () {
  return Array.fromAsync([1, 2, 3]).then(function (a) { eq(a, [1, 2, 3], "plain array"); eq(Array.isArray(a), true, "result is an array"); });
});
test("promises in a sync iterable are awaited", function () {
  return Array.fromAsync([Promise.resolve(1), 2, Promise.resolve().then(function () { return 3; })]).then(function (a) { eq(a, [1, 2, 3], "promises awaited"); });
});
test("a mapper (its results are awaited)", function () {
  return Array.fromAsync([1, 2, 3], function (x, i) { return Promise.resolve(x * 10 + i); }).then(function (a) { eq(a, [10, 21, 32], "async mapper"); });
});
test("thisArg reaches the mapper", function () {
  var seen = [];
  return Array.fromAsync([1, 2], function (x) { seen.push(this.tag); return x; }, { tag: "T" }).then(function () { eq(seen, ["T", "T"], "thisArg"); });
});
test("an async iterable", function () {
  async function* g() { yield 1; yield Promise.resolve(2); yield 3; }
  return Array.fromAsync(g()).then(function (a) { eq(a, [1, 2, 3], "async generator"); });
});
test("an array-like", function () {
  return Array.fromAsync({ length: 3, 0: "a", 1: Promise.resolve("b"), 2: "c" }).then(function (a) { eq(a, ["a", "b", "c"], "array-like"); });
});
test("an array-like with a mapper", function () {
  return Array.fromAsync({ length: 2, 0: 1, 1: 2 }, async function (x) { return x + 1; }).then(function (a) { eq(a, [2, 3], "array-like mapper"); });
});
test("strings are iterated by code point", function () {
  return Array.fromAsync("a😀b").then(function (a) { eq(a, ["a", "😀", "b"], "string"); });
});
test("a number is an empty array-like", function () {
  return Array.fromAsync(5).then(function (a) { eq(a, [], "number"); });
});
test("null and undefined reject", function () {
  return rejection(Array.fromAsync(null)).then(function (e) {
    eq(e instanceof TypeError, true, "null rejects with a TypeError");
    return rejection(Array.fromAsync());
  }).then(function (e) { eq(e instanceof TypeError, true, "undefined rejects with a TypeError"); });
});
test("a mapper that is not a function rejects, it does not throw", function () {
  var p;
  try { p = Array.fromAsync([1], 5); } catch (e) { eq("threw", "rejected", "no synchronous throw"); return; }
  eq(p instanceof Promise, true, "returns a promise");
  return rejection(p).then(function (e) { eq(e instanceof TypeError, true, "TypeError"); });
});
test("a rejected element rejects the result", function () {
  return rejection(Array.fromAsync([1, Promise.reject(new RangeError("r")), 3])).then(function (e) { eq(e instanceof RangeError, true, "RangeError"); });
});
test("a mapper that throws closes the async iterator", function () {
  var log = [];
  var it = {
    [Symbol.asyncIterator]() {
      var i = 0;
      return {
        next() { log.push("next"); return Promise.resolve({ value: i++, done: false }); },
        return() { log.push("return"); return Promise.resolve({}); },
      };
    },
  };
  return rejection(Array.fromAsync(it, function (x) { if (x == 1) throw new EvalError("m"); return x; })).then(function (e) {
    eq(e instanceof EvalError, true, "the mapper's error");
    eq(log, ["next", "next", "return"], "the iterator was closed");
  });
});
test("the error wins over a rejecting return()", function () {
  var it = {
    [Symbol.asyncIterator]() {
      return { next() { return Promise.resolve({ value: 1, done: false }); }, return() { return Promise.reject(new URIError("from return")); } };
    },
  };
  return rejection(Array.fromAsync(it, function () { throw new EvalError("m"); })).then(function (e) { eq(e instanceof EvalError, true, "EvalError"); });
});
test("a rejecting next() rejects without closing", function () {
  var closed = false;
  var it = { [Symbol.asyncIterator]() { return { next() { return Promise.reject(new SyntaxError("n")); }, return() { closed = true; return {}; } }; } };
  return rejection(Array.fromAsync(it)).then(function (e) { eq([e instanceof SyntaxError, closed], [true, false], "no close after next() failed"); });
});
test("this as a constructor", function () {
  class C extends Array {}
  return Array.fromAsync.call(C, [1, 2]).then(function (a) { eq([a instanceof C, a.length, a[1]], [true, 2, 2], "subclass, iterable"); return Array.fromAsync.call(C, { length: 2, 0: 5, 1: 6 }); })
    .then(function (a) { eq([a instanceof C, a.length, a[0]], [true, 2, 5], "subclass, array-like"); });
});
test("this that is not a constructor makes an array", function () {
  return Array.fromAsync.call({}, [1]).then(function (a) { eq(Array.isArray(a), true, "plain array"); });
});
test("a constructor that is not an array gets length set", function () {
  function Bag() {}
  return Array.fromAsync.call(Bag, [7, 8]).then(function (b) { eq([b instanceof Bag, b.length, b[0], b[1]], [true, 2, 7, 8], "Bag"); });
});
test("the order of the awaits (ticks)", function () {
  var log = [];
  var p = Array.fromAsync([1, 2]).then(function () { log.push("fromAsync"); });
  var q = (async function () { await 0; await 0; await 0; log.push("3 awaits"); })();
  Promise.resolve().then(function () { log.push("tick 1"); }).then(function () { log.push("tick 2"); });
  return Promise.all([p, q]).then(function () { eq(log.indexOf("tick 1") < log.indexOf("fromAsync"), true, "fromAsync takes more than one tick"); });
});
test("Array.fromAsync itself", function () {
  eq([typeof Array.fromAsync, Array.fromAsync.length, Array.fromAsync.name], ["function", 1, "fromAsync"], "length and name");
  eq(Object.getOwnPropertyDescriptor(Array, "fromAsync").enumerable, false, "not enumerable");
  throws_(function () { new Array.fromAsync([]); }, "not a constructor");
});
function throws_(f, label) { total++; try { f(); fails++; print("FAIL " + label + ": no error"); } catch (e) { if (!(e instanceof TypeError)) { fails++; print("FAIL " + label + ": " + e); } } }

// run them in order
var chain = Promise.resolve();
tests.forEach(function (t) {
  chain = chain.then(function () { return t[1](); }).catch(function (e) { fails++; total++; print("FAIL " + t[0] + ": " + e); });
});
chain.then(function () { print("fromasync: " + total + " checks, " + fails + " failures"); });
