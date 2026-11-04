// a top-level await that throws rejects the module and everything that waits for it; the failure is remembered
import { log } from './lib/log.mjs';

try { await import('./lib/tla_fail_parent.mjs'); print('no error?!'); } catch (e) { print('caught', e.name, e.message); }
print(log.join(', '));
try { await import('./lib/tla_fail_parent.mjs'); } catch (e) { print('again', e.name, e.message); }
try { await import('./lib/tla_fail_child.mjs'); } catch (e) { print('child', e.name, e.message); }
print(log.length);
