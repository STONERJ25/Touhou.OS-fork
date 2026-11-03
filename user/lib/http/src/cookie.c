/*
 * A cookie jar in memory for libhttp (RFC 6265 sections 5.1 to 5.4): Set-Cookie with Expires, Max-Age, Domain, Path and Secure;
 * the Cookie header for a request. HttpOnly and SameSite are read past (there is no script here to hide cookies from).
 *
 * Not done: the public suffix list (a Domain attribute without a dot is refused, which stops "com" but not "co.uk"), persistence
 * across runs, more than 300 cookies (the oldest is dropped).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <http.h>

#define MAX_COOKIES 300

typedef struct {
    char name[128];
    char value[1024];
    char domain[HTTP_MAX_HOST];
    char path[256];
    int host_only, secure;
    int64_t expires;                                             /* 0: until the program ends */
    uint64_t order;                                              /* creation order, for ties */
} cookie;

struct HttpCookieJar {
    cookie c[MAX_COOKIES];
    int n;
    uint64_t next;
};

HttpCookieJar *http_jar_new(void) { return calloc(1, sizeof(HttpCookieJar)); }
void http_jar_free(HttpCookieJar *j) { free(j); }
int http_jar_count(const HttpCookieJar *j) { return j ? j->n : 0; }

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int ieqn(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (lower((unsigned char)a[i]) != lower((unsigned char)b[i])) return 0;
    return 1;
}

static void trim(const char **s, size_t *n) {
    while (*n && (**s == ' ' || **s == '\t')) { (*s)++; (*n)--; }
    while (*n && ((*s)[*n - 1] == ' ' || (*s)[*n - 1] == '\t')) (*n)--;
}

static int is_ipv4(const char *h) {
    int dots = 0;
    for (; *h; h++) {
        if (*h == '.') dots++;
        else if (*h < '0' || *h > '9') return 0;
    }
    return dots == 3;
}

/* RFC 6265 5.1.3 */
static int domain_match(const char *host, const char *domain) {
    size_t hl = strlen(host), dl = strlen(domain);
    if (hl == dl) return strcmp(host, domain) == 0;
    return hl > dl && host[hl - dl - 1] == '.' && strcmp(host + hl - dl, domain) == 0 && !is_ipv4(host);
}

/* RFC 6265 5.1.4 */
static int path_match(const char *req, size_t rlen, const char *cpath) {
    size_t cl = strlen(cpath);
    if (rlen < cl || memcmp(req, cpath, cl) != 0) return 0;
    return rlen == cl || cpath[cl - 1] == '/' || req[cl] == '/';
}

static void default_path(const HttpUrl *u, char *out, size_t cap) {
    size_t n = strcspn(u->path, "?");
    size_t last = 0;
    for (size_t i = 0; i < n; i++)
        if (u->path[i] == '/') last = i;
    if (last == 0) snprintf(out, cap, "/");
    else snprintf(out, cap, "%.*s", (int)last, u->path);
}

/* RFC 6265 5.1.1, the parts that matter: a time hh:mm:ss, a day 1-31, a month name, a year (two digits: 70-99 are 19xx) */
static int64_t cookie_date(const char *s, size_t n) {
    static const char *months = "janfebmaraprmayjunjulaugsepoctnovdec";
    int hh = -1, mi = -1, ss = -1, day = -1, mon = -1, year = -1;
    size_t i = 0;
    while (i < n) {
        while (i < n && !((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || s[i] == ':')) i++;
        size_t b = i;
        while (i < n && ((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || s[i] == ':')) i++;
        size_t len = i - b;
        const char *t = s + b;
        if (!len) continue;
        int a, c, d;
        if (hh < 0 && len >= 5 && sscanf(t, "%d:%d:%d", &a, &c, &d) == 3) { hh = a; mi = c; ss = d; continue; }
        if (mon < 0 && len >= 3) {
            int found = -1;
            for (int k = 0; k < 12; k++)
                if (ieqn(t, months + 3 * k, 3)) found = k;
            if (found >= 0) { mon = found + 1; continue; }
        }
        if (t[0] >= '0' && t[0] <= '9') {
            int v = 0, digits = 0;
            while (digits < (int)len && t[digits] >= '0' && t[digits] <= '9') v = v * 10 + (t[digits++] - '0');
            if (day < 0 && digits <= 2) { day = v; continue; }
            if (year < 0 && (digits == 2 || digits == 4)) { year = digits == 2 ? (v >= 70 ? 1900 + v : 2000 + v) : v; continue; }
        }
    }
    if (hh < 0 || hh > 23 || mi > 59 || ss > 59 || day < 1 || day > 31 || mon < 1 || year < 1601) return -1;
    int64_t y = year - (mon <= 2);                                   /* days from civil */
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (mon > 2 ? mon - 3 : mon + 9) + 2) / 5 + day - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (era * 146097 + doe - 719468) * 86400 + hh * 3600 + mi * 60 + ss;
}

void http_jar_set(HttpCookieJar *j, const HttpUrl *from, const char *sc, int64_t now) {
    if (!j || !from || !sc) return;
    size_t total = strlen(sc), first = strcspn(sc, ";");
    const char *nv = sc;
    size_t nvl = first;
    const char *eq = memchr(nv, '=', nvl);
    if (!eq) return;                                                 /* no name=value: ignored (5.2 step 2) */
    const char *name = nv, *value = eq + 1;
    size_t nl = (size_t)(eq - nv), vl = nvl - nl - 1;
    trim(&name, &nl);
    trim(&value, &vl);
    if (nl == 0 || nl >= sizeof ((cookie *)0)->name || vl >= sizeof ((cookie *)0)->value) return;
    cookie k;
    memset(&k, 0, sizeof k);
    memcpy(k.name, name, nl);
    memcpy(k.value, value, vl);
    int64_t max_age = 0, expires = 0;
    int have_max_age = 0, have_expires = 0;
    char domain[HTTP_MAX_HOST] = "";
    default_path(from, k.path, sizeof k.path);
    for (size_t i = first; i < total;) {
        i++;                                                         /* past ';' */
        size_t e = i + strcspn(sc + i, ";");
        const char *a = sc + i, *av = NULL;
        size_t al = e - i, avl = 0;
        const char *aeq = memchr(a, '=', al);
        if (aeq) { av = aeq + 1; avl = al - (size_t)(aeq - a) - 1; al = (size_t)(aeq - a); trim(&av, &avl); }
        trim(&a, &al);
        if (al == 7 && ieqn(a, "expires", 7) && av) {
            int64_t t = cookie_date(av, avl);
            if (t >= 0) { expires = t; have_expires = 1; }
        } else if (al == 7 && ieqn(a, "max-age", 7) && av && avl) {
            int neg = av[0] == '-', ok = 1;
            int64_t v = 0;
            for (size_t q = neg; q < avl; q++) {
                if (av[q] < '0' || av[q] > '9') { ok = 0; break; }
                if (v < 1000000000000LL) v = v * 10 + (av[q] - '0');
            }
            if (ok) { max_age = neg ? -v : v; have_max_age = 1; }
        } else if (al == 6 && ieqn(a, "domain", 6) && av && avl) {
            if (av[0] == '.') { av++; avl--; }
            if (avl && avl < sizeof domain) {
                for (size_t q = 0; q < avl; q++) domain[q] = (char)lower((unsigned char)av[q]);
                domain[avl] = 0;
            }
        } else if (al == 4 && ieqn(a, "path", 4) && av && avl && av[0] == '/' && avl < sizeof k.path) {
            memcpy(k.path, av, avl);
            k.path[avl] = 0;
        } else if (al == 6 && ieqn(a, "secure", 6)) {
            k.secure = 1;
        }
        i = e;
    }
    if (domain[0]) {
        if (!domain_match(from->host, domain) || (!strchr(domain, '.') && strcmp(domain, from->host) != 0)) return;   /* 5.3 step 6 */
        snprintf(k.domain, sizeof k.domain, "%s", domain);
    } else {
        k.host_only = 1;
        snprintf(k.domain, sizeof k.domain, "%s", from->host);
    }
    if (have_max_age) k.expires = max_age <= 0 ? 1 : now + max_age;
    else if (have_expires) k.expires = expires > 0 ? expires : 1;
    int idx = -1;
    for (int i = 0; i < j->n; i++)
        if (!strcmp(j->c[i].name, k.name) && !strcmp(j->c[i].domain, k.domain) && !strcmp(j->c[i].path, k.path) && j->c[i].host_only == k.host_only) idx = i;
    if (k.expires && k.expires <= now) {                             /* an expiry in the past deletes */
        if (idx >= 0) j->c[idx] = j->c[--j->n];
        return;
    }
    if (idx >= 0) {
        k.order = j->c[idx].order;                                   /* a replaced cookie keeps its creation time (5.3 step 11) */
        j->c[idx] = k;
        return;
    }
    if (j->n == MAX_COOKIES) {                                       /* full: the oldest goes */
        int old = 0;
        for (int i = 1; i < j->n; i++)
            if (j->c[i].order < j->c[old].order) old = i;
        j->c[old] = j->c[--j->n];
    }
    k.order = j->next++;
    j->c[j->n++] = k;
}

int http_jar_header(HttpCookieJar *j, const HttpUrl *to, int64_t now, char *buf, size_t cap) {
    if (cap) buf[0] = 0;
    if (!j || !to || cap == 0) return 0;
    size_t rlen = strcspn(to->path, "?");
    int secure = strcmp(to->scheme, "https") == 0;
    const cookie *pick[MAX_COOKIES];
    int n = 0;
    for (int i = 0; i < j->n; i++) {
        const cookie *k = &j->c[i];
        if (k->expires && k->expires <= now) continue;
        if (k->host_only ? strcmp(to->host, k->domain) != 0 : !domain_match(to->host, k->domain)) continue;
        if (!path_match(to->path, rlen, k->path) || (k->secure && !secure)) continue;
        pick[n++] = k;
    }
    for (int a = 1; a < n; a++)                                      /* longer paths first, then older (5.4 step 2) */
        for (int b = a; b > 0; b--) {
            size_t la = strlen(pick[b]->path), lb = strlen(pick[b - 1]->path);
            if (la > lb || (la == lb && pick[b]->order < pick[b - 1]->order)) {
                const cookie *t = pick[b]; pick[b] = pick[b - 1]; pick[b - 1] = t;
            } else {
                break;
            }
        }
    size_t o = 0;
    int used = 0;
    for (int i = 0; i < n; i++) {
        int w = snprintf(buf + o, cap - o, "%s%s=%s", used ? "; " : "", pick[i]->name, pick[i]->value);
        if (w < 0 || (size_t)w >= cap - o) break;
        o += (size_t)w;
        used++;
    }
    return used;
}
