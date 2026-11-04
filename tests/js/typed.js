// ArrayBuffer, SharedArrayBuffer, typed arrays, DataView and Atomics (ECMA-262 25.1 - 25.4, 23.2).
// Self-checking: every check states the expected value; the file prints its failures and a summary.
var total = 0, fails = 0;
function repr(v) {
  if (typeof v === "string") return JSON.stringify(v);
  if (typeof v === "symbol") return v.toString();
  if (Array.isArray(v)) return "[" + v.map(repr).join(",") + "]";
  if (ArrayBuffer.isView(v) && !(v instanceof DataView)) return "[" + Array.prototype.map.call(v, repr).join(",") + "]";   // contents only; types are checked where they matter
  if (Object.is(v, -0)) return "-0";
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
function bytes(buf) { return Array.from(new Uint8Array(buf)); }

// ---- ArrayBuffer
var ab = new ArrayBuffer(8);
eq([ab.byteLength, ab.resizable, ab.maxByteLength, ab.detached, Object.prototype.toString.call(ab)], [8, false, 8, false, "[object ArrayBuffer]"], "a fixed-length buffer");
eq(bytes(ab), [0, 0, 0, 0, 0, 0, 0, 0], "starts zeroed");
new Uint8Array(ab).set([1, 2, 3, 4, 5, 6, 7, 8]);
eq([bytes(ab.slice(2, 5)), bytes(ab.slice(-2)), ab.slice(6, 2).byteLength, ab.slice().byteLength], [[3, 4, 5], [7, 8], 0, 8], "slice");
eq([ArrayBuffer.isView(new Uint8Array(1)), ArrayBuffer.isView(new DataView(ab)), ArrayBuffer.isView(ab), ArrayBuffer.isView({})], [true, true, false, false], "ArrayBuffer.isView");
throws(function () { new ArrayBuffer(-1); }, RangeError, "negative length");
throws(function () { ArrayBuffer(1); }, TypeError, "needs new");
throws(function () { new ArrayBuffer(Infinity); }, RangeError, "infinite length");
eq(new ArrayBuffer("3").byteLength, 3, "length is converted");
eq(new ArrayBuffer(2.9).byteLength, 2, "length is truncated");
eq(ArrayBuffer[Symbol.species] === ArrayBuffer, true, "species");

var rb = new ArrayBuffer(4, { maxByteLength: 16 });
eq([rb.byteLength, rb.resizable, rb.maxByteLength], [4, true, 16], "a resizable buffer");
var view = new Uint8Array(rb), fixedView = new Uint8Array(rb, 0, 4), offsetView = new Uint8Array(rb, 2);
eq([view.length, fixedView.length, offsetView.length], [4, 4, 2], "views on a resizable buffer");
rb.resize(10);
eq([rb.byteLength, view.length, fixedView.length, offsetView.length], [10, 10, 4, 8], "growing: length-tracking views follow");
rb.resize(3);
eq([rb.byteLength, view.length, fixedView.length, fixedView.byteLength, fixedView.byteOffset, offsetView.length], [3, 3, 0, 0, 0, 1], "shrinking: a fixed view that no longer fits is out of bounds");
rb.resize(1);
eq([offsetView.length, offsetView.byteOffset, offsetView.byteLength], [0, 0, 0], "a view whose offset is past the end is out of bounds");
throws(function () { rb.resize(17); }, RangeError, "resize beyond the maximum");
throws(function () { new ArrayBuffer(8).resize(4); }, TypeError, "resize of a fixed-length buffer");
throws(function () { new ArrayBuffer(8, { maxByteLength: 4 }); }, RangeError, "maximum below the length");
rb.resize(5);
eq(rb.byteLength, 5, "and back");
eq(bytes(rb.slice(1)).length, 4, "slice of a resizable buffer");

var tb = new ArrayBuffer(4);
new Uint8Array(tb).set([9, 8, 7, 6]);
var moved = tb.transfer();
eq([tb.detached, tb.byteLength, moved.byteLength, bytes(moved)], [true, 0, 4, [9, 8, 7, 6]], "transfer detaches and moves the contents");
var bigger = moved.transfer(6);
eq([bigger.byteLength, bytes(bigger), moved.detached], [6, [9, 8, 7, 6, 0, 0], true], "transfer to a longer buffer pads with zeros");
var fixed = new ArrayBuffer(8, { maxByteLength: 8 }).transferToFixedLength();
eq([fixed.resizable, fixed.byteLength], [false, 8], "transferToFixedLength");
throws(function () { tb.slice(0); }, TypeError, "slice of a detached buffer");
throws(function () { tb.resize(1); }, TypeError, "resize of a detached buffer");
throws(function () { tb.transfer(); }, TypeError, "transfer of a detached buffer");
var dv0 = new Uint8Array(new ArrayBuffer(4));
dv0.buffer.transfer();
eq([dv0.length, dv0.byteLength, dv0.byteOffset, dv0[0]], [0, 0, 0, undefined], "views on a detached buffer are empty");
throws(function () { dv0.fill(1); }, TypeError, "methods on a view of a detached buffer throw");
throws(function () { new Uint8Array(tb); }, TypeError, "cannot create a view on a detached buffer");

// ---- SharedArrayBuffer
var sab = new SharedArrayBuffer(8);
eq([sab.byteLength, sab.growable, Object.prototype.toString.call(sab), sab instanceof ArrayBuffer], [8, false, "[object SharedArrayBuffer]", false], "SharedArrayBuffer");
var gsab = new SharedArrayBuffer(2, { maxByteLength: 6 });
var gview = new Uint8Array(gsab);
gsab.grow(5);
eq([gsab.byteLength, gsab.growable, gsab.maxByteLength, gview.length, bytes(gsab.slice(1, 3)).length], [5, true, 6, 5, 2], "growable");
throws(function () { gsab.grow(3); }, RangeError, "cannot shrink");
throws(function () { gsab.grow(7); }, RangeError, "cannot exceed the maximum");

// ---- element types: conversion, wrapping and clamping
function roundtrip(C, values) { var a = new C(values.length); values.forEach(function (v, i) { a[i] = v; }); return Array.from(a); }
eq(roundtrip(Int8Array, [127, 128, 255, 256, -129, 1.9, -1.9, NaN, Infinity, "5", true, null, undefined]), [127, -128, -1, 0, 127, 1, -1, 0, 0, 5, 1, 0, 0], "Int8Array");
eq(roundtrip(Uint8Array, [255, 256, -1, 1.9, -1.9, NaN, 2 ** 32 + 5]), [255, 0, 255, 1, 255, 0, 5], "Uint8Array");
eq(roundtrip(Uint8ClampedArray, [300, -5, 1.5, 2.5, 0.5, 254.5, 255.5, NaN, Infinity, -Infinity]), [255, 0, 2, 2, 0, 254, 255, 0, 255, 0], "Uint8ClampedArray rounds half to even");
eq(roundtrip(Int16Array, [32767, 32768, 65535, 65536, -32769]), [32767, -32768, -1, 0, 32767], "Int16Array");
eq(roundtrip(Uint16Array, [65535, 65536, -1]), [65535, 0, 65535], "Uint16Array");
eq(roundtrip(Int32Array, [2147483647, 2147483648, 4294967295, -2147483649, 2 ** 53]), [2147483647, -2147483648, -1, 2147483647, 0], "Int32Array");
eq(roundtrip(Uint32Array, [4294967295, 4294967296, -1]), [4294967295, 0, 4294967295], "Uint32Array");
eq(roundtrip(Float32Array, [1.5, 0.1, 16777217, 1e40, -1e40, 1e-50]), [1.5, 0.10000000149011612, 16777216, Infinity, -Infinity, 0], "Float32Array rounds to single precision");
eq(roundtrip(Float64Array, [0.1, -0, 1e300, NaN]).map(function (v) { return Object.is(v, -0) ? "-0" : v; }), [0.1, "-0", 1e300, NaN], "Float64Array");
eq([Int8Array.BYTES_PER_ELEMENT, Uint16Array.BYTES_PER_ELEMENT, Float32Array.BYTES_PER_ELEMENT, Float64Array.BYTES_PER_ELEMENT, Int8Array.prototype.BYTES_PER_ELEMENT], [1, 2, 4, 8, 1], "BYTES_PER_ELEMENT");
eq(Float32Array.name + Float32Array.length + Object.getPrototypeOf(Float32Array).name + Object.getPrototypeOf(Float32Array).length, "Float32Array3TypedArray0", "names and lengths (the concrete constructors have length 3)");
var nan32 = new Float32Array([NaN]);
eq(nan32[0] !== nan32[0], true, "NaN survives");
var e1 = new Uint16Array(1);
e1[0] = 0x1234;
eq(bytes(e1.buffer), [0x34, 0x12], "little-endian layout on this platform");

// ---- construction
eq(new Uint8Array(3), [0, 0, 0], "from a length");
eq(new Uint8Array([1, 2, 300]), [1, 2, 44], "from an array");
eq(new Uint8Array({ length: 2, 0: 7, 1: 8 }), [7, 8], "from an array-like");
eq(new Uint8Array(new Set([4, 5])), [4, 5], "from an iterable");
eq(new Uint16Array(new Uint8Array([1, 2, 3])), [1, 2, 3], "from another typed array (converting)");
eq(new Uint8Array(new ArrayBuffer(6), 2, 3).byteOffset, 2, "view with offset");
eq(new Uint8Array(new ArrayBuffer(6), 2).length, 4, "view to the end");
eq(new Float64Array(new ArrayBuffer(16), 8).length, 1, "aligned offset");
throws(function () { new Uint16Array(new ArrayBuffer(6), 1); }, RangeError, "unaligned offset");
throws(function () { new Uint16Array(new ArrayBuffer(5)); }, RangeError, "length not a multiple of the element size");
throws(function () { new Uint8Array(new ArrayBuffer(4), 5); }, RangeError, "offset past the end");
throws(function () { new Uint8Array(new ArrayBuffer(4), 0, 5); }, RangeError, "length past the end");
throws(function () { Uint8Array(1); }, TypeError, "needs new");
throws(function () { new Uint8Array(-1); }, RangeError, "negative length");
throws(function () { new (Object.getPrototypeOf(Uint8Array))(); }, TypeError, "%TypedArray% cannot be constructed");
throws(function () { Object.getPrototypeOf(Uint8Array)(); }, TypeError, "%TypedArray% cannot be called");
var shared = new Uint8Array(new ArrayBuffer(4));
eq(new Uint8Array(shared.buffer)[0] = 9, 9, "assignment value");
eq(shared[0], 9, "a view shares the buffer");
eq([new Uint8Array(4).subarray(1, 3).byteOffset, new Uint8Array(4).subarray(1, 3).length, new Uint8Array(4).subarray(-1).length], [1, 2, 1], "subarray shares the buffer");
eq(Uint8Array.from([1, 2, 3], function (x) { return x * 2; }), [2, 4, 6], "from with a mapper");
eq(Uint8Array.of(1, 2, 3), [1, 2, 3], "of");
eq(Uint8Array.from("123"), [1, 2, 3], "from a string");
eq(Uint8Array.from({ length: 3 }, function (_, i) { return i + 1; }), [1, 2, 3], "from an array-like with a mapper");
eq(Object.getPrototypeOf(Uint8Array.prototype) === Object.getPrototypeOf(Int8Array.prototype), true, "a common prototype");
eq(Object.getPrototypeOf(Int8Array) === Object.getPrototypeOf(Uint8Array), true, "a common constructor");
eq(new Uint8Array(2)[Symbol.toStringTag] + Object.prototype.toString.call(new Float64Array(0)), "Uint8Array[object Float64Array]", "toStringTag");

// ---- integer-indexed exotic behaviour
var ta = new Uint8Array([10, 20, 30]);
ta[5] = 1;
ta[-1] = 1;
ta["1.5"] = 1;
ta.foo = "bar";
eq([ta[3], ta[5], ta[-1], ta["1.5"], ta.foo, Object.keys(ta)], [undefined, undefined, undefined, undefined, "bar", ["0", "1", "2", "foo"]], "out-of-range and non-integer indexes are never properties");
eq([1 in ta, 3 in ta, "-0" in ta, "1.5" in ta, 1.5 in ta, "foo" in ta], [true, false, false, false, false, true], "has");
eq([delete ta[5], delete ta[0], ta[0], Reflect.deleteProperty(ta, "0")], [true, false, 10, false], "deleting an element fails, a missing index succeeds");
var d =Object.getOwnPropertyDescriptor(ta, "1");
eq([d.value, d.writable, d.enumerable, d.configurable], [20, true, true, true], "element descriptors");
throws(function () { Object.defineProperty(ta, "0", { value: 1, enumerable: false }); }, TypeError, "elements cannot be defined as non-enumerable");
throws(function () { Object.defineProperty(ta, "7", { value: 1 }); }, TypeError, "defining out of range");
eq(Reflect.defineProperty(ta, "0", { value: 99 }) && ta[0], 99, "defining the value works");
throws(function () { Object.freeze(ta); }, TypeError, "freezing a view with elements");
eq(Object.isFrozen(Object.freeze(new Uint8Array(0))), true, "freezing an empty view");
eq(Object.getOwnPropertyNames(new Uint8Array(2)), ["0", "1"], "own names are the indexes");
eq(Reflect.ownKeys(new Uint8Array(1)), ["0"], "own keys");
eq([Object.getOwnPropertySymbols(new Uint8Array(2)).length, Object.getOwnPropertyNames(Object.assign(new Uint8Array(2), { x: 1 }))], [0, ["0", "1", "x"]], "symbols only: the indexes are not symbols");
var tsym = Symbol("s"), tsa = new Uint8Array(2);
tsa[tsym] = 1; tsa.str = 2;
eq([Object.getOwnPropertySymbols(tsa).length, Reflect.ownKeys(tsa).length, Object.keys(tsa)], [1, 4, ["0", "1", "str"]], "a symbol and a string key beside the elements");
eq(JSON.stringify(new Uint8Array([1, 2])), '{"0":1,"1":2}', "JSON of a view is an object");
var oob = new Uint8Array(new ArrayBuffer(4, { maxByteLength: 8 }), 2, 2);
oob.buffer.resize(3);
eq([oob.length, oob[0], 0 in oob, Object.keys(oob)], [0, undefined, false, []], "an out-of-bounds view has no elements");

// ---- prototype methods
var m = new Int16Array([5, -1, 4, 2, 3]);
eq(m.slice().sort(), [-1, 2, 3, 4, 5], "sort numbers (not strings)");
eq(m.slice().sort(function (a, b) { return b - a; }), [5, 4, 3, 2, -1], "sort with a comparator");
eq([m.indexOf(4), m.lastIndexOf(4), m.includes(2), m.includes(9), m.at(-1), m.at(9)], [2, 2, true, false, 3, undefined], "searching");
eq([m.map(function (x) { return x * 2; }), m.filter(function (x) { return x > 2; })], [new Int16Array([10, -2, 8, 4, 6]), new Int16Array([5, 4, 3])], "map and filter keep the type");
eq([m.reduce(function (a, b) { return a + b; }), m.reduceRight(function (a, b) { return a + "," + b; }), m.every(function (x) { return x < 9; }), m.some(function (x) { return x < 0; })], [13, "3,2,4,-1,5", true, true], "reductions");
eq([m.find(function (x) { return x < 3; }), m.findIndex(function (x) { return x < 3; }), m.findLast(function (x) { return x < 3; }), m.findLastIndex(function (x) { return x > 100; })], [-1, 1, 2, -1], "find family");
eq([m.join("-"), m.toString(), typeof m.toLocaleString, String(m)], ["5--1-4-2-3", "5,-1,4,2,3", "function", "5,-1,4,2,3"], "join and toString");
eq([m.map(function (x) { return x; }) instanceof Int16Array, m.slice(1) instanceof Int16Array, m.subarray(1) instanceof Int16Array, m.toSorted() instanceof Int16Array], [true, true, true, true], "results have the type of the receiver");
eq([m.slice(1, 3), m.slice(-2), m.subarray(1, 3), m.slice().reverse(), m.slice().fill(7, 1, 3), m.slice().copyWithin(0, 3)], [new Int16Array([-1, 4]), new Int16Array([2, 3]), new Int16Array([-1, 4]), new Int16Array([3, 2, 4, -1, 5]), new Int16Array([5, 7, 7, 2, 3]), new Int16Array([2, 3, 4, 2, 3])], "slice, subarray, reverse, fill, copyWithin");
eq([m.toReversed(), m.toSorted(), m.with(0, 9), m], [new Int16Array([3, 2, 4, -1, 5]), new Int16Array([-1, 2, 3, 4, 5]), new Int16Array([9, -1, 4, 2, 3]), new Int16Array([5, -1, 4, 2, 3])], "the copying methods leave the original");
throws(function () { m.with(9, 1); }, RangeError, "with out of range");
var st = new Uint8Array(5);
st.set([1, 2], 1); st.set(new Uint8Array([9]), 4);
eq(st, [0, 1, 2, 0, 9], "set from an array and from a view");
throws(function () { st.set([1, 2, 3], 4); }, RangeError, "set past the end");
throws(function () { st.set([1], -1); }, RangeError, "set at a negative offset");
var ov = new Uint8Array([1, 2, 3, 4, 5]);
ov.set(ov.subarray(0, 3), 2);
eq(ov, [1, 2, 1, 2, 3], "set between overlapping views of one buffer behaves as if copied first");
var ent = [];
for (var kv of new Uint8Array([7, 8]).entries()) ent.push(kv.join(":"));
eq([ent, Array.from(new Uint8Array([7, 8]).keys()), Array.from(new Uint8Array([7, 8]).values()), [...new Uint8Array([7, 8])]], [["0:7", "1:8"], [0, 1], [7, 8], [7, 8]], "iteration");
eq(Uint8Array.prototype[Symbol.iterator] === Uint8Array.prototype.values, true, "iterator is values");
eq([new Uint8Array([1, 2]).forEach.length, Uint8Array.prototype.set.length, Object.getPrototypeOf(Uint8Array).prototype.map.length], [1, 1, 1], "method lengths");
class MyArray extends Uint8Array {}
var my = new MyArray([1, 2, 3]);
eq([my.map(function (x) { return x; }) instanceof MyArray, my.slice(1) instanceof MyArray, my.subarray(1) instanceof MyArray, my.filter(Boolean).length], [true, true, true, 3], "species: subclass results");
var cmpCalls = 0;
new Uint8Array([3, 1, 2]).sort(function (a, b) { cmpCalls++; return a - b; });
eq(cmpCalls > 0, true, "the comparator is called");
throws(function () { new Uint8Array(2).sort(1); }, TypeError, "sort with a non-function comparator");
throws(function () { Uint8Array.prototype.map.call([], function () {}); }, TypeError, "methods need a typed array this");
var f32 = new Float32Array([NaN, 1, -0, 0, -Infinity]);
eq(f32.slice().sort(), [-Infinity, -0, 0, 1, NaN], "sort orders -0 before 0 and NaN last");
eq([f32.includes(NaN), f32.indexOf(NaN)], [true, -1], "includes finds NaN, indexOf does not");

// ---- DataView
var buf = new ArrayBuffer(16);
var dv = new DataView(buf);
dv.setInt16(0, -2);
dv.setUint16(2, 0x1234, true);
dv.setFloat32(4, 1.5);
dv.setFloat64(8, Math.PI, true);
eq([dv.getInt16(0), dv.getUint16(0), dv.getUint8(1), dv.getUint16(2, true), dv.getUint16(2), dv.getFloat32(4), dv.getFloat64(8, true)], [-2, 65534, 254, 0x1234, 0x3412, 1.5, Math.PI], "reading and writing with both byte orders");
eq([dv.byteLength, dv.byteOffset, dv.buffer === buf, Object.prototype.toString.call(dv)], [16, 0, true, "[object DataView]"], "DataView properties");
eq(bytes(buf).slice(0, 4), [0xFF, 0xFE, 0x34, 0x12], "the bytes written");
dv.setInt32(0, 0x01020304);
dv.setUint8(4, 255); dv.setInt8(5, -1);
eq([bytes(buf).slice(0, 6), dv.getInt32(0), dv.getInt32(0, true), dv.getInt8(4), dv.getUint8(5)], [[1, 2, 3, 4, 255, 255], 0x01020304, 0x04030201, -1, 255], "32 and 8 bits");
dv.setUint32(0, 0xFFFFFFFF);
eq([dv.getUint32(0), dv.getInt32(0)], [4294967295, -1], "unsigned 32 bits");
throws(function () { dv.getInt16(15); }, RangeError, "reading past the end");
throws(function () { dv.setUint8(16, 0); }, RangeError, "writing past the end");
throws(function () { dv.getInt8(-1); }, RangeError, "negative offset");
throws(function () { new DataView(buf, 17); }, RangeError, "offset past the end");
throws(function () { new DataView(buf, 8, 9); }, RangeError, "length past the end");
throws(function () { new DataView({}); }, TypeError, "needs a buffer");
throws(function () { DataView(buf); }, TypeError, "needs new");
dv.setFloat32(4, 1.5);
var sub = new DataView(buf, 4, 8);
eq([sub.byteOffset, sub.byteLength, sub.getFloat32(0)], [4, 8, 1.5], "a DataView with offset and length");
throws(function () { sub.getFloat64(4); }, RangeError, "bounds are the view's");
var rdv = new DataView(new ArrayBuffer(4, { maxByteLength: 8 }));
rdv.buffer.resize(8);
eq(rdv.byteLength, 8, "a length-tracking DataView");
var rdv2 = new DataView(rdv.buffer, 6);
rdv.buffer.resize(4);
eq([rdv2.byteLength, rdv2.byteOffset], [0, 0], "an out-of-bounds DataView reads 0 (as the typed arrays do, ES2025)");
throws(function () { rdv2.getInt8(0); }, TypeError, "but its get and set methods throw");
eq([new DataView(new ArrayBuffer(8)).getFloat64(0), new DataView(new ArrayBuffer(2)).getUint16(0)], [0, 0], "zero bytes");
eq(["Int8", "Uint8", "Int16", "Uint16", "Int32", "Uint32", "Float32", "Float64"].every(function (t) { return typeof DataView.prototype["get" + t] === "function" && typeof DataView.prototype["set" + t] === "function"; }), true, "the get and set methods");
dv.setFloat32(0, 16777217);
eq(dv.getFloat32(0), 16777216, "setFloat32 rounds");
dv.setUint8(0, 257);
eq(dv.getUint8(0), 1, "setUint8 wraps");
eq(dv.setInt8(0, 1), undefined, "setters return undefined");

// ---- Atomics
var ia = new Int32Array(new SharedArrayBuffer(16));
eq([Atomics.add(ia, 0, 5), Atomics.sub(ia, 0, 2), Atomics.load(ia, 0), Atomics.store(ia, 1, 7), ia[1]], [0, 5, 3, 7, 7], "add, sub, load and store return the old value, the stored value");
eq([Atomics.and(ia, 0, 1), Atomics.or(ia, 0, 6), Atomics.xor(ia, 0, 2), Atomics.load(ia, 0)], [3, 1, 7, 5], "bitwise operations");
eq([Atomics.exchange(ia, 0, 9), Atomics.compareExchange(ia, 0, 9, 1), Atomics.compareExchange(ia, 0, 9, 2), ia[0]], [5, 9, 1, 1], "exchange and compareExchange");
var i8 = new Int8Array(2);
eq([Atomics.add(i8, 0, 200), i8[0], Atomics.store(i8, 1, 300), i8[1]], [0, -56, 300, 44], "values wrap to the element type but store returns the converted number");
eq([Atomics.isLockFree(1), Atomics.isLockFree(2), Atomics.isLockFree(4), Atomics.isLockFree(3)], [true, true, true, false], "isLockFree");
eq(Atomics.notify(ia, 0, 1), 0, "notify with no waiters");
eq([Atomics.wait(ia, 0, 12345, 0), Atomics.wait(ia, 0, ia[0], 0), Atomics.wait(ia, 0, ia[0], -5), Atomics.wait(ia, 0, ia[0], 1)], ["not-equal", "timed-out", "timed-out", "timed-out"], "wait: a different value, a timeout of zero, a negative one, a short one");
var w0 = Date.now();
eq([Atomics.wait(ia, 0, ia[0], 40), Date.now() - w0 >= 35], ["timed-out", true], "wait really waits (nobody can wake this agent)");
throws(function () { Atomics.wait(ia, 9, 0, 0); }, RangeError, "wait: index out of range, checked before waiting");
throws(function () { Atomics.wait(ia, 0, Symbol(), 0); }, TypeError, "wait: the value is converted before waiting");
throws(function () { Atomics.wait(ia, 0, 0, Symbol()); }, TypeError, "wait: the timeout is converted before waiting");
eq([Atomics.notify(ia, 0), Atomics.notify(ia, 0, 5), Atomics.notify(new Int32Array(4), 0)], [0, 0, 0], "notify: no waiters, also on a non-shared array");
throws(function () { Atomics.add(new Float64Array(1), 0, 1); }, TypeError, "not for floats");
throws(function () { Atomics.add(new Uint8ClampedArray(1), 0, 1); }, TypeError, "not for clamped arrays");
throws(function () { Atomics.load(ia, 4); }, RangeError, "index out of range");
throws(function () { Atomics.wait(new Int32Array(4), 0, 0, 0); }, TypeError, "wait needs a shared buffer");
throws(function () { Atomics.wait(new Int32Array(4), 9, 0, 0); }, TypeError, "(and says so before it looks at the index)");
throws(function () { Atomics.wait(new Int8Array(new SharedArrayBuffer(4)), 0, 0, 0); }, TypeError, "wait needs an Int32Array or BigInt64Array");
eq(Atomics[Symbol.toStringTag], "Atomics", "Atomics itself");

print("typed: " + total + " checks, " + fails + " failures");
