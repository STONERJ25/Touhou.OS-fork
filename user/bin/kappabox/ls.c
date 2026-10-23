/* ls: list directory contents (colours on a terminal, columns, long format, recursion). */
#include "kappabox.h"

typedef struct {
    char *name;
    char *path;
    struct stat st;
    char link[PATH_MAX];
    int ok;
} Entry;

static int o_a, o_A, o_l, o_1, o_R, o_d, o_h, o_F, o_t, o_r, o_S, o_i, o_p;

static const char *color_for(const Entry *e) {
    unsigned m = e->st.st_mode;
    if (!kb_color || !e->ok) return "";
    if (S_ISDIR(m)) return "\x1b[1;34m";
    if (S_ISLNK(m)) return "\x1b[1;36m";
    if (S_ISCHR(m) || S_ISBLK(m)) return "\x1b[1;33m";
    if (S_ISFIFO(m)) return "\x1b[33m";
    if (m & 0111) return "\x1b[1;32m";
    return "";
}

static char suffix_for(const Entry *e) {
    unsigned m = e->st.st_mode;
    if (!e->ok) return 0;
    if (S_ISDIR(m)) return (o_F || o_p) ? '/' : 0;
    if (!o_F) return 0;
    if (S_ISLNK(m)) return '@';
    if (S_ISFIFO(m)) return '|';
    if (m & 0111) return '*';
    return 0;
}

static int cmp_entries(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    int r;
    if (o_S) r = x->st.st_size < y->st.st_size ? 1 : x->st.st_size > y->st.st_size ? -1 : 0;
    else if (o_t) r = x->st.st_mtime < y->st.st_mtime ? 1 : x->st.st_mtime > y->st.st_mtime ? -1 : 0;
    else r = 0;
    if (!r) r = strcmp(x->name, y->name);
    return o_r ? -r : r;
}

static void print_name(const Entry *e) {
    const char *c = color_for(e);
    printf("%s%s%s", c, e->name, c[0] ? "\x1b[0m" : "");
    char s = suffix_for(e);
    if (s) putchar(s);
}

static int name_width(const Entry *e) { return (int)strlen(e->name) + (suffix_for(e) ? 1 : 0); }

static void print_long(const Entry *e) {
    char ms[11], when[32], size[32];
    kb_mode_string(e->ok ? e->st.st_mode : 0, ms);
    time_t now = time(NULL), t = e->st.st_mtime;
    struct tm tm;
    localtime_r(&t, &tm);
    if (t > now - 180L * 86400 && t < now + 3600) strftime(when, sizeof when, "%b %e %H:%M", &tm);
    else strftime(when, sizeof when, "%b %e  %Y", &tm);
    if (o_h) kb_format_size((unsigned long long)e->st.st_size, size, sizeof size);
    else snprintf(size, sizeof size, "%ld", (long)e->st.st_size);
    if (o_i) printf("%6lu ", (unsigned long)e->st.st_ino);
    printf("%s %2u %-8s %-8s %8s %s ", ms, e->st.st_nlink, kb_user_name(e->st.st_uid), kb_group_name(e->st.st_gid), size, when);
    print_name(e);
    if (S_ISLNK(e->st.st_mode) && e->link[0]) printf(" -> %s", e->link);
    putchar('\n');
}

static void print_entries(Entry *ents, int n) {
    if (o_l || o_1 || !kb_color) {                       /* one per line when piped (like GNU ls) */
        for (int i = 0; i < n; i++) {
            if (o_l) print_long(&ents[i]);
            else { if (o_i) printf("%6lu ", (unsigned long)ents[i].st.st_ino); print_name(&ents[i]); putchar('\n'); }
        }
        return;
    }
    int maxw = 0;
    for (int i = 0; i < n; i++) { int w = name_width(&ents[i]) + (o_i ? 7 : 0); if (w > maxw) maxw = w; }
    int cols = kb_term_cols() / (maxw + 2);
    if (cols < 1) cols = 1;
    int rows = (n + cols - 1) / cols;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int i = c * rows + r;
            if (i >= n) break;
            if (o_i) printf("%6lu ", (unsigned long)ents[i].st.st_ino);
            print_name(&ents[i]);
            int w = name_width(&ents[i]) + (o_i ? 7 : 0);
            if (c + 1 < cols && (c + 1) * rows + r < n) for (int k = w; k < maxw + 2; k++) putchar(' ');
        }
        putchar('\n');
    }
}

static void fill(Entry *e, const char *path) {
    e->path = strdup(path);
    e->ok = lstat(path, &e->st) == 0;
    e->link[0] = 0;
    if (e->ok && S_ISLNK(e->st.st_mode)) {
        ssize_t n = readlink(path, e->link, sizeof e->link - 1);
        e->link[n > 0 ? n : 0] = 0;
    }
}

static int list_dir(const char *path, int header, int *printed_any) {
    DIR *d = opendir(path);
    if (!d) { kb_error("cannot access '%s': %s", path, strerror(errno)); return 1; }
    Entry *ents = NULL;
    int n = 0, cap = 0;
    long total = 0;
    for (struct dirent *de; (de = readdir(d));) {
        if (de->d_name[0] == '.' && !o_a && !o_A) continue;
        if (o_A && !o_a && (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))) continue;
        if (n == cap) { cap = cap ? cap * 2 : 32; ents = realloc(ents, (size_t)cap * sizeof *ents); }
        char *full = kb_join(path, de->d_name);
        fill(&ents[n], full);
        free(full);
        ents[n].name = strdup(de->d_name);
        total += (ents[n].st.st_size + 1023) / 1024;
        n++;
    }
    closedir(d);
    qsort(ents, (size_t)n, sizeof *ents, cmp_entries);
    if (*printed_any) putchar('\n');
    if (header) printf("%s:\n", path);
    if (o_l) printf("total %ld\n", total);
    print_entries(ents, n);
    *printed_any = 1;
    int rc = 0;
    if (o_R) {
        for (int i = 0; i < n; i++) {
            if (ents[i].ok && S_ISDIR(ents[i].st.st_mode) && strcmp(ents[i].name, ".") && strcmp(ents[i].name, ".."))
                rc |= list_dir(ents[i].path, 1, printed_any);
        }
    }
    for (int i = 0; i < n; i++) { free(ents[i].name); free(ents[i].path); }
    free(ents);
    return rc;
}

APPLET(ls) {
    o_a = o_A = o_l = o_1 = o_R = o_d = o_h = o_F = o_t = o_r = o_S = o_i = o_p = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *o = argv[i] + 1; *o; o++) {
            switch (*o) {
            case 'a': o_a = 1; break;
            case 'A': o_A = 1; break;
            case 'l': o_l = 1; break;
            case '1': o_1 = 1; break;
            case 'R': o_R = 1; break;
            case 'd': o_d = 1; break;
            case 'h': o_h = 1; break;
            case 'F': o_F = 1; break;
            case 'p': o_p = 1; break;
            case 't': o_t = 1; break;
            case 'r': o_r = 1; break;
            case 'S': o_S = 1; break;
            case 'i': o_i = 1; break;
            case 'C': break;
            default: kb_error("invalid option -- '%c'", *o); return 2;
            }
        }
    }
    int rc = 0, printed = 0;
    if (i >= argc) return list_dir(".", o_R, &printed);

    /* operands: files first, then directories */
    int n = argc - i;
    Entry *ents = calloc((size_t)n, sizeof *ents);
    for (int k = 0; k < n; k++) { fill(&ents[k], argv[i + k]); ents[k].name = strdup(argv[i + k]); }
    int nfiles = 0;
    Entry *files = calloc((size_t)n, sizeof *files);
    for (int k = 0; k < n; k++) {
        if (!ents[k].ok) { kb_error("cannot access '%s': %s", ents[k].name, strerror(errno)); rc = 2; continue; }
        struct stat st;
        int follow_dir = !o_d && stat(ents[k].name, &st) == 0 && S_ISDIR(st.st_mode);
        if (!follow_dir) files[nfiles++] = ents[k];
    }
    qsort(files, (size_t)nfiles, sizeof *files, cmp_entries);
    if (nfiles) { print_entries(files, nfiles); printed = 1; }
    for (int k = 0; k < n; k++) {
        struct stat st;
        if (ents[k].ok && !o_d && stat(ents[k].name, &st) == 0 && S_ISDIR(st.st_mode))
            rc |= list_dir(ents[k].name, n > 1 || o_R, &printed);
    }
    return rc;
}
