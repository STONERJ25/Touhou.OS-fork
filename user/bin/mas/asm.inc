/* asm.inc -- sections, items (what a source line turns into), operands, directives. */

typedef struct {
    u32 off;        /* in the section */
    int type;       /* R_X86_64_* */
    Sym *sym;       /* the symbol the relocation names, or NULL ... */
    int secsym;     /* ... and then the section symbol of this section (labels of this object are referred to through their section) */
    i64 addend;
} Reloc;

typedef struct {
    char *name;
    int alloc, write, exec, nobits, tls;
    int entsize;
    i64 align;
    Buf data;       /* PROGBITS contents */
    i64 size;       /* total size (for NOBITS the only thing there is) */
    Reloc *rel;
    int nrel, crel;
    int shndx;      /* section header index, set by the writer */
    int symidx;     /* index of its section symbol in .symtab */
} Section;

static Section secs[32];
static int nsecs;

enum { K_LABEL, K_EQU, K_DATA, K_RES, K_ALIGN, K_INSN, K_INCBIN };

typedef struct {
    int kind;       /* 0 none, 1 register, 2 immediate, 3 memory */
    int reg, bits, hi8;
    Tok *e;         /* the expression of an immediate, or of a memory operand's displacement */
    int ne;
    int base, index, scale;     /* memory: register numbers or -1 */
    int rel;        /* memory: 1 [rel x], 0 [abs x], -1 follow `default` */
    int msize;      /* size keyword before the operand (byte word dword qword), 0 none */
    int is_short, is_near;
} Op;

typedef struct {
    int kind;
    int sec;
    const char *file;
    int line;
    Sym *sym;               /* label, equ */
    Tok *e;                 /* equ expression, align amount, res count */
    int ne;
    int dsize;              /* data and reserve: bytes per unit */
    Tok **de;               /* data: one token slice per item */
    int *dn;
    int ndata;
    char mn[16];            /* instruction */
    Op op[3];
    int nops, rep, lock;
    u8 *bytes;              /* incbin */
    size_t len;
    int grow;               /* a short branch whose target is out of range: needs the long form */
    int near_;              /* the long form is in use */
    i64 pos;                /* offset in its section in the current layout */
    int size;
} Item;

static Item *items;
static int nitems, citems;
static int cur_sec = -1;
static char *last_label = "";
static int default_rel;     /* `default rel` */

static Item *new_item(int kind) {
    if (nitems == citems) {
        citems = citems ? citems * 2 : 256;
        items = xrealloc(items, (size_t)citems * sizeof(Item));
    }
    Item *it = &items[nitems++];
    memset(it, 0, sizeof *it);
    it->kind = kind;
    it->sec = cur_sec;
    it->file = cur_file;
    it->line = cur_line;
    return it;
}

static Tok *tok_dup(const Tok *t, int n) {
    Tok *r = xmalloc((size_t)(n ? n : 1) * sizeof(Tok));
    if (n) memcpy(r, t, (size_t)n * sizeof(Tok));
    return r;
}

/* ------------------------------------------------------------------------------------------------------- sections */

static int sec_find(const char *name) {
    for (int i = 0; i < nsecs; i++)
        if (!strcmp(secs[i].name, name)) return i;
    return -1;
}

static int sec_select(const char *name, Tok *attr, int nattr) {
    int idx = sec_find(name);
    int fresh = idx < 0;
    if (fresh) {
        if (nsecs >= 32) fatal("too many sections");
        idx = nsecs++;
        Section *s = &secs[idx];
        memset(s, 0, sizeof *s);
        s->name = xstrdup(name);
        s->align = 1;
        s->alloc = 1;
        s->write = s->exec = 0;
        if (!strcmp(name, ".text")) s->exec = 1, s->align = 16;
        else if (!strcmp(name, ".data")) s->write = 1, s->align = 4;
        else if (!strcmp(name, ".bss")) s->write = 1, s->nobits = 1, s->align = 4;
        else if (!strncmp(name, ".rodata", 7)) s->align = 4;
        else if (!strcmp(name, ".tdata")) s->write = 1, s->tls = 1;
        else if (!strcmp(name, ".tbss")) s->write = 1, s->tls = 1, s->nobits = 1;
        else if (!strcmp(name, ".init_array") || !strcmp(name, ".fini_array") || !strcmp(name, ".preinit_array")) s->entsize = 8;
    }
    Section *s = &secs[idx];
    if (!fresh) return idx;                             /* attributes of an existing section are ignored (NASM warns) */
    for (int i = 0; i < nattr; i++) {
        if (attr[i].kind != T_ID) fatal("section attribute expected");
        const char *a = attr[i].text;
        if (!icmp(a, "alloc")) s->alloc = 1;
        else if (!icmp(a, "noalloc")) s->alloc = 0;
        else if (!icmp(a, "exec")) s->exec = 1;
        else if (!icmp(a, "noexec")) s->exec = 0;
        else if (!icmp(a, "write")) s->write = 1;
        else if (!icmp(a, "nowrite")) s->write = 0;
        else if (!icmp(a, "progbits")) s->nobits = 0;
        else if (!icmp(a, "nobits")) s->nobits = 1;
        else if (!icmp(a, "align") && i + 2 < nattr && attr[i + 1].kind == T_OP && !strcmp(attr[i + 1].text, "=") && attr[i + 2].kind == T_NUM) {
            s->align = attr[i + 2].num;
            i += 2;
        } else fatal("unknown section attribute '%s'", a);
    }
    return idx;
}

/* ------------------------------------------------------------------------------------------------------- registers */

typedef struct {
    const char *name;
    int num, bits, hi8;
} RegName;

static const RegName reg_names[] = {
    {"rax", 0, 64, 0}, {"rcx", 1, 64, 0}, {"rdx", 2, 64, 0}, {"rbx", 3, 64, 0}, {"rsp", 4, 64, 0}, {"rbp", 5, 64, 0}, {"rsi", 6, 64, 0}, {"rdi", 7, 64, 0},
    {"r8", 8, 64, 0}, {"r9", 9, 64, 0}, {"r10", 10, 64, 0}, {"r11", 11, 64, 0}, {"r12", 12, 64, 0}, {"r13", 13, 64, 0}, {"r14", 14, 64, 0}, {"r15", 15, 64, 0},
    {"eax", 0, 32, 0}, {"ecx", 1, 32, 0}, {"edx", 2, 32, 0}, {"ebx", 3, 32, 0}, {"esp", 4, 32, 0}, {"ebp", 5, 32, 0}, {"esi", 6, 32, 0}, {"edi", 7, 32, 0},
    {"r8d", 8, 32, 0}, {"r9d", 9, 32, 0}, {"r10d", 10, 32, 0}, {"r11d", 11, 32, 0}, {"r12d", 12, 32, 0}, {"r13d", 13, 32, 0}, {"r14d", 14, 32, 0}, {"r15d", 15, 32, 0},
    {"ax", 0, 16, 0}, {"cx", 1, 16, 0}, {"dx", 2, 16, 0}, {"bx", 3, 16, 0}, {"sp", 4, 16, 0}, {"bp", 5, 16, 0}, {"si", 6, 16, 0}, {"di", 7, 16, 0},
    {"r8w", 8, 16, 0}, {"r9w", 9, 16, 0}, {"r10w", 10, 16, 0}, {"r11w", 11, 16, 0}, {"r12w", 12, 16, 0}, {"r13w", 13, 16, 0}, {"r14w", 14, 16, 0}, {"r15w", 15, 16, 0},
    {"al", 0, 8, 0}, {"cl", 1, 8, 0}, {"dl", 2, 8, 0}, {"bl", 3, 8, 0}, {"spl", 4, 8, 0}, {"bpl", 5, 8, 0}, {"sil", 6, 8, 0}, {"dil", 7, 8, 0},
    {"r8b", 8, 8, 0}, {"r9b", 9, 8, 0}, {"r10b", 10, 8, 0}, {"r11b", 11, 8, 0}, {"r12b", 12, 8, 0}, {"r13b", 13, 8, 0}, {"r14b", 14, 8, 0}, {"r15b", 15, 8, 0},
    {"ah", 4, 8, 1}, {"ch", 5, 8, 1}, {"dh", 6, 8, 1}, {"bh", 7, 8, 1},
};

static const RegName *find_reg(const char *name) {
    for (size_t i = 0; i < sizeof reg_names / sizeof reg_names[0]; i++)
        if (!icmp(name, reg_names[i].name)) return &reg_names[i];
    return NULL;
}

/* ------------------------------------------------------------------------------------------------------- operands */

static int size_word(const char *w) {
    if (!icmp(w, "byte")) return 1;
    if (!icmp(w, "word")) return 2;
    if (!icmp(w, "dword")) return 4;
    if (!icmp(w, "qword")) return 8;
    return 0;
}

/* the inside of [ ... ]: registers (base, index*scale), the keywords rel / abs, and a displacement made of everything else */
static void parse_mem(Op *o, Tok *t, int n) {
    Tok *disp = xmalloc((size_t)(n + 2) * sizeof(Tok));
    int nd = 0, sign = 1, i = 0;
    while (i < n) {
        Tok *x = &t[i];
        if (x->kind == T_ID && !icmp(x->text, "rel")) { o->rel = 1; i++; continue; }
        if (x->kind == T_ID && !icmp(x->text, "abs")) { o->rel = 0; i++; continue; }
        if (x->kind == T_OP && (!strcmp(x->text, "+") || !strcmp(x->text, "-"))) {
            if (x->text[0] == '-') sign = -sign;
            i++;
            continue;
        }
        const RegName *r = NULL;
        int scale = 1, used = 0, explicit_scale = 0;
        if (x->kind == T_ID && (r = find_reg(x->text))) {
            used = 1;
            if (i + 2 < n && tok_is(&t[i + 1], T_OP, "*") && t[i + 2].kind == T_NUM) {
                scale = (int)t[i + 2].num;
                used = 3;
                explicit_scale = 1;
            }
        } else if (x->kind == T_NUM && i + 2 < n && tok_is(&t[i + 1], T_OP, "*") && t[i + 2].kind == T_ID && find_reg(t[i + 2].text)) {
            r = find_reg(t[i + 2].text);
            scale = (int)x->num;
            used = 3;
            explicit_scale = 1;
        }
        if (r) {
            if (r->bits != 64) fatal("only 64-bit registers can address memory in this assembler");
            if (sign < 0) fatal("a register cannot be subtracted in an address");
            if (scale != 1 && scale != 2 && scale != 4 && scale != 8) fatal("scale %d is not 1, 2, 4 or 8", scale);
            if (scale == 1 && !explicit_scale && o->base < 0) o->base = r->num;     /* a register written with *N is the index, even for *1 */
            else if (o->index < 0) {
                o->index = r->num;
                o->scale = scale;
            } else fatal("too many registers in an address");
            i += used;
            sign = 1;
            continue;
        }
        /* a displacement term: it runs to the next + or - outside brackets */
        if (nd > 0 || sign < 0) disp[nd++] = mk_tok(T_OP, sign < 0 ? "-" : "+", 0);
        int depth = 0, k = i;
        for (; k < n; k++) {
            if (t[k].kind == T_OP && !strcmp(t[k].text, "(")) depth++;
            else if (t[k].kind == T_OP && !strcmp(t[k].text, ")")) depth--;
            else if (depth == 0 && k > i && t[k].kind == T_OP && (!strcmp(t[k].text, "+") || !strcmp(t[k].text, "-"))) break;
            disp[nd++] = t[k];
        }
        i = k;
        sign = 1;
    }
    if (o->base < 0 && o->index >= 0 && o->scale == 1) {                /* [rax*1] is [rax] */
        o->base = o->index;
        o->index = -1;
    }
    if (o->index == 4) {                                                /* rsp cannot be an index: swap it with the base */
        if (o->scale != 1 || o->base < 0) fatal("rsp cannot be an index register");
        o->index = o->base;
        o->base = 4;
    }
    o->e = disp;
    o->ne = nd;
}

static Op parse_operand(Tok *t, int n) {
    Op o;
    memset(&o, 0, sizeof o);
    o.base = o.index = -1;
    o.rel = -1;
    o.scale = 1;
    int i = 0;
    while (i < n && t[i].kind == T_ID) {
        int sz = size_word(t[i].text);
        if (sz) o.msize = sz;
        else if (!icmp(t[i].text, "short")) o.is_short = 1;
        else if (!icmp(t[i].text, "near")) o.is_near = 1;
        else if (!icmp(t[i].text, "strict")) {
        } else break;
        i++;
    }
    if (i >= n) fatal("operand expected");
    if (n - i == 1 && t[i].kind == T_ID) {
        const RegName *r = find_reg(t[i].text);
        if (r) {
            o.kind = 1;
            o.reg = r->num;
            o.bits = r->bits;
            o.hi8 = r->hi8;
            return o;
        }
    }
    if (tok_is(&t[i], T_OP, "[")) {
        if (!tok_is(&t[n - 1], T_OP, "]")) fatal("']' expected");
        o.kind = 3;
        parse_mem(&o, t + i + 1, n - i - 2);
        return o;
    }
    o.kind = 2;
    o.e = tok_dup(t + i, n - i);
    o.ne = n - i;
    return o;
}

/* ------------------------------------------------------------------------------------------------------- directives */

static const char *data_words[] = {"db", "dw", "dd", "dq", "resb", "resw", "resd", "resq", "incbin", NULL};

static int is_data_word(const char *w) {
    for (int i = 0; data_words[i]; i++)
        if (!icmp(w, data_words[i])) return 1;
    return 0;
}

/* .local names belong to the last label that is not local: ".go" after "longjmp:" is "longjmp.go".  Names starting with ".." are never local. */
static int is_local_name(const char *s) {
    return s[0] == '.' && s[1] != '.';
}

static void qualify_locals(Tok *t, int n) {
    for (int i = 0; i < n; i++)
        if (t[i].kind == T_ID && is_local_name(t[i].text) && strcmp(t[i].text, ".")) t[i].text = xfmt("%s%s", last_label, t[i].text);
}

static void define_label(const char *name_in) {
    char *name;
    if (is_local_name(name_in)) name = xfmt("%s%s", last_label, name_in);
    else {
        name = xstrdup(name_in);
        if (name[0] != '.') last_label = name;
    }
    Sym *s = sym_get(name);
    if (s->labeled) fatal("symbol '%s' defined twice", name);
    if (s->is_extern) fatal("symbol '%s' is declared extern and defined here", name);
    s->labeled = 1;
    Item *it = new_item(K_LABEL);
    it->sym = s;
    if (!s->created) s->created = ++sym_counter;
}

static int split_commas(Tok *t, int n, Tok **out_starts, int *out_lens, int max) {
    int cnt = 0, depth = 0, start = 0;
    for (int i = 0; i <= n; i++) {
        int end = i == n;
        if (!end && t[i].kind == T_OP) {
            if (!strcmp(t[i].text, "(") || !strcmp(t[i].text, "[")) depth++;
            else if (!strcmp(t[i].text, ")") || !strcmp(t[i].text, "]")) depth--;
        }
        if (end || (depth == 0 && tok_is(&t[i], T_OP, ","))) {
            if (cnt >= max) fatal("too many operands");
            out_starts[cnt] = t + start;
            out_lens[cnt] = i - start;
            cnt++;
            start = i + 1;
        }
    }
    return cnt;
}

static void asm_instruction(Tok *t, int n, int rep, int lock);
static void asm_directive_or_insn(Tok *t, int n);

static void asm_times(Tok *t, int n) {
    /* times COUNT rest-of-line: the count must be a number the preprocessor-style evaluator can compute now */
    int i = 1;
    while (i < n && !(t[i].kind == T_ID && (find_reg(t[i].text) == NULL) && (is_data_word(t[i].text) || !icmp(t[i].text, "nop") || size_word(t[i].text) || 0)) && i < n - 1) {
        /* the count is the longest prefix that is an expression: stop at the first identifier that names a directive or instruction */
        if (t[i].kind == T_ID && !sym_find(t[i].text) && !find_reg(t[i].text) && !smacro_find(t[i].text) && i > 1) break;
        i++;
    }
    int saved = ectx.pp_only;
    ectx.pp_only = 1;
    Val v = eval_range(t, 1, i);
    ectx.pp_only = saved;
    if (v.kind != V_CONST) fatal("times needs a constant count");
    if (v.num < 0 || v.num > (1 << 20)) fatal("times count %lld out of range", v.num);
    for (i64 k = 0; k < v.num; k++) asm_directive_or_insn(t + i, n - i);
}

static void asm_directive_or_insn(Tok *t, int n) {
    if (n == 0) return;
    if (t[0].kind != T_ID) fatal("unexpected '%s' at the start of a line", t[0].text ? t[0].text : "token");
    const char *w = t[0].text;

    if (!icmp(w, "bits")) {
        if (n != 2 || t[1].kind != T_NUM || t[1].num != 64) fatal("only 'bits 64' is supported");
        return;
    }
    if (!icmp(w, "use64")) return;
    if (!icmp(w, "cpu")) return;
    if (!icmp(w, "default")) {
        for (int i = 1; i < n; i++) {
            if (t[i].kind != T_ID) fatal("default rel / abs");
            if (!icmp(t[i].text, "rel")) default_rel = 1;
            else if (!icmp(t[i].text, "abs")) default_rel = 0;
            else fatal("unknown default '%s'", t[i].text);
        }
        return;
    }
    if (!icmp(w, "section") || !icmp(w, "segment")) {
        if (n < 2 || t[1].kind != T_ID) fatal("section NAME [attributes]");
        cur_sec = sec_select(t[1].text, t + 2, n - 2);
        return;
    }
    if (!icmp(w, "global") || !icmp(w, "extern")) {
        int is_ext = !icmp(w, "extern");
        for (int i = 1; i < n; i++) {
            if (t[i].kind == T_OP && !strcmp(t[i].text, ",")) continue;
            if (t[i].kind == T_OP && !strcmp(t[i].text, ":")) {          /* global name:function / :data — types are not recorded */
                i++;
                continue;
            }
            if (t[i].kind != T_ID) fatal("symbol name expected after %s", w);
            Sym *s = sym_get(t[i].text);
            if (is_ext) {
                if (s->defined) fatal("extern symbol '%s' is defined in this file", s->name);
                s->is_extern = 1;
                if (!s->created) s->created = ++sym_counter;
            } else {
                s->is_global = 1;
                if (npending_globals < 256) pending_globals[npending_globals++] = xstrdup(t[i].text);
            }
        }
        return;
    }
    if (!icmp(w, "times")) {
        asm_times(t, n);
        return;
    }
    if (!icmp(w, "align") || !icmp(w, "alignb")) {
        if (n < 2) fatal("align N");
        if (cur_sec < 0) fatal("align outside a section");
        Item *it = new_item(K_ALIGN);
        it->dsize = !icmp(w, "alignb");                          /* alignb pads with zeros, align with NOPs */
        it->e = tok_dup(t + 1, n - 1);
        it->ne = n - 1;
        return;
    }
    if (is_data_word(w)) {
        if (cur_sec < 0) fatal("'%s' outside a section", w);
        if (!icmp(w, "incbin")) {
            if (n < 2 || t[1].kind != T_STR) fatal("incbin \"file\"");
            char *path = find_file(t[1].text);
            if (!path) fatal("cannot open '%s'", t[1].text);
            note_dependency(path);
            size_t len;
            char *data = read_whole(path, &len);
            if (!data) fatal("cannot read '%s'", path);
            Item *it = new_item(K_INCBIN);
            it->bytes = (u8 *)data;
            it->len = len;
            return;
        }
        int isres = w[0] == 'r' || w[0] == 'R';
        char c = (char)tolower((u8)w[isres ? 3 : 1]);
        int sz = c == 'b' ? 1 : c == 'w' ? 2 : c == 'd' ? 4 : 8;
        if (isres) {
            if (n < 2) fatal("%s needs a count", w);
            Item *it = new_item(K_RES);
            it->dsize = sz;
            it->e = tok_dup(t + 1, n - 1);
            it->ne = n - 1;
            return;
        }
        Tok *starts[256];
        int lens[256];
        int cnt = split_commas(t + 1, n - 1, starts, lens, 256);
        if (n == 1) fatal("%s needs a value", w);
        Item *it = new_item(K_DATA);
        it->dsize = sz;
        it->de = xmalloc((size_t)cnt * sizeof(Tok *));
        it->dn = xmalloc((size_t)cnt * sizeof(int));
        for (int i = 0; i < cnt; i++) {
            if (lens[i] == 0) fatal("empty value in %s", w);
            it->de[i] = tok_dup(starts[i], lens[i]);
            it->dn[i] = lens[i];
        }
        it->ndata = cnt;
        return;
    }
    /* an instruction, perhaps after prefixes */
    int rep = 0, lock = 0, i = 0;
    while (i < n && t[i].kind == T_ID) {
        if (!icmp(t[i].text, "rep") || !icmp(t[i].text, "repe") || !icmp(t[i].text, "repz")) rep = 0xF3;
        else if (!icmp(t[i].text, "repne") || !icmp(t[i].text, "repnz")) rep = 0xF2;
        else if (!icmp(t[i].text, "lock")) lock = 1;
        else break;
        i++;
    }
    if (i >= n) fatal("an instruction is expected after the prefix");
    asm_instruction(t + i, n - i, rep, lock);
}

static void asm_instruction(Tok *t, int n, int rep, int lock) {
    if (cur_sec < 0) cur_sec = sec_select(".text", NULL, 0);
    Item *it = new_item(K_INSN);
    it->rep = rep;
    it->lock = lock;
    if (strlen(t[0].text) >= sizeof it->mn) fatal("unknown instruction '%s'", t[0].text);
    for (size_t k = 0; t[0].text[k]; k++) it->mn[k] = (char)tolower((u8)t[0].text[k]);
    if (n > 1) {
        Tok *starts[4];
        int lens[4];
        int cnt = split_commas(t + 1, n - 1, starts, lens, 4);
        if (cnt > 3) fatal("too many operands");
        for (int i = 0; i < cnt; i++) {
            if (lens[i] == 0) fatal("empty operand");
            it->op[i] = parse_operand(starts[i], lens[i]);
        }
        it->nops = cnt;
    }
}

/* one fully expanded source line: [label[:]] [directive | instruction] */
static void asm_tokens(TokList *l) {
    Tok *t = l->t;
    int n = l->n;
    if (n == 0) return;
    int i = 0;
    if (t[0].kind == T_ID && n >= 2 && tok_is(&t[1], T_OP, ":")) {
        define_label(t[0].text);
        i = 2;
        if (i >= n) return;
    } else if (t[0].kind == T_ID && n >= 2 && t[1].kind == T_ID && !icmp(t[1].text, "equ")) {
        /* NAME equ EXPRESSION */
        if (n < 3) fatal("equ needs a value");
        Sym *s = sym_get(t[0].text);
        if (s->labeled) fatal("symbol '%s' defined twice", s->name);
        s->labeled = 1;
        if (!s->created) s->created = ++sym_counter;
        qualify_locals(t + 2, n - 2);
        Item *it = new_item(K_EQU);
        it->sym = s;
        it->e = tok_dup(t + 2, n - 2);
        it->ne = n - 2;
        return;
    } else if (t[0].kind == T_ID && n >= 2 && t[1].kind == T_ID && is_data_word(t[1].text) && !is_data_word(t[0].text) && !find_reg(t[0].text)) {
        define_label(t[0].text);                                /* a label without a colon in front of a data directive */
        i = 1;
    } else if (t[0].kind == T_ID && n == 1 && !is_data_word(t[0].text) && !find_reg(t[0].text)) {
        /* a lone identifier is a label without its colon unless it is an instruction or directive */
        static const char *lone[] = {"ret", "nop", "cli", "sti", "hlt", "cld", "std", "syscall", "sysret", "ud2", "iretq", "iret", "leave", "pause", "cpuid", "rdtsc",
                                     "rdmsr", "wrmsr", "int3", "swapgs", "lfence", "mfence", "sfence", "pushfq", "popfq", "cqo", "cdq", "cdqe", "movsb", "movsw",
                                     "movsd", "movsq", "stosb", "stosw", "stosd", "stosq", "lodsb", "lodsw", "lodsd", "lodsq", "scasb", "scasw", "scasd", "scasq",
                                     "cmpsb", "cmpsw", "cmpsd", "cmpsq", "clc", "stc", "cmc", "xgetbv", "wbinvd", "invd", "retq", "sysretq", "use64", NULL};
        int is_insn = 0;
        for (int k = 0; lone[k]; k++)
            if (!icmp(t[0].text, lone[k])) is_insn = 1;
        if (!is_insn) {
            define_label(t[0].text);
            return;
        }
    }
    if (i < n && t[i].kind == T_ID) {
        static const char *plain[] = {"section", "segment", "global", "extern", "bits", "default", "cpu", "use64", "incbin", NULL};
        int skip = 0;
        for (int k = 0; plain[k]; k++)
            if (!icmp(t[i].text, plain[k])) skip = 1;
        if (!skip) qualify_locals(t + i + 1, n - i - 1);
    }
    asm_directive_or_insn(t + i, n - i);
}
