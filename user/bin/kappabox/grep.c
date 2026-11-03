/* grep / egrep / fgrep on top of libgap's regex engine. */
#include "kappabox.h"
#include <regex.h>

typedef struct {
    int invert, icase, count, list, list_non, with_name, no_name, line_num, recursive, follow, fixed, extended;
    int word, whole, only, quiet, no_msgs, color, byte_off, binary_text, null_name, skip_binary;
    long max_count;
    int after, before;
    int npat;
    char **pat;
    regex_t *re;
    char **includes, **excludes, **exclude_dirs;
    int ninc, nexc, nexcd;
} Grep;

static Grep g;

#define C_MATCH "\033[01;31m\033[K"
#define C_FILE  "\033[35m\033[K"
#define C_SEP   "\033[36m\033[K"
#define C_NUM   "\033[32m\033[K"
#define C_END   "\033[m\033[K"

static int is_word_char(int c) { return isalnum(c) || c == '_'; }

/* Finds the leftmost-longest match of any pattern at or after `from`. */
static int find_match(const char *line, size_t from, size_t *ms, size_t *me) {
    size_t best_s = (size_t)-1, best_e = 0;
    int found = 0;
    size_t len = strlen(line);
    for (int i = 0; i < g.npat; i++) {
        size_t pos = from;
        while (pos <= len) {
            size_t s, e;
            if (g.fixed) {
                const char *hit = g.icase ? strcasestr(line + pos, g.pat[i]) : strstr(line + pos, g.pat[i]);
                if (!hit) break;
                s = (size_t)(hit - line);
                e = s + strlen(g.pat[i]);
            } else {
                regmatch_t m[1];
                if (regexec(&g.re[i], line + pos, 1, m, pos > 0 ? REG_NOTBOL : 0) != 0) break;
                s = pos + (size_t)m[0].rm_so;
                e = pos + (size_t)m[0].rm_eo;
            }
            int ok = 1;
            if (g.whole) ok = s == 0 && e == len;
            else if (g.word) ok = (s == 0 || !is_word_char((unsigned char)line[s - 1])) && (e == len || !is_word_char((unsigned char)line[e]));
            if (g.whole && !ok && !g.fixed) break;
            if (ok) {
                if (e == s && len > 0 && g.only) { pos = s + 1; continue; }     /* empty matches are not worth printing with -o */
                if (!found || s < best_s || (s == best_s && e > best_e)) { best_s = s; best_e = e; found = 1; }
                break;
            }
            pos = s + 1;
        }
    }
    if (found) { *ms = best_s; *me = best_e; }
    return found;
}

typedef struct { const char *name; long total_matches; int printed_sep; } Ctx;

static void emit_prefix(const char *name, long lineno, long offset, char sep) {
    if (g.with_name) {
        if (g.color) printf(C_FILE "%s" C_END C_SEP "%c" C_END, name, g.null_name ? 0 : sep);
        else { fputs(name, stdout); putchar(g.null_name ? 0 : sep); }
    }
    if (g.line_num) {
        if (g.color) printf(C_NUM "%ld" C_END C_SEP "%c" C_END, lineno, sep);
        else printf("%ld%c", lineno, sep);
    }
    if (g.byte_off) printf("%ld%c", offset, sep);
}

static void print_line(const char *name, const char *line, long lineno, long offset, char sep) {
    if (g.only) {
        size_t from = 0, s, e;
        while (from <= strlen(line) && find_match(line, from, &s, &e) && e > s) {
            emit_prefix(name, lineno, offset + (long)s, sep);
            if (g.color) fputs(C_MATCH, stdout);
            fwrite(line + s, 1, e - s, stdout);
            if (g.color) fputs(C_END, stdout);
            putchar('\n');
            from = e;
        }
        return;
    }
    emit_prefix(name, lineno, offset, sep);
    if (g.color && sep == ':' && !g.invert) {
        size_t from = 0, s, e, len = strlen(line);
        while (from <= len && find_match(line, from, &s, &e)) {
            fwrite(line + from, 1, s - from, stdout);
            if (e > s) { fputs(C_MATCH, stdout); fwrite(line + s, 1, e - s, stdout); fputs(C_END, stdout); }
            if (e == s) { if (s < len) putchar(line[s]); from = s + 1; } else from = e;
        }
        if (from < len) fputs(line + from, stdout);
    } else fputs(line, stdout);
    putchar('\n');
}

static int grep_stream(FILE *f, const char *name) {
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    long lineno = 0, offset = 0, matches = 0;
    long last_printed = 0;
    int after_left = 0;
    char **before = g.before ? calloc((size_t)g.before, sizeof(char *)) : NULL;
    long *before_no = g.before ? calloc((size_t)g.before, sizeof(long)) : NULL;
    int nbefore = 0, bpos = 0, any_output = 0, binary = 0;

    /* A NUL byte in the first block means a binary file. Only seekable files can be probed without eating input;
     * pipes are checked line by line below. */
    long start_pos = g.binary_text ? -1 : ftell(f);
    if (start_pos >= 0) {
        char probe[512];
        size_t n = fread(probe, 1, sizeof probe, f);
        for (size_t i = 0; i < n; i++) if (!probe[i]) binary = 1;
        if (fseek(f, start_pos, SEEK_SET) != 0) rewind(f);
    }

    while ((len = getline(&line, &cap, f)) >= 0) {
        lineno++;
        long this_off = offset;
        offset += len;
        if (!g.binary_text && !binary && (size_t)len != strlen(line)) binary = 1;
        if (len && line[len - 1] == '\n') line[--len] = 0;
        size_t s, e;
        int hit = find_match(line, 0, &s, &e);
        if (g.invert) hit = !hit;
        if (hit) {
            matches++;
            if (g.quiet) { free(line); return 1; }
            if (g.list || g.list_non) break;
            if (!g.count) {
                if (binary) { printf("Binary file %s matches\n", name); break; }
                if (g.before || g.after) {
                    if (any_output && last_printed && lineno - nbefore - 1 > last_printed) puts(g.color ? C_SEP "--" C_END : "--");
                    for (int i = 0; i < nbefore; i++) {
                        int idx = (bpos - nbefore + i + g.before * 2) % g.before;
                        print_line(name, before[idx], before_no[idx], 0, '-');
                    }
                    nbefore = 0;
                }
                print_line(name, line, lineno, this_off, ':');
                last_printed = lineno;
                any_output = 1;
                after_left = g.after;
            }
            if (g.max_count && matches >= g.max_count) break;
        } else if (!g.count && !g.quiet) {
            if (after_left > 0) {
                print_line(name, line, lineno, this_off, '-');
                last_printed = lineno;
                after_left--;
            } else if (g.before) {
                free(before[bpos]);
                before[bpos] = strdup(line);
                before_no[bpos] = lineno;
                bpos = (bpos + 1) % g.before;
                if (nbefore < g.before) nbefore++;
            }
        }
    }
    free(line);
    if (before) { for (int i = 0; i < g.before; i++) free(before[i]); free(before); free(before_no); }
    if (g.count) {
        if (g.with_name) { if (g.color) printf(C_FILE "%s" C_END C_SEP ":" C_END, name); else printf("%s:", name); }
        printf("%ld\n", matches);
    }
    if (g.list && matches) { printf("%s%c", name, g.null_name ? 0 : '\n'); }
    if (g.list_non && !matches) { printf("%s%c", name, g.null_name ? 0 : '\n'); }
    return matches > 0;
}

static int name_matches(char **globs, int n, const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    for (int i = 0; i < n; i++) {
        /* tiny glob: * and ? only */
        const char *p = globs[i], *s = base, *star = NULL, *mark = NULL;
        while (*s) {
            if (*p == '*') { star = p++; mark = s; }
            else if (*p == '?' || *p == *s) { p++; s++; }
            else if (star) { p = star + 1; s = ++mark; }
            else break;
        }
        if (!*s) { while (*p == '*') p++; if (!*p) return 1; }
    }
    return 0;
}

static int grep_path(const char *path, int top) {
    struct stat st;
    int rc = 0;
    if ((top || g.follow ? stat(path, &st) : lstat(path, &st)) < 0) {
        if (!g.no_msgs) kb_error("%s: %s", path, strerror(errno));
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        if (!g.recursive) { if (!g.no_msgs) kb_error("%s: Is a directory", path); return -1; }
        if (!top && g.nexcd && name_matches(g.exclude_dirs, g.nexcd, path)) return 0;
        DIR *d = opendir(path);
        if (!d) { if (!g.no_msgs) kb_error("%s: %s", path, strerror(errno)); return -1; }
        struct dirent *de;
        char **names = NULL;
        int n = 0;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            names = realloc(names, (size_t)(n + 1) * sizeof *names);
            names[n++] = strdup(de->d_name);
        }
        closedir(d);
        for (int i = 0; i < n; i++) {
            char *child = kb_join(path, names[i]);
            int r = grep_path(child, 0);
            if (r > 0) rc = 1; else if (r < 0 && !rc) rc = rc ? rc : -1;
            free(child);
            free(names[i]);
        }
        free(names);
        return rc;
    }
    if (!top && S_ISLNK(st.st_mode) && !g.follow) return 0;
    if (g.ninc && !name_matches(g.includes, g.ninc, path)) return 0;
    if (g.nexc && name_matches(g.excludes, g.nexc, path)) return 0;
    FILE *f = fopen(path, "r");
    if (!f) { if (!g.no_msgs) kb_error("%s: %s", path, strerror(errno)); return -1; }
    int r = grep_stream(f, path);
    fclose(f);
    return r;
}

static void add_pattern(const char *p) {
    g.pat = realloc(g.pat, (size_t)(g.npat + 1) * sizeof *g.pat);
    g.pat[g.npat++] = strdup(p);
}

static void add_pattern_lines(const char *text) {
    char *copy = strdup(text), *save, *line;
    for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) add_pattern(line);
    if (!*text) add_pattern("");
    free(copy);
}

static int grep_main(int argc, char **argv, int mode) {
    memset(&g, 0, sizeof g);
    g.extended = mode == 'E';
    g.fixed = mode == 'F';
    int have_pattern = 0, c, color_when = 0;           /* 0 never, 1 auto, 2 always */
    optind = 0;
    static const struct option lopts[] = {
        { "color", 2, NULL, 1000 }, { "colour", 2, NULL, 1000 }, { "include", 1, NULL, 1001 }, { "exclude", 1, NULL, 1002 },
        { "exclude-dir", 1, NULL, 1003 }, { "null", 0, NULL, 'Z' }, { "recursive", 0, NULL, 'r' }, { "ignore-case", 0, NULL, 'i' },
        { "invert-match", 0, NULL, 'v' }, { "count", 0, NULL, 'c' }, { "line-number", 0, NULL, 'n' }, { "quiet", 0, NULL, 'q' },
        { "silent", 0, NULL, 'q' }, { "files-with-matches", 0, NULL, 'l' }, { "files-without-match", 0, NULL, 'L' },
        { "only-matching", 0, NULL, 'o' }, { "word-regexp", 0, NULL, 'w' }, { "line-regexp", 0, NULL, 'x' },
        { "extended-regexp", 0, NULL, 'E' }, { "fixed-strings", 0, NULL, 'F' }, { "regexp", 1, NULL, 'e' }, { "file", 1, NULL, 'f' },
        { "max-count", 1, NULL, 'm' }, { "with-filename", 0, NULL, 'H' }, { "no-filename", 0, NULL, 'h' }, { "text", 0, NULL, 'a' },
        { "no-messages", 0, NULL, 's' }, { "dereference-recursive", 0, NULL, 'R' }, { "after-context", 1, NULL, 'A' },
        { "before-context", 1, NULL, 'B' }, { "context", 1, NULL, 'C' }, { "byte-offset", 0, NULL, 'b' }, { 0, 0, 0, 0 },
    };
    while ((c = getopt_long(argc, argv, "EFGPe:f:iyvwxcLlm:oqsbHhnrRA:B:C:aIZ0123456789", lopts, NULL)) != -1) {
        switch (c) {
        case 'E': case 'P': g.extended = 1; g.fixed = 0; break;
        case 'F': g.fixed = 1; break;
        case 'G': g.extended = 0; g.fixed = 0; break;
        case 'e': add_pattern_lines(optarg); have_pattern = 1; break;
        case 'f': {
            FILE *pf = fopen(optarg, "r");
            if (!pf) { kb_error("%s: %s", optarg, strerror(errno)); return 2; }
            char **lines;
            size_t n = kb_read_lines(pf, &lines);
            for (size_t i = 0; i < n; i++) add_pattern(lines[i]);
            kb_free_lines(lines, n);
            fclose(pf);
            have_pattern = 1;
            break;
        }
        case 'i': case 'y': g.icase = 1; break;
        case 'v': g.invert = 1; break;
        case 'w': g.word = 1; break;
        case 'x': g.whole = 1; break;
        case 'c': g.count = 1; break;
        case 'L': g.list_non = 1; break;
        case 'l': g.list = 1; break;
        case 'm': g.max_count = atol(optarg); break;
        case 'o': g.only = 1; break;
        case 'q': g.quiet = 1; break;
        case 's': g.no_msgs = 1; break;
        case 'b': g.byte_off = 1; break;
        case 'H': g.with_name = 1; break;
        case 'h': g.no_name = 1; break;
        case 'n': g.line_num = 1; break;
        case 'r': g.recursive = 1; break;
        case 'R': g.recursive = 1; g.follow = 1; break;
        case 'A': g.after = atoi(optarg); break;
        case 'B': g.before = atoi(optarg); break;
        case 'C': g.after = g.before = atoi(optarg); break;
        case 'a': g.binary_text = 1; break;
        case 'I': g.skip_binary = 1; break;
        case 'Z': g.null_name = 1; break;
        case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
            g.after = g.before = c - '0';
            break;
        case 1000: color_when = !optarg || !strcmp(optarg, "always") || !strcmp(optarg, "yes") ? 2 : !strcmp(optarg, "auto") || !strcmp(optarg, "tty") ? 1 : 0; break;
        case 1001: g.includes = realloc(g.includes, (size_t)(g.ninc + 1) * sizeof(char *)); g.includes[g.ninc++] = optarg; break;
        case 1002: g.excludes = realloc(g.excludes, (size_t)(g.nexc + 1) * sizeof(char *)); g.excludes[g.nexc++] = optarg; break;
        case 1003: g.exclude_dirs = realloc(g.exclude_dirs, (size_t)(g.nexcd + 1) * sizeof(char *)); g.exclude_dirs[g.nexcd++] = optarg; break;
        default: return 2;
        }
    }
    if (!have_pattern) {
        if (optind >= argc) { fprintf(stderr, "Usage: %s [OPTION]... PATTERNS [FILE]...\n", kb_name); return 2; }
        add_pattern_lines(argv[optind++]);
    }
    g.color = color_when == 2 || (color_when == 1 && kb_color);
    if (!g.fixed) {
        g.re = calloc((size_t)g.npat, sizeof *g.re);
        for (int i = 0; i < g.npat; i++) {
            int rc = regcomp(&g.re[i], g.pat[i], (g.extended ? REG_EXTENDED : 0) | (g.icase ? REG_ICASE : 0));
            if (rc) { char msg[128]; regerror(rc, &g.re[i], msg, sizeof msg); kb_error("%s: %s", g.pat[i], msg); return 2; }
        }
    }
    int nfiles = argc - optind;
    if (nfiles > 1 || g.recursive) g.with_name = 1;
    if (g.no_name) g.with_name = 0;
    int status = 1, errors = 0;
    if (nfiles == 0) {
        if (g.recursive) {
            int r = grep_path(".", 1);
            if (r > 0) status = 0; else if (r < 0) errors = 1;
        } else {
            int r = grep_stream(stdin, "(standard input)");
            if (r > 0) status = 0;
        }
    }
    for (int i = optind; i < argc; i++) {
        int r;
        if (!strcmp(argv[i], "-")) r = grep_stream(stdin, "(standard input)");
        else r = grep_path(argv[i], 1);
        if (r > 0) status = 0; else if (r < 0) errors = 1;
        if (status == 0 && g.quiet) return 0;
    }
    fflush(stdout);
    return errors && status ? 2 : status;
}

APPLET(grep) { return grep_main(argc, argv, 'G'); }
APPLET(egrep) { return grep_main(argc, argv, 'E'); }
APPLET(fgrep) { return grep_main(argc, argv, 'F'); }
