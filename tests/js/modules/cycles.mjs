// a cycle of two modules, hoisted functions across it, bindings in their temporal dead zone, and a module that imports itself
import * as A from './lib/cyc_a.mjs';
import * as B from './lib/cyc_b.mjs';
import * as self from './cycles.mjs';

print(A.a, B.b, A.fa(), B.readA());
try { self.later; } catch (e) { print('before init:', e.name); }
print(Object.getOwnPropertyNames(self).join());
export let later = 'now initialised';
print(self.later);
print(self === self.self, typeof self.later);
export { self };
