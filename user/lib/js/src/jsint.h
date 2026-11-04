/*
 * jsint.h: the internals of Kosuzu, the TouhouOS JavaScript engine.  Everything the modules share: the value
 * encoding, the cells of the garbage-collected heap, strings, shapes and objects, bytecode, frames, the runtime.
 *
 * Specification followed throughout: ECMA-262 (ECMAScript 2024 Language Specification).  Section names in comments
 * ("OrdinaryGet", "ToPrimitive") are the specification's abstract operations.
 *
 * Naming: kz_* are internal functions shared between files (the public API is js_* in js.h); KZ_* are internal
 * constants; JV_* are the value-encoding macros.
 */
#ifndef JSINT_H
#define JSINT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include "js.h"

#define KZ_INLINE static inline __attribute__((always_inline))
#define KZ_NOINLINE __attribute__((noinline))
#define kz_likely(x) __builtin_expect(!!(x), 1)
#define kz_unlikely(x) __builtin_expect(!!(x), 0)
#define KZ_COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

typedef struct KzVec { JSValue *v; uint32_t len, cap; } KzVec;     /* a growable value array (see KZ_ROOT_VEC) */
typedef struct JSObject JSObject;
typedef struct JSString JSString;
typedef struct JSSymbol JSSymbol;
typedef struct JSShape JSShape;
typedef struct JSFunctionBytecode JSFunctionBytecode;
typedef struct JSVarCell JSVarCell;
typedef struct JSFrame JSFrame;
typedef struct JSBigInt JSBigInt;
typedef struct KzModule KzModule;

/* ================================================================== values
 *
 * NaN boxing.  A JSValue is a double unless its top 16 bits are 0xFFF9..0xFFFF, which no double the engine keeps can
 * have: every NaN is stored as one of the two x86 quiet NaNs 0x7FF8.. or 0xFFF8.. (the "default NaN" x86 arithmetic
 * produces), so the negative quiet-NaN space above 0xFFF8 is free for tags.  NaNs with other payloads can only come
 * from raw bits (typed arrays, DataView) and are canonicalised there (jv_from_raw_double).
 *
 *   tag 0xFFF9  int32 in the low 32 bits        (numbers have two forms: an int32 fast path and doubles)
 *   tag 0xFFFA  undefined, null, false, true and internal markers in the low bits
 *   tag 0xFFFB  string    (48-bit pointer to a JSString)
 *   tag 0xFFFC  symbol    (JSSymbol)
 *   tag 0xFFFD  object    (JSObject)
 *   tag 0xFFFE  bigint    (JSBigInt)
 *   tag 0xFFFF  internal heap pointer (accessor pairs and var cells in property slots, bytecode in constant pools);
 *               never visible to JavaScript
 * User-space pointers on x86-64 fit in 47 bits.
 */
#define JV_TAG_INT     0xFFF9u
#define JV_TAG_SPECIAL 0xFFFAu
#define JV_TAG_STRING  0xFFFBu
#define JV_TAG_SYMBOL  0xFFFCu
#define JV_TAG_OBJECT  0xFFFDu
#define JV_TAG_BIGINT  0xFFFEu
#define JV_TAG_PTR     0xFFFFu

#define JV_TAG(v)        ((uint32_t)((uint64_t)(v) >> 48))
#define JV_MK(tag, p)    ((JSValue)(((uint64_t)(tag) << 48) | (uint64_t)(p)))
#define JV_PAYLOAD(v)    ((uint64_t)(v) & 0xFFFFFFFFFFFFULL)
#define JV_IS_DOUBLE(v)  ((uint64_t)(v) < ((uint64_t)JV_TAG_INT << 48))
#define JV_IS_INT(v)     (JV_TAG(v) == JV_TAG_INT)
#define JV_IS_NUMBER(v)  ((uint64_t)(v) < ((uint64_t)JV_TAG_SPECIAL << 48))
#define JV_IS_HEAP(v)    ((uint64_t)(v) >= ((uint64_t)JV_TAG_STRING << 48))
#define JV_IS_STRING(v)  (JV_TAG(v) == JV_TAG_STRING)
#define JV_IS_SYMBOL(v)  (JV_TAG(v) == JV_TAG_SYMBOL)
#define JV_IS_OBJECT(v)  (JV_TAG(v) == JV_TAG_OBJECT)
#define JV_IS_BIGINT(v)  (JV_TAG(v) == JV_TAG_BIGINT)
#define JV_IS_INTERNAL(v) (JV_TAG(v) == JV_TAG_PTR)
#define JV_PTR(v)        ((void *)(uintptr_t)JV_PAYLOAD(v))
#define JV_OBJ(v)        ((JSObject *)JV_PTR(v))
#define JV_STR(v)        ((JSString *)JV_PTR(v))
#define JV_SYM(v)        ((JSSymbol *)JV_PTR(v))
#define JV_BIG(v)        ((JSBigInt *)JV_PTR(v))
#define JV_INT(v)        ((int32_t)(uint32_t)(v))
#define JV_MKINT(i)      JV_MK(JV_TAG_INT, (uint32_t)(int32_t)(i))
#define JV_MKOBJ(p)      JV_MK(JV_TAG_OBJECT, (uintptr_t)(p))
#define JV_MKSTR(p)      JV_MK(JV_TAG_STRING, (uintptr_t)(p))
#define JV_MKSYM(p)      JV_MK(JV_TAG_SYMBOL, (uintptr_t)(p))
#define JV_MKBIG(p)      JV_MK(JV_TAG_BIGINT, (uintptr_t)(p))
#define JV_MKPTR(p)      JV_MK(JV_TAG_PTR, (uintptr_t)(p))
#define JV_MKBOOL(b)     ((b) ? JS_TRUE : JS_FALSE)
#define JV_IS_BOOL(v)    (((v) | 1) == JS_TRUE)
#define JV_IS_NULLISH(v) (((v) | 1) == JS_NULL)       /* undefined or null */

/* internal markers (never seen by scripts) */
#define JS_HOLE      JV_MK(JV_TAG_SPECIAL, 4)    /* a missing element in a dense array */
#define JS_UNINIT    JV_MK(JV_TAG_SPECIAL, 5)    /* a let/const/class binding in its temporal dead zone */
#define JS_NOTFOUND  JV_MK(JV_TAG_SPECIAL, 7)    /* a lookup found nothing */
#define JS_EMPTY     JV_MK(JV_TAG_SPECIAL, 8)    /* a deleted shape entry, an empty map bucket */
#define KZ_NAN_BITS  0x7FF8000000000000ULL
#define KZ_INFINITY  __builtin_inf()

KZ_INLINE double jv_dbl(JSValue v) { double d; memcpy(&d, &v, 8); return d; }
KZ_INLINE JSValue jv_from_dbl(double d) { JSValue v; memcpy(&v, &d, 8); return v; }   /* d is a canonical value */
KZ_INLINE JSValue jv_from_raw_double(double d) { return d != d ? KZ_NAN_BITS : jv_from_dbl(d); }
KZ_INLINE double jv_num(JSValue v) { return JV_IS_INT(v) ? (double)JV_INT(v) : jv_dbl(v); }
/* A number result: the int form when the value is an int32 (and not -0), else a double. */
KZ_INLINE JSValue jv_number(double d) {
    if (d >= -2147483648.0 && d <= 2147483647.0) {
        int32_t i = (int32_t)d;
        if ((double)i == d && (i != 0 || !__builtin_signbit(d))) return JV_MKINT(i);
    }
    return jv_from_raw_double(d);
}
KZ_INLINE JSValue jv_uint32(uint32_t u) { return u <= 0x7FFFFFFFu ? JV_MKINT((int32_t)u) : jv_from_dbl((double)u); }
KZ_INLINE JSValue jv_int64(int64_t i) { return (i >= INT32_MIN && i <= INT32_MAX) ? JV_MKINT((int32_t)i) : jv_from_dbl((double)i); }

/* ================================================================== heap cells
 *
 * Every garbage-collected thing starts with this 8-byte header.  The allocator (gc.c) hands out cells from 64 KiB
 * pages in size classes (and big ones straight from the system allocator); the collector is a precise, non-moving
 * mark-sweep with an explicit mark stack (no recursion, however deep the object graph).
 */
typedef struct JSGCHeader {
    uint8_t gctype;         /* KzGCType */
    uint8_t mark;           /* set while marking; cleared by the sweep */
    uint16_t flags;         /* meaning depends on the type */
    uint32_t aux;           /* objects: the class id; strings: the length */
} JSGCHeader;

typedef enum KzGCType {
    KZ_GC_FREE = 0,         /* a free cell on a free list */
    KZ_GC_RAW,              /* a plain buffer (kz_malloc): never traced, freed by its owner */
    KZ_GC_STRING,
    KZ_GC_SYMBOL,
    KZ_GC_OBJECT,
    KZ_GC_SHAPE,
    KZ_GC_BYTECODE,
    KZ_GC_VARCELL,
    KZ_GC_ACCESSOR,
    KZ_GC_BIGINT,
    KZ_GC_SOURCE,           /* the text of a script, for Function.prototype.toString */
    KZ_GC_MODULE,
    KZ_GC_TEMPLATE,         /* a cached template literal object holder */
    KZ_GC_TYPE_COUNT
} KzGCType;

/* ================================================================== strings
 *
 * UTF-16 semantics.  A flat string holds its code units inline, one byte each when every unit is <= 0xFF
 * ("Latin-1", the common case) or two bytes each (STR_WIDE).  Concatenations of long strings build ropes (a node with
 * two halves) that are flattened on first use into an out-of-line buffer (STR_EXT); after that the node is an
 * ordinary flat string.  Atoms are interned flat strings: two atoms are equal iff their pointers are.
 */
#define STR_WIDE    0x01
#define STR_ATOM    0x02
#define STR_ROPE    0x04
#define STR_EXT     0x08
#define STR_HASHED  0x10
#define STR_PERM    0x20    /* predefined: never collected */
#define STR_NOTIDX  0x40    /* known not to be a canonical array index */
#define KZ_STRING_MAX 0x3FFFFFE0u

struct JSString {
    JSGCHeader h;           /* h.aux = length in code units */
    uint32_t hash;
    uint32_t depth;         /* ropes: depth of the tree (for flattening heuristics) */
    union {
        uint8_t c8[8];      /* inline data, really [length] (flexible) */
        uint16_t c16[4];
        struct { JSString *left, *right; } rope;
        void *ext;
    } u;
};
#define KZ_STR_HDR offsetof(JSString, u)

KZ_INLINE uint32_t kz_str_len(const JSString *s) { return s->h.aux; }
KZ_INLINE bool kz_str_wide(const JSString *s) { return s->h.flags & STR_WIDE; }
/* only on flat strings (kz_str_flat) */
KZ_INLINE const uint8_t *kz_str8(const JSString *s) { return (s->h.flags & STR_EXT) ? (const uint8_t *)s->u.ext : s->u.c8; }
KZ_INLINE const uint16_t *kz_str16(const JSString *s) { return (s->h.flags & STR_EXT) ? (const uint16_t *)s->u.ext : s->u.c16; }
KZ_INLINE uint32_t kz_str_at(const JSString *s, uint32_t i) { return kz_str_wide(s) ? kz_str16(s)[i] : kz_str8(s)[i]; }

/* ================================================================== symbols */
#define SYM_REGISTERED 0x01     /* Symbol.for */
#define SYM_PRIVATE    0x02     /* a class private name (#x): never listed, never proxied */
#define SYM_WELLKNOWN  0x04
struct JSSymbol {
    JSGCHeader h;
    uint32_t hash;
    uint32_t pad;
    JSValue description;        /* a string or undefined */
};

/* ================================================================== property keys
 *
 * A property key is a JSValue: an atom (string), a symbol, or an array index 0..2^32-2 stored with the int tag
 * (payload read as unsigned).  ToPropertyKey turns "12" into the index 12 and 1.5 into the atom "1.5", so a key
 * compares equal iff the 64-bit values are equal.
 */
#define KZ_KEY_IS_INDEX(k)  JV_IS_INT(k)
#define KZ_KEY_INDEX(k)     ((uint32_t)(k))
#define KZ_KEY_FROM_INDEX(i) JV_MK(JV_TAG_INT, (uint32_t)(i))
#define KZ_MAX_INDEX 0xFFFFFFFEu

/* ================================================================== shapes and objects
 *
 * An object's property layout and prototype live in its shape; the values live in the object's slot array, in the
 * shape's order.  Shapes are shared: adding key K to shape S gives the same child shape for every object (the
 * transition tree, weak in the child direction).  Objects with many properties, or after a delete, own a private
 * "dictionary" shape that is changed in place (its `version` counts structural changes so inline caches stay
 * honest).  Integer-indexed properties of ordinary objects and arrays live in a dense element vector while they are
 * plain data properties; anything else (sparse, accessors, read-only) moves them into the shape.
 */
#define KZ_PROP_W        JS_PROP_WRITABLE
#define KZ_PROP_E        JS_PROP_ENUMERABLE
#define KZ_PROP_C        JS_PROP_CONFIGURABLE
#define KZ_PROP_WEC      (KZ_PROP_W | KZ_PROP_E | KZ_PROP_C)
#define KZ_PROP_ACCESSOR (1 << 3)

typedef struct JSShapeProp { JSValue key; uint32_t attrs; uint32_t hnext; } JSShapeProp;
typedef struct JSShapeTrans { JSValue key; uint32_t attrs; JSShape *child; } JSShapeTrans;

#define SHAPE_DICT      0x01
#define SHAPE_HAS_INDEX 0x02    /* some key is an array index */
#define SHAPE_PERM      0x04

struct JSShape {
    JSGCHeader h;
    uint32_t nprops;            /* entries used (dictionary: including deleted ones) */
    uint32_t ndeleted;
    JSValue proto;              /* an object or null */
    JSShape *parent;            /* transition-tree parent (strong) */
    JSShapeProp *props;
    uint32_t prop_cap;
    uint32_t hash_size;         /* 0 (linear search) or a power of two */
    uint32_t *hash;             /* bucket -> prop index + 1 */
    uint32_t version;           /* dictionary shapes: bumped on every structural change */
    uint32_t ntrans, trans_cap;
    JSShapeTrans *trans;        /* children (weak) */
};

typedef struct JSAccessor { JSGCHeader h; JSValue getter, setter; } JSAccessor;
struct JSVarCell { JSGCHeader h; JSValue value; };

typedef enum KzClassId {
    KZ_CLASS_OBJECT = 1,
    KZ_CLASS_ARRAY,
    KZ_CLASS_ERROR,
    KZ_CLASS_FUNCTION,          /* bytecode closure */
    KZ_CLASS_NATIVE,            /* native function */
    KZ_CLASS_BOUND,
    KZ_CLASS_BOOLEAN,
    KZ_CLASS_NUMBER,
    KZ_CLASS_STRING,
    KZ_CLASS_SYMBOL,
    KZ_CLASS_BIGINT,
    KZ_CLASS_DATE,
    KZ_CLASS_REGEXP,
    KZ_CLASS_ARGUMENTS,         /* unmapped */
    KZ_CLASS_MAPPED_ARGUMENTS,
    KZ_CLASS_MAP,
    KZ_CLASS_SET,
    KZ_CLASS_WEAKMAP,
    KZ_CLASS_WEAKSET,
    KZ_CLASS_WEAKREF,
    KZ_CLASS_FINREG,
    KZ_CLASS_PROMISE,
    KZ_CLASS_PROXY,
    KZ_CLASS_ARRAYBUFFER,
    KZ_CLASS_SHAREDARRAYBUFFER,
    KZ_CLASS_UINT8C_ARRAY,      /* the typed arrays, in KzTypedKind order */
    KZ_CLASS_INT8_ARRAY,
    KZ_CLASS_UINT8_ARRAY,
    KZ_CLASS_INT16_ARRAY,
    KZ_CLASS_UINT16_ARRAY,
    KZ_CLASS_INT32_ARRAY,
    KZ_CLASS_UINT32_ARRAY,
    KZ_CLASS_FLOAT32_ARRAY,
    KZ_CLASS_FLOAT64_ARRAY,
    KZ_CLASS_BIGINT64_ARRAY,
    KZ_CLASS_BIGUINT64_ARRAY,
    KZ_CLASS_DATAVIEW,
    KZ_CLASS_GENERATOR,
    KZ_CLASS_ASYNC_GENERATOR,
    KZ_CLASS_ASYNC_FUNCTION,    /* the state of a running async function (internal) */
    KZ_CLASS_ARRAY_ITERATOR,
    KZ_CLASS_STRING_ITERATOR,
    KZ_CLASS_MAP_ITERATOR,
    KZ_CLASS_SET_ITERATOR,
    KZ_CLASS_REGEXP_STRING_ITERATOR,
    KZ_CLASS_FOR_IN_ITERATOR,
    KZ_CLASS_ASYNC_FROM_SYNC_ITERATOR,
    KZ_CLASS_ITERATOR_HELPER,
    KZ_CLASS_MODULE_NS,
    KZ_CLASS_GLOBAL,
    KZ_CLASS_HOST,              /* embedder classes: the def is in u.host */
    KZ_CLASS_COUNT
} KzClassId;

#define KZ_IS_TYPED_ARRAY(c) ((c) >= KZ_CLASS_UINT8C_ARRAY && (c) <= KZ_CLASS_BIGUINT64_ARRAY)

/* object flags (h.flags) */
#define OBJ_EXTENSIBLE   0x0001
#define OBJ_CALLABLE     0x0002
#define OBJ_CONSTRUCTOR  0x0004
#define OBJ_EXOTIC       0x0008     /* has KzExotic hooks (u depends on the class) */
#define OBJ_ELEMS_SEALED 0x0010     /* elements are non-configurable */
#define OBJ_ELEMS_FROZEN 0x0020     /* elements are non-writable too */
#define OBJ_LEN_RO       0x0040     /* arrays: length is not writable */
#define OBJ_CLASS_CTOR   0x0080     /* a class constructor: [[Call]] throws */
#define OBJ_HTMLDDA      0x0100     /* (unused) */
#define OBJ_IS_PROTO     0x0200     /* used as some shape's prototype */
#define OBJ_HAS_ELEMS    0x0400     /* elems has been allocated at some point */
#define OBJ_IMMUTABLE_PROTO 0x0800  /* Object.prototype: [[SetPrototypeOf]] only to the same value */

typedef struct KzMapRecord KzMapRecord;
typedef struct KzMapData KzMapData;
typedef struct KzGenerator KzGenerator;
typedef struct KzPromiseReaction KzPromiseReaction;
typedef struct KzRegExpCode KzRegExpCode;

typedef JSValue KzNative(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic);

typedef struct KzArrayBuffer {
    uint8_t *data;
    size_t len, max_len;
    bool detached, resizable, shared, external;
} KzArrayBuffer;

struct JSObject {
    JSGCHeader h;               /* aux = KzClassId, flags = OBJ_* */
    JSShape *shape;
    JSValue *slots;             /* values of the shape's properties */
    JSValue *elems;             /* dense elements [0, elem_len); JS_HOLE marks a missing one */
    uint32_t slot_cap, elem_len, elem_cap, pad;
    union {
        struct { uint32_t length; } array;
        struct {                /* KZ_CLASS_FUNCTION */
            JSFunctionBytecode *b;
            JSVarCell **cells;  /* the captured variables, b->ncvars of them */
            JSValue home;       /* the [[HomeObject]] for super */
            JSValue fields;     /* class constructors: the field initialiser function, or undefined */
            JSContext *realm;
        } func;
        struct {                /* KZ_CLASS_NATIVE */
            KzNative *fn;
            JSContext *realm;
            JSValue *data;      /* closure values (traced) */
            void *host_fn;      /* js_new_function's function and data pointer */
            void *host_data;
            uint16_t ndata;
            int16_t magic;
            uint8_t nargs;      /* argv holds at least this many values */
        } native;
        struct { JSValue target, this_val; JSValue *args; uint32_t argc; } bound;
        JSValue prim;           /* Boolean, Number, String, Symbol, BigInt wrappers */
        double date;
        struct { KzRegExpCode *code; JSValue source, flags; } regexp;
        KzMapData *map;
        struct { JSValue target, handler; } proxy;      /* handler null: revoked */
        KzArrayBuffer ab;
        struct {                /* typed arrays and DataView */
            JSValue buffer;
            size_t offset, length;      /* length in elements (DataView: bytes) */
            bool track;                 /* length tracks a resizable buffer */
        } ta;
        KzGenerator *gen;
        struct { KzPromiseReaction *reactions; JSValue result; uint8_t state; bool handled; } promise;
        struct { JSValue obj; uint32_t index; uint8_t kind; bool done; } iter;
        struct { JSVarCell **cells; uint32_t nmapped; } margs;
        struct { JSValue target; JSValue token; } weakref;     /* WeakRef */
        struct { struct KzFinCell *cells; JSValue callback; } finreg;
        struct { void *opaque; const JSClassDef *def; int class_id; } host;
        struct { KzModule *module; } ns;
        struct { JSValue obj; JSValue keys; uint32_t pos; uint32_t nelems; JSShape *shape; } forin;
        struct { JSValue a, b, c; } any;
    } u;
};
#define KZ_OBJ_BASE offsetof(JSObject, u)

KZ_INLINE KzClassId kz_class(const JSObject *o) { return (KzClassId)o->h.aux; }
KZ_INLINE bool kz_is_callable(JSValue v) { return JV_IS_OBJECT(v) && (JV_OBJ(v)->h.flags & OBJ_CALLABLE); }
KZ_INLINE bool kz_is_constructor(JSValue v) { return JV_IS_OBJECT(v) && (JV_OBJ(v)->h.flags & OBJ_CONSTRUCTOR); }
KZ_INLINE bool kz_is_class(JSValue v, KzClassId c) { return JV_IS_OBJECT(v) && kz_class(JV_OBJ(v)) == c; }

/* property descriptors: value, getter, setter first (callers root them as an array of three) */
#define PD_HAS_VALUE    0x0010
#define PD_HAS_GET      0x0020
#define PD_HAS_SET      0x0040
#define PD_HAS_W        0x0100
#define PD_HAS_E        0x0200
#define PD_HAS_C        0x0400
#define PD_ACCESSOR     KZ_PROP_ACCESSOR
typedef struct KzPropDesc {
    JSValue value, getter, setter;
    uint32_t flags;             /* PD_HAS_* plus the KZ_PROP_W/E/C bits */
} KzPropDesc;

/* exotic objects: hooks tried before the ordinary algorithm; KZ_FALLBACK continues with it */
#define KZ_FALLBACK 2
typedef struct KzExotic {
    int (*get_own)(JSContext *ctx, JSObject *o, JSValue key, KzPropDesc *d);       /* 1 found, 0 no, -1 */
    int (*define_own)(JSContext *ctx, JSObject *o, JSValue key, const KzPropDesc *d);   /* 1 ok, 0 refused */
    int (*has)(JSContext *ctx, JSObject *o, JSValue key);
    int (*get)(JSContext *ctx, JSObject *o, JSValue key, JSValue receiver, JSValue *out);   /* 1 got it */
    int (*set)(JSContext *ctx, JSObject *o, JSValue key, JSValue val, JSValue receiver);    /* 1 ok, 0 refused */
    int (*del)(JSContext *ctx, JSObject *o, JSValue key);                          /* 1 ok, 0 refused */
    int (*own_keys)(JSContext *ctx, JSObject *o, KzVec *keys);                     /* appends, 1 = done (no ordinary keys) */
    int (*get_proto)(JSContext *ctx, JSObject *o, JSValue *out);
    int (*set_proto)(JSContext *ctx, JSObject *o, JSValue proto);                  /* 1 ok, 0 refused */
    int (*is_extensible)(JSContext *ctx, JSObject *o);
    int (*prevent_ext)(JSContext *ctx, JSObject *o);                               /* 1 ok, 0 refused */
} KzExotic;

/* ================================================================== rooting
 *
 * The collector may run at any allocation.  A C function that holds a JSValue across something that allocates (or
 * calls JavaScript) keeps it in a root: KZ_ROOTS declares an array the collector scans until the end of the C block
 * (the cleanup attribute unlinks it on every exit path).  Parameters are rooted by the caller; return values are not.
 */
typedef struct KzRootFrame {
    struct KzRootFrame *prev;
    JSRuntime *rt;
    JSValue *vals;
    uint32_t n;
    KzVec *vec;
} KzRootFrame;
void kz_root_pop(KzRootFrame *f);
void kz_vec_root_pop(KzRootFrame *f);

#define KZ_ROOTS(ctx, name, count)                                                                  \
    JSValue name[count];                                                                            \
    for (unsigned name##_i = 0; name##_i < (count); name##_i++) name[name##_i] = JS_UNDEFINED;    \
    KzRootFrame name##_rf __attribute__((cleanup(kz_root_pop))) = { (ctx)->rt->roots, (ctx)->rt, name, (count), NULL }; \
    (ctx)->rt->roots = &name##_rf
#define KZ_ROOT(ctx, name)                                                                          \
    JSValue name = JS_UNDEFINED;                                                                    \
    KzRootFrame name##_rf __attribute__((cleanup(kz_root_pop))) = { (ctx)->rt->roots, (ctx)->rt, &name, 1, NULL }; \
    (ctx)->rt->roots = &name##_rf
#define KZ_ROOT_DESC(ctx, d)                                                                        \
    KzPropDesc d = { JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, 0 };                                 \
    KzRootFrame d##_rf __attribute__((cleanup(kz_root_pop))) = { (ctx)->rt->roots, (ctx)->rt, &d.value, 3, NULL }; \
    (ctx)->rt->roots = &d##_rf
/* a growable rooted vector, freed at the end of the block */
#define KZ_ROOT_VEC(ctx, name)                                                                      \
    KzVec name = { NULL, 0, 0 };                                                                    \
    KzRootFrame name##_rf __attribute__((cleanup(kz_vec_root_pop))) = { (ctx)->rt->roots, (ctx)->rt, NULL, 0, &name }; \
    (ctx)->rt->roots = &name##_rf
int kz_vec_push(JSContext *ctx, KzVec *v, JSValue x);    /* 0, or -1 with an out-of-memory exception */

/* ================================================================== bytecode and frames */

typedef struct KzClosureVar {
    uint8_t from_local;         /* 1: the parent's local slot `index` (which holds a JSVarCell); 0: its cell `index` */
    uint8_t pad;
    uint16_t index;
} KzClosureVar;

typedef struct KzExcEntry { uint32_t start, end, handler; uint32_t depth; } KzExcEntry;
typedef struct KzPosEntry { uint32_t pc, line, col; } KzPosEntry;

typedef struct KzIC {
    JSShape *shape;             /* receiver shape; NULL: empty */
    JSObject *holder;           /* NULL: own property, else the prototype that has it */
    JSShape *holder_shape;
    uint32_t version, holder_version;
    uint32_t slot;
    uint32_t flags;             /* IC_ACCESSOR */
} KzIC;
#define IC_ACCESSOR 1

/* a visible name for direct eval and `with` lookups: where the binding lives in the frame */
typedef struct KzVarName {
    JSValue name;
    uint16_t slot;              /* local slot or closure cell index */
    uint8_t where;              /* VN_LOCAL, VN_LOCAL_CELL, VN_CLOSURE */
    uint8_t kind;               /* VK_* (bytecode.h) */
    uint8_t outer;              /* the binding is of a function outside the one whose var scope sloppy eval code declares its vars in */
    uint8_t paramenv;           /* the binding is in the parameter environment of a function with parameter expressions */
} KzVarName;
#define VN_LOCAL      0
#define VN_LOCAL_CELL 1
#define VN_CLOSURE    2

/* function kinds (JSFunctionBytecode.kind) */
enum {
    KZ_FUNC_NORMAL, KZ_FUNC_ARROW, KZ_FUNC_METHOD, KZ_FUNC_GETTER, KZ_FUNC_SETTER, KZ_FUNC_CLASS_CTOR,
    KZ_FUNC_DERIVED_CTOR, KZ_FUNC_SCRIPT, KZ_FUNC_EVAL, KZ_FUNC_MODULE, KZ_FUNC_FIELD_INIT, KZ_FUNC_STATIC_INIT
};
/* bytecode flags (h.flags) */
#define BC_STRICT       0x0001
#define BC_GENERATOR    0x0002
#define BC_ASYNC        0x0004
#define BC_ARGUMENTS    0x0008  /* creates an arguments object */
#define BC_MAPPED_ARGS  0x0010  /* ... a mapped one */
#define BC_HAS_EVAL     0x0020  /* contains a direct eval */
#define BC_SIMPLE_PARAMS 0x0040
#define BC_NEEDS_HOME   0x0080
#define BC_CONSTRUCTOR  0x0100  /* may be called with new */
#define BC_HAS_PREFIELDS 0x0200 /* base class constructor runs field initialisers first */
#define BC_REST         0x0400  /* has a rest parameter */

struct JSFunctionBytecode {
    JSGCHeader h;               /* flags = BC_*, aux = kind */
    uint8_t *code;
    uint32_t code_len;
    uint32_t nconsts;
    JSValue *consts;
    KzClosureVar *cvars;
    uint16_t ncvars;
    uint16_t length;            /* the function's "length" */
    uint16_t nparams;           /* formal parameters copied into locals 0..nparams-1 */
    uint16_t nlocals;
    uint16_t stack_size;
    uint16_t nvarnames;
    uint32_t nexc, npos, nic;
    KzExcEntry *exc;
    KzPosEntry *pos;
    KzIC *ic;
    KzVarName *varnames;        /* for direct eval inside this function */
    JSValue name;               /* atom or empty string */
    JSValue filename;
    JSValue source;             /* the script text (KZ_GC_SOURCE) or undefined */
    uint32_t src_start, src_end;
    uint32_t line, col;
    JSContext *realm;
    JSValue module_desc;        /* a module's body: the array describing its requests, imports and exports (compile.c) */
};
KZ_INLINE int kz_bc_kind(const JSFunctionBytecode *b) { return (int)b->h.aux; }

typedef struct KzSource {
    JSGCHeader h;
    uint32_t len;
    uint32_t pad;
    char text[];                /* UTF-8 */
} KzSource;

/* A call frame.  Frames of ordinary calls live on the runtime's value stack (LIFO); generator and async frames are
   heap blocks kept by their generator.  The collector scans every frame's whole register area (locals then operand
   stack), which is filled with undefined when the frame is made, so stale operand-stack slots are always valid. */
#define FRAME_CONSTRUCT 0x01    /* called with new */
#define FRAME_HEAP      0x02    /* a generator/async frame */
#define FRAME_ENTRY     0x04    /* returning from it leaves kz_run */
#define FRAME_NATIVE    0x08    /* a native function's frame (for stack traces) */
#define FRAME_RESUME_THROW 0x10 /* resume by throwing the pending exception at the suspension point */
#define FRAME_THIS_DONE 0x20    /* sloppy this already coerced */

/* why kz_run returned JS_EMPTY */
enum { KZ_SUSPEND_NONE, KZ_SUSPEND_INITIAL, KZ_SUSPEND_YIELD, KZ_SUSPEND_YIELD_RAW, KZ_SUSPEND_AWAIT };

struct JSFrame {
    JSFrame *prev;
    JSFunctionBytecode *b;      /* NULL for native frames */
    JSObject *func;
    const uint8_t *pc;
    JSValue *sp;
    JSValue this_val;
    JSValue new_target;
    JSValue *argv;
    uint32_t argc;
    uint32_t flags;
    uint32_t nregs;
    uint32_t pad;
    JSVarCell **cells;          /* func->u.func.cells */
    JSValue *regs;              /* locals [0, nlocals) then the operand stack */
};

/* ================================================================== the runtime and contexts */

typedef enum KzIntrinsic {
    KZ_I_OBJECT_PROTO, KZ_I_FUNCTION_PROTO, KZ_I_ARRAY_PROTO, KZ_I_STRING_PROTO, KZ_I_NUMBER_PROTO,
    KZ_I_BOOLEAN_PROTO, KZ_I_SYMBOL_PROTO, KZ_I_BIGINT_PROTO, KZ_I_ERROR_PROTO, KZ_I_EVAL_ERROR_PROTO,
    KZ_I_RANGE_ERROR_PROTO, KZ_I_REFERENCE_ERROR_PROTO, KZ_I_SYNTAX_ERROR_PROTO, KZ_I_TYPE_ERROR_PROTO,
    KZ_I_URI_ERROR_PROTO, KZ_I_AGGREGATE_ERROR_PROTO, KZ_I_DATE_PROTO, KZ_I_REGEXP_PROTO, KZ_I_MAP_PROTO,
    KZ_I_SET_PROTO, KZ_I_WEAKMAP_PROTO, KZ_I_WEAKSET_PROTO, KZ_I_WEAKREF_PROTO, KZ_I_FINREG_PROTO,
    KZ_I_PROMISE_PROTO, KZ_I_ARRAYBUFFER_PROTO, KZ_I_SHAREDARRAYBUFFER_PROTO, KZ_I_DATAVIEW_PROTO,
    KZ_I_TYPEDARRAY_PROTO, KZ_I_UINT8C_PROTO, KZ_I_INT8_PROTO, KZ_I_UINT8_PROTO, KZ_I_INT16_PROTO,
    KZ_I_UINT16_PROTO, KZ_I_INT32_PROTO, KZ_I_UINT32_PROTO, KZ_I_FLOAT32_PROTO, KZ_I_FLOAT64_PROTO,
    KZ_I_BIGINT64_PROTO, KZ_I_BIGUINT64_PROTO,
    KZ_I_ITERATOR_PROTO, KZ_I_ASYNC_ITERATOR_PROTO, KZ_I_ARRAY_ITERATOR_PROTO, KZ_I_STRING_ITERATOR_PROTO,
    KZ_I_MAP_ITERATOR_PROTO, KZ_I_SET_ITERATOR_PROTO, KZ_I_REGEXP_STRING_ITERATOR_PROTO, KZ_I_GENERATOR_PROTO,
    KZ_I_ASYNC_GENERATOR_PROTO, KZ_I_GENERATOR_FUNCTION_PROTO, KZ_I_ASYNC_FUNCTION_PROTO,
    KZ_I_ASYNC_GENERATOR_FUNCTION_PROTO, KZ_I_ASYNC_FROM_SYNC_ITERATOR_PROTO, KZ_I_ITERATOR_HELPER_PROTO,
    KZ_I_WRAP_FOR_VALID_ITERATOR_PROTO,
    KZ_I_OBJECT, KZ_I_FUNCTION, KZ_I_ARRAY, KZ_I_STRING, KZ_I_NUMBER, KZ_I_BOOLEAN, KZ_I_SYMBOL, KZ_I_BIGINT,
    KZ_I_PROMISE, KZ_I_REGEXP, KZ_I_DATE, KZ_I_MAP, KZ_I_SET, KZ_I_ARRAYBUFFER, KZ_I_SHAREDARRAYBUFFER, KZ_I_TYPEDARRAY,
    KZ_I_GENERATOR_FUNCTION, KZ_I_ASYNC_FUNCTION, KZ_I_ASYNC_GENERATOR_FUNCTION, KZ_I_PROXY,
    KZ_I_ERROR, KZ_I_EVAL_ERROR, KZ_I_RANGE_ERROR, KZ_I_REFERENCE_ERROR, KZ_I_SYNTAX_ERROR, KZ_I_TYPE_ERROR,
    KZ_I_URI_ERROR, KZ_I_AGGREGATE_ERROR,
    KZ_I_EVAL, KZ_I_THROW_TYPE_ERROR, KZ_I_ARRAY_PROTO_VALUES, KZ_I_PROMISE_RESOLVE_FN, KZ_I_PROMISE_THEN,
    KZ_I_OBJECT_PROTO_TOSTRING, KZ_I_UINT8C, KZ_I_INT8, KZ_I_UINT8, KZ_I_INT16, KZ_I_UINT16, KZ_I_INT32, KZ_I_UINT32,
    KZ_I_FLOAT32, KZ_I_FLOAT64, KZ_I_BIGINT64, KZ_I_BIGUINT64,
    KZ_I_ITERATOR,
    KZ_I_COUNT
} KzIntrinsic;

enum {
#define DEF(id, str) KZ_ATOM_##id,
#include "atoms.h"
#undef DEF
    KZ_ATOM_COUNT
};

enum { KZ_SYM_asyncIterator, KZ_SYM_hasInstance, KZ_SYM_isConcatSpreadable, KZ_SYM_iterator, KZ_SYM_match,
       KZ_SYM_matchAll, KZ_SYM_replace, KZ_SYM_search, KZ_SYM_species, KZ_SYM_split, KZ_SYM_toPrimitive,
       KZ_SYM_toStringTag, KZ_SYM_unscopables, KZ_SYM_COUNT };

typedef struct KzJob {
    struct KzJob *next;
    JSContext *ctx;
    KzNative *fn;               /* fn(ctx, undefined, argc, args, 0) */
    uint32_t argc;
    JSValue args[];
} KzJob;

/* allocator size classes (gc.c) */
#define KZ_NUM_CLASSES 20
#define KZ_MAX_SMALL 1024
typedef struct KzPage KzPage;
typedef struct KzLarge KzLarge;

typedef struct KzPinEntry { JSValue v; uint32_t count; } KzPinEntry;

struct JSRuntime {
    /* the allocator */
    void *(*sys_malloc)(void *, size_t);
    void *(*sys_realloc)(void *, void *, size_t);
    void (*sys_free)(void *, void *);
    void *sys_opaque;
    size_t mem_used, mem_limit, mem_peak;
    KzPage *pages[KZ_NUM_CLASSES];
    void *free_cells[KZ_NUM_CLASSES];
    KzLarge *large;             /* large garbage-collected cells (raw ones are not listed) */
    bool oom;                   /* an allocation failed: the out-of-memory error is being thrown */
    /* the collector */
    size_t gc_alloc_bytes;      /* bytes of cells allocated since the last collection */
    size_t gc_threshold;
    size_t gc_count;
    size_t live_objects, live_strings;
    bool gc_running, gc_stress, gc_disabled;
    uint32_t gc_stress_every, gc_stress_count;   /* stress: collect at every n-th allocation */
    JSValue *mark_stack;
    size_t mark_sp, mark_cap;
    bool mark_overflow;
    KzRootFrame *roots;
    KzPinEntry *pins;           /* open-addressed set of pinned values */
    uint32_t pin_count, pin_cap;
    void *weak_list;            /* WeakMaps, WeakSets, WeakRefs, FinalizationRegistries found while marking */
    KzVec kept_alive;           /* WeakRef targets kept until the end of the current job */
    /* atoms */
    JSString **atom_table;
    uint32_t atom_count, atom_cap, atom_used;
    JSValue atom[KZ_ATOM_COUNT];
    JSValue char_str[256];      /* one-unit strings for codes 0..255 */
    JSValue sym[KZ_SYM_COUNT];  /* well-known symbols */
    JSValue symbol_registry;    /* Symbol.for: a Map object (strong) */
    /* shapes: per-prototype root shapes (weak) */
    JSShape **root_shapes;
    uint32_t root_count, root_cap;
    JSShape *null_root;         /* the root shape with a null prototype */
    /* the VM */
    uint8_t *vstack, *vstack_top, *vstack_end;
    JSFrame *frame;             /* the innermost frame */
    int depth, max_depth;
    uintptr_t c_stack_limit;
    size_t c_stack_size;
    JSValue exception;          /* the pending exception (meaningful after JS_EXCEPTION) */
    bool exc_uncatchable;       /* an interrupt or a fatal error: try/catch does not stop it */
    bool in_oom_error;
    JSValue oom_error;          /* the preallocated "out of memory" RangeError */
    JSValue new_target;         /* for the native being called with new */
    JSObject *cur_native;       /* the native function being called */
    int suspend_kind;           /* kz_run returned JS_EMPTY: KZ_SUSPEND_* */
    JSValue suspend_value;      /* the yielded/awaited value */
    int (*interrupt)(JSRuntime *, void *);
    void *interrupt_opaque;
    int interrupt_count;
    /* jobs */
    KzJob *job_head, *job_tail;
    /* contexts and host classes */
    JSContext *contexts;
    const JSClassDef **host_classes;
    int nhost_classes;
    JSHostHooks hooks;
    uint64_t rand_state[2];
    bool in_finalizer;
    struct KzJoinNode *join_stack;   /* arrays being joined (cycle detection for join/toString) */
    KzVec unhandled;                /* rejected promises without handlers (reported when the job queue empties) */
    JSObject **finreg_pending;      /* FinalizationRegistry objects with dead targets (roots until cleaned up) */
    uint32_t finreg_npending, finreg_cap;
    KzRegExpCode *re_cache[32];     /* compiled regular expressions by (pattern, flags): literals in loops compile once */
};

typedef struct KzJoinNode { struct KzJoinNode *prev; JSValue obj; } KzJoinNode;

typedef struct KzGlobalLex KzGlobalLex;

struct JSContext {
    JSRuntime *rt;
    JSContext *next;
    JSValue global;             /* the global object */
    JSValue global_lex;         /* let/const/class of scripts: an object (null proto) mapping names to var cells */
    uint32_t global_epoch;      /* bumped when a global lexical binding is added (invalidates global caches) */
    JSValue intrinsic[KZ_I_COUNT];
    JSShape *shape_object;      /* {} */
    JSShape *shape_array;       /* [] */
    JSShape *shape_func_proto;  /* function with length, name, prototype */
    JSShape *shape_func;        /* function with length, name */
    JSShape *shape_error;
    JSShape *shape_iter_result; /* { value, done } */
    JSShape *shape_arguments;   /* length, callee, @@iterator */
    JSValue template_cache;     /* unused */
    KzVec api_scope;            /* values kept alive for the embedder */
    KzVec modules;              /* the module map: loaded modules (JV_MKPTR values), found by their canonical name */
    uint32_t mod_async_seq;     /* counts the modules that became async (the order they run in later) */
    void *opaque;
};

/* ================================================================== the allocator and the collector (gc.c) */

void *kz_malloc(JSContext *ctx, size_t n);              /* a raw buffer; NULL with an out-of-memory exception */
void *kz_mallocz(JSContext *ctx, size_t n);
void *kz_realloc(JSContext *ctx, void *p, size_t n);
void kz_free(JSRuntime *rt, void *p);
void *kz_rt_malloc(JSRuntime *rt, size_t n);            /* raw, without throwing (NULL on failure) */
void *kz_rt_realloc(JSRuntime *rt, void *p, size_t n);
size_t kz_alloc_size(const void *p);                    /* usable size of a raw buffer */
void *kz_gc_new(JSContext *ctx, KzGCType type, size_t size);   /* a cell, zeroed; NULL + exception on failure */
void kz_gc_maybe(JSContext *ctx);                        /* collect if the allocation budget says so */
void kz_gc_collect(JSRuntime *rt);
void kz_mark_value(JSRuntime *rt, JSValue v);
void kz_mark_cell(JSRuntime *rt, void *cell);
JSValue kz_throw_oom(JSContext *ctx);
bool kz_rt_init_alloc(JSRuntime *rt);
void kz_rt_free_all(JSRuntime *rt);
void kz_pin(JSRuntime *rt, JSValue v);
void kz_unpin(JSRuntime *rt, JSValue v);
KZ_INLINE bool kz_cell_marked(const void *cell) { return ((const JSGCHeader *)cell)->mark != 0; }

/* ================================================================== strings and atoms (string.c) */

JSString *kz_str_alloc(JSContext *ctx, uint32_t len, bool wide);    /* uninitialised data */
JSValue kz_str_from_latin1(JSContext *ctx, const uint8_t *s, uint32_t len);
JSValue kz_str_from_utf16(JSContext *ctx, const uint16_t *s, uint32_t len);
JSValue kz_str_from_utf8(JSContext *ctx, const char *s, size_t len);   /* WTF-8 accepted; bad bytes -> U+FFFD */
JSValue kz_str_from_cstr(JSContext *ctx, const char *s);
JSValue kz_str_from_char(JSContext *ctx, uint32_t c);                   /* one code point */
JSValue kz_str_concat(JSContext *ctx, JSValue a, JSValue b);
JSValue kz_str_sub(JSContext *ctx, JSValue s, uint32_t start, uint32_t end);
JSString *kz_str_flat(JSContext *ctx, JSString *s);     /* flattens a rope (NULL on OOM) */
JSValue kz_str_flatten(JSContext *ctx, JSValue s);      /* JS_EXCEPTION on OOM */
int kz_str_cmp(JSString *a, JSString *b);               /* flat strings: code-unit order */
bool kz_str_eq(JSString *a, JSString *b);               /* flat strings */
bool kz_str_eq_ascii(JSString *a, const char *s);
uint32_t kz_str_hash(JSString *s);                      /* flat */
int32_t kz_str_index_of(JSString *s, JSString *pat, uint32_t from);
int32_t kz_str_last_index_of(JSString *s, JSString *pat, int32_t from);
char *kz_str_to_utf8(JSContext *ctx, JSValue s, size_t *len);         /* kz_malloc'd */
size_t kz_str_to_utf8_buf(JSString *s, char *buf, size_t cap);       /* flat; truncates; NUL terminates */
uint32_t kz_utf8_decode(const uint8_t *s, size_t len, size_t *pos);   /* one code point (WTF-8); 0xFFFD on errors */
int kz_utf8_encode(uint32_t c, char *out);
JSValue kz_atom(JSContext *ctx, const char *s);                       /* intern a C string */
JSValue kz_atom_from_str(JSContext *ctx, JSValue s);                  /* intern a string value */
JSValue kz_atom_len(JSContext *ctx, const char *s, size_t len);
JSValue kz_atom_find(JSContext *ctx, JSValue s);                      /* the atom, or JS_NOTFOUND (never allocates) */
JSValue kz_key_from_value(JSContext *ctx, JSValue v);                 /* ToPropertyKey; JS_EXCEPTION */
JSValue kz_key_to_value(JSContext *ctx, JSValue key);                 /* index keys become strings */
bool kz_str_is_index(JSString *s, uint32_t *idx);                     /* canonical array index (flat) */
void kz_atoms_sweep(JSRuntime *rt);
bool kz_atoms_init(JSRuntime *rt);
JSValue kz_int_to_string(JSContext *ctx, int64_t v);
JSValue kz_key_debug_name(JSContext *ctx, JSValue key);              /* "x", "[Symbol.iterator]", "3" */

typedef struct KzStrBuf {
    JSContext *ctx;
    uint8_t *buf;               /* bytes or uint16_t units */
    uint32_t len, cap;
    bool wide;
    bool failed;
} KzStrBuf;
void kz_sb_init(KzStrBuf *sb, JSContext *ctx);
int kz_sb_putc(KzStrBuf *sb, uint32_t unit);            /* one code unit (<= 0xFFFF) */
int kz_sb_put_cp(KzStrBuf *sb, uint32_t cp);            /* a code point (surrogate pair when > 0xFFFF) */
int kz_sb_puts(KzStrBuf *sb, const char *ascii);
int kz_sb_put_latin1(KzStrBuf *sb, const uint8_t *s, uint32_t n);
int kz_sb_put_str(KzStrBuf *sb, JSString *s);           /* flat */
int kz_sb_put_value(KzStrBuf *sb, JSValue str);         /* a string value (flattens) */
int kz_sb_put_substr(KzStrBuf *sb, JSString *s, uint32_t from, uint32_t to);
JSValue kz_sb_finish(KzStrBuf *sb);                     /* frees the buffer; JS_EXCEPTION on failure */
void kz_sb_free(KzStrBuf *sb);

/* ================================================================== numbers (number.c, jsmath.c) */

int kz_dtoa(double d, char *buf);                       /* Number::toString(d) in radix 10; returns the length */
JSValue kz_num_to_str(JSContext *ctx, double d);
JSValue kz_num_to_str_radix(JSContext *ctx, double d, int radix);
JSValue kz_num_to_fixed(JSContext *ctx, double d, int digits);
JSValue kz_num_to_exponential(JSContext *ctx, double d, int digits);   /* digits < 0: as many as needed */
JSValue kz_num_to_precision(JSContext *ctx, double d, int prec);
/* StringToNumber over code units: whitespace trimmed, "Infinity", hex/octal/binary prefixes, "" -> 0, else NaN */
double kz_string_to_number(JSString *s);
/* parseFloat's prefix grammar from position i; *end gets the index after the number (== i: none) */
double kz_parse_float(JSString *s, uint32_t i, uint32_t *end);
double kz_strtod(const char *s, size_t len);            /* a correctly rounded decimal literal (digits . digits e) */
double kz_parse_radix_int(const char *digits, size_t len, int radix);   /* exact for any radix */
double kz_math_pow(double x, double y);
double kz_math_exp(double x);
double kz_math_log(double x);
double kz_math_fmod(double x, double y);
double kz_math_sin(double x);
double kz_math_cos(double x);
double kz_math_tan(double x);
double kz_math_atan(double x);
double kz_math_atan2(double y, double x);
double kz_math_asin(double x);
double kz_math_acos(double x);
double kz_math_expm1(double x);
double kz_math_log1p(double x);
double kz_math_log2(double x);
double kz_math_log10(double x);
double kz_math_sinh(double x);
double kz_math_cosh(double x);
double kz_math_tanh(double x);
double kz_math_asinh(double x);
double kz_math_acosh(double x);
double kz_math_atanh(double x);
double kz_math_cbrt(double x);
double kz_math_hypot2(double x, double y);
double kz_math_floor(double x);
double kz_math_ceil(double x);
double kz_math_trunc(double x);
double kz_math_round(double x);
double kz_ldexp(double x, int e);
double kz_frexp(double x, int *e);
int32_t kz_double_to_int32(double d);                   /* ToInt32 on a double */

/* ================================================================== objects (object.c) */

JSObject *kz_obj_alloc(JSContext *ctx, KzClassId cls, JSShape *shape, size_t payload);
JSValue kz_new_object(JSContext *ctx);                  /* {} */
JSValue kz_new_object_proto(JSContext *ctx, JSValue proto);
JSValue kz_new_object_class(JSContext *ctx, KzClassId cls, JSValue proto);
JSValue kz_new_array(JSContext *ctx, uint32_t len);
JSValue kz_new_array_from(JSContext *ctx, uint32_t n, const JSValue *vals);
JSValue kz_new_plain_with_shape(JSContext *ctx, JSShape *shape);
JSShape *kz_root_shape(JSContext *ctx, JSValue proto);
JSShape *kz_shape_add(JSContext *ctx, JSShape *s, JSValue key, uint32_t attrs);   /* the child shape */
int kz_shape_find(const JSShape *s, JSValue key);       /* prop index or -1 */
JSShape *kz_shape_build(JSContext *ctx, JSValue proto, const JSValue *keys, int n, uint32_t attrs);
int kz_obj_add_prop(JSContext *ctx, JSObject *o, JSValue key, uint32_t attrs, JSValue value);  /* key not present */
int kz_obj_set_proto_raw(JSContext *ctx, JSObject *o, JSValue proto);
void kz_obj_to_dict(JSContext *ctx, JSObject *o);
int kz_elems_reserve(JSContext *ctx, JSObject *o, uint32_t cap);
int kz_array_set_length(JSContext *ctx, JSObject *a, uint32_t len);    /* 1, 0 refused */
int kz_array_push(JSContext *ctx, JSObject *a, JSValue v);             /* fast append for internal arrays */
bool kz_array_is_fast(JSObject *a);                    /* dense, ordinary, extensible, standard attributes */
JSValue kz_proto_of(JSObject *o);                       /* the shape's prototype (ordinary objects) */

/* the internal methods ([[GetOwnProperty]] and so on), dispatching to exotic hooks */
int kz_get_own_property(JSContext *ctx, JSObject *o, JSValue key, KzPropDesc *d);   /* 1 found, 0 absent, -1 */
int kz_define_own_property(JSContext *ctx, JSObject *o, JSValue key, const KzPropDesc *d, bool throw_err);
int kz_has_property(JSContext *ctx, JSObject *o, JSValue key);
JSValue kz_get(JSContext *ctx, JSValue obj, JSValue key, JSValue receiver);       /* [[Get]] on any value */
JSValue kz_get_prop(JSContext *ctx, JSValue obj, JSValue key);                     /* receiver = obj */
JSValue kz_get_atom(JSContext *ctx, JSValue obj, int atom);
JSValue kz_get_index(JSContext *ctx, JSValue obj, uint32_t idx);
int kz_set(JSContext *ctx, JSValue obj, JSValue key, JSValue val, JSValue receiver, bool throw_err);   /* 1, 0, -1 */
int kz_set_prop(JSContext *ctx, JSValue obj, JSValue key, JSValue val, bool throw_err);
int kz_set_index(JSContext *ctx, JSValue obj, uint32_t idx, JSValue val, bool throw_err);
int kz_delete_property(JSContext *ctx, JSObject *o, JSValue key, bool throw_err);   /* 1, 0, -1 */
int kz_own_keys(JSContext *ctx, JSObject *o, KzVec *keys, int filter);             /* filter: KZ_KEYS_* */
#define KZ_KEYS_STRINGS  1
#define KZ_KEYS_SYMBOLS  2
#define KZ_KEYS_ENUM     4      /* only enumerable (checks descriptors) */
#define KZ_KEYS_PRIVATE  8      /* include private names (never for scripts) */
int kz_get_proto(JSContext *ctx, JSObject *o, JSValue *out);
int kz_set_proto(JSContext *ctx, JSObject *o, JSValue proto, bool throw_err);
int kz_is_extensible(JSContext *ctx, JSObject *o);
int kz_prevent_extensions(JSContext *ctx, JSObject *o, bool throw_err);
int kz_define_value(JSContext *ctx, JSValue obj, JSValue key, JSValue val, uint32_t attrs);  /* DefinePropertyOrThrow */
int kz_define_value_atom(JSContext *ctx, JSValue obj, int atom, JSValue val, uint32_t attrs);
int kz_define_accessor(JSContext *ctx, JSValue obj, JSValue key, JSValue getter, JSValue setter, uint32_t attrs);
int kz_create_data_property(JSContext *ctx, JSValue obj, JSValue key, JSValue val);   /* CreateDataPropertyOrThrow */
int kz_set_integrity(JSContext *ctx, JSObject *o, bool frozen);     /* SetIntegrityLevel */
int kz_test_integrity(JSContext *ctx, JSObject *o, bool frozen);
JSValue kz_from_property_descriptor(JSContext *ctx, const KzPropDesc *d);
int kz_to_property_descriptor(JSContext *ctx, JSValue obj, KzPropDesc *d);
void kz_complete_descriptor(KzPropDesc *d);
extern const KzExotic *kz_exotic[KZ_CLASS_COUNT];
JSValue kz_ordinary_create_from_ctor(JSContext *ctx, JSValue new_target, KzIntrinsic default_proto, KzClassId cls);
int kz_get_proto_from_ctor(JSContext *ctx, JSValue new_target, KzIntrinsic default_proto, JSValue *out);
/* slot access helpers */
KZ_INLINE JSValue kz_slot_value(JSObject *o, uint32_t i) { return o->slots[i]; }

/* ================================================================== conversions and operators (conv.c) */

JSValue kz_to_primitive(JSContext *ctx, JSValue v, int hint);     /* hint: 0 default, 1 number, 2 string */
#define KZ_HINT_DEFAULT 0
#define KZ_HINT_NUMBER  1
#define KZ_HINT_STRING  2
bool kz_to_bool(JSValue v);
int kz_to_number(JSContext *ctx, JSValue v, double *out);          /* 0, -1 */
JSValue kz_to_numeric(JSContext *ctx, JSValue v);                  /* number or bigint value */
JSValue kz_to_number_value(JSContext *ctx, JSValue v);
int kz_to_int32(JSContext *ctx, JSValue v, int32_t *out);
int kz_to_uint32(JSContext *ctx, JSValue v, uint32_t *out);
int kz_to_integer_or_inf(JSContext *ctx, JSValue v, double *out);   /* ToIntegerOrInfinity */
int kz_to_length(JSContext *ctx, JSValue v, uint64_t *out);
int kz_to_index(JSContext *ctx, JSValue v, uint64_t *out);         /* ToIndex (RangeError) */
JSValue kz_to_string(JSContext *ctx, JSValue v);                   /* a string value or JS_EXCEPTION */
JSValue kz_to_string_noexc(JSContext *ctx, JSValue v);             /* for messages: never calls JavaScript */
JSValue kz_to_object(JSContext *ctx, JSValue v);
JSValue kz_to_property_key(JSContext *ctx, JSValue v);             /* a key (see above) */
int kz_to_relative_index(JSContext *ctx, JSValue v, uint64_t len, uint64_t def, uint64_t *out);
/* equality: 1 equal, 0 not, -1 (out of memory while flattening a rope) */
int kz_same_value(JSContext *ctx, JSValue a, JSValue b);
int kz_same_value_zero(JSContext *ctx, JSValue a, JSValue b);
int kz_strict_eq(JSContext *ctx, JSValue a, JSValue b);
enum { KZ_BIN_SUB, KZ_BIN_MUL, KZ_BIN_DIV, KZ_BIN_MOD, KZ_BIN_POW, KZ_BIN_AND, KZ_BIN_OR, KZ_BIN_XOR, KZ_BIN_SHL,
       KZ_BIN_SAR, KZ_BIN_SHR, KZ_BIN_ADD };
int kz_loose_eq(JSContext *ctx, JSValue a, JSValue b);             /* 1, 0, -1 */
int kz_less_than(JSContext *ctx, JSValue a, JSValue b, bool left_first);   /* 1 true, 0 false/undefined, -1 */
JSValue kz_typeof(JSContext *ctx, JSValue v);                      /* an atom */
int kz_instance_of(JSContext *ctx, JSValue v, JSValue target);
int kz_ordinary_has_instance(JSContext *ctx, JSValue c, JSValue o);
int kz_is_array(JSContext *ctx, JSValue v);                        /* IsArray (sees through proxies), -1 */
int kz_length_of(JSContext *ctx, JSValue obj, uint64_t *out);      /* LengthOfArrayLike */
JSValue kz_get_method(JSContext *ctx, JSValue v, JSValue key);     /* undefined when absent; TypeError if not callable */
JSValue kz_species_constructor(JSContext *ctx, JSValue o, JSValue def);
JSValue kz_add(JSContext *ctx, JSValue a, JSValue b);              /* the + operator */
JSValue kz_binary_arith(JSContext *ctx, int op, JSValue a, JSValue b);   /* - * / % ** & | ^ << >> >>> on any values */
int kz_create_list_from_array_like(JSContext *ctx, JSValue obj, KzVec *out);
JSValue kz_invoke(JSContext *ctx, JSValue obj, JSValue key, int argc, JSValue *argv);
JSValue kz_symbol_descriptive_string(JSContext *ctx, JSValue sym);

/* iterators */
typedef struct KzIter { JSValue iter, next; bool done; } KzIter;    /* callers root the first two fields */
#define KZ_ROOT_ITER(ctx, it)                                                                       \
    KzIter it = { JS_UNDEFINED, JS_UNDEFINED, false };                                              \
    KzRootFrame it##_rf __attribute__((cleanup(kz_root_pop))) = { (ctx)->rt->roots, (ctx)->rt, &it.iter, 2, NULL }; \
    (ctx)->rt->roots = &it##_rf
int kz_get_iterator(JSContext *ctx, JSValue obj, KzIter *it, bool async);
int kz_get_iterator_from_method(JSContext *ctx, JSValue obj, JSValue method, KzIter *it);
int kz_iter_step(JSContext *ctx, KzIter *it, JSValue *value);      /* 1 got a value, 0 done, -1 */
JSValue kz_iter_close(JSContext *ctx, KzIter *it, JSValue completion);   /* returns completion or JS_EXCEPTION */
int kz_iter_close_on_throw(JSContext *ctx, KzIter *it);           /* keeps the pending exception; always -1 */
JSValue kz_iter_close_async_begin(JSContext *ctx, JSValue iter);       /* AsyncIteratorClose up to the await: return()'s result, JS_EMPTY if none */
JSValue kz_iter_close_async_on_throw(JSContext *ctx, JSValue iter);  /* the same with a throw completion: errors are dropped (JS_EMPTY) */
JSValue kz_create_iter_result(JSContext *ctx, JSValue value, bool done);
int kz_iterate_to_vec(JSContext *ctx, JSValue iterable, KzVec *out);

/* ================================================================== errors (error.c) */

typedef enum KzErr {
    KZ_ERR_ERROR, KZ_ERR_EVAL, KZ_ERR_RANGE, KZ_ERR_REFERENCE, KZ_ERR_SYNTAX, KZ_ERR_TYPE, KZ_ERR_URI,
    KZ_ERR_AGGREGATE, KZ_ERR_COUNT
} KzErr;
JSValue kz_throw(JSContext *ctx, JSValue v);
JSValue kz_throw_error(JSContext *ctx, KzErr kind, const char *fmt, ...);
JSValue kz_throw_type(JSContext *ctx, const char *fmt, ...);
JSValue kz_throw_range(JSContext *ctx, const char *fmt, ...);
JSValue kz_throw_reference(JSContext *ctx, const char *fmt, ...);
JSValue kz_throw_syntax(JSContext *ctx, const char *fmt, ...);
JSValue kz_throw_syntax_at(JSContext *ctx, JSValue filename, uint32_t line, uint32_t col, const char *fmt, ...);
JSValue kz_throw_not_function(JSContext *ctx, JSValue v);
JSValue kz_throw_not_object(JSContext *ctx, JSValue v);
JSValue kz_new_error_value(JSContext *ctx, KzErr kind, JSValue message);
JSValue kz_new_error_msg(JSContext *ctx, KzErr kind, const char *fmt, ...);
int kz_capture_stack(JSContext *ctx, JSValue err, int skip);      /* defines err.stack */
void kz_format_value_for_msg(JSContext *ctx, JSValue v, char *buf, size_t cap);   /* short, never calls JS */
JSValue kz_take_exception(JSContext *ctx);
KZ_INLINE bool kz_has_exception(JSContext *ctx) { return ctx->rt->exception != JS_UNINIT; }

/* ================================================================== functions and the VM (function.c, vm.c) */

JSValue kz_call(JSContext *ctx, JSValue f, JSValue this_val, int argc, JSValue *argv);
JSValue kz_construct(JSContext *ctx, JSValue f, int argc, JSValue *argv, JSValue new_target);
JSValue kz_run(JSContext *ctx, JSFrame *f);             /* runs frame f until it returns or suspends */
JSValue kz_new_native(JSContext *ctx, KzNative *fn, JSValue name, int length, int magic, int ndata, const JSValue *data);
JSValue kz_new_native_c(JSContext *ctx, KzNative *fn, const char *name, int length, int magic);
JSValue kz_native_data(JSContext *ctx, int i);          /* data slot i of the native being called */
void kz_native_set_data(JSContext *ctx, int i, JSValue v);
JSValue kz_new_closure(JSContext *ctx, JSFunctionBytecode *b, JSFrame *parent, JSValue home);
JSValue kz_eval_script(JSContext *ctx, const char *src, size_t len, const char *filename, int flags, JSFrame *caller, int eval_site);
JSValue kz_function_ctor_impl(JSContext *ctx, int argc, JSValue *argv, int kind, JSValue new_target);
int kz_set_function_name(JSContext *ctx, JSValue f, JSValue key, const char *prefix);
int kz_check_stack(JSContext *ctx);                     /* 0, or -1 with a RangeError (C stack nearly used up) */
JSValue kz_func_name(JSContext *ctx, JSObject *f);      /* for messages */
JSContext *kz_function_realm(JSContext *ctx, JSObject *f);
void kz_poll_interrupt(JSContext *ctx);
int kz_enqueue_job(JSContext *ctx, KzNative *fn, int argc, const JSValue *argv);
JSValue kz_generator_resume(JSContext *ctx, JSObject *gen, JSValue v, int mode, bool *done);
void kz_free_frame(JSRuntime *rt, JSFrame *f);
JSFrame *kz_new_gen_frame(JSContext *ctx, JSObject *func, JSValue this_val, int argc, JSValue *argv);
JSVarCell *kz_new_cell(JSContext *ctx, JSValue v);
int kz_get_this_realm_intrinsic(JSContext *ctx, JSValue f, KzIntrinsic id, JSValue *out);
int kz_line_of_pc(JSFunctionBytecode *b, uint32_t pc, uint32_t *line, uint32_t *col);

/* ================================================================== builtins setup (realm.c and the builtin files) */

typedef struct KzFuncDef {
    const char *name;           /* "[Symbol.iterator]" names a well-known symbol */
    KzNative *fn;               /* a getter for KZ_DEF_GETTER */
    KzNative *setter;
    uint8_t length;
    uint8_t nargs;
    int16_t magic;
    uint8_t kind;               /* KZ_DEF_* */
    uint8_t attrs;              /* 0: the default (W|C for methods, C for accessors) */
} KzFuncDef;
#define KZ_DEF_METHOD 0
#define KZ_DEF_GETTER 1
#define KZ_DEF_ACCESSOR 2
#define KZ_FN(name, fn, len) { name, fn, NULL, len, len, 0, KZ_DEF_METHOD, 0 }
#define KZ_FN_ARGS(name, fn, len, nargs) { name, fn, NULL, len, nargs, 0, KZ_DEF_METHOD, 0 }
#define KZ_FN_MAGIC(name, fn, len, magic) { name, fn, NULL, len, len, magic, KZ_DEF_METHOD, 0 }
#define KZ_FN_MAGIC_ARGS(name, fn, len, nargs, magic) { name, fn, NULL, len, nargs, magic, KZ_DEF_METHOD, 0 }
#define KZ_GETTER(name, fn) { name, fn, NULL, 0, 0, 0, KZ_DEF_GETTER, 0 }
#define KZ_GETTER_MAGIC(name, fn, magic) { name, fn, NULL, 0, 0, magic, KZ_DEF_GETTER, 0 }
#define KZ_ACCESSOR(name, get, set) { name, get, set, 0, 1, 0, KZ_DEF_ACCESSOR, 0 }
int kz_define_functions(JSContext *ctx, JSValue obj, const KzFuncDef *defs, int n);
JSValue kz_def_name_key(JSContext *ctx, const char *name);
JSValue kz_make_ctor(JSContext *ctx, KzNative *fn, const char *name, int length, int magic, KzIntrinsic proto_id, int ctor_id);
bool kz_realm_init(JSContext *ctx);

int kz_init_object(JSContext *ctx);
int kz_init_function(JSContext *ctx);
int kz_init_error(JSContext *ctx);
int kz_init_array(JSContext *ctx);
int kz_init_string(JSContext *ctx);
int kz_init_number(JSContext *ctx);
int kz_init_math(JSContext *ctx);
int kz_init_symbol(JSContext *ctx);
int kz_init_json(JSContext *ctx);
int kz_init_regexp(JSContext *ctx);
int kz_init_date(JSContext *ctx);
int kz_init_map(JSContext *ctx);
int kz_init_promise(JSContext *ctx);
int kz_init_proxy(JSContext *ctx);
int kz_init_typed(JSContext *ctx);
int kz_init_iterators(JSContext *ctx);
int kz_init_iterator_helpers(JSContext *ctx);
int kz_init_bigint(JSContext *ctx);
int kz_init_modules(JSContext *ctx);
int kz_init_intl(JSContext *ctx);
int kz_init_global(JSContext *ctx);

/* ================================================================== odds and ends shared between builtins */

JSValue kz_array_iterator_new(JSContext *ctx, JSValue obj, int kind);    /* kind: 0 keys, 1 values, 2 entries */
JSValue kz_native_return_this(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic);   /* get [Symbol.species] */
int kz_group_by(JSContext *ctx, JSValue items, JSValue cb, int mode, JSValue *out);   /* 0: object, 1: Map */
JSValue kz_array_species_create(JSContext *ctx, JSValue orig, uint64_t len);
JSValue kz_array_from_async(JSContext *ctx, JSValue this_val, int argc, JSValue *argv, int magic);   /* array_from_async.c */
JSValue kz_array_create(JSContext *ctx, uint64_t len, JSValue proto);   /* ArrayCreate (RangeError over 2^32-1) */
JSValue kz_object_to_string_tag(JSContext *ctx, JSValue v);             /* Object.prototype.toString */
JSValue kz_json_stringify(JSContext *ctx, JSValue v, JSValue replacer, JSValue space);
JSValue kz_json_parse(JSContext *ctx, JSValue text, JSValue reviver);
JSValue kz_regexp_create(JSContext *ctx, JSValue pattern, JSValue flags);
JSValue kz_regexp_exec(JSContext *ctx, JSValue r, JSValue s);
int kz_regexp_syntax_check(JSContext *ctx, JSString *pattern, JSString *flags, char *err, size_t errcap);
JSValue kz_promise_new(JSContext *ctx, JSValue *resolve, JSValue *reject);   /* resolve may be NULL: no functions */
JSValue kz_new_promise_intrinsic(JSContext *ctx);                       /* a pending %Promise% */
JSValue kz_promise_resolve(JSContext *ctx, JSValue ctor, JSValue v);    /* PromiseResolve(C, x) */
typedef JSValue KzReactionFn(JSContext *ctx, JSValue data, JSValue arg);
int kz_promise_then_internal(JSContext *ctx, JSValue promise, KzReactionFn *ok, KzReactionFn *err, JSValue data,
                             JSValue result);
void kz_promise_resolve_value(JSContext *ctx, JSValue promise, JSValue v);   /* resolve a promise we created */
void kz_promise_reject_value(JSContext *ctx, JSValue promise, JSValue v);
int kz_promise_state(JSValue p);
void kz_promise_mark_handled(JSContext *ctx, JSValue promise);
void kz_report_rejections(JSRuntime *rt);
double kz_date_now(JSContext *ctx);
double kz_local_tza(JSContext *ctx, double t, bool is_utc);
JSValue kz_bigint_from_double(JSContext *ctx, double d);
JSValue kz_bigint_from_int64(JSContext *ctx, int64_t v);
JSValue kz_bigint_from_string(JSContext *ctx, JSString *s);              /* StringToBigInt; JS_UNDEFINED: syntax error */
JSValue kz_bigint_to_string(JSContext *ctx, JSValue b, int radix);
double kz_bigint_to_double(JSValue b);
int kz_bigint_cmp(JSValue a, JSValue b);
int kz_bigint_cmp_double(JSValue a, double d);                           /* -1, 0, 1; 2 for NaN */
JSValue kz_bigint_binary(JSContext *ctx, int op, JSValue a, JSValue b);
JSValue kz_bigint_neg(JSContext *ctx, JSValue a);
JSValue kz_bigint_not(JSContext *ctx, JSValue a);
bool kz_bigint_is_zero(JSValue a);
bool kz_bigint_eq(JSValue a, JSValue b);
uint32_t kz_bigint_hash(JSValue a);
int kz_bigint_to_int64(JSContext *ctx, JSValue v, int64_t *out);         /* BigInt64 wrap */
JSValue kz_to_bigint(JSContext *ctx, JSValue v);
JSValue kz_bigint_literal(JSContext *ctx, const char *digits, size_t len, int radix);
JSValue kz_bigint_from_uint64(JSContext *ctx, uint64_t v);
bool kz_is_detached(JSObject *ta);                                       /* a typed array whose buffer was detached */
bool kz_typed_oob(JSObject *ta);                                         /* ... or that no longer fits its (resized) buffer */
uint32_t kz_typed_length(JSObject *ta);                                  /* 0 when out of bounds */
/* ArrayBuffer, typed arrays, DataView, Atomics (arraybuffer.c, typed.c); element kinds are KZ_CLASS_x - KZ_CLASS_UINT8C_ARRAY */
extern const uint8_t kz_ta_size[11];
JSValue kz_elem_load(int kind, const uint8_t *p);                        /* a Number from a non-BigInt element */
JSValue kz_elem_load_big(JSContext *ctx, int kind, const uint8_t *p);    /* a BigInt from a BigInt64/BigUint64 element */
void kz_elem_store_number(int kind, uint8_t *p, double d);               /* the NumericToRawBytes of a Number */
int kz_elem_store_big(JSContext *ctx, int kind, uint8_t *p, JSValue big);
JSValue kz_new_array_buffer(JSContext *ctx, size_t len);                 /* zero-filled */
int kz_detach_array_buffer(JSContext *ctx, JSObject *ab);                /* DetachArrayBuffer; -1 for a shared buffer */
bool kz_buffer_is_detached(JSObject *ab);
int kz_map_set(JSContext *ctx, JSObject *map, JSValue key, JSValue value);
JSValue kz_map_get(JSContext *ctx, JSObject *map, JSValue key);
JSValue kz_new_map(JSContext *ctx);
void kz_map_trace(JSRuntime *rt, JSObject *o);
void kz_map_free(JSRuntime *rt, JSObject *o);
void kz_map_iter_free(JSRuntime *rt, JSObject *it);
int kz_run_finalizers(JSContext *ctx);         /* FinalizationRegistry callbacks; the count run, or -1 */
int kz_is_regexp(JSContext *ctx, JSValue v);
JSValue kz_get_substitution(JSContext *ctx, JSValue matched, JSValue str, uint32_t position, const JSValue *captures,
                            uint32_t ncaptures, JSValue named, JSValue tmpl);
int kz_locale_compare(JSContext *ctx, JSString *a, JSString *b);   /* -1, 0, 1; -2 on OOM */
void kz_weak_process(JSRuntime *rt);           /* after marking: ephemerons, then clearing */
int kz_unicode_is_id_start(uint32_t c);
int kz_unicode_is_id_continue(uint32_t c);
int kz_unicode_is_space(uint32_t c);           /* WhiteSpace (not line terminators) */
uint32_t kz_unicode_to_upper_simple(uint32_t c);
uint32_t kz_unicode_to_lower_simple(uint32_t c);
int kz_unicode_to_upper_full(uint32_t c, uint32_t out[3]);   /* returns the count */
int kz_unicode_to_lower_full(uint32_t c, uint32_t out[3]);
uint32_t kz_unicode_fold_simple(uint32_t c);   /* simple case folding (regexp /iu) */
uint32_t kz_unicode_canonicalize_nonunicode(uint32_t c);   /* regexp /i without u: toUpperCase if single */
int kz_unicode_category(uint32_t c);           /* a KZ_GC_* general category index */
int kz_unicode_normalize(JSContext *ctx, const uint32_t *in, uint32_t n, int form, KzVec *unused, uint32_t **out, uint32_t *outn);
int kz_unicode_property(const char *name, size_t nlen, const char *value, size_t vlen,
                        int (*add)(void *, uint32_t, uint32_t), void *opaque);   /* 0, 1 unknown, -1 */
bool kz_unicode_has_binary(const char *name, uint32_t c);
int kz_unicode_case_closure(uint32_t lo, uint32_t hi, bool unicode, int (*add)(void *, uint32_t), void *opaque);
int kz_unicode_case_domain(bool unicode, int (*add)(void *, uint32_t, uint32_t), void *opaque);
/* modules (module.c): the module map is ctx->modules; a module's body is a generator-like frame (generator.c) */
JSValue kz_dynamic_import(JSContext *ctx, JSValue specifier, JSValue options, JSValue referrer);   /* a promise */
JSValue kz_import_meta(JSContext *ctx, JSFunctionBytecode *b);
JSValue kz_module_evaluate_source(JSContext *ctx, const char *src, size_t len, const char *filename);
void kz_module_trace(JSRuntime *rt, KzModule *m);
void kz_module_free(JSRuntime *rt, KzModule *m);
JSValue kz_compile_module(JSContext *ctx, const char *src, size_t len, JSValue name);              /* eval.c: bytecode or JS_EXCEPTION */
JSValue kz_module_gen_new(JSContext *ctx, JSObject *func);                                          /* generator.c */
JSValue *kz_module_gen_regs(JSValue gobj);
int kz_module_gen_start(JSContext *ctx, JSValue gobj);
int kz_module_gen_run(JSContext *ctx, JSValue gobj);
int kz_module_gen_run_async(JSContext *ctx, JSValue gobj, JSValue promise);
void kz_generator_trace(JSRuntime *rt, JSObject *o);
void kz_generator_free(JSRuntime *rt, JSObject *o);
void kz_promise_trace(JSRuntime *rt, JSObject *o);
void kz_promise_free(JSRuntime *rt, JSObject *o);
void kz_finreg_trace(JSRuntime *rt, JSObject *o);
void kz_finreg_free(JSRuntime *rt, JSObject *o);
void kz_regexp_free(JSRuntime *rt, KzRegExpCode *c);       /* drops a reference to compiled code */
void kz_regexp_cache_free(JSRuntime *rt);
void kz_trace_frame(JSRuntime *rt, JSFrame *f);
JSValue kz_intrinsic(JSContext *ctx, KzIntrinsic id);
KZ_INLINE JSValue kz_intr(JSContext *ctx, KzIntrinsic id) { return ctx->intrinsic[id]; }
KZ_INLINE JSValue kz_atomv(JSContext *ctx, int id) { return ctx->rt->atom[id]; }

#endif
