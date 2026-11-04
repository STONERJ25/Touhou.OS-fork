export const a = 1;
export function fn() { return 'fn'; }
export class K { static tag = 'K-tag'; }
export var V = 'var';
export let L = 'let';
const s = 'string-named';
export { s as "string name" };
export { a as alias, a as alias2 };
export const T = typeof this;
export default 'default value';
