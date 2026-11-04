/*
 * regexp.h: the compiled form of a regular expression, shared by the compiler (regexp.c) and the matcher
 * (regexp_exec.c).  Specification: ECMA-262 22.2 (RegExp Objects) with Annex B.1.2 (the web-compatible grammar).
 *
 * A pattern is compiled to word code for a backtracking matcher.  The matcher keeps an explicit backtrack stack (no C
 * recursion, however nested the pattern or long the input), polls the runtime's interrupt handler, and bounds the
 * stack so that a hostile pattern ends in a RangeError rather than in the host running out of memory.
 */
#ifndef REGEXP_H
#define REGEXP_H

#include "jsint.h"

/* flag bits, in the order of RegExp.prototype.flags ("dgimsuvy") */
#define KZ_RE_HASINDICES   0x01
#define KZ_RE_GLOBAL       0x02
#define KZ_RE_IGNORECASE   0x04
#define KZ_RE_MULTILINE    0x08
#define KZ_RE_DOTALL       0x10
#define KZ_RE_UNICODE      0x20
#define KZ_RE_UNICODESETS  0x40
#define KZ_RE_STICKY       0x80
#define KZ_RE_EITHER_UNICODE (KZ_RE_UNICODE | KZ_RE_UNICODESETS)

/*
 * Instructions.  An instruction word is the operation in the low 8 bits plus modifier bits; operands follow as
 * further words.  Jump targets are absolute word offsets into the code.
 */
#define RX_BACK    0x100      /* consume right to left (the body of a lookbehind) */
#define RX_ICASE   0x200      /* compare canonicalised characters */
#define RX_UNI     0x400      /* characters are code points (u and v modes) */
#define RX_FLAG_A  0x800      /* ANY: dotAll; BOL/EOL: multiline; LOOP_ENTER, REPEAT_ATOM: lazy */

enum {
    RX_MATCH,           /* the whole pattern matched */
    RX_FAIL,            /* nothing matches here */
    RX_CHAR,            /* c: one character equal to c (c already canonical when RX_ICASE) */
    RX_ANY,             /* any character (not a line terminator unless RX_FLAG_A) */
    RX_SET,             /* i: one character in set i (canonicalised first when RX_ICASE) */
    RX_BOL,             /* ^ (RX_FLAG_A: multiline) */
    RX_EOL,             /* $ */
    RX_WORDB,           /* \b (RX_ICASE|RX_UNI: the extra word characters of /iu) */
    RX_NWORDB,          /* \B */
    RX_BACKREF,         /* n: the text group n captured (an unset group matches the empty string) */
    RX_BACKREF_MULTI,   /* k, g1..gk: duplicate named groups: the one that took part */
    RX_JMP,             /* target */
    RX_SPLIT_GREEDY,    /* target: continue here first, resume at target when that fails */
    RX_SPLIT_LAZY,      /* target: go to target first, resume here when that fails */
    RX_SAVE_START,      /* n: start of group n */
    RX_SAVE_END,        /* n: end of group n */
    RX_RESET_CAPS,      /* first, count: groups first..first+count-1 become unset (the start of an iteration) */
    RX_SET_REG,         /* r: register r = the current position */
    RX_EMPTY_CHECK,     /* r: fail when the position equals register r (an iteration matched nothing) */
    RX_LOOP_INIT,       /* r: counter register r = 0 (r + 1 holds the start of the current iteration) */
    RX_LOOP_ENTER,      /* r, min, max, exit: decide whether to run another iteration (RX_FLAG_A: lazy) */
    RX_LOOP_END,        /* r, min, head: an iteration finished */
    RX_LOOK_POS,        /* end: a positive lookahead or lookbehind whose body follows */
    RX_LOOK_NEG,        /* end: a negative one */
    RX_LOOK_END_POS,
    RX_LOOK_END_NEG,
    RX_REPEAT_ATOM,     /* min, max, next, then one RX_CHAR/RX_ANY/RX_SET instruction: a run of a one-character atom */
    RX_OP_COUNT
};
#define RX_OP(w) ((w) & 0xFF)
#define RX_INF   0x7FFFFFFF

/* a character set: ranges of code points in ascending order, with a bitmap for the characters below 256 */
typedef struct KzReSet {
    uint32_t nranges;
    uint32_t off;               /* index of the first range (a pair of uint32) in KzRegExpCode.ranges */
    uint32_t bitmap[8];
} KzReSet;

typedef struct KzReName { char *utf8; uint32_t len; } KzReName;

struct KzRegExpCode {
    uint32_t refcount;
    uint32_t flags;             /* KZ_RE_* */
    uint32_t ncaps;             /* capture groups, not counting the whole match */
    uint32_t nregs;
    uint32_t ncode, nsets, nranges_total;
    uint32_t *code;
    KzReSet *sets;
    uint32_t *ranges;
    KzReName *names;            /* NULL when no group is named; else ncaps + 1 entries (unnamed: utf8 NULL) */
    uint32_t min_len;           /* every match is at least this many code units long */
    uint8_t first[32];          /* bitmap: characters below 256 that can start a match */
    bool first_other;           /* ... a character >= 256 can start a match */
    bool first_any;             /* no filtering possible (the pattern can match empty, or starts with a backreference) */
    bool anchored;              /* a match can only start at index 0 */
    uint16_t *src;              /* the pattern's code units, for the compile cache */
    uint32_t srclen;
    uint32_t hash;
};

/* regexp.c */
int kz_regexp_parse_flags(JSString *flags, uint32_t *out);                 /* 0, or -1 for an invalid or repeated flag */
KzRegExpCode *kz_regexp_compile(JSContext *ctx, JSString *pattern, uint32_t flags, char *err, size_t errcap);
KzRegExpCode *kz_regexp_get(JSContext *ctx, JSString *pattern, uint32_t flags, char *err, size_t errcap);   /* cached */
void kz_regexp_retain(KzRegExpCode *c);

/* regexp_exec.c: search s (flat) from index start (only at start when sticky); caps gets 2 * (ncaps + 1) indices,
   -1 for a group that did not take part.  1 matched, 0 no match, -1 an exception is pending. */
int kz_regexp_match(JSContext *ctx, const KzRegExpCode *c, JSString *s, uint32_t start, bool sticky, int32_t *caps);

#endif
