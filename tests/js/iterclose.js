// Closing iterators (ECMA-262 7.4.12 IteratorClose, 7.4.13 AsyncIteratorClose) when a for-of / for-await-of loop is left by break, continue,
// return or an exception.  Self-checking; the checks run one after the other and the summary comes last.
//
// What is checked: return() is called exactly once; with a normal exit its errors and a non-object result are thrown, with an exception in
// flight they are ignored and the original exception wins; for await awaits the result of return() (and only when there is a return method:
// the number of microtask ticks is observable); the code that closes an iterator is not protected by the handlers of the loop itself or of a
// try statement inside it.
var total = 0, fails = 0;
function eq(actual, expected, label) {
  total++;
  if (actual !== expected) { fails++; print("FAIL " + label + ": got " + JSON.stringify(actual) + ", want " + JSON.stringify(expected)); }
}
var log = [];
function push(s) { log.push(s); }
function took() { var r = log.join(" "); log = []; return r; }
function errname(e) { return e instanceof Error ? e.constructor.name + ":" + e.message : String(e); }

// an iterable whose return() is `ret` (absent when undefined); it never finishes by itself
function syncIter(ret) {
  return { [Symbol.iterator]() { return { next() { push("next"); return { value: 1, done: false }; }, return: ret }; } };
}
function asyncIter(ret) {
  return { [Symbol.asyncIterator]() { return { next() { push("next"); return Promise.resolve({ value: 1, done: false }); }, return: ret }; } };
}
// a promise that is settled a few ticks later, noting it in the log
function later(v) {
  return new Promise(function (resolve) {
    Promise.resolve().then(function () { return Promise.resolve(); }).then(function () { push("settled"); resolve(v); });
  });
}
function rets(v) { return function () { push("return"); return v; }; }
function retsLater(v) { return function () { push("return"); return later(v); }; }
function throws(e) { return function () { push("return"); throw e; }; }
function rejects(e) { return function () { push("return"); return Promise.reject(e); }; }

var tests = [];
function test(label, f) { tests.push([label, f]); }
// runs f (sync or async), notes an exception in the log, and compares the log with `want`
function check(label, f, want) {
  test(label, async function () {
    log = [];
    try { await f(); } catch (e) { push("caught " + errname(e)); }
    for (var i = 0; i < 6; i++) await null;      // (the tick counters of the tick tests finish)
    eq(took(), want, label);
  });
}

// ---- for-of: return() is called once, whatever it does
check("sync: break", function () { for (var x of syncIter(rets({}))) break; push("after"); }, "next return after");
check("sync: break, return() throws", function () { for (var x of syncIter(throws(new Error("r")))) break; push("after"); }, "next return caught Error:r");
check("sync: break, return() gives a non-object", function () { for (var x of syncIter(rets(1))) break; push("after"); }, "next return caught TypeError:Iterator result is not an object");
check("sync: break, no return method", function () { for (var x of syncIter(undefined)) break; push("after"); }, "next after");
check("sync: exception in the body, return() throws", function () { for (var x of syncIter(throws(new Error("r")))) throw new Error("body"); }, "next return caught Error:body");
check("sync: exception in the body, return() gives a non-object", function () { for (var x of syncIter(rets(1))) throw new Error("body"); }, "next return caught Error:body");
check("sync: return statement", function () { (function () { for (var x of syncIter(rets({}))) return 1; })(); push("after"); }, "next return after");
check("sync: continue outer", function () {
  outer: for (var i = 0; i < 2; i++) { for (var x of syncIter(rets({}))) continue outer; }
  push("after");
}, "next return next return after");
check("sync: break outer closes the inner iterator first", function () {
  outer: for (var y of syncIter(function () { push("outer-return"); return {}; })) { for (var x of syncIter(function () { push("inner-return"); return {}; })) break outer; }
}, "next next inner-return outer-return");
check("sync: the inner return() throws: the outer loop closes with that exception", function () {
  outer: for (var y of syncIter(function () { push("outer-return"); return {}; })) { for (var x of syncIter(throws(new Error("inner")))) break outer; }
}, "next next return outer-return caught Error:inner");
check("sync: a try/catch inside the body does not see the error of closing", function () {
  try { for (var x of syncIter(throws(new Error("r")))) { try { break; } catch (e) { push("inner catch"); } } } catch (e) { push("outer catch"); }
}, "next return outer catch");
check("sync: a try/finally inside the body", function () {
  for (var x of syncIter(rets({}))) { try { break; } finally { push("finally"); } }
  push("after");
}, "next finally return after");
check("sync: generator.return() while suspended in the loop", function () {
  function* g() { for (var x of syncIter(rets({}))) yield x; }
  var it = g();
  it.next();
  var r = it.return("v");
  push(r.value + "," + r.done);
}, "next return v,true");
check("sync: generator.return(), return() of the inner iterator throws", function () {
  function* g() { for (var x of syncIter(throws(new Error("r")))) yield x; }
  var it = g();
  it.next();
  it.return("v");
}, "next return caught Error:r");

// ---- for await: return() is awaited
check("async: break awaits return()", async function () { for await (var x of asyncIter(retsLater({}))) break; push("after"); }, "next return settled after");
check("async: break, return() gives a non-object", async function () { for await (var x of asyncIter(retsLater(1))) break; push("after"); },
      "next return settled caught TypeError:Iterator result is not an object");
check("async: break, return() rejects", async function () { for await (var x of asyncIter(rejects(new Error("rej")))) break; push("after"); }, "next return caught Error:rej");
check("async: break, return() throws", async function () { for await (var x of asyncIter(throws(new Error("thrown")))) break; push("after"); }, "next return caught Error:thrown");
check("async: break, no return method", async function () { for await (var x of asyncIter(undefined)) break; push("after"); }, "next after");
check("async: exception in the body, return() is awaited first", async function () { for await (var x of asyncIter(retsLater({}))) throw new Error("body"); },
      "next return settled caught Error:body");
check("async: exception in the body, return() rejects", async function () { for await (var x of asyncIter(rejects(new Error("rej")))) throw new Error("body"); },
      "next return caught Error:body");
check("async: exception in the body, return() throws", async function () { for await (var x of asyncIter(throws(new Error("thrown")))) throw new Error("body"); },
      "next return caught Error:body");
check("async: exception in the body, return() gives a non-object", async function () { for await (var x of asyncIter(retsLater(5))) throw new Error("body"); },
      "next return settled caught Error:body");
check("async: exception in the body, no return method", async function () { for await (var x of asyncIter(undefined)) throw new Error("body"); }, "next caught Error:body");
check("async: return statement", async function () {
  var r = await (async function () { for await (var x of asyncIter(retsLater({}))) return "value"; })();
  push(r);
}, "next return settled value");
check("async: continue outer", async function () {
  outer: for (var i = 0; i < 2; i++) { for await (var x of asyncIter(retsLater({}))) continue outer; }
  push("after");
}, "next return settled next return settled after");
check("async: break outer, inner closed first", async function () {
  outer: for await (var y of asyncIter(function () { push("outer-return"); return later({}); })) {
    for await (var x of asyncIter(function () { push("inner-return"); return later({}); })) break outer;
  }
  push("after");
}, "next next inner-return settled outer-return settled after");
check("async: the inner return() throws: the outer loop closes with that exception", async function () {
  outer: for await (var y of asyncIter(function () { push("outer-return"); return later({}); })) {
    for await (var x of asyncIter(throws(new Error("inner")))) break outer;
  }
}, "next next return outer-return settled caught Error:inner");
check("async: a try/catch inside the body does not see the error of closing", async function () {
  try { for await (var x of asyncIter(throws(new Error("r")))) { try { break; } catch (e) { push("inner catch"); } } } catch (e) { push("outer catch"); }
}, "next return outer catch");
check("async: a sync iterable is closed through the wrapper", async function () {
  for await (var x of syncIter(rets({}))) break;
  push("after");
}, "next return after");
check("async: async generator.return() while suspended in the loop", async function () {
  async function* g() { for await (var x of asyncIter(retsLater({}))) yield x; }
  var it = g();
  await it.next();
  var r = await it.return("v");
  push(r.value + "," + r.done);
}, "next return settled v,true");

// ---- the ticks of closing: no return method, nothing is awaited; a return method, one tick for a plain result
function ticks(f, want, label) {
  check(label, async function () {
    for await (var x of f) {
      Promise.resolve().then(function () { push("tick1"); }).then(function () { push("tick2"); }).then(function () { push("tick3"); });
      break;
    }
    push("after");
  }, want);
}
ticks(asyncIter(undefined), "next after tick1 tick2 tick3", "ticks: no return method");
ticks(asyncIter(rets({})), "next return tick1 after tick2 tick3", "ticks: return() gives an object");
ticks(asyncIter(rets(Promise.resolve({}))), "next return tick1 after tick2 tick3", "ticks: return() gives a resolved promise");

// run them in order
var chain = Promise.resolve();
tests.forEach(function (t) {
  chain = chain.then(function () { return t[1](); }).catch(function (e) { fails++; total++; print("FAIL " + t[0] + ": " + e); });
});
chain.then(function () { print("iterclose: " + total + " checks, " + fails + " failures"); });
