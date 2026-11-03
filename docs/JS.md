# Kosuzu: the TouhouOS JavaScript engine

Kosuzu (after Kosuzu Motoori of the Suzunaan book rental, who can read any writing) is an ECMAScript engine written
from scratch for TouhouOS: a lexer and recursive-descent parser, a bytecode compiler, a stack VM with inline caches,
a precise garbage collector, the standard library, and a C embedding API (`user/lib/js/include/js.h`). It has no
JIT. It is a static library (`user/lib/js`), built for the OS and for the host test tool (`tests/host/jstool.c`).

## State when paused / how to resume

**Paused on 2026-11-03** (owner's usage budget). Branch `agent/js`; everything listed as solid is committed and
tested.

Solid (built clean under AddressSanitizer + UBSan, covered by `tests/host/test_js.py` and `tests/js/*`):

* The language core: all of ES2024 syntax except modules (regular expression literals parse, but are not yet
  syntax-checked or executable, and BigInt literals throw when evaluated; see below). Strict and sloppy mode, `with`, direct and indirect `eval`
  (with the caller's scopes), destructuring, spread, default and rest parameters, classes with fields, static
  blocks, private methods/accessors/fields and `#x in o`, getters/setters, computed keys, optional chaining,
  nullish coalescing, logical assignment, labelled statements, `try/catch/finally` with completion values,
  generators, async functions, async generators, `for await`, tagged templates, Annex B function hoisting.
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
  (`parseInt`, `parseFloat`, URI functions, `escape`/`unescape`).

Partial or missing (each throws a clear TypeError "... is not supported yet" rather than misbehaving):

* **RegExp**: not implemented yet. Literals are parsed but executing one (or `new RegExp`) throws. The Unicode
  data it needs is in place (`kz_unicode_property` for `\p{...}`, `kz_unicode_case_closure` for case-insensitive
  classes, `kz_unicode_fold_simple`); `String.prototype.match/replace/split/search/matchAll` already dispatch to
  `Symbol.match` etc. and work with string arguments. The design for the engine is written below.
* **Date**, **Proxy/Reflect**, **ArrayBuffer/typed arrays/DataView/Atomics**, **BigInt** (literals throw),
  **modules** (`import`/`export`, `import()`, `import.meta`), **Intl**: not started (stubs in `src/stubs.c`).
* `Array.fromAsync` and the ES2025 iterator helpers: not done.
* The `js` kappabox applet (`user/bin/kappabox/js.c`) and the QEMU test (`tests/case_js.py`): not written.
* test262: not run (no copy was fetched; fetching it needs the owner's permission for a download).

Next three steps:

1. **RegExp** (`src/regexp.c`): the backtracking compiler/matcher designed below, then `RegExp.prototype` and the
   `Symbol.match/replace/split/search/matchAll` methods (the String side is done). This unlocks most real scripts.
2. **Proxy/Reflect and Date**, then **typed arrays + BigInt** (the hooks exist: `kz_exotic[]`, `kz_bigint_*`,
   `kz_typed_*` are already called by the VM and the object model).
3. **The applet and test262**: `user/bin/kappabox/js.c` (REPL, script runner, Ctrl-C via the interrupt handler,
   `localtime_r` for the time zone), `tests/case_js.py` through `tools/qemu-slot.py`, and a test262 runner in
   `tests/host/test_js.py` once a copy is available under `build/test262/`.

How to resume: read this file, `user/lib/js/src/jsint.h` (the internal model and every module's prototypes), and
`src/stubs.c` (exactly what is missing). Run `python tools/hosttest.py discover -s tests/host -p "test_js.py" -v`.
Add a module by deleting its stubs and writing the real file; `kz_init_<module>` is already called in order by
`kz_realm_init` (`src/realm.c`).

## Supported language and library

Target: ECMAScript 2024 (ES15), with Annex B (web compatibility) for sloppy scripts, and a few ES2025 additions
(Set methods, `Promise.withResolvers` is ES2024). Unicode data: UCD 15.1 (`user/lib/js/gen_unicode.py` generates
`src/unicode_tables.h` from `/usr/share/unicode`; the tables take about 143 KiB).

Not supported, by design: the DOM, a browser `window`, networking, WebAssembly, a JIT. `SharedArrayBuffer`
semantics will be single-threaded (no Atomics.wait blocking).

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

### RegExp design (to implement)

A pattern parser (with Annex B rules outside `u`/`v` mode, a pre-scan for group count and names) builds a node
tree; a compiler emits word-coded instructions for a backtracking matcher with an explicit backtrack stack (no C
recursion): characters and classes (with direction for lookbehind, which is compiled right to left), saves,
splits, counted loops with registers (empty-iteration check, capture reset per iteration), a fast loop for
single-character atoms, lookaround barriers that keep capture undo records, backreferences. Case-insensitive
classes are closed under canonicalization with `kz_unicode_case_closure`; `\p{...}` comes from
`kz_unicode_property`. The interrupt handler is polled during long matches.

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

Conformance: test262 has not been run (no copy available offline). The corpus checks exact outputs, including
number formatting edge cases and Unicode case mapping.

Result on 2026-11-03: all 13 tests pass (`Ran 13 tests ... OK`), with the stress run collecting at every
allocation for `gc_stress.js` and at every 211th allocation for the rest of the corpus (collecting at every
allocation over the whole corpus takes far more than 5 minutes per file; the parts that finished matched).

OS build: `python tools/build.py` builds `build/lib/libjs.a` warning-free with the userland flags; every
external symbol it needs (`malloc realloc free memchr memcmp memcpy memmove memset qsort strchr strcmp strlen
strncmp ceil floor`) is in libgap. No program links it yet (the applet is not written), so the size cost for
kappabox is not measured; the objects are about 1 MiB before linking, the Unicode tables about 143 KiB of that.

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

Known bugs and gaps found so far: the C stack budget (`c_stack_size`, 512 KiB) must fit the thread's real stack
(the applet must check what the OS gives a process); `for (let x of x)` does not apply the TDZ to the right-hand
side; an `eval` inside an `eval` does not see the outer eval's `var` object; `super()` inside `eval` is not
supported; closing an async iterator after a throw does not await `return()`.
