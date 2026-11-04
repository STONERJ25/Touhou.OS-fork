// module code: strict, no `arguments`, `this` is undefined, declarations stay in the module, and the early errors
print(this, typeof arguments);
try { undeclaredVariable = 1; } catch (e) { print(e.name); }
print((function () { return this; })());
print(typeof globalThis.print, delete globalThis.nothingThere);
var v = 1;
print(typeof globalThis.v, hoisted());
function hoisted() { return 'hoisted'; }

const name = 'tests/js/modules/virtual.mjs';
const early = {
    'with': 'with ({}) {}',
    'legacy octal': 'let x = 010;',
    'delete a name': 'let x; delete x;',
    'duplicate parameters': 'function f(a, a) {}',
    'await as a name': 'var await;',
    'yield as a name': 'var yield;',
    'html comment': '<!-- x\n',
    'import in a block': '{ import x from "y"; }',
    'export in a block': '{ export let x; }',
    'duplicate export': 'export let a; export { a };',
    'duplicate export name': 'let x, z; export { x as y, z as y };',
    'duplicate default': 'export default 1; export default 2;',
    'export of nothing': 'export { nothing };',
    'return': 'return 1;',
    'new.target': 'new.target;',
    'super': 'super.x;',
    'let let': 'let let = 1;',
    'duplicate import': 'import { a } from "x"; import { a } from "y";',
    'import and let': 'import { a } from "x"; let a;',
    'duplicate function': 'function f() {} function f() {}',
    'var and function': 'var f; function f() {}',
    'function and var': 'function f() {} var f;',
    'label await': 'await: 1;',
    'assign arguments': 'arguments = 1;',
    'await in static block': 'class C { static { await 1; } }',
    'unterminated import': 'import { a',
    'string without from': 'export { "a" as b };',
    'escaped as': 'import * \\u0061s ns from "x";',
    'new import': 'new import("x");',
    'new on an import call member': 'new import("x").y;',
};
for (const [what, source] of Object.entries(early)) {
    try { kz.module(source, name); print(what, 'accepted?!'); } catch (e) { print(what + ':', e.name); }
}

// things that look unusual and are fine
const fine = [
    'export {};', 'import {} from "./lib/log.mjs";', 'import "./lib/log.mjs";', 'export default async function () {}',
    'export default async function* g() {}', 'export default function* () {}', 'export default class extends Object {}',
    'export default async () => 1;', 'import { "log" as l } from "./lib/log_names.mjs"; l;',
    'export { "a b" as "c d" } from "./lib/log_names.mjs";', 'export * as "x y" from "./lib/log_names.mjs";',
    'export * as default from "./lib/log_names.mjs";', 'await 1; for await (const x of []) ;', 'var a = import.meta, b = import("./lib/log.mjs");',
    'let async; async\nfunction f() {}', 'export { x as default }; let x;', 'import * as await_ from "./lib/log.mjs"; await_;',
];
for (const source of fine) {
    try { await kz.module(source, name); print('fine'); } catch (e) { print(JSON.stringify(source), e.name, e.message); }
}
