/* ar.inc -- mar, the archiver (GNU ar format with a symbol table, like llvm-ar and GNU ar write it).
 *   mar rcs libfoo.a a.o b.o      create / replace members     mar t libfoo.a     list     mar x libfoo.a [member...]  extract
 */

typedef struct {
    char *name;
    u8 *data;
    size_t size;
    char **syms;
    int nsyms;
} ArEnt;

static void ar_collect_syms(ArEnt *e) {
    e->syms = NULL;
    e->nsyms = 0;
    if (e->size < sizeof(Ehdr) || memcmp(e->data, "\177ELF", 4) != 0) return;
    Ehdr *eh = (Ehdr *)e->data;
    if (eh->e_shoff == 0 || eh->e_shoff + (u64)eh->e_shnum * sizeof(Shdr) > e->size) return;
    Shdr *sh = (Shdr *)(e->data + eh->e_shoff);
    for (int i = 1; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_SYMTAB) continue;
        ElfSym *syms = (ElfSym *)(e->data + sh[i].sh_offset);
        size_t n = sh[i].sh_size / sizeof(ElfSym);
        const char *strtab = (const char *)(e->data + sh[sh[i].sh_link].sh_offset);
        for (size_t k = 1; k < n; k++) {
            int bind = ELF64_ST_BIND(syms[k].st_info), type = ELF64_ST_TYPE(syms[k].st_info);
            if (bind == STB_LOCAL || syms[k].st_shndx == SHN_UNDEF) continue;
            if (type == STT_SECTION || type == STT_FILE) continue;
            e->syms = xrealloc(e->syms, (size_t)(e->nsyms + 1) * sizeof(char *));
            e->syms[e->nsyms++] = (char *)(strtab + syms[k].st_name);
        }
    }
}

static void be32(Buf *b, u32 v) {
    buf_u8(b, (int)(v >> 24));
    buf_u8(b, (int)((v >> 16) & 0xff));
    buf_u8(b, (int)((v >> 8) & 0xff));
    buf_u8(b, (int)(v & 0xff));
}

static void ar_header(Buf *b, const char *name, size_t size) {
    char hdr[61];
    memset(hdr, ' ', 60);
    hdr[60] = 0;
    size_t n = strlen(name);
    memcpy(hdr, name, MIN(n, 16));
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%d", 0);
    memcpy(hdr + 16, tmp, strlen(tmp));
    snprintf(tmp, sizeof tmp, "%d", 0);
    memcpy(hdr + 28, tmp, strlen(tmp));
    memcpy(hdr + 34, tmp, strlen(tmp));
    snprintf(tmp, sizeof tmp, "%o", 0100644);
    memcpy(hdr + 40, tmp, strlen(tmp));
    snprintf(tmp, sizeof tmp, "%llu", (unsigned long long)size);
    memcpy(hdr + 48, tmp, strlen(tmp));
    hdr[58] = '`';
    hdr[59] = '\n';
    buf_put(b, hdr, 60);
}

/* Reads the members of an existing archive; returns NULL when the file does not exist. */
static ArEnt *ar_read(const char *path, int *count) {
    size_t len;
    u8 *img = read_whole_file(path, &len);
    *count = 0;
    if (!img) return NULL;
    if (len < 8 || memcmp(img, "!<arch>\n", 8) != 0) fatal("mar: %s: not an archive", path);
    ArEnt *ents = NULL;
    int n = 0;
    char *longnames = NULL;
    size_t pos = 8;
    while (pos + 60 <= len) {
        const char *h = (const char *)img + pos;
        char sizebuf[11];
        memcpy(sizebuf, h + 48, 10);
        sizebuf[10] = 0;
        size_t size = (size_t)strtoull(sizebuf, NULL, 10);
        pos += 60;
        if (pos + size > len) fatal("mar: %s: damaged archive", path);
        u8 *data = img + pos;
        if (h[0] == '/' && h[1] == ' ') {
            /* symbol table: rebuilt on write */
        } else if (h[0] == '/' && h[1] == '/') {
            longnames = (char *)data;
        } else {
            char *name;
            size_t dsize = size;
            if (h[0] == '#' && h[1] == '1' && h[2] == '/') {
                size_t nl = (size_t)atoi(h + 3);
                name = xstrndup((const char *)data, nl);
                data += nl;
                dsize -= nl;
            } else if (h[0] == '/' && h[1] >= '0' && h[1] <= '9' && longnames) {
                char *e = longnames + atoi(h + 1);
                size_t k = 0;
                while (e[k] && e[k] != '/' && e[k] != '\n') k++;
                name = xstrndup(e, k);
            } else {
                size_t k = 0;
                while (k < 16 && h[k] != '/' && h[k] != ' ') k++;
                name = xstrndup(h, k);
            }
            ents = xrealloc(ents, (size_t)(n + 1) * sizeof(ArEnt));
            memset(&ents[n], 0, sizeof(ArEnt));
            ents[n].name = name;
            ents[n].data = data;
            ents[n].size = dsize;
            n++;
        }
        pos += size + (size & 1);
    }
    *count = n;
    return ents;
}

static void ar_write(const char *path, ArEnt *ents, int n) {
    for (int i = 0; i < n; i++) ar_collect_syms(&ents[i]);

    /* sizes of the two special members */
    size_t nsyms = 0, symnames = 0;
    for (int i = 0; i < n; i++) {
        nsyms += (size_t)ents[i].nsyms;
        for (int k = 0; k < ents[i].nsyms; k++) symnames += strlen(ents[i].syms[k]) + 1;
    }
    size_t symsize = 4 + 4 * nsyms + symnames;
    size_t symsize_padded = symsize + (symsize & 1);

    Buf longnames = {0};
    size_t *name_off = xcalloc((size_t)(n + 1) * sizeof(size_t));
    bool have_long = false;
    for (int i = 0; i < n; i++) {
        if (strlen(ents[i].name) > 15) {
            have_long = true;
            name_off[i] = longnames.len;
            buf_str(&longnames, ents[i].name);
            buf_str(&longnames, "/\n");
        }
    }
    size_t longsize_padded = longnames.len + (longnames.len & 1);

    size_t first = 8 + 60 + symsize_padded + (have_long ? 60 + longsize_padded : 0);
    Buf out = {0};
    buf_str(&out, "!<arch>\n");

    ar_header(&out, "/", symsize);
    be32(&out, (u32)nsyms);
    size_t off = first;
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < ents[i].nsyms; k++) be32(&out, (u32)off);
        off += 60 + ents[i].size + (ents[i].size & 1);
    }
    for (int i = 0; i < n; i++)
        for (int k = 0; k < ents[i].nsyms; k++) {
            buf_str(&out, ents[i].syms[k]);
            buf_u8(&out, 0);
        }
    if (symsize & 1) buf_u8(&out, '\n');

    if (have_long) {
        ar_header(&out, "//", longnames.len);
        buf_put(&out, longnames.p, longnames.len);
        if (longnames.len & 1) buf_u8(&out, '\n');
    }

    for (int i = 0; i < n; i++) {
        char nm[32];
        if (strlen(ents[i].name) > 15) snprintf(nm, sizeof nm, "/%llu", (unsigned long long)name_off[i]);
        else snprintf(nm, sizeof nm, "%s/", ents[i].name);
        ar_header(&out, nm, ents[i].size);
        buf_put(&out, ents[i].data, ents[i].size);
        if (ents[i].size & 1) buf_u8(&out, '\n');
    }
    if (write_whole_file(path, out.p, out.len, false) != 0) fatal("mar: cannot write '%s': %s", path, strerror(errno));
}

static int mar_main(int argc, char **argv) {
    if (argc < 3) {
        printf("usage: mar rcs ARCHIVE FILE...   |   mar t ARCHIVE   |   mar x ARCHIVE [MEMBER...]\n");
        printf("mar writes GNU-format archives with a symbol table, the card index mld likes to read.\n");
        return argc < 2 ? 1 : 0;
    }
    const char *ops = argv[1];
    if (ops[0] == '-') ops++;
    const char *archive = argv[2];
    bool create = false, list = false, extract = false, del = false;
    for (const char *p = ops; *p; p++) {
        if (*p == 'r' || *p == 'q') create = true;
        else if (*p == 't') list = true;
        else if (*p == 'x') extract = true;
        else if (*p == 'd') del = true;
        else if (*p == 'c' || *p == 's' || *p == 'v' || *p == 'u' || *p == 'D' || *p == 'U') {
        } else fatal("mar: unknown operation '%c'", *p);
    }
    int n = 0;
    ArEnt *ents = ar_read(archive, &n);

    if (list) {
        if (!ents && !file_exists(archive)) fatal("mar: %s: no such file", archive);
        for (int i = 0; i < n; i++) printf("%s\n", ents[i].name);
        return 0;
    }

    if (extract) {
        if (!ents && !file_exists(archive)) fatal("mar: %s: no such file", archive);
        for (int i = 0; i < n; i++) {
            bool want = argc <= 3;
            for (int k = 3; k < argc; k++)
                if (str_eq(argv[k], ents[i].name)) want = true;
            if (!want) continue;
            if (write_whole_file(ents[i].name, ents[i].data, ents[i].size, false) != 0) fatal("mar: cannot write '%s'", ents[i].name);
        }
        return 0;
    }

    if (del) {
        int m = 0;
        for (int i = 0; i < n; i++) {
            bool drop = false;
            for (int k = 3; k < argc; k++)
                if (str_eq(argv[k], ents[i].name)) drop = true;
            if (!drop) ents[m++] = ents[i];
        }
        ar_write(archive, ents, m);
        return 0;
    }

    if (!create) fatal("mar: say what to do (r, t, x or d)");
    for (int k = 3; k < argc; k++) {
        size_t len;
        u8 *data = read_whole_file(argv[k], &len);
        if (!data) fatal("mar: cannot read '%s': %s", argv[k], strerror(errno));
        const char *name = base_of(argv[k]);
        int at = -1;
        for (int i = 0; i < n; i++)
            if (str_eq(ents[i].name, name)) at = i;
        if (at < 0) {
            ents = xrealloc(ents, (size_t)(n + 1) * sizeof(ArEnt));
            at = n++;
        }
        memset(&ents[at], 0, sizeof(ArEnt));
        ents[at].name = xstrdup(name);
        ents[at].data = data;
        ents[at].size = len;
    }
    ar_write(archive, ents, n);
    return 0;
}
