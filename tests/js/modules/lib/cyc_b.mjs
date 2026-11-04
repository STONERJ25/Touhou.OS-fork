import { a, fa, CA } from './cyc_a.mjs';
export const b = 'b';
export const fromB = typeof fa;            // function declarations are initialised when the module is linked
let early;
try { early = a; } catch (e) { early = e.name; }        // `a` is a const of A, which has not run yet
try { new CA(); } catch (e) { print('class in TDZ:', e.name); }
export function readA() { return a; }
print('B body: a =', early, 'fa() =', fa());
