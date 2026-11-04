# Kosuzu: the TouhouOS JavaScript engine

Kosuzu (after Kosuzu Motoori of the Suzunaan book rental, who can read any writing) is an ECMAScript engine written
from scratch for TouhouOS: a lexer and recursive-descent parser, a bytecode compiler, a stack VM with inline caches,
a precise garbage collector, the standard library, and a C embedding API (`user/lib/js/include/js.h`). It has no
JIT. It is a static library (`user/lib/js`), built for the OS and for the host test tool (`tests/host/jstool.c`).

## State when paused / how to resume

**Second round, branch `agent/js2`** (brief: `the project's internal briefs`; the first round is on `agent/js`, merged into main).
Everything listed as done is committed and tested; the item list below is the brief's, in its order.

1. **RegExp: done** (2026-11-03). `src/regexp_parse.c` (pattern to syntax tree: Annex B, named groups with the ES2025
   duplicate-name rule, lookbehind, `(?i:...)` modifiers, the `v` flag's set syntax), `src/regexp_gen.c` (tree to word
   code, search analyses, compile cache), `src/regexp_exec.c` (the backtracking matcher), `src/builtins_regexp.c` (the
   RegExp constructor, `RegExp.prototype`, `Symbol.match/matchAll/replace/search/split`, the string iterator,
   `RegExp.escape`, `compile`). Tests: `tests/js/regexp*.js` (647 self-checking assertions, run under AddressSanitizer,
   UBSan and the collector stress mode), `tests/host/test_js_regexp.py` (random patterns against Python's `re`: 180000
   patterns agreed), the hostile-pattern tests in `tests/host/test_js.py` (`RegExpLimits`), `bench_regexp.js`.
   Not done: the legacy `RegExp.$1`/`lastMatch` statics (feature `legacy-regexp`), properties of strings in `v` mode
   (`\p{RGI_Emoji}` is a SyntaxError), a required-literal pre-check (a pattern like `(?:ab)*c` on a long string without a
   `c` is quadratic, as in other backtracking engines), fast paths that skip the result arrays in
   `replace`/`split`/`match` (they follow the specification step by step; see "Speed").
2. **Date: done** (`src/date.c`, ECMA-262 21.4 and Annex B; the time zone comes from the host hooks `tz_offset_min` and
   `tz_name`, a skipped or repeated local time uses the offset before the change). Tests: `tests/js/date.js` (168
   checks, expected values computed independently), `DateZones` in `tests/host/test_js.py` (nine fixed zones, a
   daylight-saving rule with the gap and the overlap, against Python's datetime; `jstool --tz`, `--tz-dst`, `--tz-name`).
3. **Reflect and Proxy: done** (`src/proxy.c`: every trap with the invariant checks, revocable proxies, callable and
   constructible proxies; a proxy checks the C stack, so a long chain of trap-less proxies or a prototype cycle through
   one is a RangeError). Tests: `tests/js/proxy.js` (81 checks), `ProxyAndTypedLimits`.
4. **ArrayBuffer, SharedArrayBuffer, typed arrays, DataView, Atomics: done** apart from the BigInt64 kinds, which need
   item 5 (`src/arraybuffer.c`, `src/typed.c`: resizable and growable buffers, `transfer`, length-tracking views,
   the integer-indexed exotic object, species, all the prototype methods incl. the copying ones; single-threaded
   Atomics: `wait` throws because this agent cannot block, `notify` returns 0; `Atomics.waitAsync` and Float16 are not
   there). Tests: `tests/js/typed.js` (143 checks).
5. **BigInt: done** (`src/bigint.c`: 32-bit limbs, schoolbook multiplication, Knuth's algorithm D, shifts and bitwise
   operators with two's complement semantics, exact comparison with Numbers, correctly rounded `Number(bigint)`, radix
   2..36 in both directions, `asIntN`/`asUintN`, the BigInt64 and BigUint64 kinds of typed arrays, DataView and
   Atomics; results over 2^30 bits are a RangeError, long loops poll the interrupt handler). Tests:
   `tests/host/test_js_bigint.py` (random operands of up to 3000 bits against Python's integers, 4000 pairs agreed,
   plus twelve inputs that take step D6 of the division), `tests/js/bigint.js` (142 checks), `BigIntLimits`,
   `bench_bigint.js`. Not done: Karatsuba (a product of two numbers of 100000 digits takes seconds), `Atomics.waitAsync`.
6. **The `js` applet: done** (`user/bin/kappabox/js.c`, described under "The js applet" below; manual page
   `initrd/ScarletDevilMansion/share/patchouli/1/js.pw`; `/bin/js` is made by the existing applet table, so no edit
   of `tools/build.py` was needed). Tests: `tests/host/test_js_applet.py` (the real applet built for the host under
   AddressSanitizer: 38 tests with pipes and a pseudo terminal, 14 of them for modules), `tests/js/console/inspect.js` (the inspector against
   Node's format), `tests/case_js.py` in QEMU (2 tests, 61 commands in the table plus the interactive session: all pass at the
   end of the round, 36 s and 5 s, run with `python tools/qemu-slot.py -- python tests/run_all.py js`; the manual page is
   checked there too). Cost in the image: see "OS build".
7. **Modules: done** (`src/module.c`; the compiler describes a module's requests, imports and exports in its bytecode, see
   "Modules" below): `import`/`export` in all their forms (named, default, namespace, string names, `export * [as]`
   from, re-exports), live bindings and the temporal dead zone across modules, cycles, namespace exotic objects,
   `import.meta`, `import()` from modules and scripts, top-level await with the specification's algorithm for async
   modules (cycle roots, async parents, evaluation order), a host hook that loads a module by name
   (`JSHostHooks.load_module`, also `import_meta`), `kz.module(text)` in `jstool`, and the `js` applet runs `.mjs`
   files and `--module`. test262: 100% of the module tests that need no unsupported feature (language/module-code,
   import, export, expressions/dynamic-import and import.meta: 1167 tests; `import-defer`, `source-phase-imports` and
   `import-attributes` are skipped). Tests: `tests/js/modules/*.mjs` (11 entry modules with `.expected`, a `lib/`
   of support modules) and `tests/host/test_js_modules.py` (28 tests: the corpus under AddressSanitizer and again with the
   collector running at every 7th allocation; failing entries; a chain of 2000 modules, 100000 (a RangeError), a cycle
   of 500, a star-export ladder with 2^40 paths, 1500 async modules in a chain, 3000 importers), the applet's
   `Modules` class in `test_js_applet.py` (14 tests), seven rows in `tests/case_js.py` (QEMU).
8. **Known bugs, `Array.fromAsync`, iterator helpers: done.** `Array.fromAsync` (`src/array_from_async.c`: the specification's
   async closure as a state machine, every `Await` takes the ticks the specification gives it; test262
   `built-ins/Array/fromAsync` 100%; `tests/js/fromasync.js`, 27 checks). Iterator helpers (`src/iterator_helpers.c`: the
   `Iterator` constructor, `Iterator.from`, `Iterator.prototype.map/filter/take/drop/flatMap/reduce/toArray/forEach/some/every/find`
   as native state machines, `Symbol.toStringTag` and `constructor` as the accessors the specification wants; test262
   `built-ins/Iterator` 100% for the helpers; `tests/js/iterhelpers.js`, 113 checks). The known bugs: `for (let x of x)`
   and `for (const x in x)` apply the TDZ to the right-hand side; `super.x` and `super()` work in direct eval (see
   "Engine bugs"); the "nested eval var scope" bug turned out to be seven bugs in how eval code declares and finds
   variables (below, and "Direct eval"); closing a for-await loop awaits `return()` (and `return()` is no longer called twice
   when it throws); the text of async functions and arrows starts at `async`, which also made the `AsyncFunction` and
   `AsyncGeneratorFunction` constructors work.
   Tests for all of them are in `tests/js/regressions.js` and `tests/js/iterclose.js`.

Engine bugs found by test262 and fixed on the way (regression tests in `tests/js/regressions.js`): eval code took the
`this` of an enclosing function; a base class constructor with fields recorded a negative stack depth (a `catch` in it
crashed); an arrow function with a block body at script top level could not contain `return`; property names that
spell an array index and are one character long (`{"0": 1}`, `static "1"`) were stored as a second property no
`o[0]` could reach (the strings "0".."9" are atoms of the string table and `ToPropertyKey` returned atoms unchecked).
Found by the module tests running with a collection at every allocation: when a function's first constant was the bytecode
of a hoisted nested function, the compiler did not root it while it made the constant table (the function declared first
in a module, script or function could end up as another function: `add_const` in `compile.c`); a global `let`/`const` cell was
not rooted while its property was added (`vm.c`, global declaration instantiation); a mapped `arguments` object lost its
mapping when `Object.defineProperty` made an index non-configurable (the exotic hooks assumed the value lived in the dense
elements; they are now independent of where the property is kept, and `delete` of a non-configurable mapped index no longer
unmaps it). `-DKZ_POISON` (a debug build option) fills dead cells and freed buffers with 0xDD.

Engine bugs of the last part of the round (the known-bugs item), all with regression tests:

* **Direct eval and scopes** (`compile.c` `gen_eval_site`/`gen_binding_init`/`gen_prologue`, `eval.c` `build_caller`): (1) an
  initialised `var k = 1` in sloppy eval code inside a function stored a *global* `k` (the declaration took the path for
  global code); the function's reads found the hoisted `undefined` in its var object. (2) The synthesized scope chain of the
  caller put the outermost of equally named bindings first, so `eval("x")` saw an outer function's `x` instead of its own,
  an enclosing block's `let`, a parameter or a `catch` variable. (3) The names an eval declares were bound to a same-named
  binding of an *enclosing function* (or a `let` there made them a SyntaxError); now each descriptor entry says whether it is
  of the function whose variable environment the eval extends (`KzVarName.outer`, `BF_OUTER` on the synthesized bindings) and
  only those count. (4) The object that holds a function's eval-declared vars is a scope of its own in the synthesized chain
  (after the function's bindings, before the outer ones), so a nested eval, an arrow or an inner function sees it; the
  descriptor lists the innermost function's `this`, `new.target`, home object and constructor only, so `eval("super()")` in a
  plain function inside a constructor is a SyntaxError. (5) The cell of that object was made after the function declarations
  captured it. (6) Eval code in a parameter expression of a function with parameter expressions: its vars live in an
  environment of their own (a var object bound to the parameters' scope, `eval_in_params`) that the rest of the list, the
  closures made in it and the body see, and that the parameters and the body's vars shadow; it cannot declare `arguments`
  or a parameter name (a SyntaxError, as in the specification). Before, such an eval wrote a *global* variable. (7) A function
  with parameter expressions made the object for the vars of an eval in its body in the wrong scope, so those vars were
  not found either (they also went to the global object).
* **Declarations and smaller things**: `var f;` after `function f() {}` in one scope made the function vanish (`declare_name`
  overwrote the binding's `decl`); a sloppy block could declare an async function or a generator twice, or next to a plain
  function of the same name (Annex B allows two *plain* functions only); `match`, `matchAll`, `replace`, `replaceAll`, `search`
  and `split` asked a primitive argument for its Symbol method (ES2025 asks objects only); `Promise.try` was missing; direct
  eval in a class body could not name the class's private names; `arguments` in eval code in a field initializer was not an
  error; the text of async functions started at `function` (so `AsyncFunction("...")` rejected every body).
* **Iterator closing** (`compile.c` holes, `vm.c`): the code that closes the iterator of a `for-of` was inside the exception
  range of the loop's own handler, and of a `try`/`catch` in the body, so an error or a non-object result of `return()`
  called it a second time or was caught by the inner `catch`. For `for await` the result of `return()` is awaited
  (`iter_close_async`, `iter_close_check`, `iter_close_throw_async`), also on the throw path, where its errors are ignored.

**test262** is available to the runner (`tests/host/test262.py`, below); its numbers are in "Conformance".

Solid from the first round (built clean under AddressSanitizer + UBSan, covered by `tests/host/test_js.py` and
`tests/js/*`):

* The language core: all of ES2024 syntax, modules included. Strict and sloppy
  mode, `with`, direct and indirect `eval` (with the caller's scopes), destructuring, spread, default and rest
  parameters, classes with fields, static blocks, private methods/accessors/fields and `#x in o`, getters/setters,
  computed keys, optional chaining, nullish coalescing, logical assignment, labelled statements, `try/catch/finally`
  with completion values, generators, async functions, async generators, `for await`, tagged templates, Annex B
  function hoisting.
* The collector: precise, non-moving mark-sweep with weak atoms/shapes, ephemerons (WeakMap), WeakRef,
  FinalizationRegistry, a memory limit that ends in a catchable "out of memory" RangeError, and a stress mode
  (`jstool --stress`, `--stress-every N`).
* The embedding API (`js.h`): runtimes, realms, scopes and pins, host functions and host classes with property
  hooks, interrupts, memory accounting, JSON, promises and the job queue, error reports.
* The library: Object, Function, Array, String (with Unicode 15.1 case mapping, `normalize`, `localeCompare`),
  Number (exact shortest `toString`, exact `toFixed`/`toExponential`/`toPrecision`, correctly rounded parsing),
  Boolean, Symbol (registry, well-known symbols), Math (own correctly-rounded-or-close implementations), JSON, Map,
  Set (with the ES2025 set methods), WeakMap, WeakSet, WeakRef, FinalizationRegistry, Promise (all combinators,
  `withResolvers`, unhandled-rejection reports), Error types with `stack`, `cause`, AggregateError, globals
  (`parseInt`, `parseFloat`, URI functions, `escape`/`unescape`), and now RegExp.

Still missing: Intl (stubs in `src/stubs.c`), `Atomics.waitAsync`, Float16Array, JSON modules and import attributes (`import ...
with {type: "json"}` is a SyntaxError; `import(x, {with: {...}})` rejects with a TypeError unless the attributes are empty),
`import defer` and source phase imports; the gaps the test262 numbers show (references under `with`, Annex B function-in-block
hoisting in eval code, the `caller`/`arguments` poison pills of strict functions, Unicode 16 and 17 property escapes, the
`Error.prototype.stack` accessor, `JSON.rawJSON`); see "Conformance" and "Known gaps" at the end.

Next steps, by what each would gain in test262 (counts of 2026-11-03):

1. **References under `with`** (about 120 tests: compound assignment, `++`/`--`, logical assignment, `var x = init` inside
   `with`). The specification resolves the reference *before* it evaluates the right-hand side, and stores into the object that
   held the binding then, even when the right-hand side deleted it; the engine looks the name up again at the store
   (`OP_with_get` ... `OP_with_put`, `gen_ident_load`/`gen_ident_store`). Keep the holder in a temporary local (a
   `with_get_hold slot` / `with_put_hold slot` pair) and resolve a `var` declarator's name before its initialiser.
2. **Annex B.3.3.3, function declarations in blocks of eval code** (about 150 tests in `annexB/language/eval-code`). `annexb()`
   in `compile.c` does it for functions and scripts; for eval it must run in `kz_eval_direct` before the loop that moves the
   eval's vars to the caller's var object (check the caller's `let`/`const` between the eval and the function through the
   descriptor, and `CanDeclareGlobalVar` for global eval), and the store when the block's declaration is evaluated must go
   through `gen_ident_store` like any eval var.
3. **Unicode 16 and 17** (about 200 tests: `RegExp/property-escapes/generated`, `unicodeSets`, identifiers with new scripts).
   `gen_unicode.py` reads `/usr/share/unicode`, which holds 15.1 on this machine; with the newer UCD files the tables are
   regenerated, nothing else changes.
4. Smaller: `JSON.rawJSON`, `JSON.isRawJSON` and the reviver's source text (feature `json-parse-with-source`, skipped by the runner),
   `Error.prototype.stack` as an accessor (`error-stack-accessor`, skipped), the iterator closing of destructuring assignments
   in `for-of` heads (`dstr/*iter-close*` and `array-elem-iter-rtrn-close*`, about 60 tests), `caller`/`arguments` of strict
   functions (20 tests in `built-ins/Function`), the `catch` parameter exemption for eval vars (B.3.4), and Intl.

How to resume: read this file, `user/lib/js/src/jsint.h` (the internal model and every module's prototypes), and
`src/stubs.c` (exactly what is missing). Run `python tools/hosttest.py discover -s tests/host -p "test_js*.py" -v`.
`kz_init_<module>` is called in order by `kz_realm_init` (`src/realm.c`). A quick incremental build for development (WSL):
compile every `src/*.c` and `tests/host/jstool.c` with `gcc -O1 -g -I user/lib/js/include` into `build/`, or let
`test_js.py` do it. When a test fails only under `--stress-every N`, build with `-DKZ_POISON`: a value the collector should
have kept then points into 0xDD memory instead of working until the cell is reused.

## Supported language and library

Target: ECMAScript 2024 (ES15), with Annex B (web compatibility) for sloppy scripts, and a few ES2025 additions
(Set methods, `Promise.withResolvers` is ES2024). Unicode data: UCD 15.1 (`user/lib/js/gen_unicode.py` generates
`src/unicode_tables.h` from `/usr/share/unicode`; the tables take about 143 KiB).

Not supported, by design: the DOM, a browser `window`, networking, WebAssembly, a JIT. `SharedArrayBuffer`
semantics are single-threaded (`Atomics.wait` throws a TypeError: this agent cannot block).

## Embedding

```c
#include "js.h"

static JSValue host_print(JSContext *ctx, JSValue this_val, int argc, const JSValue *argv, void *data) {
    for (int i = 0; i < argc; i++) {
        char *s = js_to_utf8(ctx, argv[i], NULL);       /* ToString, UTF-8 */
        if (!s) return JS_EXCEPTION;
        printf("%s%s", i ? " " : "", s);
        js_free_utf8(ctx, s);
    }
    printf("\n");
    return JS_UNDEFINED;
}

int main(void) {
    JSRuntimeOptions o = { .memory_limit = 64 << 20 };
    JSRuntime *rt = js_runtime_new(&o);
    JSContext *ctx = js_context_new(rt);
    js_define_str(ctx, js_global_object(ctx), "print", js_new_function(ctx, host_print, "print", 1, NULL), JS_PROP_HIDDEN);
    int mark = js_scope_open(ctx);
    JSValue r = js_eval(ctx, "print([1, 2, 3].map(x => x * 2))", (size_t)-1, "<demo>", JS_EVAL_SCRIPT);
    if (r == JS_EXCEPTION) {
        char *msg = js_error_report(ctx, js_take_exception(ctx));   /* "TypeError: ...\n    at ..." */
        fprintf(stderr, "%s\n", msg);
        js_free_utf8(ctx, msg);
    }
    js_run_jobs(rt, 0);                                  /* promise reactions */
    js_scope_close(ctx, mark);
    js_context_free(ctx);
    js_runtime_free(rt);
}
```

Rules: every value an API call returns lives until the scope that was open closes; `js_pin` keeps one beyond that.
A host function runs in its own scope. Errors are returned as `JS_EXCEPTION` with the exception pending (nothing
longjmps). Host classes (`JSClassDef`) give objects with a finalizer, a tracer for values the host holds, and
optional property hooks (get/set/has/delete/own_keys) and call/construct.

### A DOM binding, sketched

The web engine can expose its nodes as host objects: one `JSClassDef` per interface (`Node`, `Element`, ...)
whose `opaque` is the engine's node pointer, `trace` marking the JS wrapper's expandos, and the methods as
`js_new_function` closures on the prototype (`js_new_function_with_slots` keeps per-function data such as the
attribute name). A node keeps its wrapper in a field (pinned while the node is in a document, so expandos survive);
the finalizer clears it. Events: the host stores listener functions pinned and calls them with `js_call`, then
`js_run_jobs`. Collections (`childNodes`) use the `get`/`own_keys` hooks for indexed access.

## Design

* **Values**: NaN-boxed 64-bit; doubles unless the top 16 bits are 0xFFF9..0xFFFF (int32, specials, string,
  symbol, object, BigInt, internal pointer). Integral doubles are normalized to int32.
* **Strings**: Latin-1 or UTF-16 flat strings, ropes for concatenation (flattened iteratively), interned atoms for
  property names. Property keys are atoms, symbols or array indexes (int-tagged).
* **Objects**: hidden-class shapes with transitions (weakly held), dictionary mode for deletes and large
  objects, dense elements with holes, arrays' `length` virtual, an exotic-hook table per class (arguments,
  String objects, host objects; Proxy and typed arrays will plug in the same way).
* **Compiler**: the parser builds an AST with scopes and early errors; analysis resolves every identifier
  statically (locals, closure cells, globals, or dynamic lookups only inside `with` and sloppy direct `eval`);
  codegen emits stack bytecode with completion records for `finally`, an exception table, line/column tables
  and inline-cache slots.
* **VM**: computed-goto dispatch, JS-to-JS calls without C recursion, heap frames for generators and async code
  (suspension = return from the loop with a reason), monomorphic inline caches (own and prototype properties,
  globals), a C-stack guard and a call-depth limit (RangeError), an interrupt counter.
* **GC**: size-classed 64 KiB pages for cells up to 1 KiB, the system allocator above that, an explicit mark
  stack with overflow recovery, epoch marks (no clearing pass), weak processing (atoms, shapes, ephemerons,
  WeakRef, FinalizationRegistry), a two-phase sweep. Native code roots values with scoped macros (`KZ_ROOTS`,
  `KZ_ROOT_VEC`, ...) whose cleanup attribute pops them; the stress mode collects at every allocation to catch a
  missing root.
* **Numbers**: own `dtoa` (shortest round trip, exact fixed/exponential/precision with big integers), own
  correctly rounded `strtod`, own math library (double-double `pow`/`exp`/`log`, Payne-Hanek reduction for
  trigonometry).

### RegExp

Written from ECMA-262 22.2 and Annex B.1.2; the code mirrors the specification's structure.

* **Parser** (`regexp_parse.c`): a pre-scan counts the groups and finds out whether any is named (the grammar's `N`
  parameter), then a recursive-descent parser builds a tree of nodes (`regexp_ast.h`). Outside `u`/`v` mode the Annex B
  grammar applies (`\8`, legacy octal escapes, a lone `{` or `]`, `\c` oddities, quantified lookaheads, class ranges
  next to class escapes); group names may be duplicated in different alternatives (ES2025, checked with the
  alternative path of each group); `(?ims-ims:...)` modifiers change the flags the nodes are created with; the `v` flag
  has nested classes, `&&`, `--`, `\q{...}` strings and folds sets exactly (`MaybeSimpleCaseFolding`). Not supported:
  properties of strings (`\p{RGI_Emoji}`). The recursion is guarded by the engine's C stack check.
* **Case folding**: a character atom stores its canonical form (`kz_unicode_fold_simple` with `u`/`v`, the
  uppercase-based Canonicalize without); a class stores its members plus the canonical forms of its members, and the
  matcher canonicalises the input character before looking it up. That is the specification's "there exists a member
  whose Canonicalize equals" without enumerating equivalence classes, and it gets `/K/iu` versus `/K/i`,
  `\w` with `/iu` (long s, Kelvin sign) and `\P{Lu}` with `/iu` right.
* **Code** (`regexp_gen.c`, instruction list in `regexp.h`): a disjunction is `SPLIT_GREEDY`/`JMP`; a group is
  `SAVE_START`/`SAVE_END` (the other way round in a lookbehind, whose body is compiled right to left); `x*` and `x?`
  need no counter; `x{n,m}` and `x+` use two registers (count, start of the iteration); every iteration first clears
  the groups inside it (`RESET_CAPS`, RepeatMatcher step 4) and an iteration that matched nothing after the minimum
  is rejected (`EMPTY_CHECK`); a loop over one character (`a*`, `\d+`, `.*?`, `[^"]*`) is one `REPEAT_ATOM`
  instruction. Analyses: the set of characters a match can start with (a 256-bit map plus "something above 255"),
  the minimum match length, a leading `^`.
* **Matcher** (`regexp_exec.c`): an explicit backtrack stack of 8-byte entries (choice points, old values of captures and
  registers, lookaround barriers, the remaining options of an atom run); failure unwinds it. A positive lookaround
  discards its choice points but keeps the capture undo records; a negative one unwinds back to its barrier.
  `REPEAT_ATOM` pushes one entry for the whole run and steps back one character at a time (by code point under
  `u`/`v`). No C recursion. The stack is capped at 8M entries (a RangeError "Maximum call stack size exceeded") and
  its growth counts against the runtime's memory limit; the matcher polls the interrupt handler every 16384 steps,
  so a host can stop `/(a+)+b/` (the error "interrupted" cannot be caught by `try`/`catch`).
* **Cache**: compiled patterns are reference counted and cached by (source, flags) in the runtime (32 slots), so a
  literal in a loop compiles once, and the early-error check of a literal at parse time is not repeated at run time.
* **Objects** (`builtins_regexp.c`): `lastIndex` is always slot 0 of a RegExp instance; the `Symbol.*` methods follow
  the specification step by step (including `Get(rx, "exec")` and `flags`), so subclasses, species and instrumented
  objects behave as specified; `exec` is recognised and called directly when it is the built-in.

### Modules

Written from ECMA-262 16.2 (the 2025 text: the namespace re-export rule of `import * as ns; export { ns }`, the
leaf-to-root order of `AsyncModuleExecutionRejected`).

* **Embedding.** `js_eval(ctx, text, len, name, JS_EVAL_MODULE)` compiles a module called `name` (a syntax error is thrown
  at once) and returns a promise for the evaluation of the whole graph; a missing file, an import that does not resolve,
  an ambiguous star export and any error a module throws reject it. The host gives `JSHostHooks.load_module(ctx,
  specifier, referrer, &len, &resolved, &error, opaque)`: it is asked for every distinct specifier of every module, with the
  canonical name of the module that asks, and returns the source text and the canonical name (two specifiers for one file
  must give the same name: the engine keeps one module per name in `ctx->modules`, the module map); the pointers are the
  host's and valid until the next call. `import_meta(ctx, meta, name, opaque)` fills `import.meta` on first use.
  `js_promise_set_handled` tells the engine that the host reads the promise's outcome itself (otherwise a rejection is also
  reported as an unhandled rejection). `import()` is a job: it loads, links and evaluates after the code that called it has
  finished, and works in scripts too (the referrer is the script's file name). `tests/host/modload.h` is a loader for
  the host tools (paths relative to the importer, normalized); the applet has its own (`js.c`, absolute names).
* **Compiler.** A module is compiled as a generator function (an async one when the module's own code has an `await`,
  also `for await`), so that the prologue - the closures of the function declarations - can run at link time and the body
  later. The description for the linker is an array in `JSFunctionBytecode.module_desc`: the specifiers, the import entries
  (request, imported name, frame slot) and the export entries (local: slot and whether it starts in its TDZ; indirect:
  request and name; star). Bindings that cross a module boundary (exported or imported) are variable cells made by the
  linker (`BF_PRECELL`); module-level function declarations are lexical (a duplicate or `var f; function f(){}` is a
  SyntaxError); `export default` of an anonymous function or class names it "default".
* **Linking** (`module.c`): load the graph depth first through the hook; collect the modules that are new; make each one's
  frame and the cells of its local exports; resolve every indirect export and import (`ResolveExport` with the
  specification's resolve set, so a ladder of star exports is linear, not exponential; "does not provide an export named
  x", "conflicting star exports") and put the exporters' cells into the importers' frames; run the prologues. A failure
  undoes the whole link (the modules go back to new) so a retry starts clean. A namespace object is made on demand
  (`GetModuleNamespace`): sorted export names, a hash of the property keys, one cell per name; its exotic hooks give the
  live values, the TDZ error for an uninitialised binding, and the descriptors `{writable: true, enumerable: true,
  configurable: false}`; everything else follows 10.4.6.
* **Evaluation**: `InnerModuleEvaluation` with DFS indexes and cycle roots, `[[PendingAsyncDependencies]]`, the async
  parent lists and the order in which modules became async, `ExecuteAsyncModule` (the module body is driven like an async
  function's: one promise per async module, reactions registered before the body starts), `AsyncModuleExecutionFulfilled`
  and `Rejected`; an evaluation error is kept in the module and thrown again for every later import. Every walk
  over the graph recurses with a C stack check, so a chain of 100000 modules is a RangeError, not a crash.
* Not done: JSON modules and import attributes, `import defer`, source phase imports, an asynchronous load hook (loading is
  synchronous, which the specification allows; a browser that fetches over the network will block in the hook, or wrap
  the engine's loader in a job of its own). `new Function("return import(x)")` resolves `x` against the name "anonymous",
  not the module that called `new Function` (eval inside a module is right).

### Direct eval

A function that contains a direct `eval(...)` keeps every binding it can see in a variable cell (and its parents' too), and
each call site records a descriptor in the bytecode (`KzVarName`: name, kind `VK_*`, where the cell is, and `outer`): the
visible bindings innermost first, then the function's eval var object after the bindings of its scope. At run time
`kz_eval_direct` (`eval.c`) turns the descriptor into a chain of synthesized scopes (`build_caller`: one flat scope per
stretch between `with` objects and var objects, bindings prepended so the innermost of equally named ones is found first, a
marker scope for each function's var object, so that a name is looked up in the object after the function's bindings and
before the outer ones), parses and compiles the eval code with that chain as its enclosing function (`FN_SYNTH_CALLER`;
references to the caller's names become closure variables and the closure's cells are copied from the caller's frame).
A sloppy eval's `var` and function names are taken out of the eval's own scope: one the caller's function already has
(`!outer`) is that variable; otherwise it becomes a property of the function's var object, made by the eval's prologue
(`OP_eval_decl`), which the function's own code and every nested function consult by name (`WK_EVAL` entries of an
identifier's dynamic list). `outer` marks bindings of functions the eval code does not extend the variable environment of
(an enclosing function's), `VK_FUNCOBJ` hands a derived constructor's function object to eval code that calls `super()`,
and only the innermost non-arrow function's `this`, `new.target`, home object and constructor are listed (an enclosing
method's are shadowed by the function's own). A function with parameter expressions has two such objects: the body's, and
one in the parameters' scope for the evals in the parameter list (`FuncInfo.eval_in_params`); the entries of the parameters carry
`paramenv`, and a var of eval code that comes before the var object in the descriptor and has the name of a parameter or
of `arguments` is a SyntaxError (the specification's check of the environments between the eval and its variable environment).

## Limits

* Memory: `JSRuntimeOptions.memory_limit` (the host tool's `--mem MB`); exceeding it throws a RangeError "out of
  memory" that scripts can catch, and the runtime stays usable.
* Calls: 10000 nested calls by default (`max_call_depth`); the VM value stack is 1 MiB by default; native
  recursion is bounded by `c_stack_size` (512 KiB) and ends in RangeError "Maximum call stack size exceeded".
* Strings: up to 2^30 - 1 code units. Arrays: the specification's 2^32 - 1. Map/Set: 2^27 entries.
* Interrupts: `js_runtime_set_interrupt_handler`; an interrupt cannot be caught by `try/catch`.

## Tests, conformance and speed

`tests/host/test_js.py` builds `jstool` with AddressSanitizer and UBSan and runs: the corpus (`tests/js/*.js`
against `.expected` files: language features, builtins, async), the collector stress run (`gc_stress.js` with a
collection at every allocation), a table of programs that must be SyntaxErrors and of runtime errors, the limits
(memory, recursion through JS and through natives, deep nesting, interrupts), and the benchmarks on an optimized
build.

Conformance, test262 (the owner allowed the download; the lead's clone is at `build/test262` of the main checkout and the
runner takes `--dir`, `$TEST262_DIR` or finds it): `python tests/host/test262.py [dir...] [--list fail] [--json out.json
--compare before.json]` runs every test of `test/language`, `test/built-ins`, `test/annexB` and `test/harness` in a fresh
runtime and realm (`tests/host/t262tool.c`, sloppy and strict as the flags say, the harness files, `$262`, async tests);
`features:` the engine lacks are skipped and counted apart. Result at the end of the second round (2026-11-03, 136 s, 80596 runs;
the first full run of the same day, before modules, iterator helpers, `Array.fromAsync`, `Promise.try` and the eval, iterator-closing
and declaration fixes, had 39211 passing of 40700 and 96.3%; the last run fixes 1431 more tests and loses none):

| area | pass | fail | skipped | rate |
|---|---|---|---|---|
| test/built-ins | 17824 | 246 | 5789 | 98.6% |
| test/language | 22648 | 339 | 845 | 98.5% |
| test/annexB | 870 | 155 | 61 | 84.9% |
| test/harness | 116 | 0 | 0 | 100.0% |
| **all** | **41458** | **740** | **6695** | **98.2%** (no crashes, no timeouts) |

Skipped (by `features:`; see `UNSUPPORTED_FEATURES` in `tests/host/test262.py`): Temporal 4619, explicit-resource-management 430,
import-defer and source-phase-imports 251 each, Atomics.waitAsync 101 (a second agent is needed by 109 Atomics tests:
`$262.agent` cannot start one), import-attributes 96, await-dictionary 89, iterator-chunking and joint-iteration 82 each,
upsert 72, uint8array-base64 68, immutable-arraybuffer 66, json-parse-with-source and error-stack-accessor (not done, see the next
steps), Float16Array, Intl, ShadowRealm and a few others. Every directory of Date, Proxy, Reflect, BigInt, ArrayBuffer,
SharedArrayBuffer, DataView, Atomics, TypedArray(Constructors), Iterator, Array, String, JSON, Error, Promise (98.9%), the
modules, `language/eval-code` (98.6%), `language/block-scope` is at 98-100%, except RegExp (89.6%: 195 of the 196 failures are
`property-escapes/generated` and `unicodeSets`, tests for Unicode 16 and 17 against our 15.1 tables). What is left, by size
(the first three are the next steps above): `built-ins/RegExp` 195 (Unicode data), `annexB/language/eval-code` 152
(Annex B.3.3.3 in eval code), `language/expressions/compound-assignment` 78 and the update and logical-assignment
expressions 30 (references under `with`), `language/statements/for-await-of` 35 and `for-of` 34 and
`language/expressions/assignment` 22 (iterator closing of destructuring assignments in the loop head), `class` 27 (private
brand checks across realms, field names `constructor`/`prototype`, early errors), `built-ins/Function` 25 (`caller` and `arguments`
of strict functions), `language/literals/string` 10, `language/statements/with` 9.

The corpus checks exact outputs, including number formatting edge cases and Unicode case mapping.

Result on 2026-11-03 (end of the second round): `python tools/hosttest.py discover -s tests/host -p "test_js*.py"` runs 102
tests (the engine under AddressSanitizer and UBSan: `test_js.py` 34, the modules 28, the applet 38 with pipes and a pseudo
terminal, BigInt and RegExp against Python), all pass in about 4.5 minutes, with the stress run collecting at every
allocation for `gc_stress.js` and at every 211th allocation for the rest of the corpus (collecting at every
allocation over the whole corpus takes far more than 5 minutes per file; the parts that finished matched).

OS build: `python tools/build.py` builds `build/lib/libjs.a` with the userland flags (clang, no new warnings) and the `js`
applet links it into kappabox. Size, measured by building with and without `js.c` at the first cut (2026-11-03): stripped
`kappabox` 902,280 -> 1,823,896 bytes (**+0.88 MiB**: the engine 0.8 MiB, of which the Unicode tables 173 KB, the compiler 97 KB,
the parser 49 KB, the VM 44 KB; the applet and the console prelude about 60 KB), `initrd.img` 15,769,600 -> 16,742,400 bytes
(**+0.93 MiB**). With everything of the second round (modules, iterator helpers, `Array.fromAsync`, the eval work) the stripped
`kappabox` is 1,873,048 bytes (**+0.93 MiB** over the build without the applet) and `initrd.img` 16,875,520 bytes (16.1 MiB; the
engine and the applet cost about 1.05 MiB of it). The three kernel+initrd sets on the installed ESP therefore need about 3 MiB
more than without the engine; the build says "size kernel.elf + initrd.img = 16.9 MiB; a push to the installed 64 MiB ESP
needs three sets to fit: fine". Cheap savings not taken (they touch `tools/build.py`'s flags, which
this round does not own): building `libjs` with `-Os` (about -15% of its text), and dropping the property tables of
scripts nobody needs. Every external symbol the engine needs is in libgap; the applet uses `signal`, `nanosleep`,
`clock_gettime`, `localtime_r` (with `tm_gmtoff` and `tm_zone`), `termios` and `environ`, all there.

## The js applet

`js [-s] [-m MB] [-t SECONDS] FILE [ARG...] | -e CODE | -p CODE | - | -i` (the manual page has the full text): a script, code
from the command line, a program on standard input, or an interactive session with line editing (arrows, Ctrl-A/E/K/U/W/Y/L, history in
`~/.js_history`, multi-line input when the compiler says the input ended too soon, `_` for the last value, `.help .exit .clear
.load`). The program sees `console` (written in JavaScript: `user/bin/kappabox/js_prelude.h`, an `util.inspect` that follows Node's
rules: depth 2, width 80, number grouping in columns, `<ref *1>`; `%s %d %i %f %j %o %O` formats; group, count, time, assert, trace),
`print`, `process` (`argv env exit exitCode cwd platform stdout.write stderr.write nextTick`), timers and `queueMicrotask`, and
`readFile writeFile readLine readStdin sleep`. After the program the event loop runs timers and promise jobs until nothing is left.
Exit status: 0, 1 for an uncaught error or unhandled rejection, 2 for usage errors, `process.exit(n)`, 124 for the time limit, 130
for Ctrl-C. `-m` (default 64 MB) is the engine's memory limit; `-t` is a deadline for the whole run (for each input in the session), checked by the
engine's interrupt handler, in `sleep`, and while waiting for timers. Ctrl-C sets a flag that the same handler reads, so it stops loops and
regular expressions too. The C stack the engine may use (512 KiB) fits the 8 MiB the kernel gives a process (`USER_STACK_MAX`).
The engine's `Atomics.wait` sleeps through the host hook `JSHostHooks.sleep_ms`, which the applet provides (`nanosleep`).
Modules: a file ending in `.mjs`, or any file with `--module` (also `-e` and `-`), is run as an ES module; its name is the absolute
normalized path, `import` of a path (absolute, `./`, `../`; no bare names) reads the file relative to the importing module through
`JSHostHooks.load_module`, `import.meta` has `url`, `filename` and `dirname`, scripts can `import()` too. The promise of the main
module is watched by the event loop: a rejection ends the run with status 1 (reported once), a promise still pending when no job
and no timer is left is "the module never finished" with status 13 (Node's code).

Speed (host, x86-64 under WSL, optimized `-O2` build, wall time including start-up):

| benchmark | what | time |
|---|---|---|
| bench_fib.js | fib(27), about 300k calls | 0.03 s |
| bench_strings.js | 200k joins, 100k concatenations | 0.05 s |
| bench_json.js | 5 round trips of 20k objects | 0.16 s |
| bench_sort.js | 200k numbers with a comparator, 50k strings | 0.13-0.19 s |
| bench_raytrace.js | 160x120 ray tracer, objects and floats | 0.10 s |

Commands:

```
python tools/hosttest.py discover -s tests/host -p "test_js.py" -v     # the whole host suite (about 1 minute cold)
build/host-js-wsl/jstool tests/js/language.js                           # one script (under WSL)
build/host-js-wsl/jstool --stress tests/js/gc_stress.js                 # collector stress (--stress-every N)
build/host-js-wsl/jstool --mem 16 --time-limit 1000 -e "code"           # limits
```

Known gaps (the bugs listed here earlier - the TDZ of `for (let x of x)`, `super` in `eval`, an `eval` inside an `eval`, closing
an async iterator - are fixed): the C stack budget (`c_stack_size`, 512 KiB) must fit the thread's real stack (the applet
must check what the OS gives a process); eval code in a function: a `var` that redeclares a `catch` parameter of the
function is a SyntaxError (Annex B.3.4 allows it; at script level it works) and function declarations in blocks of eval
code are not hoisted (Annex B.3.3.3: about 150 tests of `annexB/language/eval-code`); `new Function("return import(x)")` resolves
against the name "anonymous"; in `with (o) { var x = init }` the binding is looked up after `init` is evaluated, not before.
