import def, { a, inc, count as n } from './m1_lib.mjs';
import * as ns from './m1_lib.mjs';
print(def, a, n);
inc();
print(n, ns.count, Object.keys(ns).join());
print(Object.prototype.toString.call(ns), typeof ns, Object.getPrototypeOf(ns));
