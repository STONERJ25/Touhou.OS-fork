// the specifier of import() is relative to the module it appears in
export async function load() { return (await import('./dyn_target.mjs')).value + 1; }
