/*
 * regexp_ast.h: the syntax tree a pattern is parsed into (regexp_parse.c) and the code generator reads
 * (regexp_gen.c).  Specification: ECMA-262 22.2.1 (Patterns) and Annex B.1.2.
 */
#ifndef REGEXP_AST_H
#define REGEXP_AST_H

#include "regexp.h"

typedef struct ReRange { uint32_t lo, hi; } ReRange;
typedef struct ReSet { ReRange *r; uint32_t n, cap; } ReSet;      /* ascending, merged once normalised */

enum {
    N_EMPTY,        /* matches the empty string */
    N_CHAR,         /* a = code point; b = its canonical form (RN_ICASE) */
    N_ANY,          /* . (RN_DOTALL) */
    N_SET,          /* set = the characters */
    N_SEQ,          /* kids in order */
    N_ALT,          /* kids are the alternatives */
    N_GROUP,        /* a = capture index; child */
    N_BACKREF,      /* a = group (b == 0), or b groups listed in the multi pool from c (duplicate names) */
    N_BOL, N_EOL,   /* ^ $ (RN_MULTI) */
    N_WORDB, N_NWORDB,
    N_LOOK,         /* child; RN_NEG, RN_BACK (lookbehind) */
    N_REPEAT        /* a = min, b = max (RX_INF), c = first capture index inside, d = how many; child; RN_LAZY */
};

#define RN_ICASE  0x01
#define RN_MULTI  0x02
#define RN_DOTALL 0x04
#define RN_NEG    0x08
#define RN_BACK   0x10
#define RN_LAZY   0x20

typedef struct ReNode {
    struct ReNode *all;             /* next node ever allocated (for freeing) */
    struct ReNode *child;
    struct ReNode **kids;
    uint32_t nkids, kcap;
    uint8_t type, flags;
    ReSet *set;
    uint32_t a, b, c, d;
    uint32_t min_len;               /* fewest code units any match of this node covers (saturating) */
} ReNode;

typedef struct ReStr { uint32_t *cp; uint32_t len; } ReStr;
typedef struct ReStrs { ReStr *v; uint32_t n, cap; } ReStrs;

typedef struct ReGroupName {
    char *utf8;
    uint32_t len, group;
    uint32_t *path;                 /* (disjunction id, alternative index) pairs from the root */
    uint32_t pathlen;
} ReGroupName;

typedef struct ReRef { char *utf8; uint32_t len; ReNode *node; } ReRef;

typedef struct ReParser {
    JSContext *ctx;
    const uint8_t *s8;
    const uint16_t *s16;
    bool wide;
    uint32_t pos, len;
    uint32_t flags;                 /* KZ_RE_* */
    bool unicode, vmode;
    bool has_named;                 /* the grammar parameter N: some group has a name */
    uint32_t total_groups;          /* counted by the pre-scan */
    uint32_t ngroups;               /* groups opened so far */
    uint8_t cur;                    /* the modifiers in force: RN_ICASE | RN_MULTI | RN_DOTALL */
    const char *err;                /* a SyntaxError message (static text) */
    bool failed;                    /* out of memory or stack: an exception is pending */
    ReNode *nodes;
    ReSet **sets; uint32_t nsets, setcap;
    ReStrs **strs; uint32_t nstrs, strscap;
    void **blocks; uint32_t nblocks, blockcap;
    ReGroupName *gnames; uint32_t ngnames, gnamecap;
    ReRef *refs; uint32_t nrefs, refcap;
    uint32_t *multi; uint32_t nmulti, multicap;
    uint32_t next_disj;
    uint32_t *path; uint32_t pathlen, pathcap;
} ReParser;

/* regexp_parse.c */
ReNode *re_parse(ReParser *P);                  /* the whole pattern; NULL with P->err or P->failed */
void re_parser_init(ReParser *P, JSContext *ctx, JSString *pattern, uint32_t flags);
void re_parser_free(ReParser *P);
/* sets, for the generator */
int re_set_push(ReParser *P, ReSet *s, uint32_t lo, uint32_t hi);
void re_set_norm(ReSet *s);

#endif
