/* Shell wildcard matching: * ? [set] [!set] [a-z] [[:class:]] and backslash escapes. */
#include <fnmatch.h>
#include <ctype.h>
#include <string.h>

static int fold(int c, int flags) { return (flags & FNM_CASEFOLD) ? tolower(c) : c; }

static int class_match(const char *name, size_t n, int c) {
    static const struct { const char *name; int (*fn)(int); } tab[] = {
        { "alpha", isalpha }, { "digit", isdigit }, { "alnum", isalnum }, { "upper", isupper }, { "lower", islower },
        { "space", isspace }, { "blank", isblank }, { "punct", ispunct }, { "print", isprint }, { "graph", isgraph },
        { "cntrl", iscntrl }, { "xdigit", isxdigit },
    };
    for (unsigned i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (strlen(tab[i].name) == n && !strncmp(tab[i].name, name, n)) return tab[i].fn(c);
    return 0;
}

/* Matches c against the bracket expression starting after '['. Returns the position after ']' or NULL if malformed. */
static const char *bracket(const char *p, int c, int flags, int *matched) {
    int neg = 0, ok = 0, first = 1;
    if (*p == '!' || *p == '^') { neg = 1; p++; }
    while (*p && (*p != ']' || first)) {
        first = 0;
        if (p[0] == '[' && p[1] == ':') {
            const char *e = strstr(p + 2, ":]");
            if (e) { if (class_match(p + 2, (size_t)(e - p - 2), c)) ok = 1; p = e + 2; continue; }
        }
        int lo = (unsigned char)*p++;
        if (lo == '\\' && !(flags & FNM_NOESCAPE) && *p) lo = (unsigned char)*p++;
        int hi = lo;
        if (*p == '-' && p[1] && p[1] != ']') {
            p++;
            hi = (unsigned char)*p++;
            if (hi == '\\' && !(flags & FNM_NOESCAPE) && *p) hi = (unsigned char)*p++;
        }
        int fc = fold(c, flags);
        if ((fc >= fold(lo, flags) && fc <= fold(hi, flags)) || (c >= lo && c <= hi)) ok = 1;
    }
    if (*p != ']') return NULL;
    *matched = neg ? !ok : ok;
    return p + 1;
}

static int match(const char *p, const char *s, int flags, const char *start) {
    for (;;) {
        char pc = *p;
        if (!pc) return *s ? FNM_NOMATCH : 0;
        if (pc == '*') {
            while (*p == '*') p++;
            if ((flags & FNM_PERIOD) && *s == '.' && (s == start || ((flags & FNM_PATHNAME) && s[-1] == '/'))) return FNM_NOMATCH;
            if (!*p) {
                if (flags & FNM_PATHNAME) return strchr(s, '/') ? FNM_NOMATCH : 0;
                return 0;
            }
            for (const char *t = s;; t++) {
                if (!match(p, t, flags, start)) return 0;
                if (!*t || ((flags & FNM_PATHNAME) && *t == '/')) return FNM_NOMATCH;
            }
        }
        if (!*s) return FNM_NOMATCH;
        if (pc == '?') {
            if ((flags & FNM_PATHNAME) && *s == '/') return FNM_NOMATCH;
            if ((flags & FNM_PERIOD) && *s == '.' && (s == start || ((flags & FNM_PATHNAME) && s[-1] == '/'))) return FNM_NOMATCH;
            p++; s++;
            continue;
        }
        if (pc == '[') {
            int ok = 0;
            const char *after = bracket(p + 1, (unsigned char)*s, flags, &ok);
            if (after) {
                if (!ok || ((flags & FNM_PATHNAME) && *s == '/')) return FNM_NOMATCH;
                if ((flags & FNM_PERIOD) && *s == '.' && (s == start || ((flags & FNM_PATHNAME) && s[-1] == '/'))) return FNM_NOMATCH;
                p = after; s++;
                continue;
            }
        }
        if (pc == '\\' && !(flags & FNM_NOESCAPE) && p[1]) { p++; pc = *p; }
        if (fold((unsigned char)pc, flags) != fold((unsigned char)*s, flags)) return FNM_NOMATCH;
        p++; s++;
    }
}

int fnmatch(const char *pattern, const char *string, int flags) { return match(pattern, string, flags, string); }
