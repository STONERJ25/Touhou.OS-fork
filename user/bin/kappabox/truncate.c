/* truncate: shrink or extend (with zeros) a file to a given size. */
#include "kappabox.h"

static int parse_size(const char *s, long long *out) {
    char *end;
    long long v = strtoll(s, &end, 10);
    if (end == s) return -1;
    switch (*end) {
    case 0: break;
    case 'k': case 'K': v *= 1024; end++; break;
    case 'm': case 'M': v *= 1024 * 1024; end++; break;
    case 'g': case 'G': v *= 1024LL * 1024 * 1024; end++; break;
    default: return -1;
    }
    if (*end) return -1;
    *out = v;
    return 0;
}

APPLET(truncate) {
    const char *spec = NULL;
    int no_create = 0, i = 1, rc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) spec = argv[++i];
        else if (!strncmp(argv[i], "-s", 2)) spec = argv[i] + 2;
        else if (!strcmp(argv[i], "-c")) no_create = 1;
        else if (!strcmp(argv[i], "--")) { i++; break; }
        else { kb_error("invalid option '%s'", argv[i]); return 1; }
    }
    if (!spec || i >= argc) { kb_error("usage: truncate -s [+-]SIZE[KMG] [-c] file..."); return 1; }
    int rel = 0;
    if (*spec == '+') { rel = 1; spec++; }
    else if (*spec == '-') { rel = -1; spec++; }
    long long want;
    if (parse_size(spec, &want) < 0 || want < 0) { kb_error("invalid size '%s'", spec); return 1; }
    for (; i < argc; i++) {
        struct stat st;
        int exists = stat(argv[i], &st) == 0;
        if (!exists && no_create) continue;
        if (!exists && rel) { kb_error("cannot stat '%s': %s", argv[i], strerror(errno)); rc = 1; continue; }
        long long target = want;
        if (rel && exists) target = rel > 0 ? (long long)st.st_size + want : (long long)st.st_size - want;
        if (target < 0) target = 0;
        int fd = open(argv[i], O_WRONLY | O_CREAT, 0644);
        if (fd < 0) { kb_error("cannot open '%s': %s", argv[i], strerror(errno)); rc = 1; continue; }
        if (ftruncate(fd, (off_t)target) < 0) { kb_error("failed to truncate '%s': %s", argv[i], strerror(errno)); rc = 1; }
        close(fd);
    }
    return rc;
}
