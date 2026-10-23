/* ld.inc -- mld, the static linker.  Reads ELF64 relocatable objects and ar archives, resolves symbols, lays the
 * program out like user/user.ld (R-X text, R-- rodata, RW- data + bss, PT_TLS) and writes a static ELF executable.
 *
 * Marisa's rule of linking: whatever sits in the library gets borrowed when somebody asks for it by name.
 */

#define LD_BASE 0x400000ULL
#define LD_PAGE 4096ULL
#define LD_HDR_SPACE 0x1000ULL

enum { K_TEXT, K_RODATA, K_DATA, K_TDATA, K_TBSS, K_BSS, K_NKIND };

typedef struct LdObj LdObj;
typedef struct LdSec LdSec;
typedef struct LdSym LdSym;

struct LdSec {
    LdObj *obj;
    int idx;
    const char *name;
    u32 type;
    u64 flags, size, align;
    const u8 *data;
    int kind;
    u64 addr;
    u64 outoff; /* offset inside the output region of its kind */
    Rela *rela;
    size_t nrela;
};

struct LdSym {
    const char *name;
    LdSec *sec;
    u64 value;
    bool defined, abs, weak, common, tls, referenced;
    u64 size, align;
    LdObj *obj;
};

struct LdObj {
    const char *name;
    u8 *img;
    size_t len;
    Shdr *sh;
    int nsh;
    const char *shstr;
    LdSec **secs;
    ElfSym *syms;
    size_t nsyms;
    const char *strtab;
    LdSym **gsyms;
    int *got_idx;
};

typedef struct {
    char *name;
    u8 *data;
    size_t size;
    bool loaded;
    char **defs;
    int ndefs;
    bool scanned;
} ArMember;

typedef struct {
    char *path;
    ArMember *mem;
    int nmem;
} Archive;

typedef struct {
    LdObj **objs;
    int nobjs;
    Archive **ars;
    int nars;
    Map syms;
    LdSym **symlist;
    int nsymlist;
    int errors;
    LdSec **kind_secs[K_NKIND];
    int kind_n[K_NKIND];
    u64 kind_addr[K_NKIND];
    u64 kind_size[K_NKIND];
    u64 kind_off[K_NKIND];
    u64 kind_align[K_NKIND];
    int ngot;
    u64 got_addr;
    u64 tp_base;
    u64 tls_addr, tls_filesz, tls_memsz, tls_align;
    bool strip;
    bool show_map;
    const char *entry;
} Linker;

static Linker L;

static void ld_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *msg = xvformat(fmt, ap);
    va_end(ap);
    fprintf(stderr, "mld: %s\n", msg);
    L.errors++;
}

static LdSym *ld_sym(const char *name, bool create) {
    LdSym *s = map_get(&L.syms, name);
    if (s || !create) return s;
    s = xcalloc(sizeof *s);
    s->name = name;
    map_put(&L.syms, name, s);
    if ((L.nsymlist & (L.nsymlist - 1)) == 0) L.symlist = xrealloc(L.symlist, (size_t)(L.nsymlist ? L.nsymlist * 2 : 1) * sizeof(LdSym *));
    L.symlist[L.nsymlist++] = s;
    return s;
}

static const char *ld_sec_kind_name(int k) {
    static const char *names[] = {".text", ".rodata", ".data", ".tdata", ".tbss", ".bss"};
    return names[k];
}

static int ld_classify(const char *name, u32 type, u64 flags) {
    if (!(flags & SHF_ALLOC)) return -1;
    if (str_starts(name, ".eh_frame") || str_starts(name, ".note") || str_starts(name, ".comment")) return -1;
    if (type != SHT_PROGBITS && type != SHT_NOBITS && type != 14 /* INIT_ARRAY */ && type != 15 /* FINI_ARRAY */) return -1;
    if (type == SHT_NOBITS) return (flags & SHF_TLS) ? K_TBSS : K_BSS;
    if (flags & SHF_TLS) return K_TDATA;
    if (flags & SHF_EXECINSTR) return K_TEXT;
    if (flags & SHF_WRITE) return K_DATA;
    return K_RODATA;
}

/* Marks a symbol as defined by an object (strong, weak or common) following the usual rules. */
static void ld_define(LdSym *s, LdObj *obj, LdSec *sec, u64 value, bool abs, bool weak, bool common, u64 size, u64 align, bool tls) {
    if (common) {
        if (s->defined && !s->common) return;
        if (s->common) {
            if (size > s->size) s->size = size;
            if (align > s->align) s->align = align;
            return;
        }
        s->common = true;
        s->size = size;
        s->align = align ? align : 1;
        s->obj = obj;
        return;
    }
    if (s->defined && !s->common) {
        if (weak) return;
        if (!s->weak) {
            ld_error("multiple definition of '%s' (first in %s, again in %s) -- Marisa: two of you claim the same book, ze", s->name, s->obj ? s->obj->name : "?", obj->name);
            return;
        }
    }
    s->defined = true;
    s->common = false;
    s->weak = weak;
    s->sec = sec;
    s->value = value;
    s->abs = abs;
    s->tls = tls;
    s->size = size;
    s->obj = obj;
}

static LdObj *ld_add_object(const char *name, u8 *img, size_t len) {
    Ehdr *eh = (Ehdr *)img;
    if (len < sizeof(Ehdr) || memcmp(eh->e_ident, "\177ELF", 4) != 0) {
        ld_error("%s: not an ELF file", name);
        return NULL;
    }
    if (eh->e_ident[4] != 2 || eh->e_ident[5] != 1 || eh->e_machine != 62 || eh->e_type != ET_REL) {
        ld_error("%s: not an x86-64 relocatable object", name);
        return NULL;
    }
    if (eh->e_shoff == 0 || eh->e_shoff + (u64)eh->e_shnum * sizeof(Shdr) > len) {
        ld_error("%s: damaged section table", name);
        return NULL;
    }
    LdObj *o = xcalloc(sizeof *o);
    o->name = name;
    o->img = img;
    o->len = len;
    o->sh = (Shdr *)(img + eh->e_shoff);
    o->nsh = eh->e_shnum;
    o->shstr = (const char *)(img + o->sh[eh->e_shstrndx].sh_offset);
    o->secs = xcalloc((size_t)o->nsh * sizeof(LdSec *));
    for (int i = 1; i < o->nsh; i++) {
        Shdr *s = &o->sh[i];
        if (s->sh_type == SHT_SYMTAB) {
            o->syms = (ElfSym *)(img + s->sh_offset);
            o->nsyms = s->sh_size / sizeof(ElfSym);
            o->strtab = (const char *)(img + o->sh[s->sh_link].sh_offset);
        }
    }
    for (int i = 1; i < o->nsh; i++) {
        Shdr *s = &o->sh[i];
        const char *sname = o->shstr + s->sh_name;
        int kind = ld_classify(sname, s->sh_type, s->sh_flags);
        if (kind < 0) continue;
        LdSec *sec = xcalloc(sizeof *sec);
        sec->obj = o;
        sec->idx = i;
        sec->name = sname;
        sec->type = s->sh_type;
        sec->flags = s->sh_flags;
        sec->size = s->sh_size;
        sec->align = s->sh_addralign ? s->sh_addralign : 1;
        sec->data = s->sh_type == SHT_NOBITS ? NULL : img + s->sh_offset;
        sec->kind = kind;
        o->secs[i] = sec;
    }
    for (int i = 1; i < o->nsh; i++) {
        Shdr *s = &o->sh[i];
        if (s->sh_type != SHT_RELA) continue;
        LdSec *target = (s->sh_info > 0 && (int)s->sh_info < o->nsh) ? o->secs[s->sh_info] : NULL;
        if (!target) continue;
        target->rela = (Rela *)(img + s->sh_offset);
        target->nrela = s->sh_size / sizeof(Rela);
    }
    o->gsyms = xcalloc((o->nsyms + 1) * sizeof(LdSym *));
    o->got_idx = xcalloc((o->nsyms + 1) * sizeof(int));
    for (size_t i = 0; i < o->nsyms; i++) o->got_idx[i] = -1;
    for (size_t i = 1; i < o->nsyms; i++) {
        ElfSym *s = &o->syms[i];
        int bind = ELF64_ST_BIND(s->st_info), type = ELF64_ST_TYPE(s->st_info);
        if (type == STT_SECTION || type == STT_FILE || bind == STB_LOCAL) continue;
        const char *nm = o->strtab + s->st_name;
        if (!*nm) continue;
        LdSym *g = ld_sym(nm, true);
        o->gsyms[i] = g;
        if (s->st_shndx == SHN_UNDEF) {
            if (bind != STB_WEAK) g->referenced = true;
            continue;
        }
        if (s->st_shndx == SHN_COMMON) {
            ld_define(g, o, NULL, 0, false, bind == STB_WEAK, true, s->st_size, s->st_value, false);
            continue;
        }
        if (s->st_shndx == SHN_ABS) {
            ld_define(g, o, NULL, s->st_value, true, bind == STB_WEAK, false, s->st_size, 0, false);
            continue;
        }
        if (s->st_shndx >= o->nsh) continue;
        LdSec *sec = o->secs[s->st_shndx];
        if (!sec) continue; /* lives in a discarded section */
        ld_define(g, o, sec, s->st_value, false, bind == STB_WEAK, false, s->st_size, 0, type == STT_TLS);
    }
    if ((L.nobjs & (L.nobjs - 1)) == 0) L.objs = xrealloc(L.objs, (size_t)(L.nobjs ? L.nobjs * 2 : 1) * sizeof(LdObj *));
    L.objs[L.nobjs++] = o;
    return o;
}

/* ---- archives */

static Archive *ld_read_archive(const char *path, u8 *img, size_t len) {
    Archive *ar = xcalloc(sizeof *ar);
    ar->path = xstrdup(path);
    char *longnames = NULL;
    size_t pos = 8;
    int cap = 0;
    while (pos + 60 <= len) {
        const char *h = (const char *)img + pos;
        char sizebuf[11];
        memcpy(sizebuf, h + 48, 10);
        sizebuf[10] = 0;
        size_t size = (size_t)strtoull(sizebuf, NULL, 10);
        pos += 60;
        if (pos + size > len) {
            ld_error("%s: damaged archive", path);
            break;
        }
        u8 *data = img + pos;
        char *name = NULL;
        size_t dsize = size;
        if (h[0] == '/' && h[1] == ' ') {
            /* symbol table: not needed, the members are scanned for their own symbols */
        } else if (h[0] == '/' && h[1] == '/') {
            longnames = (char *)data;
        } else {
            if (h[0] == '#' && h[1] == '1' && h[2] == '/') {
                size_t n = (size_t)atoi(h + 3);
                name = xstrndup((const char *)data, n);
                char *nul = memchr(name, 0, n);
                (void)nul;
                data += n;
                dsize = size - n;
            } else if (h[0] == '/' && h[1] >= '0' && h[1] <= '9' && longnames) {
                size_t off = (size_t)atoi(h + 1);
                char *e = longnames + off;
                size_t n = 0;
                while (e[n] && e[n] != '/' && e[n] != '\n') n++;
                name = xstrndup(e, n);
            } else {
                size_t n = 0;
                while (n < 16 && h[n] != '/' && h[n] != ' ') n++;
                name = xstrndup(h, n);
            }
            if (ar->nmem == cap) {
                cap = cap ? cap * 2 : 32;
                ar->mem = xrealloc(ar->mem, (size_t)cap * sizeof(ArMember));
            }
            ArMember *m = &ar->mem[ar->nmem++];
            memset(m, 0, sizeof *m);
            m->name = name;
            m->data = data;
            m->size = dsize;
        }
        pos += size + (size & 1);
    }
    return ar;
}

/* Which global symbols does a member define?  (read straight from its symbol table) */
static void ld_scan_member(ArMember *m) {
    m->scanned = true;
    if (m->size < sizeof(Ehdr) || memcmp(m->data, "\177ELF", 4) != 0) return;
    Ehdr *eh = (Ehdr *)m->data;
    if (eh->e_shoff == 0 || eh->e_shoff + (u64)eh->e_shnum * sizeof(Shdr) > m->size) return;
    Shdr *sh = (Shdr *)(m->data + eh->e_shoff);
    for (int i = 1; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_SYMTAB) continue;
        ElfSym *syms = (ElfSym *)(m->data + sh[i].sh_offset);
        size_t n = sh[i].sh_size / sizeof(ElfSym);
        const char *strtab = (const char *)(m->data + sh[sh[i].sh_link].sh_offset);
        for (size_t k = 1; k < n; k++) {
            int bind = ELF64_ST_BIND(syms[k].st_info);
            if (bind == STB_LOCAL || syms[k].st_shndx == SHN_UNDEF) continue;
            if (ELF64_ST_TYPE(syms[k].st_info) == STT_SECTION || ELF64_ST_TYPE(syms[k].st_info) == STT_FILE) continue;
            m->defs = xrealloc(m->defs, (size_t)(m->ndefs + 1) * sizeof(char *));
            m->defs[m->ndefs++] = (char *)(strtab + syms[k].st_name);
        }
    }
}

static bool ld_needed(const char *name) {
    LdSym *s = map_get(&L.syms, name);
    return s && s->referenced && !s->defined && !s->common;
}

/* Pulls archive members in until nothing that is still undefined can be found in any library. */
static void ld_resolve_archives(void) {
    bool again = true;
    while (again) {
        again = false;
        for (int a = 0; a < L.nars; a++) {
            Archive *ar = L.ars[a];
            for (int i = 0; i < ar->nmem; i++) {
                ArMember *m = &ar->mem[i];
                if (m->loaded) continue;
                if (!m->scanned) ld_scan_member(m);
                bool want = false;
                for (int k = 0; k < m->ndefs && !want; k++)
                    if (ld_needed(m->defs[k])) want = true;
                if (!want) continue;
                m->loaded = true;
                u8 *copy = xmalloc(m->size + 16);
                memcpy(copy, m->data, m->size);
                char *disp = xformat("%s(%s)", ar->path, m->name);
                ld_add_object(disp, copy, m->size);
                again = true;
            }
        }
    }
}

/* ---- layout */

static u64 align_up_u64(u64 v, u64 a) {
    return a > 1 ? (v + a - 1) / a * a : v;
}

static void ld_add_kind_sec(LdSec *sec) {
    int k = sec->kind;
    if ((L.kind_n[k] & 63) == 0) L.kind_secs[k] = xrealloc(L.kind_secs[k], (size_t)(L.kind_n[k] + 64) * sizeof(LdSec *));
    L.kind_secs[k][L.kind_n[k]++] = sec;
}

static u64 ld_sym_addr(LdObj *o, size_t idx, bool *ok);

static void ld_prescan_got(void) {
    for (int oi = 0; oi < L.nobjs; oi++) {
        LdObj *o = L.objs[oi];
        for (int si = 1; si < o->nsh; si++) {
            LdSec *sec = o->secs[si];
            if (!sec || !sec->rela) continue;
            for (size_t k = 0; k < sec->nrela; k++) {
                u32 t = ELF64_R_TYPE(sec->rela[k].r_info);
                if (t == R_X86_64_GOTPCREL || t == R_X86_64_GOTPCRELX || t == R_X86_64_REX_GOTPCRELX || t == R_X86_64_GOTTPOFF) {
                    size_t sym = ELF64_R_SYM(sec->rela[k].r_info);
                    if (sym < o->nsyms && o->got_idx[sym] < 0) o->got_idx[sym] = L.ngot++;
                }
            }
        }
    }
}

static void ld_layout(void) {
    /* collect sections by kind; common symbols become one synthetic bss block */
    for (int oi = 0; oi < L.nobjs; oi++) {
        LdObj *o = L.objs[oi];
        for (int i = 1; i < o->nsh; i++)
            if (o->secs[i]) ld_add_kind_sec(o->secs[i]);
    }
    ld_prescan_got();

    /* region sizes */
    u64 got_size = (u64)L.ngot * 8;
    for (int k = 0; k < K_NKIND; k++) {
        u64 off = (k == K_DATA) ? got_size : 0, al = 1;
        for (int i = 0; i < L.kind_n[k]; i++) {
            LdSec *s = L.kind_secs[k][i];
            off = align_up_u64(off, s->align);
            s->outoff = off;
            off += s->size;
            if (s->align > al) al = s->align;
        }
        if (k == K_BSS) {
            /* COMMON symbols follow the ordinary .bss input sections */
            for (int i = 0; i < L.nsymlist; i++) {
                LdSym *s = L.symlist[i];
                if (!s->common) continue;
                off = align_up_u64(off, s->align);
                s->value = off;
                off += s->size;
                if (s->align > al) al = s->align;
            }
        }
        L.kind_size[k] = off;
        L.kind_align[k] = al;
    }

    /* addresses.  text at LD_BASE (file offset 0x1000), then rodata and data page by page; .tdata follows .data;
     * .bss follows .tdata (.tbss takes no address space of its own, like in lld) */
    u64 addr = LD_BASE, off = LD_HDR_SPACE;
    L.kind_addr[K_TEXT] = addr;
    L.kind_off[K_TEXT] = off;
    u64 text_end = addr + L.kind_size[K_TEXT];
    addr = align_up_u64(text_end, LD_PAGE);
    off = LD_HDR_SPACE + (addr - LD_BASE);
    L.kind_addr[K_RODATA] = addr;
    L.kind_off[K_RODATA] = off;
    addr = align_up_u64(addr + L.kind_size[K_RODATA], LD_PAGE);
    off = LD_HDR_SPACE + (addr - LD_BASE);
    L.kind_addr[K_DATA] = addr;
    L.kind_off[K_DATA] = off;
    L.got_addr = addr;
    u64 data_end = addr + L.kind_size[K_DATA];
    u64 tdata_addr = align_up_u64(data_end, L.kind_align[K_TDATA]);
    L.kind_addr[K_TDATA] = tdata_addr;
    L.kind_off[K_TDATA] = off + (tdata_addr - addr);
    u64 tdata_end = tdata_addr + L.kind_size[K_TDATA];
    u64 tbss_addr = align_up_u64(tdata_end, L.kind_align[K_TBSS]);
    L.kind_addr[K_TBSS] = tbss_addr;
    L.kind_off[K_TBSS] = L.kind_off[K_TDATA] + L.kind_size[K_TDATA];
    u64 bss_addr = align_up_u64(tdata_end, L.kind_align[K_BSS]);
    L.kind_addr[K_BSS] = bss_addr;
    L.kind_off[K_BSS] = L.kind_off[K_TDATA] + L.kind_size[K_TDATA];

    L.tls_addr = tdata_addr;
    L.tls_filesz = L.kind_size[K_TDATA];
    L.tls_memsz = (tbss_addr - tdata_addr) + L.kind_size[K_TBSS];
    L.tls_align = MAX(L.kind_align[K_TDATA], L.kind_align[K_TBSS]);
    {
        u64 a = L.tls_align ? L.tls_align : 1;
        u64 block = L.tls_memsz + ((0 - tdata_addr - L.tls_memsz) & (a - 1));
        L.tp_base = tdata_addr + block;
    }

    for (int k = 0; k < K_NKIND; k++)
        for (int i = 0; i < L.kind_n[k]; i++) {
            LdSec *s = L.kind_secs[k][i];
            s->addr = L.kind_addr[k] + s->outoff;
        }
    for (int i = 0; i < L.nsymlist; i++) {
        LdSym *s = L.symlist[i];
        if (s->common) {
            s->abs = true;
            s->value += L.kind_addr[K_BSS];
            s->defined = true;
        }
    }
}

static void ld_define_linker_symbols(void) {
    struct {
        const char *name;
        u64 value;
    } defs[] = {
        {"__tls_init_image", L.tls_addr},
        {"__tls_init_size", L.tls_filesz},
        {"__tls_mem_size", L.tls_memsz},
        {"__tls_align", L.tls_align ? L.tls_align : 1},
        {"__ehdr_start", LD_BASE - LD_HDR_SPACE},
        {"_etext", L.kind_addr[K_TEXT] + L.kind_size[K_TEXT]},
        {"etext", L.kind_addr[K_TEXT] + L.kind_size[K_TEXT]},
        {"_edata", L.kind_addr[K_BSS]},
        {"edata", L.kind_addr[K_BSS]},
        {"__bss_start", L.kind_addr[K_BSS]},
        {"_end", L.kind_addr[K_BSS] + L.kind_size[K_BSS]},
        {"end", L.kind_addr[K_BSS] + L.kind_size[K_BSS]},
    };
    for (size_t i = 0; i < sizeof defs / sizeof defs[0]; i++) {
        LdSym *s = ld_sym(defs[i].name, false);
        if (!s || s->defined) continue;
        if (!s->referenced) continue;
        s->defined = true;
        s->abs = true;
        s->value = defs[i].value;
    }
}

static u64 ld_sym_addr(LdObj *o, size_t idx, bool *ok) {
    *ok = true;
    if (idx == 0) return 0;
    if (idx >= o->nsyms) {
        *ok = false;
        return 0;
    }
    ElfSym *s = &o->syms[idx];
    int bind = ELF64_ST_BIND(s->st_info);
    if (bind == STB_LOCAL) {
        if (s->st_shndx == SHN_ABS) return s->st_value;
        if (s->st_shndx == SHN_UNDEF || s->st_shndx >= o->nsh || !o->secs[s->st_shndx]) {
            *ok = false;
            return 0;
        }
        return o->secs[s->st_shndx]->addr + s->st_value;
    }
    LdSym *g = o->gsyms[idx];
    if (!g) {
        *ok = false;
        return 0;
    }
    if (!g->defined) return 0; /* weak undefined; strong ones were reported before */
    if (g->abs) return g->value;
    return g->sec->addr + g->value;
}

static bool fits_i32(i64 v) {
    return v >= -2147483648LL && v <= 2147483647LL;
}

static void ld_apply_relocs(LdSec *sec, u8 *dst, u8 *got) {
    LdObj *o = sec->obj;
    for (size_t k = 0; k < sec->nrela; k++) {
        Rela *r = &sec->rela[k];
        u32 type = ELF64_R_TYPE(r->r_info);
        size_t symidx = ELF64_R_SYM(r->r_info);
        bool ok;
        u64 S = ld_sym_addr(o, symidx, &ok);
        if (!ok) {
            ld_error("%s: %s: relocation against a symbol in a discarded section", o->name, sec->name);
            continue;
        }
        i64 A = r->r_addend;
        u64 P = sec->addr + r->r_offset;
        u8 *where = dst + r->r_offset;
        i64 v;
        switch (type) {
        case R_X86_64_NONE:
            break;
        case R_X86_64_64:
            put64(where, S + (u64)A);
            break;
        case R_X86_64_PC32:
        case R_X86_64_PLT32:
            v = (i64)(S + (u64)A - P);
            if (!fits_i32(v)) ld_error("%s: %s+0x%llx: relocation truncated to fit (PC32)", o->name, sec->name, (unsigned long long)r->r_offset);
            put32(where, (u32)v);
            break;
        case R_X86_64_PC64:
            put64(where, S + (u64)A - P);
            break;
        case R_X86_64_32:
            v = (i64)(S + (u64)A);
            if (v < 0 || v > 0xffffffffLL) ld_error("%s: %s+0x%llx: relocation truncated to fit (32)", o->name, sec->name, (unsigned long long)r->r_offset);
            put32(where, (u32)v);
            break;
        case R_X86_64_32S:
            v = (i64)(S + (u64)A);
            if (!fits_i32(v)) ld_error("%s: %s+0x%llx: relocation truncated to fit (32S)", o->name, sec->name, (unsigned long long)r->r_offset);
            put32(where, (u32)v);
            break;
        case R_X86_64_TPOFF32:
            v = (i64)(S + (u64)A - L.tp_base);
            if (!fits_i32(v)) ld_error("%s: %s: thread-local offset out of range", o->name, sec->name);
            put32(where, (u32)v);
            break;
        case R_X86_64_TPOFF64:
            put64(where, S + (u64)A - L.tp_base);
            break;
        case R_X86_64_GOTPCREL:
        case R_X86_64_GOTPCRELX:
        case R_X86_64_REX_GOTPCRELX:
        case R_X86_64_GOTTPOFF: {
            int slot = o->got_idx[symidx];
            if (slot < 0) {
                ld_error("%s: internal error: no GOT slot", o->name);
                break;
            }
            put64(got + (size_t)slot * 8, type == R_X86_64_GOTTPOFF ? S - L.tp_base : S);
            v = (i64)(L.got_addr + (u64)slot * 8 + (u64)A - P);
            if (!fits_i32(v)) ld_error("%s: GOT reference out of range", o->name);
            put32(where, (u32)v);
            break;
        }
        default:
            ld_error("%s: %s: unsupported relocation type %u", o->name, sec->name, type);
            break;
        }
    }
}

static int ld_sym_cmp(const void *a, const void *b) {
    LdSym *x = *(LdSym *const *)a, *y = *(LdSym *const *)b;
    u64 ax = x->abs ? x->value : (x->sec ? x->sec->addr + x->value : 0);
    u64 ay = y->abs ? y->value : (y->sec ? y->sec->addr + y->value : 0);
    if (ax != ay) return ax < ay ? -1 : 1;
    return strcmp(x->name, y->name);
}

static void ld_write(const char *out) {
    /* image of the three loadable regions */
    u64 text_sz = L.kind_size[K_TEXT], ro_sz = L.kind_size[K_RODATA];
    u64 data_sz = L.kind_size[K_DATA];
    u64 tdata_end_off = (L.kind_off[K_TDATA] - L.kind_off[K_DATA]) + L.kind_size[K_TDATA];
    u64 data_file_sz = MAX(data_sz, tdata_end_off);
    u64 bss_end = L.kind_addr[K_BSS] + L.kind_size[K_BSS];
    u64 data_mem_sz = bss_end - L.kind_addr[K_DATA];
    if (data_mem_sz < data_file_sz) data_mem_sz = data_file_sz;

    u64 file_end = L.kind_off[K_DATA] + data_file_sz;
    Buf img = {0};
    buf_zero(&img, (size_t)file_end);
    u8 *got = img.p + L.kind_off[K_DATA];
    for (int k = 0; k < K_NKIND; k++) {
        if (k == K_TBSS || k == K_BSS) continue;
        for (int i = 0; i < L.kind_n[k]; i++) {
            LdSec *s = L.kind_secs[k][i];
            if (!s->size || !s->data) continue;
            memcpy(img.p + L.kind_off[k] + s->outoff, s->data, (size_t)s->size);
        }
    }
    for (int k = 0; k < K_NKIND; k++) {
        if (k == K_TBSS || k == K_BSS) continue;
        for (int i = 0; i < L.kind_n[k]; i++) {
            LdSec *s = L.kind_secs[k][i];
            if (s->rela) ld_apply_relocs(s, img.p + L.kind_off[k] + s->outoff, got);
        }
    }
    /* relocations in .bss sections cannot exist; in .tbss neither */

    LdSym *entry = ld_sym(L.entry, false);
    u64 entry_addr = 0;
    if (!entry || !entry->defined) ld_error("entry point '%s' is not defined", L.entry);
    else entry_addr = entry->abs ? entry->value : entry->sec->addr + entry->value;
    if (L.errors) return;

    /* symbol table (for objdump and for readable crash reports); section headers follow the image */
    Buf strtab = {0}, symtab = {0};
    buf_u8(&strtab, 0);
    buf_zero(&symtab, sizeof(ElfSym));
    int nsyms = 1;
    int first_global = 1;
    if (!L.strip) {
        for (int oi = 0; oi < L.nobjs; oi++) {
            LdObj *o = L.objs[oi];
            for (size_t i = 1; i < o->nsyms; i++) {
                ElfSym *s = &o->syms[i];
                int bind = ELF64_ST_BIND(s->st_info), type = ELF64_ST_TYPE(s->st_info);
                if (type == STT_SECTION || type == STT_FILE || bind != STB_LOCAL) continue;
                const char *nm = o->strtab + s->st_name;
                if (!*nm || nm[0] == '.' || s->st_shndx == SHN_UNDEF || s->st_shndx >= o->nsh || !o->secs[s->st_shndx]) continue;
                ElfSym e;
                memset(&e, 0, sizeof e);
                e.st_name = (u32)strtab.len;
                buf_str(&strtab, nm);
                buf_u8(&strtab, 0);
                e.st_info = (u8)ELF64_ST_INFO(STB_LOCAL, type);
                e.st_shndx = (u16)(o->secs[s->st_shndx]->kind + 1);
                e.st_value = o->secs[s->st_shndx]->addr + s->st_value;
                e.st_size = s->st_size;
                buf_put(&symtab, &e, sizeof e);
                nsyms++;
            }
        }
        first_global = nsyms;
        for (int i = 0; i < L.nsymlist; i++) {
            LdSym *s = L.symlist[i];
            if (!s->defined) continue;
            ElfSym e;
            memset(&e, 0, sizeof e);
            e.st_name = (u32)strtab.len;
            buf_str(&strtab, s->name);
            buf_u8(&strtab, 0);
            e.st_info = (u8)ELF64_ST_INFO(s->weak ? STB_WEAK : STB_GLOBAL, s->tls ? STT_TLS : STT_NOTYPE);
            e.st_shndx = s->abs ? SHN_ABS : (u16)(s->sec->kind + 1);
            e.st_value = s->abs ? s->value : s->sec->addr + s->value;
            e.st_size = s->size;
            buf_put(&symtab, &e, sizeof e);
            nsyms++;
        }
    }

    /* section header string table */
    Buf shstr = {0};
    buf_u8(&shstr, 0);
    u32 nm_text = (u32)shstr.len; buf_str(&shstr, ".text"); buf_u8(&shstr, 0);
    u32 nm_ro = (u32)shstr.len; buf_str(&shstr, ".rodata"); buf_u8(&shstr, 0);
    u32 nm_data = (u32)shstr.len; buf_str(&shstr, ".data"); buf_u8(&shstr, 0);
    u32 nm_tdata = (u32)shstr.len; buf_str(&shstr, ".tdata"); buf_u8(&shstr, 0);
    u32 nm_tbss = (u32)shstr.len; buf_str(&shstr, ".tbss"); buf_u8(&shstr, 0);
    u32 nm_bss = (u32)shstr.len; buf_str(&shstr, ".bss"); buf_u8(&shstr, 0);
    u32 nm_symtab = (u32)shstr.len; buf_str(&shstr, ".symtab"); buf_u8(&shstr, 0);
    u32 nm_strtab = (u32)shstr.len; buf_str(&shstr, ".strtab"); buf_u8(&shstr, 0);
    u32 nm_shstr = (u32)shstr.len; buf_str(&shstr, ".shstrtab"); buf_u8(&shstr, 0);

    buf_align(&img, 8);
    u64 symtab_off = img.len;
    buf_put(&img, symtab.p, symtab.len);
    u64 strtab_off = img.len;
    buf_put(&img, strtab.p, strtab.len);
    u64 shstr_off = img.len;
    buf_put(&img, shstr.p, shstr.len);
    buf_align(&img, 8);
    u64 shoff = img.len;

    int have_ro = ro_sz > 0, have_data = data_file_sz > 0 || L.kind_size[K_BSS] > 0;
    (void)have_ro;
    (void)have_data;
    Shdr sh[10];
    memset(sh, 0, sizeof sh);
    int n = 1;
    sh[n].sh_name = nm_text; sh[n].sh_type = SHT_PROGBITS; sh[n].sh_flags = SHF_ALLOC | SHF_EXECINSTR;
    sh[n].sh_addr = L.kind_addr[K_TEXT]; sh[n].sh_offset = L.kind_off[K_TEXT]; sh[n].sh_size = text_sz; sh[n].sh_addralign = 16; n++;
    sh[n].sh_name = nm_ro; sh[n].sh_type = SHT_PROGBITS; sh[n].sh_flags = SHF_ALLOC;
    sh[n].sh_addr = L.kind_addr[K_RODATA]; sh[n].sh_offset = L.kind_off[K_RODATA]; sh[n].sh_size = ro_sz; sh[n].sh_addralign = 16; n++;
    sh[n].sh_name = nm_data; sh[n].sh_type = SHT_PROGBITS; sh[n].sh_flags = SHF_ALLOC | SHF_WRITE;
    sh[n].sh_addr = L.kind_addr[K_DATA]; sh[n].sh_offset = L.kind_off[K_DATA]; sh[n].sh_size = data_sz; sh[n].sh_addralign = 16; n++;
    sh[n].sh_name = nm_tdata; sh[n].sh_type = SHT_PROGBITS; sh[n].sh_flags = SHF_ALLOC | SHF_WRITE | SHF_TLS;
    sh[n].sh_addr = L.kind_addr[K_TDATA]; sh[n].sh_offset = L.kind_off[K_TDATA]; sh[n].sh_size = L.kind_size[K_TDATA]; sh[n].sh_addralign = L.kind_align[K_TDATA]; n++;
    sh[n].sh_name = nm_tbss; sh[n].sh_type = SHT_NOBITS; sh[n].sh_flags = SHF_ALLOC | SHF_WRITE | SHF_TLS;
    sh[n].sh_addr = L.kind_addr[K_TBSS]; sh[n].sh_offset = L.kind_off[K_TBSS]; sh[n].sh_size = L.kind_size[K_TBSS]; sh[n].sh_addralign = L.kind_align[K_TBSS]; n++;
    sh[n].sh_name = nm_bss; sh[n].sh_type = SHT_NOBITS; sh[n].sh_flags = SHF_ALLOC | SHF_WRITE;
    sh[n].sh_addr = L.kind_addr[K_BSS]; sh[n].sh_offset = L.kind_off[K_BSS]; sh[n].sh_size = L.kind_size[K_BSS]; sh[n].sh_addralign = L.kind_align[K_BSS]; n++;
    int symtab_idx = n;
    sh[n].sh_name = nm_symtab; sh[n].sh_type = SHT_SYMTAB; sh[n].sh_offset = symtab_off; sh[n].sh_size = symtab.len;
    sh[n].sh_link = (u32)(n + 1); sh[n].sh_info = (u32)first_global; sh[n].sh_addralign = 8; sh[n].sh_entsize = sizeof(ElfSym); n++;
    sh[n].sh_name = nm_strtab; sh[n].sh_type = SHT_STRTAB; sh[n].sh_offset = strtab_off; sh[n].sh_size = strtab.len; sh[n].sh_addralign = 1; n++;
    int shstr_idx = n;
    sh[n].sh_name = nm_shstr; sh[n].sh_type = SHT_STRTAB; sh[n].sh_offset = shstr_off; sh[n].sh_size = shstr.len; sh[n].sh_addralign = 1; n++;
    (void)symtab_idx;
    buf_put(&img, sh, (size_t)n * sizeof(Shdr));

    /* ELF header and program headers */
    Phdr ph[4];
    memset(ph, 0, sizeof ph);
    int np = 0;
    if (text_sz) {
        ph[np].p_type = PT_LOAD; ph[np].p_flags = PF_R | PF_X;
        ph[np].p_offset = L.kind_off[K_TEXT]; ph[np].p_vaddr = ph[np].p_paddr = L.kind_addr[K_TEXT];
        ph[np].p_filesz = ph[np].p_memsz = text_sz; ph[np].p_align = LD_PAGE; np++;
    }
    if (ro_sz) {
        ph[np].p_type = PT_LOAD; ph[np].p_flags = PF_R;
        ph[np].p_offset = L.kind_off[K_RODATA]; ph[np].p_vaddr = ph[np].p_paddr = L.kind_addr[K_RODATA];
        ph[np].p_filesz = ph[np].p_memsz = ro_sz; ph[np].p_align = LD_PAGE; np++;
    }
    if (data_mem_sz) {
        ph[np].p_type = PT_LOAD; ph[np].p_flags = PF_R | PF_W;
        ph[np].p_offset = L.kind_off[K_DATA]; ph[np].p_vaddr = ph[np].p_paddr = L.kind_addr[K_DATA];
        ph[np].p_filesz = data_file_sz; ph[np].p_memsz = data_mem_sz; ph[np].p_align = LD_PAGE; np++;
    }
    if (L.tls_memsz) {
        ph[np].p_type = PT_TLS; ph[np].p_flags = PF_R;
        ph[np].p_offset = L.kind_off[K_TDATA]; ph[np].p_vaddr = ph[np].p_paddr = L.tls_addr;
        ph[np].p_filesz = L.tls_filesz; ph[np].p_memsz = L.tls_memsz; ph[np].p_align = L.tls_align ? L.tls_align : 1; np++;
    }
    Ehdr eh;
    memset(&eh, 0, sizeof eh);
    memcpy(eh.e_ident, "\177ELF", 4);
    eh.e_ident[4] = 2;
    eh.e_ident[5] = 1;
    eh.e_ident[6] = 1;
    eh.e_type = ET_EXEC;
    eh.e_machine = 62;
    eh.e_version = 1;
    eh.e_entry = entry_addr;
    eh.e_phoff = sizeof(Ehdr);
    eh.e_shoff = shoff;
    eh.e_ehsize = sizeof(Ehdr);
    eh.e_phentsize = sizeof(Phdr);
    eh.e_phnum = (u16)np;
    eh.e_shentsize = sizeof(Shdr);
    eh.e_shnum = (u16)n;
    eh.e_shstrndx = (u16)shstr_idx;
    memcpy(img.p, &eh, sizeof eh);
    memcpy(img.p + sizeof eh, ph, (size_t)np * sizeof(Phdr));

    if (write_whole_file(out, img.p, img.len, true) != 0) {
        ld_error("cannot write '%s': %s", out, strerror(errno));
        return;
    }
    if (L.show_map) {
        LdSym **list = xmalloc((size_t)L.nsymlist * sizeof(LdSym *));
        int cnt = 0;
        for (int i = 0; i < L.nsymlist; i++)
            if (L.symlist[i]->defined) list[cnt++] = L.symlist[i];
        qsort(list, (size_t)cnt, sizeof(LdSym *), ld_sym_cmp);
        for (int i = 0; i < cnt; i++) {
            LdSym *s = list[i];
            printf("%016llx %s\n", (unsigned long long)(s->abs ? s->value : s->sec->addr + s->value), s->name);
        }
    }
}

/* ---- driver */

typedef struct {
    char *path;
    int kind; /* 0 = file by path, 1 = in-memory object */
    u8 *data;
    size_t len;
} LdInput;

static u8 *ld_load_file(const char *path, size_t *len) {
    u8 *img = read_whole_file(path, len);
    if (!img) ld_error("cannot open '%s': %s", path, strerror(errno));
    return img;
}

static void ld_add_file(const char *path) {
    size_t len;
    u8 *img = ld_load_file(path, &len);
    if (!img) return;
    if (len >= 8 && !memcmp(img, "!<arch>\n", 8)) {
        Archive *ar = ld_read_archive(path, img, len);
        L.ars = xrealloc(L.ars, (size_t)(L.nars + 1) * sizeof(Archive *));
        L.ars[L.nars++] = ar;
        return;
    }
    ld_add_object(xstrdup(path), img, len);
}

/* Reports every symbol that is referenced but never defined. */
static void ld_check_undefined(void) {
    for (int oi = 0; oi < L.nobjs; oi++) {
        LdObj *o = L.objs[oi];
        for (size_t i = 1; i < o->nsyms; i++) {
            LdSym *g = o->gsyms[i];
            if (!g || g->defined || g->common) continue;
            ElfSym *s = &o->syms[i];
            if (s->st_shndx != SHN_UNDEF) continue;
            if (ELF64_ST_BIND(s->st_info) == STB_WEAK) continue;
            if (g->weak) continue;
            ld_error("%s: undefined reference to '%s' -- Marisa: that book isn't in the library, ze", o->name, g->name);
            g->defined = true; /* once per symbol */
            g->abs = true;
            g->value = 0;
        }
    }
}

/* Links the objects/archives named in `files` (already filtered for -l) into `out`. */
int ld_link_files(char **files, int nfiles, const char *out, bool strip, bool show_map, const char *entry) {
    memset(&L, 0, sizeof L);
    L.strip = strip;
    L.show_map = show_map;
    L.entry = entry ? entry : "_start";
    for (int i = 0; i < nfiles; i++) ld_add_file(files[i]);
    if (L.errors) return 1;
    ld_resolve_archives();
    ld_layout();
    ld_define_linker_symbols();
    ld_check_undefined();
    if (L.errors) return 1;
    ld_write(out);
    return L.errors ? 1 : 0;
}

/* The in-memory variant used by the compiler driver: objects are handed over as buffers. */
int ld_link_mixed(LdInput *in, int nin, const char *out, bool strip, bool show_map, const char *entry) {
    memset(&L, 0, sizeof L);
    L.strip = strip;
    L.show_map = show_map;
    L.entry = entry ? entry : "_start";
    for (int i = 0; i < nin; i++) {
        if (in[i].kind == 1) ld_add_object(in[i].path, in[i].data, in[i].len);
        else ld_add_file(in[i].path);
    }
    if (L.errors) return 1;
    ld_resolve_archives();
    ld_layout();
    ld_define_linker_symbols();
    ld_check_undefined();
    if (L.errors) return 1;
    ld_write(out);
    return L.errors ? 1 : 0;
}
