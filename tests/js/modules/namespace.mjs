// the module namespace exotic object: its keys, descriptors, internal methods and what strict code may do to it
import * as ns from './lib/ns_lib.mjs';
import { bump } from './lib/ns_lib.mjs';

const show = (v) => typeof v === 'symbol' ? String(v) : JSON.stringify(v);
const names = Reflect.ownKeys(ns);
print(names.map(show).join(' '));            // strings in code unit order ("10" before "2"), then the symbol
print(Object.prototype.toString.call(ns), ns[Symbol.toStringTag], typeof ns, Object.getPrototypeOf(ns));
print(JSON.stringify(Object.getOwnPropertyDescriptor(ns, 'x')));
print(JSON.stringify(Object.getOwnPropertyDescriptor(ns, Symbol.toStringTag)));
print(JSON.stringify(Object.getOwnPropertyDescriptor(ns, 'nothing')));
print(ns[0], ns['1'], ns[10], ns[2], ns.x, ns.nothing, ns[Symbol.iterator]);

// live: y is the module's variable
print(ns.y);
bump(); bump();
print(ns.y, JSON.stringify(Object.getOwnPropertyDescriptor(ns, 'y')));

// the internal methods through Reflect
print('set', Reflect.set(ns, 'x', 5), Reflect.set(ns, 'new', 5), Reflect.set(ns, Symbol.iterator, 1));
print('define', Reflect.defineProperty(ns, 'x', { value: 1 }), Reflect.defineProperty(ns, 'x', { value: 2 }),
      Reflect.defineProperty(ns, 'x', { writable: false }), Reflect.defineProperty(ns, 'x', { enumerable: false }),
      Reflect.defineProperty(ns, 'x', { configurable: true }), Reflect.defineProperty(ns, 'x', { get() {} }),
      Reflect.defineProperty(ns, 'x', { writable: true, enumerable: true, configurable: false, value: 1 }),
      Reflect.defineProperty(ns, 'nothing', { value: 1 }), Reflect.defineProperty(ns, Symbol.toStringTag, { value: 'Module' }),
      Reflect.defineProperty(ns, Symbol.toStringTag, { value: 'Other' }));
print('delete', Reflect.deleteProperty(ns, 'x'), Reflect.deleteProperty(ns, 'nothing'), Reflect.deleteProperty(ns, Symbol.toStringTag));
print('has', Reflect.has(ns, 'x'), Reflect.has(ns, 'nothing'), Reflect.has(ns, Symbol.toStringTag), 'toString' in ns);
print('proto', Reflect.setPrototypeOf(ns, null), Reflect.setPrototypeOf(ns, {}), Reflect.getPrototypeOf(ns));
print('extensible', Reflect.isExtensible(ns), Reflect.preventExtensions(ns), Object.isExtensible(ns));
print('integrity', Object.isSealed(ns), Object.isFrozen(ns));

// strict code (modules are strict): failed operations throw
const attempts = {
    'assign existing': () => { ns.x = 2; },
    'assign new': () => { ns.newThing = 1; },
    'assign symbol': () => { ns[Symbol.iterator] = 1; },
    'delete existing': () => { delete ns.x; },
    'delete missing': () => delete ns.nothing,
    'defineProperty': () => Object.defineProperty(ns, 'x', { value: 3 }),
    'defineProperty same': () => Object.defineProperty(ns, 'x', { value: 1 }),
    'setPrototypeOf': () => Object.setPrototypeOf(ns, {}),
    'freeze': () => Object.freeze(ns),
    'seal': () => Object.seal(ns),
};
for (const [name, f] of Object.entries(attempts)) {
    try { const r = f(); print(name, 'ok', r === ns ? 'ns' : String(r)); } catch (e) { print(name, e.name); }
}

// enumeration copies the current values; functions are not JSON
print(Object.keys(ns).join());
print(JSON.stringify(Object.entries(ns).filter(([k, v]) => typeof v !== 'function')));
print(JSON.stringify({ ...ns }));
print(JSON.stringify(ns));
const seen = [];
for (const k in ns) seen.push(k);
print(seen.join());
print(JSON.stringify(Object.assign({}, ns).y), Object.fromEntries(Object.entries(ns)).default);

// a namespace with an export called `then` makes import() resolve through it
const r = await import('./lib/ns_then.mjs');
print(r);
