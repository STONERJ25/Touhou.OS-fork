// BigInt (ECMA-262 6.1.6.2, 7.1.13-7.1.14, 13.15.3, 21.2) and the BigInt64 typed arrays.  The arithmetic itself is checked against
// Python's integers by tests/host/test_js_bigint.py; this file is about the language around it: literals, conversions, mixing
// rules, comparisons, errors, the constructor and prototype, collections, property keys, JSON, typed arrays, DataView, Atomics.
// Self-checking: every check states the expected value; the file prints its failures and a summary.
var total = 0, fails = 0;
function repr(v) {
  if (typeof v === "bigint") return v + "n";
  if (typeof v === "string") return JSON.stringify(v);
  if (typeof v === "symbol") return v.toString();
  if (Array.isArray(v)) return "[" + v.map(repr).join(",") + "]";
  if (ArrayBuffer.isView(v) && !(v instanceof DataView)) return v.constructor.name + "[" + Array.prototype.map.call(v, repr).join(",") + "]";
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

// ---- literals
eq([typeof 1n, typeof 0n, typeof -1n, typeof Object(1n), typeof BigInt(1)], ["bigint", "bigint", "bigint", "object", "bigint"], "typeof");
eq([0xFFn, 0XaBn, 0b1010n, 0o777n, 1_000_000n, 0x1_0n], [255n, 171n, 10n, 511n, 1000000n, 16n], "literal forms");
eq([0n === -0n, 0n === 0x0n, 123456789012345678901234567890n.toString()], [true, true, "123456789012345678901234567890"], "zero and long literals");
eq(340282366920938463463374607431768211456n, 2n ** 128n, "a literal of five limbs");
eq([!0n, !1n, !!-1n, 0n ? "t" : "f", 1n ? "t" : "f", 0n || "d", 0n ?? "d", 1n && 2n], [true, false, true, "f", "t", "d", 0n, 2n], "truthiness");

// ---- equality and comparison
eq([1n == 1, 1n === 1, 1n == "1", "1" == 1n, 1n == "1.5", 0n == "", 0n == false, 1n == true, 2n == true, null == 0n, undefined == 0n, 1n == Object(1n)],
   [true, false, true, true, false, true, true, true, false, false, false, true], "== and ===");
eq([1n == { valueOf: function () { return 1n; } }, 1n == { valueOf: function () { return 1; } }, "10" == 10n, 10n == " 10 ", 10n == "0xa", 10n == "10n"],
   [true, true, true, true, true, false], "== with objects and strings");
eq([1n < 2, 2 > 1n, 1n < 2n, 1n < "2", "3" > 2n, 1n < "x", 1n >= "x", 1n < NaN, 1n >= NaN, 10n > "9", "10" < "9", 1n <= 1, 1n >= 1.5, 2n > 1.5],
   [true, true, true, true, true, false, false, false, false, true, true, true, false, true], "relational operators across types");
eq([2n ** 53n + 1n > 2 ** 53, 2n ** 53n + 1n == 2 ** 53, 2n ** 64n == 2 ** 64, 2n ** 1024n < Infinity, -(2n ** 1024n) > -Infinity, 2n ** 1024n == Infinity, 5n < 5.5, 5n > 4.5, -5n < -4.5],
   [true, false, true, true, true, false, true, true, true], "exact comparison with Numbers");
eq([Object.is(0n, -0n), Object.is(1n, 1n), Object.is(1n, 1), [0n].includes(-0n), 1n !== 1n, 2n ** 64n === 2n ** 64n], [true, true, false, true, false, true], "SameValue and strict equality");
eq([10n, 9n, 1n].sort().join(), "1,10,9", "the default sort compares strings");
eq([10n, 9n, 1n, -5n].sort(function (a, b) { return a < b ? -1 : a > b ? 1 : 0; }).join(), "-5,1,9,10", "sort with a comparator");
eq(Math.max.apply(null, [1, 2]) > 1n, true, "Number against BigInt");

// ---- arithmetic semantics
eq([7n / 2n, -7n / 2n, 7n / -2n, -7n / -2n, 7n % 2n, -7n % 2n, 7n % -2n, -7n % -2n], [3n, -3n, -3n, 3n, 1n, -1n, 1n, -1n], "division truncates, the remainder follows the dividend");
eq([2n ** 0n, 0n ** 0n, 0n ** 5n, (-2n) ** 3n, (-2n) ** 4n, 2n ** 3n ** 2n, 10n ** 20n], [1n, 1n, 0n, -8n, 16n, 512n, 100000000000000000000n], "exponentiation");
eq([(-1n) ** (2n ** 100n), (-1n) ** (2n ** 100n + 1n), 1n ** (2n ** 200n), 0n ** (2n ** 200n), 2n ** 100n, (-2n) ** 101n], [1n, -1n, 1n, 0n, 1267650600228229401496703205376n, -2535301200456458802993406410752n], "exponents too big for a result stay cheap for 0, 1 and -1");
eq([5n + 3n, 5n - 8n, 6n * -7n, -(-5n), -0n, 0n * -5n, 0n - 0n, 5n - 5n], [8n, -3n, -42n, 5n, 0n, 0n, 0n, 0n], "basic operators");
eq([1n << 70n, (1n << 70n) >> 69n, -1n >> 100n, -5n >> 1n, 5n >> 1n, 1n << -1n, 255n >> -4n, 5n >> (2n ** 100n), -5n >> (2n ** 100n), 0n << (2n ** 100n)],
   [1180591620717411303424n, 2n, -1n, -3n, 2n, 0n, 4080n, 0n, -1n, 0n], "shifts, negative counts, enormous counts");
eq([5n & 3n, 5n | 3n, 5n ^ 3n, -5n & 3n, -5n | 3n, -5n ^ 3n, ~5n, ~-5n, ~0n], [1n, 7n, 6n, 3n, -5n, -8n, -6n, 4n, -1n], "bitwise operators");
var i = 5n;
i++; ++i; i--;
eq([i, i++, i, ++i, i--, --i, i += 10n, i *= 2n, i **= 2n, i %= 1000n, i <<= 3n, i >>= 2n, i &= 0xFn, i |= 0x10n, i ^= 3n, i /= 2n, i -= 1n],
   [6n, 6n, 7n, 8n, 8n, 6n, 16n, 32n, 1024n, 24n, 192n, 48n, 0n, 16n, 19n, 9n, 8n], "update and compound assignment");
var f = 1n;
for (var k = 1n; k <= 25n; k++) f *= k;
eq(f, 15511210043330985984000000n, "25!");
var fa = 0n, fb = 1n;
for (var n = 0; n < 300; n++) { var t = fa + fb; fa = fb; fb = t; }
eq(fa, 222232244629420445529739893461909967206666939096499764990979600n, "the 300th Fibonacci number");
eq([3n ** 200n, 2n ** 127n - 1n], [265613988875874769338781322035779626829233452653394495974574961739092490901302182994384699044001n, 170141183460469231731687303715884105727n], "powers");
eq((10n ** 40n) / (10n ** 20n + 7n), 99999999999999999993n, "a quotient of long numbers");
eq(((1n << 200n) - 1n) % ((1n << 64n) + 13n) < (1n << 64n) + 13n, true, "a remainder is smaller than the divisor");
var gcd = function (a, b) { while (b) { var r = a % b; a = b; b = r; } return a; };
eq(gcd(2n ** 100n - 1n, 2n ** 60n - 1n), 1048575n, "Euclid's algorithm on big numbers");

// ---- mixing and the operators that do not exist for BigInt
throws(function () { return 1n + 1; }, TypeError, "1n + 1");
throws(function () { return 1 + 1n; }, TypeError, "1 + 1n");
throws(function () { return 1n * 1.5; }, TypeError, "1n * 1.5");
throws(function () { return 1n - "1"; }, TypeError, "1n - '1'");
throws(function () { return 2n ** 2; }, TypeError, "2n ** 2");
throws(function () { return 1n & 1; }, TypeError, "1n & 1");
throws(function () { return 1n << 1; }, TypeError, "1n << 1");
throws(function () { return +1n; }, TypeError, "unary plus");
throws(function () { return 1n >>> 0n; }, TypeError, ">>> on BigInts");
throws(function () { var x = 1n; x >>>= 1n; }, TypeError, ">>>=");
throws(function () { return Math.sqrt(4n); }, TypeError, "Math.sqrt(4n)");
throws(function () { return Math.max(1n, 2n); }, TypeError, "Math.max(1n, 2n)");
throws(function () { return isNaN(1n); }, TypeError, "isNaN(1n)");
throws(function () { return new Date(1n); }, TypeError, "new Date(1n)");
throws(function () { return [].concat(1n).length + 1n; }, TypeError, "a length plus a BigInt");
throws(function () { return 1n / 0n; }, RangeError, "division by zero");
throws(function () { return 1n % 0n; }, RangeError, "remainder by zero");
throws(function () { return 2n ** -1n; }, RangeError, "negative exponent");
throws(function () { return 2n ** (2n ** 40n); }, RangeError, "a result too big");
throws(function () { return 1n << (2n ** 40n); }, RangeError, "a shift too big");
throws(function () { return (-3n) ** (2n ** 33n); }, RangeError, "a negative base, too big");
eq(1n + "x" + 2n, "1x2", "+ with a string is concatenation");
eq([`${10n}`, "a" + 1n, [1n, 2n].join("-"), String(-0n), (0n).toString(), (-0n).toString(), [1n] + ""], ["10", "a1", "1-2", "0", "0", "0", "1"], "strings from BigInts");
eq([Number(1n), Number(-5n), Object.is(Number(-0n), 0), Number("10n"), parseInt(10n), parseFloat("10n"), Number.isNaN(1n), Number.isInteger(1n), Number.isSafeInteger(1n)],
   [1, -5, true, NaN, 10, 10, false, false, false], "Number() and the Number functions");

// ---- Number(bigint) rounds to nearest, ties to even
eq([Number(2n ** 53n + 1n) === 2 ** 53, Number(2n ** 53n + 3n) === 2 ** 53 + 4, Number(2n ** 53n + 2n) === 2 ** 53 + 2, Number(-(2n ** 53n) - 1n) === -(2 ** 53), Number(2n ** 64n - 1n) === 2 ** 64],
   [true, true, true, true, true], "rounding to a double");
eq([Number(2n ** 1024n), Number(2n ** 1024n - 1n), Number(2n ** 1024n - 2n ** 970n), Number(2n ** 1024n - 2n ** 970n - 1n), Number(-(2n ** 1024n))],
   [Infinity, Infinity, Infinity, 1.7976931348623157e308, -Infinity], "the edge of the doubles");
eq([Number((1n << 100n) + (1n << 47n)) === 2 ** 100, Number((1n << 100n) + (1n << 47n) + 1n) === 2 ** 100 + 2 ** 48, Number(12345678901234567890123n)], [true, true, 1.2345678901234568e22], "sticky bits decide a tie");

// ---- BigInt() and the conversions
eq([BigInt(10), BigInt("0x10"), BigInt(" 12 "), BigInt(""), BigInt("   "), BigInt(true), BigInt(false), BigInt("-10"), BigInt("+10"), BigInt("0b101"), BigInt("0O17"), BigInt("007"), BigInt(" 10 ")],
   [10n, 16n, 12n, 0n, 0n, 1n, 0n, -10n, 10n, 5n, 15n, 7n, 10n], "BigInt() from primitives");
eq([BigInt([]), BigInt([7]), BigInt({ valueOf: function () { return 5; } }), BigInt({ toString: function () { return "0x20"; }, valueOf: null }), BigInt(Object(3n)), BigInt(new String("9"))],
   [0n, 7n, 5n, 32n, 3n, 9n], "BigInt() from objects");
eq([BigInt(1e21), BigInt(2 ** 53 + 2), BigInt(-0), BigInt(-1e21), BigInt(Number.MAX_VALUE) % 1000n, BigInt(2 ** 64)], [10n ** 21n, 9007199254740994n, 0n, -(10n ** 21n), 368n, 18446744073709551616n], "BigInt() from integral Numbers");
throws(function () { BigInt(1.5); }, RangeError, "BigInt(1.5)");
throws(function () { BigInt(NaN); }, RangeError, "BigInt(NaN)");
throws(function () { BigInt(Infinity); }, RangeError, "BigInt(Infinity)");
throws(function () { BigInt(Number.MIN_VALUE); }, RangeError, "BigInt(5e-324)");
throws(function () { BigInt(undefined); }, TypeError, "BigInt(undefined)");
throws(function () { BigInt(); }, TypeError, "BigInt()");
throws(function () { BigInt(null); }, TypeError, "BigInt(null)");
throws(function () { BigInt(Symbol()); }, TypeError, "BigInt(Symbol())");
throws(function () { new BigInt(1); }, TypeError, "new BigInt(1)");
throws(function () { BigInt("1.5"); }, SyntaxError, "BigInt('1.5')");
throws(function () { BigInt("1e3"); }, SyntaxError, "BigInt('1e3')");
throws(function () { BigInt("1_0"); }, SyntaxError, "BigInt('1_0')");
throws(function () { BigInt("1n"); }, SyntaxError, "BigInt('1n')");
throws(function () { BigInt("0x"); }, SyntaxError, "BigInt('0x')");
throws(function () { BigInt("-0x10"); }, SyntaxError, "BigInt('-0x10')");
throws(function () { BigInt("+0b1"); }, SyntaxError, "BigInt('+0b1')");
throws(function () { BigInt("0x1g"); }, SyntaxError, "BigInt('0x1g')");
throws(function () { BigInt("abc"); }, SyntaxError, "BigInt('abc')");
throws(function () { BigInt("Infinity"); }, SyntaxError, "BigInt('Infinity')");
throws(function () { BigInt("- 1"); }, SyntaxError, "BigInt('- 1')");
throws(function () { BigInt({}); }, SyntaxError, "BigInt({})");
eq([BigInt("9".repeat(1000)) + 1n === 10n ** 1000n, BigInt("0x" + "f".repeat(100)) === 16n ** 100n - 1n, BigInt("0b" + "1".repeat(200)) === 2n ** 200n - 1n, (10n ** 5000n).toString().length], [true, true, true, 5001], "long strings");

// ---- BigInt.asIntN and BigInt.asUintN
eq([BigInt.asIntN(8, 255n), BigInt.asIntN(8, 128n), BigInt.asIntN(8, 127n), BigInt.asIntN(8, -129n), BigInt.asIntN(8, -128n), BigInt.asIntN(3, 4n), BigInt.asIntN(1, 1n), BigInt.asIntN(1, 2n), BigInt.asIntN(0, 5n)],
   [-1n, -128n, 127n, 127n, -128n, -4n, -1n, 0n, 0n], "asIntN");
eq([BigInt.asUintN(8, -1n), BigInt.asUintN(8, 256n), BigInt.asUintN(64, -1n), BigInt.asUintN(1, 3n), BigInt.asUintN(0, 5n), BigInt.asUintN(32, -(2n ** 32n)), BigInt.asUintN(33, -1n)],
   [255n, 0n, 18446744073709551615n, 1n, 0n, 0n, 8589934591n], "asUintN");
eq([BigInt.asIntN(64, 2n ** 63n), BigInt.asIntN(65, 2n ** 63n), BigInt.asIntN(64, -(2n ** 63n) - 1n), BigInt.asUintN(200, -1n) === 2n ** 200n - 1n, BigInt.asIntN(200, 2n ** 199n) === -(2n ** 199n), BigInt.asIntN(200, 2n ** 199n - 1n) === 2n ** 199n - 1n],
   [-9223372036854775808n, 9223372036854775808n, 9223372036854775807n, true, true, true], "asIntN and asUintN across limbs");
eq([BigInt.asUintN(2 ** 53 - 1, 5n), BigInt.asIntN(2 ** 53 - 1, -5n), BigInt.asIntN("8", "255"), BigInt.asUintN(8.9, 257n)], [5n, -5n, -1n, 1n], "huge widths, converted arguments");
throws(function () { BigInt.asUintN(2 ** 53 - 1, -5n); }, RangeError, "a negative number needs as many bits as the width");
throws(function () { BigInt.asIntN(-1, 0n); }, RangeError, "negative width");
throws(function () { BigInt.asIntN(2 ** 53, 0n); }, RangeError, "width beyond 2^53 - 1");
throws(function () { BigInt.asUintN(1, 1); }, TypeError, "a Number for the BigInt");

// ---- the constructor and the prototype
eq([BigInt.name, BigInt.length, BigInt.asIntN.length, BigInt.asUintN.length, BigInt.prototype.toString.length, BigInt.prototype.toLocaleString.length, BigInt.prototype.valueOf.length], ["BigInt", 1, 2, 2, 0, 0, 0], "names and lengths");
eq([Object.prototype.toString.call(1n), BigInt.prototype[Symbol.toStringTag], 1n.constructor === BigInt, Object.getPrototypeOf(1n) === BigInt.prototype, BigInt.prototype.constructor === BigInt, Object.getPrototypeOf(BigInt) === Function.prototype],
   ["[object BigInt]", "BigInt", true, true, true, true], "identity");
var tag = Object.getOwnPropertyDescriptor(BigInt.prototype, Symbol.toStringTag);
eq([tag.value, tag.writable, tag.enumerable, tag.configurable], ["BigInt", false, false, true], "the tag's attributes");
eq(Object.getOwnPropertyNames(BigInt.prototype).sort().join(), "constructor,toLocaleString,toString,valueOf", "BigInt.prototype's names");
eq(Object.getOwnPropertyNames(BigInt).sort().join(), "asIntN,asUintN,length,name,prototype", "BigInt's names");
eq([(255n).toString(16), (255n).toString(2), (-255n).toString(36), (35n).toString(36), (2n ** 64n).toString(16), (-255n).toString(2), (2n ** 200n).toString(36), (2n ** 200n).toString(16), (0n).toString(2), (10n).toString(undefined)],
   ["ff", "11111111", "-73", "z", "10000000000000000", "-11111111", "bnklg118comha6gqury14067gur54n8won6guf4", "1" + "0".repeat(50), "0", "10"], "toString(radix)");
eq((2n ** 200n).toString(5), "111020132102420112021343001342032333120043341314112104422342034202402044234211314121001", "toString(5) of a long number");
eq([(1234567n).toLocaleString(), (-1234n).toLocaleString(), (123n).toLocaleString(), (0n).toLocaleString(), (1234567890123456789012n).toLocaleString()], ["1,234,567", "-1,234", "123", "0", "1,234,567,890,123,456,789,012"], "toLocaleString");
eq([(5n).valueOf() === 5n, Object(5n).valueOf() === 5n, BigInt.prototype.toString.call(Object(7n)), typeof Object(1n).valueOf()], [true, true, "7", "bigint"], "valueOf and wrappers");
eq([Object(1n) + 1n, Object(1n) == 1n, Object(1n) === 1n, Object(1n) instanceof BigInt, 1n instanceof BigInt, typeof Object(1n)], [2n, true, false, true, false, "object"], "wrapper objects");
throws(function () { (1n).toString(1); }, RangeError, "radix 1");
throws(function () { (1n).toString(37); }, RangeError, "radix 37");
throws(function () { BigInt.prototype.toString.call(1); }, TypeError, "toString on a Number");
throws(function () { BigInt.prototype.valueOf.call("1"); }, TypeError, "valueOf on a String");
throws(function () { BigInt.prototype.valueOf.call(BigInt.prototype); }, TypeError, "BigInt.prototype is not a BigInt");
throws(function () { Number.prototype.toString.call(1n); }, TypeError, "Number.prototype.toString on a BigInt");
throws(function () { class B extends BigInt {} new B(); }, TypeError, "a subclass cannot be constructed");
BigInt.prototype.double = function () { return this * 2n; };
eq(21n.double(), 42n, "methods added to BigInt.prototype are seen by primitives");
delete BigInt.prototype.double;

// ---- collections, property keys, JSON
var m = new Map([[1n, "a"], [1, "b"], [2n ** 64n, "big"]]);
eq([m.size, m.get(1n), m.get(1), m.get(2n ** 64n), m.has(2n), new Set([0n, -0n]).size, new Set([2n ** 64n, 2n ** 64n, 1n, 1n]).size], [3, "a", "b", "big", false, 1, 2], "Map and Set use value equality");
eq([[1n].includes(1n), [1n].indexOf(1n), [1n, 2n].lastIndexOf(2n), [1n].includes(1), [NaN, 1n].findIndex(function (x) { return x === 1n; })], [true, 0, 1, false, 1], "Array searches");
var keyed = { 1: "x", "18446744073709551616": "y" };
eq([keyed[1n], keyed[2n ** 64n], "abc"[1n], [10, 20][1n], Object.keys({ [5n]: 1 }).join(), (5n in [0, 1, 2, 3, 4, 5]), 2n in [1, 2, 3]], ["x", "y", "b", 20, "5", true, true], "BigInts as property keys");
eq(new Array(1n).length, 1, "Array(1n) makes an array holding it");
throws(function () { JSON.stringify(1n); }, TypeError, "JSON.stringify(1n)");
throws(function () { JSON.stringify({ a: [1n] }); }, TypeError, "JSON.stringify of a nested BigInt");
throws(function () { JSON.stringify(Object(1n)); }, TypeError, "JSON.stringify of a BigInt wrapper");
eq(JSON.stringify({ a: 1n }, function (k, v) { return typeof v === "bigint" ? v + "" : v; }), '{"a":"1"}', "a replacer");
BigInt.prototype.toJSON = function () { return this.toString() + "n"; };
eq([JSON.stringify(1n), JSON.stringify({ a: 2n }), JSON.stringify([3n])], ['"1n"', '{"a":"2n"}', '["3n"]'], "toJSON on BigInt.prototype");
delete BigInt.prototype.toJSON;
eq(JSON.parse("12345678901234567890"), 12345678901234567000, "JSON.parse has no BigInt");

// ---- BigInt64Array and BigUint64Array
var a = new BigInt64Array(3);
a[0] = 1n; a[1] = -1n; a[2] = 2n ** 63n;
var u = new BigUint64Array(a.buffer);
eq([a, u], [BigInt64Array.of(1n, -1n, -(2n ** 63n)), BigUint64Array.of(1n, 2n ** 64n - 1n, 2n ** 63n)], "stores wrap, views reinterpret");
eq([BigInt64Array.BYTES_PER_ELEMENT, BigUint64Array.BYTES_PER_ELEMENT, a.byteLength, a.BYTES_PER_ELEMENT, a.constructor.name, Object.prototype.toString.call(u)], [8, 8, 24, 8, "BigInt64Array", "[object BigUint64Array]"], "sizes and names");
a[0] = "12"; a[1] = true;
eq([a[0], a[1]], [12n, 1n], "strings and booleans convert like BigInt()");
a[5] = 7n;
eq([a[5], a.length, 5 in a], [undefined, 3, false], "out of range");
throws(function () { a[0] = 1; }, TypeError, "a Number into a BigInt64Array");
throws(function () { a[0] = "x"; }, SyntaxError, "a bad string");
throws(function () { a.fill(1); }, TypeError, "fill with a Number");
throws(function () { new BigInt64Array([1]); }, TypeError, "from an array of Numbers");
throws(function () { new BigInt64Array(new Uint8Array(2)); }, TypeError, "from a Number array");
throws(function () { new Uint8Array(new BigInt64Array(2)); }, TypeError, "a Number array from a BigInt array");
throws(function () { a.set(new Uint8Array(1)); }, TypeError, "set from a Number array");
throws(function () { new BigInt64Array(new ArrayBuffer(7)); }, RangeError, "length not a multiple of 8");
var b = BigInt64Array.from([3n, -1n, 2n]);
eq([b.map(function (x) { return x * 2n; }), b.filter(function (x) { return x > 0n; }), b.reduce(function (s, x) { return s + x; }, 0n), b.join(), b.toString(), b.includes(2n), b.includes(2), b.indexOf(-1n), b.indexOf(-1)],
   [BigInt64Array.of(6n, -2n, 4n), BigInt64Array.of(3n, 2n), 4n, "3,-1,2", "3,-1,2", true, false, 1, -1], "methods");
eq([b.slice().sort(), b.toSorted(), b.toReversed(), b.with(0, 9n), b.slice().fill(5n, 1), b.subarray(1), b.at(-1)], [BigInt64Array.of(-1n, 2n, 3n), BigInt64Array.of(-1n, 2n, 3n), BigInt64Array.of(2n, -1n, 3n), BigInt64Array.of(9n, -1n, 2n), BigInt64Array.of(3n, 5n, 5n), BigInt64Array.of(-1n, 2n), 2n], "sorting and copying");
eq(BigUint64Array.of(3n, 2n ** 64n - 1n, 0n, 2n ** 63n).sort(), BigUint64Array.of(0n, 3n, 2n ** 63n, 2n ** 64n - 1n), "unsigned sort");
eq([...BigInt64Array.of(1n, 2n)], [1n, 2n], "iteration");
b.set([7n, 8n], 1);
eq(b, BigInt64Array.of(3n, 7n, 8n), "set from an array of BigInts");
eq(Reflect.ownKeys(BigInt64Array.of(1n, 2n)), ["0", "1"], "keys");

// ---- DataView and Atomics
var dv = new DataView(new ArrayBuffer(16));
dv.setBigInt64(0, -2n);
dv.setBigUint64(8, 2n ** 64n - 1n, true);
eq([dv.getBigInt64(0), dv.getBigUint64(0), dv.getBigInt64(8, true), dv.getBigUint64(8, true), dv.getBigUint64(8), dv.getUint8(0)], [-2n, 18446744073709551614n, -1n, 18446744073709551615n, 18446744073709551615n, 255], "DataView with BigInts");
dv.setBigInt64(0, 2n ** 64n + 5n, true);
eq([dv.getBigInt64(0, true), dv.getUint8(0), dv.getUint8(7)], [5n, 5, 0], "stores wrap");
throws(function () { dv.setBigInt64(0, 1); }, TypeError, "a Number into setBigInt64");
throws(function () { dv.getBigInt64(9); }, RangeError, "reading past the end");
eq(Object.getOwnPropertyNames(DataView.prototype).filter(function (n) { return /^(get|set)/.test(n); }).length, 20, "ten get and ten set methods");
var ia = new BigInt64Array(new SharedArrayBuffer(16));
eq([Atomics.add(ia, 0, 5n), Atomics.sub(ia, 0, 2n), Atomics.load(ia, 0), Atomics.store(ia, 1, 7n), Atomics.and(ia, 0, 1n), Atomics.or(ia, 0, 6n), Atomics.xor(ia, 0, 2n), Atomics.exchange(ia, 0, 9n), Atomics.compareExchange(ia, 0, 9n, 1n), Atomics.compareExchange(ia, 0, 9n, 2n), ia[0]],
   [0n, 5n, 3n, 7n, 3n, 1n, 7n, 5n, 9n, 1n, 1n], "Atomics on a BigInt64Array");
eq([Atomics.store(ia, 0, 2n ** 70n), ia[0], Atomics.isLockFree(8)], [2n ** 70n, 0n, true], "Atomics.store returns the converted BigInt, the element wraps");
throws(function () { Atomics.add(ia, 0, 1); }, TypeError, "a Number for a BigInt64Array");
eq([Atomics.wait(ia, 0, 0n, 0), Atomics.wait(ia, 0, 1n, 0)], ["timed-out", "not-equal"], "Atomics.wait on a BigInt64Array");
throws(function () { Atomics.wait(ia, 0, 0, 0); }, TypeError, "wait: a Number for a BigInt64Array");

print("bigint: " + total + " checks, " + fails + " failures");
