/*
 * x86emu, the 80386 and 80486 models: a real-mode interpreter for the 32-bit instruction set. include/x86emu.h is the
 * interface and docs/PC98.md ("CPU core") the list of what is modelled. x86cpu.c keeps the 8086, V30 and 286 and hands
 * the 386-class models to x86_386_step() here, so nothing in this file can change how the older models behave.
 *
 * Sources: Intel's 80386 Programmer's Reference Manual (1986) and i486 Microprocessor Programmer's Reference Manual
 * (1990) for the instruction set, the real-address-mode exceptions and the system registers; the 386 and 486 data sheets
 * for the clock counts. Where the manuals say "undefined", the results follow the SingleStepTests 80386 real-mode
 * vectors (recorded on a 386EX: https://github.com/SingleStepTests/80386, fetched by tests/host/x86sst.py on request and
 * never committed) wherever they show a rule; nothing here is verified on a 486. No code from other emulators.
 *
 * How it works, and where it differs from x86cpu.c:
 *  - Registers: cpu->r[] holds the low 16 bits of EAX..EDI, so the machine and HLE BIOS code that read AX..DI work as for
 *    the older models; cpu->rh[] holds the upper halves, which 8- and 16-bit operations cannot reach.
 *  - Faults: every instruction runs on a register snapshot. What faults in real mode (an operand that does not end at or
 *    below the segment limit, which is FFFFh for every segment because nothing can change the reset descriptors without
 *    protected mode, so a word at FFFFh faults instead of wrapping as on the 8086; an invalid opcode; a misused LOCK; a
 *    divide error; BOUND; an instruction longer than 15 bytes; a jump beyond FFFFh) sets cpu->fault. From then on memory
 *    writes and port accesses are suppressed; at the end of the instruction the snapshot is restored and the exception
 *    is entered with IP at the faulting instruction, as the 386 does. Two exceptions to the rollback: the flags of a
 *    divide error stay as the instruction left them (the 386 changes them on its way to the fault; which values it
 *    leaves is not modelled, so here they are those from before), and the registers a POPA had already loaded.
 *  - Operands are 1, 2 or 4 bytes (sz); one ALU, one shifter and one string engine serve all three sizes.
 *  - Real-mode interrupts read their vector through the IDTR (base 0, limit 3FFh after reset; LIDT may move it) and push
 *    FLAGS, CS and IP as words whatever the operand size. A vector beyond the IDT limit becomes a #GP, then a double
 *    fault; no room for the three words (SP odd and below 6) or an undeliverable double fault shuts the CPU down.
 *  - Clocks: the data sheets' counts for the common case (the 486 column where it differs; the 386's "+m" for the next
 *    instruction after a jump counted as 3), no prefetch queue, cache or wait states. Good for pacing, nothing more.
 */
#include <x86emu.h>
#include <string.h>
#include "x86cpu386.h"

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

#define IS486(c) ((c)->model == X86_MODEL_80486)

/* Real-mode exception vectors. */
enum { EX_DE = 0, EX_DB = 1, EX_NMI = 2, EX_BP = 3, EX_OF = 4, EX_BR = 5, EX_UD = 6, EX_NM = 7, EX_DF = 8, EX_SS = 12,
       EX_GP = 13 };

#define LIMIT 0xFFFFu               /* the limit of every segment in real mode (see the header comment) */
#define AC_HI ((uint16_t)(X86_EFLAGS_AC >> 16))

/* Clock counts: the 386's, or the 486's on that model; TRM picks the register or the memory form. */
#define T(a, b) (c->cycles += (uint64_t)(IS486(c) ? (b) : (a)))
#define TRM(r3, m3, r4, m4) do { if (c->mod == 3) T(r3, r4); else T(m3, m4); } while (0)

/* The first exception an instruction raises wins. */
static inline void fault(C *c, int vec) { if (!c->fault) c->fault = (uint8_t)(vec + 1); }

/* ------------------------------------------------------------------ registers */

static inline uint32_t gr32(C *c, int i) { return (uint32_t)c->r[i] | (uint32_t)c->rh[i] << 16; }
static inline void sr32(C *c, int i, uint32_t v) { c->r[i] = (uint16_t)v; c->rh[i] = (uint16_t)(v >> 16); }
static inline uint8_t gr8(C *c, int i) { return i < 4 ? (uint8_t)c->r[i] : (uint8_t)(c->r[i - 4] >> 8); }
static inline void sr8(C *c, int i, uint8_t v) {
    if (i < 4) c->r[i] = (uint16_t)((c->r[i] & 0xFF00) | v);
    else c->r[i - 4] = (uint16_t)((c->r[i - 4] & 0x00FF) | (uint16_t)v << 8);
}
static inline uint32_t getr(C *c, int i, int sz) { return sz == 4 ? gr32(c, i) : sz == 2 ? c->r[i] : gr8(c, i); }
static inline void setr(C *c, int i, uint32_t v, int sz) {
    if (sz == 4) sr32(c, i, v);
    else if (sz == 2) c->r[i] = (uint16_t)v;
    else sr8(c, i, (uint8_t)v);
}
static inline uint32_t szmask(int sz) { return sz == 4 ? 0xFFFFFFFFu : sz == 2 ? 0xFFFFu : 0xFFu; }
static inline uint32_t szsign(int sz) { return 1u << (8 * sz - 1); }
static inline uint32_t sext(uint32_t v, int sz) {
    return sz == 1 ? (uint32_t)(int32_t)(int8_t)v : sz == 2 ? (uint32_t)(int32_t)(int16_t)v : v;
}
/* SI/ESI, DI/EDI and CX/ECX of string instructions, LOOP and JCXZ: the address size picks the width. */
static inline uint32_t areg(C *c, int i) { return c->asz == 4 ? gr32(c, i) : c->r[i]; }
static inline void set_areg(C *c, int i, uint32_t v) { if (c->asz == 4) sr32(c, i, v); else c->r[i] = (uint16_t)v; }

/* ------------------------------------------------------------------ flags */

static inline void setf(C *c, uint16_t f, int on) { if (on) c->flags |= f; else c->flags &= (uint16_t)~f; }
static inline int even_parity(uint8_t v) { v ^= v >> 4; return !((0x6996 >> (v & 15)) & 1); }
static inline void szp(C *c, uint32_t v, int sz) {
    v &= szmask(sz);
    setf(c, ZF, v == 0);
    setf(c, SF, (v & szsign(sz)) != 0);
    setf(c, PF, even_parity((uint8_t)v));
}
/* FLAGS loads (POPF, IRET, SAHF goes elsewhere): real mode runs at privilege level 0, so every flag below bit 15 can be
 * written, IOPL and NT included (that is how software tells a 386 from a 286, whose real mode keeps bits 12-15 at 0);
 * bit 15 reads 0 and bit 1 reads 1. */
static inline void load_flags16(C *c, uint32_t v) { c->flags = (uint16_t)((v & 0x7FD5) | 0x0002); }
/* POPFD and IRETD also reach EFLAGS' upper half: the 486 keeps AC; RF reads back 0 (no debug breakpoints), VM stays 0,
 * and neither chip has the ID flag (bit 21), which is how software knows CPUID is missing. */
static inline void load_flags32(C *c, uint32_t v) {
    load_flags16(c, v);
    c->flags_hi = IS486(c) ? (uint16_t)((v >> 16) & AC_HI) : 0;
}
static inline uint32_t eflags(C *c) { return c->flags | (uint32_t)c->flags_hi << 16; }

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

/* ------------------------------------------------------------------ memory, fetch, ports */

static inline uint8_t bus_rd(C *c, uint32_t a) { return c->bus.read8(c->bus.ctx, a & c->addr_mask); }
static inline void bus_wr(C *c, uint32_t a, uint8_t v) { c->bus.write8(c->bus.ctx, a & c->addr_mask, v); }
/* Word accesses go to the bus as words when it has the hooks (the PC-98's EGC treats a word differently from two bytes),
 * except for a word at the very top of the address space, which wraps. */
static inline uint16_t bus_rd16(C *c, uint32_t a) {
    uint32_t m = a & c->addr_mask;
    if (c->bus.read16 && m != c->addr_mask) return c->bus.read16(c->bus.ctx, m);
    return (uint16_t)(bus_rd(c, a) | (uint16_t)bus_rd(c, a + 1) << 8);
}
static inline void bus_wr16(C *c, uint32_t a, uint16_t v) {
    uint32_t m = a & c->addr_mask;
    if (c->bus.write16 && m != c->addr_mask) { c->bus.write16(c->bus.ctx, m, v); return; }
    bus_wr(c, a, (uint8_t)v);
    bus_wr(c, a + 1, (uint8_t)(v >> 8));
}
static inline uint32_t base_of(C *c, int seg) { return (uint32_t)c->s[seg] << 4; }

/* The real-mode limit check: an operand of sz bytes at off must end at or below the limit, or the 386 raises #SS (for
 * the stack segment) or #GP (for any other). */
static inline int in_limit(C *c, int seg, uint32_t off, int sz) {
    if (off <= LIMIT && (uint32_t)(sz - 1) <= LIMIT - off) return 1;
    fault(c, seg == X86_SS ? EX_SS : EX_GP);
    return 0;
}
static uint32_t rdm(C *c, int seg, uint32_t off, int sz) {
    if (c->fault || !in_limit(c, seg, off, sz)) return 0;
    uint32_t a = base_of(c, seg) + off;
    if (sz == 1) return bus_rd(c, a);
    if (sz == 2) return bus_rd16(c, a);
    uint32_t lo = bus_rd16(c, a);
    return lo | (uint32_t)bus_rd16(c, a + 2) << 16;
}
static void wrm(C *c, int seg, uint32_t off, uint32_t v, int sz) {
    if (c->fault || !in_limit(c, seg, off, sz)) return;
    uint32_t a = base_of(c, seg) + off;
    if (sz == 1) { bus_wr(c, a, (uint8_t)v); return; }
    bus_wr16(c, a, (uint16_t)v);
    if (sz == 4) bus_wr16(c, a + 2, (uint16_t)(v >> 16));
}

/* Code fetch: beyond 15 bytes in one instruction, or past offset FFFFh (the CS limit), is a #GP. The 386 does not wrap
 * IP: after the byte at FFFFh EIP is 10000h, so an instruction that ends there completes and the next fetch faults,
 * pushing IP 0 (the SingleStepTests vectors show this). fetch_wrap stands for that EIP while IP reads 0; set_ip(),
 * which every control transfer goes through, clears it. */
static uint8_t fetch8(C *c) {
    if (c->fault) return 0;
    if (++c->ilen > 15 || (c->fetch_wrap && c->ip == 0)) { fault(c, EX_GP); return 0; }
    uint8_t v = bus_rd(c, base_of(c, X86_CS) + c->ip);
    if (c->ip == LIMIT) c->fetch_wrap = 1;
    c->ip++;
    return v;
}
static inline void set_ip(C *c, uint32_t v) { c->ip = (uint16_t)v; c->fetch_wrap = 0; }
static uint16_t fetch16(C *c) { uint16_t lo = fetch8(c); return (uint16_t)(lo | (uint16_t)fetch8(c) << 8); }
static uint32_t fetch32(C *c) { uint32_t lo = fetch16(c); return lo | (uint32_t)fetch16(c) << 16; }
static uint32_t fetchsz(C *c, int sz) { return sz == 4 ? fetch32(c) : sz == 2 ? fetch16(c) : fetch8(c); }

static uint8_t in8raw(C *c, uint16_t p) { return c->bus.in8 ? c->bus.in8(c->bus.ctx, p) : 0xFF; }
static uint16_t in16raw(C *c, uint16_t p) {
    if (c->bus.in16) return c->bus.in16(c->bus.ctx, p);
    uint16_t lo = in8raw(c, p);
    return (uint16_t)(lo | (uint16_t)in8raw(c, (uint16_t)(p + 1)) << 8);
}
static void out8raw(C *c, uint16_t p, uint8_t v) { if (c->bus.out8) c->bus.out8(c->bus.ctx, p, v); }
static void out16raw(C *c, uint16_t p, uint16_t v) {
    if (c->bus.out16) { c->bus.out16(c->bus.ctx, p, v); return; }
    out8raw(c, p, (uint8_t)v);
    out8raw(c, (uint16_t)(p + 1), (uint8_t)(v >> 8));
}
/* A doubleword I/O access goes out as two word accesses (the PC-98's C-bus is 16 bits wide). Nothing reaches the bus
 * once the instruction has faulted. */
static uint32_t io_in(C *c, uint16_t port, int sz) {
    if (c->fault) return 0;
    if (sz == 1) return in8raw(c, port);
    if (sz == 2) return in16raw(c, port);
    uint32_t lo = in16raw(c, port);
    return lo | (uint32_t)in16raw(c, (uint16_t)(port + 2)) << 16;
}
static void io_out(C *c, uint16_t port, uint32_t v, int sz) {
    if (c->fault) return;
    if (sz == 1) out8raw(c, port, (uint8_t)v);
    else if (sz == 2) out16raw(c, port, (uint16_t)v);
    else { out16raw(c, port, (uint16_t)v); out16raw(c, (uint16_t)(port + 2), (uint16_t)(v >> 16)); }
}

/* The stack is SS:SP (a 16-bit stack: real mode never sets the descriptor's B bit). */
static void push(C *c, uint32_t v, int sz) {
    uint16_t sp = (uint16_t)(SP - sz);
    wrm(c, X86_SS, sp, v, sz);
    if (!c->fault) SP = sp;
}
static uint32_t pop(C *c, int sz) {
    uint32_t v = rdm(c, X86_SS, SP, sz);
    if (!c->fault) SP = (uint16_t)(SP + sz);
    return v;
}

/* ------------------------------------------------------------------ ModR/M and SIB */

static void modrm(C *c) {
    uint8_t b = fetch8(c);
    c->mod = b >> 6;
    c->reg = (b >> 3) & 7;
    c->rm = b & 7;
    c->ea_esp = 0;
    if (c->mod == 3) return;
    int seg = X86_DS;
    uint32_t ea;
    if (c->asz == 2) {
        uint16_t e;
        switch (c->rm) {
        case 0: e = (uint16_t)(BX + SI); break;
        case 1: e = (uint16_t)(BX + DI); break;
        case 2: e = (uint16_t)(BP + SI); seg = X86_SS; break;
        case 3: e = (uint16_t)(BP + DI); seg = X86_SS; break;
        case 4: e = SI; break;
        case 5: e = DI; break;
        case 6: e = BP; seg = X86_SS; break;
        default: e = BX; break;
        }
        if (c->mod == 0 && c->rm == 6) { e = fetch16(c); seg = X86_DS; }
        else if (c->mod == 1) e = (uint16_t)(e + (uint16_t)(int16_t)(int8_t)fetch8(c));
        else if (c->mod == 2) e = (uint16_t)(e + fetch16(c));
        ea = e;
    } else {
        int base = c->rm, index = 4, scale = 0;
        if (base == 4) {
            uint8_t sib = fetch8(c);
            scale = sib >> 6;
            index = (sib >> 3) & 7;
            base = sib & 7;
        }
        if (base == 5 && c->mod == 0) {
            ea = fetch32(c);                        /* disp32, no base */
        } else {
            ea = gr32(c, base);
            if (base == 4 || base == 5) seg = X86_SS;
            c->ea_esp = base == 4;
            /* With index 100b ("none") the 386 still applies the SIB scale, to the base register: an undocumented effect
             * the SingleStepTests 80386 vectors show. Later chips ignore the scale there (the 486 is assumed to). */
            if (index == 4 && scale && !IS486(c)) ea <<= scale;
        }
        if (index != 4) ea += gr32(c, index) << scale;
        if (c->mod == 1) ea += (uint32_t)(int32_t)(int8_t)fetch8(c);
        else if (c->mod == 2) ea += fetch32(c);
    }
    if (c->seg_ovr >= 0) seg = c->seg_ovr;
    c->ea32 = ea;
    c->ea_seg = (uint8_t)seg;
}

/* The offset of the second part of a multi-part memory operand (the selector of a far pointer, BOUND's upper bound,
 * the base in LGDT's operand): with 16-bit addressing it wraps at 64 KiB like the effective address itself, so a far
 * pointer at FFFEh takes its selector from offset 0 without a fault (SingleStepTests vectors); each part is checked on
 * its own. With 32-bit addressing nothing wraps. */
static inline uint32_t ea_plus(C *c, uint32_t n) {
    uint32_t a = c->ea32 + n;
    return c->asz == 2 ? (a & 0xFFFF) : a;
}

static inline uint32_t get_rm(C *c, int sz) { return c->mod == 3 ? getr(c, c->rm, sz) : rdm(c, c->ea_seg, c->ea32, sz); }
static inline void set_rm(C *c, uint32_t v, int sz) {
    if (c->mod == 3) setr(c, c->rm, v, sz); else wrm(c, c->ea_seg, c->ea32, v, sz);
}
static inline int dseg(C *c) { return c->seg_ovr >= 0 ? c->seg_ovr : X86_DS; }

/* LOCK is accepted only on a read-modify-write of memory (ADD..XOR without CMP, INC, DEC, NOT, NEG, XCHG, BTS/BTR/BTC,
 * and the 486's XADD and CMPXCHG); anywhere else it is an invalid opcode. The SingleStepTests vectors agree, and show
 * that BT, CMP, TEST and MOV are not lockable. lock_ok() is called after the ModR/M byte of a lockable form. */
static inline int lock_ok(C *c, int lockable) {
    if (!c->lock || (lockable && c->mod != 3)) return 1;
    fault(c, EX_UD);
    return 0;
}
static int lock_candidate(uint8_t op) {
    switch (op) {
    case 0x0F: case 0x80: case 0x81: case 0x82: case 0x83: case 0x86: case 0x87: case 0xF6: case 0xF7: case 0xFE:
    case 0xFF:
        return 1;
    }
    return op < 0x38 && (op & 6) == 0;              /* 00/01, 08/09, ... 30/31: r/m op= reg */
}

/* ------------------------------------------------------------------ arithmetic */

/* The eight classic ALU operations (0 ADD, 1 OR, 2 ADC, 3 SBB, 4 AND, 5 SUB, 6 XOR, 7 CMP) at any size. */
static uint32_t alu(C *c, int op, uint32_t a, uint32_t b, int sz) {
    uint32_t m = szmask(sz), sign = szsign(sz), r, ci;
    a &= m;
    b &= m;
    switch (op) {
    case 0: case 2: {
        ci = op == 2 ? (c->flags & CF) : 0;
        uint64_t full = (uint64_t)a + b + ci;
        r = (uint32_t)full & m;
        setf(c, CF, full > m);
        setf(c, OF, ((a ^ r) & (b ^ r) & sign) != 0);
        setf(c, AF, ((a ^ b ^ r) & 0x10) != 0);
        break;
    }
    case 3: case 5: case 7:
        ci = op == 3 ? (c->flags & CF) : 0;
        r = (a - b - ci) & m;
        setf(c, CF, (uint64_t)b + ci > a);
        setf(c, OF, ((a ^ b) & (a ^ r) & sign) != 0);
        setf(c, AF, ((a ^ b ^ r) & 0x10) != 0);
        break;
    default:
        r = op == 1 ? (a | b) : op == 4 ? (a & b) : (a ^ b);
        c->flags &= (uint16_t)~(CF | OF | AF);      /* AF is undefined after logic operations; the 386 clears it */
        break;
    }
    szp(c, r, sz);
    return r;
}

static uint32_t incdec(C *c, uint32_t v, int dec, int sz) {
    uint32_t m = szmask(sz), sign = szsign(sz);
    v &= m;
    uint32_t r = (dec ? v - 1 : v + 1) & m;
    setf(c, OF, dec ? v == sign : r == sign);
    setf(c, AF, dec ? (r & 0xF) == 0xF : (r & 0xF) == 0);
    szp(c, r, sz);
    return r;
}

/* Group 2 at any size: 0 ROL, 1 ROR, 2 RCL, 3 RCR, 4 SHL, 5 SHR, 6 SAL (= SHL), 7 SAR. The 386 masks the count to five
 * bits for every operand size; a masked count of zero changes nothing, flags included. RCL/RCR rotate through CF over
 * 9, 17 or 33 bits. What the manuals leave undefined follows the SingleStepTests 80386 vectors (every one agrees): OF
 * after a multi-bit shift is the single-bit formula applied to the result (MSB XOR CF for left shifts and ROL/RCL, the
 * top two bits for ROR/RCR, 0 for SHR and SAR); CF of SHL/SHR is the last bit shifted out, 0 past the width; and AF is
 * set by SHL, SHR and SAR (rotates leave it). */
static uint32_t shift(C *c, int op, uint32_t v, unsigned cnt, int sz) {
    unsigned n = 8u * (unsigned)sz;
    uint32_t m = szmask(sz), msb = szsign(sz), r;
    int cf, of;
    cnt &= 31;
    v &= m;
    if (cnt == 0) return v;
    switch (op) {
    case 0: {
        unsigned k = cnt % n;
        r = k ? ((v << k) | (v >> (n - k))) & m : v;
        cf = (int)(r & 1);
        of = ((r & msb) != 0) ^ cf;
        break;
    }
    case 1: {
        unsigned k = cnt % n;
        r = k ? ((v >> k) | (v << (n - k))) & m : v;
        cf = (r & msb) != 0;
        of = cf ^ (int)((r >> (n - 2)) & 1);
        break;
    }
    case 2: case 3: {
        unsigned k = cnt % (n + 1);
        uint64_t big = (uint64_t)(c->flags & CF) << n | v, mm = ((uint64_t)1 << (n + 1)) - 1;
        if (k) big = op == 2 ? ((big << k) | (big >> (n + 1 - k))) & mm : ((big >> k) | (big << (n + 1 - k))) & mm;
        r = (uint32_t)big & m;
        cf = (int)(big >> n) & 1;
        of = op == 2 ? (((r & msb) != 0) ^ cf) : (((r & msb) != 0) ^ (int)((r >> (n - 2)) & 1));
        break;
    }
    /* SHL/SHR past the width: CF is 0, except that a byte shifted by 16 or 24 gives the CF of a shift by 8 (the vectors,
     * without exception). */
    case 4: case 6:
        r = cnt < n ? (v << cnt) & m : 0;
        cf = cnt <= n ? (int)((v >> (n - cnt)) & 1) : (sz == 1 && !(cnt & 7)) ? (int)(v & 1) : 0;
        of = ((r & msb) != 0) ^ cf;
        break;
    case 5:
        r = cnt < n ? v >> cnt : 0;
        cf = cnt <= n ? (int)((v >> (cnt - 1)) & 1) : (sz == 1 && !(cnt & 7)) ? (int)(v >> 7) : 0;
        of = cnt == 1 ? (v & msb) != 0 : 0;
        break;
    default: {
        uint32_t s = (v & msb) ? m : 0;
        if (cnt < n) { r = ((v >> cnt) | (s << (n - cnt))) & m; cf = (int)((v >> (cnt - 1)) & 1); }
        else { r = s; cf = (int)(s & 1); }
        of = 0;
        break;
    }
    }
    setf(c, CF, cf);
    setf(c, OF, of);
    if (op >= 4) {
        szp(c, r, sz);
        c->flags |= AF;
    }
    return r;
}

/* SHLD/SHRD: dst shifted by cnt (masked to five bits) with the bits coming in from src. A 16-bit operand with a count
 * above 16 is undefined in the manuals; the 386 shifts a 48-bit quantity with the source repeated, dst:src:src for SHLD
 * and src:src:dst for SHRD (all the SingleStepTests 80386 vectors agree), which for counts up to 16 is the defined
 * result. CF is the last bit shifted out of that quantity. OF is defined for a count of 1 (the sign change); for every
 * count the 386 gives MSB XOR CF after SHLD and the XOR of the top two result bits after SHRD (which is the same thing
 * for a count of 1), and it sets AF (the vectors again). */
static uint32_t shxd(C *c, int left, uint32_t dst, uint32_t src, unsigned cnt, int sz) {
    uint32_t m = szmask(sz), msb = szsign(sz), r;
    int cf;
    cnt &= 31;
    dst &= m;
    src &= m;
    if (cnt == 0) return dst;
    if (sz == 4) {
        r = left ? (dst << cnt) | (src >> (32 - cnt)) : (dst >> cnt) | (src << (32 - cnt));
        cf = left ? (int)((dst >> (32 - cnt)) & 1) : (int)((dst >> (cnt - 1)) & 1);
    } else if (left) {
        uint64_t q = ((uint64_t)dst << 32 | (uint64_t)src << 16 | src) << cnt;   /* dst:src:src, shifted up */
        r = (uint32_t)(q >> 32) & m;
        cf = (int)((q >> 48) & 1);
    } else {
        uint64_t q = (uint64_t)src << 32 | (uint64_t)src << 16 | dst;            /* src:src:dst, shifted down */
        r = (uint32_t)(q >> cnt) & m;
        cf = (int)((q >> (cnt - 1)) & 1);
    }
    setf(c, CF, cf);
    if (left) setf(c, OF, ((r & msb) != 0) != cf);
    else setf(c, OF, ((r ^ (r << 1)) & msb) != 0);
    szp(c, r, sz);
    c->flags |= AF;
    return r;
}

/* MUL, IMUL (one operand), DIV, IDIV at any size (group 3, reg 4-7). A divide error is a fault (#DE, IP at the
 * instruction); the quotient range is the full signed range, -128 and -32768 included, as on the 286. */
static void store_wide(C *c, uint64_t p, int sz) {
    if (sz == 1) AX = (uint16_t)p;
    else if (sz == 2) { AX = (uint16_t)p; DX = (uint16_t)(p >> 16); }
    else { sr32(c, X86_AX, (uint32_t)p); sr32(c, X86_DX, (uint32_t)(p >> 32)); }
}
static void muldiv(C *c, int op, uint32_t src, int sz) {
    unsigned n = 8u * (unsigned)sz;
    uint32_t m = szmask(sz);
    src &= m;
    switch (op) {
    case 4: {
        uint64_t p = (uint64_t)getr(c, X86_AX, sz) * src;
        store_wide(c, p, sz);
        setf(c, CF | OF, (p >> n) != 0);
        szp(c, (uint32_t)p, sz);
        T(sz == 1 ? 12 : sz == 2 ? 17 : 25, sz == 1 ? 13 : sz == 2 ? 18 : 27);
        return;
    }
    case 5: {
        int64_t p = (int64_t)(int32_t)sext(getr(c, X86_AX, sz), sz) * (int64_t)(int32_t)sext(src, sz);
        store_wide(c, (uint64_t)p, sz);
        setf(c, CF | OF, (int64_t)(int32_t)sext((uint32_t)p & m, sz) != p);
        szp(c, (uint32_t)p, sz);
        T(sz == 1 ? 14 : sz == 2 ? 19 : 27, sz == 1 ? 15 : sz == 2 ? 20 : 30);
        return;
    }
    case 6: {
        uint64_t num = sz == 1 ? AX : sz == 2 ? ((uint64_t)DX << 16 | AX) : ((uint64_t)gr32(c, X86_DX) << 32 | gr32(c, X86_AX));
        T(sz == 1 ? 14 : sz == 2 ? 22 : 38, sz == 1 ? 16 : sz == 2 ? 24 : 40);
        if (src == 0 || num / src > m) { fault(c, EX_DE); return; }
        uint64_t q = num / src, rem = num % src;
        if (sz == 1) AX = (uint16_t)(rem << 8 | q);
        else { setr(c, X86_AX, (uint32_t)q, sz); setr(c, X86_DX, (uint32_t)rem, sz); }
        return;
    }
    default: {
        T(sz == 1 ? 19 : sz == 2 ? 27 : 43, sz == 1 ? 19 : sz == 2 ? 27 : 43);
        uint64_t num = sz == 1 ? AX : sz == 2 ? ((uint64_t)DX << 16 | AX) : ((uint64_t)gr32(c, X86_DX) << 32 | gr32(c, X86_AX));
        unsigned nn = 2 * n;                        /* the dividend has twice the operand's bits */
        int nneg = (int)((num >> (nn - 1)) & 1);
        uint64_t an = nneg ? (nn == 64 ? 0 - num : ((uint64_t)1 << nn) - num) : num;
        uint32_t d = src;
        int dneg = (d & szsign(sz)) != 0;
        uint64_t ad = dneg ? (uint64_t)((0 - d) & m) : d;      /* (the most negative divisor stays 2^(n-1)) */
        if (ad == 0) { fault(c, EX_DE); return; }
        uint64_t aq = an / ad, ar = an % ad;
        int qneg = nneg != dneg;
        if (aq > (qneg ? (uint64_t)szsign(sz) : (uint64_t)szsign(sz) - 1)) { fault(c, EX_DE); return; }
        uint32_t q = (uint32_t)(qneg ? 0 - aq : aq) & m, r = (uint32_t)(nneg ? 0 - ar : ar) & m;
        if (sz == 1) AX = (uint16_t)(r << 8 | q);
        else { setr(c, X86_AX, q, sz); setr(c, X86_DX, r, sz); }
        c->flags &= (uint16_t)~OF;                  /* the one undefined flag of IDIV the vectors pin down */
        return;
    }
    }
}

/* IMUL r, r/m (0F AF) and IMUL r, r/m, imm (69, 6B): the product truncated to the operand size. */
static uint32_t imul_trunc(C *c, uint32_t a, uint32_t b, int sz) {
    int64_t p = (int64_t)(int32_t)sext(a, sz) * (int64_t)(int32_t)sext(b, sz);
    uint32_t r = (uint32_t)p & szmask(sz);
    setf(c, CF | OF, (int64_t)(int32_t)sext(r, sz) != p);
    szp(c, r, sz);
    return r;
}

/* Decimal adjustments, after the 386 manual's definitions; the undefined flags as the SingleStepTests 80386 vectors
 * show them (all agree): DAA/DAS set OF to the signed overflow of the adjustment, as the 8086 does. */
static void daa_das(C *c, int sub) {
    uint8_t al = (uint8_t)AX, old = al;
    int cf = (c->flags & CF) != 0, af = (c->flags & AF) != 0, ncf = 0;
    if ((al & 0x0F) > 9 || af) {
        ncf = cf || (sub ? al < 6 : al > 0xF9);
        al = (uint8_t)(sub ? al - 6 : al + 6);
        af = 1;
    } else {
        af = 0;
    }
    if (old > 0x99 || cf) {
        al = (uint8_t)(sub ? al - 0x60 : al + 0x60);
        ncf = 1;
    }
    sr8(c, 0, al);
    setf(c, CF, ncf);
    setf(c, AF, af);
    szp(c, al, 1);
    setf(c, OF, sub ? ((old & 0x80) && !(al & 0x80)) : (!(old & 0x80) && (al & 0x80)));
    T(4, 2);
}

/* AAA/AAS adjust AX as a whole (AX + 106h, AX - 106h). SF, ZF and PF come from AL after the +6/-6 and before the mask
 * to the low nibble (the unadjusted AL when no adjustment is made), OF is the signed overflow of that +6/-6 (0 without
 * one): the SingleStepTests 80386 vectors, every one. */
static void aaa_aas(C *c, int sub) {
    uint8_t al = (uint8_t)AX;
    if ((AX & 0x0F) > 9 || (c->flags & AF)) {
        uint8_t r = (uint8_t)(sub ? al - 6 : al + 6);
        AX = (uint16_t)(sub ? AX - 0x106 : AX + 0x106);
        c->flags |= AF | CF;
        szp(c, r, 1);
        setf(c, OF, sub ? ((al ^ 6) & (al ^ r) & 0x80) != 0 : ((al ^ r) & (6 ^ r) & 0x80) != 0);
    } else {
        c->flags &= (uint16_t)~(AF | CF | OF);
        szp(c, al, 1);
    }
    sr8(c, 0, (uint8_t)(AX & 0x0F));
    T(4, 3);
}

/* ------------------------------------------------------------------ interrupts and exceptions */

static void shutdown(C *c) {
    c->stop = X86_STOP_SHUTDOWN;
    c->halted = 0;
    c->in_rep = 0;
}

/* Enters interrupt vec the real-mode way, pushing ret_ip. */
static void enter_int(C *c, int vec, uint16_t ret_ip) {
    if ((SP & 1) && SP < 6) { shutdown(c); return; }   /* no room for FLAGS, CS, IP: nowhere to put a #SS either */
    while ((uint32_t)vec * 4 + 3 > c->idt_limit) {
        if (vec == EX_DF) { shutdown(c); return; }
        vec = vec == EX_GP ? EX_DF : EX_GP;
    }
    /* The vector is read before the frame is pushed (a stack that overlaps the table does not change it: the vectors). */
    uint32_t a = c->idt_base + (uint32_t)vec * 4;
    uint16_t new_ip = (uint16_t)(bus_rd(c, a) | bus_rd(c, a + 1) << 8);
    uint16_t new_cs = (uint16_t)(bus_rd(c, a + 2) | bus_rd(c, a + 3) << 8);
    uint32_t ss = base_of(c, X86_SS);
    uint16_t sp = SP, w[3] = { c->flags, c->s[X86_CS], ret_ip };
    for (int i = 0; i < 3; i++) {
        sp = (uint16_t)(sp - 2);
        bus_wr(c, ss + sp, (uint8_t)w[i]);
        bus_wr(c, ss + sp + 1u, (uint8_t)(w[i] >> 8));
    }
    SP = sp;
    c->flags &= (uint16_t)~(IF | TF);
    c->flags_hi &= (uint16_t)~AC_HI;
    set_ip(c, new_ip);
    c->s[X86_CS] = new_cs;
    c->halted = 0;
    c->in_rep = 0;
    T(37, 30);
}

/* INT n, INT3, INTO, ICEBP: a vector beyond the IDT limit is a #GP fault on the instruction itself; otherwise the host
 * may handle the call (HLE BIOS; not for ICEBP), or the interrupt is entered with the next IP pushed. */
static void soft_int(C *c, int vec, int hle) {
    if (c->fault) return;
    if ((uint32_t)vec * 4 + 3 > c->idt_limit) { fault(c, EX_GP); return; }
    if (hle && c->bus.soft_int && c->bus.soft_int(c->bus.ctx, c, (uint8_t)vec)) return;
    enter_int(c, vec, c->ip);
}

/* ------------------------------------------------------------------ control transfers */

/* A near jump: with a 16-bit operand size the target is truncated to 16 bits; with 32 bits all of EIP is kept, and a
 * target beyond the CS limit is a #GP at the jump. */
/* The address of the next instruction: 10000h when the instruction ended at FFFFh (see fetch8()). */
static inline uint32_t next_ip(C *c) { return (c->fetch_wrap && c->ip == 0) ? 0x10000u : c->ip; }
static void jump_near(C *c, uint32_t target) {
    if (c->osz == 2) target &= 0xFFFF;
    else if (target > LIMIT) { fault(c, EX_GP); return; }
    set_ip(c, target);
}
static void jump_rel(C *c, uint32_t rel) { jump_near(c, next_ip(c) + rel); }
static void call_near(C *c, uint32_t target) {
    if (c->osz == 2) target &= 0xFFFF;
    else if (target > LIMIT) { fault(c, EX_GP); return; }
    push(c, next_ip(c), c->osz);
    if (!c->fault) set_ip(c, target);
}
static void jump_far(C *c, uint16_t sel, uint32_t off) {
    if (off > LIMIT) { fault(c, EX_GP); return; }
    c->s[X86_CS] = sel;
    set_ip(c, off);
}
static void call_far(C *c, uint16_t sel, uint32_t off) {
    if (off > LIMIT) { fault(c, EX_GP); return; }
    push(c, c->s[X86_CS], c->osz);
    push(c, next_ip(c), c->osz);
    if (c->fault) return;
    c->s[X86_CS] = sel;
    set_ip(c, off);
}

/* PUSH and POP of a segment register with a 32-bit operand size move SP by 4 but write or read only the selector's
 * word, leaving the upper half of the stack slot alone (SingleStepTests vectors: the 386 does not even touch it). */
static void push_sreg(C *c, int seg) {
    uint16_t sp = (uint16_t)(SP - c->osz);
    wrm(c, X86_SS, sp, c->s[seg], 2);
    if (!c->fault) SP = sp;
}
static uint16_t pop_sreg(C *c) {
    uint16_t v = (uint16_t)rdm(c, X86_SS, SP, 2);
    if (!c->fault) SP = (uint16_t)(SP + c->osz);
    return v;
}

/* ------------------------------------------------------------------ string instructions */

static int is_string(uint8_t op) {
    return (op >= 0xA4 && op <= 0xA7) || (op >= 0xAA && op <= 0xAF) || (op >= 0x6C && op <= 0x6F);
}

/* One iteration of MOVS/CMPS/STOS/LODS/SCAS/INS/OUTS. The address size picks SI/DI/CX or ESI/EDI/ECX; a REP
 * instruction runs one iteration per step and leaves IP on its first prefix until it is done. */
static void string_op(C *c, uint8_t op) {
    int sz = (op & 1) ? c->osz : 1;
    uint32_t d = (c->flags & DF) ? (uint32_t)-sz : (uint32_t)sz;
    int src = dseg(c), cmp = 0;
    if (c->rep) {
        if (areg(c, X86_CX) == 0) { c->in_rep = 0; T(5, 5); return; }
        if (!c->in_rep) T(5, 7);
    }
    switch (op) {
    case 0xA4: case 0xA5:
        wrm(c, X86_ES, areg(c, X86_DI), rdm(c, src, areg(c, X86_SI), sz), sz);
        set_areg(c, X86_SI, areg(c, X86_SI) + d);
        set_areg(c, X86_DI, areg(c, X86_DI) + d);
        if (c->rep) T(4, 3); else T(7, 7);
        break;
    case 0xA6: case 0xA7: {
        uint32_t a = rdm(c, src, areg(c, X86_SI), sz), b = rdm(c, X86_ES, areg(c, X86_DI), sz);
        alu(c, 7, a, b, sz);
        set_areg(c, X86_SI, areg(c, X86_SI) + d);
        set_areg(c, X86_DI, areg(c, X86_DI) + d);
        cmp = 1;
        if (c->rep) T(9, 7); else T(10, 8);
        break;
    }
    case 0xAA: case 0xAB:
        wrm(c, X86_ES, areg(c, X86_DI), getr(c, X86_AX, sz), sz);
        set_areg(c, X86_DI, areg(c, X86_DI) + d);
        if (c->rep) T(5, 4); else T(4, 5);
        break;
    case 0xAC: case 0xAD:
        setr(c, X86_AX, rdm(c, src, areg(c, X86_SI), sz), sz);
        set_areg(c, X86_SI, areg(c, X86_SI) + d);
        if (c->rep) T(6, 4); else T(5, 5);
        break;
    case 0xAE: case 0xAF:
        alu(c, 7, getr(c, X86_AX, sz), rdm(c, X86_ES, areg(c, X86_DI), sz), sz);
        set_areg(c, X86_DI, areg(c, X86_DI) + d);
        cmp = 1;
        if (c->rep) T(8, 5); else T(7, 6);
        break;
    case 0x6C: case 0x6D:
        /* The destination is checked before the port is read, so a faulting INS has no effect on the device. */
        if (in_limit(c, X86_ES, areg(c, X86_DI), sz)) wrm(c, X86_ES, areg(c, X86_DI), io_in(c, DX, sz), sz);
        set_areg(c, X86_DI, areg(c, X86_DI) + d);
        if (c->rep) T(6, 8); else T(15, 17);
        break;
    default:
        io_out(c, DX, rdm(c, src, areg(c, X86_SI), sz), sz);
        set_areg(c, X86_SI, areg(c, X86_SI) + d);
        if (c->rep) T(5, 10); else T(14, 17);
        break;
    }
    if (!c->rep || c->fault) return;
    set_areg(c, X86_CX, areg(c, X86_CX) - 1);
    int done = areg(c, X86_CX) == 0;
    if (cmp) done |= (c->rep == 0xF3) ? !(c->flags & ZF) : (c->flags & ZF) != 0;
    if (done) {
        c->in_rep = 0;
    } else {
        c->in_rep = 1;
        set_ip(c, c->op_ip);
    }
}

/* ------------------------------------------------------------------ bit instructions */

/* BT, BTS, BTR, BTC (kind 0-3). A register operand takes the bit number modulo its width. A memory operand with the bit
 * number in a register addresses a bit string: the signed number selects the word or doubleword at EA + size * floor(bit
 * / width) (the offset wraps at 16 bits with 16-bit addressing); with an immediate it is taken modulo the width. CF
 * receives the bit; the other flags are left as they were (undefined; unverified). */
static void bit_op(C *c, int kind, uint32_t bitno, int from_reg) {
    int sz = c->osz;
    uint32_t n = 8u * (uint32_t)sz, v, off = c->ea32, bit;
    if (c->mod != 3 && from_reg) {
        int64_t sb = (int64_t)(int32_t)sext(bitno, sz);
        int64_t units = sb >= 0 ? sb / (int64_t)n : -((-sb + (int64_t)n - 1) / (int64_t)n);
        off += (uint32_t)(units * sz);
        if (c->asz == 2) off &= 0xFFFF;
    }
    bit = bitno & (n - 1);
    v = c->mod == 3 ? getr(c, c->rm, sz) : rdm(c, c->ea_seg, off, sz);
    setf(c, CF, (v >> bit) & 1);
    if (kind == 0) { TRM(3, 12, 3, 8); return; }
    v = kind == 1 ? v | (1u << bit) : kind == 2 ? v & ~(1u << bit) : v ^ (1u << bit);
    if (c->mod == 3) setr(c, c->rm, v, sz); else wrm(c, c->ea_seg, off, v, sz);
    TRM(6, 13, 6, 13);
}

/* BSF/BSR: the index of the lowest or highest set bit; ZF set and the destination unchanged for a zero source. */
static void bit_scan(C *c, int reverse) {
    int sz = c->osz;
    uint32_t v = get_rm(c, sz) & szmask(sz);
    if (c->fault) return;
    if (v == 0) { c->flags |= ZF; T(10, 6); return; }
    c->flags &= (uint16_t)~ZF;
    int i;
    if (reverse) for (i = 8 * sz - 1; !((v >> i) & 1); i--) {}
    else for (i = 0; !((v >> i) & 1); i++) {}
    setr(c, c->reg, (uint32_t)i, sz);
    T(10 + 3 * (reverse ? 8 * sz - 1 - i : i), 6 + (reverse ? 8 * sz - 1 - i : i));
}

/* ------------------------------------------------------------------ system registers */

static uint32_t cr0_read(C *c) {
    /* The 386 returns its reserved bits 5-30 as ones; the 486 returns them as zeros and always reports ET (unverified). */
    return IS486(c) ? c->cr0 | X86_CR0_ET : (c->cr0 & 0x8000001Fu) | 0x7FFFFFE0u;
}
static void cr0_write(C *c, uint32_t v) {
    uint32_t keep = X86_CR0_PE | X86_CR0_MP | X86_CR0_EM | X86_CR0_TS | X86_CR0_PG |
                    (IS486(c) ? X86_CR0_NE | X86_CR0_WP | X86_CR0_AM | X86_CR0_NW | X86_CR0_CD : X86_CR0_ET);
    v &= keep;
    if ((v & X86_CR0_PG) && !(v & X86_CR0_PE)) { fault(c, EX_GP); return; }
    if (IS486(c) && (v & X86_CR0_NW) && !(v & X86_CR0_CD)) { fault(c, EX_GP); return; }
    if (c->fault) return;
    c->cr0 = (v & ~X86_CR0_PE) | (IS486(c) ? X86_CR0_ET : 0);
    if (v & X86_CR0_PE) c->stop = X86_STOP_PROTECTED;    /* protected mode is phase 2: the CPU stops here */
}

/* MOV to and from CR, DR and TR (0F 20-26): the operand is always a 32-bit register, whatever the mod field says. */
static void mov_sysreg(C *c, uint8_t op) {
    uint8_t b = fetch8(c);
    int reg = (b >> 3) & 7, rm = b & 7;
    if (c->fault) return;
    switch (op) {
    case 0x20: case 0x22:
        if (reg == 1 || reg > 3) { fault(c, EX_UD); return; }
        if (op == 0x20) sr32(c, rm, reg == 0 ? cr0_read(c) : reg == 2 ? c->cr2 : c->cr3);
        else if (reg == 0) cr0_write(c, gr32(c, rm));
        else if (reg == 2) c->cr2 = gr32(c, rm);
        else c->cr3 = gr32(c, rm) & (IS486(c) ? 0xFFFFF018u : 0xFFFFF000u);
        if (op == 0x20) T(6, 4); else T(reg == 0 ? 10 : 5, reg == 0 ? 17 : 4);
        return;
    case 0x21: case 0x23:
        if (reg == 4 || reg == 5) reg += 2;         /* DR4 and DR5 are aliases of DR6 and DR7 on these chips */
        if (op == 0x21) sr32(c, rm, c->dr[reg]);
        else if (reg == 6) c->dr[6] = (gr32(c, rm) & 0x0000E00Fu) | 0xFFFF0FF0u;
        else if (reg == 7) c->dr[7] = gr32(c, rm) | (IS486(c) ? 0x400u : 0);
        else c->dr[reg] = gr32(c, rm);
        T(22, 10);
        return;
    default:                                        /* 24, 26: test registers */
        if (reg < (IS486(c) ? 3 : 6)) { fault(c, EX_UD); return; }
        if (op == 0x24) sr32(c, rm, c->tr[reg]); else c->tr[reg] = gr32(c, rm);
        T(12, 4);
        return;
    }
}

/* Group 7 (0F 01): SGDT, SIDT, LGDT, LIDT, SMSW, LMSW, and the 486's INVLPG. With a 16-bit operand size LGDT/LIDT
 * load a 24-bit base and SGDT/SIDT store the base with a zero top byte (the 286 stored FFh there). */
static void group7(C *c) {
    modrm(c);
    if (c->fault) return;
    switch (c->reg) {
    case 0: case 1: {
        if (c->mod == 3) { fault(c, EX_UD); return; }
        uint32_t base = c->reg ? c->idt_base : c->gdt_base;
        wrm(c, c->ea_seg, c->ea32, c->reg ? c->idt_limit : c->gdt_limit, 2);
        wrm(c, c->ea_seg, ea_plus(c, 2), c->osz == 2 ? base & 0x00FFFFFFu : base, 4);
        T(9, 10);
        return;
    }
    case 2: case 3: {
        if (c->mod == 3) { fault(c, EX_UD); return; }
        uint32_t lim = rdm(c, c->ea_seg, c->ea32, 2), base = rdm(c, c->ea_seg, ea_plus(c, 2), 4);
        if (c->fault) return;
        if (c->osz == 2) base &= 0x00FFFFFFu;
        if (c->reg == 2) { c->gdt_limit = (uint16_t)lim; c->gdt_base = base; }
        else { c->idt_limit = (uint16_t)lim; c->idt_base = base; }
        T(11, 11);
        return;
    }
    case 4:                                         /* SMSW: a 32-bit register gets all of CR0 (unverified) */
        if (c->mod == 3) setr(c, c->rm, c->osz == 4 ? cr0_read(c) : cr0_read(c) & 0xFFFF, c->osz);
        else wrm(c, c->ea_seg, c->ea32, cr0_read(c) & 0xFFFF, 2);
        TRM(2, 3, 2, 3);
        return;
    case 6: {                                       /* LMSW: PE, MP, EM, TS; PE can be set but not cleared */
        uint32_t v = get_rm(c, 2);
        if (c->fault) return;
        uint32_t nv = (c->cr0 & ~0xEu) | (v & 0xFu);
        c->cr0 = nv & ~X86_CR0_PE;
        if (nv & X86_CR0_PE) c->stop = X86_STOP_PROTECTED;
        TRM(10, 13, 13, 13);
        return;
    }
    case 7:
        if (IS486(c) && c->mod != 3) { T(12, 12); return; }   /* INVLPG: no TLB to flush */
        fault(c, EX_UD);
        return;
    default:
        fault(c, EX_UD);
        return;
    }
}

/* LDS, LES, LSS, LFS, LGS: offset (16 or 32 bits) then selector, from memory only. */
static void load_far(C *c, int seg) {
    modrm(c);
    if (c->mod == 3) { fault(c, EX_UD); return; }
    uint32_t off = rdm(c, c->ea_seg, c->ea32, c->osz);
    uint16_t sel = (uint16_t)rdm(c, c->ea_seg, ea_plus(c, (uint32_t)c->osz), 2);
    if (c->fault) return;
    setr(c, c->reg, off, c->osz);
    c->s[seg] = sel;
    T(7, 6);
}

/* ------------------------------------------------------------------ the 0F page */

static void op_0f(C *c) {
    uint8_t op = fetch8(c);
    int sz = c->osz;
    uint32_t v, t;
    if (c->fault) return;
    if (c->lock && !(op == 0xAB || op == 0xB3 || op == 0xBB || op == 0xBA ||
                     (IS486(c) && (op == 0xB0 || op == 0xB1 || op == 0xC0 || op == 0xC1)))) {
        fault(c, EX_UD);
        return;
    }
    if (op >= 0x80 && op <= 0x8F) {                 /* Jcc rel16/rel32 */
        t = sext(fetchsz(c, sz), sz);
        if (c->fault) return;
        if (cond(c, op & 15)) { jump_rel(c, t); T(10, 3); } else T(3, 1);
        return;
    }
    if (op >= 0x90 && op <= 0x9F) {                 /* SETcc r/m8 (the reg field is ignored) */
        modrm(c);
        set_rm(c, cond(c, op & 15) ? 1 : 0, 1);
        TRM(4, 5, 4, 3);
        return;
    }
    if (op >= 0xC8 && op <= 0xCF && IS486(c)) {    /* BSWAP r32; with a 16-bit operand size the result is undefined:
                                                     * the register's low half becomes 0 here (unverified) */
        int r = op & 7;
        v = gr32(c, r);
        if (sz == 4) sr32(c, r, (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24));
        else c->r[r] = 0;
        T(1, 1);
        return;
    }
    switch (op) {
    case 0x01: group7(c); return;
    case 0x06: c->cr0 &= ~X86_CR0_TS; T(5, 7); return;              /* CLTS */
    case 0x07:                                                      /* LOADALL (386): not modelled */
        c->stop = X86_STOP_UNIMPLEMENTED;
        c->bad_opcode = 0x0F;
        c->bad_opcode2 = op;
        c->ip = c->op_ip;
        return;
    case 0x08: case 0x09:                                           /* INVD, WBINVD (486): no cache to flush */
        if (!IS486(c)) break;
        T(4, 4);
        return;
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x26:
        mov_sysreg(c, op);
        return;
    case 0xA0: case 0xA8:                                           /* PUSH FS, PUSH GS */
        push_sreg(c, op == 0xA0 ? X86_FS : X86_GS);
        T(2, 3);
        return;
    case 0xA1: case 0xA9:                                           /* POP FS, POP GS */
        v = pop_sreg(c);
        if (!c->fault) c->s[op == 0xA1 ? X86_FS : X86_GS] = (uint16_t)v;
        T(7, 3);
        return;
    case 0xA3: case 0xAB: case 0xB3: case 0xBB: {                   /* BT, BTS, BTR, BTC r/m, r */
        modrm(c);
        int kind = op == 0xA3 ? 0 : op == 0xAB ? 1 : op == 0xB3 ? 2 : 3;
        if (!lock_ok(c, kind != 0)) return;
        bit_op(c, kind, getr(c, c->reg, sz), 1);
        return;
    }
    case 0xBA:                                                      /* group 8: BT, BTS, BTR, BTC r/m, imm8 */
        modrm(c);
        if (c->reg < 4) { fault(c, EX_UD); return; }
        if (!lock_ok(c, c->reg != 4)) return;
        t = fetch8(c);
        bit_op(c, c->reg - 4, t, 0);
        return;
    case 0xA4: case 0xA5: case 0xAC: case 0xAD:                     /* SHLD, SHRD r/m, r, imm8 / CL */
        modrm(c);
        v = get_rm(c, sz);
        t = (op & 1) ? (uint32_t)(uint8_t)CX : fetch8(c);
        set_rm(c, shxd(c, op < 0xA8, v, getr(c, c->reg, sz), t, sz), sz);
        TRM(3, 7, 3, 4);
        return;
    case 0xAF:                                                      /* IMUL r, r/m */
        modrm(c);
        v = get_rm(c, sz);
        if (!c->fault) setr(c, c->reg, imul_trunc(c, getr(c, c->reg, sz), v, sz), sz);
        T(sz == 2 ? 20 : 28, sz == 2 ? 20 : 28);
        return;
    case 0xB0: case 0xB1: {                                         /* CMPXCHG r/m, r (486) */
        if (!IS486(c)) break;
        int s = (op & 1) ? sz : 1;
        modrm(c);
        if (!lock_ok(c, 1)) return;
        v = get_rm(c, s);
        alu(c, 7, getr(c, X86_AX, s), v, s);
        if (c->flags & ZF) set_rm(c, getr(c, c->reg, s), s);
        else { set_rm(c, v, s); if (!c->fault) setr(c, X86_AX, v, s); }  /* a memory operand is written back either way */
        TRM(6, 7, 6, 7);
        return;
    }
    case 0xB2: load_far(c, X86_SS); return;                         /* LSS */
    case 0xB4: load_far(c, X86_FS); return;                         /* LFS */
    case 0xB5: load_far(c, X86_GS); return;                         /* LGS */
    case 0xB6: case 0xB7: case 0xBE: case 0xBF: {                   /* MOVZX, MOVSX */
        int s = (op & 1) ? 2 : 1;
        modrm(c);
        v = get_rm(c, s);
        if (op >= 0xBE) v = sext(v, s);
        if (!c->fault) setr(c, c->reg, v, sz);
        TRM(3, 6, 3, 3);
        return;
    }
    case 0xBC: case 0xBD:                                           /* BSF, BSR */
        modrm(c);
        bit_scan(c, op == 0xBD);
        return;
    case 0xC0: case 0xC1: {                                         /* XADD r/m, r (486) */
        if (!IS486(c)) break;
        int s = (op & 1) ? sz : 1;
        modrm(c);
        if (!lock_ok(c, 1)) return;
        v = get_rm(c, s);
        t = alu(c, 0, v, getr(c, c->reg, s), s);
        if (c->fault) return;
        setr(c, c->reg, v, s);
        set_rm(c, t, s);
        TRM(3, 4, 3, 4);
        return;
    }
    default:
        break;
    }
    /* 0F 00 (LLDT and friends), LAR, LSL: protected mode only; CPUID, RSM, the Pentium page and everything else:
     * not on these chips. */
    fault(c, EX_UD);
}

/* ------------------------------------------------------------------ one instruction */

/* A string instruction with a REP prefix: remember how it was decoded, so the following iterations need no fetch. */
static void rep_string(C *c, uint8_t op) {
    if (c->rep) {
        c->rep_op = op;
        c->rep_prefix = c->rep;
        c->rep_osz = c->osz;
        c->rep_asz = c->asz;
        c->rep_seg = c->seg_ovr;
        c->rep_ip = c->op_ip;
        c->rep_next = c->ip;
        c->rep_wrap = c->fetch_wrap;
    }
    string_op(c, op);
}

static void execute(C *c) {
    uint8_t op;
    if (c->in_rep && c->ip == c->rep_ip) {
        /* The next iteration of a REP string instruction runs from what was decoded: the 386 does not fetch the
         * instruction again, so a REP MOVS over its own bytes carries on (SingleStepTests vectors). An interrupt
         * between iterations ends this (the instruction is fetched afresh after the IRET). */
        c->seg_ovr = c->rep_seg;
        c->rep = c->rep_prefix;
        c->osz = c->rep_osz;
        c->asz = c->rep_asz;
        c->lock = 0;
        c->ilen = 0;
        c->op_ip = c->ip;
        c->ip = c->rep_next;
        c->fetch_wrap = c->rep_wrap;
        string_op(c, c->rep_op);
        return;
    }
    c->in_rep = 0;
    c->seg_ovr = -1;
    c->rep = 0;
    c->lock = 0;
    c->osz = 2;
    c->asz = 2;
    c->ilen = 0;
    c->op_ip = c->ip;
    for (;;) {
        op = fetch8(c);
        if (op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E) c->seg_ovr = (int8_t)((op >> 3) & 3);
        else if (op == 0x64 || op == 0x65) c->seg_ovr = (int8_t)(X86_FS + (op & 1));
        else if (op == 0x66) c->osz = 4;
        else if (op == 0x67) c->asz = 4;
        else if (op == 0xF0) c->lock = 1;
        else if (op == 0xF2 || op == 0xF3) c->rep = op;
        else break;
        if (!c->in_rep) T(0, 1);                    /* a prefix costs the 486 a clock (the 386 nothing); a REP
                                                     * iteration does not pay for its prefixes again */
    }
    if (c->fault) return;
    if (c->lock && !lock_candidate(op)) { fault(c, EX_UD); return; }
    if (c->rep && !is_string(op)) c->rep = 0;       /* REP on anything else is ignored */

    int sz = (op & 1) ? c->osz : 1;
    uint32_t v, t;
    switch (op) {
    /* ---- ALU: ADD OR ADC SBB AND SUB XOR CMP in six forms each */
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x18: case 0x19: case 0x1A: case 0x1B:
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x28: case 0x29: case 0x2A: case 0x2B:
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x38: case 0x39: case 0x3A: case 0x3B: {
        int aop = op >> 3;
        modrm(c);
        if (!lock_ok(c, 1)) break;
        if (op & 2) {
            v = get_rm(c, sz);
            v = alu(c, aop, getr(c, c->reg, sz), v, sz);
            if (aop != 7) setr(c, c->reg, v, sz);
            TRM(2, 6, 1, 2);
        } else {
            v = alu(c, aop, get_rm(c, sz), getr(c, c->reg, sz), sz);
            if (aop != 7) set_rm(c, v, sz);
            if (aop == 7) TRM(2, 5, 1, 2); else TRM(2, 7, 1, 3);
        }
        break;
    }
    case 0x04: case 0x05: case 0x0C: case 0x0D: case 0x14: case 0x15: case 0x1C: case 0x1D:
    case 0x24: case 0x25: case 0x2C: case 0x2D: case 0x34: case 0x35: case 0x3C: case 0x3D: {
        int aop = op >> 3;
        t = fetchsz(c, sz);
        v = alu(c, aop, getr(c, X86_AX, sz), t, sz);
        if (aop != 7 && !c->fault) setr(c, X86_AX, v, sz);
        T(2, 1);
        break;
    }
    case 0x06: case 0x0E: case 0x16: case 0x1E:     /* PUSH ES/CS/SS/DS */
        push_sreg(c, (op >> 3) & 3);
        T(2, 3);
        break;
    case 0x07: case 0x17: case 0x1F:                /* POP ES/SS/DS */
        v = pop_sreg(c);
        if (c->fault) break;
        c->s[(op >> 3) & 3] = (uint16_t)v;
        if (op == 0x17) c->shadow = 1;              /* only an SS load holds interrupts off on the 386 */
        T(7, 3);
        break;
    case 0x0F: op_0f(c); break;
    case 0x27: daa_das(c, 0); break;
    case 0x2F: daa_das(c, 1); break;
    case 0x37: aaa_aas(c, 0); break;
    case 0x3F: aaa_aas(c, 1); break;

    /* ---- INC/DEC/PUSH/POP r */
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        setr(c, op & 7, incdec(c, getr(c, op & 7, c->osz), op >= 0x48, c->osz), c->osz);
        T(2, 1);
        break;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        push(c, getr(c, op & 7, c->osz), c->osz);   /* PUSH SP pushes the value before the push, as on the 286 */
        T(2, 1);
        break;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        v = pop(c, c->osz);
        if (!c->fault) setr(c, op & 7, v, c->osz);
        T(4, 1);
        break;

    /* ---- 186/286 additions and their 32-bit forms */
    case 0x60: {                                    /* PUSHA / PUSHAD */
        /* The block is written from its lowest address up (DI first, at SP - 8 * size, AX last); an operand across the
         * FFFFh/0000h wrap raises #SS there, with the words below it already written and SP as it was. That is what
         * the SingleStepTests 80386 vectors show (the manual's list of SP values says #GP). */
        int s = c->osz;
        uint16_t base = (uint16_t)(SP - 8 * s);
        for (int i = 7; i >= 0 && !c->fault; i--)
            wrm(c, X86_SS, (uint16_t)(base + (7 - i) * s), getr(c, i, s), s);   /* getr(SP) is the SP before */
        if (c->fault) break;
        SP = base;
        T(18, 11);
        break;
    }
    case 0x61: {                                    /* POPA / POPAD (the saved SP is skipped) */
        int s = c->osz;
        for (int i = 7; i >= 0; i--) {
            v = pop(c, s);
            if (c->fault) break;
            /* The registers are loaded one by one: a stack fault part-way leaves those already popped loaded
             * (SingleStepTests 80386 vectors), so they are kept out of the rollback. */
            if (i != X86_SP) { setr(c, i, v, s); c->keep |= (uint8_t)(1u << i); }
            /* POPAD on a 16-bit stack: SP moves on as usual, but the upper half of ESP is loaded from the saved ESP
             * image (SingleStepTests 80386 vectors; POPA leaves it alone). */
            else if (s == 4) c->rh[X86_SP] = (uint16_t)(v >> 16);
        }
        T(24, 9);
        break;
    }
    case 0x62: {                                    /* BOUND r, m&m */
        int s = c->osz;
        modrm(c);
        if (c->mod == 3) { fault(c, EX_UD); break; }
        int32_t lo = (int32_t)sext(rdm(c, c->ea_seg, c->ea32, s), s);
        int32_t hi = (int32_t)sext(rdm(c, c->ea_seg, ea_plus(c, (uint32_t)s), s), s);
        int32_t x = (int32_t)sext(getr(c, c->reg, s), s);
        T(10, 7);
        if (!c->fault && (x < lo || x > hi)) fault(c, EX_BR);
        break;
    }
    case 0x68: push(c, fetchsz(c, c->osz), c->osz); T(2, 1); break;
    case 0x6A: push(c, sext(fetch8(c), 1), c->osz); T(2, 1); break;
    case 0x69: case 0x6B: {                         /* IMUL r, r/m, imm */
        int s = c->osz;
        modrm(c);
        v = get_rm(c, s);
        t = op == 0x69 ? fetchsz(c, s) : sext(fetch8(c), 1);
        if (!c->fault) setr(c, c->reg, imul_trunc(c, v, t, s), s);
        T(s == 2 ? 20 : 28, s == 2 ? 20 : 28);
        break;
    }
    case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        rep_string(c, op);
        break;

    /* ---- conditional jumps */
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:
        t = sext(fetch8(c), 1);
        if (c->fault) break;
        if (cond(c, op & 15)) { jump_rel(c, t); T(10, 3); } else T(3, 1);
        break;

    /* ---- group 1, TEST, XCHG, MOV */
    case 0x80: case 0x81: case 0x82: case 0x83:     /* 82 is 80 again (valid on the 386) */
        modrm(c);
        if (!lock_ok(c, c->reg != 7)) break;
        v = get_rm(c, sz);
        t = op == 0x81 ? fetchsz(c, sz) : op == 0x83 ? sext(fetch8(c), 1) : fetch8(c);
        v = alu(c, c->reg, v, t, sz);
        if (c->reg != 7) set_rm(c, v, sz);
        if (c->reg == 7) TRM(2, 5, 1, 2); else TRM(2, 7, 1, 3);
        break;
    case 0x84: case 0x85:
        modrm(c);
        alu(c, 4, get_rm(c, sz), getr(c, c->reg, sz), sz);
        TRM(2, 5, 1, 2);
        break;
    case 0x86: case 0x87:
        modrm(c);
        if (!lock_ok(c, 1)) break;
        v = get_rm(c, sz);
        set_rm(c, getr(c, c->reg, sz), sz);
        if (!c->fault) setr(c, c->reg, v, sz);
        TRM(3, 5, 3, 5);
        break;
    case 0x88: case 0x89:
        modrm(c);
        set_rm(c, getr(c, c->reg, sz), sz);
        TRM(2, 2, 1, 1);
        break;
    case 0x8A: case 0x8B:
        modrm(c);
        v = get_rm(c, sz);
        if (!c->fault) setr(c, c->reg, v, sz);
        TRM(2, 4, 1, 1);
        break;
    case 0x8C:                                      /* MOV r/m, sreg: a register gets the selector zero-extended to the
                                                     * operand size, memory always gets a word */
        modrm(c);
        if (c->reg > 5) { fault(c, EX_UD); break; }
        if (c->mod == 3) setr(c, c->rm, c->s[c->reg], c->osz);
        else wrm(c, c->ea_seg, c->ea32, c->s[c->reg], 2);
        TRM(2, 2, 3, 3);
        break;
    case 0x8D:                                      /* LEA */
        modrm(c);
        if (c->mod == 3) { fault(c, EX_UD); break; }
        setr(c, c->reg, c->ea32, c->osz);
        T(2, 1);
        break;
    case 0x8E:                                      /* MOV sreg, r/m16 (not CS) */
        modrm(c);
        if (c->reg > 5 || c->reg == X86_CS) { fault(c, EX_UD); break; }
        v = get_rm(c, 2);
        if (c->fault) break;
        c->s[c->reg] = (uint16_t)v;
        if (c->reg == X86_SS) c->shadow = 1;
        TRM(2, 5, 3, 3);
        break;
    case 0x8F:                                      /* POP r/m (reg 0 only) */
        modrm(c);
        if (c->reg != 0) { fault(c, EX_UD); break; }
        v = pop(c, c->osz);
        if (c->ea_esp) c->ea32 += (uint32_t)c->osz; /* [ESP+...] is computed after the increment */
        set_rm(c, v, c->osz);
        TRM(4, 5, 1, 6);
        break;

    case 0x90: T(3, 1); break;                      /* NOP */
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        v = getr(c, X86_AX, c->osz);
        setr(c, X86_AX, getr(c, op & 7, c->osz), c->osz);
        setr(c, op & 7, v, c->osz);
        T(3, 3);
        break;
    case 0x98:                                      /* CBW / CWDE */
        if (c->osz == 2) AX = (uint16_t)(int16_t)(int8_t)AX;
        else sr32(c, X86_AX, (uint32_t)(int32_t)(int16_t)AX);
        T(3, 3);
        break;
    case 0x99:                                      /* CWD / CDQ */
        if (c->osz == 2) DX = (AX & 0x8000) ? 0xFFFF : 0;
        else sr32(c, X86_DX, (gr32(c, X86_AX) & 0x80000000u) ? 0xFFFFFFFFu : 0);
        T(2, 3);
        break;
    case 0x9A: {                                    /* CALL far ptr16:16 / ptr16:32 */
        uint32_t off = fetchsz(c, c->osz);
        uint16_t sel = fetch16(c);
        if (c->fault) break;
        call_far(c, sel, off);
        T(20, 18);
        break;
    }
    case 0x9B:                                      /* WAIT: #NM when MP and TS are both set; no coprocessor is busy */
        if ((c->cr0 & (X86_CR0_MP | X86_CR0_TS)) == (X86_CR0_MP | X86_CR0_TS)) fault(c, EX_NM);
        T(6, 1);
        break;
    case 0x9C:                                      /* PUSHF / PUSHFD (RF and VM read 0 in the image) */
        push(c, c->osz == 4 ? eflags(c) : c->flags, c->osz);
        T(4, 4);
        break;
    case 0x9D:                                      /* POPF / POPFD */
        v = pop(c, c->osz);
        if (c->fault) break;
        if (c->osz == 4) load_flags32(c, v); else load_flags16(c, v);
        T(5, 9);
        break;
    case 0x9E:                                      /* SAHF */
        c->flags = (uint16_t)((c->flags & 0xFF00) | ((AX >> 8) & 0xD5) | 0x0002);
        T(3, 2);
        break;
    case 0x9F: sr8(c, 4, (uint8_t)c->flags); T(2, 3); break;       /* LAHF */

    /* ---- MOV with a direct offset (16 or 32 bits: the address size), string instructions, TEST acc */
    case 0xA0: case 0xA1:
        t = fetchsz(c, c->asz);
        v = rdm(c, dseg(c), t, sz);
        if (!c->fault) setr(c, X86_AX, v, sz);
        T(4, 1);
        break;
    case 0xA2: case 0xA3:
        t = fetchsz(c, c->asz);
        wrm(c, dseg(c), t, getr(c, X86_AX, sz), sz);
        T(2, 1);
        break;
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        rep_string(c, op);
        break;
    case 0xA8: case 0xA9:
        t = fetchsz(c, sz);
        alu(c, 4, getr(c, X86_AX, sz), t, sz);
        T(2, 1);
        break;

    /* ---- MOV reg, imm */
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        v = fetch8(c);
        if (!c->fault) sr8(c, op & 7, (uint8_t)v);
        T(2, 1);
        break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        v = fetchsz(c, c->osz);
        if (!c->fault) setr(c, op & 7, v, c->osz);
        T(2, 1);
        break;

    /* ---- shifts by an immediate, returns, LES/LDS, MOV r/m, imm, ENTER/LEAVE, INT, IRET */
    case 0xC0: case 0xC1:
        modrm(c);
        v = get_rm(c, sz);
        t = fetch8(c);
        set_rm(c, shift(c, c->reg, v, t, sz), sz);
        if (c->reg == 2 || c->reg == 3) TRM(9, 10, 8, 9); else TRM(3, 7, 2, 4);
        break;
    case 0xC2: case 0xC3:                           /* RET near (imm16) */
        t = op == 0xC2 ? fetch16(c) : 0;
        v = pop(c, c->osz);
        if (c->fault) break;
        if (c->osz == 4 && v > LIMIT) { fault(c, EX_GP); break; }
        set_ip(c, v);
        SP = (uint16_t)(SP + t);
        T(13, 5);
        break;
    case 0xC4: load_far(c, X86_ES); break;          /* LES */
    case 0xC5: load_far(c, X86_DS); break;          /* LDS */
    case 0xC6: case 0xC7:                           /* MOV r/m, imm (reg 0 only on the 386) */
        modrm(c);
        if (c->reg != 0) { fault(c, EX_UD); break; }
        t = fetchsz(c, sz);
        set_rm(c, t, sz);
        TRM(2, 2, 1, 1);
        break;
    case 0xC8: {                                    /* ENTER imm16, imm8 */
        int s = c->osz;
        uint16_t size = fetch16(c);
        uint8_t level = fetch8(c) & 31;
        if (c->fault) break;
        push(c, getr(c, X86_BP, s), s);
        uint16_t frame = SP;
        if (level) {
            for (int i = 1; i < level; i++) {
                BP = (uint16_t)(BP - s);
                push(c, rdm(c, X86_SS, BP, s), s);
            }
            push(c, frame, s);
        }
        if (c->fault) break;
        setr(c, X86_BP, frame, s);                  /* with a 32-bit operand size EBP gets the frame pointer zero-extended */
        SP = (uint16_t)(SP - size);
        T(level ? 12 + 4 * level : 10, level ? 17 + 3 * level : 14);
        break;
    }
    case 0xC9:                                      /* LEAVE */
        SP = BP;
        v = pop(c, c->osz);
        if (!c->fault) setr(c, X86_BP, v, c->osz);
        T(4, 5);
        break;
    case 0xCA: case 0xCB: {                         /* RETF (imm16) */
        t = op == 0xCA ? fetch16(c) : 0;
        v = pop(c, c->osz);
        uint16_t sel = (uint16_t)pop(c, c->osz);
        if (c->fault) break;
        if (c->osz == 4 && v > LIMIT) { fault(c, EX_GP); break; }
        set_ip(c, v);
        c->s[X86_CS] = sel;
        SP = (uint16_t)(SP + t);
        T(21, 13);
        break;
    }
    case 0xCC: soft_int(c, EX_BP, 1); break;
    case 0xCD: t = fetch8(c); soft_int(c, (int)t, 1); break;
    case 0xCE:
        if (c->flags & OF) soft_int(c, EX_OF, 1); else T(3, 3);
        break;
    case 0xCF: {                                    /* IRET / IRETD */
        v = pop(c, c->osz);
        uint16_t sel = (uint16_t)pop(c, c->osz);
        t = pop(c, c->osz);
        if (c->fault) break;
        if (c->osz == 4 && v > LIMIT) { fault(c, EX_GP); break; }
        set_ip(c, v);
        c->s[X86_CS] = sel;
        if (c->osz == 4) load_flags32(c, t); else load_flags16(c, t);
        T(22, 15);
        break;
    }

    /* ---- shifts, ASCII adjust, XLAT, ESC */
    case 0xD0: case 0xD1: case 0xD2: case 0xD3:
        modrm(c);
        v = get_rm(c, sz);
        set_rm(c, shift(c, c->reg, v, (op & 2) ? (uint8_t)CX : 1, sz), sz);
        if (c->reg == 2 || c->reg == 3) TRM(9, 10, 8, 9); else TRM(3, 7, 3, 4);
        break;
    case 0xD4: {                                    /* AAM imm8 */
        uint8_t base = fetch8(c);
        if (c->fault) break;
        T(17, 15);
        if (base == 0) { fault(c, EX_DE); break; }
        uint8_t al = (uint8_t)AX;
        AX = (uint16_t)((al / base) << 8 | (al % base));
        szp(c, (uint8_t)AX, 1);
        c->flags &= (uint16_t)~(CF | OF | AF);      /* undefined */
        break;
    }
    case 0xD5: {                                    /* AAD imm8: flags as the addition AL + AH * imm */
        uint8_t base = fetch8(c);
        if (c->fault) break;
        uint8_t r = (uint8_t)alu(c, 0, (uint8_t)AX, (uint8_t)((AX >> 8) * base), 1);
        AX = r;
        T(19, 14);
        break;
    }
    case 0xD6: sr8(c, 0, (c->flags & CF) ? 0xFF : 0x00); T(2, 2); break;   /* SALC (undocumented, present) */
    case 0xD7:                                      /* XLAT: [BX or EBX + AL] */
        t = c->asz == 4 ? gr32(c, X86_BX) + (uint8_t)AX : (uint16_t)(BX + (uint8_t)AX);
        v = rdm(c, dseg(c), t, 1);
        if (!c->fault) sr8(c, 0, (uint8_t)v);
        T(5, 4);
        break;
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        /* ESC with no coprocessor: #NM when CR0.EM or CR0.TS asks for it; otherwise the operand is decoded and nothing
         * happens, except that the status/control word stores (FNSTSW AX, FNSTSW/FNSTCW m16) deliver the FFFFh of an empty
         * coprocessor bus, so the usual FNINIT/FNSTSW probe sees no FPU whatever the program preset (unverified). */
        modrm(c);
        if (c->cr0 & (X86_CR0_EM | X86_CR0_TS)) { fault(c, EX_NM); break; }
        if (op == 0xDF && c->mod == 3 && c->reg == 4 && c->rm == 0) AX = 0xFFFF;
        else if (c->mod != 3 && c->reg == 7 && (op == 0xD9 || op == 0xDD)) wrm(c, c->ea_seg, c->ea32, 0xFFFF, 2);
        T(2, 2);
        break;

    /* ---- loops, I/O, CALL/JMP */
    case 0xE0: case 0xE1: case 0xE2: {              /* LOOPNE, LOOPE, LOOP: CX or ECX by the address size */
        t = sext(fetch8(c), 1);
        if (c->fault) break;
        set_areg(c, X86_CX, areg(c, X86_CX) - 1);
        int go = areg(c, X86_CX) != 0;
        if (op == 0xE0) go = go && !(c->flags & ZF);
        if (op == 0xE1) go = go && (c->flags & ZF);
        if (go) { jump_rel(c, t); T(13, 7); } else T(11, 6);
        break;
    }
    case 0xE3:                                      /* JCXZ / JECXZ (67 E3) */
        t = sext(fetch8(c), 1);
        if (c->fault) break;
        if (areg(c, X86_CX) == 0) { jump_rel(c, t); T(12, 8); } else T(5, 5);
        break;
    case 0xE4: case 0xE5:
        t = fetch8(c);
        v = io_in(c, (uint16_t)t, sz);
        if (!c->fault) setr(c, X86_AX, v, sz);
        T(12, 14);
        break;
    case 0xE6: case 0xE7:
        t = fetch8(c);
        io_out(c, (uint16_t)t, getr(c, X86_AX, sz), sz);
        T(10, 16);
        break;
    case 0xEC: case 0xED:
        v = io_in(c, DX, sz);
        if (!c->fault) setr(c, X86_AX, v, sz);
        T(13, 14);
        break;
    case 0xEE: case 0xEF:
        io_out(c, DX, getr(c, X86_AX, sz), sz);
        T(11, 16);
        break;
    case 0xE8:
        t = sext(fetchsz(c, c->osz), c->osz);
        if (c->fault) break;
        call_near(c, next_ip(c) + t);
        T(10, 3);
        break;
    case 0xE9:
        t = sext(fetchsz(c, c->osz), c->osz);
        if (c->fault) break;
        jump_rel(c, t);
        T(10, 3);
        break;
    case 0xEA: {
        uint32_t off = fetchsz(c, c->osz);
        uint16_t sel = fetch16(c);
        if (c->fault) break;
        jump_far(c, sel, off);
        T(15, 17);
        break;
    }
    case 0xEB:
        t = sext(fetch8(c), 1);
        if (c->fault) break;
        jump_rel(c, t);
        T(10, 3);
        break;

    /* ---- ICEBP, HLT, flag instructions, groups 3 to 5 */
    case 0xF1: soft_int(c, EX_DB, 0); break;        /* ICEBP: INT 1 without a bond-out chip */
    case 0xF4: c->halted = 1; T(5, 4); break;
    case 0xF5: c->flags ^= CF; T(2, 2); break;
    case 0xF6: case 0xF7:
        modrm(c);
        if (!lock_ok(c, c->reg == 2 || c->reg == 3)) break;
        switch (c->reg) {
        case 0: case 1:                             /* TEST r/m, imm (1 is an alias, valid on the 386) */
            v = get_rm(c, sz);
            t = fetchsz(c, sz);
            alu(c, 4, v, t, sz);
            TRM(2, 5, 1, 2);
            break;
        case 2:
            set_rm(c, ~get_rm(c, sz), sz);
            TRM(2, 6, 1, 3);
            break;
        case 3:
            v = get_rm(c, sz);
            set_rm(c, alu(c, 5, 0, v, sz), sz);
            TRM(2, 6, 1, 3);
            break;
        default:
            v = get_rm(c, sz);
            if (!c->fault) muldiv(c, c->reg, v, sz);
            break;
        }
        break;
    case 0xF8: c->flags &= (uint16_t)~CF; T(2, 2); break;
    case 0xF9: c->flags |= CF; T(2, 2); break;
    case 0xFA: c->flags &= (uint16_t)~IF; T(3, 5); break;
    case 0xFB: c->flags |= IF; c->shadow = 1; T(3, 5); break;
    case 0xFC: c->flags &= (uint16_t)~DF; T(2, 2); break;
    case 0xFD: c->flags |= DF; T(2, 2); break;
    case 0xFE:
        modrm(c);
        if (c->reg > 1) { fault(c, EX_UD); break; }
        if (!lock_ok(c, 1)) break;
        set_rm(c, incdec(c, get_rm(c, 1), c->reg, 1), 1);
        TRM(2, 6, 1, 3);
        break;
    case 0xFF:
        modrm(c);
        if (!lock_ok(c, c->reg <= 1)) break;
        switch (c->reg) {
        case 0: case 1:
            set_rm(c, incdec(c, get_rm(c, c->osz), c->reg, c->osz), c->osz);
            TRM(2, 6, 1, 3);
            break;
        case 2:                                     /* CALL near r/m */
            v = get_rm(c, c->osz);
            if (!c->fault) call_near(c, v);
            TRM(10, 13, 5, 5);
            break;
        case 3: case 5: {                           /* CALL far m16:16/m16:32, JMP far m16:16/m16:32 */
            if (c->mod == 3) { fault(c, EX_UD); break; }
            uint32_t off = rdm(c, c->ea_seg, c->ea32, c->osz);
            uint16_t sel = (uint16_t)rdm(c, c->ea_seg, ea_plus(c, (uint32_t)c->osz), 2);
            if (c->fault) break;
            if (c->reg == 3) { call_far(c, sel, off); T(25, 17); }
            else { jump_far(c, sel, off); T(15, 13); }
            break;
        }
        case 4:                                     /* JMP near r/m */
            v = get_rm(c, c->osz);
            if (!c->fault) jump_near(c, v);
            TRM(10, 13, 5, 5);
            break;
        case 6:
            v = get_rm(c, c->osz);
            push(c, v, c->osz);
            TRM(2, 5, 1, 4);
            break;
        default:
            fault(c, EX_UD);
            break;
        }
        break;
    default:
        /* 63 (ARPL: protected mode only), D8..DF are above; F1 is ICEBP; nothing else is left on the one-byte page. */
        fault(c, EX_UD);
        break;
    }
}

/* ------------------------------------------------------------------ public entry points (through x86cpu.c) */

/* The system registers after RESET: CR0 of a 386 without a 387 (ET clear) or of a 486 (CD, NW and ET set), DR6/DR7 with
 * their fixed bits, the IDTR on the real-mode vector table. */
void x86_386_init(X86Cpu *c) {
    c->cr0 = IS486(c) ? 0x60000010u : 0;
    c->cr2 = c->cr3 = 0;
    memset(c->dr, 0, sizeof c->dr);
    c->dr[6] = 0xFFFF0FF0u;
    c->dr[7] = IS486(c) ? 0x400u : 0;
    memset(c->tr, 0, sizeof c->tr);
    c->gdt_base = c->idt_base = 0;
    c->gdt_limit = 0xFFFF;
    c->idt_limit = 0x03FF;
}

void x86_386_reset(X86Cpu *c) {
    memset(c->r, 0, sizeof c->r);
    memset(c->rh, 0, sizeof c->rh);
    memset(c->s, 0, sizeof c->s);
    c->s[X86_CS] = 0xF000;
    c->ip = 0xFFF0;
    c->flags = 0x0002;
    c->flags_hi = 0;
    x86_386_init(c);
    /* DX holds the component ID: family 3 or 4 and a stepping (unverified values: 386DX D1 = 0308h, 486SX = 0422h). */
    DX = IS486(c) ? 0x0422 : 0x0308;
    c->halted = c->shadow = c->nmi_pending = c->in_rep = 0;
    c->stop = X86_STOP_NONE;
    c->fault = 0;
    c->fetch_wrap = 0;
}

void x86_386_interrupt(X86Cpu *c, uint8_t vector) { enter_int(c, vector, c->ip); }

int x86_386_step(X86Cpu *c) {
    uint64_t start = c->cycles;
    if (c->stop) { c->cycles += 1; return 1; }
    int shadow = c->shadow;
    c->shadow = 0;
    if (c->nmi_pending) {
        c->nmi_pending = 0;
        enter_int(c, EX_NMI, c->ip);
        return (int)(c->cycles - start);
    }
    if (!shadow && (c->flags & IF) && c->bus.intr_ack) {
        int vec = c->bus.intr_ack(c->bus.ctx);
        if (vec >= 0) {
            c->cycles += 4;                         /* INTA cycles on top of the entry */
            enter_int(c, vec, c->ip);
            return (int)(c->cycles - start);
        }
    }
    if (c->halted) { c->cycles += 1; return 1; }
    if (c->bus.trace) c->bus.trace(c->bus.ctx, c);
    int trap = (c->flags & TF) != 0;

    /* The snapshot an exception rolls back to. */
    uint16_t r[8], rh[8], s[6], flags = c->flags, flags_hi = c->flags_hi;
    memcpy(r, c->r, sizeof r);
    memcpy(rh, c->rh, sizeof rh);
    memcpy(s, c->s, sizeof s);
    c->fault = 0;
    c->keep = 0;
    execute(c);
    c->instructions++;
    if (c->fault) {
        int vec = c->fault - 1;
        for (int i = 0; i < 8; i++)
            if (!(c->keep >> i & 1)) { c->r[i] = r[i]; c->rh[i] = rh[i]; }
        memcpy(c->s, s, sizeof s);
        if (vec != EX_DE) { c->flags = flags; c->flags_hi = flags_hi; }
        c->ip = c->op_ip;                           /* enter_int() below replaces it (and clears fetch_wrap) */
        c->fault = 0;
        c->shadow = 0;
        c->in_rep = 0;
        c->halted = 0;
        enter_int(c, vec, c->op_ip);
        trap = 0;                                   /* the faulting instruction did not complete */
    }
    if (trap && !c->stop && !c->halted) enter_int(c, EX_DB, c->ip);
    if (c->cycles == start) c->cycles++;
    return (int)(c->cycles - start);
}
