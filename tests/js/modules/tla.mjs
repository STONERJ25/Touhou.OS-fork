// the order in which modules with and without top-level await run: siblings do not wait for each other, a module
// waits for the async modules it imports, and the modules that wait run in the order they were put off
import './lib/tla_a.mjs';       // async: starts now, finishes later
import './lib/tla_b.mjs';       // sync: runs at once
import './lib/tla_c.mjs';       // imports A: waits for it
import './lib/tla_d.mjs';       // async
import './lib/tla_e.mjs';       // imports D (and B, done already): waits for D
import { log } from './lib/log.mjs';
log.push('main');
print(log.join(', '));
