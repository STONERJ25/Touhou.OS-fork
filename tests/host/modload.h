/*
 * modload.h: the module loader the host test programs (jstool, t262tool) give the engine as JSHostHooks.load_module.
 * A specifier is a file path: relative ("./x.js", "../y/x.js": to the directory of the importing module) or absolute.
 * The canonical name of a module is its normalized path ("." and ".." segments and doubled slashes removed), so that
 * two spellings of one file are one module.  The source and the name returned belong to this file until the next call
 * (the engine copies them at once).
 *
 * Include it once, in a program that has <js.h>, <stdio.h>, <stdlib.h> and <string.h>, and set
 * hooks.load_module = modload_hook.
 */
#ifndef MODLOAD_H
#define MODLOAD_H

static char *modload_src, *modload_name;

static char *modload_read(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 15, n = 0;
    char *b = malloc(cap + 1);
    for (;;) {
        if (n == cap) { cap *= 2; b = realloc(b, cap + 1); }
        size_t r = fread(b + n, 1, cap - n, f);
        if (!r) break;
        n += r;
    }
    fclose(f);
    b[n] = 0;
    *len = n;
    return b;
}

/* collapses "." and ".." and repeated slashes in place; a relative path may keep leading ".." segments */
static void modload_normalize(char *p) {
    bool is_abs = p[0] == '/';
    char *segs[512];
    int n = 0;
    char *in = p;
    while (*in) {
        while (*in == '/') in++;
        if (!*in) break;
        char *seg = in;
        while (*in && *in != '/') in++;
        bool more = *in != 0;
        *in = 0;                                    /* (terminates the segment in place) */
        if (more) in++;
        if (!strcmp(seg, ".")) continue;
        if (!strcmp(seg, "..")) {
            if (n > 0 && strcmp(segs[n - 1], "..")) { n--; continue; }
            if (is_abs) continue;
        }
        if (n < 512) segs[n++] = seg;
    }
    char tmp[4096];
    size_t o = 0;
    if (is_abs) tmp[o++] = '/';
    for (int i = 0; i < n; i++) {
        size_t l = strlen(segs[i]);
        if (o + l + 2 >= sizeof tmp) break;
        memcpy(tmp + o, segs[i], l);
        o += l;
        if (i + 1 < n) tmp[o++] = '/';
    }
    tmp[o] = 0;
    strcpy(p, o ? tmp : ".");
}

static char *modload_hook(JSContext *ctx, const char *spec, const char *referrer, size_t *len, char **resolved,
                          const char **error, void *opaque) {
    (void)ctx; (void)opaque;
    free(modload_src);
    free(modload_name);
    modload_src = modload_name = NULL;
    char path[4096];
    if (spec[0] == '/') snprintf(path, sizeof path, "%s", spec);
    else if (!strncmp(spec, "./", 2) || !strncmp(spec, "../", 3) || !strcmp(spec, ".") || !strcmp(spec, "..")) {
        const char *slash = strrchr(referrer, '/');
        size_t dir = slash ? (size_t)(slash - referrer) + 1 : 0;
        snprintf(path, sizeof path, "%.*s%s", (int)dir, referrer, spec);
    } else {
        *error = "only relative and absolute paths can be imported";
        return NULL;
    }
    modload_normalize(path);
    modload_src = modload_read(path, len);
    if (!modload_src) {
        *error = "no such file";
        return NULL;
    }
    modload_name = strdup(path);
    *resolved = modload_name;
    return modload_src;
}

#endif
