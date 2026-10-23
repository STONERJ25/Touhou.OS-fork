/* Interactive line editing: raw-mode editor with history, kill ring, reverse search and tab completion. */
#include "gapsh.h"
#include <sys/ioctl.h>

#define MAX_LINE 4096
#define MAX_HIST 1000

extern char **environ;
const char *builtin_name(int i);
const char *alias_lookup(const char *name);

/* ------------------------------------------------------------------ history */

static char *g_hist[MAX_HIST];
static int g_nhist;

int history_count(void) { return g_nhist; }
const char *history_get(int i) { return i >= 0 && i < g_nhist ? g_hist[i] : NULL; }

void history_add(const char *line) {
    if (!line || !*line) return;
    if (g_nhist && !strcmp(g_hist[g_nhist - 1], line)) return;
    if (g_nhist == MAX_HIST) {
        free(g_hist[0]);
        memmove(g_hist, g_hist + 1, (MAX_HIST - 1) * sizeof(char *));
        g_nhist--;
    }
    g_hist[g_nhist++] = xstrdup(line);
}

void history_print(void) {
    for (int i = 0; i < g_nhist; i++) printf("%5d  %s\n", i + 1, g_hist[i]);
}

static char *history_path(void) {
    const char *home = var_get("HOME");
    return home ? xasprintf("%s/.gapsh_history", home) : NULL;
}

void history_load(void) {
    char *path = history_path();
    if (!path) return;
    FILE *f = fopen(path, "r");
    free(path);
    if (!f) return;
    char line[MAX_LINE];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        history_add(line);
    }
    fclose(f);
}

void history_save(void) {
    char *path = history_path();
    if (!path) return;
    FILE *f = fopen(path, "w");
    free(path);
    if (!f) return;
    int start = g_nhist > 500 ? g_nhist - 500 : 0;
    for (int i = start; i < g_nhist; i++) fprintf(f, "%s\n", g_hist[i]);
    fclose(f);
}

/* ------------------------------------------------------------------ terminal */

static struct termios g_orig;
static int g_raw;

static void raw_on(void) {
    if (g_raw) return;
    tcgetattr(0, &g_orig);
    struct termios t = g_orig;
    t.c_lflag &= ~(ICANON | ECHO | ISIG);
    t.c_iflag &= ~ICRNL;
    t.c_cc[VMIN] = 1;
    tcsetattr(0, TCSANOW, &t);
    g_raw = 1;
}

static void raw_off(void) {
    if (!g_raw) return;
    tcsetattr(0, TCSANOW, &g_orig);
    g_raw = 0;
}

static int term_cols(void) {
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col) return ws.ws_col;
    return 80;
}

static int read_byte(void) {
    unsigned char c;
    for (;;) {
        ssize_t r = read(0, &c, 1);
        if (r == 1) return c;
        if (r == 0) return -1;
        if (errno != EINTR) return -1;
    }
}

static int visible_len(const char *s) {
    int n = 0;
    for (; *s; s++) {
        if (*s == 27 && s[1] == '[') {
            s += 2;
            while (*s && !isalpha((unsigned char)*s)) s++;
            if (!*s) break;
            continue;
        }
        if (((unsigned char)*s & 0xC0) != 0x80) n++;
    }
    return n;
}

/* ------------------------------------------------------------------ completion */

static StrVec g_cmd_cache;
static int g_cmd_cached;

static int cmp_ptr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void load_commands(void) {
    if (g_cmd_cached) return;
    g_cmd_cached = 1;
    for (int i = 0; builtin_name(i); i++) sv_push(&g_cmd_cache, xstrdup(builtin_name(i)));
    const char *path = var_get("PATH");
    if (!path) path = "/bin";
    while (*path) {
        const char *end = strchr(path, ':');
        size_t len = end ? (size_t)(end - path) : strlen(path);
        char *dir = xasprintf("%.*s", (int)len, path);
        DIR *d = opendir(dir);
        if (d) {
            for (struct dirent *e; (e = readdir(d));)
                if (e->d_name[0] != '.') sv_push(&g_cmd_cache, xstrdup(e->d_name));
            closedir(d);
        }
        free(dir);
        if (!end) break;
        path = end + 1;
    }
    if (g_cmd_cache.n) qsort(g_cmd_cache.v, (size_t)g_cmd_cache.n, sizeof(char *), cmp_ptr);
}

static void add_unique(StrVec *sv, const char *s) {
    for (int i = 0; i < sv->n; i++) if (!strcmp(sv->v[i], s)) return;
    sv_push(sv, xstrdup(s));
}

static void collect(StrVec *out, const char *word, int command_pos) {
    size_t wl = strlen(word);
    if (command_pos && !strchr(word, '/')) {
        load_commands();
        for (int i = 0; i < g_cmd_cache.n; i++)
            if (!strncmp(g_cmd_cache.v[i], word, wl)) add_unique(out, g_cmd_cache.v[i]);
        return;
    }
    char dirpart[PATH_MAX] = "";
    const char *base = word;
    const char *slash = strrchr(word, '/');
    if (slash) { snprintf(dirpart, sizeof dirpart, "%.*s", (int)(slash - word) + 1, word); base = slash + 1; }
    char open_path[PATH_MAX];
    if (dirpart[0] == '~') {
        const char *home = var_get("HOME");
        snprintf(open_path, sizeof open_path, "%s%s", home ? home : "", dirpart + 1);
    } else {
        snprintf(open_path, sizeof open_path, "%s", dirpart[0] ? dirpart : ".");
    }
    DIR *d = opendir(open_path);
    if (!d) return;
    size_t bl = strlen(base);
    for (struct dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.' && base[0] != '.') continue;
        if (strncmp(e->d_name, base, bl)) continue;
        char full[PATH_MAX];
        snprintf(full, sizeof full, "%s%s", dirpart, e->d_name);
        struct stat st;
        char chk[PATH_MAX + 8];
        snprintf(chk, sizeof chk, "%s/%s", open_path, e->d_name);
        if (stat(chk, &st) == 0 && S_ISDIR(st.st_mode)) strlcat(full, "/", sizeof full);
        add_unique(out, full);
    }
    closedir(d);
}

static void insert_char(char *buf, size_t *len, size_t *pos, char c) {
    if (*len + 1 >= MAX_LINE) return;
    memmove(buf + *pos + 1, buf + *pos, *len - *pos + 1);
    buf[(*pos)++] = c;
    (*len)++;
}

static void insert_escaped(char *buf, size_t *len, size_t *pos, const char *text) {
    for (; *text; text++) {
        if (strchr(" \t'\"\\$&;|<>()*?[]#`", *text)) insert_char(buf, len, pos, '\\');
        insert_char(buf, len, pos, *text);
    }
}

/* Returns 1 if something changed. 'second' is true on a repeated Tab. */
static int do_complete(char *buf, size_t *len, size_t *pos, int second, const char *prompt, int *need_redraw) {
    size_t start = 0;                                       /* start of the word under the cursor, honouring "\ " escapes */
    for (size_t i = 0; i < *pos; i++) {
        if (buf[i] == '\\' && i + 1 < *pos) { i++; continue; }
        if (strchr(" \t|;&<>()", buf[i])) start = i + 1;
    }
    char word[PATH_MAX];
    size_t wn = 0;
    for (size_t i = start; i < *pos && wn < sizeof word - 1; i++) {
        if (buf[i] == '\\' && i + 1 < *pos) i++;
        word[wn++] = buf[i];
    }
    word[wn] = 0;
    int command_pos = 1;
    for (size_t i = start; i > 0; i--) {
        char c = buf[i - 1];
        if (c == ' ' || c == '\t') continue;
        command_pos = c == '|' || c == ';' || c == '&' || c == '(';
        break;
    }
    StrVec cands = { 0 };
    collect(&cands, word, command_pos);
    if (!cands.n) { write(1, "\a", 1); return 0; }
    qsort(cands.v, (size_t)cands.n, sizeof(char *), cmp_ptr);

    if (cands.n == 1) {
        size_t wl = *pos - start;
        memmove(buf + start, buf + *pos, *len - *pos + 1);
        *len -= wl;
        *pos = start;
        insert_escaped(buf, len, pos, cands.v[0]);
        size_t cl = strlen(cands.v[0]);
        if (cands.v[0][cl - 1] != '/' && *len + 1 < MAX_LINE) {
            memmove(buf + *pos + 1, buf + *pos, *len - *pos + 1);
            buf[(*pos)++] = ' ';
            (*len)++;
        }
        return 1;
    }
    size_t common = strlen(cands.v[0]);
    for (int i = 1; i < cands.n; i++) {
        size_t k = 0;
        while (k < common && cands.v[i][k] == cands.v[0][k]) k++;
        common = k;
    }
    if (common > strlen(word)) {
        size_t wl = *pos - start;
        memmove(buf + start, buf + *pos, *len - *pos + 1);
        *len -= wl;
        *pos = start;
        char *pre = xasprintf("%.*s", (int)common, cands.v[0]);
        insert_escaped(buf, len, pos, pre);
        free(pre);
        return 1;
    }
    if (second) {
        int cols = term_cols();
        size_t maxw = 0;
        for (int i = 0; i < cands.n; i++) if (strlen(cands.v[i]) > maxw) maxw = strlen(cands.v[i]);
        int per = cols / (int)(maxw + 2);
        if (per < 1) per = 1;
        printf("\r\n");
        for (int i = 0; i < cands.n; i++) {
            printf("%-*s", (int)maxw + 2, cands.v[i]);
            if ((i + 1) % per == 0 || i + 1 == cands.n) printf("\r\n");
        }
        fflush(stdout);
        *need_redraw = 1;
        (void)prompt;
    } else {
        write(1, "\a", 1);
    }
    return 0;
}

/* ------------------------------------------------------------------ editing */

static void refresh(const char *prompt, int plen, const char *buf, size_t len, size_t pos) {
    int cols = term_cols();
    int avail = cols - plen - 1;
    if (avail < 8) avail = 8;
    size_t start = 0;
    if ((int)pos >= avail) start = pos - (size_t)avail + 1;
    size_t shown = len - start;
    if ((int)shown > avail) shown = (size_t)avail;
    char out[MAX_LINE + 256];
    int n = snprintf(out, sizeof out, "\r%s", prompt);
    memcpy(out + n, buf + start, shown);
    n += (int)shown;
    n += snprintf(out + n, sizeof out - (size_t)n, "\x1b[K");
    int col = plen + (int)(pos - start);
    n += snprintf(out + n, sizeof out - (size_t)n, "\r");
    if (col > 0) n += snprintf(out + n, sizeof out - (size_t)n, "\x1b[%dC", col);
    write(1, out, (size_t)n);
}

static char g_kill[MAX_LINE];

char *edit_readline(const char *prompt) {
    if (!isatty(0)) {
        char *line = NULL;
        size_t cap = 0;
        fflush(stdout);
        ssize_t n = getline(&line, &cap, stdin);
        if (n < 0) { free(line); return NULL; }
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        return line;
    }
    fflush(NULL);
    char buf[MAX_LINE];
    size_t len = 0, pos = 0;
    buf[0] = 0;
    int plen = visible_len(prompt);
    int hist_idx = g_nhist;
    char *saved_line = NULL;
    int last_was_tab = 0;
    raw_on();
    write(1, "\r", 1);
    write(1, prompt, strlen(prompt));

    for (;;) {
        int c = read_byte();
        if (c < 0) { raw_off(); free(saved_line); return NULL; }
        int tab = 0;
        if (c == '\r' || c == '\n') {
            refresh(prompt, plen, buf, len, len);
            write(1, "\r\n", 2);
            raw_off();
            free(saved_line);
            return xstrdup(buf);
        } else if (c == 3) {                                   /* Ctrl-C: abandon the line */
            write(1, "^C\r\n", 4);
            raw_off();
            free(saved_line);
            return xstrdup("");
        } else if (c == 4) {                                   /* Ctrl-D */
            if (len == 0) { write(1, "\r\n", 2); raw_off(); free(saved_line); return NULL; }
            if (pos < len) { memmove(buf + pos, buf + pos + 1, len - pos); len--; }
        } else if (c == 127 || c == 8) {                       /* backspace */
            if (pos > 0) { memmove(buf + pos - 1, buf + pos, len - pos + 1); pos--; len--; }
        } else if (c == 1) pos = 0;                            /* Ctrl-A */
        else if (c == 5) pos = len;                            /* Ctrl-E */
        else if (c == 2) { if (pos) pos--; }                   /* Ctrl-B */
        else if (c == 6) { if (pos < len) pos++; }             /* Ctrl-F */
        else if (c == 11) {                                    /* Ctrl-K */
            strlcpy(g_kill, buf + pos, sizeof g_kill);
            buf[pos] = 0;
            len = pos;
        } else if (c == 21) {                                  /* Ctrl-U */
            strlcpy(g_kill, buf, pos + 1 < sizeof g_kill ? pos + 1 : sizeof g_kill);
            memmove(buf, buf + pos, len - pos + 1);
            len -= pos;
            pos = 0;
        } else if (c == 23) {                                  /* Ctrl-W */
            size_t p = pos;
            while (p > 0 && buf[p - 1] == ' ') p--;
            while (p > 0 && buf[p - 1] != ' ') p--;
            strlcpy(g_kill, buf + p, pos - p + 1 < sizeof g_kill ? pos - p + 1 : sizeof g_kill);
            memmove(buf + p, buf + pos, len - pos + 1);
            len -= pos - p;
            pos = p;
        } else if (c == 25) {                                  /* Ctrl-Y */
            size_t kl = strlen(g_kill);
            if (len + kl < MAX_LINE - 1) {
                memmove(buf + pos + kl, buf + pos, len - pos + 1);
                memcpy(buf + pos, g_kill, kl);
                pos += kl;
                len += kl;
            }
        } else if (c == 20) {                                  /* Ctrl-T: transpose */
            if (pos > 0 && len > 1) {
                if (pos == len) pos--;
                char t = buf[pos - 1]; buf[pos - 1] = buf[pos]; buf[pos] = t;
                pos++;
            }
        } else if (c == 12) {                                  /* Ctrl-L */
            write(1, "\x1b[2J\x1b[H", 7);
        } else if (c == 18) {                                  /* Ctrl-R: incremental reverse search */
            char term[128] = "";
            size_t tl = 0;
            int found = -1;
            for (;;) {
                char status[300];
                snprintf(status, sizeof status, "\r\x1b[K(reverse-i-search)`%s': %s", term, found >= 0 ? g_hist[found] : "");
                write(1, status, strlen(status));
                int k = read_byte();
                int from = g_nhist - 1;
                if (k < 0 || k == 7 || k == 27 || k == 3) { found = -1; break; }
                if (k == '\r' || k == '\n') break;
                if (k == 127 || k == 8) { if (tl) term[--tl] = 0; }
                else if (k == 18) { if (found > 0) from = found - 1; }
                else if (k >= 32 && tl < sizeof term - 1) { term[tl++] = (char)k; term[tl] = 0; }
                found = -1;
                if (tl) for (int i = from; i >= 0; i--) if (strstr(g_hist[i], term)) { found = i; break; }
            }
            if (found >= 0) { strlcpy(buf, g_hist[found], sizeof buf); len = pos = strlen(buf); }
        } else if (c == 9) {                                   /* Tab */
            int redraw = 0;
            do_complete(buf, &len, &pos, last_was_tab, prompt, &redraw);
            tab = 1;
            if (redraw) { write(1, "\r", 1); write(1, prompt, strlen(prompt)); }
        } else if (c == 27) {                                  /* escape sequences */
            int c1 = read_byte();
            if (c1 == '[' || c1 == 'O') {
                int params[4] = { 0 }, np = 0, fin;
                for (;;) {
                    fin = read_byte();
                    if (fin >= '0' && fin <= '9') { params[np] = params[np] * 10 + (fin - '0'); continue; }
                    if (fin == ';') { if (np < 3) np++; continue; }
                    break;
                }
                switch (fin) {
                case 'A':
                    if (hist_idx > 0) {
                        if (hist_idx == g_nhist) { free(saved_line); saved_line = xstrdup(buf); }
                        hist_idx--;
                        strlcpy(buf, g_hist[hist_idx], sizeof buf);
                        len = pos = strlen(buf);
                    }
                    break;
                case 'B':
                    if (hist_idx < g_nhist) {
                        hist_idx++;
                        strlcpy(buf, hist_idx == g_nhist ? (saved_line ? saved_line : "") : g_hist[hist_idx], sizeof buf);
                        len = pos = strlen(buf);
                    }
                    break;
                case 'C': if (pos < len) pos++; break;
                case 'D': if (pos > 0) pos--; break;
                case 'H': pos = 0; break;
                case 'F': pos = len; break;
                case '~':
                    if (params[0] == 3 && pos < len) { memmove(buf + pos, buf + pos + 1, len - pos); len--; }
                    else if (params[0] == 1 || params[0] == 7) pos = 0;
                    else if (params[0] == 4 || params[0] == 8) pos = len;
                    break;
                default: break;
                }
            } else if (c1 == 'b') {
                while (pos > 0 && buf[pos - 1] == ' ') pos--;
                while (pos > 0 && buf[pos - 1] != ' ') pos--;
            } else if (c1 == 'f') {
                while (pos < len && buf[pos] == ' ') pos++;
                while (pos < len && buf[pos] != ' ') pos++;
            } else if (c1 == 'd') {
                size_t e = pos;
                while (e < len && buf[e] == ' ') e++;
                while (e < len && buf[e] != ' ') e++;
                memmove(buf + pos, buf + e, len - e + 1);
                len -= e - pos;
            }
        } else if (c >= 32 && len < MAX_LINE - 2) {            /* printable (and UTF-8 continuation bytes) */
            memmove(buf + pos + 1, buf + pos, len - pos + 1);
            buf[pos++] = (char)c;
            len++;
        }
        last_was_tab = tab;
        refresh(prompt, plen, buf, len, pos);
    }
}
