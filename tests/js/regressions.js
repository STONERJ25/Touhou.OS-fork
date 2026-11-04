// Regressions for engine bugs that test262 found (each check names the bug).  Self-checking: prints its failures and a summary.
var total = 0, fails = 0;
function repr(v) {
  if (typeof v === "string") return JSON.stringify(v);
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

// ---- property names that spell an array index are index keys, whatever way they are written.  The strings "0".."9" are
// ---- atoms of the string table, and an atom used to be taken as the key itself, so {"0": 2} had a property that o[0] never saw.
var o = { foo: 1, "0": 2, bar: 3, "1": 4, "10": 5, 7: 6 };
eq(Object.keys(o), ["0", "1", "7", "10", "foo", "bar"], "string-literal index keys sort as indexes");
eq([o[0], o["0"], o[1], o[10], o["7"]], [2, 2, 4, 5, 6], "string-literal index keys read back both ways");
eq([0 in o, "1" in o, o.hasOwnProperty(7), Object.hasOwn(o, "10")], [true, true, true, true], "in / hasOwn");
o[0] = 9;
eq(Object.keys(o).length, 6, "assigning o[0] replaces, it does not add");
eq(delete o["1"], true, "delete by string");
eq(Object.keys(o), ["0", "7", "10", "foo", "bar"], "deleted");
eq(JSON.stringify({ "2": "b", "1": "a", x: 1 }), '{"1":"a","2":"b","x":1}', "JSON.stringify order");
var q = { ["7"]: 1, 8: 2, "9"() { return 3; }, get "10"() { return 4; }, set "11"(v) { this.stored = v; } };
q[11] = 5;
eq([Object.keys(q), q[7], q[9](), q[10], q.stored], [["7", "8", "9", "10", "11", "stored"], 1, 3, 4, 5], "methods and accessors with string-literal index names");
eq([q[9].name, Object.getOwnPropertyDescriptor(q, "10").get.name], ["9", "get 10"], "their function names");
class C {
  static "0" = 1; static 1 = 2; "2" = 3; static "3"() { return 4; } get "4"() { return 5; } static get "5"() { return 6; }
}
eq([C[0], C["1"], C[3](), C[5], Object.keys(new C()), new C()[4]], [1, 2, 4, 6, ["2"], 5], "class elements with index names");
eq(Object.getOwnPropertyNames(C).slice(0, 4), ["0", "1", "3", "5"], "class static index keys come first");
// a function name that looks like an index is a string: using it as a key must give the index key again
var f = ({ 12() {} })[12];
var t = {}; t[f.name] = "x";
eq([Object.keys(t), t[12]], [["12"], "x"], "an atom made for a function name is still the index key");
var obj = {}; obj["5"] = 1; obj[5] += 1;
eq([obj[5], obj["5"], Object.keys(obj)], [2, 2, ["5"]], "computed string keys");
var arr = ["a", "b", "c"]; var digit = "1";
eq([arr[digit], arr["2"], digit in arr, arr.hasOwnProperty(digit)], ["b", "c", true, true], "array elements by one-character strings");
var sk = "abc"[1];
var tmp = [10, 20, 30];
eq(tmp[sk === "b" ? "1" : "0"], 20, "index string from a conditional");
eq(Object.entries({ 1: "a", "0": "b" }), [["0", "b"], ["1", "a"]], "entries");
eq(Reflect.ownKeys({ b: 1, 1: 1, a: 1, 0: 1 }), ["0", "1", "b", "a"], "ownKeys order");

// ---- an arrow function with a block body at script top level may contain return
var arrow = (x) => { if (x) return x * 2; return -1; };
eq([arrow(4), arrow(0)], [8, -1], "return inside an arrow at top level");
var arrow2 = async () => { return 1; };
var arrow3 = (a, b = 2) => { return a + b; };
eq(arrow3(1), 3, "arrow with defaults");

// ---- eval code takes the this binding of the function it is in, not the one of an enclosing function
function evalThis() { return eval("this"); }
var host = { m: evalThis };
eq(host.m() === host, true, "this in a direct eval is the callee's this");
function outer() {
  var self = this;
  function inner() { return eval("this"); }
  return [inner.call("inner") instanceof String, inner.call({ k: 1 }).k, self === inner.call(self)];
}
eq(outer.call({}), [true, 1, true], "a nested function with eval has its own this");
function evalNewTarget() { return eval("new.target"); }
eq([new evalNewTarget() === evalNewTarget, evalNewTarget()], [true, undefined], "new.target in a direct eval");
var base = { greet() { return "base"; } };
var derived = { __proto__: base, greet() { return "derived+" + eval("super.greet()"); } };
eq(derived.greet(), "derived+base", "super in a direct eval inside a method");
var arrowEval = function () { return (() => eval("this"))(); };
eq(arrowEval.call(host) === host, true, "an arrow function's eval sees the enclosing this");
class EvalCtor { constructor() { this.t = eval("this"); } }
var ec = new EvalCtor();
eq(ec.t === ec, true, "this in eval in a constructor");

// ---- super in direct eval.  super.x used to read the home object through the wrong closure cell (a class's instance methods
// ---- got the constructor's prototype chain, not the prototype's), and super() in eval was a SyntaxError.
class SA { constructor(x) { this.x = x; this.nt = new.target.name; } sm() { return "SA.sm"; } }
SA.prototype.mark = "SA-proto";
class SB extends SA {
  t() { return [eval("super.mark"), eval("super['mark']"), eval("(() => super.mark)()"), super.mark]; }
  c() { return eval("super.sm()"); }
  static s() { return eval("super.name"); }
}
SB.prototype.mark = "SB-proto";
var sb = new SB(1);
sb.mark = "own";
eq(sb.t(), ["SA-proto", "SA-proto", "SA-proto", "SA-proto"], "super.x in eval in a class method looks at the parent's prototype");
eq([sb.c(), SB.s()], ["SA.sm", "SA"], "super.m() in eval, and in a static method");
class SC extends SA { constructor() { eval("super(7)"); this.y = 1; } }
var sc = new SC();
eq([sc.x, sc.y, sc.nt, sc instanceof SC], [7, 1, "SC", true], "super() in a constructor's eval");
class SD extends SA { constructor() { var f = () => eval("super(8)"); f(); } }
eq([new SD().x, new SD().nt], [8, "SD"], "super() in an arrow's eval in a constructor");
class SE extends SA { f1 = "field"; constructor() { eval("super(5)"); this.z = this.f1; } }
var se = new SE();
eq([se.x, se.f1, se.z], [5, "field", "field"], "the fields are initialised after super() in eval");
class SF extends SA { constructor() { eval("super(1)"); eval("super(2)"); } }
throws(function () { new SF(); }, ReferenceError, "super() twice through eval");
class SG extends SA { constructor() { super(1); eval("super(2)"); } }
throws(function () { new SG(); }, ReferenceError, "super() in eval after super()");
class SH extends SA { constructor() { eval("this.q = 1"); } }
throws(function () { new SH(); }, ReferenceError, "this in eval before super()");
class SI extends SA { constructor() { var f = () => eval("super.sm"); f(); } }
throws(function () { new SI(); }, ReferenceError, "super.x in an arrow's eval before super()");
class SJ extends SA { constructor() { eval("eval('super(10)')"); } }
eq(new SJ().x, 10, "super() in a nested eval");
class SK extends SA { constructor() { super(); this.r = eval("super.sm()"); } }
eq(new SK().r, "SA.sm", "super.m() in eval after super()");
class SL extends SA { constructor() { var f = function () { return eval("super()"); }; super(); f(); } }
throws(function () { new SL(); }, SyntaxError, "super() in eval in a plain function inside a constructor");
class SM extends SA { m() { var f = function () { return eval("super.sm"); }; return f(); } }
throws(function () { new SM(1).m(); }, SyntaxError, "super.x in eval in a plain function inside a method");
class SN extends SA { m() { return eval("super()"); } }
throws(function () { new SN(1).m(); }, SyntaxError, "super() in eval in a method");
class SO { m() { var arrow = () => super.toString; return eval("super()"); } }
throws(function () { new SO().m(); }, SyntaxError, "super() in eval in a base class method that has an arrow using super.x");
throws(function () { (0, eval)("super()"); }, SyntaxError, "super() in global eval");

// ---- the vars of sloppy direct eval code.  An initialised `var k = 1` in eval used to assign a global k, so the function never saw
// ---- it; the code of a nested eval did not see the function's var object; a name declared by eval was bound to the same name of an
// ---- enclosing function instead of getting a new binding; and eval always saw the outermost one of equally named bindings.
function ev1() { eval("var k = 1"); return k; }
function ev2() { eval("var k = 1; var m = k + 1"); return [k, m]; }
function ev3() { eval("var q = 1; eval('q = 2')"); return q; }
function ev4() { eval("var a = 1; eval('var b = a + 1;')"); return a + b; }
function ev5() { eval("function g() { return v; } var v = 3"); return g(); }
function ev6() { eval("eval('function h() { return 9; }')"); return h(); }
function ev7() { var f = () => { eval("var av = 1"); return av; }; return f(); }
function ev8() { function use() { return ev * 2; } eval("var ev = 10"); return use(); }
function ev9() { eval("var ev = 10"); function use() { return ev * 2; } return use(); }
eq([ev1(), ev2(), ev3(), ev4(), ev5(), ev6(), ev7(), ev8(), ev9()], [1, [1, 2], 2, 3, 3, 9, 1, 20, 20], "vars and functions declared by eval");
function ev10() { var x = "outer"; function inner() { eval("var x = 'eval'"); return eval("x"); } return inner() + "/" + x; }
eq(ev10(), "eval/outer", "an eval var is a new binding of the function it runs in, not the same-named binding of an outer function");
function ev11() { let x = "outer"; return (function () { eval("var x = 1"); return x + "/" + typeof x; })() + "/" + x; }
eq(ev11(), "1/number/outer", "an outer function's let is no conflict for an eval var");
throws(function () { let x; eval("var x"); }, SyntaxError, "a let of the function conflicts with an eval var");
throws(function () { { let x; eval("var x"); } }, SyntaxError, "a let of an enclosing block conflicts with an eval var");
eq([(function () { var x = "outer"; function g() { var x = "inner"; return eval("x"); } return g(); })(),
    (function () { let x = 1; { let x = 2; return eval("x"); } })(),
    (function () { var x = "outer"; return (function (x) { return eval("x"); })("param"); })(),
    (function () { var x = "outer"; try { throw "caught"; } catch (x) { return eval("x"); } })(),
    (function () { var x = "outer"; return (() => { var x = "inner"; return eval("x"); })(); })()],
   ["inner", 2, "param", "caught", "inner"], "eval sees the innermost of equally named bindings");
eq((function () { eval("var dv = 1"); var r = delete dv; return r + "/" + typeof dv; })(), "true/undefined", "an eval var can be deleted");
eq((function () { var b = typeof zz; eval("var zz = 1"); return b + "/" + typeof zz; })(), "undefined/number", "typeof before and after");
eq((function () { eval("var cv = 'c'"); return (function () { return eval("cv"); })(); })(), "c", "an eval in a closure sees the vars of the enclosing function's eval");
eq((function () { eval("var a1 = 1"); return (function () { eval("var a2 = 2"); return (function () { eval("var a3 = 3"); return eval("a1 + a2 + a3"); })(); })(); })(), 6, "three levels");
throws(function () { (function (p = eval("var arguments")) {})(); }, SyntaxError, "var arguments in eval in a parameter expression");
eq((function () { var f = (p = eval("var arguments = 'param'")) => 1; return f() + "/" + typeof globalThis.arguments; })(), "1/undefined", "an arrow's parameter expression does not leak the eval var");
// a sloppy eval in a parameter expression declares its vars in an environment of its own, outside the one the parameters are in:
// the rest of the parameter list, closures made in it before the eval ran, and the body see them; the global is untouched
var peX = "outside", peProbe1, peProbe2;
function pe1(_ = peProbe1 = function () { return peX; }, __ = (eval('var peX = "inside"'), peProbe2 = function () { return peX; })) { return peX; }
eq([pe1(), peProbe1(), peProbe2(), peX], ["inside", "inside", "inside", "outside"], "eval vars of a parameter list");
function pe2(a = 1) { eval("var ev = 5"); function g() { return ev; } return g() + a; }
eq(pe2(), 6, "eval vars in the body of a function with parameter expressions");
var peArrow = (p = eval("var arguments = 'param'"), q = () => arguments) => arguments + q();
eq(peArrow(), "paramparam", "an arrow's eval declares arguments for the rest of its parameters and its body");

// ---- ES2025: match, matchAll, replace, replaceAll, search and split ask only an *object* argument for its Symbol method; the
// ---- @@match of String.prototype, Number.prototype or Boolean.prototype is not looked up for a primitive argument
var symLookups = [];
var symHolders = [String.prototype, Number.prototype, Boolean.prototype];
var symNames = [Symbol.match, Symbol.matchAll, Symbol.replace, Symbol.search, Symbol.split];
for (let holder of symHolders)
  for (let sym of symNames) Object.defineProperty(holder, sym, { get() { symLookups.push(String(sym)); }, configurable: true });
try {
  var symResults = ["a1b".match("1")[0], "a1b".match(1)[0], [..."a1b".matchAll(1)].length, "a1b".replace(1, "-"), "a1b1".replaceAll(1, "-"),
                    "a1b".search(1), "a1b".search(true), "a1b".split(1).join("|"), "a1b".split("1").length, "a1b".replaceAll("1", "-")];
} finally {
  for (let holder of symHolders) for (let sym of symNames) delete holder[sym];
}
eq([symResults.join(), symLookups.length], ["1,1,1,a-b,a-b-,1,-1,a|b,2,a-b", 0], "no Symbol.match/replace/... lookup on a primitive argument");

// ---- Annex B lets a sloppy block declare one name twice only with two plain functions
function isSyntaxError(src) { try { new Function(src); } catch (e) { return e instanceof SyntaxError; } return false; }
eq([isSyntaxError("{ function f() {} function f() {} }"), isSyntaxError("{ async function f() {} function f() {} }"),
    isSyntaxError("{ function f() {} function* f() {} }"), isSyntaxError("{ function* f() {} function f() {} }"),
    isSyntaxError("{ async function* f() {} async function* f() {} }"), isSyntaxError("switch (0) { case 0: function f() {} async function f() {} }")],
   [false, true, true, true, true, true], "duplicate function declarations in a block");

// ---- `var f` after `function f() {}` redeclares the binding and must not lose the function (it became undefined)
function hoistedFn() { return "fn"; }
var hoistedFn;
var hoistedBefore = typeof hoistedFn2;
var hoistedFn2 = 5;
function hoistedFn2() { return "fn2"; }
eq([typeof hoistedFn, hoistedFn(), hoistedBefore, hoistedFn2],
   ["function", "fn", "function", 5], "function and var of one name at script level");
eq((function () { var seen = typeof inner1; function inner1() {} var inner1 = 3; return seen + "/" + inner1; })(), "function/3", "and in a function");

// ---- eval code and classes: it sees the private names of the classes around it (it was a SyntaxError), and `arguments` is a
// ---- SyntaxError in eval code that runs in a field initializer (also from an arrow function in one)
class PE1 { #m = "private"; static #s = "static"; #meth() { return "method"; } get #acc() { return "accessor"; }
  x = eval("this.#m"); y = eval("PE1.#s"); z = eval("this.#meth()"); w = (() => eval("this.#acc"))(); has = eval("#m in this");
  viaMethod() { return eval("this.#m + '/' + (#m in this)"); } }
var peInst = new PE1();
eq([peInst.x, peInst.y, peInst.z, peInst.w, peInst.has, peInst.viaMethod()], ["private", "static", "method", "accessor", true, "private/true"], "private names in eval");
throws(function () { new (class { f() { return eval("this.#nope"); } })().f(); }, SyntaxError, "an undeclared private name in eval");
class PE2 { x = eval("arguments"); }
throws(function () { new PE2(); }, SyntaxError, "arguments in eval in a field initializer");
class PE3 { x = () => eval("arguments"); }
throws(function () { new PE3().x(); }, SyntaxError, "arguments in eval in an arrow in a field initializer");
class PE4 { m() { return eval("typeof arguments"); } }
eq(new PE4().m(), "object", "arguments in eval in a method is fine");

// ---- the bindings of the head of for-in / for-of are in their temporal dead zone while the object expression is evaluated
throws(function () { for (let x of x) ; }, ReferenceError, "for (let x of x)");
throws(function () { for (const x in x) ; }, ReferenceError, "for (const x in x)");
throws(function () { for (let [x] of [x]) ; }, ReferenceError, "for (let [x] of [x])");
eq((function () { var x = [1, 2]; var s = 0; for (var x of x) s += x; return s; })(), 3, "for (var x of x) is fine");
eq((function () { var fns = []; for (let x of [1, 2]) fns.push(() => x); return [fns[0](), fns[1]()]; })(), [1, 2], "each iteration still has its own binding");

// ---- a base class with fields keeps a consistent stack depth (a catch in its constructor crashed the engine)
class Fields {
  a = 1;
  constructor() { try { throw new Error("x"); } catch (e) { this.caught = e.message; } this.b = this.a + 1; }
}
var fl = new Fields();
eq([fl.a, fl.b, fl.caught], [1, 2, "x"], "try/catch in a constructor of a class with fields");
class Fields2 { x = 1; static y = 2; constructor() { for (var i = 0; i < 3; i++) { try { if (i == 1) throw i; } catch (e) { this.e = e; } finally { this.i = i; } } } }
var f2 = new Fields2();
eq([f2.x, f2.e, f2.i, Fields2.y], [1, 1, 2, 2], "loops and finally in such a constructor");
class Derived extends Fields { c = 3; constructor() { super(); try { throw 1; } catch (e) { this.d = e; } } }
var dd = new Derived();
eq([dd.a, dd.b, dd.c, dd.d, dd.caught], [1, 2, 3, 1, "x"], "derived class with fields and a catch");

// ---- Function.prototype.toString is the text of the whole definition: a method's includes its name (and async, *, get, set), a class's is
// ---- the whole class also when the constructor is written out (it used to be the constructor's own text)
class TS1 { constructor(x) { this.x = x; } }
class TS2 extends TS1 { static s = 1; constructor() { super(2); } }
class TS3 { static async *sag() {} static sm(a, b) { return a; } get acc() { return 1; } set acc(v) {} async am() {} *gen() {} ["c" + 1]() {} #p() {} }
var tsobj = { m(a) { return a; }, get g() { return 1; }, set g(v) {}, async am() {}, *gen() {}, async *ag() {}, ["comp" + 1]() {}, plain: function () {}, arrow: (a) => a };
eq(String(TS1), "class TS1 { constructor(x) { this.x = x; } }", "class with an explicit constructor");
eq(String(TS2), "class TS2 extends TS1 { static s = 1; constructor() { super(2); } }", "derived class with a static field and a constructor");
eq([String(TS3.sag), String(TS3.sm)], ["async *sag() {}", "sm(a, b) { return a; }"], "static methods without the static");
var ts3p = Object.getOwnPropertyDescriptor(TS3.prototype, "acc");
eq([String(ts3p.get), String(ts3p.set), String(TS3.prototype.am), String(TS3.prototype.gen), String(TS3.prototype.c1)], ["get acc() { return 1; }", "set acc(v) {}", "async am() {}", "*gen() {}", "[\"c\" + 1]() {}"], "class accessors and methods");
var tsg = Object.getOwnPropertyDescriptor(tsobj, "g");
eq([String(tsobj.m), String(tsg.get), String(tsg.set), String(tsobj.am), String(tsobj.gen), String(tsobj.ag), String(tsobj.comp1), String(tsobj.plain), String(tsobj.arrow)],
   ["m(a) { return a; }", "get g() { return 1; }", "set g(v) {}", "async am() {}", "*gen() {}", "async *ag() {}", "[\"comp\" + 1]() {}", "function () {}", "(a) => a"], "object literal methods");
eq(String(class {}).slice(0, 8), "class {}", "an anonymous class");
// the text of an async function or arrow starts at `async` (it started at `function`), which also made the AsyncFunction and
// AsyncGeneratorFunction constructors reject every body: they check that the text they build is one function expression
async function tsAsyncDecl() {}
async function* tsAsyncGenDecl() {}
eq([String(async function f() {}), String(async function* g() {}), String(async (a) => a), String(async a => a), String(tsAsyncDecl), String(tsAsyncGenDecl)],
   ["async function f() {}", "async function* g() {}", "async (a) => a", "async a => a", "async function tsAsyncDecl() {}", "async function* tsAsyncGenDecl() {}"], "async function text");
var AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;
var AsyncGeneratorFunction = Object.getPrototypeOf(async function* () {}).constructor;
eq(String(new AsyncFunction("a", "return await a")), "async function anonymous(a\n) {\nreturn await a\n}", "the AsyncFunction constructor");
eq([typeof new AsyncGeneratorFunction("yield 1")().next, Object.prototype.toString.call(new AsyncGeneratorFunction("a", "yield a")), new AsyncFunction("return 1")() instanceof Promise],
   ["function", "[object AsyncGeneratorFunction]", true], "the AsyncGeneratorFunction constructor");

print("regressions: " + total + " checks, " + fails + " failures");
