/* config: the PC-98 application's settings and recent-images files (config.h). */
#include "config.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void config_defaults(Pc98Config *c) {
    memset(c, 0, sizeof *c);
    c->cpu = CFG_CPU_386;
    c->mhz = 16;
    c->fm = CFG_FM_86;
    c->sound = 1;
    c->volume = 80;
    c->scale = 1;
    c->speed = 100;
    c->idle = 1;
    c->vconfig = 1;
}

const char *config_cpu_name(int cpu) {
    switch (cpu) {
    case CFG_CPU_8086: return "8086";
    case CFG_CPU_V30: return "v30";
    case CFG_CPU_286: return "286";
    case CFG_CPU_386: return "386";
    case CFG_CPU_486: return "486";
    }
    return "386";
}

const char *config_fm_name(int fm) { return fm == CFG_FM_86 ? "86" : fm == CFG_FM_26K ? "26k" : "none"; }

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return *a == *b;
}

/* A whole number, digits only (no sign, no blanks inside), at most 9 of them. -1 when it is not one. */
static long number(const char *s) {
    if (!*s) return -1;
    long v = 0;
    int digits = 0;
    for (; *s; s++, digits++) {
        if (*s < '0' || *s > '9' || digits >= 9) return -1;
        v = v * 10 + (*s - '0');
    }
    return v;
}

static int boolean(const char *s) {
    if (ieq(s, "on") || ieq(s, "yes") || ieq(s, "true") || ieq(s, "1")) return 1;
    if (ieq(s, "off") || ieq(s, "no") || ieq(s, "false") || ieq(s, "0")) return 0;
    return -1;
}

typedef struct { char *buf; size_t cap, used; } Report;

static void say(Report *r, int line, const char *fmt, ...) {
    if (!r->buf || r->used + 1 >= r->cap) return;
    int n = snprintf(r->buf + r->used, r->cap - r->used, "line %d: ", line);
    if (n < 0) return;
    r->used += (size_t)n < r->cap - r->used ? (size_t)n : r->cap - r->used - 1;
    va_list ap;
    va_start(ap, fmt);
    n = vsnprintf(r->buf + r->used, r->cap - r->used, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    r->used += (size_t)n < r->cap - r->used ? (size_t)n : r->cap - r->used - 1;
    if (r->used + 2 < r->cap) { r->buf[r->used++] = '\n'; r->buf[r->used] = 0; }
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}

static int apply(Pc98Config *c, const char *key, const char *val, Report *rep, int line) {
    long n;
    int b;
    if (ieq(key, "font") || ieq(key, "rhythm")) {
        char *dst = ieq(key, "font") ? c->font : c->rhythm;
        if (strlen(val) >= CONFIG_PATH_MAX) { say(rep, line, "%s: the path is too long", key); return 0; }
        memcpy(dst, val, strlen(val) + 1);
        return 1;
    }
    if (ieq(key, "cpu")) {
        int v = ieq(val, "8086") ? CFG_CPU_8086 : ieq(val, "v30") ? CFG_CPU_V30 : ieq(val, "286") ? CFG_CPU_286
              : ieq(val, "386") ? CFG_CPU_386 : ieq(val, "486") ? CFG_CPU_486 : -1;
        if (v < 0) { say(rep, line, "cpu: '%s' is not 8086, v30, 286, 386 or 486", val); return 0; }
        c->cpu = v;
        return 1;
    }
    if (ieq(key, "mhz")) {
        n = number(val);
        if (n < 1 || n > 200) { say(rep, line, "mhz: '%s' is not a number from 1 to 200", val); return 0; }
        c->mhz = (int)n;
        return 1;
    }
    if (ieq(key, "fm")) {
        int v = ieq(val, "86") ? CFG_FM_86 : ieq(val, "26k") ? CFG_FM_26K : ieq(val, "none") || ieq(val, "off") ? CFG_FM_NONE : -1;
        if (v < 0) { say(rep, line, "fm: '%s' is not 86, 26k or none", val); return 0; }
        c->fm = v;
        return 1;
    }
    if (ieq(key, "sound") || ieq(key, "idle") || ieq(key, "virtual_config")) {
        b = boolean(val);
        if (b < 0) { say(rep, line, "%s: '%s' is not on or off", key, val); return 0; }
        if (ieq(key, "sound")) c->sound = b; else if (ieq(key, "idle")) c->idle = b; else c->vconfig = b;
        return 1;
    }
    if (ieq(key, "volume")) {
        n = number(val);
        if (n < 0 || n > 100) { say(rep, line, "volume: '%s' is not a number from 0 to 100", val); return 0; }
        c->volume = (int)n;
        return 1;
    }
    if (ieq(key, "scale")) {
        n = ieq(val, "fit") ? 0 : number(val);
        if (n < 0 || n > 4 || (n == 0 && !ieq(val, "fit"))) { say(rep, line, "scale: '%s' is not 1, 2, 3, 4 or fit", val); return 0; }
        c->scale = (int)n;
        return 1;
    }
    if (ieq(key, "speed")) {
        n = ieq(val, "max") ? 0 : number(val);
        if (n < 0 || n > 10000 || (n == 0 && !ieq(val, "max"))) { say(rep, line, "speed: '%s' is not a percentage (1 to 10000) or max", val); return 0; }
        c->speed = (int)n;
        return 1;
    }
    say(rep, line, "unknown key '%s'", key);
    return 0;
}

int config_parse(Pc98Config *c, const char *text, size_t len, char *report, size_t report_cap) {
    Report rep = { report, report_cap, 0 };
    if (report && report_cap) report[0] = 0;
    int applied = 0, line = 0;
    size_t pos = 0;
    while (pos < len) {
        char buf[CONFIG_PATH_MAX + 64];
        size_t end = pos;
        while (end < len && text[end] != '\n') end++;
        line++;
        size_t n = end - pos;
        int too_long = n >= sizeof buf;
        if (too_long) n = sizeof buf - 1;
        memcpy(buf, text + pos, n);
        buf[n] = 0;
        pos = end + 1;
        char *s = trim(buf);
        if (!*s || *s == '#') continue;
        if (too_long) { say(&rep, line, "the line is too long"); continue; }
        char *eq = strchr(s, '=');
        if (!eq) { say(&rep, line, "no '=' in '%s'", s); continue; }
        *eq = 0;
        applied += apply(c, trim(s), trim(eq + 1), &rep, line);
    }
    return applied;
}

/* Appends one formatted piece to buf (cap bytes, *used so far); what does not fit is cut, but *used keeps counting. */
static void put(char *buf, size_t cap, size_t *used, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char piece[2 * CONFIG_PATH_MAX];
    int n = vsnprintf(piece, sizeof piece, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof piece) n = (int)sizeof piece - 1;
    if (buf && cap && *used < cap - 1) {
        size_t room = cap - 1 - *used, k = (size_t)n < room ? (size_t)n : room;
        memcpy(buf + *used, piece, k);
        buf[*used + k] = 0;
    }
    *used += (size_t)n;
}

size_t config_format(const Pc98Config *c, char *out, size_t cap) {
    size_t used = 0;
    if (out && cap) out[0] = 0;
    put(out, cap, &used, "# PC-98 emulator settings (the Hakurei emulator), one key=value per line.\n");
    put(out, cap, &used, "font=%s\nrhythm=%s\n", c->font, c->rhythm);
    put(out, cap, &used, "cpu=%s\nmhz=%d\nfm=%s\n", config_cpu_name(c->cpu), c->mhz, config_fm_name(c->fm));
    put(out, cap, &used, "sound=%s\nvolume=%d\n", c->sound ? "on" : "off", c->volume);
    if (c->scale) put(out, cap, &used, "scale=%d\n", c->scale); else put(out, cap, &used, "scale=fit\n");
    if (c->speed) put(out, cap, &used, "speed=%d\n", c->speed); else put(out, cap, &used, "speed=max\n");
    put(out, cap, &used, "idle=%s\n", c->idle ? "on" : "off");
    put(out, cap, &used, "virtual_config=%s\n", c->vconfig ? "on" : "off");
    return used;
}

/* ---------------------------------------------------------------- recent images */

void recent_parse(Pc98Recent *r, const char *text, size_t len) {
    memset(r, 0, sizeof *r);
    size_t pos = 0;
    while (pos < len && r->n < RECENT_MAX) {
        size_t end = pos;
        while (end < len && text[end] != '\n') end++;
        size_t n = end - pos;
        while (n && (text[pos + n - 1] == '\r' || text[pos + n - 1] == ' ' || text[pos + n - 1] == '\t')) n--;
        if (n && n < CONFIG_PATH_MAX && text[pos] != '#') {
            char *d = r->path[r->n];
            memcpy(d, text + pos, n);
            d[n] = 0;
            int dup = 0;
            for (int i = 0; i < r->n; i++) if (!strcmp(r->path[i], d)) dup = 1;
            if (!dup) r->n++;
        }
        pos = end + 1;
    }
}

void recent_add(Pc98Recent *r, const char *path) {
    if (!path || !*path || strlen(path) >= CONFIG_PATH_MAX) return;
    int at = r->n;
    for (int i = 0; i < r->n; i++) if (!strcmp(r->path[i], path)) { at = i; break; }
    if (at == r->n) {                           /* new: the last one falls off when the list is full */
        if (r->n < RECENT_MAX) r->n++;
        at = r->n - 1;
    }
    for (int i = at; i > 0; i--) memcpy(r->path[i], r->path[i - 1], CONFIG_PATH_MAX);
    memset(r->path[0], 0, CONFIG_PATH_MAX);
    memcpy(r->path[0], path, strlen(path) + 1);
}

size_t recent_format(const Pc98Recent *r, char *out, size_t cap) {
    size_t used = 0;
    for (int i = 0; i < r->n; i++) {
        size_t n = strlen(r->path[i]);
        if (out && used + n + 1 < cap) { memcpy(out + used, r->path[i], n); out[used + n] = '\n'; }
        used += n + 1;
    }
    if (out && cap) out[used < cap ? used : cap - 1] = 0;
    return used;
}
