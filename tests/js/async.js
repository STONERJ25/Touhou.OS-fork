// generators
function* g1() { var x = yield 1; print("got", x); try { yield 2; } finally { print("cleanup"); } return 3; }
var it = g1();
print(JSON.stringify(it.next()), JSON.stringify(it.next("a")), JSON.stringify(it.return(9)), JSON.stringify(it.next()));
function* g2() { yield* [1, 2]; var r = yield* g3(); yield r; }
function* g3() { yield "x"; return "ret"; }
print([...g2()].join());
function* fib() { let [a, b] = [0, 1]; for (;;) { yield a; [a, b] = [b, a + b]; } }
var f = []; for (var v of fib()) { if (v > 50) break; f.push(v); } print("fib", f.join());
var gthrow = (function* () { try { yield 1; } catch (e) { yield "caught " + e; } })();
gthrow.next(); print(gthrow.throw("boom").value);
class Tree { constructor(v, l, r) { this.v = v; this.l = l; this.r = r; } *[Symbol.iterator]() { if (this.l) yield* this.l; yield this.v; if (this.r) yield* this.r; } }
print("tree", [...new Tree(2, new Tree(1), new Tree(3))].join());
// promises and async
var log = [];
Promise.resolve(1).then(v => log.push("then " + v));
new Promise((res, rej) => rej(new Error("bad"))).catch(e => log.push("catch " + e.message));
Promise.all([1, Promise.resolve(2), new Promise(r => r(3))]).then(v => log.push("all " + v.join()));
Promise.allSettled([Promise.reject(1), 2]).then(v => log.push("settled " + v.map(x => x.status).join()));
Promise.any([Promise.reject(1), Promise.resolve(5)]).then(v => log.push("any " + v));
Promise.race([new Promise(r => setTimeoutish(r)), Promise.resolve("fast")]).then(v => log.push("race " + v));
function setTimeoutish(r) {}
async function af(x) { var a = await x; var b = await Promise.resolve(a + 1); return a + b; }
af(10).then(v => log.push("async " + v));
async function aerr() { await null; throw new TypeError("async fail"); }
aerr().catch(e => log.push("async catch " + e.name));
var order = [];
(async () => { order.push(1); await undefined; order.push(3); })(); order.push(2);
Promise.resolve().then(() => order.push(4));
async function* ag() { yield 1; yield await Promise.resolve(2); return 3; }
(async () => { var out = []; for await (const v of ag()) out.push(v); log.push("for-await " + out.join()); })();
(async () => { var out = []; for await (const v of [Promise.resolve("p"), "q"]) out.push(v); log.push("for-await-sync " + out.join()); })();
Promise.resolve().finally(() => log.push("finally"));
var wr = Promise.withResolvers(); wr.promise.then(v => log.push("withResolvers " + v)); wr.resolve("ok");
class P { #x = 1; static #count = 0; #m() { return this.#x + 10; } get #g() { return this.#x * 2; } inc() { P.#count++; return this.#m() + this.#g; } static count() { return P.#count; } has(o) { return #x in o; } }
var pi = new P(); print("private", pi.inc(), P.count(), pi.has(pi), pi.has({}));
try { P.prototype.inc.call({}); } catch (e) { print("brand", e.constructor.name); }
// after the jobs
Promise.resolve().then(() => Promise.resolve()).then(() => Promise.resolve()).then(() => { print("log", log.join(" | ")); print("order", order.join()); });
