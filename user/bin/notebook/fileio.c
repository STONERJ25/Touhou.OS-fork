/* Files: reading and writing documents, line ending conversion, the recent files list (~/.config/notebook.recent) and the
 * preferences (~/.config/notebook.conf). */
#include "notebook.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

const char *nb_eol_name(EolStyle e) {
    switch (e) {
    case EOL_CRLF: return "CRLF";
    case EOL_CR: return "CR";
    default: return "LF";
    }
}

/* ---------------------------------------------------------------- reading */

int nb_read_all(const char *path, char **data, size_t *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return errno ? errno : EIO;
    size_t cap = 65536;
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size > 0) {
        if ((uint64_t)st.st_size > NB_MAX_FILE) { close(fd); return EFBIG; }
        cap = (size_t)st.st_size + 2;
    }
    char *buf = malloc(cap + 1);
    if (!buf) { close(fd); return ENOMEM; }
    size_t n = 0;
    for (;;) {
        if (n + 1 >= cap) {
            if (cap > NB_MAX_FILE) { free(buf); close(fd); return EFBIG; }
            size_t nc = cap * 2;
            char *nb = realloc(buf, nc + 1);
            if (!nb) { free(buf); close(fd); return ENOMEM; }
            buf = nb;
            cap = nc;
        }
        ssize_t r = read(fd, buf + n, cap - n - 1);
        if (r < 0) {
            if (errno == EINTR) continue;
            int e = errno ? errno : EIO;
            free(buf);
            close(fd);
            return e;
        }
        if (r == 0) break;
        n += (size_t)r;
    }
    close(fd);
    buf[n] = 0;
    *data = buf;
    *len = n;
    return 0;
}

long nb_find_nul(const char *data, size_t len) {
    const char *p = memchr(data, 0, len);
    return p ? (long)(p - data) : -1;
}

size_t nb_normalize(char *data, size_t len, FileInfo *info) {
    memset(info, 0, sizeof *info);
    size_t off = 0;
    if (len >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF) {
        info->bom = true;
        off = 3;
    }
    char *t = data + off;
    size_t n = len - off;
    size_t crlf = 0, lf = 0, cr = 0;
    for (size_t i = 0; i < n; i++) {
        char c = t[i];
        if (c == '\n') { if (i > 0 && t[i - 1] == '\r') crlf++; else lf++; }
        else if (c == '\r' && (i + 1 >= n || t[i + 1] != '\n')) cr++;
    }
    info->crlf = crlf; info->lf = lf; info->cr = cr;
    if (cr > 0 && crlf == 0 && lf == 0) info->eol = EOL_CR;
    else info->eol = crlf > lf ? EOL_CRLF : EOL_LF;
    info->mixed = (info->eol == EOL_LF && crlf > 0) || (info->eol == EOL_CRLF && lf > 0);
    size_t w = 0;
    if (info->eol == EOL_CR) {
        for (size_t i = 0; i < n; i++) t[w++] = t[i] == '\r' ? '\n' : t[i];
    } else if (crlf > 0) {
        for (size_t i = 0; i < n; i++) {
            if (t[i] == '\r' && i + 1 < n && t[i + 1] == '\n') continue;
            t[w++] = t[i];
        }
    } else w = n;
    if (off) nb_memmove(data, t, w);
    info->utf8_ok = nb_u8_valid(data, w);
    return w;
}

/* ---------------------------------------------------------------- writing */

static int write_full(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n > (1u << 20) ? (1u << 20) : n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return errno ? errno : EIO;
        }
        if (w == 0) return EIO;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int nb_write_doc(const char *path, Doc *d, EolStyle eol, bool bom) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return errno ? errno : EIO;
    int rc = 0;
    if (bom) rc = write_full(fd, "\xEF\xBB\xBF", 3);
    const char *t = doc_all(d);
    size_t len = doc_len(d);
    if (!rc) {
        if (eol == EOL_LF) rc = write_full(fd, t, len);
        else {
            size_t cap = 65536, o = 0;
            char *out = malloc(cap);
            if (!out) rc = ENOMEM;
            else {
                for (size_t i = 0; i < len && !rc; i++) {
                    if (o + 2 > cap) { rc = write_full(fd, out, o); o = 0; }
                    if (t[i] == '\n') { if (eol == EOL_CRLF) out[o++] = '\r'; out[o++] = eol == EOL_CRLF ? '\n' : '\r'; }
                    else out[o++] = t[i];
                }
                if (!rc && o) rc = write_full(fd, out, o);
                free(out);
            }
        }
    }
    if (close(fd) < 0 && !rc) rc = errno ? errno : EIO;
    if (!rc) sync();
    return rc;
}

/* ---------------------------------------------------------------- paths */

void nb_abspath(const char *in, char *out, size_t cap) {
    char tmp[NB_PATH_MAX * 2];
    if (in[0] == '/') snprintf(tmp, sizeof tmp, "%s", in);
    else {
        char cwd[NB_PATH_MAX];
        if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "/");
        snprintf(tmp, sizeof tmp, "%s/%s", cwd, in);
    }
    char *segs[256];
    int ns = 0;
    char *save = NULL;
    for (char *s = strtok_r(tmp, "/", &save); s; s = strtok_r(NULL, "/", &save)) {
        if (!strcmp(s, ".") || !*s) continue;
        if (!strcmp(s, "..")) { if (ns > 0) ns--; continue; }
        if (ns < 256) segs[ns++] = s;
    }
    size_t o = 0;
    if (cap == 0) return;
    if (ns == 0) { snprintf(out, cap, "/"); return; }
    for (int i = 0; i < ns; i++) {
        size_t l = strlen(segs[i]);
        if (o + l + 2 > cap) break;
        out[o++] = '/';
        memcpy(out + o, segs[i], l);
        o += l;
    }
    out[o] = 0;
}

static void config_dir(char *out, size_t cap) {
    const char *home = getenv("HOME");
    snprintf(out, cap, "%s/.config", home && *home ? home : "/tmp");
}

/* ---------------------------------------------------------------- recent files */

static void recent_path(char *out, size_t cap) {
    char dir[NB_PATH_MAX];
    config_dir(dir, sizeof dir);
    snprintf(out, cap, "%s/notebook.recent", dir);
}

static void recent_save(void) {
    char dir[NB_PATH_MAX], path[NB_PATH_MAX + 32];
    config_dir(dir, sizeof dir);
    mkdir(dir, 0755);
    recent_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < A.nrecent; i++) fprintf(f, "%s\n", A.recent[i]);
    fclose(f);
}

void recent_load(void) {
    char path[NB_PATH_MAX + 32];
    recent_path(path, sizeof path);
    A.nrecent = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[NB_PATH_MAX + 8];
    while (A.nrecent < NB_RECENT_MAX && fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (n == 0 || line[0] != '/') continue;
        snprintf(A.recent[A.nrecent++], NB_PATH_MAX, "%s", line);
    }
    fclose(f);
}

void recent_remove(const char *path) {
    for (int i = 0; i < A.nrecent; i++) {
        if (strcmp(A.recent[i], path)) continue;
        for (int j = i; j + 1 < A.nrecent; j++) memcpy(A.recent[j], A.recent[j + 1], NB_PATH_MAX);
        A.nrecent--;
        i--;
    }
}

void recent_add(const char *path) {
    if (!path || path[0] != '/' || strchr(path, '\n')) return;
    recent_remove(path);
    if (A.nrecent >= NB_RECENT_MAX) A.nrecent = NB_RECENT_MAX - 1;
    for (int j = A.nrecent; j > 0; j--) memcpy(A.recent[j], A.recent[j - 1], NB_PATH_MAX);
    snprintf(A.recent[0], NB_PATH_MAX, "%s", path);
    A.nrecent++;
    recent_save();
}

void recent_clear(void) {
    A.nrecent = 0;
    recent_save();
}

/* ---------------------------------------------------------------- preferences */

void config_load(void) {
    char dir[NB_PATH_MAX], path[NB_PATH_MAX + 32];
    config_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/notebook.conf", dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        int v = atoi(eq + 1);
        if (!strcmp(line, "line_numbers")) A.ed.numbers = v != 0;
        else if (!strcmp(line, "word_wrap")) A.ed.wrap = v != 0;
        else if (!strcmp(line, "tab_width")) A.ed.tab_w = (v == 2 || v == 4 || v == 8) ? v : 4;
        else if (!strcmp(line, "status_bar")) A.show_status = v != 0;
        else if (!strcmp(line, "highlight")) A.ed.highlight = v != 0;
        else if (!strcmp(line, "indent_spaces")) A.ed.spaces = v != 0;
    }
    fclose(f);
}

void config_save(void) {
    char dir[NB_PATH_MAX], path[NB_PATH_MAX + 32];
    config_dir(dir, sizeof dir);
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/notebook.conf", dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# Patchouli's Notebook preferences\n");
    fprintf(f, "line_numbers=%d\nword_wrap=%d\ntab_width=%d\nstatus_bar=%d\nhighlight=%d\nindent_spaces=%d\n", A.ed.numbers, A.ed.wrap,
            A.ed.tab_w, A.show_status, A.ed.highlight, A.pref_spaces);
    fclose(f);
}
