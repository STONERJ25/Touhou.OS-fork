/* elf.inc -- the layout passes, the final emission, and the ELF64 relocatable object writer.
 *
 * The object follows the layout NASM's elf64 writer produces so that the two assemblers' outputs can be compared byte for byte:
 *   ELF header, the section header table right behind it, then the section contents (each at a 16-byte boundary) in this order:
 *   the sections of the source in order of first appearance, .shstrtab, .symtab, .strtab, then one .rela.NAME per section that has
 *   relocations.  The symbol table: the null symbol, a FILE symbol (the source path as given), one SECTION symbol per source section,
 *   then the local symbols (labels and equ constants, in the order they were created), then the global ones (externs and global labels
 *   in creation order).  References to labels of this object are relocated against the section symbol with the label's offset as addend. */

#define EM_X86_64 62
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_NOBITS 8

/* ---------------------------------------------------------------------------------------------------------- layout */

static i64 sec_pos[32];

static int layout_pass(void) {
    int changed = 0;
    for (int i = 0; i < nsecs; i++) sec_pos[i] = 0;
    for (int k = 0; k < nitems; k++) {
        Item *it = &items[k];
        cur_file = it->file;
        cur_line = it->line;
        if (it->sec < 0 && it->kind != K_EQU) fatal("code or data outside a section");
        i64 pos = it->sec >= 0 ? sec_pos[it->sec] : 0;
        if (it->pos != pos) changed = 1;
        it->pos = pos;
        int size = 0;
        switch (it->kind) {
        case K_LABEL: {
            Sym *s = it->sym;
            if (!s->defined || s->sec != it->sec || s->val != pos) changed = 1;
            sym_define(s, it->sec, pos);
            break;
        }
        case K_EQU: {
            ectx.sec = it->sec;
            ectx.pos = pos;
            ectx.pp_only = 0;
            Val v = eval_range(it->e, 0, it->ne);
            Sym *s = it->sym;
            if (v.kind == V_CONST) {
                if (!s->defined || s->sec != SEC_ABS || s->val != v.num) changed = 1;
                sym_define(s, SEC_ABS, v.num);
            } else if (v.kind == V_SEC) {
                if (!s->defined || s->sec != v.sec || s->val != v.num) changed = 1;
                sym_define(s, v.sec, v.num);
            } else if (v.kind == V_UNK) {
                changed = 1;
            } else {
                fatal("equ with an external symbol is not supported");
            }
            break;
        }
        case K_DATA: {
            for (int d = 0; d < it->ndata; d++) {
                if (it->dn[d] == 1 && it->de[d][0].kind == T_STR) size += (int)it->de[d][0].num + (it->dsize > 1 ? (it->dsize - ((int)it->de[d][0].num % it->dsize)) % it->dsize : 0);
                else size += it->dsize;
            }
            break;
        }
        case K_RES: {
            ectx.sec = it->sec;
            ectx.pos = pos;
            ectx.pp_only = 0;
            Val v = eval_range(it->e, 0, it->ne);
            if (v.kind == V_UNK) {
                changed = 1;
                break;
            }
            if (v.kind != V_CONST || v.num < 0 || v.num > (1 << 30)) fatal("bad reserve count");
            size = (int)(v.num * it->dsize);
            break;
        }
        case K_ALIGN: {
            ectx.sec = it->sec;
            ectx.pos = pos;
            ectx.pp_only = 0;
            Val v = eval_range(it->e, 0, it->ne);
            if (v.kind == V_UNK) {
                changed = 1;
                break;
            }
            if (v.kind != V_CONST || v.num <= 0 || (v.num & (v.num - 1))) fatal("align needs a power of two");
            if (v.num > secs[it->sec].align) secs[it->sec].align = v.num;
            size = (int)((v.num - (pos & (v.num - 1))) & (v.num - 1));
            break;
        }
        case K_INCBIN:
            size = (int)it->len;
            break;
        case K_INSN: {
            Buf scratch = {0};
            size = encode_item(it, &scratch, 0);
            free(scratch.p);
            if (it->grow && !it->near_) {
                it->near_ = 1;                                                  /* the long form is longer: the next pass lays everything out again */
                changed = 1;
            }
            break;
        }
        }
        if (it->size != size) changed = 1;
        it->size = size;
        if (it->sec >= 0) sec_pos[it->sec] += size;
    }
    return changed;
}

/* ------------------------------------------------------------------------------------------------------- emission */

static void emit_data(Item *it, Section *s) {
    for (int d = 0; d < it->ndata; d++) {
        Tok *t = it->de[d];
        int n = it->dn[d];
        if (n == 1 && t[0].kind == T_STR) {
            buf_put(&s->data, t[0].text, (size_t)t[0].num);
            size_t pad = it->dsize > 1 ? (size_t)((it->dsize - (t[0].num % it->dsize)) % it->dsize) : 0;
            buf_zero(&s->data, pad);
            continue;
        }
        ectx.sec = it->sec;
        ectx.pos = it->pos + (i64)s->data.len - it->pos;                        /* $ is the start of the item for all of its values; good enough here */
        ectx.pos = it->pos;
        ectx.pp_only = 0;
        Val v = eval_range(t, 0, n);
        int sz = it->dsize;
        if (v.kind == V_UNK) fatal("symbol '%s' is not defined in this file (declare it extern?)", v.ext ? v.ext->name : "?");
        if (v.kind == V_CONST) {
            buf_le(&s->data, (u64)v.num, sz);
        } else {
            int type = sz == 8 ? R_X86_64_64 : sz == 4 ? R_X86_64_32 : sz == 2 ? R_X86_64_16 : R_X86_64_8;
            add_reloc(it->sec, (u32)s->data.len, type, v.kind == V_EXT ? v.ext : NULL, v.kind == V_SEC ? v.sec : -1, v.num);
            buf_zero(&s->data, (size_t)sz);
        }
    }
}

static void emit_all(void) {
    for (int i = 0; i < nsecs; i++) {
        secs[i].size = sec_pos[i];
        secs[i].data.len = 0;
        secs[i].nrel = 0;
    }
    final_pass = 1;
    for (int k = 0; k < nitems; k++) {
        Item *it = &items[k];
        cur_file = it->file;
        cur_line = it->line;
        Section *s = &secs[it->sec >= 0 ? it->sec : 0];
        if (it->sec >= 0 && s->nobits && it->kind != K_LABEL && it->kind != K_EQU && it->kind != K_RES && it->kind != K_ALIGN)
            fatal("initialised data or code in the uninitialised section %s", s->name);
        switch (it->kind) {
        case K_LABEL:
        case K_EQU:
            break;
        case K_DATA:
            emit_data(it, s);
            break;
        case K_RES:
        case K_ALIGN:
            if (!s->nobits) {
                if (it->kind == K_ALIGN && !it->dsize) {
                    /* ALIGN pads with NOPs in every section that has contents, ALIGNB with zeros (as NASM does) */
                    for (int i = 0; i < it->size; i++) buf_u8(&s->data, 0x90);
                } else buf_zero(&s->data, (size_t)it->size);
            }
            break;
        case K_INCBIN:
            buf_put(&s->data, it->bytes, it->len);
            break;
        case K_INSN: {
            int n = encode_item(it, &s->data, 1);
            if (n != it->size) fatal("internal error: the size of an instruction changed after the layout (%d, then %d)", it->size, n);
            break;
        }
        }
        if (!s->nobits && it->sec >= 0 && (i64)s->data.len != it->pos + it->size) fatal("internal error: section %s is %zu bytes long, expected %lld", s->name, s->data.len, it->pos + it->size);
    }
    final_pass = 0;
}

static void assemble_all(void) {
    for (int pass = 0; pass < 200; pass++) {
        if (!layout_pass()) {
            emit_all();
            return;
        }
    }
    /* the layout did not settle (an align fighting a branch): make every branch long and try again */
    for (int k = 0; k < nitems; k++) items[k].near_ = 1;
    for (int pass = 0; pass < 50; pass++)
        if (!layout_pass()) {
            emit_all();
            return;
        }
    fatal("the layout of the sections does not settle");
}

/* ----------------------------------------------------------------------------------------------------- the object */

typedef struct {
    Buf b;
} StrTab;

static u32 st_add(Buf *b, const char *s) {
    u32 off = (u32)b->len;
    buf_put(b, s, strlen(s) + 1);
    return off;
}

typedef struct {
    u32 name;
    u8 info, other;
    u16 shndx;
    u64 value, size;
} OutSym;

static void put_shdr(Buf *b, u32 name, u32 type, u64 flags, u64 offset, u64 size, u32 link, u32 info, u64 align, u64 entsize) {
    buf_le(b, name, 4);
    buf_le(b, type, 4);
    buf_le(b, flags, 8);
    buf_le(b, 0, 8);
    buf_le(b, offset, 8);
    buf_le(b, size, 8);
    buf_le(b, link, 4);
    buf_le(b, info, 4);
    buf_le(b, align, 8);
    buf_le(b, entsize, 8);
}

static void write_object(const char *path, const char *src_path) {
    /* ---- the symbol table */
    Sym *all[65536];
    int nall = 0;
    for (int h = 0; h < NSYM_BUCKETS; h++)
        for (Sym *s = sym_tab[h]; s; s = s->next) {
            if (!s->created) continue;                                      /* mentioned only in a global directive, or never defined */
            if (nall >= 65536) fatal("too many symbols");
            all[nall++] = s;
        }
    /* creation order */
    for (int i = 1; i < nall; i++) {
        Sym *x = all[i];
        int j = i - 1;
        while (j >= 0 && all[j]->created > x->created) {
            all[j + 1] = all[j];
            j--;
        }
        all[j + 1] = x;
    }
    for (int i = 0; i < nall; i++)
        if (!all[i]->defined && !all[i]->is_extern) fatal("symbol '%s' is not defined", all[i]->name);

    Buf strtab = {0}, symtab = {0}, shstr = {0};
    buf_u8(&strtab, 0);
    buf_u8(&shstr, 0);
    OutSym *syms = xcalloc((size_t)(nall + nsecs + 4) * sizeof(OutSym));
    int nsym = 0;
    syms[nsym++] = (OutSym){0, 0, 0, 0, 0, 0};
    syms[nsym++] = (OutSym){st_add(&strtab, src_path), 0x04, 0, 0xfff1, 0, 0};          /* FILE */
    /* the string table lists the names in the order the symbols were created (NASM does that), the symbol table groups locals before globals */
    u32 *str_off = xcalloc((size_t)(nall + 1) * sizeof(u32));
    for (int i = 0; i < nall; i++) str_off[i] = st_add(&strtab, all[i]->name);
    /* section header indexes: the sections of the source start at 1 */
    for (int i = 0; i < nsecs; i++) secs[i].shndx = i + 1;
    for (int i = 0; i < nsecs; i++) {
        secs[i].symidx = nsym;
        syms[nsym++] = (OutSym){0, 0x03, 0, (u16)secs[i].shndx, 0, 0};                  /* SECTION */
    }
    int nlocal_first = nsym;
    (void)nlocal_first;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < nall; i++) {
            Sym *s = all[i];
            int global = s->is_extern || s->is_global;
            if ((pass == 0) == global) continue;
            u16 shndx = !s->defined ? 0 : s->sec == SEC_ABS ? 0xfff1 : (u16)secs[s->sec].shndx;
            u64 value = !s->defined ? 0 : (u64)s->val;
            s->symidx = nsym;
            syms[nsym++] = (OutSym){str_off[i], (u8)(global ? 0x10 : 0x00), 0, shndx, value, 0};
        }
        if (pass == 0) nlocal_first = nsym;
    }
    int first_global = nlocal_first;
    for (int i = 0; i < nsym; i++) {
        buf_le(&symtab, syms[i].name, 4);
        buf_u8(&symtab, syms[i].info);
        buf_u8(&symtab, syms[i].other);
        buf_le(&symtab, syms[i].shndx, 2);
        buf_le(&symtab, syms[i].value, 8);
        buf_le(&symtab, syms[i].size, 8);
    }

    /* ---- section header table: source sections, .shstrtab, .symtab, .strtab, .rela.* */
    int nrela = 0;
    for (int i = 0; i < nsecs; i++)
        if (secs[i].nrel) nrela++;
    int nsh = 1 + nsecs + 3 + nrela;
    int idx_shstrtab = 1 + nsecs, idx_symtab = idx_shstrtab + 1, idx_strtab = idx_symtab + 1;

    u32 *name_off = xcalloc((size_t)nsh * sizeof(u32));
    for (int i = 0; i < nsecs; i++) name_off[1 + i] = st_add(&shstr, secs[i].name);
    name_off[idx_shstrtab] = st_add(&shstr, ".shstrtab");
    name_off[idx_symtab] = st_add(&shstr, ".symtab");
    name_off[idx_strtab] = st_add(&shstr, ".strtab");
    int ri = 0;
    int rela_of[32];
    for (int i = 0; i < nsecs; i++) {
        rela_of[i] = -1;
        if (!secs[i].nrel) continue;
        rela_of[i] = idx_strtab + 1 + ri++;
        name_off[rela_of[i]] = st_add(&shstr, xfmt(".rela%s", secs[i].name));
    }

    Buf rela[32];
    memset(rela, 0, sizeof rela);
    for (int i = 0; i < nsecs; i++) {
        for (int r = 0; r < secs[i].nrel; r++) {
            Reloc *x = &secs[i].rel[r];
            u32 symi = x->sym ? (u32)x->sym->symidx : (u32)secs[x->secsym].symidx;
            buf_le(&rela[i], x->off, 8);
            buf_le(&rela[i], ((u64)symi << 32) | (u32)x->type, 8);
            buf_le(&rela[i], (u64)x->addend, 8);
        }
    }

    Buf out = {0};
    /* ELF header */
    static const u8 ident[16] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    buf_put(&out, ident, 16);
    buf_le(&out, 1, 2);                 /* ET_REL */
    buf_le(&out, EM_X86_64, 2);
    buf_le(&out, 1, 4);
    buf_le(&out, 0, 8);                 /* entry */
    buf_le(&out, 0, 8);                 /* phoff */
    buf_le(&out, 0x40, 8);              /* shoff */
    buf_le(&out, 0, 4);
    buf_le(&out, 64, 2);
    buf_le(&out, 0, 2);
    buf_le(&out, 0, 2);
    buf_le(&out, 64, 2);
    buf_le(&out, (u64)nsh, 2);
    buf_le(&out, (u64)idx_shstrtab, 2);

    /* where each section's contents go */
    u64 off = 0x40 + (u64)nsh * 64;
    u64 *sh_off = xcalloc((size_t)nsh * sizeof(u64));
    u64 *sh_size = xcalloc((size_t)nsh * sizeof(u64));
    for (int i = 0; i < nsecs; i++) {
        off = (off + 15) & ~15ULL;
        sh_off[1 + i] = off;
        sh_size[1 + i] = (u64)secs[i].size;
        if (!secs[i].nobits) off += (u64)secs[i].size;
    }
    off = (off + 15) & ~15ULL;
    sh_off[idx_shstrtab] = off;
    sh_size[idx_shstrtab] = shstr.len;
    off += shstr.len;
    off = (off + 15) & ~15ULL;
    sh_off[idx_symtab] = off;
    sh_size[idx_symtab] = symtab.len;
    off += symtab.len;
    off = (off + 15) & ~15ULL;
    sh_off[idx_strtab] = off;
    sh_size[idx_strtab] = strtab.len;
    off += strtab.len;
    for (int i = 0; i < nsecs; i++) {
        if (rela_of[i] < 0) continue;
        off = (off + 15) & ~15ULL;
        sh_off[rela_of[i]] = off;
        sh_size[rela_of[i]] = rela[i].len;
        off += rela[i].len;
    }

    put_shdr(&out, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    for (int i = 0; i < nsecs; i++) {
        Section *s = &secs[i];
        u64 flags = (s->write ? 1 : 0) | (s->alloc ? 2 : 0) | (s->exec ? 4 : 0) | (s->tls ? 0x400 : 0);
        put_shdr(&out, name_off[1 + i], s->nobits ? SHT_NOBITS : SHT_PROGBITS, flags, sh_off[1 + i], sh_size[1 + i], 0, 0, (u64)s->align, (u64)s->entsize);
    }
    put_shdr(&out, name_off[idx_shstrtab], SHT_STRTAB, 0, sh_off[idx_shstrtab], sh_size[idx_shstrtab], 0, 0, 1, 0);
    put_shdr(&out, name_off[idx_symtab], SHT_SYMTAB, 0, sh_off[idx_symtab], sh_size[idx_symtab], (u32)idx_strtab, (u32)first_global, 8, 24);
    put_shdr(&out, name_off[idx_strtab], SHT_STRTAB, 0, sh_off[idx_strtab], sh_size[idx_strtab], 0, 0, 1, 0);
    for (int i = 0; i < nsecs; i++)
        if (rela_of[i] >= 0) put_shdr(&out, name_off[rela_of[i]], SHT_RELA, 0, sh_off[rela_of[i]], sh_size[rela_of[i]], (u32)idx_symtab, (u32)secs[i].shndx, 8, 24);

    /* contents */
    for (int i = 0; i < nsecs; i++) {
        if (secs[i].nobits) continue;
        while (out.len < sh_off[1 + i]) buf_u8(&out, 0);
        buf_put(&out, secs[i].data.p, secs[i].data.len);
    }
    while (out.len < sh_off[idx_shstrtab]) buf_u8(&out, 0);
    buf_put(&out, shstr.p, shstr.len);
    while (out.len < sh_off[idx_symtab]) buf_u8(&out, 0);
    buf_put(&out, symtab.p, symtab.len);
    while (out.len < sh_off[idx_strtab]) buf_u8(&out, 0);
    buf_put(&out, strtab.p, strtab.len);
    for (int i = 0; i < nsecs; i++) {
        if (rela_of[i] < 0) continue;
        while (out.len < sh_off[rela_of[i]]) buf_u8(&out, 0);
        buf_put(&out, rela[i].p, rela[i].len);
    }
    while (out.len & 15) buf_u8(&out, 0);                                   /* NASM ends the file on a 16-byte boundary */

    FILE *f = fopen(path, "wb");
    if (!f) fatal("cannot write '%s': %s", path, strerror(errno));
    if (fwrite(out.p, 1, out.len, f) != out.len) fatal("cannot write '%s'", path);
    fclose(f);
}
