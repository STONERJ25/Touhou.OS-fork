// Small enough to run with the collector forced at every allocation (jstool --stress): every value here must stay
// rooted across allocations, so a missing root shows up as a wrong result or an ASan report.
function make(n) { var a = []; for (var i = 0; i < n; i++) a.push({ i: i, s: "v" + i, f: function () { return i; } }); return a; }
var objs = make(20);
print("objects", objs.length, objs[7].s, objs[19].f(), objs.map(o => o.i).reduce((x, y) => x + y, 0));
var s = ""; for (var k = 0; k < 30; k++) s += String.fromCharCode(65 + k % 26); print("string", s.length, s.slice(0, 5));
var m = new Map(); for (var j = 0; j < 15; j++) m.set("k" + j, [j, { j }]); m.delete("k3");
print("map", m.size, m.get("k14")[1].j, [...m.keys()].slice(0, 3).join());
var st = new Set("abracadabra"); print("set", [...st].join(""));
var json = JSON.stringify({ a: [1, "two", { three: 3 }], b: null, c: "é" });
print("json", json, JSON.parse(json).a[2].three);
function* gen(n) { for (let i = 0; i < n; i++) yield [i, i * i]; }
print("generator", [...gen(6)].map(([a, b]) => a + ":" + b).join(" "));
class Animal { constructor(name) { this.name = name; } speak() { return this.name + " makes a sound"; } }
class Dog extends Animal { #tricks = []; teach(t) { this.#tricks.push(t); return this; } speak() { return super.speak() + " and knows " + this.#tricks.join("+"); } }
print("class", new Dog("Rex").teach("sit").teach("roll").speak());
var { x, y: [p, ...q], ...rest } = { x: 1, y: [2, 3, 4], z: 5, w: 6 };
print("destructure", x, p, q.join(), Object.keys(rest).join());
try { null.prop; } catch (e) { print("caught", e instanceof TypeError, typeof e.stack); }
var sorted = [5, 3, 9, 1, 7, 2, 8].sort((a, b) => a - b); print("sort", sorted.join());
print("tagged", ((s, ...v) => s.raw.join("|") + v.join(","))`a${1}b${2}c`);
var wm = new WeakMap(); var key = {}; wm.set(key, "kept"); print("weakmap", wm.get(key));
async function chain(v) { var a = await v; var b = await Promise.all([a, a + 1]); return b.join("&"); }
chain(Promise.resolve(10)).then(r => print("async", r));
Promise.reject(new Error("handled")).catch(e => print("rejection", e.message));
print("normalize", "é".normalize("NFC").length, "ﬁ".normalize("NFKD"));
