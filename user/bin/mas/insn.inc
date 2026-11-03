/* insn.inc -- the x86-64 instruction encoder: the instructions the kernel's assembly files use and the usual company they keep, with
 * the encodings NASM chooses (89 /r rather than 8B /r for register to register moves, 83 /r ib for small immediates, the accumulator
 * forms 05 id / A9 id when an immediate does not fit a byte, mov r32, imm32 for a 64-bit load of a small positive number, short
 * branches whenever the target is close). */

enum {
    R_X86_64_NONE = 0, R_X86_64_64 = 1, R_X86_64_PC32 = 2, R_X86_64_32 = 10, R_X86_64_32S = 11, R_X86_64_16 = 12, R_X86_64_8 = 14
};

typedef struct {
    int off, size, pcrel, sign;
    Val v;
} Fixup;

typedef struct {
    u8 b[32];
    int n;
    Fixup fx[3];
    int nfx;
} Enc;

static void eb(Enc *e, u32 v) {
    if (e->n >= (int)sizeof e->b) fatal("instruction too long");
    e->b[e->n++] = (u8)v;
}

static void ele(Enc *e, u64 v, int bytes) {
    for (int i = 0; i < bytes; i++) eb(e, (u32)(v >> (8 * i)));
}

/* a field of `size` bytes whose value is known only when the layout is: constant, or a relocation, or a position-relative number */
static void efix(Enc *e, int size, int pcrel, int sign, Val v) {
    if (e->nfx >= 3) fatal("too many fixups in one instruction");
    Fixup *f = &e->fx[e->nfx++];
    f->off = e->n;
    f->size = size;
    f->pcrel = pcrel;
    f->sign = sign;
    f->v = v;
    ele(e, 0, size);
}

static int fits8(i64 v) { return v >= -128 && v <= 127; }
static int fits32s(i64 v) { return v >= -2147483648LL && v <= 2147483647LL; }
static int fits32u(i64 v) { return v >= 0 && v <= 0xFFFFFFFFLL; }

static Val ev(Item *it, Tok *t, int n) {
    if (n == 0) return v_const(0);
    ectx.sec = it->sec;
    ectx.pos = it->pos;
    ectx.pp_only = 0;
    return eval_range(t, 0, n);
}

static Val ev_op(Item *it, Op *o) {
    return ev(it, o->e, o->ne);
}

static int op_bits(Op *o) {
    if (o->kind == 1) return o->bits;
    return o->msize * 8;
}

static int needs_byte_rex(Op *o) {
    return o->kind == 1 && o->bits == 8 && !o->hi8 && o->reg >= 4 && o->reg < 8;
}

/* [66] [REX] opcode modrm [sib] [disp]: `reg` is the ModRM reg field (a register number or an opcode extension), rm a register or memory operand */
static void emit_rm(Item *it, Enc *e, int p66, int w, int forcerex, const u8 *opc, int nopc, int reg, Op *rm) {
    int rex = (w ? 8 : 0) | ((reg & 8) ? 4 : 0);
    if (rm->kind == 1) {
        if (rm->reg & 8) rex |= 1;
    } else {
        if (rm->index >= 0 && (rm->index & 8)) rex |= 2;
        if (rm->base >= 0 && (rm->base & 8)) rex |= 1;
    }
    if (p66) eb(e, 0x66);
    if (rex || forcerex) {
        eb(e, 0x40 | rex);
    }
    for (int i = 0; i < nopc; i++) eb(e, opc[i]);
    int r = reg & 7;
    if (rm->kind == 1) {
        eb(e, 0xC0 | (r << 3) | (rm->reg & 7));
        return;
    }
    Val d = ev_op(it, rm);
    int use_rel = rm->rel >= 0 ? rm->rel : default_rel;
    if (d.kind == V_CONST) use_rel = 0;                         /* a plain number is an absolute address even under `default rel` (NASM warns) */
    if (rm->base < 0 && rm->index < 0) {
        if (use_rel) {
            eb(e, (r << 3) | 5);
            efix(e, 4, 1, 1, d);
        } else {
            eb(e, (r << 3) | 4);
            eb(e, 0x25);
            efix(e, 4, 0, 1, d);
        }
        return;
    }
    int mod, base = rm->base;
    int disp_none = d.kind == V_CONST && d.num == 0 && base >= 0 && (base & 7) != 5;
    if (base < 0) mod = 0;                                      /* [index*scale + disp32] */
    else if (disp_none) mod = 0;
    else if (d.kind == V_CONST && fits8(d.num)) mod = 1;
    else mod = 2;
    int sib = rm->index >= 0 || base < 0 || (base & 7) == 4;
    eb(e, (mod << 6) | (r << 3) | (sib ? 4 : (base & 7)));
    if (sib) {
        int ss = rm->scale == 8 ? 3 : rm->scale == 4 ? 2 : rm->scale == 2 ? 1 : 0;
        int idx = rm->index >= 0 ? (rm->index & 7) : 4;
        eb(e, (ss << 6) | (idx << 3) | (base < 0 ? 5 : (base & 7)));
    }
    if (base < 0) efix(e, 4, 0, 1, d);
    else if (mod == 1) eb(e, (u32)d.num);
    else if (mod == 2) efix(e, 4, 0, 1, d);
}

/* opcode with a register in its low three bits: B8+r, 50+r ... */
static void emit_plus_reg(Enc *e, int p66, int w, int forcerex, u32 opc, int reg) {
    int rex = (w ? 8 : 0) | ((reg & 8) ? 1 : 0);
    if (p66) eb(e, 0x66);
    if (rex || forcerex) eb(e, 0x40 | rex);
    eb(e, opc + (u32)(reg & 7));
}

static void emit_imm(Item *it, Enc *e, Op *o, int size, int sign) {
    Val v = ev_op(it, o);
    efix(e, size, 0, sign, v);
}

/* the size of the operation: from a register operand, else from the size keyword on a memory operand */
static int op_size(Item *it, Op *a, Op *b) {
    int sa = a ? op_bits(a) : 0, sb = b ? op_bits(b) : 0;
    if (sa && sb && sa != sb && !(b && b->kind == 2)) fatal("mismatch in operand sizes");
    if (sa) return sa;
    if (sb && b->kind != 2) return sb;
    (void)it;
    fatal("operation size not specified");
}

static const struct {
    const char *name;
    u8 bytes[4];
    int len;
} zero_ops[] = {
    {"nop", {0x90}, 1}, {"ret", {0xC3}, 1}, {"retq", {0xC3}, 1}, {"ud2", {0x0F, 0x0B}, 2}, {"syscall", {0x0F, 0x05}, 2}, {"sysret", {0x0F, 0x07}, 2},
    {"sysretq", {0x48, 0x0F, 0x07}, 3}, {"cli", {0xFA}, 1}, {"sti", {0xFB}, 1}, {"cld", {0xFC}, 1}, {"std", {0xFD}, 1}, {"hlt", {0xF4}, 1},
    {"iretq", {0x48, 0xCF}, 2}, {"iretd", {0xCF}, 1}, {"iret", {0xCF}, 1}, {"leave", {0xC9}, 1}, {"pause", {0xF3, 0x90}, 2}, {"cpuid", {0x0F, 0xA2}, 2},
    {"rdtsc", {0x0F, 0x31}, 2}, {"rdtscp", {0x0F, 0x01, 0xF9}, 3}, {"rdmsr", {0x0F, 0x32}, 2}, {"wrmsr", {0x0F, 0x30}, 2}, {"int3", {0xCC}, 1},
    {"swapgs", {0x0F, 0x01, 0xF8}, 3}, {"lfence", {0x0F, 0xAE, 0xE8}, 3}, {"mfence", {0x0F, 0xAE, 0xF0}, 3}, {"sfence", {0x0F, 0xAE, 0xF8}, 3},
    {"pushfq", {0x9C}, 1}, {"popfq", {0x9D}, 1}, {"cqo", {0x48, 0x99}, 2}, {"cdq", {0x99}, 1}, {"cdqe", {0x48, 0x98}, 2}, {"cwde", {0x98}, 1},
    {"cbw", {0x66, 0x98}, 2}, {"lahf", {0x9F}, 1}, {"sahf", {0x9E}, 1}, {"clc", {0xF8}, 1}, {"stc", {0xF9}, 1}, {"cmc", {0xF5}, 1},
    {"xgetbv", {0x0F, 0x01, 0xD0}, 3}, {"wbinvd", {0x0F, 0x09}, 2}, {"invd", {0x0F, 0x08}, 2},
    {"movsb", {0xA4}, 1}, {"movsw", {0x66, 0xA5}, 2}, {"movsd", {0xA5}, 1}, {"movsq", {0x48, 0xA5}, 2},
    {"stosb", {0xAA}, 1}, {"stosw", {0x66, 0xAB}, 2}, {"stosd", {0xAB}, 1}, {"stosq", {0x48, 0xAB}, 2},
    {"lodsb", {0xAC}, 1}, {"lodsw", {0x66, 0xAD}, 2}, {"lodsd", {0xAD}, 1}, {"lodsq", {0x48, 0xAD}, 2},
    {"scasb", {0xAE}, 1}, {"scasw", {0x66, 0xAF}, 2}, {"scasd", {0xAF}, 1}, {"scasq", {0x48, 0xAF}, 2},
    {"cmpsb", {0xA6}, 1}, {"cmpsw", {0x66, 0xA7}, 2}, {"cmpsd", {0xA7}, 1}, {"cmpsq", {0x48, 0xA7}, 2},
};

static const char *cc_names[16] = {"o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"};

static int cc_from_name(const char *s) {
    static const struct {
        const char *n;
        int cc;
    } alias[] = {{"c", 2}, {"nae", 2}, {"nc", 3}, {"nb", 3}, {"z", 4}, {"nz", 5}, {"na", 6}, {"nbe", 7}, {"pe", 10}, {"po", 11}, {"nge", 12}, {"nl", 13}, {"ng", 14}, {"nle", 15}};
    for (int i = 0; i < 16; i++)
        if (!icmp(s, cc_names[i])) return i;
    for (size_t i = 0; i < sizeof alias / sizeof alias[0]; i++)
        if (!icmp(s, alias[i].n)) return alias[i].cc;
    return -1;
}

static void enc_branch(Item *it, Enc *e, int is_call, int cc) {
    Op *o = &it->op[0];
    if (it->nops != 1) fatal("'%s' takes one operand", it->mn);
    if (o->kind == 1 || o->kind == 3) {
        if (cc >= 0) fatal("conditional jumps need a label");
        static const u8 ff[1] = {0xFF};
        if (o->kind == 1 && o->bits != 64) fatal("an indirect jump or call needs a 64-bit register");
        emit_rm(it, e, 0, 0, 0, ff, 1, is_call ? 2 : 4, o);
        return;
    }
    Val v = ev_op(it, o);
    int target_here = v.kind == V_UNK || (v.kind == V_SEC && v.sec == it->sec);
    if (is_call) {
        eb(e, 0xE8);
        efix(e, 4, 1, 1, v);
        return;
    }
    int want_short = o->is_short || (!it->near_ && !o->is_near && target_here);
    if (want_short) {
        eb(e, cc < 0 ? 0xEB : 0x70 + (u32)cc);
        efix(e, 1, 1, 1, v);
        return;
    }
    if (cc < 0) eb(e, 0xE9);
    else {
        eb(e, 0x0F);
        eb(e, 0x80 + (u32)cc);
    }
    efix(e, 4, 1, 1, v);
}

static const struct {
    const char *name;
    int ext;
} shift_ops[] = {{"rol", 0}, {"ror", 1}, {"rcl", 2}, {"rcr", 3}, {"shl", 4}, {"sal", 4}, {"shr", 5}, {"sar", 7}};

static const char *alu_names[8] = {"add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"};

static void enc_insn(Item *it, Enc *e) {
    const char *m = it->mn;
    Op *o = it->op;
    int n = it->nops;
    if (it->lock) eb(e, 0xF0);
    if (it->rep) eb(e, (u32)it->rep);

    if (n == 0) {
        for (size_t i = 0; i < sizeof zero_ops / sizeof zero_ops[0]; i++)
            if (!strcmp(m, zero_ops[i].name)) {
                for (int k = 0; k < zero_ops[i].len; k++) eb(e, zero_ops[i].bytes[k]);
                return;
            }
        fatal("unknown instruction '%s' (or it needs operands)", m);
    }

    /* ---- mov */
    if (!strcmp(m, "mov")) {
        if (n != 2) fatal("mov takes two operands");
        Op *d = &o[0], *s = &o[1];
        if (d->kind == 1 && s->kind == 1) {
            if (d->bits != s->bits) fatal("mismatch in operand sizes");
            u8 opc[1] = {(u8)(d->bits == 8 ? 0x88 : 0x89)};
            if ((needs_byte_rex(d) || needs_byte_rex(s)) && (d->hi8 || s->hi8)) fatal("cannot use ah..bh with a REX prefix");
            emit_rm(it, e, d->bits == 16, d->bits == 64, needs_byte_rex(d) || needs_byte_rex(s), opc, 1, s->reg, d);
            return;
        }
        if (d->kind == 1 && s->kind == 3) {
            if (s->msize && s->msize * 8 != d->bits) fatal("mismatch in operand sizes");
            u8 opc[1] = {(u8)(d->bits == 8 ? 0x8A : 0x8B)};
            emit_rm(it, e, d->bits == 16, d->bits == 64, needs_byte_rex(d), opc, 1, d->reg, s);
            return;
        }
        if (d->kind == 3 && s->kind == 1) {
            if (d->msize && d->msize * 8 != s->bits) fatal("mismatch in operand sizes");
            u8 opc[1] = {(u8)(s->bits == 8 ? 0x88 : 0x89)};
            emit_rm(it, e, s->bits == 16, s->bits == 64, needs_byte_rex(s), opc, 1, s->reg, d);
            return;
        }
        if (d->kind == 1 && s->kind == 2) {
            Val v = ev_op(it, s);
            int forcerex = needs_byte_rex(d);
            if (d->bits == 8) {
                emit_plus_reg(e, 0, 0, forcerex, 0xB0, d->reg);
                efix(e, 1, 0, 0, v);
            } else if (d->bits == 16) {
                emit_plus_reg(e, 1, 0, 0, 0xB8, d->reg);
                efix(e, 2, 0, 0, v);
            } else if (d->bits == 32) {
                emit_plus_reg(e, 0, 0, 0, 0xB8, d->reg);
                efix(e, 4, 0, 0, v);
            } else if (v.kind == V_CONST && fits32u(v.num)) {
                emit_plus_reg(e, 0, 0, 0, 0xB8, d->reg);                    /* mov r32, imm32 zero-extends: one byte shorter than the REX.W form */
                efix(e, 4, 0, 0, v);
            } else if (v.kind == V_CONST && fits32s(v.num)) {
                static const u8 c7[1] = {0xC7};
                emit_rm(it, e, 0, 1, 0, c7, 1, 0, d);
                efix(e, 4, 0, 1, v);
            } else {
                emit_plus_reg(e, 0, 1, 0, 0xB8, d->reg);                    /* movabs */
                efix(e, 8, 0, 0, v);
            }
            return;
        }
        if (d->kind == 3 && s->kind == 2) {
            int bits = d->msize * 8;
            if (!bits) fatal("operation size not specified");
            u8 opc[1] = {(u8)(bits == 8 ? 0xC6 : 0xC7)};
            emit_rm(it, e, bits == 16, bits == 64, 0, opc, 1, 0, d);
            emit_imm(it, e, s, bits == 8 ? 1 : bits == 16 ? 2 : 4, bits == 64);
            return;
        }
        fatal("unsupported operands for mov");
    }

    /* ---- lea */
    if (!strcmp(m, "lea")) {
        if (n != 2 || o[0].kind != 1 || o[1].kind != 3) fatal("lea REG, [MEM]");
        static const u8 opc[1] = {0x8D};
        emit_rm(it, e, o[0].bits == 16, o[0].bits == 64, 0, opc, 1, o[0].reg, &o[1]);
        return;
    }

    /* ---- the two-operand arithmetic group */
    for (int k = 0; k < 8; k++) {
        if (strcmp(m, alu_names[k])) continue;
        if (n != 2) fatal("%s takes two operands", m);
        Op *d = &o[0], *s = &o[1];
        int base = k * 8;
        if (s->kind == 2) {
            int bits = op_size(it, d, NULL);
            Val v = ev_op(it, s);
            int acc = d->kind == 1 && d->reg == 0 && !d->hi8;
            if (bits == 8) {
                if (acc) {
                    eb(e, (u32)base + 4);
                } else {
                    static const u8 o80[1] = {0x80};
                    emit_rm(it, e, 0, 0, needs_byte_rex(d), o80, 1, k, d);
                }
                efix(e, 1, 0, 0, v);
            } else if (v.kind == V_CONST && fits8(v.num)) {
                static const u8 o83[1] = {0x83};
                emit_rm(it, e, bits == 16, bits == 64, 0, o83, 1, k, d);
                efix(e, 1, 0, 1, v);
            } else if (acc) {
                if (bits == 16) eb(e, 0x66);
                if (bits == 64) eb(e, 0x48);
                eb(e, (u32)base + 5);
                efix(e, bits == 16 ? 2 : 4, 0, bits == 64, v);
            } else {
                static const u8 o81[1] = {0x81};
                emit_rm(it, e, bits == 16, bits == 64, 0, o81, 1, k, d);
                efix(e, bits == 16 ? 2 : 4, 0, bits == 64, v);
            }
            return;
        }
        if (d->kind == 1 && s->kind == 1) {
            if (d->bits != s->bits) fatal("mismatch in operand sizes");
            u8 opc[1] = {(u8)(base + (d->bits == 8 ? 0 : 1))};
            emit_rm(it, e, d->bits == 16, d->bits == 64, needs_byte_rex(d) || needs_byte_rex(s), opc, 1, s->reg, d);
            return;
        }
        if (d->kind == 1 && s->kind == 3) {
            if (s->msize && s->msize * 8 != d->bits) fatal("mismatch in operand sizes");
            u8 opc[1] = {(u8)(base + (d->bits == 8 ? 2 : 3))};
            emit_rm(it, e, d->bits == 16, d->bits == 64, needs_byte_rex(d), opc, 1, d->reg, s);
            return;
        }
        if (d->kind == 3 && s->kind == 1) {
            if (d->msize && d->msize * 8 != s->bits) fatal("mismatch in operand sizes");
            u8 opc[1] = {(u8)(base + (s->bits == 8 ? 0 : 1))};
            emit_rm(it, e, s->bits == 16, s->bits == 64, needs_byte_rex(s), opc, 1, s->reg, d);
            return;
        }
        fatal("unsupported operands for %s", m);
    }

    /* ---- test */
    if (!strcmp(m, "test")) {
        if (n != 2) fatal("test takes two operands");
        Op *d = &o[0], *s = &o[1];
        if (s->kind == 2) {
            int bits = op_size(it, d, NULL);
            Val v = ev_op(it, s);
            int acc = d->kind == 1 && d->reg == 0 && !d->hi8;
            if (acc) {
                if (bits == 16) eb(e, 0x66);
                if (bits == 64) eb(e, 0x48);
                eb(e, bits == 8 ? 0xA8 : 0xA9);
            } else {
                u8 opc[1] = {(u8)(bits == 8 ? 0xF6 : 0xF7)};
                emit_rm(it, e, bits == 16, bits == 64, needs_byte_rex(d), opc, 1, 0, d);
            }
            efix(e, bits == 8 ? 1 : bits == 16 ? 2 : 4, 0, bits == 64, v);
            return;
        }
        Op *rop = s->kind == 1 ? s : d, *rm = s->kind == 1 ? d : s;
        if (rop->kind != 1 || (rm->kind != 1 && rm->kind != 3)) fatal("unsupported operands for test");
        u8 opc[1] = {(u8)(rop->bits == 8 ? 0x84 : 0x85)};
        emit_rm(it, e, rop->bits == 16, rop->bits == 64, needs_byte_rex(rop) || needs_byte_rex(rm), opc, 1, rop->reg, rm);
        return;
    }

    /* ---- one-operand group: inc dec not neg mul div idiv */
    {
        static const struct {
            const char *name;
            int opc8, opc, ext;
        } g1[] = {{"inc", 0xFE, 0xFF, 0}, {"dec", 0xFE, 0xFF, 1}, {"not", 0xF6, 0xF7, 2}, {"neg", 0xF6, 0xF7, 3}, {"mul", 0xF6, 0xF7, 4}, {"div", 0xF6, 0xF7, 6},
                  {"idiv", 0xF6, 0xF7, 7}};
        for (size_t i = 0; i < sizeof g1 / sizeof g1[0]; i++) {
            if (strcmp(m, g1[i].name)) continue;
            if (n != 1) fatal("%s takes one operand", m);
            int bits = op_size(it, &o[0], NULL);
            u8 opc[1] = {(u8)(bits == 8 ? g1[i].opc8 : g1[i].opc)};
            emit_rm(it, e, bits == 16, bits == 64, needs_byte_rex(&o[0]), opc, 1, g1[i].ext, &o[0]);
            return;
        }
    }

    /* ---- imul */
    if (!strcmp(m, "imul")) {
        if (n == 1) {
            int bits = op_size(it, &o[0], NULL);
            u8 opc[1] = {(u8)(bits == 8 ? 0xF6 : 0xF7)};
            emit_rm(it, e, bits == 16, bits == 64, needs_byte_rex(&o[0]), opc, 1, 5, &o[0]);
            return;
        }
        if (n == 2 && o[0].kind == 1 && o[0].bits != 8 && (o[1].kind == 1 || o[1].kind == 3)) {
            static const u8 opc[2] = {0x0F, 0xAF};
            emit_rm(it, e, o[0].bits == 16, o[0].bits == 64, 0, opc, 2, o[0].reg, &o[1]);
            return;
        }
        if (n == 3 && o[0].kind == 1 && (o[1].kind == 1 || o[1].kind == 3) && o[2].kind == 2) {
            Val v = ev_op(it, &o[2]);
            int small = v.kind == V_CONST && fits8(v.num);
            u8 opc[1] = {(u8)(small ? 0x6B : 0x69)};
            emit_rm(it, e, o[0].bits == 16, o[0].bits == 64, 0, opc, 1, o[0].reg, &o[1]);
            efix(e, small ? 1 : (o[0].bits == 16 ? 2 : 4), 0, 1, v);
            return;
        }
        fatal("unsupported operands for imul");
    }

    /* ---- push and pop */
    if (!strcmp(m, "push") || !strcmp(m, "pop")) {
        int is_push = m[1] == 'u';
        if (n != 1) fatal("%s takes one operand", m);
        Op *a = &o[0];
        if (a->kind == 1) {
            if (a->bits != 64) fatal("%s needs a 64-bit register in 64-bit mode", m);
            emit_plus_reg(e, 0, 0, 0, is_push ? 0x50 : 0x58, a->reg);
            return;
        }
        if (a->kind == 2 && is_push) {
            Val v = ev_op(it, a);
            if (v.kind == V_CONST && fits8(v.num)) {
                eb(e, 0x6A);
                efix(e, 1, 0, 1, v);
            } else {
                eb(e, 0x68);
                efix(e, 4, 0, 1, v);
            }
            return;
        }
        if (a->kind == 3) {
            if (a->msize && a->msize != 8) fatal("only a qword can be pushed or popped in 64-bit mode");
            u8 opc[1] = {(u8)(is_push ? 0xFF : 0x8F)};
            emit_rm(it, e, 0, 0, 0, opc, 1, is_push ? 6 : 0, a);
            return;
        }
        fatal("unsupported operand for %s", m);
    }

    /* ---- branches */
    if (!strcmp(m, "call")) {
        enc_branch(it, e, 1, -1);
        return;
    }
    if (!strcmp(m, "jmp")) {
        enc_branch(it, e, 0, -1);
        return;
    }
    if (m[0] == 'j' && cc_from_name(m + 1) >= 0) {
        enc_branch(it, e, 0, cc_from_name(m + 1));
        return;
    }

    /* ---- ret imm, int, in, out */
    if (!strcmp(m, "ret") && n == 1 && o[0].kind == 2) {
        eb(e, 0xC2);
        emit_imm(it, e, &o[0], 2, 0);
        return;
    }
    if (!strcmp(m, "int") && n == 1 && o[0].kind == 2) {
        eb(e, 0xCD);
        emit_imm(it, e, &o[0], 1, 0);
        return;
    }
    if (!strcmp(m, "in") && n == 2 && o[0].kind == 1 && o[0].reg == 0) {
        int bits = o[0].bits;
        if (bits == 16) eb(e, 0x66);
        if (o[1].kind == 1 && o[1].reg == 2 && o[1].bits == 16) eb(e, bits == 8 ? 0xEC : 0xED);
        else if (o[1].kind == 2) {
            eb(e, bits == 8 ? 0xE4 : 0xE5);
            emit_imm(it, e, &o[1], 1, 0);
        } else fatal("in AL/AX/EAX, DX or imm8");
        return;
    }
    if (!strcmp(m, "out") && n == 2 && o[1].kind == 1 && o[1].reg == 0) {
        int bits = o[1].bits;
        if (bits == 16) eb(e, 0x66);
        if (o[0].kind == 1 && o[0].reg == 2 && o[0].bits == 16) eb(e, bits == 8 ? 0xEE : 0xEF);
        else if (o[0].kind == 2) {
            eb(e, bits == 8 ? 0xE6 : 0xE7);
            emit_imm(it, e, &o[0], 1, 0);
        } else fatal("out DX or imm8, AL/AX/EAX");
        return;
    }

    /* ---- shifts and rotates */
    for (size_t i = 0; i < sizeof shift_ops / sizeof shift_ops[0]; i++) {
        if (strcmp(m, shift_ops[i].name)) continue;
        if (n != 2) fatal("%s takes two operands", m);
        int bits = op_size(it, &o[0], NULL);
        int ext = shift_ops[i].ext;
        if (o[1].kind == 1 && o[1].reg == 1 && o[1].bits == 8) {
            u8 opc[1] = {(u8)(bits == 8 ? 0xD2 : 0xD3)};
            emit_rm(it, e, bits == 16, bits == 64, needs_byte_rex(&o[0]), opc, 1, ext, &o[0]);
            return;
        }
        if (o[1].kind != 2) fatal("shift count must be CL or an immediate");
        Val v = ev_op(it, &o[1]);
        if (v.kind == V_CONST && v.num == 1) {
            u8 opc[1] = {(u8)(bits == 8 ? 0xD0 : 0xD1)};
            emit_rm(it, e, bits == 16, bits == 64, needs_byte_rex(&o[0]), opc, 1, ext, &o[0]);
            return;
        }
        u8 opc[1] = {(u8)(bits == 8 ? 0xC0 : 0xC1)};
        emit_rm(it, e, bits == 16, bits == 64, needs_byte_rex(&o[0]), opc, 1, ext, &o[0]);
        efix(e, 1, 0, 0, v);
        return;
    }

    /* ---- movzx / movsx / movsxd */
    if (!strcmp(m, "movzx") || !strcmp(m, "movsx")) {
        if (n != 2 || o[0].kind != 1 || (o[1].kind != 1 && o[1].kind != 3)) fatal("%s REG, REG/MEM", m);
        int sb = op_bits(&o[1]);
        if (sb != 8 && sb != 16) fatal("%s needs a byte or word source (use a size keyword on memory)", m);
        u8 opc[2] = {0x0F, (u8)(m[3] == 'z' ? (sb == 8 ? 0xB6 : 0xB7) : (sb == 8 ? 0xBE : 0xBF))};
        emit_rm(it, e, o[0].bits == 16, o[0].bits == 64, needs_byte_rex(&o[1]), opc, 2, o[0].reg, &o[1]);
        return;
    }
    if (!strcmp(m, "movsxd")) {
        if (n != 2 || o[0].kind != 1 || (o[1].kind != 1 && o[1].kind != 3)) fatal("movsxd REG64, REG32/MEM32");
        static const u8 opc[1] = {0x63};
        emit_rm(it, e, 0, o[0].bits == 64, 0, opc, 1, o[0].reg, &o[1]);
        return;
    }

    /* ---- cmovcc, setcc */
    if (!strncmp(m, "cmov", 4) && cc_from_name(m + 4) >= 0) {
        if (n != 2 || o[0].kind != 1 || (o[1].kind != 1 && o[1].kind != 3)) fatal("%s REG, REG/MEM", m);
        u8 opc[2] = {0x0F, (u8)(0x40 + cc_from_name(m + 4))};
        emit_rm(it, e, o[0].bits == 16, o[0].bits == 64, 0, opc, 2, o[0].reg, &o[1]);
        return;
    }
    if (!strncmp(m, "set", 3) && cc_from_name(m + 3) >= 0) {
        if (n != 1) fatal("%s takes one operand", m);
        u8 opc[2] = {0x0F, (u8)(0x90 + cc_from_name(m + 3))};
        emit_rm(it, e, 0, 0, needs_byte_rex(&o[0]), opc, 2, 0, &o[0]);
        return;
    }

    /* ---- xchg, xadd, cmpxchg, bswap, bsf, bsr, popcnt, bt* */
    if (!strcmp(m, "xchg")) {
        if (n != 2) fatal("xchg takes two operands");
        Op *a = &o[0], *b = &o[1];
        if (a->kind == 3) {
            Op *t = a;
            a = b;
            b = t;
        }
        if (a->kind != 1 || (b->kind != 1 && b->kind != 3)) fatal("unsupported operands for xchg");
        if (a->bits != 8 && b->kind == 1 && (a->reg == 0 || b->reg == 0) && a->bits == b->bits) {
            Op *x = a->reg == 0 ? b : a;                                   /* xchg with the accumulator: 90+r */
            emit_plus_reg(e, a->bits == 16, a->bits == 64, 0, 0x90, x->reg);
            return;
        }
        u8 opc[1] = {(u8)(a->bits == 8 ? 0x86 : 0x87)};
        emit_rm(it, e, a->bits == 16, a->bits == 64, needs_byte_rex(a) || needs_byte_rex(b), opc, 1, a->reg, b);
        return;
    }
    if (!strcmp(m, "xadd") || !strcmp(m, "cmpxchg")) {
        if (n != 2 || o[1].kind != 1) fatal("%s RM, REG", m);
        int x = m[0] == 'x';
        u8 opc[2] = {0x0F, (u8)((x ? 0xC0 : 0xB0) + (o[1].bits == 8 ? 0 : 1))};
        emit_rm(it, e, o[1].bits == 16, o[1].bits == 64, needs_byte_rex(&o[1]), opc, 2, o[1].reg, &o[0]);
        return;
    }
    if (!strcmp(m, "bswap")) {
        if (n != 1 || o[0].kind != 1 || o[0].bits < 32) fatal("bswap REG32/REG64");
        int rex = (o[0].bits == 64 ? 8 : 0) | ((o[0].reg & 8) ? 1 : 0);
        if (rex) eb(e, 0x40 | rex);
        eb(e, 0x0F);
        eb(e, 0xC8 + (u32)(o[0].reg & 7));
        return;
    }
    if (!strcmp(m, "bsf") || !strcmp(m, "bsr") || !strcmp(m, "popcnt") || !strcmp(m, "lzcnt") || !strcmp(m, "tzcnt")) {
        if (n != 2 || o[0].kind != 1 || (o[1].kind != 1 && o[1].kind != 3)) fatal("%s REG, REG/MEM", m);
        int cnt = m[0] == 'p' || m[0] == 'l' || m[0] == 't';
        if (cnt) eb(e, 0xF3);
        u8 opc[2] = {0x0F, (u8)(!strcmp(m, "bsf") || !strcmp(m, "tzcnt") ? 0xBC : !strcmp(m, "bsr") || !strcmp(m, "lzcnt") ? 0xBD : 0xB8)};
        emit_rm(it, e, o[0].bits == 16, o[0].bits == 64, 0, opc, 2, o[0].reg, &o[1]);
        return;
    }
    {
        static const struct {
            const char *name;
            int opc, ext;
        } bt[] = {{"bt", 0xA3, 4}, {"bts", 0xAB, 5}, {"btr", 0xB3, 6}, {"btc", 0xBB, 7}};
        for (size_t i = 0; i < sizeof bt / sizeof bt[0]; i++) {
            if (strcmp(m, bt[i].name)) continue;
            if (n != 2) fatal("%s takes two operands", m);
            if (o[1].kind == 1) {
                u8 opc[2] = {0x0F, (u8)bt[i].opc};
                emit_rm(it, e, o[1].bits == 16, o[1].bits == 64, 0, opc, 2, o[1].reg, &o[0]);
                return;
            }
            int bits = op_size(it, &o[0], NULL);
            u8 opc[2] = {0x0F, 0xBA};
            emit_rm(it, e, bits == 16, bits == 64, 0, opc, 2, bt[i].ext, &o[0]);
            emit_imm(it, e, &o[1], 1, 0);
            return;
        }
    }
    fatal("unknown instruction '%s' or unsupported operands", m);
}

/* ------------------------------------------------------------------------------------------------------ resolving */

static void add_reloc(int sec, u32 off, int type, Sym *sym, int secsym, i64 addend) {
    Section *s = &secs[sec];
    if (s->nrel == s->crel) {
        s->crel = s->crel ? s->crel * 2 : 32;
        s->rel = xrealloc(s->rel, (size_t)s->crel * sizeof(Reloc));
    }
    Reloc *r = &s->rel[s->nrel++];
    r->off = off;
    r->type = type;
    r->sym = sym;
    r->secsym = secsym;
    r->addend = addend;
}

static int final_pass;          /* the last walk over the items: symbols must all be known, branches must fit */

/* turns the pending fields of an encoding into bytes and relocations; `base` is the item's offset in its section; `emit` also records the relocations */
static void resolve_fixups(Item *it, Enc *e, int emit) {
    for (int i = 0; i < e->nfx; i++) {
        Fixup *f = &e->fx[i];
        Val v = f->v;
        if (v.kind == V_UNK) {
            if (final_pass) fatal("symbol '%s' is not defined in this file (declare it extern?)", v.ext ? v.ext->name : "?");
            continue;
        }
        if (f->pcrel) {
            if (v.kind == V_SEC && v.sec == it->sec) {
                i64 rel = v.num - (it->pos + e->n);
                if (f->size == 1) {
                    if (!fits8(rel)) {
                        if (it->op[0].is_short) fatal("short jump out of range");
                        it->grow = 1;                                   /* the short form does not reach: the layout switches this branch to the long one */
                        if (final_pass && !it->near_) fatal("short jump out of range");
                        continue;
                    }
                } else if (!fits32s(rel)) fatal("relative jump out of range");
                put_le(e->b + f->off, (u64)rel, f->size);
            } else if (v.kind == V_SEC || v.kind == V_EXT) {
                if (f->size != 4) fatal("a short jump needs a label in the same section");
                if (emit) add_reloc(it->sec, (u32)(it->pos + f->off), R_X86_64_PC32, v.kind == V_EXT ? v.ext : NULL, v.kind == V_SEC ? v.sec : -1,
                                    v.num - (e->n - f->off));
            } else {
                fatal("a relative reference to a constant address is not supported");
            }
            continue;
        }
        if (v.kind == V_CONST) {
            if (f->size == 4 && f->sign && !fits32s(v.num) && !(v.num >= 0 && v.num <= 0xFFFFFFFFLL && !f->sign)) {
                if (fits32u(v.num)) {
                    /* a 32-bit field written as an unsigned number in a signed context is fine for 32-bit operations */
                } else fatal("value %lld does not fit in 32 bits", v.num);
            }
            if (f->size == 1 && (v.num < -128 || v.num > 255)) fatal("value %lld does not fit in a byte", v.num);
            put_le(e->b + f->off, (u64)v.num, f->size);
            continue;
        }
        /* an address: absolute relocation */
        int type = f->size == 8 ? R_X86_64_64 : f->size == 4 ? (f->sign ? R_X86_64_32S : R_X86_64_32) : f->size == 2 ? R_X86_64_16 : R_X86_64_8;
        if (emit) add_reloc(it->sec, (u32)(it->pos + f->off), type, v.kind == V_EXT ? v.ext : NULL, v.kind == V_SEC ? v.sec : -1, v.num);
    }
}

/* the encoding of one instruction item: its length, and (with emit) the bytes appended to the section */
static int encode_item(Item *it, Buf *out, int emit) {
    Enc e;
    memset(&e, 0, sizeof e);
    it->grow = 0;
    enc_insn(it, &e);
    resolve_fixups(it, &e, emit);
    if (out) buf_put(out, e.b, (size_t)e.n);
    return e.n;
}
