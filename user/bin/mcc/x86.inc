/* x86.inc -- output sections, relocations and the x86-64 instruction encoder used by the code generator. */

enum { SEC_TEXT, SEC_DATA, SEC_RODATA, SEC_BSS, SEC_TDATA, SEC_TBSS, SEC_NUM };

typedef struct {
    u32 offset;
    u32 type;
    Obj *target;
    i64 addend;
} ObjReloc;

typedef struct {
    Buf data;
    u64 size; /* for .bss/.tbss: the size; for the others: data.len */
    u64 align;
    ObjReloc *rel;
    int nrel, caprel;
} OutSec;

static OutSec osec[SEC_NUM];

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
enum { CC_O, CC_NO, CC_B, CC_AE, CC_E, CC_NE, CC_BE, CC_A, CC_S, CC_NS, CC_P, CC_NP, CC_L, CC_GE, CC_LE, CC_G };

/* a memory operand: [base + index*scale + disp], or [rip + sym + disp] when base < 0 */
typedef struct {
    int base;
    int index;
    int scale;
    i32 disp;
    Obj *sym;
    int reloc;
} Mem;

static Buf *TB(void) {
    return &osec[SEC_TEXT].data;
}

static int cur_pos(void) {
    return (int)osec[SEC_TEXT].data.len;
}

static void e1(int b) {
    buf_u8(TB(), b);
}

static void e4(u32 v) {
    buf_u32(TB(), v);
}

static void e8(u64 v) {
    buf_u64(TB(), v);
}

static void add_reloc(int sec, u32 off, int type, Obj *target, i64 addend) {
    OutSec *s = &osec[sec];
    if (s->nrel == s->caprel) {
        s->caprel = s->caprel ? s->caprel * 2 : 64;
        s->rel = xrealloc(s->rel, (size_t)s->caprel * sizeof(ObjReloc));
    }
    ObjReloc *r = &s->rel[s->nrel++];
    r->offset = off;
    r->type = (u32)type;
    r->target = target;
    r->addend = addend;
}

static Mem mem_reg(int base, i32 disp) {
    Mem m;
    m.base = base;
    m.index = -1;
    m.scale = 1;
    m.disp = disp;
    m.sym = NULL;
    m.reloc = 0;
    return m;
}

static Mem mem_sym(Obj *sym, i32 addend) {
    Mem m;
    m.base = -1;
    m.index = -1;
    m.scale = 1;
    m.disp = addend;
    m.sym = sym;
    m.reloc = sym->is_function && !sym->is_definition ? R_X86_64_PLT32 : R_X86_64_PC32;
    return m;
}

/* ---- encoding primitives */

static void rex(int w, int reg, int index, int base, int force) {
    int v = 0x40 | (w ? 8 : 0) | ((reg & 8) ? 4 : 0) | ((index & 8) ? 2 : 0) | ((base & 8) ? 1 : 0);
    if (v != 0x40 || force) e1(v);
}

static void eop(int op) {
    if (op > 0xFFFF) {
        e1(op >> 16);
        e1((op >> 8) & 0xFF);
        e1(op & 0xFF);
    } else if (op > 0xFF) {
        e1(op >> 8);
        e1(op & 0xFF);
    } else {
        e1(op);
    }
}

static void put_mem(int reg, Mem m, int imm_after) {
    int r = reg & 7;
    if (m.base < 0) {
        e1((r << 3) | 5);
        int pos = cur_pos();
        e4(0);
        if (m.sym) add_reloc(SEC_TEXT, (u32)pos, m.reloc, m.sym, (i64)m.disp - 4 - imm_after);
        return;
    }
    int b = m.base & 7;
    bool need_sib = (b == 4) || m.index >= 0;
    int mod;
    if (m.disp == 0 && b != 5) mod = 0;
    else if (m.disp >= -128 && m.disp <= 127) mod = 1;
    else mod = 2;
    e1((mod << 6) | (r << 3) | (need_sib ? 4 : b));
    if (need_sib) {
        int idx = m.index >= 0 ? (m.index & 7) : 4;
        int ss = m.scale == 8 ? 3 : m.scale == 4 ? 2 : m.scale == 2 ? 1 : 0;
        e1((ss << 6) | (idx << 3) | b);
    }
    if (mod == 1) e1(m.disp & 0xFF);
    else if (mod == 2) e4((u32)m.disp);
}

/* op reg, rm  (both registers) */
static void i_rr(int pfx, int w, int op, int reg, int rm, int byte_force) {
    if (pfx) e1(pfx);
    rex(w, reg, 0, rm, byte_force);
    eop(op);
    e1(0xC0 | ((reg & 7) << 3) | (rm & 7));
}

/* op reg, [mem] */
static void i_rm(int pfx, int w, int op, int reg, Mem m, int imm_after, int byte_force) {
    if (pfx) e1(pfx);
    rex(w, reg, m.index >= 0 ? m.index : 0, m.base >= 0 ? m.base : 0, byte_force);
    eop(op);
    put_mem(reg, m, imm_after);
}

/* ---- integer instructions */

static void push_r(int r) {
    if (r >= 8) e1(0x41);
    e1(0x50 + (r & 7));
}

static void pop_r(int r) {
    if (r >= 8) e1(0x41);
    e1(0x58 + (r & 7));
}

static void mov_rr(int dst, int src) {
    if (dst != src) i_rr(0, 1, 0x89, src, dst, 0);
}

static void mov_rr32(int dst, int src) {
    i_rr(0, 0, 0x89, src, dst, 0);
}

static void mov_ri(int dst, i64 v) {
    if (v >= 0 && v <= 0xFFFFFFFFLL) {
        if (dst >= 8) e1(0x41);
        e1(0xB8 + (dst & 7));
        e4((u32)v);
    } else if (v >= -2147483648LL && v < 0) {
        rex(1, 0, 0, dst, 0);
        e1(0xC7);
        e1(0xC0 | (dst & 7));
        e4((u32)(i32)v);
    } else {
        rex(1, 0, 0, dst, 0);
        e1(0xB8 + (dst & 7));
        e8((u64)v);
    }
}

static void lea(int dst, Mem m) {
    i_rm(0, 1, 0x8D, dst, m, 0, 0);
}

/* ALU operations: opc is the "op r/m, r" opcode (add 01, or 09, and 21, sub 29, xor 31, cmp 39) */
enum { A_ADD = 0, A_OR = 1, A_AND = 4, A_SUB = 5, A_XOR = 6, A_CMP = 7 };

static void alu_rr(int kind, int w, int dst, int src) {
    static const int opc[8] = {0x01, 0x09, 0, 0, 0x21, 0x29, 0x31, 0x39};
    i_rr(0, w, opc[kind], src, dst, 0);
}

static void alu_ri(int kind, int w, int reg, i64 imm) {
    if (imm >= -128 && imm <= 127) {
        rex(w, 0, 0, reg, 0);
        e1(0x83);
        e1(0xC0 | (kind << 3) | (reg & 7));
        e1((int)(imm & 0xFF));
    } else {
        rex(w, 0, 0, reg, 0);
        e1(0x81);
        e1(0xC0 | (kind << 3) | (reg & 7));
        e4((u32)(i32)imm);
    }
}

static void alu_rm(int kind, int w, int reg, Mem m) {
    /* op reg, [m]: the "r, r/m" forms are opc + 2 */
    static const int opc[8] = {0x03, 0x0B, 0, 0, 0x23, 0x2B, 0x33, 0x3B};
    i_rm(0, w, opc[kind], reg, m, 0, 0);
}

static void sub_rsp(int n) {
    if (n) alu_ri(A_SUB, 1, RSP, n);
}

static void add_rsp(int n) {
    if (n) alu_ri(A_ADD, 1, RSP, n);
}

static void imul_rr(int w, int dst, int src) {
    i_rr(0, w, 0x0FAF, dst, src, 0);
}

static void unary_r(int w, int ext, int reg) { /* F7 /ext: not 2, neg 3, mul 4, imul 5, div 6, idiv 7 */
    rex(w, 0, 0, reg, 0);
    e1(0xF7);
    e1(0xC0 | (ext << 3) | (reg & 7));
}

static void shift_cl(int w, int ext, int reg) { /* D3 /ext: shl 4, shr 5, sar 7 */
    rex(w, 0, 0, reg, 0);
    e1(0xD3);
    e1(0xC0 | (ext << 3) | (reg & 7));
}

static void shift_ri(int w, int ext, int reg, int n) {
    rex(w, 0, 0, reg, 0);
    e1(0xC1);
    e1(0xC0 | (ext << 3) | (reg & 7));
    e1(n);
}

static void test_rr(int w, int a, int b) {
    i_rr(0, w, 0x85, b, a, 0);
}

static void setcc(int cc, int reg) {
    rex(0, 0, 0, reg, reg >= 4 && reg < 8);
    e1(0x0F);
    e1(0x90 + cc);
    e1(0xC0 | (reg & 7));
}

static void movzx_rr8(int dst, int src) { /* movzx dst32, src8 */
    i_rr(0, 0, 0x0FB6, dst, src, src >= 4 && src < 8);
}

static void movsx_rr8(int dst, int src, int w) {
    i_rr(0, w, 0x0FBE, dst, src, src >= 4 && src < 8);
}

static void movzx_rr16(int dst, int src) {
    i_rr(0, 0, 0x0FB7, dst, src, 0);
}

static void movsx_rr16(int dst, int src, int w) {
    i_rr(0, w, 0x0FBF, dst, src, 0);
}

static void movsxd_rr(int dst, int src) {
    i_rr(0, 1, 0x63, dst, src, 0);
}

static void cqo(void) {
    e1(0x48);
    e1(0x99);
}

static void cdq(void) {
    e1(0x99);
}

static void call_r(int reg) {
    rex(0, 0, 0, reg, 0);
    e1(0xFF);
    e1(0xD0 | (reg & 7));
}

static void ret_(void) {
    e1(0xC3);
}

static void leave_(void) {
    e1(0xC9);
}

static void ud2(void) {
    e1(0x0F);
    e1(0x0B);
}

static void rep_movsb(void) {
    e1(0xF3);
    e1(0xA4);
}

static void rep_stosb(void) {
    e1(0xF3);
    e1(0xAA);
}

/* loads of a given size into dst; the result is zero- or sign-extended according to is_unsigned */
static void load_mem(int dst, Mem m, int size, bool is_unsigned) {
    switch (size) {
    case 1:
        i_rm(0, 0, is_unsigned ? 0x0FB6 : 0x0FBE, dst, m, 0, 0);
        break;
    case 2:
        i_rm(0, 0, is_unsigned ? 0x0FB7 : 0x0FBF, dst, m, 0, 0);
        break;
    case 4:
        if (is_unsigned) i_rm(0, 0, 0x8B, dst, m, 0, 0);
        else i_rm(0, 1, 0x63, dst, m, 0, 0);
        break;
    case 8:
        i_rm(0, 1, 0x8B, dst, m, 0, 0);
        break;
    default:
        ICE();
    }
}

static void store_mem(Mem m, int src, int size) {
    switch (size) {
    case 1:
        i_rm(0, 0, 0x88, src, m, 0, src >= 4 && src < 8);
        break;
    case 2:
        i_rm(0x66, 0, 0x89, src, m, 0, 0);
        break;
    case 4:
        i_rm(0, 0, 0x89, src, m, 0, 0);
        break;
    case 8:
        i_rm(0, 1, 0x89, src, m, 0, 0);
        break;
    default:
        ICE();
    }
}

static void mov_mi32(Mem m, int size, i32 imm) {
    /* mov size [m], imm */
    if (size == 2) e1(0x66);
    rex(size == 8, 0, m.index >= 0 ? m.index : 0, m.base >= 0 ? m.base : 0, 0);
    e1(size == 1 ? 0xC6 : 0xC7);
    put_mem(0, m, size == 1 ? 1 : size == 2 ? 2 : 4);
    if (size == 1) e1(imm & 0xFF);
    else if (size == 2) {
        e1(imm & 0xFF);
        e1((imm >> 8) & 0xFF);
    } else e4((u32)imm);
}

/* ---- SSE */

static void sse_rr(int pfx, int op, int reg, int rm) {
    i_rr(pfx, 0, op, reg, rm, 0);
}

static void sse_rm(int pfx, int op, int reg, Mem m) {
    i_rm(pfx, 0, op, reg, m, 0, 0);
}

static void movss_load(int x, Mem m) { sse_rm(0xF3, 0x0F10, x, m); }
static void movsd_load(int x, Mem m) { sse_rm(0xF2, 0x0F10, x, m); }
static void movss_store(Mem m, int x) { sse_rm(0xF3, 0x0F11, x, m); }
static void movsd_store(Mem m, int x) { sse_rm(0xF2, 0x0F11, x, m); }
static void movaps_rr(int dst, int src) { sse_rr(0, 0x0F28, dst, src); }

static void movq_xmm_r(int x, int r) { /* movq xmm, r64 */
    e1(0x66);
    rex(1, x, 0, r, 0);
    e1(0x0F);
    e1(0x6E);
    e1(0xC0 | ((x & 7) << 3) | (r & 7));
}

static void movq_r_xmm(int r, int x) { /* movq r64, xmm */
    e1(0x66);
    rex(1, x, 0, r, 0);
    e1(0x0F);
    e1(0x7E);
    e1(0xC0 | ((x & 7) << 3) | (r & 7));
}

static void movd_xmm_r(int x, int r) {
    e1(0x66);
    rex(0, x, 0, r, 0);
    e1(0x0F);
    e1(0x6E);
    e1(0xC0 | ((x & 7) << 3) | (r & 7));
}

static void movd_r_xmm(int r, int x) {
    e1(0x66);
    rex(0, x, 0, r, 0);
    e1(0x0F);
    e1(0x7E);
    e1(0xC0 | ((x & 7) << 3) | (r & 7));
}

static void cvtsi2fp(bool dbl, int w, int x, int r) { /* cvtsi2ss/sd xmm, r32/r64 */
    i_rr(dbl ? 0xF2 : 0xF3, w, 0x0F2A, x, r, 0);
}

static void cvttfp2si(bool dbl, int w, int r, int x) { /* cvttss2si/cvttsd2si r, xmm */
    i_rr(dbl ? 0xF2 : 0xF3, w, 0x0F2C, r, x, 0);
}

static void ucomi(bool dbl, int a, int b) { /* ucomiss/ucomisd a, b */
    i_rr(dbl ? 0x66 : 0, 0, 0x0F2E, a, b, 0);
}

static void xorp(bool dbl, int a, int b) {
    i_rr(dbl ? 0x66 : 0, 0, 0x0F57, a, b, 0);
}

/* ---- labels and jumps inside a function */

typedef struct {
    int pos;
    int label;
} Fixup;

static int *label_pos;
static int label_cap;
static Fixup *fixups;
static int nfixups, capfixups;

static void label_ensure(int id) {
    if (id < label_cap) return;
    int ncap = label_cap ? label_cap : 1024;
    while (ncap <= id) ncap *= 2;
    label_pos = xrealloc(label_pos, (size_t)ncap * sizeof(int));
    for (int i = label_cap; i < ncap; i++) label_pos[i] = -1;
    label_cap = ncap;
}

static int last_jmp_pos = -1;   /* where the latest unconditional jump starts */
static int last_jmp_label;
static int last_label_pos = -1; /* where the latest label was put */

static void label_set(int id) {
    label_ensure(id);
    /* a jump to the very next instruction is dropped (the usual `return` at the end of a function) */
    if (last_jmp_pos >= 0 && last_jmp_pos + 5 == cur_pos() && last_jmp_label == id && last_label_pos != cur_pos() && nfixups > 0 &&
        fixups[nfixups - 1].pos == last_jmp_pos + 1) {
        TB()->len -= 5;
        nfixups--;
        last_jmp_pos = -1;
    }
    label_pos[id] = cur_pos();
    last_label_pos = cur_pos();
}

static void add_fixup(int pos, int label) {
    if (nfixups == capfixups) {
        capfixups = capfixups ? capfixups * 2 : 256;
        fixups = xrealloc(fixups, (size_t)capfixups * sizeof(Fixup));
    }
    fixups[nfixups].pos = pos;
    fixups[nfixups].label = label;
    nfixups++;
}

static void jmp_label(int id) {
    last_jmp_pos = cur_pos();
    last_jmp_label = id;
    e1(0xE9);
    add_fixup(cur_pos(), id);
    e4(0);
}

static void jcc_label(int cc, int id) {
    e1(0x0F);
    e1(0x80 + cc);
    add_fixup(cur_pos(), id);
    e4(0);
}

static void resolve_fixups(void) {
    for (int i = 0; i < nfixups; i++) {
        int id = fixups[i].label;
        if (id >= label_cap || label_pos[id] < 0) fatal("internal error: jump to an undefined label %d", id);
        put32(TB()->p + fixups[i].pos, (u32)(label_pos[id] - (fixups[i].pos + 4)));
    }
    nfixups = 0;
}

static void call_sym(Obj *fn) {
    e1(0xE8);
    add_reloc(SEC_TEXT, (u32)cur_pos(), fn->is_definition && fn->is_static ? R_X86_64_PC32 : R_X86_64_PLT32, fn, -4);
    e4(0);
}
