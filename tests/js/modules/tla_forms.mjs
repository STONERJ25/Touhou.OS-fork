// await at the top level of a module, in the places an expression may be
const results = [];
results.push(await 1);
results.push(await Promise.resolve(2));
const thenable = { then(resolve) { resolve(3); } };
results.push(await thenable);
for await (const v of [Promise.resolve(4), 5]) results.push(v);
async function* gen() { yield 6; yield 7; }
for await (const v of gen()) results.push(v);
{ results.push(await 8); }
if (await true) results.push(9);
const obj = { a: await 10, [await 'k']: await 11 };
results.push(obj.a, obj.k);
print(results.join());

try { await Promise.reject(new Error('rejected')); } catch (e) { print('caught', e.message); }

outer: for (let i = 0; i < 3; i++) {
    await null;
    if (i === 1) break outer;
    results.push('i' + i);
}
print(results.slice(-1)[0]);

// the module is an async function: an await in a nested block, a switch, a try/finally
switch (await 'b') { case 'a': print('a'); break; case 'b': print('switch b'); }
try { print('try', await 1); } finally { print('finally', await 2); }
print(typeof await 0, await (async () => 'arrow')());
