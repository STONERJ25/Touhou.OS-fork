/*
 * pc98gfx: the PC-9801 display hardware as one device model (include/pc98gfx.h; behaviour, gaps and the "unverified"
 * list in docs/PC98_GFX.md).
 *
 * Written from: the uPD7220 GDC data sheet (command codes, parameter formats, the drawing-parameter table), the
 * publicly documented PC-98 memory and I/O maps (text and graphics VRAM, the attribute bits GRBVUrbs, the palette ports,
 * mode flip-flops 68h and 6Ah, the GRCG mode register 7Ch), published descriptions of how the EGC's shifter behaves on
 * real machines (it re-arms itself once a row's bit length has been written; a write that finds too few source dots
 * writes nothing), and our own analysis of a FONT.ROM image's layout. No emulator's code was used.
 *
 * Three orders of bits appear below, and each conversion is written out where it happens:
 *  - VRAM bytes as the CPU sees them: bit 7 is the leftmost of 8 dots. A "dot-ordered" 16-bit value puts the byte at
 *    the even address on top, so bit 15 is the leftmost of 16 dots (that is what the EGC works on).
 *  - The GDC's words: bit n is dot n of the word counted from the left (its dot address dAD), so a GDC word is the
 *    bit-reversed dot-ordered value. The GDC's MASK register and WDAT/RDAT data use this order.
 *  - The EGC shifter's "stream order": dots in the order a row is processed, left to right, or right to left when the
 *    descending bit is set.
 */
#include <pc98gfx.h>
#include <string.h>

/* ------------------------------------------------------------------ small helpers */

static uint16_t rev16(uint16_t v) {
    v = (uint16_t)(((v & 0x5555u) << 1) | ((v >> 1) & 0x5555u));
    v = (uint16_t)(((v & 0x3333u) << 2) | ((v >> 2) & 0x3333u));
    v = (uint16_t)(((v & 0x0F0Fu) << 4) | ((v >> 4) & 0x0F0Fu));
    return (uint16_t)((v << 8) | (v >> 8));
}

static uint16_t swap16(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}

static int32_t sx14(uint16_t v) {                   /* a 14-bit two's complement drawing parameter */
    v &= 0x3FFF;
    return (v & 0x2000) ? (int32_t)v - 0x4000 : (int32_t)v;
}

static uint32_t isqrt32(uint32_t v) {               /* floor(sqrt(v)) */
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        bit >>= 2;
    }
    return r;
}

/* The eight fixed colours of the text layer and of the digital palette, by GRB code. */
static const uint32_t GRB_RGB[8] = {
    0x000000, 0x0000FF, 0xFF0000, 0xFF00FF, 0x00FF00, 0x00FFFF, 0xFFFF00, 0xFFFFFF,
};

/* ------------------------------------------------------------------ set-up */

static void gdc_reset(Pc98GfxGdc *d) {
    memset(d, 0, sizeof *d);
    d->d = 8;                                       /* the FIGS defaults of the data sheet */
    d->d2 = 8;
    d->d1 = 0x3FFF;
    d->dm = 0x3FFF;
}

static void set_partition(Pc98GfxGdc *d, int n, uint32_t sad, unsigned len) {
    uint8_t *p = d->pram + 4 * n;                   /* SAD 18 bits, LEN 10 bits, WD and IM clear */
    p[0] = (uint8_t)sad;
    p[1] = (uint8_t)(sad >> 8);
    p[2] = (uint8_t)(((sad >> 16) & 3) | ((len & 15) << 4));
    p[3] = (uint8_t)((len >> 4) & 0x3F);
}

static int is_memsw(uint32_t a) {
    return a >= PC98GFX_MEMSW && a <= PC98GFX_MEMSW + 28 && ((a - PC98GFX_MEMSW) & 3) == 0;
}

static void egc_shifter_reset(Pc98GfxEgc *e) {
    memset(e->q, 0, sizeof e->q);
    e->qn = 0;
    e->in_started = 0;
    e->out_started = 0;
    e->out_done = 0;
    e->tail = 0;
}

void pc98gfx_reset(Pc98Gfx *g) {
    /* Text: spaces, white, shown (what the BIOS's screen clear leaves); the memory switches keep their values. */
    for (unsigned i = 0; i < 0x2000; i += 2) {
        g->tvram[i] = 0x20;
        g->tvram[i + 1] = 0;
        if (!is_memsw(PC98GFX_TVRAM_ATTR + i)) {
            g->tvram[0x2000 + i] = 0xE1;
            g->tvram[0x2000 + i + 1] = 0;
        }
    }
    memset(g->gvram, 0, sizeof g->gvram);
    memset(g->gaiji, 0, sizeof g->gaiji);

    gdc_reset(&g->gdc[0]);
    gdc_reset(&g->gdc[1]);
    Pc98GfxGdc *t = &g->gdc[0], *gr = &g->gdc[1];
    t->display_on = 1;
    t->pitch = 80;
    t->csrform[0] = 0x0F;                           /* cursor off, 16 lines per row */
    t->csrform[1] = 0x00;                           /* cursor from line 0, blinking */
    t->csrform[2] = (uint8_t)(15 << 3 | 3);         /* ... to line 15; blink rate 12 frames */
    set_partition(t, 0, 0, 400);
    gr->display_on = 0;
    gr->pitch = 40;
    set_partition(gr, 0, 0, 400);
    memset(gr->pram + 8, 0xFF, 8);                  /* the drawing pattern: solid */

    memset(&g->grcg, 0, sizeof g->grcg);
    memset(&g->egc, 0, sizeof g->egc);
    g->egc.reg[PC98GFX_EGC_ACCESS] = 0xFFF0;        /* all planes, the values programs start with */
    g->egc.reg[PC98GFX_EGC_FGBG] = 0x00FF;
    g->egc.reg[PC98GFX_EGC_MASK] = 0xFFFF;
    g->egc.reg[PC98GFX_EGC_LENG] = 0x000F;

    g->mode1 = PC98GFX_FF1_DISPLAY;
    g->analog = 0;
    g->egc_mode = 0;
    g->egc_permit = 0;
    g->border = 0;
    memset(g->crtc, 0, sizeof g->crtc);
    g->disp_page = g->draw_page = 0;
    g->pal_digital[0] = 0x37;                       /* colour n shows as GRB code n */
    g->pal_digital[1] = 0x26;
    g->pal_digital[2] = 0x15;
    g->pal_digital[3] = 0x04;
    g->pal_index = 0;
    for (int i = 0; i < 16; i++) {                  /* our own default: eight dark and eight bright GRB colours */
        uint8_t level = (i & 8) ? 15 : 7;
        g->pal_analog[i][0] = (i & 4) ? level : 0;
        g->pal_analog[i][1] = (i & 2) ? level : 0;
        g->pal_analog[i][2] = (i & 1) ? level : 0;
    }
    g->pal_analog[8][0] = g->pal_analog[8][1] = g->pal_analog[8][2] = 4;
    g->cg_code_lo = g->cg_code_hi = g->cg_line = 0;
    g->vsync = g->hblank = 0;
    g->frame = 0;
    g->ff2_unknown = 0;
    g->ff2_last_unknown = 0;
}

void pc98gfx_init(Pc98Gfx *g) {
    memset(g, 0, sizeof *g);
    g->has_egc = 1;
    pc98gfx_reset(g);
}

void pc98gfx_set_font(Pc98Gfx *g, const uint8_t *ank8x16, Pc98GfxKanjiFn kanji, void *user) {
    g->ank = ank8x16;
    g->kanji = kanji;
    g->kanji_user = user;
}

uint8_t *pc98gfx_plane(Pc98Gfx *g, int page, int plane) {
    return g->gvram[page & 1][plane & 3];
}

uint8_t pc98gfx_memsw(const Pc98Gfx *g, int n) {
    if (n < 1 || n > 8) return 0xFF;
    return g->tvram[PC98GFX_MEMSW - PC98GFX_TVRAM + 4u * (unsigned)(n - 1)];
}

void pc98gfx_set_memsw(Pc98Gfx *g, int n, uint8_t v) {
    if (n >= 1 && n <= 8) g->tvram[PC98GFX_MEMSW - PC98GFX_TVRAM + 4u * (unsigned)(n - 1)] = v;
}

/* ------------------------------------------------------------------ text VRAM */

static void text_write(Pc98Gfx *g, uint32_t a, uint8_t v) {
    if (is_memsw(a) && !(g->mode1 & PC98GFX_FF1_MEMSW_WRITE)) return;     /* NVMW protect */
    g->tvram[a - PC98GFX_TVRAM] = v;
}

/* ------------------------------------------------------------------ the GRCG */

/* Tile compare read: a 1 for each dot whose colour equals the tiles on every enabled plane. In RMW mode a read is an
 * ordinary read of the plane addressed. */
static uint8_t grcg_read(Pc98Gfx *g, int pl, unsigned off) {
    uint8_t m = g->grcg.mode;
    if (m & PC98GFX_GRCG_RMW) return g->gvram[g->draw_page][pl][off];
    uint8_t r = 0xFF;
    for (int p = 0; p < 4; p++)
        if (!(m & (1 << p))) r &= (uint8_t)~(g->gvram[g->draw_page][p][off] ^ g->grcg.tile[p]);
    return r;
}

/* Tile data write (the CPU's byte is ignored: every enabled plane gets its tile) or read-modify-write (the CPU's byte
 * says which dots take the tile). The plane in the address does not matter. */
static void grcg_write(Pc98Gfx *g, unsigned off, uint8_t v) {
    uint8_t m = g->grcg.mode;
    for (int p = 0; p < 4; p++) {
        if (m & (1 << p)) continue;
        uint8_t *b = &g->gvram[g->draw_page][p][off];
        if (m & PC98GFX_GRCG_RMW) *b = (uint8_t)((*b & ~v) | (g->grcg.tile[p] & v));
        else *b = g->grcg.tile[p];
    }
}

/* ------------------------------------------------------------------ the EGC */

#define EGC_OPE_SHIFT_CPU   0x0400      /* the shifter is fed by CPU writes (else by VRAM reads) */
#define EGC_OPE_COMPARE     0x2000      /* reads return a compare result */

/* The EGC works on 16-dot words. An access covers dots lo..hi (0 = the left dot) of the word at an even plane offset:
 * all sixteen for a word access, eight for a byte access. Because a byte is half of a word rather than a unit of its
 * own, a word access that arrives as two byte accesses (low byte first, as from a CPU with an 8-bit bus) does what the
 * word does, for rows that run left to right; a descending row needs real word accesses. */

static uint16_t word_get(const uint8_t *pl, unsigned w) {           /* dot order: bit 15 is the left dot */
    return (uint16_t)(pl[w] << 8 | pl[w + 1]);
}

static void word_put(uint8_t *pl, unsigned w, uint16_t v) {
    pl[w] = (uint8_t)(v >> 8);
    pl[w + 1] = (uint8_t)v;
}

static uint16_t dots_mask(unsigned lo, unsigned hi) {               /* dots lo..hi of a dot-ordered word */
    return (uint16_t)((0xFFFFu >> lo) & (0xFFFFu << (15 - hi)));
}

/* Dots first..last of a dot-ordered word in stream order (the first dot in bit n - 1 of the result), and back. In a
 * descending row the stream runs right to left, so `first` is the higher dot number. */
static uint32_t take(uint16_t v, unsigned last, unsigned n, int desc) {
    uint32_t m = (1u << n) - 1u;
    return desc ? (rev16(v) >> last) & m : ((uint32_t)v >> (15 - last)) & m;
}

static uint16_t put(uint32_t s, unsigned last, int desc) {
    return desc ? rev16((uint16_t)(s << last)) : (uint16_t)(s << (15 - last));
}

static int egc_desc(const Pc98GfxEgc *e) {
    return (e->reg[PC98GFX_EGC_SFT] & 0x1000) != 0;
}

/* The part of an access a row's stream uses: all of it, or, while the row has not started on this side of the
 * shifter, from the row's bit address on (none of it when the row starts further on). Returns the dot count. */
static unsigned row_part(unsigned lo, unsigned hi, int started, unsigned bit, int desc, unsigned *first, unsigned *last) {
    *first = desc ? hi : lo;
    *last = desc ? lo : hi;
    if (!started) {
        if (desc ? bit < lo : bit > hi) return 0;
        if (desc ? bit < *first : bit > *first) *first = bit;
    }
    return desc ? *first - *last + 1 : *last - *first + 1;
}

/* Feeds the dots of an access into the shifter (all planes). The shifter keeps the newest 64 dots. */
static void egc_push(Pc98GfxEgc *e, const uint16_t v[4], unsigned lo, unsigned hi) {
    int desc = egc_desc(e);
    unsigned first, last;
    unsigned n = row_part(lo, hi, e->in_started, e->reg[PC98GFX_EGC_SFT] & 15u, desc, &first, &last);
    if (!n) return;
    e->in_started = 1;
    if (e->qn + n > 64) {
        unsigned drop = e->qn + n - 64;
        for (int p = 0; p < 4; p++) e->q[p] <<= drop;
        e->qn = (uint8_t)(e->qn - drop);
    }
    for (int p = 0; p < 4; p++) e->q[p] |= ((uint64_t)take(v[p], last, n, desc) << (64 - n)) >> e->qn;
    e->qn = (uint8_t)(e->qn + n);
}

/* Takes m dots (1-16) out of the shifter, per plane; 0 when it holds fewer. */
static int egc_pop(Pc98GfxEgc *e, unsigned m, uint32_t out[4]) {
    if (!m || e->qn < m) return 0;
    for (int p = 0; p < 4; p++) {
        out[p] = (uint32_t)(e->q[p] >> (64 - m));
        e->q[p] <<= m;
    }
    e->qn = (uint8_t)(e->qn - m);
    return 1;
}

/* The pattern operand: the pattern register, or the background or foreground colour (every dot of a plane set when
 * the colour has that plane's bit), as 4A2h bits 13-14 choose. */
static void egc_pattern(const Pc98GfxEgc *e, uint16_t P[4]) {
    unsigned src = (e->reg[PC98GFX_EGC_FGBG] >> 13) & 3;
    for (int p = 0; p < 4; p++) {
        if (src == 1) P[p] = (e->reg[PC98GFX_EGC_BG] >> p) & 1 ? 0xFFFF : 0;
        else if (src == 2) P[p] = (e->reg[PC98GFX_EGC_FG] >> p) & 1 ? 0xFFFF : 0;
        else P[p] = e->pat[p];
    }
}

/* The three-operand raster operation: bit k of the code is the result for S = k bit 2, D = k bit 1, P = k bit 0
 * (so F0h is the source, CCh the destination, AAh the pattern). */
static uint16_t egc_rop(uint8_t rop, uint16_t s, uint16_t d, uint16_t p) {
    uint16_t r = 0;
    for (int k = 0; k < 8; k++)
        if (rop & (1 << k))
            r |= (uint16_t)((k & 4 ? s : (uint16_t)~s) & (k & 2 ? d : (uint16_t)~d) & (k & 1 ? p : (uint16_t)~p));
    return r;
}

/* A VRAM read through the EGC of dots lo..hi of the word at w. Returns the whole word in dot order (the caller keeps
 * its byte of a byte access). */
static uint16_t egc_read(Pc98Gfx *g, unsigned w, unsigned lo, unsigned hi) {
    Pc98GfxEgc *e = &g->egc;
    uint16_t ope = e->reg[PC98GFX_EGC_OPE], V[4], lane = dots_mask(lo, hi);
    e->tail = 0;
    for (int p = 0; p < 4; p++) V[p] = word_get(g->gvram[g->draw_page][p], w);
    if (((ope >> 8) & 3) == 1)                      /* the pattern register follows VRAM reads */
        for (int p = 0; p < 4; p++) e->pat[p] = (uint16_t)((e->pat[p] & ~lane) | (V[p] & lane));
    if (!(ope & EGC_OPE_SHIFT_CPU)) egc_push(e, V, lo, hi);     /* the source is VRAM: what was read enters */
    if (ope & EGC_OPE_COMPARE) {                    /* dots where every selected plane matches the pattern operand */
        uint16_t P[4], r = 0xFFFF;
        egc_pattern(e, P);
        for (int p = 0; p < 4; p++)
            if (e->reg[PC98GFX_EGC_FGBG] & (1 << p)) r &= (uint16_t)~(V[p] ^ P[p]);
        return r;
    }
    return V[(e->reg[PC98GFX_EGC_FGBG] >> 8) & 3];  /* the read plane */
}

/* A VRAM write through the EGC to dots lo..hi of the word at w: the source dots come out of the shifter aligned to the
 * destination bit address, are combined with the destination and the pattern, and land on the enabled planes under
 * the mask. A write that finds fewer dots in the shifter than it needs writes nothing (the first write of a row whose
 * source bit address is ahead of the destination's, unless the program read one extra word first). After the row's
 * bit length the shifter starts over by itself. cpu: the CPU's data in dot order (only lo..hi matter). */
static void egc_write(Pc98Gfx *g, unsigned w, unsigned lo, unsigned hi, uint16_t cpu) {
    Pc98GfxEgc *e = &g->egc;
    uint16_t ope = e->reg[PC98GFX_EGC_OPE];
    int desc = egc_desc(e);
    if (e->tail) {                                  /* the second byte of the word whose first byte ended the row */
        int rest = e->tail == w + 1 && lo == 8 && hi == 15;
        e->tail = 0;
        if (rest) return;                           /* (a word access would have dropped these dots with the row) */
    }
    if (ope & EGC_OPE_SHIFT_CPU) {
        uint16_t c[4] = { cpu, cpu, cpu, cpu };
        egc_push(e, c, lo, hi);
    }
    unsigned first, last, len = (e->reg[PC98GFX_EGC_LENG] & 0xFFFu) + 1u;
    unsigned n = row_part(lo, hi, e->out_started, (e->reg[PC98GFX_EGC_SFT] >> 4) & 15u, desc, &first, &last);
    if (!n) return;                                 /* the row starts further on */
    if (n > len - e->out_done) {                    /* the row ends inside this access */
        n = len - e->out_done;
        last = desc ? first - n + 1 : first + n - 1;
    }
    uint32_t s[4];
    if (!egc_pop(e, n, s)) {
        e->underruns++;
        return;
    }
    uint16_t S[4], D[4], P[4], valid = put((1u << n) - 1u, last, desc);
    for (int p = 0; p < 4; p++) {
        S[p] = put(s[p], last, desc);
        D[p] = word_get(g->gvram[g->draw_page][p], w);
    }
    egc_pattern(e, P);
    uint16_t mask = (uint16_t)(swap16(e->reg[PC98GFX_EGC_MASK]) & valid);
    unsigned ws = (ope >> 11) & 3;
    for (int p = 0; p < 4; p++) {
        if (e->reg[PC98GFX_EGC_ACCESS] & (1 << p)) continue;
        uint16_t r = ws == 1 ? egc_rop((uint8_t)ope, S[p], D[p], P[p]) : ws == 2 ? P[p] : S[p];
        word_put(g->gvram[g->draw_page][p], w, (uint16_t)((D[p] & ~mask) | (r & mask)));
    }
    if (((ope >> 8) & 3) == 2) {                    /* the pattern register follows the destination of writes */
        uint16_t lane = dots_mask(lo, hi);
        for (int p = 0; p < 4; p++) e->pat[p] = (uint16_t)((e->pat[p] & ~lane) | (D[p] & lane));
    }
    e->out_started = 1;
    e->out_done = (uint16_t)(e->out_done + n);
    if (e->out_done >= len) {
        egc_shifter_reset(e);
        if (lo == 0 && hi == 7) e->tail = (uint16_t)(w + 1);    /* the even byte: a split word sends it first */
    }
}

static void egc_port_write(Pc98Gfx *g, uint16_t port, uint8_t v) {
    if (!g->has_egc || !g->egc_mode) return;        /* the registers answer only in EGC mode */
    unsigned r = (unsigned)(port - 0x4A0u) >> 1;
    uint16_t *reg = &g->egc.reg[r];
    if (port & 1) *reg = (uint16_t)((*reg & 0x00FF) | v << 8);
    else *reg = (uint16_t)((*reg & 0xFF00) | v);
    if (r == PC98GFX_EGC_SFT || r == PC98GFX_EGC_LENG) egc_shifter_reset(&g->egc);
}

/* ------------------------------------------------------------------ graphics VRAM as the CPU sees it */

enum { VRAM_PLAIN, VRAM_GRCG, VRAM_EGC };

static int vram_mode(const Pc98Gfx *g) {
    if (!(g->grcg.mode & PC98GFX_GRCG_ON)) return VRAM_PLAIN;
    return (g->egc_mode && g->has_egc) ? VRAM_EGC : VRAM_GRCG;
}

static int addr_plane(uint32_t a) {
    if (a >= PC98GFX_PLANE_B && a < 0xC0000u) return (int)((a - PC98GFX_PLANE_B) >> 15);
    if (a >= PC98GFX_PLANE_E && a < PC98GFX_PLANE_E + PC98GFX_PLANE_SIZE) return 3;
    return -1;
}

int pc98gfx_mem_claims(uint32_t a) {
    return (a >= PC98GFX_TVRAM && a < PC98GFX_CGWIN_END) || addr_plane(a) >= 0;
}

uint8_t pc98gfx_mem_read8(Pc98Gfx *g, uint32_t a) {
    if (a >= PC98GFX_TVRAM && a < PC98GFX_TVRAM_END) return g->tvram[a - PC98GFX_TVRAM];
    int pl = addr_plane(a);
    if (pl < 0) return 0xFF;                        /* the CG window and anything not ours */
    unsigned off = a & 0x7FFFu;
    switch (vram_mode(g)) {
    case VRAM_PLAIN: return g->gvram[g->draw_page][pl][off];
    case VRAM_GRCG: return grcg_read(g, pl, off);
    default: {                                      /* half of a word: the even byte holds the left 8 dots */
        unsigned lo = (off & 1) * 8;
        uint16_t r = egc_read(g, off & ~1u, lo, lo + 7);
        return (off & 1) ? (uint8_t)r : (uint8_t)(r >> 8);
    }
    }
}

void pc98gfx_mem_write8(Pc98Gfx *g, uint32_t a, uint8_t v) {
    if (a >= PC98GFX_TVRAM && a < PC98GFX_TVRAM_END) {
        text_write(g, a, v);
        return;
    }
    int pl = addr_plane(a);
    if (pl < 0) return;
    unsigned off = a & 0x7FFFu;
    switch (vram_mode(g)) {
    case VRAM_PLAIN: g->gvram[g->draw_page][pl][off] = v; return;
    case VRAM_GRCG: grcg_write(g, off, v); return;
    default: {
        unsigned lo = (off & 1) * 8;
        egc_write(g, off & ~1u, lo, lo + 7, (off & 1) ? v : (uint16_t)(v << 8));
        return;
    }
    }
}

uint16_t pc98gfx_mem_read16(Pc98Gfx *g, uint32_t a) {
    if (!(a & 1) && addr_plane(a) >= 0 && vram_mode(g) == VRAM_EGC)
        return swap16(egc_read(g, a & 0x7FFFu, 0, 15));
    uint8_t lo = pc98gfx_mem_read8(g, a);
    return (uint16_t)(lo | pc98gfx_mem_read8(g, a + 1) << 8);
}

void pc98gfx_mem_write16(Pc98Gfx *g, uint32_t a, uint16_t v) {
    if (!(a & 1) && addr_plane(a) >= 0 && vram_mode(g) == VRAM_EGC) {
        egc_write(g, a & 0x7FFFu, 0, 15, swap16(v));
        return;
    }
    pc98gfx_mem_write8(g, a, (uint8_t)v);
    pc98gfx_mem_write8(g, a + 1, (uint8_t)(v >> 8));
}

/* ------------------------------------------------------------------ the GDCs */

/* Display memory as each GDC sees it, one GDC word per address. The text GDC (character mode) addresses the 8192
 * words of text VRAM (codes 0000h-0FFFh, attributes 1000h-1FFFh); the graphics GDC addresses the four planes of the
 * draw page, 4000h words each (B 0000h, R 4000h, G 8000h, E C000h), with the bit order of its words reversed against
 * the CPU's (its dot 0 is the left dot). */
static uint16_t gdc_mem_read(const Pc98Gfx *g, int gi, uint32_t addr) {
    if (gi == 0) {
        unsigned i = (addr & 0x1FFFu) * 2;
        return (uint16_t)(g->tvram[i] | g->tvram[i + 1] << 8);
    }
    const uint8_t *pl = g->gvram[g->draw_page][(addr >> 14) & 3];
    unsigned o = (addr & 0x3FFFu) * 2;
    return rev16((uint16_t)(pl[o] << 8 | pl[o + 1]));
}

static void gdc_mem_write(Pc98Gfx *g, int gi, uint32_t addr, uint16_t v) {
    if (gi == 0) {
        unsigned i = (addr & 0x1FFFu) * 2;
        text_write(g, PC98GFX_TVRAM + i, (uint8_t)v);
        text_write(g, PC98GFX_TVRAM + i + 1, (uint8_t)(v >> 8));
        return;
    }
    uint8_t *pl = g->gvram[g->draw_page][(addr >> 14) & 3];
    unsigned o = (addr & 0x3FFFu) * 2;
    uint16_t d = rev16(v);
    pl[o] = (uint8_t)(d >> 8);
    pl[o + 1] = (uint8_t)d;
}

/* The read-modify-write logic the GDC applies to every memory cycle (WDAT's MOD field). */
static uint16_t gdc_rmw(uint16_t old, uint16_t data, uint16_t mask, int mod) {
    switch (mod & 3) {
    case 0: return (uint16_t)((old & ~mask) | (data & mask));     /* replace */
    case 1: return (uint16_t)(old ^ (data & mask));               /* complement */
    case 2: return (uint16_t)(old & ~(data & mask));              /* reset */
    default: return (uint16_t)(old | (data & mask));              /* set */
    }
}

/* Drawing directions: 0 is down the screen (the next line), 2 right, 4 up, 6 left; odd ones are the diagonals. */
static const int8_t DIR_X[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
static const int8_t DIR_Y[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };

static void gdc_step_dot(Pc98GfxGdc *d, int dir) {
    int dx = DIR_X[dir & 7], dy = DIR_Y[dir & 7];
    if (dx > 0) {
        if (++d->dad == 16) { d->dad = 0; d->ead++; }
    } else if (dx < 0) {
        if (d->dad == 0) { d->dad = 15; d->ead--; } else d->dad--;
    }
    if (dy > 0) d->ead += d->pitch;
    else if (dy < 0) d->ead -= d->pitch;
    d->ead &= 0x3FFFF;
}

static void gdc_step_word(Pc98GfxGdc *d, int dir) {
    int32_t delta = DIR_X[dir & 7] + DIR_Y[dir & 7] * (int32_t)d->pitch;
    d->ead = (uint32_t)((int32_t)d->ead + delta) & 0x3FFFF;
}

static uint16_t gdc_pattern(const Pc98GfxGdc *d) {
    return (uint16_t)(d->pram[8] | d->pram[9] << 8);
}

static void gdc_plot(Pc98Gfx *g, Pc98GfxGdc *d, int bit) {
    uint16_t m = (uint16_t)(1u << d->dad);
    uint16_t old = gdc_mem_read(g, 1, d->ead);
    gdc_mem_write(g, 1, d->ead, gdc_rmw(old, bit ? m : 0, m, d->mod));
    d->dots++;
}

/* An octant: the axis step and the diagonal step that bound it (octant k lies between directions k and k + 1), and the
 * step across the axis towards the diagonal (axis + across = diagonal). */
static void octant(int oct, int *axis, int *diag, int *across) {
    oct &= 7;
    if (oct & 1) {
        *axis = (oct + 1) & 7;
        *diag = oct;
        *across = (oct + 7) & 7;
    } else {
        *axis = oct;
        *diag = (oct + 1) & 7;
        *across = (oct + 2) & 7;
    }
}

/* Line (data sheet parameters: DC = |dI|, D = 2|dD| - |dI|, D2 = 2(|dD| - |dI|), D1 = 2|dD|): DC + 1 dots from the
 * cursor; each step goes diagonally while D is not negative (D += D2), else along the axis (D += D1). */
static void gdc_line(Pc98Gfx *g, Pc98GfxGdc *d) {
    int axis, diag, across;
    octant(d->dir, &axis, &diag, &across);
    int32_t e = sx14(d->d), d1 = sx14(d->d1), d2 = sx14(d->d2);
    unsigned n = d->dc & 0x3FFF;
    uint16_t pat = gdc_pattern(d);
    gdc_plot(g, d, pat & 1);
    for (unsigned i = 1; i <= n; i++) {
        if (e >= 0) { gdc_step_dot(d, diag); e += d2; }
        else { gdc_step_dot(d, axis); e += d1; }
        gdc_plot(g, d, (pat >> (i & 15)) & 1);
    }
}

/* Arc (DC = r sin(end angle), D = r - 1, DM = r sin(start angle)): one octant of a circle of radius r starting where
 * its tangent is the octant's axis direction, bending towards the diagonal. Step i is i dots along the axis and
 * r - round(sqrt(r^2 - i^2)) dots across; steps before DM are not drawn. (The chip's own error recurrence is not in
 * the data sheet; this picks the dot nearest the true circle.) */
static void gdc_arc(Pc98Gfx *g, Pc98GfxGdc *d) {
    int axis, diag, across;
    octant(d->dir, &axis, &diag, &across);
    uint32_t r = (d->d & 0x3FFFu) + 1u;
    unsigned n = d->dc & 0x3FFF, skip = d->dm & 0x3FFF;
    uint16_t pat = gdc_pattern(d);
    uint32_t moved = 0;                             /* dots stepped across so far */
    for (unsigned i = 0; i <= n; i++) {
        uint32_t want = r;                          /* beyond the radius (nonsense DC): stay at the centre line */
        if (i <= r) want = r - (isqrt32(4u * (r * r - i * i)) + 1u) / 2u;
        if (i) {
            if (want > moved) { gdc_step_dot(d, diag); moved++; }
            else gdc_step_dot(d, axis);
            while (want > moved) {                  /* steeper than the diagonal: the extra dots across */
                gdc_step_dot(d, across);
                moved++;
            }
        }
        if (i >= skip) gdc_plot(g, d, (pat >> (i & 15)) & 1);
    }
}

/* Rectangle (DC = 3, D = A - 1, D2 = B - 1, DM = A - 1): four sides from the cursor, turning by 90 degrees (two
 * direction steps) after each: A - 1 dots, B - 1, A - 1, B - 1. Each dot once, the cursor ends where it started. */
static void gdc_rect(Pc98Gfx *g, Pc98GfxGdc *d) {
    int dir = d->dir & 7;
    unsigned sides = (d->dc & 0x3FFFu) + 1u, k = 0;
    if (sides > 4) sides = 4;
    uint16_t pat = gdc_pattern(d);
    for (unsigned s = 0; s < sides; s++) {
        unsigned len = (s & 1) ? (d->d2 & 0x3FFFu) : s == 0 ? (d->d & 0x3FFFu) : (d->dm & 0x3FFFu);
        for (unsigned j = 0; j < len; j++, k++) {
            gdc_plot(g, d, (pat >> (k & 15)) & 1);
            gdc_step_dot(d, dir);
        }
        dir = (dir + 2) & 7;
    }
}

/* Graphics character (GCHRD): the 8x8 pattern in PRAM bytes 8-15 (byte 8 + j is line j, bit i is dot i), DC + 1 lines
 * of D dots (8 when D is 0), each pattern dot drawn zoom x zoom times (ZOOM's low nibble + 1). Dots run along DIR, lines
 * advance two direction steps clockwise from it (down for DIR = 2). */
static void gdc_gchrd(Pc98Gfx *g, Pc98GfxGdc *d) {
    int dir = d->dir & 7, ldir = (dir + 6) & 7;
    unsigned zw = (d->zoom & 15u) + 1u;
    unsigned lines = (d->dc & 0x3FFFu) + 1u, dots = d->d & 0x3FFFu;
    if (!dots) dots = 8;
    if (lines > 1024) lines = 1024;                 /* bounded work for nonsense parameters */
    if (dots > 1024) dots = 1024;
    for (unsigned j = 0; j < lines; j++) {
        uint8_t row = d->pram[8 + (j & 7)];
        for (unsigned zy = 0; zy < zw; zy++) {
            uint32_t ead0 = d->ead;
            uint8_t dad0 = d->dad;
            for (unsigned i = 0; i < dots; i++)
                for (unsigned zx = 0; zx < zw; zx++) {
                    gdc_plot(g, d, (row >> (i & 7)) & 1);
                    gdc_step_dot(d, dir);
                }
            d->ead = ead0;
            d->dad = dad0;
            gdc_step_dot(d, ldir);
        }
    }
}

static void gdc_figd(Pc98Gfx *g, int gi) {
    Pc98GfxGdc *d = &g->gdc[gi];
    if (gi == 0) {                                  /* character mode: no figure drawing here */
        d->unknown++;
        return;
    }
    uint8_t f = d->figs_flags;
    if (f & 0x08) gdc_line(g, d);                   /* L */
    else if (f & 0x20) gdc_arc(g, d);               /* A */
    else if (f & 0x40) gdc_rect(g, d);              /* R */
    else {                                          /* no figure: DC + 1 dots along DIR */
        unsigned n = d->dc & 0x3FFF;
        uint16_t pat = gdc_pattern(d);
        for (unsigned i = 0; i <= n; i++) {
            if (i) gdc_step_dot(d, d->dir);
            gdc_plot(g, d, (pat >> (i & 15)) & 1);
        }
    }
    d->mask = (uint16_t)(1u << d->dad);             /* the dot address follows the drawing */
}

/* One WDAT transfer: DC + 1 memory cycles with the same data, moving along DIR by words. The graphics GDC writes under
 * its MASK register; the text GDC (character mode) writes the whole word (or the byte TYPE selects). */
static void gdc_wdat_unit(Pc98Gfx *g, int gi, uint16_t data, uint16_t typemask) {
    Pc98GfxGdc *d = &g->gdc[gi];
    uint32_t n = (d->dc & 0x3FFFu) + 1u;
    uint16_t m = gi == 0 ? typemask : (uint16_t)(d->mask & typemask);
    for (uint32_t k = 0; k < n; k++) {
        uint16_t old = gdc_mem_read(g, gi, d->ead);
        gdc_mem_write(g, gi, d->ead, gdc_rmw(old, data, m, d->mod));
        d->words_written++;
        gdc_step_word(d, d->dir);
    }
}

static void gdc_command(Pc98Gfx *g, int gi, uint8_t c) {
    Pc98GfxGdc *d = &g->gdc[gi];
    d->cmd = c;
    d->nparam = 0;
    d->commands++;
    d->wlow_valid = 0;
    d->fifo_len = d->fifo_pos = 0;                  /* a new command ends any read */
    d->rdat_left = 0;
    d->rdat_high_valid = 0;
    switch (c) {
    case 0x00: case 0x01: case 0x09:                /* RESET (and the 7220A's variants): blank; SYNC parameters follow */
        d->display_on = 0;
        return;
    case 0x0E: case 0x0F:                           /* SYNC, bit 0 = display enable */
    case 0x0C: case 0x0D:                           /* BCTRL */
        d->display_on = c & 1;
        return;
    case 0x6B:                                      /* START */
        d->display_on = 1;
        return;
    case 0x6E: case 0x6F:                           /* VSYNC master/slave */
    case 0x46: case 0x47: case 0x49: case 0x4A: case 0x4B:      /* ZOOM, PITCH, CSRW, MASK, CSRFORM: parameters follow */
        return;
    case 0x4C:                                      /* FIGS: the drawing parameters start from their defaults */
        d->figs_flags = 0;
        d->dir = 0;
        d->dc = 0;
        d->d = 8;
        d->d2 = 8;
        d->d1 = 0x3FFF;
        d->dm = 0x3FFF;
        return;
    case 0x6C:                                      /* FIGD */
        gdc_figd(g, gi);
        return;
    case 0x68:                                      /* GCHRD */
        if (gi == 0) d->unknown++; else gdc_gchrd(g, d);
        return;
    case 0xE0:                                      /* CURD (CSRR): EAD in three bytes, then the MASK register */
        d->fifo[0] = (uint8_t)d->ead;
        d->fifo[1] = (uint8_t)(d->ead >> 8);
        d->fifo[2] = (uint8_t)((d->ead >> 16) & 3);
        d->fifo[3] = (uint8_t)d->mask;
        d->fifo[4] = (uint8_t)(d->mask >> 8);
        d->fifo_len = 5;
        return;
    case 0xC0:                                      /* LPRD: no light pen */
        d->fifo[0] = d->fifo[1] = d->fifo[2] = 0;
        d->fifo_len = 3;
        return;
    default:
        break;
    }
    if ((c & 0xF0) == 0x70) return;                 /* PRAM from address c & 15: parameters follow */
    if ((c & 0xE0) == 0x20) {                       /* 001TTxMM: WDAT (x = 0) or DMAW (x = 1) */
        if (c & 4) { d->dma++; return; }
        d->wtype = (c >> 3) & 3;
        d->mod = c & 3;
        return;
    }
    if ((c & 0xE0) == 0xA0) {                       /* 101TTxMM: RDAT or DMAR */
        if (c & 4) { d->dma++; return; }
        d->rdat_type = (c >> 3) & 3;
        d->rdat_left = (d->dc & 0x3FFFu) + 1u;
        return;
    }
    d->unknown++;
}

static void set14(uint16_t *f, int high, uint8_t v) {
    if (high) *f = (uint16_t)((*f & 0x00FF) | (v & 0x3F) << 8);
    else *f = (uint16_t)((*f & 0x3F00) | v);
}

static void gdc_param(Pc98Gfx *g, int gi, uint8_t v) {
    Pc98GfxGdc *d = &g->gdc[gi];
    unsigned i = d->nparam;
    if (d->nparam < 255) d->nparam++;
    uint8_t c = d->cmd;
    if (c == 0x00 || c == 0x01 || c == 0x09 || c == 0x0E || c == 0x0F) {
        if (i < 8) d->sync[i] = v;
        return;
    }
    if ((c & 0xF0) == 0x70) {
        unsigned a = (c & 15u) + i;
        if (a < 16) d->pram[a] = v;
        return;
    }
    if ((c & 0xE4) == 0x20) {                       /* WDAT data: words as low/high pairs, or single bytes */
        if (d->wtype == 2) gdc_wdat_unit(g, gi, v, 0x00FF);
        else if (d->wtype == 3) gdc_wdat_unit(g, gi, (uint16_t)(v << 8), 0xFF00);
        else if (!d->wlow_valid) { d->wlow = v; d->wlow_valid = 1; }
        else { d->wlow_valid = 0; gdc_wdat_unit(g, gi, (uint16_t)(d->wlow | v << 8), 0xFFFF); }
        return;
    }
    switch (c) {
    case 0x46: if (i == 0) d->zoom = v; return;
    case 0x47: if (i == 0) d->pitch = v; return;
    case 0x4A:
        if (i == 0) d->mask = (uint16_t)((d->mask & 0xFF00) | v);
        else if (i == 1) d->mask = (uint16_t)((d->mask & 0x00FF) | v << 8);
        return;
    case 0x4B: if (i < 3) d->csrform[i] = v; return;
    case 0x49:                                      /* CSRW: EAD low, middle, then its 2 high bits and the dot address */
        if (i == 0) d->ead = (d->ead & ~0xFFu) | v;
        else if (i == 1) d->ead = (d->ead & ~0xFF00u) | (uint32_t)v << 8;
        else if (i == 2) {
            d->ead = (d->ead & 0xFFFFu) | (uint32_t)(v & 3) << 16;
            if (gi == 1) {                          /* graphics mode: the dot address, also as the mask */
                d->dad = (uint8_t)(v >> 4);
                d->mask = (uint16_t)(1u << d->dad);
            }
        }
        return;
    case 0x4C:                                      /* FIGS: flags and direction, then DC, D, D2, D1, DM (14 bits each) */
        switch (i) {
        case 0: d->figs_flags = v & 0xF8; d->dir = v & 7; return;
        case 1: case 2: set14(&d->dc, i == 2, v); return;
        case 3: case 4: set14(&d->d, i == 4, v); return;
        case 5: case 6: set14(&d->d2, i == 6, v); return;
        case 7: case 8: set14(&d->d1, i == 8, v); return;
        case 9: case 10: set14(&d->dm, i == 10, v); return;
        default: return;
        }
    default:
        return;
    }
}

static int gdc_data_ready(const Pc98GfxGdc *d) {
    return d->fifo_pos < d->fifo_len || d->rdat_high_valid || d->rdat_left;
}

static uint8_t gdc_read_data(Pc98Gfx *g, int gi) {
    Pc98GfxGdc *d = &g->gdc[gi];
    if (d->fifo_pos < d->fifo_len) return d->fifo[d->fifo_pos++];
    if (d->rdat_high_valid) {
        d->rdat_high_valid = 0;
        return d->rdat_high;
    }
    if (d->rdat_left) {                             /* RDAT: the next word, moving along DIR */
        uint16_t w = gdc_mem_read(g, gi, d->ead);
        gdc_step_word(d, d->dir);
        d->rdat_left--;
        if (d->rdat_type == 3) return (uint8_t)(w >> 8);
        if (d->rdat_type != 2) {
            d->rdat_high = (uint8_t)(w >> 8);
            d->rdat_high_valid = 1;
        }
        return (uint8_t)w;
    }
    return 0xFF;
}

/* Status: FIFO empty always (commands run at once, so drawing is never in progress), data ready, the beam. */
static uint8_t gdc_status(const Pc98Gfx *g, int gi) {
    uint8_t s = 0x04;
    if (gdc_data_ready(&g->gdc[gi])) s |= 0x01;
    if (g->vsync) s |= 0x20;
    if (g->hblank) s |= 0x40;
    return s;
}

/* ------------------------------------------------------------------ character generator and fonts */

/* A5h bit 5 picks the half of a 16-dot character the A9h port reads or writes: set = the left half. */
#define CG_LEFT 0x20

static int is_gaiji_row(uint8_t row) {      /* row = JIS first byte - 20h: the user-defined rows 76h and 77h */
    row &= 0x7F;
    return row == 0x56 || row == 0x57;
}

/* A two-byte character's 32-byte glyph: from the CG RAM for user-defined rows, else from the kanji source. */
static int kanji_glyph(const Pc98Gfx *g, uint8_t row, uint8_t cell, uint8_t out[32]) {
    row &= 0x7F;
    if (is_gaiji_row(row)) {
        memcpy(out, g->gaiji[row - 0x56][cell & 0x7F], 32);
        return 1;
    }
    if (g->kanji && g->kanji(g->kanji_user, (uint16_t)((row + 0x20u) << 8 | (cell & 0x7Fu)), out)) return 1;
    memset(out, 0, 32);
    return 0;
}

int pc98gfx_cg_glyph(const Pc98Gfx *g, uint16_t code, uint8_t glyph[32]) {
    uint8_t lo = (uint8_t)code, hi = (uint8_t)(code >> 8);
    if (hi) return kanji_glyph(g, lo, hi, glyph);
    memset(glyph, 0, 32);
    if (!g->ank) return 0;
    memcpy(glyph, g->ank + (unsigned)lo * 16, 16);
    return 1;
}

static uint8_t cg_read(const Pc98Gfx *g) {
    unsigned line = g->cg_line & 15u;
    if (!(g->cg_code_hi & 0x7F)) return g->ank ? g->ank[(unsigned)g->cg_code_lo * 16 + line] : 0;
    uint8_t glyph[32];
    kanji_glyph(g, g->cg_code_hi, g->cg_code_lo, glyph);
    return glyph[((g->cg_line & CG_LEFT) ? 0 : 16) + line];
}

static void cg_write(Pc98Gfx *g, uint8_t v) {
    uint8_t row = g->cg_code_hi & 0x7F;
    if (!is_gaiji_row(row)) return;                 /* the ROM characters cannot be written */
    g->gaiji[row - 0x56][g->cg_code_lo & 0x7F][((g->cg_line & CG_LEFT) ? 0 : 16) + (g->cg_line & 15u)] = v;
}

uint32_t pc98gfx_ank_unicode(uint8_t c) {
    if (c == 0x5C) return 0x00A5;                   /* JIS X 0201: yen sign */
    if (c == 0x7E) return 0x203E;                   /* overline */
    if (c >= 0x20 && c < 0x7F) return c;
    if (c >= 0xA1 && c <= 0xDF) return 0xFF61u + (c - 0xA1u);   /* half-width katakana */
    return 0;
}

int pc98gfx_ank_from_unicode(uint8_t out[4096], const unsigned char *(*glyph)(unsigned cp)) {
    memset(out, 0, 4096);
    if (!glyph) return 0;
    const unsigned char *missing = glyph(0xFFFFu);  /* a noncharacter: whatever the font draws for "no glyph" */
    int n = 0;
    for (unsigned c = 0; c < 256; c++) {
        uint32_t cp = pc98gfx_ank_unicode((uint8_t)c);
        if (!cp) continue;
        const unsigned char *gl = glyph(cp);
        if (c == 0x7E && gl == missing) gl = glyph(0x00AF);     /* macron for the overline */
        if (!gl) continue;
        memcpy(out + c * 16, gl, 16);
        n++;
    }
    return n;
}

int pc98gfx_fontrom_valid(const uint8_t *rom, size_t size) {
    if (!rom || size != PC98GFX_FONTROM_SIZE) return 0;
    const uint8_t *a = rom + 0x800;
    uint8_t space = 0, letter = 0;
    for (int i = 0; i < 16; i++) {
        space |= a[0x20 * 16 + i];
        letter |= a[0x41 * 16 + i];
    }
    return !space && letter;                        /* a blank space and a non-blank 'A' in the 8x16 table */
}

const uint8_t *pc98gfx_fontrom_ank16(const uint8_t *rom) {
    return rom + 0x800;
}

int pc98gfx_fontrom_kanji(void *user, uint16_t jis, uint8_t glyph[32]) {
    const uint8_t *rom = (const uint8_t *)user;
    unsigned row = jis >> 8, cell = jis & 0xFFu;
    if (!rom || row < 0x21 || row > 0x7C || cell < 0x20 || cell > 0x7F) return 0;
    memcpy(glyph, rom + 0x1800 + ((row - 0x21) * 96u + (cell - 0x20)) * 32u, 32);
    return 1;
}

/* ------------------------------------------------------------------ I/O ports */

static void mode_ff2(Pc98Gfx *g, uint8_t v) {
    switch (v) {
    case 0x00: case 0x01: g->analog = v & 1; return;                    /* 8 / 16 colours */
    case 0x04: case 0x05: if (g->egc_permit) g->egc_mode = v & 1; return;   /* GRCG / EGC mode */
    case 0x06: case 0x07: g->egc_permit = v & 1; return;                /* mode change protect / permit */
    default:
        g->ff2_unknown++;
        g->ff2_last_unknown = v;
        return;
    }
}

static int port_write8(Pc98Gfx *g, uint16_t port, uint8_t v) {
    switch (port) {
    case 0x60: gdc_param(g, 0, v); return 1;
    case 0x62: gdc_command(g, 0, v); return 1;
    case 0xA0: gdc_param(g, 1, v); return 1;
    case 0xA2: gdc_command(g, 1, v); return 1;
    case 0x68: {                                    /* mode flip-flop 1: 0000nnnv */
        uint8_t bit = (uint8_t)(1u << ((v >> 1) & 7));
        if (v & 1) g->mode1 |= bit; else g->mode1 &= (uint8_t)~bit;
        return 1;
    }
    case 0x6A: mode_ff2(g, v); return 1;
    case 0x6C: g->border = v; return 1;
    case 0x70: case 0x72: case 0x74: case 0x76: case 0x78: case 0x7A:
        g->crtc[(port - 0x70) / 2] = v;
        return 1;
    case 0x7C:                                      /* GRCG mode; the tile pointer starts over at B */
        g->grcg.mode = v;
        g->grcg.tile_next = 0;
        return 1;
    case 0x7E:
        g->grcg.tile[g->grcg.tile_next] = v;
        g->grcg.tile_next = (uint8_t)((g->grcg.tile_next + 1) & 3);
        return 1;
    case 0xA1: g->cg_code_lo = v; return 1;
    case 0xA3: g->cg_code_hi = v; return 1;
    case 0xA5: g->cg_line = v; return 1;
    case 0xA9: cg_write(g, v); return 1;
    case 0xA4: g->disp_page = v & 1; return 1;
    case 0xA6: g->draw_page = v & 1; return 1;
    case 0xA8: if (g->analog) g->pal_index = v & 15; else g->pal_digital[0] = v; return 1;
    case 0xAA: if (g->analog) g->pal_analog[g->pal_index][0] = v & 15; else g->pal_digital[1] = v; return 1;
    case 0xAC: if (g->analog) g->pal_analog[g->pal_index][1] = v & 15; else g->pal_digital[2] = v; return 1;
    case 0xAE: if (g->analog) g->pal_analog[g->pal_index][2] = v & 15; else g->pal_digital[3] = v; return 1;
    default: break;
    }
    if (port >= 0x4A0 && port <= 0x4AF) {
        egc_port_write(g, port, v);
        return 1;
    }
    return 0;
}

static int port_read8(Pc98Gfx *g, uint16_t port, uint8_t *v) {
    switch (port) {
    case 0x60: *v = gdc_status(g, 0); return 1;
    case 0x62: *v = gdc_read_data(g, 0); return 1;
    case 0xA0: *v = gdc_status(g, 1); return 1;
    case 0xA2: *v = gdc_read_data(g, 1); return 1;
    case 0xA4: *v = g->disp_page; return 1;
    case 0xA6: *v = g->draw_page; return 1;
    case 0xA8: *v = g->analog ? g->pal_index : g->pal_digital[0]; return 1;
    case 0xAA: *v = g->analog ? g->pal_analog[g->pal_index][0] : g->pal_digital[1]; return 1;
    case 0xAC: *v = g->analog ? g->pal_analog[g->pal_index][1] : g->pal_digital[2]; return 1;
    case 0xAE: *v = g->analog ? g->pal_analog[g->pal_index][2] : g->pal_digital[3]; return 1;
    case 0xA9: *v = cg_read(g); return 1;
    default: return 0;
    }
}

int pc98gfx_port_write(Pc98Gfx *g, uint16_t port, uint32_t value, int width) {
    int mine = 0;
    if (width < 1) width = 1;
    for (int k = 0; k < width && k < 4; k++) mine |= port_write8(g, (uint16_t)(port + k), (uint8_t)(value >> (8 * k)));
    return mine;
}

int pc98gfx_port_read(Pc98Gfx *g, uint16_t port, uint32_t *value, int width) {
    int mine = 0;
    uint32_t r = 0;
    if (width < 1) width = 1;
    for (int k = 0; k < width && k < 4; k++) {
        uint8_t b = 0xFF;
        if (port_read8(g, (uint16_t)(port + k), &b)) mine = 1;
        r |= (uint32_t)b << (8 * k);
    }
    if (mine && value) *value = r;
    return mine;
}

/* ------------------------------------------------------------------ time */

void pc98gfx_vsync(Pc98Gfx *g) {
    g->frame++;
}

void pc98gfx_set_retrace(Pc98Gfx *g, int vsync, int hblank) {
    g->vsync = vsync != 0;
    g->hblank = hblank != 0;
}

int pc98gfx_blink_visible(const Pc98Gfx *g) {
    return !((g->frame >> 5) & 1);
}

int pc98gfx_cursor_visible(const Pc98Gfx *g) {
    const uint8_t *f = g->gdc[0].csrform;
    if (f[1] & 0x20) return 1;                      /* SC: steady cursor */
    unsigned br = ((f[1] >> 6) & 3u) | (unsigned)(f[2] & 7) << 2;
    if (!br) br = 32;
    return !((g->frame / br) & 1);
}

/* ------------------------------------------------------------------ output */

uint32_t pc98gfx_colour(const Pc98Gfx *g, unsigned idx) {
    if (g->analog) {
        const uint8_t *p = g->pal_analog[idx & 15];
        return (uint32_t)(p[1] & 15) * 17u << 16 | (uint32_t)(p[0] & 15) * 17u << 8 | (uint32_t)(p[2] & 15) * 17u;
    }
    idx &= 7;                                       /* colours 0-3: high nibbles of AEh ACh AAh A8h; 4-7: low nibbles */
    uint8_t v = g->pal_digital[3 - (idx & 3)];
    return GRB_RGB[idx < 4 ? (v >> 4) & 7 : v & 7];
}

typedef struct {
    uint32_t sad;
    unsigned len;
} Partition;

static void partitions(const Pc98GfxGdc *d, int n, Partition *out) {
    for (int i = 0; i < n; i++) {
        const uint8_t *p = d->pram + 4 * i;
        out[i].sad = p[0] | (uint32_t)p[1] << 8 | (uint32_t)(p[2] & 3) << 16;
        out[i].len = (unsigned)(p[2] >> 4) | (unsigned)(p[3] & 0x3F) << 4;
    }
}

/* Which partition shows scan line y, and the line within it; -1 when none. A LEN of 0 reaches to the bottom. */
static int partition_of(const Partition *pt, int n, int y, int *ly) {
    int start = 0;
    for (int i = 0; i < n; i++) {
        if (!pt[i].len || y < start + (int)pt[i].len) {
            *ly = y - start;
            return i;
        }
        start += (int)pt[i].len;
    }
    return -1;
}

static void render_graphics(const Pc98Gfx *g, uint32_t *rgb, int pitch) {
    const Pc98GfxGdc *d = &g->gdc[1];
    uint32_t pal[16];
    for (unsigned i = 0; i < 16; i++) pal[i] = pc98gfx_colour(g, i);
    Partition pt[2];
    partitions(d, 2, pt);
    unsigned per_line = (d->csrform[0] & 0x1Fu) + 1u;  /* 200-line mode: 2 rasters per line of memory */
    const uint8_t *B = g->gvram[g->disp_page & 1][0], *R = g->gvram[g->disp_page & 1][1];
    const uint8_t *G = g->gvram[g->disp_page & 1][2], *E = g->gvram[g->disp_page & 1][3];
    for (int y = 0; y < PC98GFX_HEIGHT; y++) {
        uint32_t *row = rgb + (size_t)y * (size_t)pitch;
        int ly = 0, p = d->display_on ? partition_of(pt, 2, y, &ly) : -1;
        if (p < 0 || (per_line > 1 && (g->mode1 & PC98GFX_FF1_HIDE_ODD) && ((unsigned)ly % per_line))) {
            for (int x = 0; x < PC98GFX_WIDTH; x++) row[x] = 0;
            continue;
        }
        uint32_t wa = pt[p].sad + ((unsigned)ly / per_line) * d->pitch;
        for (unsigned w = 0; w < 40; w++) {
            unsigned o = ((wa + w) & 0x3FFFu) * 2;
            unsigned b = (unsigned)B[o] << 8 | B[o + 1], r = (unsigned)R[o] << 8 | R[o + 1];
            unsigned gg = (unsigned)G[o] << 8 | G[o + 1], e = (unsigned)E[o] << 8 | E[o + 1];
            uint32_t *px = row + w * 16;
            for (int k = 15; k >= 0; k--, px++)
                *px = pal[((b >> k) & 1) | ((r >> k) & 1) << 1 | ((gg >> k) & 1) << 2 | ((e >> k) & 1) << 3];
        }
    }
}

/* Simple graphics: the code's bits light a 2 x 4 grid of blocks (bits 0-3 the left column top to bottom, bits 4-7 the
 * right column). */
static uint8_t semigraphic(uint8_t code, unsigned line, unsigned height) {
    unsigned band = line * 4 / height;
    return (uint8_t)(((code >> band) & 1 ? 0xF0 : 0) | ((code >> (4 + band)) & 1 ? 0x0F : 0));
}

static void render_text(const Pc98Gfx *g, uint32_t *rgb, int pitch, int blink_vis, int cursor_vis) {
    const Pc98GfxGdc *d = &g->gdc[0];
    if (!d->display_on) return;
    Partition pt[4];
    partitions(d, 4, pt);
    unsigned h = (d->csrform[0] & 0x1Fu) + 1u;
    int wide = (g->mode1 & PC98GFX_FF1_40COL) != 0, cols = wide ? 40 : 80;
    int semi = (g->mode1 & PC98GFX_FF1_SEMIGRAPHICS) != 0;
    int cur_on = (d->csrform[0] & 0x80) && cursor_vis;
    unsigned ctop = d->csrform[1] & 0x1Fu, cbot = (unsigned)d->csrform[2] >> 3;
    uint32_t cur_addr = d->ead & 0xFFFu;
    /* One character row's cells, fetched once per row. */
    uint8_t glyph[80][16], attr[80], code[80], cursor[80];
    int row_part = -1;
    unsigned row_no = 0;
    for (int y = 0; y < PC98GFX_HEIGHT; y++) {
        int ly = 0, p = partition_of(pt, 4, y, &ly);
        if (p < 0) continue;
        unsigned crow = (unsigned)ly / h, cl = (unsigned)ly % h;
        if (p != row_part || crow != row_no) {
            row_part = p;
            row_no = crow;
            for (int c = 0; c < cols; c++) {
                uint32_t a = (pt[p].sad + crow * d->pitch + (unsigned)c) & 0xFFFu;
                uint8_t lo = g->tvram[2 * a], hi = g->tvram[2 * a + 1];
                attr[c] = g->tvram[0x2000 + 2 * a];
                code[c] = lo;
                cursor[c] = a == cur_addr;
                if (!hi) {
                    if (g->ank) memcpy(glyph[c], g->ank + (unsigned)lo * 16, 16);
                    else memset(glyph[c], 0, 16);
                } else {                            /* two-byte code: bit 7 of the low byte marks the right half (NEC's DOS writes
                                                     * 23h 66h, A3h 66h), and so does bit 7 of the high byte (TH02 to TH05 write their
                                                     * user-defined characters as 56h c, 56h c|80h: with only the low byte's bit they
                                                     * drew every character as two left halves) */
                    uint8_t full[32];
                    kanji_glyph(g, lo, hi, full);
                    memcpy(glyph[c], full + (((lo | hi) & 0x80) ? 16 : 0), 16);
                }
            }
        }
        uint32_t *line = rgb + (size_t)y * (size_t)pitch;
        for (int c = 0; c < cols; c++) {
            uint8_t a = attr[c], bits;
            if (semi && (a & PC98GFX_ATTR_VLINE)) bits = semigraphic(code[c], cl, h);
            else bits = cl < 16 ? glyph[c][cl] : 0;
            if (!(a & PC98GFX_ATTR_SHOW)) bits = 0;
            if ((a & PC98GFX_ATTR_BLINK) && !blink_vis) bits = 0;
            if ((a & PC98GFX_ATTR_UNDERLINE) && cl == h - 1) bits = 0xFF;
            if ((a & PC98GFX_ATTR_VLINE) && !semi) bits |= 0x80;
            if (a & PC98GFX_ATTR_REVERSE) bits = (uint8_t)~bits;
            if (cur_on && cursor[c] && cl >= ctop && cl <= cbot) bits = (uint8_t)~bits;
            if (!bits) continue;
            uint32_t colr = GRB_RGB[(a >> 5) & 7];
            if (wide) {
                uint32_t *px = line + c * 16;
                for (int k = 0; k < 8; k++) if (bits & (0x80 >> k)) px[2 * k] = px[2 * k + 1] = colr;
            } else {
                uint32_t *px = line + c * 8;
                for (int k = 0; k < 8; k++) if (bits & (0x80 >> k)) px[k] = colr;
            }
        }
    }
}

void pc98gfx_render_phase(const Pc98Gfx *g, uint32_t *rgb, int pitch, int blink_vis, int cursor_vis) {
    if (!(g->mode1 & PC98GFX_FF1_DISPLAY)) {
        for (int y = 0; y < PC98GFX_HEIGHT; y++)
            for (int x = 0; x < PC98GFX_WIDTH; x++) rgb[(size_t)y * (size_t)pitch + (size_t)x] = 0;
        return;
    }
    render_graphics(g, rgb, pitch);
    render_text(g, rgb, pitch, blink_vis, cursor_vis);
}

void pc98gfx_render(const Pc98Gfx *g, uint32_t *rgb, int pitch) {
    pc98gfx_render_phase(g, rgb, pitch, pc98gfx_blink_visible(g), pc98gfx_cursor_visible(g));
}

void pc98gfx_text_dump(const Pc98Gfx *g, char *buf) {
    const Pc98GfxGdc *d = &g->gdc[0];
    Partition pt[1];
    partitions(d, 1, pt);
    char *o = buf;
    for (unsigned y = 0; y < PC98GFX_TEXT_ROWS; y++) {
        for (unsigned x = 0; x < PC98GFX_TEXT_COLS; x++) {
            uint32_t a = (pt[0].sad + y * d->pitch + x) & 0xFFFu;
            uint8_t lo = g->tvram[2 * a], hi = g->tvram[2 * a + 1], at = g->tvram[0x2000 + 2 * a];
            char ch;
            if (!(at & PC98GFX_ATTR_SHOW)) ch = ' ';
            else if (hi) ch = '?';
            else if (lo < 0x20) ch = ' ';
            else if (lo < 0x7F) ch = (char)lo;
            else ch = '?';
            *o++ = ch;
        }
        *o++ = '\n';
    }
    *o = 0;
}

static char *put_uint(char *p, unsigned v) {
    char tmp[12];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = tmp[--n];
    return p;
}

size_t pc98gfx_ppm(const uint32_t *rgb, int width, int height, int pitch, uint8_t *out, size_t cap) {
    if (width <= 0 || height <= 0) return 0;
    char head[40], *p = head;
    memcpy(p, "P6\n", 3);
    p = put_uint(p + 3, (unsigned)width);
    *p++ = ' ';
    p = put_uint(p, (unsigned)height);
    memcpy(p, "\n255\n", 5);
    p += 5;
    size_t hl = (size_t)(p - head), need = hl + (size_t)width * (size_t)height * 3;
    if (!out || cap < need) return need;
    memcpy(out, head, hl);
    uint8_t *q = out + hl;
    for (int y = 0; y < height; y++) {
        const uint32_t *row = rgb + (size_t)y * (size_t)pitch;
        for (int x = 0; x < width; x++) {
            *q++ = (uint8_t)(row[x] >> 16);
            *q++ = (uint8_t)(row[x] >> 8);
            *q++ = (uint8_t)row[x];
        }
    }
    return need;
}
