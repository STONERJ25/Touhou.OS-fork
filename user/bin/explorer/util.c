/* Paths, formatting, file types and small helpers shared by the whole Explorer. Nothing here needs a window. */
#include "explorer.h"
#include <ctype.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

bool g_trace;

uint64_t ex_now_ms(void) { return k_now_ms(); }

void ex_trace(const char *fmt, ...) {
    if (!g_trace) return;
    char buf[480];
    int n = snprintf(buf, sizeof buf, "ex: ");
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof buf - (size_t)n - 2, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
    buf[n++] = '\n';
    if (write(1, buf, (size_t)n) < 0) { }
}

/* ---------------------------------------------------------------- paths */

void path_join(char *out, size_t cap, const char *dir, const char *name) {
    char tmp[EX_PATH * 2];
    size_t dl = strlen(dir);
    if (dl == 0 || (dl == 1 && dir[0] == '/')) snprintf(tmp, sizeof tmp, "/%s", name);
    else if (dir[dl - 1] == '/') snprintf(tmp, sizeof tmp, "%s%s", dir, name);
    else snprintf(tmp, sizeof tmp, "%s/%s", dir, name);
    strlcpy(out, tmp, cap);
}

void path_parent(const char *path, char *out, size_t cap) {
    char tmp[EX_PATH];
    strlcpy(tmp, path, sizeof tmp);
    size_t n = strlen(tmp);
    while (n > 1 && tmp[n - 1] == '/') tmp[--n] = 0;
    char *s = strrchr(tmp, '/');
    if (!s) { strlcpy(out, ".", cap); return; }
    if (s == tmp) tmp[1] = 0;
    else *s = 0;
    strlcpy(out, tmp, cap);
}

const char *path_base(const char *path) {
    const char *s = strrchr(path, '/');
    if (!s) return path;
    return s[1] ? s + 1 : path;
}

bool path_normalize(const char *in, const char *cwd, char *out, size_t cap) {
    char full[EX_PATH * 2];
    const char *home = getenv("HOME");
    if (in[0] == '~' && (in[1] == 0 || in[1] == '/') && home) snprintf(full, sizeof full, "%s%s", home, in + 1);
    else if (in[0] == '/') snprintf(full, sizeof full, "%s", in);
    else snprintf(full, sizeof full, "%s/%s", cwd && *cwd ? cwd : "/", in);
    char *parts[160];
    int np = 0;
    char *p = full;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char *s = p;
        while (*p && *p != '/') p++;
        if (*p) *p++ = 0;
        if (!strcmp(s, ".")) continue;
        if (!strcmp(s, "..")) { if (np) np--; continue; }
        if (np >= 160) return false;
        parts[np++] = s;
    }
    if (np == 0) {
        if (cap < 2) return false;
        strcpy(out, "/");
        return true;
    }
    size_t o = 0;
    for (int i = 0; i < np; i++) {
        size_t l = strlen(parts[i]);
        if (o + 1 + l + 1 > cap) return false;
        out[o++] = '/';
        memcpy(out + o, parts[i], l);
        o += l;
    }
    out[o] = 0;
    return true;
}

bool path_inside(const char *path, const char *dir) {
    size_t dl = strlen(dir);
    if (dl == 1 && dir[0] == '/') return path[0] == '/';
    if (strncmp(path, dir, dl)) return false;
    return path[dl] == 0 || path[dl] == '/';
}

bool path_exists(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0;
}

const char *ext_of(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) return "";
    return dot + 1;
}

void errtext(int err, char *out, size_t cap) { snprintf(out, cap, "%s", strerror(err)); }

/* ---------------------------------------------------------------- formatting */

void fmt_size(char *out, size_t cap, int64_t n) {
    if (n < 0) n = 0;
    long long v = (long long)n;
    if (v < 1024) snprintf(out, cap, "%lld byte%s", v, v == 1 ? "" : "s");
    else if (v < 1024 * 1024) snprintf(out, cap, "%lld.%lld KB", v / 1024, v * 10 / 1024 % 10);
    else if (v < 1024LL * 1024 * 1024) snprintf(out, cap, "%lld.%lld MB", v >> 20, (v * 10 >> 20) % 10);
    else snprintf(out, cap, "%lld.%lld GB", v >> 30, (v * 10 >> 30) % 10);
}

void fmt_time(char *out, size_t cap, int64_t t) {
    if (t <= 0) { snprintf(out, cap, "-"); return; }
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    snprintf(out, cap, "%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

void fmt_time_long(char *out, size_t cap, int64_t t) {
    static const char *const days[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *const months[12] = { "January", "February", "March", "April", "May", "June", "July", "August", "September",
                                            "October", "November", "December" };
    if (t <= 0) { snprintf(out, cap, "unknown"); return; }
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    snprintf(out, cap, "%s, %d %s %d, %02d:%02d:%02d", days[tm.tm_wday % 7], tm.tm_mday, months[tm.tm_mon % 12], tm.tm_year + 1900,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

void fmt_mode(char *out, uint32_t mode) {
    char t = '-';
    switch (mode & S_IFMT) {
    case S_IFDIR: t = 'd'; break;
    case S_IFLNK: t = 'l'; break;
    case S_IFCHR: t = 'c'; break;
    case S_IFBLK: t = 'b'; break;
    case S_IFIFO: t = 'p'; break;
    case S_IFSOCK: t = 's'; break;
    default: break;
    }
    out[0] = t;
    out[1] = mode & S_IRUSR ? 'r' : '-';
    out[2] = mode & S_IWUSR ? 'w' : '-';
    out[3] = mode & S_ISUID ? (mode & S_IXUSR ? 's' : 'S') : (mode & S_IXUSR ? 'x' : '-');
    out[4] = mode & S_IRGRP ? 'r' : '-';
    out[5] = mode & S_IWGRP ? 'w' : '-';
    out[6] = mode & S_ISGID ? (mode & S_IXGRP ? 's' : 'S') : (mode & S_IXGRP ? 'x' : '-');
    out[7] = mode & S_IROTH ? 'r' : '-';
    out[8] = mode & S_IWOTH ? 'w' : '-';
    out[9] = mode & S_ISVTX ? (mode & S_IXOTH ? 't' : 'T') : (mode & S_IXOTH ? 'x' : '-');
    out[10] = 0;
}

typedef struct { uint32_t id; char name[32]; } NameCache;
static NameCache g_uc[64], g_gc[64];
static int g_nuc, g_ngc;

const char *uid_name(uint32_t uid) {
    for (int i = 0; i < g_nuc; i++) if (g_uc[i].id == uid) return g_uc[i].name;
    char name[32];
    struct passwd *pw = getpwuid(uid);
    if (pw && pw->pw_name) strlcpy(name, pw->pw_name, sizeof name);
    else snprintf(name, sizeof name, "%u", uid);
    if (g_nuc < 64) {
        g_uc[g_nuc].id = uid;
        strlcpy(g_uc[g_nuc].name, name, sizeof g_uc[g_nuc].name);
        return g_uc[g_nuc++].name;
    }
    static char tmp[32];
    strlcpy(tmp, name, sizeof tmp);
    return tmp;
}

const char *gid_name(uint32_t gid) {
    for (int i = 0; i < g_ngc; i++) if (g_gc[i].id == gid) return g_gc[i].name;
    char name[32];
    struct group *gr = getgrgid(gid);
    if (gr && gr->gr_name) strlcpy(name, gr->gr_name, sizeof name);
    else snprintf(name, sizeof name, "%u", gid);
    if (g_ngc < 64) {
        g_gc[g_ngc].id = gid;
        strlcpy(g_gc[g_ngc].name, name, sizeof g_gc[g_ngc].name);
        return g_gc[g_ngc++].name;
    }
    static char tmp[32];
    strlcpy(tmp, name, sizeof tmp);
    return tmp;
}

/* ---------------------------------------------------------------- names */

void fold_name(const char *in, char *out, size_t cap) {
    size_t i = 0;
    for (; in[i] && i + 1 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        out[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
    }
    out[i] = 0;
}

/* Natural order on folded names: runs of digits compare as numbers, so file2 comes before file10. */
int natcmp(const char *a, const char *b) {
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            const char *za = a, *zb = b;
            while (*za == '0') za++;
            while (*zb == '0') zb++;
            const char *ea = za, *eb = zb;
            while (isdigit((unsigned char)*ea)) ea++;
            while (isdigit((unsigned char)*eb)) eb++;
            long la = (long)(ea - za), lb = (long)(eb - zb);
            if (la != lb) return la < lb ? -1 : 1;
            int c = memcmp(za, zb, (size_t)la);
            if (c) return c < 0 ? -1 : 1;
            long pa = (long)(ea - a), pb = (long)(eb - b);              /* same value: fewer leading zeros first */
            if (pa != pb) return pa < pb ? -1 : 1;
            a = ea;
            b = eb;
            continue;
        }
        if (*a != *b) return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
        a++;
        b++;
    }
    return *a ? 1 : *b ? -1 : 0;
}

bool name_valid(const char *name, const char **why) {
    static const char *w;
    if (!name[0]) w = "A name cannot be empty.";
    else if (!strcmp(name, ".") || !strcmp(name, "..")) w = "\".\" and \"..\" are not names a file can have.";
    else if (strchr(name, '/')) w = "A name cannot contain a slash (/).";
    else if (strlen(name) > 255) w = "That name is longer than 255 bytes.";
    else {
        for (const unsigned char *p = (const unsigned char *)name; *p; p++)
            if (*p < 32 || *p == 127) { w = "A name cannot contain control characters."; goto bad; }
        return true;
    }
bad:
    if (why) *why = w;
    return false;
}

/* A name that does not exist in `dir` yet. style: "new" -> "base", "base (2)", ...; "copy" -> "stem (copy).ext", "stem (copy 2).ext"; "num" -> "stem (2).ext". */
void unique_name(const char *dir, const char *base, char *out, size_t cap, const char *style) {
    char path[EX_PATH * 2];
    char stem[EX_NAME], ext[EX_NAME];
    const char *dot = strrchr(base, '.');
    bool folder_like = !strcmp(style, "new");
    if (dot && dot != base && !folder_like) {
        snprintf(stem, sizeof stem, "%.*s", (int)(dot - base), base);
        snprintf(ext, sizeof ext, "%s", dot);
    } else {
        snprintf(stem, sizeof stem, "%s", base);
        ext[0] = 0;
    }
    if (folder_like) {
        path_join(path, sizeof path, dir, base);
        if (!path_exists(path)) { strlcpy(out, base, cap); return; }
    }
    for (int i = 1; i < 100000; i++) {
        char cand[EX_NAME + 32];
        if (!strcmp(style, "copy")) {
            if (i == 1) snprintf(cand, sizeof cand, "%s (copy)%s", stem, ext);
            else snprintf(cand, sizeof cand, "%s (copy %d)%s", stem, i, ext);
        } else snprintf(cand, sizeof cand, "%s (%d)%s", stem, i + 1, ext);
        path_join(path, sizeof path, dir, cand);
        if (!path_exists(path)) { strlcpy(out, cand, cap); return; }
    }
    strlcpy(out, base, cap);
}

void sh_quote(char *out, size_t cap, const char *s) {
    size_t o = 0;
    if (cap < 3) { if (cap) out[0] = 0; return; }
    out[o++] = '\'';
    for (; *s && o + 6 < cap; s++) {
        if (*s == '\'') { memcpy(out + o, "'\\''", 4); o += 4; }
        else out[o++] = *s;
    }
    out[o++] = '\'';
    out[o] = 0;
}

/* ---------------------------------------------------------------- file types */

static bool ext_in(const char *ext, const char *const *list) {
    for (; *list; list++) if (!strcasecmp(ext, *list)) return true;
    return false;
}

static const char *const k_text_ext[] = {
    "txt", "md", "markdown", "rst", "log", "cfg", "conf", "ini", "json", "xml", "yml", "yaml", "toml", "csv", "tsv", "html", "htm", "css",
    "js", "c", "h", "cc", "cpp", "hpp", "py", "sh", "bash", "gap", "lua", "rb", "pl", "tex", "diff", "patch", "asm", "s", "mk", "inc",
    "ld", "pw", "rc", "list", "text", "me", "nfo", "desktop", "service", NULL,
};
static const char *const k_image_ext[] = { "ppm", "bmp", "pgm", "pbm", "pnm", "png", "jpg", "jpeg", "jpe", "gif", "tga", NULL };    /* what libimg reads (the Gallery opens them) */
static const char *const k_archive_ext[] = { "tar", "gz", "tgz", "zip", "bz2", "xz", "7z", "rar", "z", "zst", NULL };
static const char *const k_text_names[] = {
    "makefile", "readme", "license", "licence", "copying", "authors", "news", "todo", "changelog", "profile", "motd", "passwd", "group",
    "hosts", "hostname", "fstab", "shells", "sudoers", "timezone", "issue", NULL,
};

int classify(const char *name, uint32_t mode, int kind) {
    if (kind == EK_OTHER) {
        if (S_ISCHR(mode) || S_ISBLK(mode)) return FT_DEVICE;
        if (S_ISFIFO(mode)) return FT_PIPE;
        if ((mode & S_IFMT) == S_IFSOCK) return FT_SOCKET;
        return FT_GENERIC;
    }
    if (kind == EK_DIR) return FT_GENERIC;
    const char *ext = ext_of(name);
    bool exec = kind == EK_FILE && (mode & 0111) != 0;
    if (exec && !strcasecmp(ext, "sh")) return FT_PROGRAM;
    if (*ext) {
        if (ext_in(ext, k_image_ext)) return FT_IMAGE;
        if (ext_in(ext, k_archive_ext)) return FT_ARCHIVE;
        if (ext_in(ext, k_text_ext)) return FT_TEXT;
    }
    if (exec) return FT_PROGRAM;
    if (!*ext) {
        char low[EX_NAME];
        fold_name(name, low, sizeof low);
        if (ext_in(low, k_text_names)) return FT_TEXT;
        if (name[0] == '.' && name[1]) return FT_TEXT;                  /* .gaprc, .profile: configuration */
    }
    return FT_GENERIC;
}

const char *type_name(const Entry *e, char *buf, size_t cap) {
    if (e->kind == EK_LINK) return e->broken ? "Broken link" : e->is_dir ? "Link to folder" : "Link";
    if (e->kind == EK_DIR) return "Folder";
    const char *ext = ext_of(e->name);
    switch (e->ftype) {
    case FT_TEXT:
        if (!strcasecmp(ext, "c")) return "C source";
        if (!strcasecmp(ext, "h")) return "C header";
        if (!strcasecmp(ext, "sh") || !strcasecmp(ext, "bash") || !strcasecmp(ext, "gap")) return "Shell script";
        if (!strcasecmp(ext, "md") || !strcasecmp(ext, "markdown")) return "Markdown document";
        if (!strcasecmp(ext, "pw")) return "Manual page";
        return "Text document";
    case FT_IMAGE:
        if (!strcasecmp(ext, "ppm")) return "PPM image";
        if (!strcasecmp(ext, "bmp")) return "Bitmap image";
        if (!strcasecmp(ext, "png")) return "PNG image";
        if (!strcasecmp(ext, "jpg") || !strcasecmp(ext, "jpeg") || !strcasecmp(ext, "jpe")) return "JPEG image";
        if (!strcasecmp(ext, "gif")) return "GIF image";
        if (!strcasecmp(ext, "tga")) return "TGA image";
        return "Image";
    case FT_PROGRAM: return "Program";
    case FT_ARCHIVE: return "Archive";
    case FT_DEVICE: return "Device";
    case FT_PIPE: return "Named pipe";
    case FT_SOCKET: return "Socket";
    default: break;
    }
    if (*ext && strlen(ext) <= 8) {
        size_t i = 0;
        for (; ext[i] && i + 6 < cap; i++) buf[i] = (char)toupper((unsigned char)ext[i]);
        snprintf(buf + i, cap - i, " file");
        return buf;
    }
    return "File";
}

KIcon icon_for(const Entry *e) {
    if (e->is_dir) return KI_FOLDER;
    switch (e->ftype) {
    case FT_TEXT: return KI_TEXT;
    case FT_IMAGE: return KI_IMAGE;
    case FT_PROGRAM: return KI_TERMINAL;
    case FT_DEVICE: case FT_PIPE: case FT_SOCKET: return KI_GEAR;
    default: return KI_FILE;
    }
}

/* The kit's icons cover folders, text, images and terminals; links, archives and the rest are dressed up here. */
void draw_entry_icon(KCanvas *cv, const Entry *e, int x, int y, int size, uint32_t fade_bg, int fade_alpha) {
    k_icon(cv, icon_for(e), x, y, size);
    if (e->ftype == FT_ARCHIVE && !e->is_dir) {
        int u = size;
        kc_fill(cv, kr(x + u * 10 / 32, y + u * 12 / 32, u * 12 / 32, u * 11 / 32), 0xC99A2E);
        kc_outline(cv, kr(x + u * 10 / 32, y + u * 12 / 32, u * 12 / 32, u * 11 / 32), 0x6B4E12);
        kc_fill(cv, kr(x + u * 15 / 32, y + u * 12 / 32, u * 2 / 32 + 1, u * 11 / 32), 0xF0C65A);
    }
    if (e->kind == EK_LINK) {
        int b = size * 12 / 32;
        if (b < 7) b = 7;
        int bx = x, by = y + size - b;
        uint32_t ink = e->broken ? 0xB5122B : 0x2A5CAA;
        kc_fill(cv, kr(bx, by, b, b), 0xFFFFFF);
        kc_outline(cv, kr(bx, by, b, b), ink);
        kc_line(cv, bx + 2, by + b - 3, bx + b - 3, by + 2, ink);
        kc_hline(cv, bx + b - 5, by + 2, 4, ink);
        kc_vline(cv, bx + b - 3, by + 2, 4, ink);
    }
    if (fade_alpha > 0) kc_fill_alpha(cv, kr(x, y, size, size), fade_bg, fade_alpha);
}
