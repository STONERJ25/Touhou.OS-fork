/*
 * regexp_exec.c: the matcher.  A backtracking virtual machine over the word code regexp_gen.c produces; it follows
 * ECMA-262 22.2.2 (Pattern Semantics) and keeps the specification's continuation-passing structure as an explicit
 * stack, so no pattern or input can overflow the C stack.
 *
 * The backtrack stack holds 8-byte entries, newest on top, of these kinds:
 *   CHOICE   resume at a pc with a position (a SPLIT, a loop decision)
 *   CAP/REG  the old value of a capture slot / loop register, put back when the stack unwinds past it
 *   LA_POS   the start of a positive lookaround body; LA_NEG the same for a negative one (carries the pc to continue
 *            at when the body fails, which is what makes the assertion succeed)
 *   GREEDY/LAZY (with a DATA entry below): the remaining options of a run of a one-character atom (REPEAT_ATOM)
 * Failure pops entries, undoing capture and register writes, until a choice is found.
 *
 * Long matches poll the runtime's interrupt handler, so a host can stop a catastrophic pattern; the stack is bounded
 * (a RangeError instead of exhausting memory).
 */
#include "regexp.h"

typedef struct BtEntry { uint32_t w0; int32_t w1; } BtEntry;

enum { BT_CHOICE, BT_CAP, BT_REG, BT_LA_POS, BT_LA_NEG, BT_GREEDY, BT_LAZY, BT_DATA };
#define BT_SHIFT 28
#define BT_MK(kind, arg) (((uint32_t)(kind) << BT_SHIFT) | (uint32_t)(arg))
#define BT_KIND(w) ((w) >> BT_SHIFT)
#define BT_ARG(w) ((w) & 0x0FFFFFFFu)

#define BT_LOCAL 512
#define BT_MAX_ENTRIES (1u << 23)       /* 64 MiB */
#define POLL_EVERY 16384

typedef struct Matcher {
    JSContext *ctx;
    const KzRegExpCode *c;
    const uint8_t *in8;
    const uint16_t *in16;
    bool wide;
    int32_t len;
    int32_t *caps;
    int32_t *regs;
    BtEntry *bt;
    size_t sp, cap;
    uint64_t steps, next_poll;
    BtEntry local[BT_LOCAL];
} Matcher;

static inline uint32_t in_at(const Matcher *m, int32_t i) { return m->wide ? m->in16[i] : m->in8[i]; }
static inline bool is_lt(uint32_t c) { return c == 10 || c == 13 || c == 0x2028 || c == 0x2029; }
static inline bool is_high(uint32_t c) { return (c & 0xFC00) == 0xD800; }
static inline bool is_low(uint32_t c) { return (c & 0xFC00) == 0xDC00; }

static inline uint32_t canon_c(uint32_t c, bool uni) {
    if (c < 128) return uni ? (c >= 'A' && c <= 'Z' ? c + 32 : c) : (c >= 'a' && c <= 'z' ? c - 32 : c);
    return uni ? kz_unicode_fold_simple(c) : kz_unicode_canonicalize_nonunicode(c);
}

static inline bool is_word(uint32_t c, bool ext) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           (ext && (c == 0x17F || c == 0x212A));
}

static inline bool set_has(const KzRegExpCode *c, const KzReSet *s, uint32_t ch) {
    if (ch < 256) return (s->bitmap[ch >> 5] >> (ch & 31)) & 1;
    const uint32_t *r = c->ranges + 2 * (size_t)s->off;
    uint32_t lo = 0, hi = s->nranges;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (ch < r[2 * mid]) hi = mid;
        else if (ch > r[2 * mid + 1]) lo = mid + 1;
        else return true;
    }
    return false;
}

/* one character forward / backward (a code point when uni) */
static inline bool rd_fwd(const Matcher *m, int32_t pos, uint32_t uni, uint32_t *c, int32_t *npos) {
    if (pos >= m->len) return false;
    uint32_t x = in_at(m, pos);
    pos++;
    if (uni && is_high(x) && pos < m->len) {
        uint32_t y = in_at(m, pos);
        if (is_low(y)) { x = 0x10000 + ((x - 0xD800) << 10) + (y - 0xDC00); pos++; }
    }
    *c = x;
    *npos = pos;
    return true;
}

static inline bool rd_back(const Matcher *m, int32_t pos, uint32_t uni, uint32_t *c, int32_t *npos) {
    if (pos <= 0) return false;
    uint32_t x = in_at(m, pos - 1);
    pos--;
    if (uni && is_low(x) && pos > 0) {
        uint32_t y = in_at(m, pos - 1);
        if (is_high(y)) { x = 0x10000 + ((y - 0xD800) << 10) + (x - 0xDC00); pos--; }
    }
    *c = x;
    *npos = pos;
    return true;
}

/* does the one-character atom (CHAR/ANY/SET with its modifier bits in w) match at pos? */
static inline bool atom_step(const Matcher *m, uint32_t w, uint32_t arg, int32_t pos, int32_t *npos) {
    uint32_t c;
    if (w & RX_BACK) { if (!rd_back(m, pos, w & RX_UNI, &c, npos)) return false; }
    else if (!rd_fwd(m, pos, w & RX_UNI, &c, npos)) return false;
    switch (RX_OP(w)) {
    case RX_CHAR:
        if (w & RX_ICASE) c = canon_c(c, (w & RX_UNI) != 0);
        return c == arg;
    case RX_ANY:
        return (w & RX_FLAG_A) || !is_lt(c);
    default:    /* RX_SET */
        if (w & RX_ICASE) c = canon_c(c, (w & RX_UNI) != 0);
        return set_has(m->c, &m->c->sets[arg], c);
    }
}

/* a backreference to [s, e): does the same text sit at pos (before it when backward)?  Sets *npos when it does. */
static bool backref_match(const Matcher *m, uint32_t w, int32_t s, int32_t e, int32_t pos, int32_t *npos) {
    int32_t len = e - s, g;
    if (w & RX_BACK) {
        g = pos - len;
        if (g < 0) return false;
        *npos = g;
    } else {
        if (m->len - pos < len) return false;
        g = pos;
        *npos = pos + len;
    }
    if (!(w & RX_ICASE)) {
        for (int32_t i = 0; i < len; i++)
            if (in_at(m, s + i) != in_at(m, g + i)) return false;
        return true;
    }
    bool uni = (w & RX_UNI) != 0;
    int32_t i = 0;
    while (i < len) {
        uint32_t a = in_at(m, s + i), b = in_at(m, g + i);
        int32_t ia = 1, ib = 1;
        if (uni) {
            if (is_high(a) && i + 1 < len && is_low(in_at(m, s + i + 1))) { a = 0x10000 + ((a - 0xD800) << 10) + (in_at(m, s + i + 1) - 0xDC00); ia = 2; }
            if (is_high(b) && i + 1 < len && is_low(in_at(m, g + i + 1))) { b = 0x10000 + ((b - 0xD800) << 10) + (in_at(m, g + i + 1) - 0xDC00); ib = 2; }
        }
        if (ia != ib || canon_c(a, uni) != canon_c(b, uni)) return false;
        i += ia;
    }
    return true;
}

/* ------------------------------------------------------------------ the backtrack stack */

static int bt_grow(Matcher *m) {
    size_t nc = m->cap * 2;
    if (nc > BT_MAX_ENTRIES) nc = BT_MAX_ENTRIES;
    if (m->sp >= BT_MAX_ENTRIES || nc <= m->cap) {
        kz_throw_range(m->ctx, "Maximum call stack size exceeded");
        return -1;
    }
    BtEntry *nb;
    if (m->bt == m->local) {
        nb = kz_malloc(m->ctx, nc * sizeof(BtEntry));
        if (!nb) return -1;
        memcpy(nb, m->bt, m->sp * sizeof(BtEntry));
    } else {
        nb = kz_realloc(m->ctx, m->bt, nc * sizeof(BtEntry));
        if (!nb) return -1;
    }
    m->bt = nb;
    m->cap = nc;
    return 0;
}

#define PUSH(w0v, w1v) do {                                                         \
        if (kz_unlikely(m->sp == m->cap) && bt_grow(m)) return -1;                  \
        m->bt[m->sp].w0 = (uint32_t)(w0v);                                          \
        m->bt[m->sp].w1 = (int32_t)(w1v);                                           \
        m->sp++;                                                                    \
    } while (0)

static int poll_interrupt(Matcher *m) {
    m->next_poll = m->steps + POLL_EVERY;
    kz_poll_interrupt(m->ctx);
    return kz_has_exception(m->ctx) ? -1 : 0;
}

/* ------------------------------------------------------------------ one attempt at one start position */

/* 1 matched (caps filled), 0 no match here, -1 exception */
static int run(Matcher *m, int32_t start) {
    const KzRegExpCode *c = m->c;
    const uint32_t *code = c->code;
    const uint32_t *pc = code;
    int32_t pos = start;
    int32_t *caps = m->caps, *regs = m->regs;
    m->sp = 0;
    caps[0] = start;
    for (;;) {
        uint32_t w = *pc++;
        switch (RX_OP(w)) {
        case RX_MATCH:
            caps[1] = pos;
            return 1;
        case RX_FAIL:
            goto fail;
        case RX_CHAR: case RX_SET: {
            uint32_t arg = *pc++;
            int32_t np;
            if (!atom_step(m, w, arg, pos, &np)) goto fail;
            pos = np;
            break;
        }
        case RX_ANY: {
            int32_t np;
            if (!atom_step(m, w, 0, pos, &np)) goto fail;
            pos = np;
            break;
        }
        case RX_BOL:
            if (pos == 0 || ((w & RX_FLAG_A) && is_lt(in_at(m, pos - 1)))) break;
            goto fail;
        case RX_EOL:
            if (pos == m->len || ((w & RX_FLAG_A) && is_lt(in_at(m, pos)))) break;
            goto fail;
        case RX_WORDB: case RX_NWORDB: {
            bool ext = (w & (RX_ICASE | RX_UNI)) == (RX_ICASE | RX_UNI);
            bool a = pos > 0 && is_word(in_at(m, pos - 1), ext);
            bool b = pos < m->len && is_word(in_at(m, pos), ext);
            if ((a != b) == (RX_OP(w) == RX_WORDB)) break;
            goto fail;
        }
        case RX_BACKREF: {
            uint32_t g = *pc++;
            int32_t s = caps[2 * g], e = caps[2 * g + 1], np;
            if (s < 0 || e < 0) break;                      /* an unset group matches the empty string */
            if (!backref_match(m, w, s, e, pos, &np)) goto fail;
            pos = np;
            break;
        }
        case RX_BACKREF_MULTI: {
            uint32_t k = *pc++;
            const uint32_t *list = pc;
            pc += k;
            int32_t s = -1, e = -1, np;
            for (uint32_t i = 0; i < k; i++) {
                uint32_t g = list[i];
                if (caps[2 * g] >= 0 && caps[2 * g + 1] >= 0) { s = caps[2 * g]; e = caps[2 * g + 1]; break; }
            }
            if (s < 0) break;
            if (!backref_match(m, w, s, e, pos, &np)) goto fail;
            pos = np;
            break;
        }
        case RX_JMP:
            pc = code + *pc;
            break;
        case RX_SPLIT_GREEDY: {
            uint32_t target = *pc++;
            PUSH(BT_MK(BT_CHOICE, target), pos);
            break;
        }
        case RX_SPLIT_LAZY: {
            uint32_t target = *pc++;
            PUSH(BT_MK(BT_CHOICE, pc - code), pos);
            pc = code + target;
            break;
        }
        case RX_SAVE_START: case RX_SAVE_END: {
            uint32_t slot = 2 * *pc++ + (RX_OP(w) == RX_SAVE_END);
            if (caps[slot] != pos) {
                PUSH(BT_MK(BT_CAP, slot), caps[slot]);
                caps[slot] = pos;
            }
            break;
        }
        case RX_RESET_CAPS: {
            uint32_t first = pc[0], n = pc[1];
            pc += 2;
            for (uint32_t slot = 2 * first; slot < 2 * (first + n); slot++)
                if (caps[slot] != -1) {
                    PUSH(BT_MK(BT_CAP, slot), caps[slot]);
                    caps[slot] = -1;
                }
            break;
        }
        case RX_SET_REG: {
            uint32_t r = *pc++;
            if (regs[r] != pos) {
                PUSH(BT_MK(BT_REG, r), regs[r]);
                regs[r] = pos;
            }
            break;
        }
        case RX_EMPTY_CHECK: {
            uint32_t r = *pc++;
            if (regs[r] == pos) goto fail;
            break;
        }
        case RX_LOOP_INIT: {
            uint32_t r = *pc++;
            if (regs[r] != 0) {
                PUSH(BT_MK(BT_REG, r), regs[r]);
                regs[r] = 0;
            }
            break;
        }
        case RX_LOOP_ENTER: {
            uint32_t r = pc[0], min = pc[1], max = pc[2], exit = pc[3];
            pc += 4;
            uint32_t cnt = (uint32_t)regs[r];
            if (cnt < min) break;                           /* an iteration is required */
            if (cnt >= max) { pc = code + exit; break; }
            if (w & RX_FLAG_A) {                            /* lazy: leave the loop first, iterate on backtracking */
                PUSH(BT_MK(BT_CHOICE, pc - code), pos);
                pc = code + exit;
            } else PUSH(BT_MK(BT_CHOICE, exit), pos);
            break;
        }
        case RX_LOOP_END: {
            uint32_t r = pc[0], min = pc[1], head = pc[2];
            pc += 3;
            uint32_t cnt = (uint32_t)regs[r];
            if (cnt >= min && pos == regs[r + 1]) goto fail;    /* an optional iteration that matched nothing */
            PUSH(BT_MK(BT_REG, r), regs[r]);
            if (cnt < 0x7FFFFFFFu) regs[r] = (int32_t)cnt + 1;
            pc = code + head;
            break;
        }
        case RX_LOOK_POS:
            pc++;
            PUSH(BT_MK(BT_LA_POS, 0), pos);
            break;
        case RX_LOOK_NEG: {
            uint32_t end = *pc++;
            PUSH(BT_MK(BT_LA_NEG, end), pos);
            break;
        }
        case RX_LOOK_END_POS: {
            /* the body matched: forget its choice points but keep what it captured; the position is where we began */
            size_t i = m->sp;
            while (i > 0 && BT_KIND(m->bt[i - 1].w0) != BT_LA_POS) i--;
            size_t barrier = i - 1;
            pos = m->bt[barrier].w1;
            size_t wr = barrier;
            for (size_t k = i; k < m->sp; k++)
                if (BT_KIND(m->bt[k].w0) == BT_CAP) m->bt[wr++] = m->bt[k];
            m->sp = wr;
            break;
        }
        case RX_LOOK_END_NEG: {
            /* the body matched, so the negative assertion fails: undo what it did and fail past the barrier */
            for (;;) {
                BtEntry e = m->bt[--m->sp];
                uint32_t kind = BT_KIND(e.w0);
                if (kind == BT_LA_NEG) break;
                if (kind == BT_CAP) caps[BT_ARG(e.w0)] = e.w1;
                else if (kind == BT_REG) regs[BT_ARG(e.w0)] = e.w1;
            }
            goto fail;
        }
        case RX_REPEAT_ATOM: {
            uint32_t ipc = (uint32_t)(pc - 1 - code);
            uint32_t min = pc[0], max = pc[1], next = pc[2];
            uint32_t aw = pc[3], aarg = RX_OP(aw) == RX_ANY ? 0 : pc[4];
            uint32_t cnt = 0;
            int32_t p = pos, np;
            if (!(w & RX_FLAG_A)) {
                int32_t pmin = pos;
                if (!(aw & (RX_BACK | RX_UNI | RX_ICASE))) {            /* plain code units: tight loops */
                    int32_t lim = max < (uint32_t)(m->len - p) ? p + (int32_t)max : m->len;
                    switch (RX_OP(aw)) {
                    case RX_CHAR:
                        if (m->wide) while (p < lim && m->in16[p] == aarg) p++;
                        else while (p < lim && m->in8[p] == aarg) p++;
                        break;
                    case RX_ANY:
                        if (aw & RX_FLAG_A) p = lim;
                        else if (m->wide) while (p < lim && !is_lt(m->in16[p])) p++;
                        else while (p < lim && !is_lt(m->in8[p])) p++;
                        break;
                    default: {
                        const KzReSet *s = &c->sets[aarg];
                        if (m->wide) while (p < lim && set_has(c, s, m->in16[p])) p++;
                        else while (p < lim && set_has(c, s, m->in8[p])) p++;
                        break;
                    }
                    }
                    cnt = (uint32_t)(p - pos);
                    if (cnt >= min) pmin = pos + (int32_t)min;
                } else {
                    while (cnt < max && atom_step(m, aw, aarg, p, &np)) {
                        p = np;
                        cnt++;
                        if (cnt == min) pmin = p;
                    }
                }
                if (cnt < min) goto fail;
                m->steps += cnt;
                if (cnt > min) {
                    PUSH(BT_MK(BT_DATA, 0), pmin);
                    PUSH(BT_MK(BT_GREEDY, next), p);
                }
                pos = p;
                pc = code + next;
                if (kz_unlikely(m->steps >= m->next_poll) && poll_interrupt(m)) return -1;
                break;
            }
            while (cnt < min) {
                if (!atom_step(m, aw, aarg, p, &np)) goto fail;
                p = np;
                cnt++;
            }
            m->steps += cnt;
            if (max > min) {
                PUSH(BT_MK(BT_DATA, 0), cnt);
                PUSH(BT_MK(BT_LAZY, ipc), p);
            }
            pos = p;
            pc = code + next;
            break;
        }
        default:
            goto fail;
        }
        continue;

    fail:
        if (kz_unlikely(++m->steps >= m->next_poll) && poll_interrupt(m)) return -1;
        for (;;) {
            if (m->sp == 0) return 0;
            BtEntry *e = &m->bt[m->sp - 1];
            uint32_t kind = BT_KIND(e->w0);
            if (kind == BT_CHOICE) {
                pc = code + BT_ARG(e->w0);
                pos = e->w1;
                m->sp--;
                break;
            }
            if (kind == BT_CAP) { caps[BT_ARG(e->w0)] = e->w1; m->sp--; continue; }
            if (kind == BT_REG) { regs[BT_ARG(e->w0)] = e->w1; m->sp--; continue; }
            if (kind == BT_LA_POS || kind == BT_DATA) { m->sp--; continue; }
            if (kind == BT_LA_NEG) {                /* the body of a negative lookaround failed: the assertion holds */
                pc = code + BT_ARG(e->w0);
                pos = e->w1;
                m->sp--;
                break;
            }
            if (kind == BT_GREEDY) {
                int32_t pmin = m->bt[m->sp - 2].w1, p = e->w1, np = p - 1;
                if ((c->flags & KZ_RE_EITHER_UNICODE) && np > pmin && is_low(in_at(m, np)) && is_high(in_at(m, np - 1))) np--;
                pc = code + BT_ARG(e->w0);
                pos = np;
                if (np > pmin) e->w1 = np;
                else m->sp -= 2;
                break;
            }
            /* BT_LAZY: one more repetition of the atom */
            {
                const uint32_t *ins = code + BT_ARG(e->w0);
                uint32_t max = ins[2], next = ins[3], aw = ins[4], aarg = RX_OP(aw) == RX_ANY ? 0 : ins[5];
                int32_t p = e->w1, np;
                uint32_t cnt = (uint32_t)m->bt[m->sp - 2].w1;
                if (!atom_step(m, aw, aarg, p, &np)) { m->sp -= 2; continue; }
                cnt++;
                if (cnt >= max) m->sp -= 2;
                else { m->bt[m->sp - 2].w1 = (int32_t)cnt; e->w1 = np; }
                pc = code + next;
                pos = np;
                break;
            }
        }
    }
}

/* ------------------------------------------------------------------ searching */

int kz_regexp_match(JSContext *ctx, const KzRegExpCode *c, JSString *s, uint32_t start, bool sticky, int32_t *caps) {
    uint32_t len = kz_str_len(s);
    if (start > len) return 0;
    if (len - start < c->min_len) return 0;
    Matcher mem;
    Matcher *m = &mem;
    int32_t regs_local[16];
    int32_t *regs = c->nregs <= 16 ? regs_local : kz_malloc(ctx, (size_t)c->nregs * sizeof(int32_t));
    if (!regs) return -1;
    m->ctx = ctx;
    m->c = c;
    m->wide = kz_str_wide(s);
    m->in8 = m->wide ? NULL : kz_str8(s);
    m->in16 = m->wide ? kz_str16(s) : NULL;
    m->len = (int32_t)len;
    m->caps = caps;
    m->regs = regs;
    m->bt = m->local;
    m->sp = 0;
    m->cap = BT_LOCAL;
    m->steps = 0;
    m->next_poll = POLL_EVERY;
    memset(regs, 0, (size_t)c->nregs * sizeof(int32_t));
    bool uni = (c->flags & KZ_RE_EITHER_UNICODE) != 0;
    size_t ncap_slots = 2 * ((size_t)c->ncaps + 1);
    int32_t i = (int32_t)start;
    int rc = 0;
    /* a start inside a surrogate pair (u and v modes) begins at the pair */
    if (uni && i > 0 && i < m->len && is_low(in_at(m, i)) && is_high(in_at(m, i - 1))) i--;
    for (;;) {
        if (!sticky) {
            if (c->anchored && i > 0) break;
            if (!c->first_any) {
                while (i < m->len) {
                    uint32_t ch = in_at(m, i);
                    if (ch < 256 ? (c->first[ch >> 3] >> (ch & 7)) & 1 : c->first_other) break;
                    i++;
                }
                if (i >= m->len) break;             /* the pattern consumes at least one character */
            }
            if ((uint32_t)(m->len - i) < c->min_len) break;
        }
        for (size_t k = 0; k < ncap_slots; k++) caps[k] = -1;
        rc = run(m, i);
        if (rc != 0) break;
        if (sticky || i >= m->len) break;
        i++;
        if (uni && i < m->len && is_low(in_at(m, i)) && is_high(in_at(m, i - 1))) i++;
        if (kz_unlikely(++m->steps >= m->next_poll) && poll_interrupt(m)) { rc = -1; break; }
    }
    if (m->bt != m->local) kz_free(ctx->rt, m->bt);
    if (regs != regs_local) kz_free(ctx->rt, regs);
    return rc;
}
