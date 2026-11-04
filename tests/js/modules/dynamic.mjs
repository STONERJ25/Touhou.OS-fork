// import(): a promise, a job later than the call, one module instance per name, expressions of every kind, failures
import { log } from './lib/log.mjs';

const p = import('./lib/dyn_target.mjs');
print(p instanceof Promise, typeof p.then);
print('after the call: target has not run yet:', log.length);
const ns1 = await p;
print('target ran', log.join());
const ns2 = await import('./lib/dyn_target.mjs');
print(ns1 === ns2, ns1.value, log.length);

// relative to the module that contains the call, wherever the call is
const nested = await import('./lib/dyn_nested.mjs');
print(await nested.load());
const viaFn = async () => (await import('./lib/dyn_target.mjs')).value;
const viaEval = await eval("import('./lib/dyn_target.mjs')");
print(await viaFn(), viaEval === ns1);

// the specifier goes through ToString; a conversion that fails rejects the promise, it does not throw
print((await import({ toString() { return './lib/dyn_target.mjs'; } })) === ns1);
for (const bad of [{ toString() { throw new URIError('conversion'); } }, Symbol('s')]) {
    const r = import(bad);
    print(r instanceof Promise);
    try { await r; } catch (e) { print(e.name); }
}

// the second argument
for (const opts of [5, null, { with: 5 }, { with: { type: 1 } }]) {
    try { await import('./lib/dyn_target.mjs', opts); print('accepted'); } catch (e) { print(JSON.stringify(opts), e.name); }
}
print((await import('./lib/dyn_target.mjs', undefined)) === ns1, (await import('./lib/dyn_target.mjs', {})) === ns1,
      (await import('./lib/dyn_target.mjs', { with: {} })) === ns1);

// a missing file or a syntax error rejects each time; an error thrown by a module is remembered
for (let i = 0; i < 2; i++) {
    for (const m of ['dyn_missing', 'dyn_syntax', 'dyn_throws']) {
        try { await import('./lib/' + m + '.mjs'); print(m, 'loaded?!'); } catch (e) { print(m, e.name); }
    }
}
