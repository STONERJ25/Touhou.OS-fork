// builtins smoke test
var a = [5, 1, 4, 2, 3];
print("sort", a.slice().sort().join(), a.slice().sort((x, y) => y - x).join("-"));
print("map/filter/reduce", a.map(x => x * 2).join(), a.filter(x => x & 1).join(), a.reduce((s, x) => s + x, 0));
print("splice", (function () { var b = [1, 2, 3, 4, 5]; var r = b.splice(1, 2, "a", "b", "c"); return b.join() + "|" + r.join(); })());
print("indexOf", a.indexOf(4), a.includes(3), a.lastIndexOf(9), [NaN].includes(NaN), [NaN].indexOf(NaN));
print("flat", [1, [2, [3, [4]]]].flat(Infinity).join(), [[1, 2], [3]].flatMap(x => x).length);
print("from", Array.from("abc").join(), Array.from({ length: 3 }, (_, i) => i * i).join(), Array.of(7, 8).join());
print("find", a.find(x => x > 3), a.findIndex(x => x > 3), a.findLast(x => x < 3), a.findLastIndex(x => x > 100));
print("misc", [1, 2, 3].reverse().join(), [1, 2, 3].fill(0, 1).join(), [1, 2, 3, 4, 5].copyWithin(0, 3).join());
print("toSorted", a.toSorted().join(), a.join(), a.with(0, 9).join(), a.toSpliced(1, 3).join(), a.toReversed().join());
print("iter", [...[1, 2].entries()].join(";"), [...[1, 2].keys()].join(), [..."héllo"].length);
var cyc = [1, 2]; cyc.push(cyc); print("cycle", String(cyc));
print("keys", Object.keys({ b: 1, a: 2, 1: 3, 0: 4 }).join(), Object.values({ x: 1, y: 2 }).join(), JSON === undefined);
print("assign", JSON === undefined ? "" : "", Object.entries(Object.assign({}, { p: 1 }, { q: 2 })).join(";"));
var fz = Object.freeze({ k: 1 }); print("frozen", Object.isFrozen(fz), Object.isSealed(fz), Object.isExtensible(fz));
var o = Object.create({ inherited: 1 }, { own: { value: 2, enumerable: true } });
print("create", o.inherited, o.own, Object.getPrototypeOf(o).inherited, o.hasOwnProperty("own"), Object.hasOwn(o, "inherited"));
print("descriptor", JSON === undefined, Object.getOwnPropertyDescriptor([1], "length").writable, Object.getOwnPropertyNames("ab").join());
print("tostring", Object.prototype.toString.call([]), Object.prototype.toString.call(null), String({}), `${[1, [2, 3]]}`);
print("groupBy", Object.keys(Object.groupBy([1, 2, 3, 4], x => x % 2 ? "odd" : "even")).join());
// strings
var s = "Hello, World";
print("string", s.length, s.toUpperCase(), s.toLowerCase(), s.indexOf("o"), s.lastIndexOf("o"), s.slice(-5), s.substring(7, 0));
print("split", "a,b,,c".split(",").length, "abc".split("").join("|"), s.split(", ", 1).join());
print("replace", "aaa".replace("a", "b"), "aaa".replaceAll("a", "$&$&"), "x-y".replace("-", (m, p) => "[" + p + "]"));
print("pad", "5".padStart(3, "0"), "5".padEnd(3, "ab"), "ab".repeat(3), "  x ".trim() + "|", "  x ".trimStart() + "|");
print("unicode", "straße".toUpperCase(), "İ".toLowerCase().length, "ΌΣΟΣ".toLowerCase(), "é".normalize("NFD").length, "é".normalize() === "é");
print("codepoints", "😀".length, "😀".codePointAt(0), String.fromCodePoint(0x1F600) === "😀", "abc".at(-1), "abc".charCodeAt(1));
print("compare", "a".localeCompare("b"), "b".localeCompare("a"), "a".localeCompare("A"), ["b", "a", "C"].sort((x, y) => x.localeCompare(y)).join());
print("raw", String.raw`a\nb${1}c`, "abc".startsWith("ab"), "abc".endsWith("bc"), "abc".includes("d"));
print("html", "x".bold(), "x".anchor('a"b'), "abc".substr(-2, 1));
// numbers
print("number", (255).toString(16), (0.1).toFixed(20), (1234.5678).toFixed(2), (123.456).toExponential(2), (0.00001).toPrecision(1));
print("number2", Number("0x1F"), Number(" 12 "), Number("1e3"), Number(""), Number("abc"), parseInt("08"), parseFloat("3.14abc"));
print("number3", Number.isInteger(5.0), Number.isSafeInteger(2 ** 53), Number.MAX_SAFE_INTEGER, Number.EPSILON > 0, Number.parseFloat === parseFloat);
print("locale", (1234567.891).toLocaleString(), (-0.5).toLocaleString());
print("math", Math.max(1, 3, 2), Math.min(), Math.round(-2.5), Math.round(2.5), Math.sign(-3), Math.trunc(-4.7), Math.hypot(3, 4));
print("math2", Math.sqrt(2), Math.cbrt(27), Math.abs(-5), Math.floor(-0.5), Math.ceil(0.5), Math.clz32(1), Math.imul(3, -4), Math.fround(5.5));
print("math3", Math.sin(Math.PI / 2), Math.cos(Math.PI), Math.atan2(1, 1), Math.exp(1), Math.log(Math.E), Math.pow(2, 0.5), Math.log10(1000));
var r = Math.random(); print("random", r >= 0 && r < 1);
// symbols
var sym = Symbol("desc");
print("symbol", sym.toString(), sym.description, Symbol.for("k") === Symbol.for("k"), Symbol.keyFor(Symbol.for("k")), typeof Symbol.iterator);
var so = {}; so[sym] = 1; print("symkeys", Object.getOwnPropertySymbols(so).length, Object.keys(so).length);
// Map and Set
var m = new Map([[1, "one"], ["1", "string one"], [NaN, "nan"]]);
m.set(-0, "zero");
print("map", m.size, m.get(1), m.get("1"), m.get(NaN), m.get(0), m.has(2), [...m.keys()].join());
m.delete(1); print("map2", m.size, [...m].map(e => e[0]).join(), m.get(1));
var st = new Set([1, 2, 2, 3]);
print("set", st.size, [...st].join(), st.has(2), [...st.entries()].join(";"));
var it = st.values(); it.next(); st.delete(2); st.add(4);
print("set-iter", [...it].join());
print("set-ops", [...new Set([1, 2, 3]).union(new Set([3, 4]))].join(), [...new Set([1, 2, 3]).intersection(new Set([2, 3, 9]))].join(),
      new Set([1]).isSubsetOf(new Set([1, 2])));
var wm = new WeakMap(); var key = {}; wm.set(key, 5); print("weakmap", wm.get(key), wm.has({}), wm.delete(key), wm.has(key));
var gm = Map.groupBy([1, 2, 3], x => x % 2); print("map-groupBy", gm.get(1).join(), gm.get(0).join());
var forEachLog = []; m.forEach((v, k) => forEachLog.push(k)); print("map-forEach", forEachLog.join());
