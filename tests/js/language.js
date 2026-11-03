// closures and captured variables
function counter() { var n = 0; return function () { return ++n; }; }
var c = counter(); c(); c();
print("counter", c());
var fs = [];
for (let i = 0; i < 3; i++) fs.push = undefined, fs[i] = () => i;
print("per-iteration", fs[0](), fs[1](), fs[2]());
// exceptions
try { throw 42; } catch (e) { print("caught", e); }
function tf() { try { return "try"; } finally { print("finally runs"); } }
print(tf());
var log = "";
for (var j = 0; j < 5; j++) { try { if (j == 1) continue; if (j == 3) break; log += j; } finally { log += "f"; } }
print("loop-finally", log);
// objects
var o = { a: 1, b: "two", get c() { return this.a + 10; }, m(x) { return this.a + x; } };
print("object", o.a, o.b, o.c, o.m(5));
o.d = 4; delete o.a;
print("delete", o.a, o.d, "a" in o, "d" in o);
// arrays
var arr = [1, 2, 3];
arr[5] = 6;
print("array", arr.length, arr[1], arr[4], arr[5]);
// switch and labels
function sw(x) { switch (x) { case 1: return "one"; case 2: case 3: return "two or three"; default: return "other"; } }
print(sw(1), sw(3), sw(9));
outer: for (var a = 0; a < 3; a++) { for (var b = 0; b < 3; b++) { if (b == 1) continue outer; if (a == 2) break outer; } }
print("labels", a, b);
// operators
print(typeof 1, typeof "s", typeof {}, typeof null, typeof undefined, typeof function () {}, typeof x_undefined);
print(7 % 3, -7 % 3, 2 ** 10, 1 / 0, -1 / 0, 0 / 0, 5 >>> 1, -5 >> 1, ~5, 1 << 31);
print(1 < 2, "a" < "b", null == undefined, null === undefined, NaN == NaN, "1" == 1);
print(0.1 + 0.2, 1e21, 123456789012345680000, 1.5e-7, -0, 100, 3.14159);
// classes
class A { constructor(x) { this.x = x; } get double() { return this.x * 2; } static make() { return new A(7); } }
class B extends A { constructor() { super(3); this.y = 1; } sum() { return this.x + this.y + super.constructor.name.length; } }
print("class", new A(2).double, A.make().x, new B().sum());
// destructuring, spread, default parameters
var [p, q = 5, ...rest] = [1, undefined, 3, 4];
var { u, v: w = 9, ...others } = { u: 1, z: 2, y: 3 };
print("destructure", p, q, rest.length, u, w, others.z, others.y);
function defs(a, b = a + 1, ...more) { return a + b + more.length; }
print("defaults", defs(1), defs(1, 2, 3, 4));
print("spread", Math === undefined ? "no Math" : "Math", [...[1, 2], ...[3]].length);
// template literals
var name = "Kosuzu";
print(`hello ${name}, ${1 + 1} books`);
// optional chaining and nullish
var n = null;
print(n?.x, n ?? "default", o?.b, o.zz?.yy);
