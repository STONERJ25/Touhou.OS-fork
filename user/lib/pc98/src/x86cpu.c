/*
 * x86emu: 8086 / V30 / 80286 real-mode interpreter. See include/x86emu.h for the interface and docs/PC98.md for the list
 * of quirks and what is left out.
 *
 * Structure: x86_step() takes a pending interrupt or decodes prefixes, then one opcode in a single switch. FLAGS are
 * computed eagerly (no lazy flags): it keeps the code obvious and the flags exact, and the interpreter is still far
 * faster than the 8 MHz machine it imitates. Every memory access goes through the bus callbacks one byte at a time, so a
 * word that straddles the end of a segment wraps to offset 0 of the same segment, as on the 8086.
 *
 * Cycle counts are the 8086 data sheet's (Intel 8086 family user's manual, instruction timing table) for the common
 * case, plus the effective-address cost; where the data sheet gives a range the middle is used.
 */
#include <x86emu.h>
#include <string.h>

typedef X86Cpu C;

#define CF X86_CF
#define PF X86_PF
#define AF X86_AF
#define ZF X86_ZF
#define SF X86_SF
#define TF X86_TF
#define IF X86_IF
#define DF X86_DF
#define OF X86_OF

#define AX c->r[X86_AX]
#define CX c->r[X86_CX]
#define DX c->r[X86_DX]
#define BX c->r[X86_BX]
#define SP c->r[X86_SP]
#define BP c->r[X86_BP]
#define SI c->r[X86_SI]
#define DI c->r[X86_DI]

#define IS8086(c) ((c)->model == X86_MODEL_8086)
#define IS286(c) ((c)->model == X86_MODEL_80286)
#define HAS186(c) ((c)->model != X86_MODEL_8086)

/* ------------------------------------------------------------------ bus access */

static inline uint8_t mrd(C *c, uint32_t a) { return c->bus.read8(c->bus.ctx, a & c->addr_mask); }
static inline void mwr(C *c, uint32_t a, uint8_t v) { c->bus.write8(c->bus.ctx, a & c->addr_mask, v); }
static inline uint32_t lin(C *c, int seg, uint16_t off) { return ((uint32_t)c->s[seg] << 4) + off; }
static inline uint8_t rd8(C *c, int seg, uint16_t off) { return mrd(c, lin(c, seg, off)); }
static inline uint16_t rd16(C *c, int seg, uint16_t off) {
    uint16_t lo = rd8(c, seg, off);
    return lo | (uint16_t)rd8(c, seg, (uint16_t)(off + 1)) << 8;
}
static inline void wr8(C *c, int seg, uint16_t off, uint8_t v) { mwr(c, lin(c, seg, off), v); }
static inline void wr16(C *c, int seg, uint16_t off, uint16_t v) {
    wr8(c, seg, off, (uint8_t)v);
    wr8(c, seg, (uint16_t)(off + 1), (uint8_t)(v >> 8));
}
static inline uint16_t rdw(C *c, int seg, uint16_t off, int w) { return w ? rd16(c, seg, off) : rd8(c, seg, off); }
static inline void wrw(C *c, int seg, uint16_t off, uint16_t v, int w) {
    if (w) wr16(c, seg, off, v); else wr8(c, seg, off, (uint8_t)v);
}
static inline uint8_t fetch8(C *c) { return rd8(c, X86_CS, c->ip++); }
static inline uint16_t fetch16(C *c) {
    uint16_t lo = fetch8(c);
    return lo | (uint16_t)fetch8(c) << 8;
}

static uint8_t io_in8(C *c, uint16_t port) { return c->bus.in8 ? c->bus.in8(c->bus.ctx, port) : 0xFF; }
static void io_out8(C *c, uint16_t port, uint8_t v) { if (c->bus.out8) c->bus.out8(c->bus.ctx, port, v); }
static uint16_t io_in(C *c, uint16_t port, int w) {
    if (!w) return io_in8(c, port);
    if (c->bus.in16) return c->bus.in16(c->bus.ctx, port);
    uint16_t lo = io_in8(c, port);
    return lo | (uint16_t)io_in8(c, (uint16_t)(port + 1)) << 8;
}
static void io_out(C *c, uint16_t port, uint16_t v, int w) {
    if (!w) { io_out8(c, port, (uint8_t)v); return; }
    if (c->bus.out16) { c->bus.out16(c->bus.ctx, port, v); return; }
    io_out8(c, port, (uint8_t)v);
    io_out8(c, (uint16_t)(port + 1), (uint8_t)(v >> 8));
}

/* ------------------------------------------------------------------ registers and flags */

static inline uint8_t getr8(C *c, int i) { return i < 4 ? (uint8_t)c->r[i] : (uint8_t)(c->r[i - 4] >> 8); }
static inline void setr8(C *c, int i, uint8_t v) {
    if (i < 4) c->r[i] = (uint16_t)((c->r[i] & 0xFF00) | v);
    else c->r[i - 4] = (uint16_t)((c->r[i - 4] & 0x00FF) | (uint16_t)v << 8);
}
static inline uint16_t getr(C *c, int i, int w) { return w ? c->r[i] : getr8(c, i); }
static inline void setr(C *c, int i, uint16_t v, int w) { if (w) c->r[i] = v; else setr8(c, i, (uint8_t)v); }

/* The bits FLAGS can hold: 8086 and V30 read bits 12-15 as 1, the 286 in real mode as 0; bit 1 is always 1. */
static inline uint16_t fix_flags(C *c, uint16_t f) {
    f = (uint16_t)((f & 0x0FD5) | 0x0002);
    return IS286(c) ? f : (uint16_t)(f | 0xF000);
}
static inline void setf(C *c, uint16_t f, int on) { if (on) c->flags |= f; else c->flags &= (uint16_t)~f; }
static inline int even_parity(uint8_t v) { v ^= v >> 4; return !((0x6996 >> (v & 15)) & 1); }
static inline void szp(C *c, uint16_t v, int w) {
    setf(c, ZF, (w ? v : (v & 0xFF)) == 0);
    setf(c, SF, v & (w ? 0x8000 : 0x80));
    setf(c, PF, even_parity((uint8_t)v));
}

static inline void push(C *c, uint16_t v) { SP -= 2; wr16(c, X86_SS, SP, v); }
static inline uint16_t pop(C *c) { uint16_t v = rd16(c, X86_SS, SP); SP += 2; return v; }

/* ------------------------------------------------------------------ ModR/M */

/* Effective-address cost (8086 data sheet): base or index 5, BX+SI and BP+DI 7, BX+DI and BP+SI 8, a displacement +4,
 * a direct address 6, a segment override +2. */
static const uint8_t ea_clk[8] = { 7, 8, 8, 7, 5, 5, 5, 5 };

static void modrm(C *c) {
    uint8_t b = fetch8(c);
    c->mod = b >> 6;
    c->reg = (b >> 3) & 7;
    c->rm = b & 7;
    if (c->mod == 3) return;
    uint16_t ea = 0;
    int seg = X86_DS, clk;
    switch (c->rm) {
    case 0: ea = BX + SI; break;
    case 1: ea = BX + DI; break;
    case 2: ea = BP + SI; seg = X86_SS; break;
    case 3: ea = BP + DI; seg = X86_SS; break;
    case 4: ea = SI; break;
    case 5: ea = DI; break;
    case 6: ea = BP; seg = X86_SS; break;
    case 7: ea = BX; break;
    }
    if (c->mod == 0 && c->rm == 6) {
        ea = fetch16(c);
        seg = X86_DS;
        clk = 6;
    } else {
        clk = ea_clk[c->rm];
        if (c->mod == 1) { ea += (uint16_t)(int8_t)fetch8(c); clk += 4; }
        else if (c->mod == 2) { ea += fetch16(c); clk += 4; }
    }
    if (c->seg_ovr >= 0) { seg = c->seg_ovr; clk += 2; }
    c->ea = ea;
    c->ea_seg = (uint8_t)seg;
    c->cycles += (uint64_t)clk;
}

static inline uint16_t get_rm(C *c, int w) { return c->mod == 3 ? getr(c, c->rm, w) : rdw(c, c->ea_seg, c->ea, w); }
static inline void set_rm(C *c, uint16_t v, int w) {
    if (c->mod == 3) setr(c, c->rm, v, w); else wrw(c, c->ea_seg, c->ea, v, w);
}
static inline int dseg(C *c) { return c->seg_ovr >= 0 ? c->seg_ovr : X86_DS; }
#define CLK(r, m) (c->cycles += (uint64_t)(c->mod == 3 ? (r) : (m)))

/* ------------------------------------------------------------------ interrupts */

static void int_entry(C *c, uint8_t vec, uint16_t ret_ip) {
    push(c, c->flags);
    c->flags &= (uint16_t)~(IF | TF);
    push(c, c->s[X86_CS]);
    push(c, ret_ip);
    uint32_t a = (uint32_t)vec * 4;
    c->ip = (uint16_t)(mrd(c, a) | mrd(c, a + 1) << 8);
    c->s[X86_CS] = (uint16_t)(mrd(c, a + 2) | mrd(c, a + 3) << 8);
    c->halted = 0;
    c->in_rep = 0;
    c->cycles += 51;
}

/* INT n, INT3, INTO: the host may handle the call itself (HLE BIOS). */
static void soft_int(C *c, uint8_t vec) {
    if (c->bus.soft_int && c->bus.soft_int(c->bus.ctx, c, vec)) return;
    int_entry(c, vec, c->ip);
}

/* Divide error: the 8086 (and V30) push the address of the next instruction, the 286 that of the divide itself. */
static void divide_error(C *c) { int_entry(c, 0, IS286(c) ? c->op_ip : c->ip); }

/* 286 invalid opcode (INT 6, a fault: IP points at the instruction). */
static void invalid_opcode(C *c, uint8_t op) {
    if (IS286(c)) { int_entry(c, 6, c->op_ip); return; }
    c->stop = X86_STOP_UNIMPLEMENTED;
    c->bad_opcode = op;
    c->ip = c->op_ip;
}

/* ------------------------------------------------------------------ arithmetic */

/* The eight classic ALU operations: 0 ADD, 1 OR, 2 ADC, 3 SBB, 4 AND, 5 SUB, 6 XOR, 7 CMP. */
static uint16_t alu(C *c, int op, uint16_t a, uint16_t b, int w) {
    uint32_t mask = w ? 0xFFFF : 0xFF, sign = w ? 0x8000 : 0x80, res, ci;
    switch (op) {
    case 0: case 2:
        ci = (op == 2) ? (c->flags & CF) : 0;
        res = (uint32_t)a + b + ci;
        setf(c, CF, res > mask);
        setf(c, OF, (a ^ res) & (b ^ res) & sign);
        setf(c, AF, (a ^ b ^ res) & 0x10);
        break;
    case 3: case 5: case 7:
        ci = (op == 3) ? (c->flags & CF) : 0;
        res = (uint32_t)a - b - ci;
        setf(c, CF, (uint32_t)b + ci > a);
        setf(c, OF, (a ^ b) & (a ^ res) & sign);
        setf(c, AF, (a ^ b ^ res) & 0x10);
        break;
    default:
        res = op == 1 ? (uint32_t)(a | b) : op == 4 ? (uint32_t)(a & b) : (uint32_t)(a ^ b);
        c->flags &= (uint16_t)~(CF | OF | AF);      /* AF is undefined after logic ops; the 8086 leaves it clear */
        break;
    }
    res &= mask;
    szp(c, (uint16_t)res, w);
    return (uint16_t)res;
}

static uint16_t incdec(C *c, uint16_t v, int dec, int w) {
    uint16_t mask = w ? 0xFFFF : 0xFF, sign = w ? 0x8000 : 0x80;
    uint16_t r = (uint16_t)((dec ? v - 1 : v + 1) & mask);
    setf(c, OF, dec ? v == sign : r == sign);
    setf(c, AF, dec ? (r & 0xF) == 0xF : (r & 0xF) == 0);
    szp(c, r, w);
    return r;
}

/* Group 2: 0 ROL, 1 ROR, 2 RCL, 3 RCR, 4 SHL, 5 SHR, 6 SETMO (8086) / SHL (186+), 7 SAR. The 8086 takes the count as it
 * is (up to 255 single-bit steps), the 186-class cores mask it to 5 bits. A zero count changes nothing, flags included.
 * OF after a multi-bit shift is what the last single-bit step leaves: the top bit changed or not. */
static uint16_t shift(C *c, int op, uint16_t v, unsigned cnt, int w) {
    uint32_t mask = w ? 0xFFFF : 0xFF, msb = w ? 0x8000 : 0x80;
    if (HAS186(c)) cnt &= 31;
    if (cnt == 0) return v;
    c->cycles += 4u * cnt;
    if (op == 6 && IS8086(c)) {
        /* SETMO / SETMOC (undocumented): the operand becomes all ones; the 8086's flags here are undefined. */
        c->flags &= (uint16_t)~(CF | OF | AF);
        szp(c, (uint16_t)mask, w);
        return (uint16_t)mask;
    }
    uint32_t r = v, prev = v;
    int cf = (c->flags & CF) != 0;
    for (unsigned i = 0; i < cnt; i++) {
        prev = r;
        switch (op) {
        case 0: cf = (r & msb) != 0; r = ((r << 1) | (uint32_t)cf) & mask; break;
        case 1: cf = r & 1; r = (r >> 1) | (cf ? msb : 0); break;
        case 2: { int n = (r & msb) != 0; r = ((r << 1) | (uint32_t)cf) & mask; cf = n; break; }
        case 3: { int n = r & 1; r = (r >> 1) | (cf ? msb : 0); cf = n; break; }
        case 4: case 6: cf = (r & msb) != 0; r = (r << 1) & mask; break;
        case 5: cf = r & 1; r >>= 1; break;
        case 7: cf = r & 1; r = (r >> 1) | (r & msb); break;
        }
    }
    setf(c, CF, cf);
    setf(c, OF, ((r ^ prev) & msb) != 0);
    if (op >= 4) {
        szp(c, (uint16_t)r, w);
        c->flags &= (uint16_t)~AF;                  /* undefined; left clear */
    }
    return (uint16_t)r;
}

/* The 8086 divides by shifting and subtracting; its first step compares the high half of the (absolute) dividend with
 * the (absolute) divisor and gives up with a divide error when the quotient cannot fit. The flags it leaves (pushed by
 * the interrupt) are those of that subtraction, as the SingleStepTests 8088 vectors show. A signed quotient that turns
 * out too large only at the end leaves the flags of later steps, which are not modelled (they stay as they were). */
static void div_overflow(C *c, uint16_t hi, uint16_t divisor, int w) {
    alu(c, 5, hi, divisor, w);
    divide_error(c);
}

/* Group 3 multiply and divide. Returns 0, or 1 when a divide error was raised. */
static int muldiv(C *c, int op, uint16_t src, int w) {
    if (!w) {
        uint8_t al = (uint8_t)AX;
        switch (op) {
        case 4: {
            uint16_t r = (uint16_t)(al * (uint8_t)src);
            AX = r;
            setf(c, CF | OF, (r & 0xFF00) != 0);
            szp(c, r, 0);
            c->cycles += 74;
            return 0;
        }
        case 5: {
            int16_t r = (int16_t)((int8_t)al * (int8_t)src);
            AX = (uint16_t)r;
            setf(c, CF | OF, r != (int8_t)r);
            szp(c, (uint16_t)r, 0);
            c->cycles += 89;
            return 0;
        }
        case 6: {
            c->cycles += 85;
            if ((AX >> 8) >= (uint8_t)src) { div_overflow(c, AX >> 8, (uint8_t)src, 0); return 1; }
            uint16_t q = AX / (uint8_t)src, rem = AX % (uint8_t)src;
            AX = (uint16_t)(rem << 8 | q);
            return 0;
        }
        default: {
            c->cycles += 107;
            int d = (int8_t)src;
            int n = (int16_t)AX;
            unsigned an = (unsigned)(n < 0 ? -n : n), ad = (unsigned)(d < 0 ? -d : d);
            if ((an >> 8) >= ad) { div_overflow(c, (uint16_t)(an >> 8), (uint16_t)ad, 0); return 1; }
            int q = n / d, rem = n % d;
            if (q > 127 || q < (IS286(c) ? -128 : -127)) { divide_error(c); return 1; }
            if (c->rep && IS8086(c)) q = -q;        /* 8086: a REP prefix negates the quotient */
            AX = (uint16_t)((uint8_t)rem << 8 | (uint8_t)q);
            return 0;
        }
        }
    }
    switch (op) {
    case 4: {
        uint32_t r = (uint32_t)AX * src;
        AX = (uint16_t)r;
        DX = (uint16_t)(r >> 16);
        setf(c, CF | OF, DX != 0);
        szp(c, AX, 1);
        c->cycles += 125;
        return 0;
    }
    case 5: {
        int32_t r = (int32_t)(int16_t)AX * (int16_t)src;
        AX = (uint16_t)r;
        DX = (uint16_t)((uint32_t)r >> 16);
        setf(c, CF | OF, r != (int16_t)r);
        szp(c, AX, 1);
        c->cycles += 141;
        return 0;
    }
    case 6: {
        c->cycles += 153;
        if (DX >= src) { div_overflow(c, DX, src, 1); return 1; }
        uint32_t n = (uint32_t)DX << 16 | AX, q = n / src, rem = n % src;
        AX = (uint16_t)q;
        DX = (uint16_t)rem;
        return 0;
    }
    default: {
        c->cycles += 175;
        int64_t d = (int16_t)src;
        int64_t n = (int32_t)((uint32_t)DX << 16 | AX);
        uint64_t an = (uint64_t)(n < 0 ? -n : n), ad = (uint64_t)(d < 0 ? -d : d);
        if ((an >> 16) >= ad) { div_overflow(c, (uint16_t)(an >> 16), (uint16_t)ad, 1); return 1; }
        int64_t q = n / d, rem = n % d;
        if (q > 32767 || q < (IS286(c) ? -32768 : -32767)) { divide_error(c); return 1; }
        if (c->rep && IS8086(c)) q = -q;
        AX = (uint16_t)q;
        DX = (uint16_t)rem;
        return 0;
    }
    }
}

static void daa_das(C *c, int sub) {
    uint8_t al = (uint8_t)AX, old = al;
    int cf = (c->flags & CF) != 0, af = (c->flags & AF) != 0, af_in = af;
    if ((al & 0x0F) > 9 || af) {
        al = (uint8_t)(sub ? al - 6 : al + 6);
        af = 1;
    }
    /* The 8086 compares the original AL with 9F instead of 99 when AF came in set (SingleStepTests 8088 vectors). */
    if (old > (af_in ? 0x9F : 0x99) || cf) {
        al = (uint8_t)(sub ? al - 0x60 : al + 0x60);
        cf = 1;
    }
    setr8(c, 0, al);
    setf(c, CF, cf);
    setf(c, AF, af);
    szp(c, al, 0);
    /* OF is undefined: the 8086 reports a signed overflow of the adjustment. */
    setf(c, OF, sub ? ((old & 0x80) && !(al & 0x80)) : (!(old & 0x80) && (al & 0x80)));
    c->cycles += 4;
}

static void aaa_aas(C *c, int sub) {
    uint8_t al = (uint8_t)AX;
    if ((al & 0x0F) > 9 || (c->flags & AF)) {
        if (IS286(c)) {
            AX = (uint16_t)(sub ? AX - 6 : AX + 6);
            AX = (uint16_t)(sub ? AX - 0x100 : AX + 0x100);
        } else {
            setr8(c, 0, (uint8_t)(sub ? al - 6 : al + 6));
            setr8(c, 4, (uint8_t)(sub ? (AX >> 8) - 1 : (AX >> 8) + 1));
        }
        c->flags |= AF | CF;
    } else {
        c->flags &= (uint16_t)~(AF | CF);
    }
    szp(c, (uint8_t)AX, 0);                         /* undefined; the result before masking */
    setr8(c, 0, (uint8_t)(AX & 0x0F));
    c->cycles += 4;
}

/* ------------------------------------------------------------------ string instructions */

static int is_string(uint8_t op) {
    return (op >= 0xA4 && op <= 0xA7) || (op >= 0xAA && op <= 0xAF) || (op >= 0x6C && op <= 0x6F);
}

static void string_op(C *c, uint8_t op) {
    int w = op & 1;
    uint16_t d = (uint16_t)((c->flags & DF) ? -(1 + w) : (1 + w));
    int src = dseg(c);
    if (c->rep) {
        if (CX == 0) { c->in_rep = 0; c->cycles += 9; return; }
        if (!c->in_rep) c->cycles += 9;
    }
    int cmp = 0;
    switch (op) {
    case 0xA4: case 0xA5:
        wrw(c, X86_ES, DI, rdw(c, src, SI, w), w);
        SI += d; DI += d;
        c->cycles += 17;
        break;
    case 0xA6: case 0xA7: {
        uint16_t a = rdw(c, src, SI, w), b = rdw(c, X86_ES, DI, w);
        alu(c, 7, a, b, w);
        SI += d; DI += d;
        cmp = 1;
        c->cycles += 22;
        break;
    }
    case 0xAA: case 0xAB:
        wrw(c, X86_ES, DI, getr(c, X86_AX, w), w);
        DI += d;
        c->cycles += 10;
        break;
    case 0xAC: case 0xAD:
        setr(c, X86_AX, rdw(c, src, SI, w), w);
        SI += d;
        c->cycles += 13;
        break;
    case 0xAE: case 0xAF:
        alu(c, 7, getr(c, X86_AX, w), rdw(c, X86_ES, DI, w), w);
        DI += d;
        cmp = 1;
        c->cycles += 15;
        break;
    case 0x6C: case 0x6D:
        wrw(c, X86_ES, DI, io_in(c, DX, w), w);
        DI += d;
        c->cycles += 14;
        break;
    case 0x6E: case 0x6F:
        io_out(c, DX, rdw(c, src, SI, w), w);
        SI += d;
        c->cycles += 14;
        break;
    }
    if (!c->rep) return;
    CX--;
    int done = CX == 0;
    if (cmp) {
        switch (c->rep) {
        case 0xF3: done |= !(c->flags & ZF); break;    /* REPE */
        case 0xF2: done |= (c->flags & ZF) != 0; break; /* REPNE */
        case 0x65: done |= !(c->flags & CF); break;    /* V30 REPC */
        case 0x64: done |= (c->flags & CF) != 0; break; /* V30 REPNC */
        }
    }
    if (done) {
        c->in_rep = 0;
    } else {
        c->in_rep = 1;
        c->ip = c->op_ip;
    }
}

/* ------------------------------------------------------------------ conditions */

static int cond(C *c, int cc) {
    uint16_t f = c->flags;
    int r;
    switch (cc >> 1) {
    case 0: r = (f & OF) != 0; break;
    case 1: r = (f & CF) != 0; break;
    case 2: r = (f & ZF) != 0; break;
    case 3: r = (f & (CF | ZF)) != 0; break;
    case 4: r = (f & SF) != 0; break;
    case 5: r = (f & PF) != 0; break;
    case 6: r = !(f & SF) != !(f & OF); break;
    default: r = ((f & ZF) != 0) || (!(f & SF) != !(f & OF)); break;
    }
    return (cc & 1) ? !r : r;
}

/* ------------------------------------------------------------------ the NEC V30's 0F page and the 286's 0F page */

static void op_0f_v30(C *c) {
    uint8_t op = fetch8(c);
    if (op >= 0x10 && op <= 0x1F) {
        /* TEST1/CLR1/SET1/NOT1 r/m, CL (10-17) or r/m, imm8 (18-1F); even opcodes are byte operands. */
        int w = op & 1, kind = (op >> 1) & 3;
        modrm(c);
        unsigned bit = (op >= 0x18) ? fetch8(c) : (uint8_t)CX;
        bit &= w ? 15 : 7;
        uint16_t v = get_rm(c, w), m = (uint16_t)(1u << bit);
        switch (kind) {
        case 0: setf(c, ZF, !(v & m)); c->flags &= (uint16_t)~(CF | OF); break;
        case 1: set_rm(c, v & (uint16_t)~m, w); break;
        case 2: set_rm(c, v | m, w); break;
        case 3: set_rm(c, v ^ m, w); break;
        }
        CLK(4, 13);
        return;
    }
    if (op == 0x28 || op == 0x2A) {
        /* ROL4 / ROR4 r/m8: rotate the nibbles of AL's low half and the operand. */
        modrm(c);
        uint8_t v = (uint8_t)get_rm(c, 0), al = (uint8_t)AX;
        if (op == 0x28) {
            set_rm(c, (uint8_t)(v << 4 | (al & 0x0F)), 0);
            setr8(c, 0, (uint8_t)((al & 0xF0) | v >> 4));
        } else {
            set_rm(c, (uint8_t)((al & 0x0F) << 4 | v >> 4), 0);
            setr8(c, 0, (uint8_t)((al & 0xF0) | (v & 0x0F)));
        }
        CLK(25, 28);
        return;
    }
    c->stop = X86_STOP_UNIMPLEMENTED;      /* ADD4S, SUB4S, CMP4S, INS, EXT, BRKEM, ... */
    c->bad_opcode = 0x0F;
    c->ip = c->op_ip;
}

static void op_0f_286(C *c) {
    uint8_t op = fetch8(c);
    if (op == 0x01) {
        modrm(c);
        if (c->reg == 4) {                          /* SMSW */
            set_rm(c, (uint16_t)(c->msw | 0xFFF0), 1);
            CLK(2, 3);
            return;
        }
        if (c->reg == 6) {                          /* LMSW: the PE bit cannot be set here */
            uint16_t v = get_rm(c, 1);
            c->msw = (uint16_t)((c->msw & 1) | (v & 0x000F));
            if (v & 1) {
                c->stop = X86_STOP_PROTECTED;
                c->msw &= (uint16_t)~1u;
            }
            CLK(3, 6);
            return;
        }
    }
    /* Everything else on this page needs protected mode (or is LOADALL): invalid in real mode. */
    invalid_opcode(c, 0x0F);
}

/* ------------------------------------------------------------------ one instruction */

static void execute(C *c) {
    uint8_t op;
    int prefixes = 0;
    c->seg_ovr = -1;
    c->rep = 0;
    c->op_ip = c->ip;
    for (;;) {
        op = fetch8(c);
        if (op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E) c->seg_ovr = (int8_t)((op >> 3) & 3);
        else if (op == 0xF2 || op == 0xF3) c->rep = op;
        else if (op == 0xF0 || (op == 0xF1 && !IS286(c))) { /* LOCK (and its 8086 alias F1): no effect here */ }
        else if ((op == 0x64 || op == 0x65) && c->model == X86_MODEL_V30) c->rep = op;
        else break;
        if (!c->in_rep) c->cycles += 2;         /* a REP iteration does not pay for its prefixes again */
        if (++prefixes >= 0x10000) { c->stop = X86_STOP_PREFIXES; c->ip = c->op_ip; return; }
    }
    if (c->rep && !is_string(op) && !(op >= 0xF6 && op <= 0xF7)) c->rep = 0;
    if (IS8086(c)) {
        /* Opcodes the 8086 decodes as aliases of their neighbours (the 186 gave them new meanings). */
        if (op >= 0x60 && op <= 0x6F) op += 0x10;
        else if (op == 0xC0 || op == 0xC1) op += 2;
        else if (op == 0xC8 || op == 0xC9) op += 2;
    }

    uint16_t v, t;
    int w = op & 1;
    switch (op) {
    /* ---- ALU: ADD OR ADC SBB AND SUB XOR CMP in six forms each */
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x18: case 0x19: case 0x1A: case 0x1B:
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x28: case 0x29: case 0x2A: case 0x2B:
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x38: case 0x39: case 0x3A: case 0x3B: {
        int aop = op >> 3;
        modrm(c);
        if (op & 2) {                               /* reg = reg op r/m */
            v = alu(c, aop, getr(c, c->reg, w), get_rm(c, w), w);
            if (aop != 7) setr(c, c->reg, v, w);
            CLK(3, 9);
        } else {                                    /* r/m = r/m op reg */
            v = alu(c, aop, get_rm(c, w), getr(c, c->reg, w), w);
            if (aop != 7) set_rm(c, v, w);
            CLK(3, aop == 7 ? 9 : 16);
        }
        break;
    }
    case 0x04: case 0x05: case 0x0C: case 0x0D: case 0x14: case 0x15: case 0x1C: case 0x1D:
    case 0x24: case 0x25: case 0x2C: case 0x2D: case 0x34: case 0x35: case 0x3C: case 0x3D: {
        int aop = op >> 3;
        t = w ? fetch16(c) : fetch8(c);
        v = alu(c, aop, getr(c, X86_AX, w), t, w);
        if (aop != 7) setr(c, X86_AX, v, w);
        c->cycles += 4;
        break;
    }
    case 0x06: case 0x0E: case 0x16: case 0x1E:     /* PUSH sreg */
        push(c, c->s[(op >> 3) & 3]);
        c->cycles += 10;
        break;
    case 0x07: case 0x17: case 0x1F:                /* POP sreg */
        c->s[(op >> 3) & 3] = pop(c);
        if (op == 0x17 || !IS286(c)) c->shadow = 1;
        c->cycles += 8;
        break;
    case 0x0F:
        if (IS8086(c)) {                            /* POP CS (8086 only) */
            c->s[X86_CS] = pop(c);
            c->shadow = 1;
            c->cycles += 8;
        } else if (c->model == X86_MODEL_V30) {
            op_0f_v30(c);
        } else {
            op_0f_286(c);
        }
        break;
    case 0x27: daa_das(c, 0); break;
    case 0x2F: daa_das(c, 1); break;
    case 0x37: aaa_aas(c, 0); break;
    case 0x3F: aaa_aas(c, 1); break;

    /* ---- INC/DEC/PUSH/POP r16 */
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        c->r[op & 7] = incdec(c, c->r[op & 7], 0, 1);
        c->cycles += 2;
        break;
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        c->r[op & 7] = incdec(c, c->r[op & 7], 1, 1);
        c->cycles += 2;
        break;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        if (op == 0x54 && IS286(c)) { t = SP; push(c, t); }   /* the 286 pushes SP as it was */
        else if (op == 0x54) { SP -= 2; wr16(c, X86_SS, SP, SP); } /* 8086/V30: the decremented value */
        else push(c, c->r[op & 7]);
        c->cycles += 11;
        break;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        v = pop(c);
        c->r[op & 7] = v;
        c->cycles += 8;
        break;

    /* ---- 80186 additions (V30, 286; the 8086 never gets here, see the aliases above) */
    case 0x60:                                      /* PUSHA */
        t = SP;
        push(c, AX); push(c, CX); push(c, DX); push(c, BX);
        push(c, t); push(c, BP); push(c, SI); push(c, DI);
        c->cycles += 36;
        break;
    case 0x61:                                      /* POPA */
        DI = pop(c); SI = pop(c); BP = pop(c); (void)pop(c);
        BX = pop(c); DX = pop(c); CX = pop(c); AX = pop(c);
        c->cycles += 51;
        break;
    case 0x62: {                                    /* BOUND r16, m16&16 */
        modrm(c);
        if (c->mod == 3) { invalid_opcode(c, op); break; }
        int16_t lo = (int16_t)rd16(c, c->ea_seg, c->ea), hi = (int16_t)rd16(c, c->ea_seg, (uint16_t)(c->ea + 2));
        int16_t x = (int16_t)c->r[c->reg];
        c->cycles += 35;
        if (x < lo || x > hi) int_entry(c, 5, c->op_ip);
        break;
    }
    case 0x63: case 0x66: case 0x67:
        if (c->model == X86_MODEL_V30) { c->cycles += 2; break; }    /* V30: no-ops as far as we care */
        invalid_opcode(c, op);
        break;
    case 0x64: case 0x65:                           /* only reached on the 286 (prefixes on the V30) */
        invalid_opcode(c, op);
        break;
    case 0x68: push(c, fetch16(c)); c->cycles += 10; break;
    case 0x6A: push(c, (uint16_t)(int8_t)fetch8(c)); c->cycles += 10; break;
    case 0x69: case 0x6B: {                         /* IMUL r16, r/m16, imm */
        modrm(c);
        int32_t a = (int16_t)get_rm(c, 1);
        int32_t b = op == 0x69 ? (int16_t)fetch16(c) : (int8_t)fetch8(c);
        int32_t r = a * b;
        c->r[c->reg] = (uint16_t)r;
        setf(c, CF | OF, r != (int16_t)r);
        szp(c, (uint16_t)r, 1);
        c->cycles += 25;
        break;
    }
    case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        string_op(c, op);
        break;

    /* ---- conditional jumps */
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int8_t rel = (int8_t)fetch8(c);
        if (cond(c, op & 15)) { c->ip = (uint16_t)(c->ip + rel); c->cycles += 16; }
        else c->cycles += 4;
        break;
    }

    /* ---- group 1, TEST, XCHG, MOV */
    case 0x80: case 0x81: case 0x82: case 0x83:
        modrm(c);
        v = get_rm(c, w);
        t = (op == 0x81) ? fetch16(c) : (op == 0x83) ? (uint16_t)(int8_t)fetch8(c) : fetch8(c);
        v = alu(c, c->reg, v, t, w);
        if (c->reg != 7) set_rm(c, v, w);
        CLK(4, c->reg == 7 ? 10 : 17);
        break;
    case 0x84: case 0x85:
        modrm(c);
        alu(c, 4, get_rm(c, w), getr(c, c->reg, w), w);
        CLK(3, 9);
        break;
    case 0x86: case 0x87:
        modrm(c);
        v = get_rm(c, w);
        set_rm(c, getr(c, c->reg, w), w);
        setr(c, c->reg, v, w);
        CLK(4, 17);
        break;
    case 0x88: case 0x89:
        modrm(c);
        set_rm(c, getr(c, c->reg, w), w);
        CLK(2, 9);
        break;
    case 0x8A: case 0x8B:
        modrm(c);
        setr(c, c->reg, get_rm(c, w), w);
        CLK(2, 8);
        break;
    case 0x8C:                                      /* MOV r/m16, sreg (the 8086 looks at two bits of reg) */
        modrm(c);
        if (IS286(c) && c->reg > 3) { invalid_opcode(c, op); break; }
        set_rm(c, c->s[c->reg & 3], 1);
        CLK(2, 9);
        break;
    case 0x8D:                                      /* LEA */
        modrm(c);
        if (c->mod == 3) {
            if (IS286(c)) invalid_opcode(c, op);   /* undefined on the 8086: leave the register alone */
            break;
        }
        c->r[c->reg] = c->ea;
        c->cycles += 2;
        break;
    case 0x8E: {                                    /* MOV sreg, r/m16 */
        modrm(c);
        int s = c->reg & 3;
        if (IS286(c) && (c->reg > 3 || s == X86_CS)) { invalid_opcode(c, op); break; }
        c->s[s] = get_rm(c, 1);
        if (s == X86_SS || !IS286(c)) c->shadow = 1;
        CLK(2, 8);
        break;
    }
    case 0x8F:                                      /* POP r/m16 (reg != 0 is undefined; treated as 0) */
        modrm(c);
        v = pop(c);
        set_rm(c, v, 1);
        CLK(8, 17);
        break;

    case 0x90: c->cycles += 3; break;               /* NOP (XCHG AX, AX) */
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        t = AX; AX = c->r[op & 7]; c->r[op & 7] = t;
        c->cycles += 3;
        break;
    case 0x98: AX = (uint16_t)(int8_t)AX; c->cycles += 2; break;                 /* CBW */
    case 0x99: DX = (AX & 0x8000) ? 0xFFFF : 0; c->cycles += 5; break;           /* CWD */
    case 0x9A: {                                    /* CALL far ptr16:16 */
        uint16_t off = fetch16(c), seg = fetch16(c);
        push(c, c->s[X86_CS]);
        push(c, c->ip);
        c->s[X86_CS] = seg;
        c->ip = off;
        c->cycles += 28;
        break;
    }
    case 0x9B: c->cycles += 3; break;               /* WAIT: no coprocessor, TEST is never busy */
    case 0x9C: push(c, c->flags); c->cycles += 10; break;
    case 0x9D: c->flags = fix_flags(c, pop(c)); c->cycles += 8; break;
    case 0x9E:                                      /* SAHF */
        c->flags = fix_flags(c, (uint16_t)((c->flags & 0xFF00) | (AX >> 8)));
        c->cycles += 4;
        break;
    case 0x9F: setr8(c, 4, (uint8_t)c->flags); c->cycles += 4; break;           /* LAHF */

    /* ---- MOV with a direct address, string instructions, TEST acc */
    case 0xA0: case 0xA1:
        t = fetch16(c);
        setr(c, X86_AX, rdw(c, dseg(c), t, w), w);
        c->cycles += 10;
        break;
    case 0xA2: case 0xA3:
        t = fetch16(c);
        wrw(c, dseg(c), t, getr(c, X86_AX, w), w);
        c->cycles += 10;
        break;
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        string_op(c, op);
        break;
    case 0xA8: case 0xA9:
        t = w ? fetch16(c) : fetch8(c);
        alu(c, 4, getr(c, X86_AX, w), t, w);
        c->cycles += 4;
        break;

    /* ---- MOV reg, imm */
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        setr8(c, op & 7, fetch8(c));
        c->cycles += 4;
        break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        c->r[op & 7] = fetch16(c);
        c->cycles += 4;
        break;

    /* ---- returns, LES/LDS, MOV r/m, imm, ENTER/LEAVE, INT */
    case 0xC0: case 0xC1:                           /* 186: shift r/m, imm8 */
        modrm(c);
        v = get_rm(c, w);
        t = fetch8(c);
        set_rm(c, shift(c, c->reg, v, t, w), w);
        CLK(5, 17);
        break;
    case 0xC2:
        t = fetch16(c);
        c->ip = pop(c);
        SP += t;
        c->cycles += 12;
        break;
    case 0xC3: c->ip = pop(c); c->cycles += 8; break;
    case 0xC4: case 0xC5:                           /* LES / LDS */
        modrm(c);
        if (c->mod == 3) {
            if (IS286(c)) invalid_opcode(c, op);   /* undefined on the 8086: no effect here */
            break;
        }
        c->r[c->reg] = rd16(c, c->ea_seg, c->ea);
        c->s[op == 0xC4 ? X86_ES : X86_DS] = rd16(c, c->ea_seg, (uint16_t)(c->ea + 2));
        c->cycles += 16;
        break;
    case 0xC6: case 0xC7:                           /* MOV r/m, imm (reg field ignored, as on the 8086) */
        modrm(c);
        t = w ? fetch16(c) : fetch8(c);
        set_rm(c, t, w);
        CLK(4, 10);
        break;
    case 0xC8: {                                    /* 186: ENTER imm16, imm8 */
        uint16_t size = fetch16(c);
        uint8_t level = fetch8(c) & 31;
        push(c, BP);
        uint16_t frame = SP;
        if (level) {
            for (int i = 1; i < level; i++) { BP -= 2; push(c, rd16(c, X86_SS, BP)); }
            push(c, frame);
        }
        BP = frame;
        SP -= size;
        c->cycles += 15u + 4u * level;
        break;
    }
    case 0xC9: SP = BP; BP = pop(c); c->cycles += 8; break;  /* 186: LEAVE */
    case 0xCA:
        t = fetch16(c);
        c->ip = pop(c);
        c->s[X86_CS] = pop(c);
        SP += t;
        c->cycles += 17;
        break;
    case 0xCB:
        c->ip = pop(c);
        c->s[X86_CS] = pop(c);
        c->cycles += 18;
        break;
    case 0xCC: soft_int(c, 3); c->cycles += 1; break;
    case 0xCD: t = fetch8(c); soft_int(c, (uint8_t)t); break;
    case 0xCE:
        if (c->flags & OF) { soft_int(c, 4); c->cycles += 2; }
        else c->cycles += 4;
        break;
    case 0xCF:
        c->ip = pop(c);
        c->s[X86_CS] = pop(c);
        c->flags = fix_flags(c, pop(c));
        c->cycles += 24;
        break;

    /* ---- shifts, ASCII adjust, XLAT, ESC */
    case 0xD0: case 0xD1: case 0xD2: case 0xD3:
        modrm(c);
        v = get_rm(c, w);
        set_rm(c, shift(c, c->reg, v, (op & 2) ? (uint8_t)CX : 1, w), w);
        if (op & 2) CLK(8, 20); else CLK(2, 15);
        break;
    case 0xD4: {                                    /* AAM imm8 (the V30 always divides by 10) */
        uint8_t base = fetch8(c);
        if (c->model == X86_MODEL_V30) base = 10;
        c->cycles += 83;
        if (base == 0) {                            /* divide error; the 8086 leaves the flags of a zero result */
            c->flags &= (uint16_t)~(CF | OF | AF);
            szp(c, 0, 0);
            divide_error(c);
            break;
        }
        uint8_t al = (uint8_t)AX;
        AX = (uint16_t)((al / base) << 8 | (al % base));
        szp(c, (uint8_t)AX, 0);
        c->flags &= (uint16_t)~(CF | OF | AF);      /* undefined */
        break;
    }
    case 0xD5: {                                    /* AAD imm8 */
        uint8_t base = fetch8(c);
        if (c->model == X86_MODEL_V30) base = 10;
        uint8_t r = (uint8_t)alu(c, 0, (uint8_t)AX, (uint8_t)((AX >> 8) * base), 0); /* flags: as the 8086's add */
        AX = r;
        c->cycles += 60;
        break;
    }
    case 0xD6: setr8(c, 0, (c->flags & CF) ? 0xFF : 0x00); c->cycles += 3; break;  /* SALC (undocumented) */
    case 0xD7: setr8(c, 0, rd8(c, dseg(c), (uint16_t)(BX + (uint8_t)AX))); c->cycles += 11; break;
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        modrm(c);                                   /* ESC: no coprocessor; operand decoded and ignored */
        c->cycles += 2;
        break;

    /* ---- loops, I/O, CALL/JMP */
    case 0xE0: case 0xE1: case 0xE2: {
        int8_t rel = (int8_t)fetch8(c);
        CX--;
        int go = CX != 0;
        if (op == 0xE0) go = go && !(c->flags & ZF);
        if (op == 0xE1) go = go && (c->flags & ZF);
        if (go) { c->ip = (uint16_t)(c->ip + rel); c->cycles += 17; }
        else c->cycles += 5;
        break;
    }
    case 0xE3: {
        int8_t rel = (int8_t)fetch8(c);
        if (CX == 0) { c->ip = (uint16_t)(c->ip + rel); c->cycles += 18; }
        else c->cycles += 6;
        break;
    }
    case 0xE4: case 0xE5: t = fetch8(c); setr(c, X86_AX, io_in(c, t, w), w); c->cycles += 10; break;
    case 0xE6: case 0xE7: t = fetch8(c); io_out(c, t, getr(c, X86_AX, w), w); c->cycles += 10; break;
    case 0xEC: case 0xED: setr(c, X86_AX, io_in(c, DX, w), w); c->cycles += 8; break;
    case 0xEE: case 0xEF: io_out(c, DX, getr(c, X86_AX, w), w); c->cycles += 8; break;
    case 0xE8:
        t = fetch16(c);
        push(c, c->ip);
        c->ip = (uint16_t)(c->ip + t);
        c->cycles += 19;
        break;
    case 0xE9: t = fetch16(c); c->ip = (uint16_t)(c->ip + t); c->cycles += 15; break;
    case 0xEA: {
        uint16_t off = fetch16(c), seg = fetch16(c);
        c->s[X86_CS] = seg;
        c->ip = off;
        c->cycles += 15;
        break;
    }
    case 0xEB: { int8_t rel = (int8_t)fetch8(c); c->ip = (uint16_t)(c->ip + rel); c->cycles += 15; break; }

    /* ---- HLT, flag instructions, groups 3 to 5 */
    case 0xF1: invalid_opcode(c, op); break;        /* 286 only: the others take it as LOCK */
    case 0xF4: c->halted = 1; c->cycles += 2; break;
    case 0xF5: c->flags ^= CF; c->cycles += 2; break;
    case 0xF6: case 0xF7:
        modrm(c);
        switch (c->reg) {
        case 0: case 1:                             /* TEST r/m, imm (1 is an alias) */
            v = get_rm(c, w);
            t = w ? fetch16(c) : fetch8(c);
            alu(c, 4, v, t, w);
            CLK(5, 11);
            break;
        case 2: set_rm(c, (uint16_t)~get_rm(c, w), w); CLK(3, 16); break;
        case 3: set_rm(c, alu(c, 5, 0, get_rm(c, w), w), w); CLK(3, 16); break;
        default:
            v = get_rm(c, w);
            if (c->mod != 3) c->cycles += 6;
            muldiv(c, c->reg, v, w);
            break;
        }
        break;
    case 0xF8: c->flags &= (uint16_t)~CF; c->cycles += 2; break;
    case 0xF9: c->flags |= CF; c->cycles += 2; break;
    case 0xFA: c->flags &= (uint16_t)~IF; c->cycles += 2; break;
    case 0xFB: c->flags |= IF; c->shadow = 1; c->cycles += 2; break;
    case 0xFC: c->flags &= (uint16_t)~DF; c->cycles += 2; break;
    case 0xFD: c->flags |= DF; c->cycles += 2; break;
    case 0xFE:
        modrm(c);
        if (c->reg > 1) { invalid_opcode(c, op); break; }
        set_rm(c, incdec(c, get_rm(c, 0), c->reg, 0), 0);
        CLK(3, 15);
        break;
    case 0xFF:
        modrm(c);
        switch (c->reg) {
        case 0: case 1: set_rm(c, incdec(c, get_rm(c, 1), c->reg, 1), 1); CLK(2, 15); break;
        case 2: v = get_rm(c, 1); push(c, c->ip); c->ip = v; CLK(16, 21); break;
        case 3: case 5:
            if (c->mod == 3) { invalid_opcode(c, op); break; }
            v = rd16(c, c->ea_seg, c->ea);
            t = rd16(c, c->ea_seg, (uint16_t)(c->ea + 2));
            if (c->reg == 3) { push(c, c->s[X86_CS]); push(c, c->ip); c->cycles += 37; }
            else c->cycles += 24;
            c->s[X86_CS] = t;
            c->ip = v;
            break;
        case 4: c->ip = get_rm(c, 1); CLK(11, 18); break;
        case 6: case 7:                             /* PUSH r/m16 (7 is an 8086 alias) */
            if (c->reg == 7 && IS286(c)) { invalid_opcode(c, op); break; }
            if (c->mod == 3 && c->rm == X86_SP && !IS286(c)) { SP -= 2; wr16(c, X86_SS, SP, SP); }
            else { v = get_rm(c, 1); push(c, v); }
            CLK(11, 16);
            break;
        }
        break;
    default:
        invalid_opcode(c, op);
        break;
    }
}

/* ------------------------------------------------------------------ public interface */

void x86_init(X86Cpu *cpu, X86Model model, const X86Bus *bus) {
    memset(cpu, 0, sizeof *cpu);
    cpu->model = model;
    cpu->bus = *bus;
    cpu->addr_mask = 0xFFFFF;
    cpu->flags = fix_flags(cpu, 0);
    cpu->seg_ovr = -1;
}

void x86_reset(X86Cpu *cpu) {
    memset(cpu->r, 0, sizeof cpu->r);
    memset(cpu->s, 0, sizeof cpu->s);
    if (cpu->model == X86_MODEL_80286) {
        cpu->s[X86_CS] = 0xF000;
        cpu->ip = 0xFFF0;
        cpu->msw = 0xFFF0;
    } else {
        cpu->s[X86_CS] = 0xFFFF;
        cpu->ip = 0;
    }
    cpu->flags = fix_flags(cpu, 0);
    cpu->halted = cpu->shadow = cpu->nmi_pending = cpu->in_rep = 0;
    cpu->stop = X86_STOP_NONE;
}

void x86_nmi(X86Cpu *cpu) { cpu->nmi_pending = 1; }

void x86_interrupt(X86Cpu *cpu, uint8_t vector) { int_entry(cpu, vector, cpu->ip); }

int x86_step(X86Cpu *c) {
    uint64_t start = c->cycles;
    if (c->stop) { c->cycles += 1; return 1; }
    int shadow = c->shadow;
    c->shadow = 0;
    if (c->nmi_pending) {
        c->nmi_pending = 0;
        int_entry(c, 2, c->ip);
        return (int)(c->cycles - start);
    }
    if (!shadow && (c->flags & IF) && c->bus.intr_ack) {
        int vec = c->bus.intr_ack(c->bus.ctx);
        if (vec >= 0) {
            c->cycles += 10;                        /* INTA cycles on top of the INT entry */
            int_entry(c, (uint8_t)vec, c->ip);
            return (int)(c->cycles - start);
        }
    }
    if (c->halted) { c->cycles += 1; return 1; }
    if (c->bus.trace) c->bus.trace(c->bus.ctx, c);
    int trap = (c->flags & TF) != 0;
    execute(c);
    c->instructions++;
    if (trap && !c->stop && !c->halted) int_entry(c, 1, c->ip);
    if (c->cycles == start) c->cycles++;
    return (int)(c->cycles - start);
}

uint64_t x86_run(X86Cpu *c, uint64_t cycles) {
    uint64_t start = c->cycles, end = start + cycles;
    while (c->cycles < end && !c->stop) {
        if (c->halted && !c->nmi_pending) {
            /* Only an interrupt can end HLT. Ask once; if nothing is pending, the rest of the slice is idle time. */
            if ((c->flags & IF) && c->bus.intr_ack) {
                x86_step(c);
                if (c->halted) { c->cycles = end; break; }
                continue;
            }
            c->cycles = end;
            break;
        }
        x86_step(c);
    }
    return c->cycles - start;
}

const char *x86_model_name(X86Model model) {
    switch (model) {
    case X86_MODEL_8086: return "8086";
    case X86_MODEL_V30: return "V30";
    case X86_MODEL_80286: return "80286";
    }
    return "?";
}
