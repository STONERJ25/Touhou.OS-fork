// Iterator and the iterator helpers (ECMA-262 27.1.3, 27.1.4): Iterator, Iterator.from, map, filter, take, drop, flatMap,
// reduce, toArray, forEach, some, every, find.  Self-checking: every check states the expected value.
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
function throws(f, ctor, label) {
  total++;
  try { f(); } catch (e) { if (e instanceof ctor) return; fails++; print("FAIL " + label + ": wrong error " + e); return; }
  fails++; print("FAIL " + label + ": no error");
}

// an iterator over the values in `items` that records what happens to it in `log`
function source(items, log) {
  var i = 0;
  return {
    __proto__: Iterator.prototype,
    next: function () { log && log.push("next"); return i < items.length ? { value: items[i++], done: false } : { value: undefined, done: true }; },
    return: function () { log && log.push("return"); return {}; },
  };
}
function range(n) { var a = []; for (var i = 0; i < n; i++) a.push(i); return a; }

// ---- the constructor and the prototype
eq([typeof Iterator, Iterator.length, Iterator.name], ["function", 0, "Iterator"], "Iterator");
throws(function () { Iterator(); }, TypeError, "Iterator() without new");
throws(function () { new Iterator(); }, TypeError, "new Iterator() is abstract");
class MyIter extends Iterator { next() { return { done: true }; } }
eq([new MyIter() instanceof Iterator, Object.getPrototypeOf(MyIter.prototype) === Iterator.prototype], [true, true], "Iterator can be subclassed");
eq(Object.getOwnPropertyDescriptor(Iterator, "prototype").writable, false, "Iterator.prototype is read-only");
eq(Iterator.prototype === Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]())), true, "%IteratorPrototype% is Iterator.prototype");
var d = Object.getOwnPropertyDescriptor(Iterator.prototype, "constructor");
eq([typeof d.get, typeof d.set, d.enumerable, d.configurable, d.get.call(null) === Iterator], ["function", "function", false, true, true], "Iterator.prototype.constructor is an accessor");
d = Object.getOwnPropertyDescriptor(Iterator.prototype, Symbol.toStringTag);
eq([typeof d.get, typeof d.set, d.get.call(null)], ["function", "function", "Iterator"], "Iterator.prototype[@@toStringTag] is an accessor");
eq(Object.prototype.toString.call(new MyIter()), "[object Iterator]", "toStringTag of an iterator");
var o = Object.create(Iterator.prototype);
o.constructor = 5;
eq([Object.getOwnPropertyDescriptor(o, "constructor").value, Object.getOwnPropertyDescriptor(o, "constructor").enumerable], [5, true], "assigning constructor through a subclass makes an own property");
throws(function () { Iterator.prototype.constructor = 1; }, TypeError, "assigning to Iterator.prototype.constructor itself");
throws(function () { Object.getOwnPropertyDescriptor(Iterator.prototype, Symbol.toStringTag).set.call(1, "x"); }, TypeError, "the setter needs an object");
eq(Object.getOwnPropertyNames(Iterator.prototype).sort().join(), "constructor,drop,every,filter,find,flatMap,forEach,map,reduce,some,take,toArray", "the methods of Iterator.prototype");
eq(["map", "filter", "take", "drop", "flatMap", "reduce", "forEach", "some", "every", "find", "toArray"].map(function (n) { return Iterator.prototype[n].length; }).join(), "1,1,1,1,1,1,1,1,1,1,0", "lengths");

// ---- map, filter, take, drop
eq(source(range(5)).map(function (x) { return x * x; }).toArray(), [0, 1, 4, 9, 16], "map");
eq(source(range(6)).filter(function (x) { return x % 2; }).toArray(), [1, 3, 5], "filter");
eq(source(range(10)).take(3).toArray(), [0, 1, 2], "take");
eq(source(range(5)).drop(2).toArray(), [2, 3, 4], "drop");
eq(source(range(3)).take(10).toArray(), [0, 1, 2], "take more than there is");
eq(source(range(3)).drop(10).toArray(), [], "drop more than there is");
eq(source(range(4)).take(Infinity).toArray(), [0, 1, 2, 3], "take(Infinity)");
eq(source(range(4)).drop(Infinity).toArray(), [], "drop(Infinity)");
eq(source(range(5)).map(function (x, i) { return [x, i]; }).toArray().join("|"), "0,0|1,1|2,2|3,3|4,4", "map gets the counter");
eq(source(range(5)).filter(function (x, i) { return i % 2 === 0; }).toArray(), [0, 2, 4], "filter gets the counter");
eq(source(range(10)).drop(2).take(3).map(function (x) { return x + 100; }).filter(function (x) { return x !== 103; }).toArray(), [102, 104], "a chain");
eq(source(range(3)).take(2.9).toArray(), [0, 1], "the limit is truncated");
eq(source(range(3)).take(-0).toArray(), [], "take(-0)");

// laziness and the order of calls
var log = [];
var it = source(range(3), log).map(function (x) { log.push("map " + x); return x; });
eq(log, [], "nothing runs before next()");
log.length = 0;
it = source(range(3), log).map(function (x) { log.push("map " + x); return x; });
var r = it.next();
eq([r.value, r.done, log.join()], [0, false, "next,map 0"], "one step per next()");
it.next(); it.next();
r = it.next();
eq([r.value, r.done, log.join()], [undefined, true, "next,map 0,next,map 1,next,map 2,next"], "the end");
log.length = 0;
r = it.next();
eq([r.done, log.length], [true, 0], "a finished helper does not touch the iterator again");
eq([Object.prototype.toString.call(it), Object.getPrototypeOf(it) === Object.getPrototypeOf(source([]).filter(function () {}))], ["[object Iterator Helper]", true], "iterator helper objects");
eq(typeof it[Symbol.iterator], "function", "a helper is iterable (Iterator.prototype[@@iterator])");
eq(it[Symbol.iterator]() === it, true, "a helper iterates itself");

// take closes the source at its limit; drop and the others do not
log = [];
source(range(5), log).take(2).toArray();
eq(log.join(), "next,next,return", "take(2) closes the source after two values");
log = [];
eq(source(range(2), log).take(5).toArray(), [0, 1], "take with a bigger limit");
eq(log.join(), "next,next,next", "...does not close an exhausted source");
log = [];
source(range(5), log).take(0).toArray();
eq(log.join(), "return", "take(0) closes without a next()");

// return(): before the first next, in the middle, after the end
log = [];
it = source(range(5), log).map(function (x) { return x; });
r = it.return();
eq([r.value, r.done, log.join()], [undefined, true, "return"], "return() before the first next() closes the source");
r = it.next();
eq([r.done, log.join()], [true, "return"], "...and the helper is finished");
log = [];
it = source(range(5), log).filter(function () { return true; });
it.next();
log.length = 0;
r = it.return();
eq([r.done, log.join()], [true, "return"], "return() in the middle closes the source");
r = it.return();
eq(log.join(), "return", "...once");

// errors close the source
log = [];
it = source(range(5), log).map(function (x) { if (x == 1) throw new RangeError("x"); return x; });
it.next();
throws(function () { it.next(); }, RangeError, "an exception from the mapper");
eq(log.join(), "next,next,return", "...closes the source");
eq(it.next().done, true, "...and finishes the helper");
log = [];
it = source(range(3), log).filter(function () { throw new TypeError("p"); });
throws(function () { it.next(); }, TypeError, "an exception from the predicate");
eq(log.join(), "next,return", "...closes the source");
// the error from the mapper wins over one from return()
var bad = source(range(3));
bad.return = function () { throw new EvalError("from return"); };
throws(function () { bad.map(function () { throw new RangeError("from mapper"); }).next(); }, RangeError, "the mapper's error wins over return()'s");
// next() throwing does not close
log = [];
var thrower = { __proto__: Iterator.prototype, next: function () { throw new URIError("n"); }, return: function () { log.push("return"); return {}; } };
throws(function () { thrower.map(function (x) { return x; }).next(); }, URIError, "next() throwing");
eq(log.length, 0, "...does not call return()");
// bad results from next()
throws(function () { ({ __proto__: Iterator.prototype, next: function () { return 1; } }).map(function () {}).next(); }, TypeError, "next() returning a non-object");
throws(function () { ({ __proto__: Iterator.prototype, next: 5 }).map(function () {}).next(); }, TypeError, "next not callable");

// argument validation closes the receiver
log = [];
var closable = source(range(3), log);
throws(function () { closable.map(); }, TypeError, "map without a function");
eq(log.join(), "return", "...closes the receiver");
log.length = 0;
throws(function () { closable.take(NaN); }, RangeError, "take(NaN)");
throws(function () { closable.take(-1); }, RangeError, "take(-1)");
throws(function () { closable.drop(-1); }, RangeError, "drop(-1)");
throws(function () { closable.drop(2 ** 53); }, RangeError, "drop(2**53)");
throws(function () { closable.take(Symbol()); }, TypeError, "take(Symbol)");
eq(log.join(), "return,return,return,return,return", "each closes the receiver");
throws(function () { Iterator.prototype.map.call(1, function () {}); }, TypeError, "map on a primitive");
throws(function () { Iterator.prototype.take.call(undefined, 1); }, TypeError, "take on undefined");
// next is read once, after the arguments are checked
var reads = 0;
var counting = { __proto__: Iterator.prototype, get next() { reads++; return function () { return { done: true }; }; } };
var h = counting.map(function () {});
eq(reads, 1, "next is read when the helper is made");
h.next(); h.next();
eq(reads, 1, "...and only then");

// a running helper cannot be re-entered
var inner;
inner = source(range(3)).map(function (x) { inner.next(); return x; });
throws(function () { inner.next(); }, TypeError, "calling next() from inside the mapper");
var self_return;
self_return = source(range(3)).map(function (x) { self_return.return(); return x; });
throws(function () { self_return.next(); }, TypeError, "calling return() from inside the mapper");

// the helpers advance the underlying iterator, which others can share
var shared = source(range(6));
var a = shared.take(2), b = shared.take(2);
eq([a.toArray(), b.toArray()], [[0, 1], [2, 3]], "two helpers on one source");

// ---- flatMap
eq(source(range(3)).flatMap(function (x) { return [x, x * 10]; }).toArray(), [0, 0, 1, 10, 2, 20], "flatMap with arrays");
eq(source(range(3)).flatMap(function (x) { return source(range(x)); }).toArray(), [0, 0, 1], "flatMap with iterators");
eq(source(range(3)).flatMap(function (x, i) { return [i]; }).toArray(), [0, 1, 2], "flatMap gets the counter");
throws(function () { source(range(2)).flatMap(function () { return "ab"; }).toArray(); }, TypeError, "flatMap rejects strings");
throws(function () { source(range(2)).flatMap(function () { return 1; }).toArray(); }, TypeError, "flatMap rejects numbers");
eq(source(range(2)).flatMap(function () { return new String("ab"); }).toArray(), ["a", "b", "a", "b"], "...but takes String objects");
log = [];
it = source(range(3), log).flatMap(function (x) { return source([1, 2], log); });
it.next();
log.length = 0;
it.return();
eq(log.join(), "return,return", "return() closes the inner iterator, then the outer one");
log = [];
throws(function () { source(range(3), log).flatMap(function () { return 5; }).next(); }, TypeError, "a bad inner value");
eq(log.join(), "next,return", "...closes the outer iterator");

// ---- Iterator.from
var plain = { next: function () { return { done: true }; } };
var w = Iterator.from(plain);
eq([w instanceof Iterator, w !== plain, typeof w.next, typeof w.return], [true, true, "function", "function"], "Iterator.from wraps a plain iterator");
eq(w.next().done, true, "the wrapper calls next of the plain iterator");
var it2 = source([1]);
eq(Iterator.from(it2) === it2, true, "an Iterator is returned as it is");
eq(Iterator.from([1, 2, 3]).toArray(), [1, 2, 3], "Iterator.from(array)");
eq(Iterator.from("abc").toArray(), ["a", "b", "c"], "Iterator.from(string)");
throws(function () { Iterator.from(5); }, TypeError, "Iterator.from(number)");
throws(function () { Iterator.from(null); }, TypeError, "Iterator.from(null)");
var wrapped = Iterator.from({ next: function () { return { value: 7, done: false }; }, return: function () { return { value: "closed", done: true }; } });
eq([wrapped.next().value, wrapped.return().value], [7, "closed"], "the wrapper forwards next and return");
eq(Iterator.from({ next: function () {} }).return().done, true, "return without a return method: done");

// ---- the eager methods
eq(source(range(5)).reduce(function (a, b) { return a + b; }), 10, "reduce");
eq(source(range(5)).reduce(function (a, b) { return a + b; }, 100), 110, "reduce with an initial value");
eq(source([]).reduce(function (a, b) { return a + b; }, "x"), "x", "reduce of nothing with an initial value");
throws(function () { source([]).reduce(function (a, b) { return a + b; }); }, TypeError, "reduce of nothing");
eq(source(range(4)).reduce(function (a, b, i) { return a + ":" + i; }, "s"), "s:0:1:2:3", "reduce gets the counter");
log = [];
throws(function () { source(range(3), log).reduce(function () { throw new RangeError("r"); }, 0); }, RangeError, "reduce: an exception from the reducer");
eq(log.join(), "next,return", "...closes the source");
var seen = [];
source(range(3)).forEach(function (x, i) { seen.push(x + ":" + i); });
eq(seen, ["0:0", "1:1", "2:2"], "forEach");
eq(source(range(3)).forEach(function () {}), undefined, "forEach returns undefined");
log = [];
eq([source(range(5), log).some(function (x) { return x == 2; }), log.join()], [true, "next,next,next,return"], "some stops and closes");
eq(source(range(5)).some(function (x) { return x > 9; }), false, "some");
log = [];
eq([source(range(5), log).every(function (x) { return x < 2; }), log.join()], [false, "next,next,next,return"], "every stops and closes");
eq(source(range(5)).every(function (x) { return x < 9; }), true, "every");
log = [];
eq([source(range(5), log).find(function (x) { return x > 2; }), log.join()], [3, "next,next,next,next,return"], "find stops and closes");
eq(source(range(5)).find(function (x) { return x > 9; }), undefined, "find finds nothing");
eq(source([]).some(function () { return true; }), false, "some of nothing");
eq(source([]).every(function () { return false; }), true, "every of nothing");
throws(function () { source(range(2)).forEach(1); }, TypeError, "forEach needs a function");
throws(function () { Iterator.prototype.toArray.call(null); }, TypeError, "toArray on null");
eq(source(range(3)).toArray(), [0, 1, 2], "toArray");
// on built-in iterators
eq(new Set([1, 2, 3]).values().map(function (x) { return x * 2; }).toArray(), [2, 4, 6], "map over a Set iterator");
eq(new Map([[1, "a"], [2, "b"]]).keys().filter(function (k) { return k > 1; }).toArray(), [2], "filter over a Map iterator");
eq("héllo"[Symbol.iterator]().take(2).toArray(), ["h", "é"], "take over a string iterator");
function* gen() { try { yield 1; yield 2; yield 3; } finally { fin.push("finally"); } }
var fin = [];
eq(gen().take(1).toArray(), [1], "take over a generator");
eq(fin, ["finally"], "...closes the generator");
fin = [];
eq(gen().find(function (x) { return x == 2; }), 2, "find over a generator");
eq(fin, ["finally"], "...closes the generator");
eq(Object.getPrototypeOf(gen.prototype) === Iterator.prototype || Object.getPrototypeOf(Object.getPrototypeOf(gen.prototype)) === Iterator.prototype, true, "generators inherit from Iterator.prototype");

print("iterhelpers: " + total + " checks, " + fails + " failures");
