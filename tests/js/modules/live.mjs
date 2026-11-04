// live bindings, assignment to imports, and what a namespace allows
import { count, increment, reset, obj, setObj, later } from './lib/live_lib.mjs';
import * as ns from './lib/live_lib.mjs';

print(count, ns.count);
increment(); increment();
print(count, ns.count);
reset();
print(count);

// the binding changes; a function that captured it sees the change too
const read = () => count;
increment();
print(read(), ns.count);

// importers cannot assign (module code is strict: a failed assignment throws)
try { count = 5; } catch (e) { print(e.name); }
try { count++; } catch (e) { print(e.name); }
try { ns.count = 1; } catch (e) { print(e.name); }
try { delete ns.count; } catch (e) { print(e.name); }
print(count);

// an object export is shared, not copied; replacing it changes what importers see
obj.x = 1;
print(JSON.stringify(obj));
setObj({ y: 2 });
print(JSON.stringify(obj));

print(later);
print(typeof count, typeof ns, typeof increment);
