// Proxy and Reflect (ECMA-262 10.5 Proxy Object Internal Methods, 28.1 Reflect, 28.2 Proxy Objects).
// Self-checking: every check states the expected value; the file prints its failures and a summary.
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

// ---- no handler traps: everything goes to the target
var target = { a: 1, b: 2 };
var p = new Proxy(target, {});
p.c = 3;
eq([target.c, p.a, "a" in p, Object.keys(p), delete p.a, "a" in target], [3, 1, true, ["a", "b", "c"], true, false], "transparent proxy");
eq([Object.getPrototypeOf(p) === Object.prototype, Object.isExtensible(p), typeof p], [true, true, "object"], "proxy of an object");
eq([typeof new Proxy(function () {}, {}), typeof new Proxy(class {}, {})], ["function", "function"], "typeof a proxy of a function");
eq([Array.isArray(new Proxy([], {})), Array.isArray(new Proxy({}, {})), Object.prototype.toString.call(new Proxy([], {}))], [true, false, "[object Array]"], "proxy of an array is an array");
eq(JSON.stringify(new Proxy([1, 2, 3], {})), "[1,2,3]", "JSON.stringify sees through");
eq(JSON.stringify(new Proxy({ x: 1, y: [2] }, {})), '{"x":1,"y":[2]}', "JSON.stringify of an object proxy");

// ---- every trap, with the arguments it gets
var log = [];
function logged(name, result) { return function () { log.push(name); return result.apply(null, arguments); }; }
var tgt = { k: 1 };
var handler = {
  get: function (t, key, receiver) { log.push("get " + String(key)); return Reflect.get(t, key, receiver); },
  set: function (t, key, value, receiver) { log.push("set " + String(key)); return Reflect.set(t, key, value, receiver); },
  has: function (t, key) { log.push("has " + String(key)); return Reflect.has(t, key); },
  deleteProperty: function (t, key) { log.push("delete " + String(key)); return Reflect.deleteProperty(t, key); },
  defineProperty: function (t, key, d) { log.push("define " + String(key)); return Reflect.defineProperty(t, key, d); },
  getOwnPropertyDescriptor: function (t, key) { log.push("gopd " + String(key)); return Reflect.getOwnPropertyDescriptor(t, key); },
  ownKeys: function (t) { log.push("ownKeys"); return Reflect.ownKeys(t); },
  getPrototypeOf: function (t) { log.push("getProto"); return Reflect.getPrototypeOf(t); },
  setPrototypeOf: function (t, pr) { log.push("setProto"); return Reflect.setPrototypeOf(t, pr); },
  isExtensible: function (t) { log.push("isExt"); return Reflect.isExtensible(t); },
  preventExtensions: function (t) { log.push("prevent"); return Reflect.preventExtensions(t); }
};
var lp = new Proxy(tgt, handler);
lp.k; lp.z = 1; "k" in lp; delete lp.z; Object.defineProperty(lp, "w", { value: 1, configurable: true });
Object.getOwnPropertyDescriptor(lp, "k"); Object.keys(lp); Object.getPrototypeOf(lp); Object.setPrototypeOf(lp, null);
Object.isExtensible(lp);
eq(log.join(), "get k,set z,gopd z,define z,has k,delete z,define w,gopd k,ownKeys,gopd k,gopd w,getProto,setProto,isExt",
   "the traps called by each operation, in order");
log.length = 0;
for (var key in new Proxy({ x: 1, y: 2 }, { ownKeys: function (t) { log.push("ownKeys"); return Reflect.ownKeys(t); }, getPrototypeOf: function (t) { log.push("getProto"); return null; },
                                          getOwnPropertyDescriptor: function (t, k) { log.push("gopd " + k); return Reflect.getOwnPropertyDescriptor(t, k); } })) log.push("key " + key);
eq([log[0], log.indexOf("getProto") > 0, log.filter(function (s) { return s.slice(0, 3) === "key"; }).join(), log.indexOf("gopd x") > 0], ["ownKeys", true, "key x,key y", true],
   "for-in over a proxy asks for the keys, the prototype and the descriptors (the order after ownKeys is the engine's)");

// ---- receiver, this and arguments
var recv = [];
var rp = new Proxy({ get v() { return this; }, set s(x) { recv.push(this); } }, {});
eq(rp.v === rp, true, "a getter on the target runs with the proxy as this");
rp.s = 1;
eq(recv[0] === rp, true, "a setter too");
var child = Object.create(rp);
eq(child.v === child, true, "through the prototype chain the receiver is the object asked");
var gotArgs;
var ap = new Proxy(function f(a, b) { return a + b; }, { apply: function (t, thisArg, args) { gotArgs = [thisArg, args.length, Array.isArray(args)]; return t.apply(thisArg, args) * 10; } });
eq([ap(1, 2), gotArgs[1], gotArgs[2], gotArgs[0]], [30, 2, true, undefined], "apply trap");
var o2 = { m: ap };
o2.m(1, 1);
eq(gotArgs[0] === o2, true, "apply trap gets the this value");
var cp = new Proxy(function F(x) { this.x = x; }, { construct: function (t, args, newTarget) { return { made: args[0], nt: newTarget === cp }; } });
var made = new cp(5);
eq([made.made, made.nt], [5, true], "construct trap");
throws(function () { new (new Proxy(function () {}, { construct: function () { return 1; } }))(); }, TypeError, "construct trap must return an object");
throws(function () { new (new Proxy(() => {}, {}))(); }, TypeError, "a proxy of an arrow function is not a constructor");
throws(function () { (new Proxy({}, {}))(); }, TypeError, "a proxy of a plain object is not callable");
eq(Reflect.construct(function () { return new.target; }, [], Array) === Array, true, "Reflect.construct newTarget");

// ---- invariants
var frozen = Object.freeze({ a: 1 });
throws(function () { new Proxy(frozen, { get: function () { return 2; } }).a; }, TypeError, "get must report the value of a non-writable, non-configurable property");
eq(new Proxy(frozen, { get: function () { return 1; } }).a, 1, "the same value is fine");
throws(function () { "a" in new Proxy(frozen, { has: function () { return false; } }); }, TypeError, "has cannot hide a non-configurable property");
throws(function () { "z" in new Proxy(Object.preventExtensions({ z: 1 }), { has: function () { return false; } }); }, TypeError, "has cannot hide a property of a non-extensible target");
throws(function () { delete new Proxy(frozen, { deleteProperty: function () { return true; } }).a; }, TypeError, "deleteProperty cannot claim to delete a non-configurable property");
throws(function () { Object.keys(new Proxy(frozen, { ownKeys: function () { return []; } })); }, TypeError, "ownKeys must include the non-configurable keys");
throws(function () { Object.keys(new Proxy({}, { ownKeys: function () { return ["a", "a"]; } })); }, TypeError, "ownKeys result without duplicates");
throws(function () { Object.keys(new Proxy({}, { ownKeys: function () { return [1]; } })); }, TypeError, "ownKeys result of strings and symbols only");
throws(function () { Object.keys(new Proxy(Object.preventExtensions({ a: 1 }), { ownKeys: function () { return ["a", "b"]; } })); }, TypeError, "ownKeys of a non-extensible target is exactly its keys");
throws(function () { Object.getPrototypeOf(new Proxy(Object.preventExtensions({}), { getPrototypeOf: function () { return Array.prototype; } })); }, TypeError, "getPrototypeOf of a non-extensible target");
throws(function () { Object.getPrototypeOf(new Proxy({}, { getPrototypeOf: function () { return 1; } })); }, TypeError, "getPrototypeOf returns an object or null");
throws(function () { Object.isExtensible(new Proxy({}, { isExtensible: function () { return false; } })); }, TypeError, "isExtensible must agree with the target");
throws(function () { Object.preventExtensions(new Proxy({}, { preventExtensions: function () { return true; } })); }, TypeError, "preventExtensions cannot claim success on an extensible target");
throws(function () { Object.getOwnPropertyDescriptor(new Proxy({ a: 1 }, { getOwnPropertyDescriptor: function () { return 1; } }), "a"); }, TypeError, "descriptor trap returns an object or undefined");
throws(function () { Object.getOwnPropertyDescriptor(new Proxy(frozen, { getOwnPropertyDescriptor: function () { return undefined; } }), "a"); }, TypeError, "cannot report a non-configurable property as absent");
throws(function () { Object.defineProperty(new Proxy({}, { defineProperty: function () { return true; } }), "x", { value: 1, configurable: false }); }, TypeError, "defineProperty cannot claim a non-configurable property the target lacks");
eq(Reflect.set(new Proxy({}, { set: function () { return false; } }), "a", 1), false, "set trap false: Reflect.set reports it");
throws(function () { "use strict"; new Proxy({}, { set: function () { return false; } }).a = 1; }, TypeError, "set trap false throws in strict code");
eq((function () { new Proxy({}, { set: function () { return false; } }).a = 1; return "silent"; })(), "silent", "and is silent in sloppy code");
throws(function () { new Proxy(frozen, { set: function () { return true; } }).a = 2; }, TypeError, "set cannot claim to change a frozen property");
eq(new Proxy({}, { get: function (t, k) { return typeof k; } }).anything, "string", "get trap result is used");

// ---- the target of a revoked proxy
var r = Proxy.revocable({ a: 1 }, {});
eq([r.proxy.a, typeof r.revoke], [1, "function"], "revocable");
r.revoke();
r.revoke();
throws(function () { r.proxy.a; }, TypeError, "get on a revoked proxy");
throws(function () { r.proxy.a = 1; }, TypeError, "set on a revoked proxy");
throws(function () { "a" in r.proxy; }, TypeError, "has on a revoked proxy");
throws(function () { Object.keys(r.proxy); }, TypeError, "ownKeys on a revoked proxy");
eq(typeof r.proxy, "object", "typeof a revoked proxy is still object");
var onRevoked = new Proxy(r.proxy, {});      // allowed since ES2020: only using it fails
throws(function () { onRevoked.a; }, TypeError, "a proxy of a revoked proxy fails when used");
var rf = Proxy.revocable(function () {}, {});
rf.revoke();
eq(typeof rf.proxy, "function", "typeof a revoked function proxy is still function");
throws(function () { rf.proxy(); }, TypeError, "calling a revoked proxy");
var ra = Proxy.revocable([], {});
ra.revoke();
throws(function () { Array.isArray(ra.proxy); }, TypeError, "Array.isArray on a revoked proxy");

// ---- construction rules
throws(function () { Proxy({}, {}); }, TypeError, "Proxy needs new");
throws(function () { new Proxy(1, {}); }, TypeError, "target must be an object");
throws(function () { new Proxy({}, null); }, TypeError, "handler must be an object");
eq([Proxy.length, Proxy.name, Proxy.prototype, typeof Proxy.revocable], [2, "Proxy", undefined, "function"], "Proxy itself");
var chain = new Proxy(new Proxy(new Proxy({ deep: 1 }, {}), {}), {});
eq([chain.deep, Object.keys(chain)], [1, ["deep"]], "proxies of proxies");

// ---- the usual uses
var defaults = new Proxy({}, { get: function (t, k) { return k in t ? t[k] : "default " + String(k); } });
defaults.set = 1;
eq([defaults.set, defaults.other], [1, "default other"], "default values");
var neg = new Proxy([10, 20, 30], { get: function (t, k, rc) { var i = Number(k); return Number.isInteger(i) && i < 0 ? t[t.length + i] : Reflect.get(t, k, rc); } });
eq([neg[-1], neg[0], neg.length, neg.map(function (x) { return x + 1; })], [30, 10, 3, [11, 21, 31]], "negative array indexes");
var validated = new Proxy({}, { set: function (t, k, v) { if (typeof v !== "number") throw new TypeError("number needed"); t[k] = v; return true; } });
validated.n = 4;
throws(function () { validated.s = "x"; }, TypeError, "a validating set trap");
eq(validated.n, 4, "validated value kept");
var hidden = new Proxy({ _secret: 1, shown: 2 }, {
  has: function (t, k) { return k[0] !== "_" && k in t; },
  ownKeys: function (t) { return Reflect.ownKeys(t).filter(function (k) { return k[0] !== "_"; }); },
  get: function (t, k) { return k[0] === "_" ? undefined : t[k]; }
});
eq([Object.keys(hidden), "_secret" in hidden, hidden._secret, hidden.shown, JSON.stringify(hidden)], [["shown"], false, undefined, 2, '{"shown":2}'], "hiding keys");
var spreadSrc = new Proxy({ x: 1, y: 2 }, { ownKeys: function (t) { return ["y", "x"]; } });
eq([Object.keys({ ...spreadSrc }), Object.assign({}, spreadSrc).y, Object.entries(spreadSrc)], [["y", "x"], 2, [["y", 2], ["x", 1]]], "spread, assign and entries use ownKeys order");
var counter = 0;
var cnt = new Proxy({}, { get: function () { return ++counter; } });
eq([cnt.a, cnt.b, `${cnt.c}`], [1, 2, "3"], "a get trap that counts");
var sym = Symbol("s");
var sp = new Proxy({}, { get: function (t, k) { return typeof k === "symbol" ? "symbol" : "string"; } });
eq([sp[sym], sp.x, sp[Symbol.iterator]], ["symbol", "string", "symbol"], "symbols reach the trap as symbols");
var fnp = new Proxy(function () {}, {});
eq([fnp.name, fnp.length, fnp instanceof Function, Object.getPrototypeOf(fnp) === Function.prototype], ["", 0, true, true], "a function proxy");
var instP = new Proxy(class A {}, {});
eq([new instP() instanceof instP], [true], "instanceof with a proxy of a class");
class Base { constructor() { this.made = new.target === Sub2; } }
var Sub2 = class extends new Proxy(Base, {}) {};
eq(new Sub2().made, true, "extending a proxy of a class");

// ---- Reflect
eq([Reflect.get({ a: 5 }, "a"), Reflect.get({ get a() { return this.b; } }, "a", { b: 7 })], [5, 7], "Reflect.get with a receiver");
var rs = { set a(v) { this.stored = v; } }, recvObj = {};
eq([Reflect.set(rs, "a", 3, recvObj), recvObj.stored, rs.stored], [true, 3, undefined], "Reflect.set with a receiver");
eq([Reflect.has({ a: 1 }, "a"), Reflect.has([], "length"), Reflect.has({}, "toString")], [true, true, true], "Reflect.has");
eq([Reflect.ownKeys({ b: 1, [Symbol.for("q")]: 1, 1: 1 }).length, Reflect.ownKeys([1]).join()], [3, "0,length"], "Reflect.ownKeys");
eq([Reflect.defineProperty({}, "x", { value: 1 }), Reflect.defineProperty(Object.freeze({}), "x", { value: 1 })], [true, false], "Reflect.defineProperty returns a boolean");
eq([Reflect.deleteProperty({ a: 1 }, "a"), Reflect.deleteProperty(Object.freeze({ a: 1 }), "a")], [true, false], "Reflect.deleteProperty");
eq([Reflect.getPrototypeOf([]) === Array.prototype, Reflect.setPrototypeOf({}, null), Reflect.setPrototypeOf(Object.preventExtensions({}), {})], [true, true, false], "Reflect prototypes");
eq([Reflect.isExtensible({}), Reflect.preventExtensions({}), Reflect.isExtensible(Object.freeze({}))], [true, true, false], "Reflect extensibility");
eq(Reflect.apply(Math.max, null, [1, 5, 3]), 5, "Reflect.apply");
eq([Reflect.getOwnPropertyDescriptor({ a: 1 }, "a").value, Reflect.getOwnPropertyDescriptor({}, "a")], [1, undefined], "Reflect.getOwnPropertyDescriptor");
throws(function () { Reflect.get(1, "a"); }, TypeError, "Reflect.get of a primitive");
throws(function () { Reflect.apply(1); }, TypeError, "Reflect.apply of a non-function");
throws(function () { Reflect.construct(function () {}, [], 1); }, TypeError, "Reflect.construct with a bad newTarget");
throws(function () { Reflect.construct(function () {}, 1); }, TypeError, "Reflect.construct with bad arguments");
eq([Reflect[Symbol.toStringTag], Object.prototype.toString.call(Reflect), typeof Reflect], ["Reflect", "[object Reflect]", "object"], "Reflect itself");
eq(Object.getOwnPropertyNames(Reflect).sort().join(), "apply,construct,defineProperty,deleteProperty,get,getOwnPropertyDescriptor,getPrototypeOf,has,isExtensible,ownKeys,preventExtensions,set,setPrototypeOf", "Reflect has the thirteen functions");

print("proxy: " + total + " checks, " + fails + " failures");
