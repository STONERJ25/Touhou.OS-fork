/* obj.inc -- data emission and the ELF64 relocatable object writer. */

static int data_section_of(Obj *var) {
    if (var->is_tls) return var->init_data ? SEC_TDATA : SEC_TBSS;
    if (!var->init_data) return SEC_BSS;
    return var->is_const_data ? SEC_RODATA : SEC_DATA;
}

static void emit_data(Obj *var) {
    int sec = data_section_of(var);
    OutSec *s = &osec[sec];
    int align = var->align ? var->align : 1;
    if (var->ty->kind == TY_ARRAY && var->ty->size >= 16 && align < 16) align = 16;
    int size = var->ty->size > 0 ? var->ty->size : 0;

    if (sec == SEC_BSS || sec == SEC_TBSS) {
        s->size = (s->size + (u64)align - 1) / (u64)align * (u64)align;
        var->sec_off = (int)s->size;
        s->size += (u64)(size ? size : 1);
    } else {
        buf_align(&s->data, (size_t)align);
        var->sec_off = (int)s->data.len;
        buf_put(&s->data, var->init_data, (size_t)size);
        for (Relocation *rel = var->rel; rel; rel = rel->next) add_reloc(sec, (u32)(var->sec_off + rel->offset), R_X86_64_64, rel->target, rel->addend);
        s->size = s->data.len;
    }
    if ((u64)align > s->align) s->align = (u64)align;
    var->sec = sec;
    var->size_out = size;
    var->emitted = true;
}

static void codegen_program(Obj *prog) {
    for (Obj *fn = prog; fn; fn = fn->next) {
        if (!fn->is_function || !fn->is_definition || !fn->is_live) continue;
        gen_function(fn);
    }
    for (Obj *var = prog; var; var = var->next) {
        if (var->is_function || !var->is_definition || !var->is_live) continue;
        if (var->ty->size < 0) {
            /* a tentative definition of an incomplete array is one element long */
            if (var->ty->kind == TY_ARRAY) var->ty = array_of(var->ty->base, 1);
            else error_tok(var->tok ? var->tok : (Token *)NULL, "storage size of '%s' is unknown", var->name);
        }
        emit_data(var);
    }
}

/* ---- the object file */

static const char *sec_names[SEC_NUM] = {".text", ".data", ".rodata", ".bss", ".tdata", ".tbss"};

static bool sec_present(int s) {
    OutSec *o = &osec[s];
    if (s == SEC_BSS || s == SEC_TBSS) return o->size > 0;
    return o->data.len > 0 || o->nrel > 0;
}

static u64 sec_flags(int s) {
    switch (s) {
    case SEC_TEXT:
        return SHF_ALLOC | SHF_EXECINSTR;
    case SEC_RODATA:
        return SHF_ALLOC;
    case SEC_TDATA:
    case SEC_TBSS:
        return SHF_ALLOC | SHF_WRITE | SHF_TLS;
    default:
        return SHF_ALLOC | SHF_WRITE;
    }
}

static u32 add_name(Buf *tab, const char *s) {
    u32 off = (u32)tab->len;
    buf_str(tab, s);
    buf_u8(tab, 0);
    return off;
}

static bool is_local_label_name(const char *name) {
    return name[0] == '.' && name[1] == 'L';
}

static void collect_targets(Obj *prog, Obj ***list, int *n) {
    /* every Obj used as a relocation target and defined elsewhere gets an undefined symbol */
    (void)prog;
    int cap = 0;
    *n = 0;
    *list = NULL;
    for (int s = 0; s < SEC_NUM; s++) {
        for (int i = 0; i < osec[s].nrel; i++) {
            Obj *t = osec[s].rel[i].target;
            if (t->emitted || t->sym_index) continue;
            t->sym_index = -1; /* seen */
            if (*n == cap) {
                cap = cap ? cap * 2 : 32;
                *list = xrealloc(*list, (size_t)cap * sizeof(Obj *));
            }
            (*list)[(*n)++] = t;
        }
    }
}

static Buf write_object(Obj *prog, const char *srcname) {
    Buf strtab = {0}, symtab = {0}, shstr = {0};
    buf_u8(&strtab, 0);
    buf_u8(&shstr, 0);

    int shndx[SEC_NUM];
    int nsh = 1;
    for (int s = 0; s < SEC_NUM; s++) shndx[s] = sec_present(s) ? nsh++ : 0;
    int rela_shndx[SEC_NUM];
    for (int s = 0; s < SEC_NUM; s++) rela_shndx[s] = (shndx[s] && osec[s].nrel > 0) ? nsh++ : 0;
    int symtab_shndx = nsh++;
    int strtab_shndx = nsh++;
    int shstr_shndx = nsh++;
    int note_shndx = nsh++;

    /* symbols: null, section symbols, file, static objects, then globals (defined, undefined) */
    ElfSym nullsym;
    memset(&nullsym, 0, sizeof nullsym);
    buf_put(&symtab, &nullsym, sizeof nullsym);
    int nsyms = 1;

    int secsym[SEC_NUM];
    for (int s = 0; s < SEC_NUM; s++) {
        secsym[s] = 0;
        if (!shndx[s]) continue;
        ElfSym e;
        memset(&e, 0, sizeof e);
        e.st_info = (u8)ELF64_ST_INFO(STB_LOCAL, STT_SECTION);
        e.st_shndx = (u16)shndx[s];
        buf_put(&symtab, &e, sizeof e);
        secsym[s] = nsyms++;
    }

    {
        ElfSym e;
        memset(&e, 0, sizeof e);
        e.st_name = add_name(&strtab, base_of(srcname));
        e.st_info = (u8)ELF64_ST_INFO(STB_LOCAL, STT_FILE);
        e.st_shndx = SHN_ABS;
        buf_put(&symtab, &e, sizeof e);
        nsyms++;
    }

    for (Obj *o = prog; o; o = o->next) {
        if (!o->emitted || !o->is_static || is_local_label_name(o->name)) continue;
        ElfSym e;
        memset(&e, 0, sizeof e);
        e.st_name = add_name(&strtab, o->name);
        e.st_info = (u8)ELF64_ST_INFO(STB_LOCAL, o->is_function ? STT_FUNC : (o->is_tls ? STT_TLS : STT_OBJECT));
        e.st_shndx = (u16)shndx[o->sec];
        e.st_value = (u64)o->sec_off;
        e.st_size = (u64)o->size_out;
        buf_put(&symtab, &e, sizeof e);
        o->sym_index = nsyms++;
    }

    int first_global = nsyms;

    for (Obj *o = prog; o; o = o->next) {
        if (!o->emitted || o->is_static) continue;
        ElfSym e;
        memset(&e, 0, sizeof e);
        e.st_name = add_name(&strtab, o->name);
        e.st_info = (u8)ELF64_ST_INFO(o->is_weak ? STB_WEAK : STB_GLOBAL, o->is_function ? STT_FUNC : (o->is_tls ? STT_TLS : STT_OBJECT));
        e.st_shndx = (u16)shndx[o->sec];
        e.st_value = (u64)o->sec_off;
        e.st_size = (u64)o->size_out;
        buf_put(&symtab, &e, sizeof e);
        o->sym_index = nsyms++;
    }

    Obj **undef;
    int nundef;
    collect_targets(prog, &undef, &nundef);
    for (int i = 0; i < nundef; i++) {
        Obj *o = undef[i];
        ElfSym e;
        memset(&e, 0, sizeof e);
        e.st_name = add_name(&strtab, o->name);
        e.st_info = (u8)ELF64_ST_INFO(o->is_weak ? STB_WEAK : STB_GLOBAL, o->is_function ? STT_FUNC : (o->is_tls ? STT_TLS : STT_NOTYPE));
        e.st_shndx = SHN_UNDEF;
        buf_put(&symtab, &e, sizeof e);
        o->sym_index = nsyms++;
    }

    /* relocation tables */
    Buf rela[SEC_NUM];
    memset(rela, 0, sizeof rela);
    for (int s = 0; s < SEC_NUM; s++) {
        for (int i = 0; i < osec[s].nrel; i++) {
            ObjReloc *r = &osec[s].rel[i];
            Obj *t = r->target;
            Rela out;
            u32 sym;
            i64 addend = r->addend;
            if (t->emitted && t->sym_index <= 0) {
                /* an anonymous static (string literal...): refer to its section */
                sym = (u32)secsym[t->sec];
                addend += t->sec_off;
            } else {
                sym = (u32)t->sym_index;
            }
            out.r_offset = r->offset;
            out.r_info = ELF64_R_INFO(sym, r->type);
            out.r_addend = addend;
            buf_put(&rela[s], &out, sizeof out);
        }
    }

    /* section header names */
    u32 name_of[SEC_NUM], rela_name_of[SEC_NUM];
    for (int s = 0; s < SEC_NUM; s++) {
        name_of[s] = add_name(&shstr, sec_names[s]);
        rela_name_of[s] = add_name(&shstr, xformat(".rela%s", sec_names[s]));
    }
    u32 nm_symtab = add_name(&shstr, ".symtab");
    u32 nm_strtab = add_name(&shstr, ".strtab");
    u32 nm_shstr = add_name(&shstr, ".shstrtab");
    u32 nm_note = add_name(&shstr, ".note.GNU-stack");

    /* lay out the file */
    Buf out = {0};
    buf_zero(&out, sizeof(Ehdr));
    Shdr *sh = xcalloc((size_t)nsh * sizeof(Shdr));

    for (int s = 0; s < SEC_NUM; s++) {
        if (!shndx[s]) continue;
        Shdr *h = &sh[shndx[s]];
        h->sh_name = name_of[s];
        h->sh_flags = sec_flags(s);
        h->sh_addralign = osec[s].align ? osec[s].align : 1;
        if (s == SEC_BSS || s == SEC_TBSS) {
            h->sh_type = SHT_NOBITS;
            h->sh_size = osec[s].size;
            h->sh_offset = out.len;
        } else {
            h->sh_type = SHT_PROGBITS;
            buf_align(&out, (size_t)h->sh_addralign);
            h->sh_offset = out.len;
            h->sh_size = osec[s].data.len;
            buf_put(&out, osec[s].data.p, osec[s].data.len);
        }
    }

    buf_align(&out, 8);
    for (int s = 0; s < SEC_NUM; s++) {
        if (!rela_shndx[s]) continue;
        Shdr *h = &sh[rela_shndx[s]];
        h->sh_name = rela_name_of[s];
        h->sh_type = SHT_RELA;
        h->sh_flags = SHF_INFO_LINK;
        h->sh_link = (u32)symtab_shndx;
        h->sh_info = (u32)shndx[s];
        h->sh_addralign = 8;
        h->sh_entsize = sizeof(Rela);
        h->sh_offset = out.len;
        h->sh_size = rela[s].len;
        buf_put(&out, rela[s].p, rela[s].len);
    }

    {
        Shdr *h = &sh[symtab_shndx];
        h->sh_name = nm_symtab;
        h->sh_type = SHT_SYMTAB;
        h->sh_link = (u32)strtab_shndx;
        h->sh_info = (u32)first_global;
        h->sh_addralign = 8;
        h->sh_entsize = sizeof(ElfSym);
        h->sh_offset = out.len;
        h->sh_size = symtab.len;
        buf_put(&out, symtab.p, symtab.len);
    }
    {
        Shdr *h = &sh[strtab_shndx];
        h->sh_name = nm_strtab;
        h->sh_type = SHT_STRTAB;
        h->sh_addralign = 1;
        h->sh_offset = out.len;
        h->sh_size = strtab.len;
        buf_put(&out, strtab.p, strtab.len);
    }
    {
        Shdr *h = &sh[shstr_shndx];
        h->sh_name = nm_shstr;
        h->sh_type = SHT_STRTAB;
        h->sh_addralign = 1;
        h->sh_offset = out.len;
        h->sh_size = shstr.len;
        buf_put(&out, shstr.p, shstr.len);
    }
    {
        Shdr *h = &sh[note_shndx];
        h->sh_name = nm_note;
        h->sh_type = SHT_PROGBITS;
        h->sh_addralign = 1;
        h->sh_offset = out.len;
    }

    buf_align(&out, 8);
    u64 shoff = out.len;
    buf_put(&out, sh, (size_t)nsh * sizeof(Shdr));

    Ehdr eh;
    memset(&eh, 0, sizeof eh);
    memcpy(eh.e_ident, "\177ELF", 4);
    eh.e_ident[4] = 2;
    eh.e_ident[5] = 1;
    eh.e_ident[6] = 1;
    eh.e_type = ET_REL;
    eh.e_machine = 62;
    eh.e_version = 1;
    eh.e_shoff = shoff;
    eh.e_ehsize = sizeof(Ehdr);
    eh.e_shentsize = sizeof(Shdr);
    eh.e_shnum = (u16)nsh;
    eh.e_shstrndx = (u16)shstr_shndx;
    memcpy(out.p, &eh, sizeof eh);
    return out;
}

/* Clears the code generator's output between two translation units (for tools that compile several files). */
static void reset_output(void) {
    for (int s = 0; s < SEC_NUM; s++) {
        free(osec[s].data.p);
        free(osec[s].rel);
        memset(&osec[s], 0, sizeof osec[s]);
    }
}
