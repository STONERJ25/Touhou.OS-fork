// export * (and its rules), export * as, export { x as y } from, default and star, shadowing, ambiguity
import * as hub from './lib/rx_hub.mjs';
import { shared, one, two, viaDefault, nsOfLeaf, renamed, own } from './lib/rx_hub.mjs';

print(Object.getOwnPropertyNames(hub).join());
print(shared, one, two, viaDefault, renamed, own);
print(typeof nsOfLeaf, Object.getOwnPropertyNames(nsOfLeaf).join(), nsOfLeaf.leaf);
// "default" is never re-exported by export *
print('default' in hub, 'default' in nsOfLeaf);
// both stars supply `dup` from different modules: the name is ambiguous, so it is left out of the namespace
print('dup' in hub, hub.dup);
// ...but the same binding reached by two paths is not ambiguous
print(shared === hub.shared);

// an import of an ambiguous name, a missing name and a missing module are SyntaxErrors or load errors at link time
for (const spec of ['./lib/rx_bad_ambiguous.mjs', './lib/rx_bad_missing.mjs', './lib/rx_bad_indirect.mjs', './lib/rx_nothing_here.mjs']) {
    try {
        await import(spec);
        print(spec, 'loaded?!');
    } catch (e) {
        print(spec.slice(6), e.name);
    }
}
