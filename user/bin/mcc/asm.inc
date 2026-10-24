/* asm.inc -- inline assembly: GNU extended asm with the common constraints, and a small AT&T assembler for what real
 * programs put into it (system calls, rep movs/stos, cpuid, rdtsc, port I/O, atomics, simple ALU and mov forms).
 * x87 and SSE instructions are not understood.
 *
 * The code generator keeps nothing in registers between statements (only rbp, rsp are special and rbx/r12-r15 are not
 * used), so an asm statement may use every caller-saved register freely; clobbered callee-saved ones are saved around it.
 */

typedef struct {
    int kind; /* 0 none, 1 register, 2 immediate, 3 memory */
    int reg;
    int size; /* register width in bytes */
    bool hi8; /* ah, ch, dh, bh */
    i64 imm;
    Mem mem;
    int seg; /* 0, or the segment prefix byte (fs 0x64, gs 0x65) */
} AsmOp;

static const char *reg64_names[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
static const char *reg32_names[16] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"};
static const char *reg16_names[16] = {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di", "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"};
static const char *reg8_names[16] = {"al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil", "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"};

static Token *asm_tok;

static void asm_error(const char *what, const char *text) {
    error_tok(asm_tok, "inline assembly: %s '%s' is not supported by mcc", what, text);
}

static bool find_reg(const char *name, int *reg, int *size, bool *hi8) {
    *hi8 = false;
    for (int i = 0; i < 16; i++) {
        if (!strcmp(name, reg64_names[i])) { *reg = i; *size = 8; return true; }
        if (!strcmp(name, reg32_names[i])) { *reg = i; *size = 4; return true; }
        if (!strcmp(name, reg16_names[i])) { *reg = i; *size = 2; return true; }
        if (!strcmp(name, reg8_names[i])) { *reg = i; *size = 1; return true; }
    }
    static const char *hi[4] = {"ah", "ch", "dh", "bh"};
    for (int i = 0; i < 4; i++)
        if (!strcmp(name, hi[i])) { *reg = i; *size = 1; *hi8 = true; return true; }
    return false;
}

static char *asm_skip_ws(char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static i64 asm_number(char **pp) {
    char *p = *pp;
    bool neg = false;
    if (*p == '-') { neg = true; p++; }
    else if (*p == '+') p++;
    i64 v = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        for (; isxdigit((u8)*p); p++) v = v * 16 + from_hex(*p);
    } else if (p[0] == '0' && isdigit((u8)p[1])) {
        for (; isdigit((u8)*p); p++) v = v * 8 + (*p - '0');
    } else {
        if (!isdigit((u8)*p)) asm_error("number", p);
        for (; isdigit((u8)*p); p++) v = v * 10 + (*p - '0');
    }
    *pp = p;
    return neg ? -v : v;
}

static char *asm_regname(char *p, char *out) {
    int n = 0;
    while (isalnum((u8)*p) && n < 15) out[n++] = *p++;
    out[n] = 0;
    return p;
}

/* parse one AT&T operand: %reg, $imm, disp(%base,%index,scale), %seg:disp */
static char *asm_operand(char *p, AsmOp *o) {
    memset(o, 0, sizeof *o);
    p = asm_skip_ws(p);
    if (*p == '*') p = asm_skip_ws(p + 1); /* call *%rax */
    o->mem = mem_reg(-1, 0);
    if (*p == '$') {
        p++;
        o->kind = 2;
        o->imm = asm_number(&p);
        return p;
    }
    if (*p == '%') {
        char name[16];
        char *q = asm_regname(p + 1, name);
        if (*q == ':' && (!strcmp(name, "fs") || !strcmp(name, "gs"))) {
            o->seg = name[0] == 'f' ? 0x64 : 0x65;
            p = q + 1;
        } else {
            if (!find_reg(name, &o->reg, &o->size, &o->hi8)) asm_error("register", name);
            o->kind = 1;
            return q;
        }
    }
    /* memory: [disp](base[,index[,scale]]) or an absolute address */
    i64 disp = 0;
    if (*p == '-' || *p == '+' || isdigit((u8)*p)) disp = asm_number(&p);
    o->kind = 3;
    o->mem.disp = (i32)disp;
    o->mem.base = -1;
    if (*p == '(') {
        p++;
        char name[16];
        p = asm_skip_ws(p);
        if (*p == '%') {
            p = asm_regname(p + 1, name);
            int sz;
            bool hi;
            if (!find_reg(name, &o->mem.base, &sz, &hi)) asm_error("register", name);
        }
        p = asm_skip_ws(p);
        if (*p == ',') {
            p++;
            p = asm_skip_ws(p);
            p = asm_regname(p + 1, name);
            int sz;
            bool hi;
            if (!find_reg(name, &o->mem.index, &sz, &hi)) asm_error("register", name);
            p = asm_skip_ws(p);
            o->mem.scale = 1;
            if (*p == ',') {
                p++;
                o->mem.scale = (int)asm_number(&p);
            }
        }
        p = asm_skip_ws(p);
        if (*p != ')') asm_error("operand", p);
        p++;
    }
    return p;
}

/* an operand in the ModRM r/m field: register, or memory (with the segment prefix) */
static void asm_rm(int pfx, int w, int op, int reg, AsmOp *rm, int imm_after, bool byte) {
    if (rm->seg) e1(rm->seg);
    if (rm->kind == 1) {
        i_rr(pfx, w, op, reg, rm->reg, byte && (reg >= 4 && reg < 8 ? 1 : (rm->reg >= 4 && rm->reg < 8)));
        return;
    }
    if (rm->kind != 3) asm_error("operand", "");
    if (rm->mem.base < 0) {
        /* absolute address: modrm 00 reg 100, sib 00 100 101, disp32 */
        if (pfx) e1(pfx);
        rex(w, reg, 0, 0, byte && reg >= 4 && reg < 8);
        eop(op);
        e1(((reg & 7) << 3) | 4);
        e1(0x25);
        e4((u32)rm->mem.disp);
        return;
    }
    i_rm(pfx, w, op, reg, rm->mem, imm_after, byte && reg >= 4 && reg < 8);
}

static int size_of_ops(AsmOp *ops, int n, int suffix) {
    if (suffix) return suffix;
    for (int i = 0; i < n; i++)
        if (ops[i].kind == 1) return ops[i].size;
    asm_error("operand size of", "instruction");
    return 0;
}

static void emit_imm(i64 v, int size) {
    if (size == 1) e1((int)(v & 0xFF));
    else if (size == 2) {
        e1((int)(v & 0xFF));
        e1((int)((v >> 8) & 0xFF));
    } else e4((u32)v);
}

typedef struct {
    const char *name;
    int len;
    u8 bytes[4];
} AsmFixed;

static const AsmFixed asm_fixed[] = {
    {"nop", 1, {0x90}}, {"pause", 2, {0xF3, 0x90}}, {"syscall", 2, {0x0F, 0x05}}, {"sysret", 2, {0x0F, 0x07}}, {"ud2", 2, {0x0F, 0x0B}},
    {"cpuid", 2, {0x0F, 0xA2}}, {"rdtsc", 2, {0x0F, 0x31}}, {"rdtscp", 3, {0x0F, 0x01, 0xF9}}, {"hlt", 1, {0xF4}}, {"cli", 1, {0xFA}},
    {"sti", 1, {0xFB}}, {"cld", 1, {0xFC}}, {"std", 1, {0xFD}}, {"clc", 1, {0xF8}}, {"stc", 1, {0xF9}}, {"cmc", 1, {0xF5}},
    {"mfence", 3, {0x0F, 0xAE, 0xF0}}, {"lfence", 3, {0x0F, 0xAE, 0xE8}}, {"sfence", 3, {0x0F, 0xAE, 0xF8}}, {"int3", 1, {0xCC}},
    {"leave", 1, {0xC9}}, {"ret", 1, {0xC3}}, {"retq", 1, {0xC3}}, {"pushfq", 1, {0x9C}}, {"popfq", 1, {0x9D}}, {"pushf", 1, {0x9C}},
    {"popf", 1, {0x9D}}, {"cltq", 2, {0x48, 0x98}}, {"cqto", 2, {0x48, 0x99}}, {"cltd", 1, {0x99}}, {"cwtl", 1, {0x98}},
    {"xgetbv", 3, {0x0F, 0x01, 0xD0}}, {"lahf", 1, {0x9F}}, {"sahf", 1, {0x9E}}, {"swapgs", 3, {0x0F, 0x01, 0xF8}}, {"iretq", 2, {0x48, 0xCF}},
    {"movsb", 1, {0xA4}}, {"movsw", 2, {0x66, 0xA5}}, {"movsl", 1, {0xA5}}, {"movsq", 2, {0x48, 0xA5}},
    {"stosb", 1, {0xAA}}, {"stosw", 2, {0x66, 0xAB}}, {"stosl", 1, {0xAB}}, {"stosq", 2, {0x48, 0xAB}},
    {"lodsb", 1, {0xAC}}, {"lodsw", 2, {0x66, 0xAD}}, {"lodsl", 1, {0xAD}}, {"lodsq", 2, {0x48, 0xAD}},
    {"scasb", 1, {0xAE}}, {"scasw", 2, {0x66, 0xAF}}, {"scasl", 1, {0xAF}}, {"scasq", 2, {0x48, 0xAF}},
    {"cmpsb", 1, {0xA6}}, {"cmpsw", 2, {0x66, 0xA7}}, {"cmpsl", 1, {0xA7}}, {"cmpsq", 2, {0x48, 0xA7}},
};

/* numeric local labels (1: ... jne 1b / 1f) of the current asm statement */
static int asm_back[10], asm_fwd[10];

static void asm_local_label_define(int n) {
    int id = asm_fwd[n] ? asm_fwd[n] : next_label();
    asm_fwd[n] = 0;
    label_set(id);
    asm_back[n] = id;
}

static int asm_label_ref(char *p, char **end) {
    /* "1b" or "1f" */
    if (isdigit((u8)p[0]) && (p[1] == 'b' || p[1] == 'f') && !isalnum((u8)p[2])) {
        int n = p[0] - '0';
        *end = p + 2;
        if (p[1] == 'b') {
            if (!asm_back[n]) asm_error("backward label", p);
            return asm_back[n];
        }
        if (!asm_fwd[n]) asm_fwd[n] = next_label();
        return asm_fwd[n];
    }
    return 0;
}

static const char *cc_names[] = {"o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"};

static int cc_by_name(const char *s) {
    static const struct {
        const char *n;
        int cc;
    } alias[] = {{"c", 2}, {"nae", 2}, {"nc", 3}, {"nb", 3}, {"z", 4}, {"nz", 5}, {"na", 6}, {"nbe", 7}, {"pe", 10}, {"po", 11}, {"nge", 12},
                 {"nl", 13}, {"ng", 14}, {"nle", 15}};
    for (int i = 0; i < 16; i++)
        if (!strcmp(s, cc_names[i])) return i;
    for (size_t i = 0; i < sizeof alias / sizeof alias[0]; i++)
        if (!strcmp(s, alias[i].n)) return alias[i].cc;
    return -1;
}

/* assembles one instruction (without labels): mnemonic, operands in AT&T order (source first) */
static void asm_instruction(char *text) {
    char *p = asm_skip_ws(text);
    if (!*p) return;
    char mnem[32];
    int n = 0;
    while (*p && !isspace((u8)*p) && n < 31) mnem[n++] = *p++;
    mnem[n] = 0;
    bool lock = false;
    int rep = 0;
    for (;;) {
        if (!strcmp(mnem, "lock")) lock = true;
        else if (!strcmp(mnem, "rep") || !strcmp(mnem, "repe") || !strcmp(mnem, "repz")) rep = 0xF3;
        else if (!strcmp(mnem, "repne") || !strcmp(mnem, "repnz")) rep = 0xF2;
        else break;
        p = asm_skip_ws(p);
        n = 0;
        while (*p && !isspace((u8)*p) && n < 31) mnem[n++] = *p++;
        mnem[n] = 0;
        if (!n) asm_error("prefix without instruction", text);
    }
    if (lock) e1(0xF0);
    if (rep) e1(rep);

    for (size_t i = 0; i < sizeof asm_fixed / sizeof asm_fixed[0]; i++) {
        if (!strcmp(mnem, asm_fixed[i].name)) {
            for (int k = 0; k < asm_fixed[i].len; k++) e1(asm_fixed[i].bytes[k]);
            return;
        }
    }

    /* operands */
    AsmOp ops[3];
    int nops = 0;
    char *labelend;
    int target_label = 0;
    p = asm_skip_ws(p);
    while (*p && nops < 3) {
        if (nops == 0 && (mnem[0] == 'j' || !strcmp(mnem, "call"))) {
            target_label = asm_label_ref(p, &labelend);
            if (target_label) {
                p = labelend;
                break;
            }
        }
        p = asm_operand(p, &ops[nops++]);
        p = asm_skip_ws(p);
        if (*p == ',') p++;
        else break;
    }
    p = asm_skip_ws(p);
    if (*p && *p != '#') asm_error("operand", p);

    /* jumps */
    if (target_label || (mnem[0] == 'j' && nops == 0)) {
        if (!strcmp(mnem, "jmp")) jmp_label(target_label);
        else if (!strcmp(mnem, "call")) {
            e1(0xE8);
            add_fixup(cur_pos(), target_label);
            e4(0);
        } else {
            int cc = cc_by_name(mnem + 1);
            if (cc < 0) asm_error("instruction", mnem);
            jcc_label(cc, target_label);
        }
        return;
    }

    if ((!strcmp(mnem, "call") || !strcmp(mnem, "jmp") || !strcmp(mnem, "callq") || !strcmp(mnem, "jmpq")) && nops == 1 && ops[0].kind == 1) {
        i_rr(0, 0, 0xFF, mnem[0] == 'c' ? 2 : 4, ops[0].reg, 0);
        return;
    }

    /* the operand-size suffix */
    int suffix = 0;
    char base[32];
    strcpy(base, mnem);
    size_t len = strlen(base);
    static const char *sized[] = {"mov", "add", "or", "adc", "sbb", "and", "sub", "xor", "cmp", "test", "lea", "push", "pop", "inc", "dec", "neg", "not", "mul",
                                  "imul", "div", "idiv", "shl", "shr", "sar", "rol", "ror", "xchg", "xadd", "cmpxchg", "bswap", "bsf", "bsr", "popcnt", "in", "out", "sal"};
    if (len > 1 && strchr("bwlq", base[len - 1])) {
        char tmp[32];
        strcpy(tmp, base);
        tmp[len - 1] = 0;
        for (size_t i = 0; i < sizeof sized / sizeof sized[0]; i++)
            if (!strcmp(tmp, sized[i])) {
                suffix = base[len - 1] == 'b' ? 1 : base[len - 1] == 'w' ? 2 : base[len - 1] == 'l' ? 4 : 8;
                strcpy(base, tmp);
                break;
            }
    }

    /* movzx / movsx family: movzbl, movzwq, movsbw, movslq ... */
    if ((str_starts(mnem, "movz") || str_starts(mnem, "movs")) && strlen(mnem) == 6 && nops == 2) {
        bool zx = mnem[3] == 'z';
        int from = mnem[4] == 'b' ? 1 : mnem[4] == 'w' ? 2 : mnem[4] == 'l' ? 4 : 0;
        int to = mnem[5] == 'w' ? 2 : mnem[5] == 'l' ? 4 : mnem[5] == 'q' ? 8 : 0;
        if (from && to && ops[1].kind == 1) {
            int op = from == 1 ? (zx ? 0x0FB6 : 0x0FBE) : from == 2 ? (zx ? 0x0FB7 : 0x0FBF) : 0x63;
            asm_rm(to == 2 ? 0x66 : 0, to == 8, op, ops[1].reg, &ops[0], 0, from == 1);
            return;
        }
    }

    static const struct {
        const char *name;
        int kind;
    } alu[] = {{"add", 0}, {"or", 1}, {"adc", 2}, {"sbb", 3}, {"and", 4}, {"sub", 5}, {"xor", 6}, {"cmp", 7}};
    for (size_t i = 0; i < sizeof alu / sizeof alu[0]; i++) {
        if (strcmp(base, alu[i].name) || nops != 2) continue;
        int k = alu[i].kind;
        int size = size_of_ops(ops, nops, suffix);
        int pfx = size == 2 ? 0x66 : 0, w = size == 8;
        bool byte = size == 1;
        if (ops[0].kind == 2) {
            /* op $imm, r/m */
            if (!byte && ops[0].imm >= -128 && ops[0].imm <= 127) {
                asm_rm(pfx, w, 0x83, k, &ops[1], 1, false);
                e1((int)(ops[0].imm & 0xFF));
            } else {
                asm_rm(pfx, w, byte ? 0x80 : 0x81, k, &ops[1], byte ? 1 : size == 2 ? 2 : 4, byte);
                emit_imm(ops[0].imm, byte ? 1 : size == 2 ? 2 : 4);
            }
        } else if (ops[0].kind == 1) {
            asm_rm(pfx, w, k * 8 + (byte ? 0 : 1), ops[0].reg, &ops[1], 0, byte);
        } else if (ops[1].kind == 1) {
            asm_rm(pfx, w, k * 8 + (byte ? 2 : 3), ops[1].reg, &ops[0], 0, byte);
        } else {
            asm_error("operands of", mnem);
        }
        return;
    }

    if (!strcmp(base, "mov") && nops == 2) {
        int size = size_of_ops(ops, nops, suffix);
        int pfx = size == 2 ? 0x66 : 0, w = size == 8;
        bool byte = size == 1;
        if (ops[0].kind == 2) {
            if (ops[1].kind == 1) {
                if (byte) {
                    if (ops[1].reg >= 8) e1(0x41);
                    else if (ops[1].reg >= 4) e1(0x40);
                    e1(0xB0 + (ops[1].reg & 7));
                    e1((int)(ops[0].imm & 0xFF));
                } else if (size == 8 && (ops[0].imm > 2147483647LL || ops[0].imm < -2147483648LL)) {
                    mov_ri(ops[1].reg, ops[0].imm);
                } else if (size == 8) {
                    asm_rm(0, 1, 0xC7, 0, &ops[1], 4, false);
                    e4((u32)ops[0].imm);
                } else {
                    if (pfx) e1(pfx);
                    if (ops[1].reg >= 8) e1(0x41);
                    e1(0xB8 + (ops[1].reg & 7));
                    emit_imm(ops[0].imm, size == 2 ? 2 : 4);
                }
            } else {
                asm_rm(pfx, w, byte ? 0xC6 : 0xC7, 0, &ops[1], byte ? 1 : size == 2 ? 2 : 4, false);
                emit_imm(ops[0].imm, byte ? 1 : size == 2 ? 2 : 4);
            }
        } else if (ops[0].kind == 1) {
            asm_rm(pfx, w, byte ? 0x88 : 0x89, ops[0].reg, &ops[1], 0, byte);
        } else if (ops[1].kind == 1) {
            asm_rm(pfx, w, byte ? 0x8A : 0x8B, ops[1].reg, &ops[0], 0, byte);
        } else {
            asm_error("operands of", mnem);
        }
        return;
    }

    if (!strcmp(base, "lea") && nops == 2 && ops[1].kind == 1 && ops[0].kind == 3) {
        asm_rm(ops[1].size == 2 ? 0x66 : 0, ops[1].size == 8, 0x8D, ops[1].reg, &ops[0], 0, false);
        return;
    }

    if (!strcmp(base, "test") && nops == 2) {
        int size = size_of_ops(ops, nops, suffix);
        bool byte = size == 1;
        if (ops[0].kind == 2) {
            asm_rm(size == 2 ? 0x66 : 0, size == 8, byte ? 0xF6 : 0xF7, 0, &ops[1], byte ? 1 : size == 2 ? 2 : 4, false);
            emit_imm(ops[0].imm, byte ? 1 : size == 2 ? 2 : 4);
        } else {
            asm_rm(size == 2 ? 0x66 : 0, size == 8, byte ? 0x84 : 0x85, ops[0].reg, &ops[1], 0, byte);
        }
        return;
    }

    if ((!strcmp(base, "xchg") || !strcmp(base, "xadd") || !strcmp(base, "cmpxchg")) && nops == 2 && ops[0].kind == 1) {
        int size = ops[0].size;
        bool byte = size == 1;
        int op = !strcmp(base, "xchg") ? (byte ? 0x86 : 0x87) : !strcmp(base, "xadd") ? (byte ? 0x0FC0 : 0x0FC1) : (byte ? 0x0FB0 : 0x0FB1);
        asm_rm(size == 2 ? 0x66 : 0, size == 8, op, ops[0].reg, &ops[1], 0, byte);
        return;
    }

    if (!strcmp(base, "push") && nops == 1) {
        if (ops[0].kind == 1) {
            push_r(ops[0].reg);
        } else if (ops[0].kind == 2) {
            e1(0x68);
            e4((u32)ops[0].imm);
        } else {
            asm_rm(0, 0, 0xFF, 6, &ops[0], 0, false);
        }
        return;
    }
    if (!strcmp(base, "pop") && nops == 1 && ops[0].kind == 1) {
        pop_r(ops[0].reg);
        return;
    }

    static const struct {
        const char *name;
        int op, ext;
    } unary[] = {{"not", 0xF7, 2}, {"neg", 0xF7, 3}, {"mul", 0xF7, 4}, {"div", 0xF7, 6}, {"idiv", 0xF7, 7}, {"inc", 0xFF, 0}, {"dec", 0xFF, 1}};
    for (size_t i = 0; i < sizeof unary / sizeof unary[0]; i++) {
        if (strcmp(base, unary[i].name) || nops != 1) continue;
        int size = size_of_ops(ops, nops, suffix);
        bool byte = size == 1;
        int op = unary[i].op == 0xF7 ? (byte ? 0xF6 : 0xF7) : (byte ? 0xFE : 0xFF);
        asm_rm(size == 2 ? 0x66 : 0, size == 8, op, unary[i].ext, &ops[0], 0, false);
        return;
    }

    if (!strcmp(base, "imul") && nops == 2 && ops[1].kind == 1) {
        asm_rm(ops[1].size == 2 ? 0x66 : 0, ops[1].size == 8, 0x0FAF, ops[1].reg, &ops[0], 0, false);
        return;
    }

    static const struct {
        const char *name;
        int ext;
    } shifts[] = {{"rol", 0}, {"ror", 1}, {"shl", 4}, {"sal", 4}, {"shr", 5}, {"sar", 7}};
    for (size_t i = 0; i < sizeof shifts / sizeof shifts[0]; i++) {
        if (strcmp(base, shifts[i].name) || nops < 1) continue;
        AsmOp *dst = &ops[nops - 1];
        int size = size_of_ops(dst, 1, suffix);
        bool byte = size == 1;
        int pfx = size == 2 ? 0x66 : 0, w = size == 8;
        if (nops == 1) {
            asm_rm(pfx, w, byte ? 0xD0 : 0xD1, shifts[i].ext, dst, 0, false);
        } else if (ops[0].kind == 2) {
            asm_rm(pfx, w, byte ? 0xC0 : 0xC1, shifts[i].ext, dst, 1, false);
            e1((int)(ops[0].imm & 0xFF));
        } else if (ops[0].kind == 1 && ops[0].reg == RCX && ops[0].size == 1) {
            asm_rm(pfx, w, byte ? 0xD2 : 0xD3, shifts[i].ext, dst, 0, false);
        } else {
            asm_error("operands of", mnem);
        }
        return;
    }

    if (!strcmp(base, "bswap") && nops == 1 && ops[0].kind == 1) {
        rex(ops[0].size == 8, 0, 0, ops[0].reg, 0);
        e1(0x0F);
        e1(0xC8 + (ops[0].reg & 7));
        return;
    }
    if ((!strcmp(base, "bsf") || !strcmp(base, "bsr") || !strcmp(base, "popcnt")) && nops == 2 && ops[1].kind == 1) {
        int op = !strcmp(base, "bsf") ? 0x0FBC : !strcmp(base, "bsr") ? 0x0FBD : 0x0FB8;
        asm_rm(!strcmp(base, "popcnt") ? 0xF3 : (ops[1].size == 2 ? 0x66 : 0), ops[1].size == 8, op, ops[1].reg, &ops[0], 0, false);
        return;
    }

    /* port I/O and int */
    if (!strcmp(mnem, "int") && nops == 1 && ops[0].kind == 2) {
        e1(0xCD);
        e1((int)ops[0].imm);
        return;
    }
    if ((!strcmp(base, "in") || !strcmp(mnem, "inb") || !strcmp(mnem, "inw") || !strcmp(mnem, "inl")) && nops == 2 && ops[1].kind == 1) {
        int size = ops[1].size;
        if (size == 2) e1(0x66);
        if (ops[0].kind == 2) {
            e1(size == 1 ? 0xE4 : 0xE5);
            e1((int)ops[0].imm);
        } else {
            e1(size == 1 ? 0xEC : 0xED);
        }
        return;
    }
    if ((!strcmp(base, "out") || !strcmp(mnem, "outb") || !strcmp(mnem, "outw") || !strcmp(mnem, "outl")) && nops == 2 && ops[0].kind == 1) {
        int size = ops[0].size;
        if (size == 2) e1(0x66);
        if (ops[1].kind == 2) {
            e1(size == 1 ? 0xE6 : 0xE7);
            e1((int)ops[1].imm);
        } else {
            e1(size == 1 ? 0xEE : 0xEF);
        }
        return;
    }

    /* setcc / cmovcc */
    if (str_starts(mnem, "set") && nops == 1) {
        int cc = cc_by_name(mnem + 3);
        if (cc >= 0) {
            asm_rm(0, 0, 0x0F90 + cc, 0, &ops[0], 0, true);
            return;
        }
    }
    if (str_starts(mnem, "cmov") && nops == 2 && ops[1].kind == 1) {
        int cc = cc_by_name(mnem + 4);
        if (cc >= 0) {
            asm_rm(ops[1].size == 2 ? 0x66 : 0, ops[1].size == 8, 0x0F40 + cc, ops[1].reg, &ops[0], 0, false);
            return;
        }
    }

    asm_error("instruction", mnem);
}

/* assembles a whole template: instructions separated by ';' or newlines, "N:" local labels, '#' comments, directives skipped */
static void asm_assemble(const char *template_text, Token *tok) {
    asm_tok = tok;
    memset(asm_back, 0, sizeof asm_back);
    memset(asm_fwd, 0, sizeof asm_fwd);
    char *buf = xstrdup(template_text);
    char *p = buf;
    while (*p) {
        char *end = p;
        while (*end && *end != ';' && *end != '\n') end++;
        char saved = *end;
        *end = 0;
        char *line = asm_skip_ws(p);
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        /* labels "1:" */
        while (isdigit((u8)line[0]) && line[1] == ':') {
            asm_local_label_define(line[0] - '0');
            line = asm_skip_ws(line + 2);
        }
        if (*line && *line != '.') asm_instruction(line);
        if (!saved) break;
        p = end + 1;
    }
    for (int i = 0; i < 10; i++)
        if (asm_fwd[i]) asm_error("undefined forward label", "N");
}

/* ---- extended asm: operands */

static int clobber_reg(const char *name) {
    int reg, size;
    bool hi;
    if (!strcmp(name, "memory") || !strcmp(name, "cc")) return -1;
    if (find_reg(name, &reg, &size, &hi)) return reg;
    return -1;
}

static void asm_operand_text(Buf *out, AsmOperand *o, char modifier) {
    if (o->kind == 2) {
        char tmp[32];
        snprintf(tmp, sizeof tmp, "$%lld", (long long)eval(o->expr));
        buf_str(out, tmp);
        return;
    }
    if (o->kind == 3) {
        char tmp[48];
        if (o->direct_mem) snprintf(tmp, sizeof tmp, "%d(%%rbp)", o->mem_disp);
        else snprintf(tmp, sizeof tmp, "(%%%s)", reg64_names[o->reg]);
        buf_str(out, tmp);
        return;
    }
    int size = o->expr->ty->size;
    if (o->expr->ty->base || o->expr->ty->kind == TY_PTR || o->expr->ty->kind == TY_ARRAY) size = 8;
    if (modifier == 'b') size = 1;
    else if (modifier == 'w') size = 2;
    else if (modifier == 'k') size = 4;
    else if (modifier == 'q') size = 8;
    const char *name = size == 1 ? reg8_names[o->reg] : size == 2 ? reg16_names[o->reg] : size == 4 ? reg32_names[o->reg] : reg64_names[o->reg];
    buf_u8(out, '%');
    buf_str(out, name);
}

static void gen_asm(Node *node) {
    AsmInfo *ai = node->asm_info;
    if (!ai) {
        asm_assemble(node->str, node->tok);
        return;
    }
    int n = ai->nout + ai->nin;
    AsmOperand *ops = ai->ops;
    unsigned used = 0; /* registers that are taken (bit per register) */
    unsigned clobbered = 0;
    for (int i = 0; i < ai->nclob; i++) {
        int r = clobber_reg(ai->clobbers[i]);
        if (r >= 0) clobbered |= 1u << r;
    }

    /* constraints */
    for (int i = 0; i < n; i++) {
        AsmOperand *o = &ops[i];
        o->is_out = i < ai->nout;
        o->match = -1;
        o->reg = -1;
        o->kind = 0;
        for (const char *c = o->cons; *c; c++) {
            switch (*c) {
            case '=':
            case '&':
            case '%':
            case ' ':
                break;
            case '+':
                o->rw = true;
                break;
            case 'a': o->kind = 1; o->reg = RAX; break;
            case 'b': o->kind = 1; o->reg = RBX; break;
            case 'c': o->kind = 1; o->reg = RCX; break;
            case 'd': o->kind = 1; o->reg = RDX; break;
            case 'S': o->kind = 1; o->reg = RSI; break;
            case 'D': o->kind = 1; o->reg = RDI; break;
            case 'r':
            case 'q':
            case 'Q':
            case 'R':
            case 'g':
                if (!o->kind) o->kind = 1;
                break;
            case 'm':
            case 'o':
            case 'V':
                if (!o->kind) o->kind = 3;
                break;
            case 'i':
            case 'n':
            case 'I':
            case 'J':
            case 'K':
            case 'N':
            case 'e':
            case 'Z':
                if (!o->kind) o->kind = 2;
                break;
            default:
                if (isdigit((u8)*c)) {
                    o->match = *c - '0';
                    break;
                }
                error_tok(node->tok, "inline assembly: constraint '%s' is not supported by mcc", o->cons);
            }
        }
        if (o->match >= 0) o->kind = 1;
        if (!o->kind) o->kind = 1;
        if (o->kind == 1 && o->reg >= 0) used |= 1u << o->reg;
        /* a register variable (register long r10 __asm__("r10")) pins its operand to that register */
        if (o->kind == 1 && o->reg < 0 && o->expr->kind == ND_VAR && o->expr->var->asm_reg) {
            int reg, size;
            bool hi;
            if (find_reg(o->expr->var->asm_reg, &reg, &size, &hi)) {
                o->reg = reg;
                used |= 1u << reg;
            }
        }
    }
    for (int i = 0; i < n; i++) {
        AsmOperand *o = &ops[i];
        if (o->match >= 0 && o->match < n) {
            o->reg = ops[o->match].reg;
            continue;
        }
    }
    static const int pool[] = {RAX, RCX, RDX, RSI, RDI, R8, R9, R10, R11};
    for (int i = 0; i < n; i++) {
        AsmOperand *o = &ops[i];
        if (o->kind == 2) {
            if (!is_const_expr(o->expr)) error_tok(node->tok, "inline assembly: an immediate operand must be a constant");
            continue;
        }
        if (o->kind == 3) {
            /* "m": a frame variable is addressed directly, anything else through a register holding its address */
            Mem m;
            if (o->expr->kind == ND_VAR && o->expr->var->is_local && simple_addr(o->expr, &m) && m.base == RBP) {
                o->direct_mem = true;
                o->mem_disp = m.disp;
                continue;
            }
        }
        if (o->reg >= 0) continue;
        int pick = -1;
        for (size_t k = 0; k < sizeof pool / sizeof pool[0]; k++) {
            int r = pool[k];
            if (!(used & (1u << r)) && !(clobbered & (1u << r))) {
                pick = r;
                break;
            }
        }
        if (pick < 0) error_tok(node->tok, "inline assembly: out of registers");
        o->reg = pick;
        used |= 1u << pick;
    }
    for (int i = 0; i < n; i++) {
        AsmOperand *o = &ops[i];
        if (o->match >= 0 && o->match < n && o->reg < 0) o->reg = ops[o->match].reg;
    }

    for (int i = 0; i < n; i++)
        if (ops[i].kind == 1 && ops[i].reg >= 0 && (ops[i].reg == RBX || ops[i].reg >= R12)) clobbered |= 1u << ops[i].reg;

    /* callee-saved registers the template says it clobbers are saved around the statement */
    int saved[5], nsaved = 0;
    static const int callee[] = {RBX, R12, R13, R14, R15};
    for (int k = 0; k < 5; k++)
        if (clobbered & (1u << callee[k])) {
            push_r(callee[k]);
            depth++;
            saved[nsaved++] = callee[k];
        }

    /* the values go in: evaluate everything first, then load the registers */
    int nload = 0, load_op[24];
    for (int i = 0; i < n; i++) {
        AsmOperand *o = &ops[i];
        bool needs_value = false;
        if (!o->is_out) needs_value = o->kind == 1 || (o->kind == 3 && !o->direct_mem);
        else needs_value = (o->rw && o->kind == 1) || (o->kind == 3 && !o->direct_mem);
        if (o->match >= 0 && !o->is_out) needs_value = true;
        if (!needs_value || o->reg < 0) continue;
        if (o->kind == 3) {
            gen_addr(o->expr);
        } else {
            gen_expr(o->expr);
            if (is_flonum(o->expr->ty)) error_tok(node->tok, "inline assembly: floating point operands are not supported by mcc");
        }
        push();
        load_op[nload++] = i;
    }
    for (int k = nload - 1; k >= 0; k--) pop(ops[load_op[k]].reg);

    /* the template with the operands substituted */
    Buf text = {0};
    const char *t = node->str;
    for (; *t; t++) {
        if (*t != '%') {
            buf_u8(&text, *t);
            continue;
        }
        t++;
        if (*t == '%') {
            buf_u8(&text, '%');
            continue;
        }
        if (*t == '=') {
            char tmp[16];
            snprintf(tmp, sizeof tmp, "%d", unique_counter++);
            buf_str(&text, tmp);
            continue;
        }
        char modifier = 0;
        if (isalpha((u8)*t) && (isdigit((u8)t[1]) || t[1] == '[')) modifier = *t++;
        int idx = -1;
        if (*t == '[') {
            const char *e = strchr(t, ']');
            if (!e) error_tok(node->tok, "inline assembly: bad operand name");
            char *nm = xstrndup(t + 1, (size_t)(e - t - 1));
            for (int i = 0; i < n; i++)
                if (ops[i].name && !strcmp(ops[i].name, nm)) idx = i;
            t = e;
        } else if (isdigit((u8)*t)) {
            idx = *t - '0';
        }
        if (idx < 0 || idx >= n) error_tok(node->tok, "inline assembly: no such operand");
        asm_operand_text(&text, &ops[idx], modifier);
    }
    buf_u8(&text, 0);
    asm_assemble((char *)text.p, node->tok);

    /* the values come out */
    int nsave = 0, save_op[24];
    for (int i = 0; i < ai->nout; i++) {
        AsmOperand *o = &ops[i];
        if (o->kind != 1) continue;
        save_op[nsave++] = i;
    }
    bool simple = true;
    Mem mems[24];
    for (int k = 0; k < nsave; k++) {
        Node *lv = ops[save_op[k]].expr;
        if (!simple_addr(lv, &mems[k]) || mems[k].base != RBP) simple = false;
    }
    if (simple) {
        for (int k = 0; k < nsave; k++) {
            AsmOperand *o = &ops[save_op[k]];
            store_mem(mems[k], o->reg, o->expr->ty->size);
        }
    } else {
        for (int k = 0; k < nsave; k++) {
            push_r(ops[save_op[k]].reg);
            depth++;
        }
        for (int k = nsave - 1; k >= 0; k--) {
            AsmOperand *o = &ops[save_op[k]];
            gen_addr(o->expr);
            pop(RDX);
            store_mem(mem_reg(RAX, 0), RDX, o->expr->ty->size);
        }
    }

    for (int k = nsaved - 1; k >= 0; k--) {
        pop_r(saved[k]);
        depth--;
    }
}
