/* df: how much room each realm of Gensokyo has left (Keine keeps the ledger). */
#include "kappabox.h"
#include <sys/statfs.h>

static int o_human, o_type, o_all, o_posix;

static void fmt_bytes(unsigned long long bytes, char *out, size_t n) {
    if (o_human) kb_format_size(bytes, out, n);
    else snprintf(out, n, "%llu", bytes / 1024ull);
}

static void row(const struct statfs *st, const char *src, const char *mnt) {
    unsigned long long bs = st->f_bsize ? st->f_bsize : 4096;
    unsigned long long total = st->f_blocks * bs, avail = st->f_bavail * bs;
    unsigned long long used = st->f_blocks > st->f_bfree ? (st->f_blocks - st->f_bfree) * bs : 0;
    char a[24], b[24], c[24], pct[8];
    fmt_bytes(total, a, sizeof a);
    fmt_bytes(used, b, sizeof b);
    fmt_bytes(avail, c, sizeof c);
    unsigned long long denom = used + avail;
    if (denom) snprintf(pct, sizeof pct, "%llu%%", (used * 100 + denom - 1) / denom);
    else strcpy(pct, "-");
    printf("%-18s ", src);
    if (o_type) printf("%-7s ", st->f_fstypename[0] ? st->f_fstypename : "?");
    printf("%10s %10s %10s %4s %s%s\n", a, b, c, pct, mnt, (st->f_flags & MNT_RDONLY) ? " (ro)" : "");
}

static int one(const char *path, int explicit_path) {
    struct statfs st;
    if (statfs(path, &st) < 0) { kb_error("%s: %s", path, strerror(errno)); return 1; }
    if (!explicit_path && !o_all && st.f_blocks == 0) return 0;          /* /proc and friends hold nothing */
    row(&st, st.f_mntfromname[0] ? st.f_mntfromname : "-", st.f_mntonname[0] ? st.f_mntonname : path);
    return 0;
}

APPLET(df) {
    int i = 1, rc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'h') o_human = 1;
            else if (*o == 'k') o_human = 0;
            else if (*o == 'T') o_type = 1;
            else if (*o == 'a') o_all = 1;
            else if (*o == 'P') o_posix = 1;
            else { kb_error("invalid option -- '%c'\nusage: df [-hkTaP] [path...]", *o); return 2; }
        }
    }
    if (isatty(1) && !o_posix) printf("\x1b[1mKeine's ledger of the lands\x1b[0m -- how much room each realm has left\n");
    printf("%-18s ", "Filesystem");
    if (o_type) printf("%-7s ", "Type");
    printf("%10s %10s %10s %4s %s\n", o_human ? "Size" : "1K-blocks", "Used", o_human ? "Avail" : "Available", "Use%", "Mounted on");
    if (i < argc) {
        for (; i < argc; i++) rc |= one(argv[i], 1);
        return rc;
    }
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) { kb_error("/proc/mounts: %s", strerror(errno)); return 1; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char src[64], tgt[64];
        if (sscanf(line, "%63s %63s", src, tgt) == 2) rc |= one(tgt, 0);
    }
    fclose(f);
    return rc;
}
