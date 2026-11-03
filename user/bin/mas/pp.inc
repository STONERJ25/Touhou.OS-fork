/* pp.inc -- the preprocessor, the part of NASM's that the kernel's assembly files use: %macro with numbered parameters, %+ pasting,
 * %%local labels, %rep, %assign / %define, %if / %elif / %else / %endif (and %ifdef / %ifndef), %include.  Expressions in the preprocessor
 * are plain integer arithmetic over numbers and %assign names (ectx.pp_only). */

typedef struct SMacro {
    char *name;
    TokList body;
    struct SMacro *next;
} SMacro;

typedef struct Macro {
    char *name;
    int nparams;
    char **body;
    int nbody;
    struct Macro *next;
} Macro;

static SMacro *smacros;
static Macro *macros;
static int macro_serial = 1;            /* NASM numbers the %%local labels of its macro calls from 2 */
static char *inc_dirs[32];
static int ninc_dirs;

typedef struct {
    int active;     /* the lines of this branch are read */
    int taken;      /* some branch of this %if has been true */
    int parent;     /* the enclosing region is active */
} PCond;

static PCond pconds[64];
static int npconds;

/* collecting the body of a %macro or a %rep */
static int collecting;          /* 0 no, 1 macro, 2 rep */
static Macro *def_macro;
static char **coll;
static int ncoll, ccoll;
static int coll_depth;
static i64 rep_count;
static int include_depth;

static void asm_tokens(TokList *l);
static void pp_line(const char *text);

static void coll_add(const char *text) {
    if (ncoll == ccoll) {
        ccoll = ccoll ? ccoll * 2 : 32;
        coll = xrealloc(coll, (size_t)ccoll * sizeof(char *));
    }
    coll[ncoll++] = xstrdup(text);
}

static SMacro *smacro_find(const char *name) {
    for (SMacro *m = smacros; m; m = m->next)
        if (!strcmp(m->name, name)) return m;
    return NULL;
}

static void smacro_set(const char *name, TokList body) {
    SMacro *m = smacro_find(name);
    if (!m) {
        m = xcalloc(sizeof *m);
        m->name = xstrdup(name);
        m->next = smacros;
        smacros = m;
    }
    m->body = body;
}

static void smacro_undef(const char *name) {
    SMacro **pp = &smacros;
    for (; *pp; pp = &(*pp)->next)
        if (!strcmp((*pp)->name, name)) {
            *pp = (*pp)->next;
            return;
        }
}

static Macro *macro_find(const char *name) {
    for (Macro *m = macros; m; m = m->next)
        if (!strcmp(m->name, name)) return m;
    return NULL;
}

/* single-line macros: every identifier that names one is replaced by its body (repeatedly, to a depth) */
static TokList expand_smacros(TokList in, int depth) {
    if (depth > 32) fatal("macro expansion goes too deep (%%define loop?)");
    TokList out = {0};
    for (int i = 0; i < in.n; i++) {
        Tok t = in.t[i];
        SMacro *m = t.kind == T_ID ? smacro_find(t.text) : NULL;
        if (!m) {
            tl_push(&out, t);
            continue;
        }
        TokList body = {0};
        for (int k = 0; k < m->body.n; k++) tl_push(&body, m->body.t[k]);
        TokList exp = expand_smacros(body, depth + 1);
        for (int k = 0; k < exp.n; k++) {
            Tok b = exp.t[k];
            if (k == 0) b.space = t.space;
            tl_push(&out, b);
        }
    }
    return out;
}

/* a %+ b: the two neighbours become one identifier (or number) */
static void paste_tokens(TokList *l) {
    for (int i = 0; i < l->n; i++) {
        if (l->t[i].kind != T_PASTE) continue;
        if (i == 0 || i + 1 >= l->n) fatal("%%+ needs a token on each side");
        Tok *a = &l->t[i - 1], *b = &l->t[i + 1];
        char *ta = tl_text(l, i - 1, i), *tb = tl_text(l, i + 1, i + 2);
        Tok r = mk_tok(T_ID, NULL, 0);
        r.text = xfmt("%s%s", ta, tb);
        r.space = a->space;
        if (a->kind == T_NUM && b->kind == T_NUM) {
            r.kind = T_NUM;
            r.num = strtoll(r.text, NULL, 10);
        }
        (void)b;
        l->t[i - 1] = r;
        memmove(&l->t[i], &l->t[i + 2], (size_t)(l->n - i - 2) * sizeof(Tok));
        l->n -= 2;
        i--;
    }
}

static i64 pp_eval(TokList *l, int from) {
    int saved = ectx.pp_only;
    ectx.pp_only = 1;
    if (from >= l->n) fatal("expression expected");
    Val v = eval_range(l->t, from, l->n);
    ectx.pp_only = saved;
    if (v.kind != V_CONST) fatal("a preprocessor expression must be a number");
    return v.num;
}

static int icmp(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((u8)*a) != tolower((u8)*b)) break;
    return tolower((u8)*a) - tolower((u8)*b);
}

/* ------------------------------------------------------------------------------------------------ conditionals */

static int pp_active(void) {
    return npconds == 0 || pconds[npconds - 1].active;
}

static int cond_truth(const char *kw, TokList *l) {
    if (!icmp(kw, "%ifdef") || !icmp(kw, "%ifndef")) {
        if (l->n < 2 || l->t[1].kind != T_ID) fatal("%s needs a name", kw);
        int def = smacro_find(l->t[1].text) != NULL;
        return !icmp(kw, "%ifdef") ? def : !def;
    }
    TokList e = expand_smacros(*l, 0);
    return pp_eval(&e, 1) != 0;
}

static int is_if_word(const char *w) {
    return !icmp(w, "%if") || !icmp(w, "%ifdef") || !icmp(w, "%ifndef");
}

/* ------------------------------------------------------------------------------------------------ lines */

static void run_rep(void) {
    char **body = coll;
    int n = ncoll;
    i64 count = rep_count;
    coll = NULL;
    ncoll = ccoll = 0;
    if (count < 0 || count > 1000000) fatal("%%rep count %lld is out of range", count);
    for (i64 i = 0; i < count; i++)
        for (int k = 0; k < n; k++) pp_line(body[k]);
}

static void define_macro_end(void) {
    def_macro->body = coll;
    def_macro->nbody = ncoll;
    def_macro->next = macros;
    macros = def_macro;
    def_macro = NULL;
    coll = NULL;
    ncoll = ccoll = 0;
}

static void expand_macro_call(Macro *m, TokList *l, int first) {
    /* arguments: the rest of the line split at top-level commas */
    TokList args[10];
    memset(args, 0, sizeof args);
    int nargs = 0, depth = 0;
    int i = first + 1;
    if (i < l->n) nargs = 1;
    for (; i < l->n; i++) {
        Tok t = l->t[i];
        if (t.kind == T_OP && (!strcmp(t.text, "(") || !strcmp(t.text, "[") || !strcmp(t.text, "{"))) depth++;
        else if (t.kind == T_OP && (!strcmp(t.text, ")") || !strcmp(t.text, "]") || !strcmp(t.text, "}"))) depth--;
        else if (t.kind == T_OP && !strcmp(t.text, ",") && depth == 0) {
            if (++nargs > 9) fatal("macro '%s' called with too many parameters", m->name);
            continue;
        }
        if (nargs == 0) fatal("internal: argument without a slot");
        if (args[nargs - 1].n == 0) t.space = 0;
        tl_push(&args[nargs - 1], t);
    }
    if (nargs != m->nparams) fatal("macro '%s' expects %d parameters, not %d", m->name, m->nparams, nargs);
    int serial = ++macro_serial;
    for (int b = 0; b < m->nbody; b++) {
        TokList body = tokenize(m->body[b]);
        TokList out = {0};
        for (int k = 0; k < body.n; k++) {
            Tok t = body.t[k];
            if (t.kind == T_PARAM) {
                if (t.num == 0) {
                    tl_push(&out, mk_tok(T_NUM, NULL, nargs));
                    continue;
                }
                if (t.num > m->nparams) fatal("macro '%s' has no parameter %%%d", m->name, (int)t.num);
                TokList *a = &args[t.num - 1];
                for (int q = 0; q < a->n; q++) {
                    Tok x = a->t[q];
                    if (q == 0) x.space = t.space;
                    tl_push(&out, x);
                }
            } else if (t.kind == T_ID && t.text[0] == '%' && t.text[1] == '%') {
                t.text = xfmt("..@%d.%s", serial, t.text + 2);
                tl_push(&out, t);
            } else {
                tl_push(&out, t);
            }
        }
        char *text = tl_text(&out, 0, out.n);
        pp_line(text);
    }
}

static void read_source_file(const char *path, const char *as);

static void pp_directive(TokList *l) {
    const char *kw = l->t[0].text;

    /* conditionals are processed even in an inactive region, to keep their nesting right */
    if (is_if_word(kw)) {
        if (npconds >= 64) fatal("%%if nested too deeply");
        PCond *c = &pconds[npconds++];
        c->parent = npconds == 1 ? 1 : pconds[npconds - 2].active;
        c->active = 0;
        c->taken = 0;
        if (c->parent) {
            c->active = cond_truth(kw, l);
            c->taken = c->active;
        }
        return;
    }
    if (!icmp(kw, "%elif")) {
        if (!npconds) fatal("%%elif without %%if");
        PCond *c = &pconds[npconds - 1];
        if (c->parent && !c->taken) {
            c->active = cond_truth("%if", l);
            c->taken = c->active;
        } else {
            c->active = 0;
        }
        return;
    }
    if (!icmp(kw, "%else")) {
        if (!npconds) fatal("%%else without %%if");
        PCond *c = &pconds[npconds - 1];
        c->active = c->parent && !c->taken;
        c->taken = 1;
        return;
    }
    if (!icmp(kw, "%endif")) {
        if (!npconds) fatal("%%endif without %%if");
        npconds--;
        return;
    }
    if (!pp_active()) return;

    if (!icmp(kw, "%assign")) {
        if (l->n < 3 || l->t[1].kind != T_ID) fatal("%%assign NAME EXPRESSION");
        TokList e = {0};
        for (int i = 2; i < l->n; i++) tl_push(&e, l->t[i]);
        TokList x = expand_smacros(e, 0);
        TokList body = {0};
        tl_push(&body, mk_tok(T_NUM, NULL, pp_eval(&x, 0)));
        smacro_set(l->t[1].text, body);
        return;
    }
    if (!icmp(kw, "%define") || !icmp(kw, "%xdefine")) {
        if (l->n < 2 || l->t[1].kind != T_ID) fatal("%%define NAME BODY");
        TokList body = {0};
        for (int i = 2; i < l->n; i++) tl_push(&body, l->t[i]);
        if (!icmp(kw, "%xdefine")) body = expand_smacros(body, 0);
        smacro_set(l->t[1].text, body);
        return;
    }
    if (!icmp(kw, "%undef")) {
        if (l->n < 2 || l->t[1].kind != T_ID) fatal("%%undef NAME");
        smacro_undef(l->t[1].text);
        return;
    }
    if (!icmp(kw, "%macro")) {
        if (l->n < 3 || l->t[1].kind != T_ID || l->t[2].kind != T_NUM) fatal("%%macro NAME NUMBER_OF_PARAMETERS");
        if (l->n > 3) fatal("%%macro with default parameters or ranges is not supported");
        def_macro = xcalloc(sizeof(Macro));
        def_macro->name = xstrdup(l->t[1].text);
        def_macro->nparams = (int)l->t[2].num;
        collecting = 1;
        coll_depth = 0;
        return;
    }
    if (!icmp(kw, "%rep")) {
        TokList x = expand_smacros(*l, 0);
        rep_count = pp_eval(&x, 1);
        collecting = 2;
        coll_depth = 0;
        return;
    }
    if (!icmp(kw, "%endmacro") || !icmp(kw, "%endrep")) fatal("%s without its opening directive", kw);
    if (!icmp(kw, "%include")) {
        if (l->n < 2 || l->t[1].kind != T_STR) fatal("%%include \"file\"");
        read_source_file(l->t[1].text, NULL);
        return;
    }
    if (!icmp(kw, "%error")) {
        fatal("%%error: %s", l->n > 1 ? tl_text(l, 1, l->n) : "");
    }
    fatal("unknown preprocessor directive '%s'", kw);
}

static void pp_line(const char *text) {
    if (collecting) {
        /* inside %macro ... %endmacro or %rep ... %endrep: lines are kept as they are */
        TokList l = tokenize(text);
        const char *kw = l.n && l.t[0].kind == T_ID && l.t[0].text[0] == '%' ? l.t[0].text : "";
        if (collecting == 1) {
            if (!icmp(kw, "%endmacro")) {
                collecting = 0;
                define_macro_end();
                return;
            }
        } else {
            if (!icmp(kw, "%rep")) coll_depth++;
            else if (!icmp(kw, "%endrep")) {
                if (coll_depth-- == 0) {
                    collecting = 0;
                    run_rep();
                    return;
                }
            }
        }
        coll_add(text);
        return;
    }
    TokList l = tokenize(text);
    if (!l.n) return;
    if (l.t[0].kind == T_ID && l.t[0].text[0] == '%' && l.t[0].text[1] != '%') {
        pp_directive(&l);
        return;
    }
    if (!pp_active()) return;

    TokList x = expand_smacros(l, 0);
    paste_tokens(&x);
    /* a multi-line macro call, perhaps after a label */
    int first = 0;
    if (x.n >= 2 && x.t[0].kind == T_ID && x.t[1].kind == T_OP && !strcmp(x.t[1].text, ":") && x.n >= 3) first = 2;
    if (first < x.n && x.t[first].kind == T_ID) {
        Macro *m = macro_find(x.t[first].text);
        if (m) {
            if (first == 2) {
                TokList lab = {0};
                tl_push(&lab, x.t[0]);
                tl_push(&lab, x.t[1]);
                asm_tokens(&lab);
            }
            expand_macro_call(m, &x, first);
            return;
        }
    }
    asm_tokens(&x);
}

/* ------------------------------------------------------------------------------------------------------- files */

static char *read_whole(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    Buf b = {0};
    u8 tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) buf_put(&b, tmp, n);
    fclose(f);
    if (len) *len = b.len;
    buf_u8(&b, 0);
    return (char *)b.p;
}

/* looks for a file the way NASM does: as given, then in each -I directory (the directory text and the name are joined as written) */
static char *find_file(const char *name) {
    FILE *f = fopen(name, "rb");
    if (f) {
        fclose(f);
        return xstrdup(name);
    }
    for (int i = 0; i < ninc_dirs; i++) {
        char *p = xfmt("%s%s", inc_dirs[i], name);
        f = fopen(p, "rb");
        if (f) {
            fclose(f);
            return p;
        }
        free(p);
        p = xfmt("%s/%s", inc_dirs[i], name);
        f = fopen(p, "rb");
        if (f) {
            fclose(f);
            return p;
        }
        free(p);
    }
    return NULL;
}

static char *dep_files[256];
static int ndep_files;

static void note_dependency(const char *path) {
    if (ndep_files < 256) dep_files[ndep_files++] = xstrdup(path);
}

static void read_source_file(const char *path, const char *as) {
    char *found = find_file(path);
    if (!found) fatal("cannot open '%s'", path);
    if (include_depth >= 16) fatal("includes nested too deeply");
    size_t len;
    char *text = read_whole(found, &len);
    if (!text) fatal("cannot read '%s'", found);
    if (include_depth > 0) note_dependency(found);
    const char *saved_file = cur_file;
    int saved_line = cur_line;
    cur_file = as ? as : found;
    include_depth++;
    int line_no = 0;
    Buf logical = {0};
    char *p = text;
    while (*p) {
        char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        line_no++;
        cur_line = line_no;
        size_t m = n;
        if (m && p[m - 1] == '\r') m--;
        if (m && p[m - 1] == '\\') {                       /* a backslash joins the next line */
            buf_put(&logical, p, m - 1);
        } else {
            buf_put(&logical, p, m);
            buf_u8(&logical, 0);
            pp_line((char *)logical.p);
            logical.len = 0;
        }
        p += n + (nl ? 1 : 0);
    }
    if (logical.len) {
        buf_u8(&logical, 0);
        pp_line((char *)logical.p);
    }
    include_depth--;
    cur_file = saved_file;
    cur_line = saved_line;
    free(text);
    free(found);
}
