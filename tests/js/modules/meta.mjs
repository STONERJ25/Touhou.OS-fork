// import.meta: one ordinary object per module, with what the host gives it
import { other } from './lib/meta_other.mjs';

print(typeof import.meta, Object.getPrototypeOf(import.meta), Object.isExtensible(import.meta));
print(import.meta === import.meta);
print(typeof import.meta.url, import.meta.url.endsWith('meta.mjs'));
print(other.url.endsWith('meta_other.mjs'), other.same, import.meta !== other.meta);
const f = () => import.meta;
print(f() === import.meta);
import.meta.custom = 1;
print(import.meta.custom, Object.keys(import.meta).join());
