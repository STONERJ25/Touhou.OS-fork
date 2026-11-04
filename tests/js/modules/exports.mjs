// every form of import and export: names, defaults, strings as names, aliases, declarations, namespaces
import dflt, { a as renamed, fn, K, V, L, "string name" as sn, alias, alias2, T } from './lib/exports_lib.mjs';
import * as ns from './lib/exports_lib.mjs';
import { default as again, a } from './lib/exports_lib.mjs';
import defaultOnly from './lib/exports_default_fn.mjs';
import ClassDefault from './lib/exports_default_class.mjs';
import anon from './lib/exports_default_anon.mjs';
import './lib/exports_side_effect.mjs';
import './lib/exports_side_effect.mjs';

print(dflt, again, dflt === again);
print(renamed, a, alias, alias2, sn);
print(fn(), K.tag, V, L, T);
print(Object.keys(ns).join(' | '));
print(Object.getOwnPropertyNames(ns).join(' | '));
print(defaultOnly.name, defaultOnly(), typeof defaultOnly);
print(ClassDefault.name, new ClassDefault().kind);
print(JSON.stringify(anon.name), anon());
print(fn.name, K.name, typeof K);

// the module's own top-level declarations are not properties of the global object
var topVar = 1;
function topFn() {}
let topLet = 2;
print(typeof globalThis.topVar, typeof globalThis.topFn, typeof globalThis.topLet);
print(typeof this, this === undefined);
print(typeof ns.default, ns.default);
