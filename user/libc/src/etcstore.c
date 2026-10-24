/*
 * Keine's records: /etc files kept on the HUMANVILLAGE volume so they survive a restart. See etcstore.h and
 * docs/PERSISTENT_ETC.md for the design (where the files live, the boot order, atomic writes, how shadow stays root's).
 */
#include <etcstore.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <abi.h>

const char *const etc_kept_names[] = { "passwd", "shadow", "group", "hostname", "timezone", "network.conf", "update.conf", NULL };

static char g_note[160];

const char *etc_note(void) { return g_note; }

static void note(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_note, sizeof g_note, fmt, ap);
    va_end(ap);
}

int etc_is_kept(const char *name) {
    for (int i = 0; etc_kept_names[i]; i++) if (!strcmp(name, etc_kept_names[i])) return 1;
    return 0;
}

unsigned etc_file_mode(const char *name) { return strcmp(name, "shadow") ? 0644 : 0600; }

/* ------------------------------------------------------------------------------------------------ checking */

typedef struct { char *why; size_t n; } Why;

static int bad(Why *w, int line, const char *fmt, ...) {
    char msg[120];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (w->why && w->n) {
        if (line > 0) snprintf(w->why, w->n, "line %d: %s", line, msg);
        else snprintf(w->why, w->n, "%s", msg);
    }
    return -1;
}

/* Splits a line (no newline) at `sep` into at most `max` fields; returns how many there are (more than max: max + 1). */
static int split(char *line, char sep, char **f, int max) {
    int n = 0;
    char *p = line;
    for (;;) {
        if (n == max) return max + 1;
        f[n++] = p;
        char *c = strchr(p, sep);
        if (!c) return n;
        *c = 0;
        p = c + 1;
    }
}

static int count_fields(const char *line, char sep) {
    int n = 1;
    for (; *line; line++) n += *line == sep;
    return n;
}

static bool good_name(const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n > 31 || s[0] == '-' || s[0] == '.') return false;
    for (const char *p = s; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.')) return false;
    return true;
}

static bool good_number(const char *s, unsigned long max) {
    if (!*s || strlen(s) > 10) return false;
    unsigned long v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + (unsigned long)(*p - '0');
    }
    return v <= max;
}

static bool is_hex(const char *s, size_t n) {
    if (strlen(s) != n) return false;
    for (size_t i = 0; i < n; i++) if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}

/* A name that appears twice is a damaged file, not a choice. Names are kept in a small list while checking. */
typedef struct { char names[256][32]; int n; } Seen;

static bool seen_before(Seen *s, const char *name) {
    for (int i = 0; i < s->n; i++) if (!strcmp(s->names[i], name)) return true;
    if (s->n < 256) snprintf(s->names[s->n++], sizeof s->names[0], "%s", name);
    return false;
}

static int check_passwd(char *line, int ln, Why *w, Seen *seen, bool *root) {
    char *f[8];
    int fields = count_fields(line, ':');
    if (fields != 7) return bad(w, ln, "%d fields, a passwd line has 7", fields);
    split(line, ':', f, 7);
    if (!good_name(f[0])) return bad(w, ln, "'%s' is not a user name", f[0]);
    if (!good_number(f[2], 4294967294ul)) return bad(w, ln, "the uid of %s is not a number", f[0]);
    if (!good_number(f[3], 4294967294ul)) return bad(w, ln, "the gid of %s is not a number", f[0]);
    if (f[5][0] != '/') return bad(w, ln, "the home of %s is not an absolute path", f[0]);
    if (f[6][0] != '/') return bad(w, ln, "the shell of %s is not an absolute path", f[0]);
    if (seen_before(seen, f[0])) return bad(w, ln, "%s is listed twice", f[0]);
    if (!strcmp(f[0], "root") && !strcmp(f[2], "0")) *root = true;
    return 0;
}

static int check_group(char *line, int ln, Why *w, Seen *seen) {
    char *f[5];
    int fields = count_fields(line, ':');
    if (fields != 4) return bad(w, ln, "%d fields, a group line has 4", fields);
    split(line, ':', f, 4);
    if (!good_name(f[0])) return bad(w, ln, "'%s' is not a group name", f[0]);
    if (!good_number(f[2], 4294967294ul)) return bad(w, ln, "the gid of %s is not a number", f[0]);
    if (f[3][0]) {
        char *m[300];
        int k = split(f[3], ',', m, 299);
        for (int i = 0; i < k && i < 299; i++) if (!good_name(m[i])) return bad(w, ln, "'%s' in group %s is not a user name", m[i], f[0]);
    }
    if (seen_before(seen, f[0])) return bad(w, ln, "group %s is listed twice", f[0]);
    return 0;
}

static int check_shadow(char *line, int ln, Why *w, Seen *seen, bool *root) {
    char *f[10];
    int fields = count_fields(line, ':');
    if (fields < 2 || fields > 9) return bad(w, ln, "%d fields, a shadow line has 2 to 9", fields);
    split(line, ':', f, 9);
    if (!good_name(f[0])) return bad(w, ln, "'%s' is not a user name", f[0]);
    const char *h = f[1];
    if (!strncmp(h, "$gap$", 5)) {
        const char *salt = h + 5, *d = strchr(salt, '$');
        if (!d || d == salt || d - salt > 22 || !is_hex(d + 1, 64)) return bad(w, ln, "the password hash of %s is damaged", f[0]);
    } else if (h[0] && h[0] != '!' && h[0] != '*') {
        return bad(w, ln, "the password field of %s is neither a $gap$ hash, empty, '!' nor '*'", f[0]);
    }
    if (seen_before(seen, f[0])) return bad(w, ln, "%s is listed twice", f[0]);
    if (!strcmp(f[0], "root")) *root = true;
    return 0;
}

static bool parse_ipv4(const char *s, unsigned long *out) {
    unsigned long v = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return false;
        unsigned long o = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') { o = o * 10 + (unsigned long)(*s++ - '0'); if (++digits > 3) return false; }
        if (o > 255) return false;
        v = v << 8 | o;
        if (part < 3 && *s++ != '.') return false;
    }
    if (*s) return false;
    *out = v;
    return true;
}

static bool usable_host(unsigned long a) {
    unsigned top = (unsigned)(a >> 24);
    return top != 0 && top != 127 && top < 224;
}

static bool good_mac(const char *s) {
    if (strlen(s) != 17) return false;
    for (int i = 0; i < 17; i++) {
        char c = s[i];
        if (i % 3 == 2) { if (c != ':') return false; }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

/* network.conf: IFACE dhcp|static ADDR/PREFIX [gateway GW] [dns D [D [D]]] [mac XX:XX:XX:XX:XX:XX] */
static int check_network(char *line, int ln, Why *w, Seen *seen) {
    char *t[16];
    int n = 0;
    for (char *p = strtok(line, " \t"); p; p = strtok(NULL, " \t")) {
        if (n == 16) return bad(w, ln, "too many words");
        t[n++] = p;
    }
    if (n < 2) return bad(w, ln, "expected 'IFACE dhcp' or 'IFACE static ADDR/PREFIX ...'");
    if (strlen(t[0]) > 15 || !good_name(t[0])) return bad(w, ln, "'%s' is not a card name", t[0]);
    int i = 2;
    unsigned long addr = 0, mask = 0, gw = 0;
    bool is_static = !strcmp(t[1], "static"), have_gw = false;
    if (is_static) {
        if (n < 3) return bad(w, ln, "static needs ADDR/PREFIX");
        char a[24];
        snprintf(a, sizeof a, "%s", t[2]);
        char *slash = strchr(a, '/');
        if (!slash) return bad(w, ln, "'%s' has no /PREFIX", t[2]);
        *slash = 0;
        if (!parse_ipv4(a, &addr) || !usable_host(addr)) return bad(w, ln, "'%s' is not a usable address", a);
        if (!good_number(slash + 1, 30) || atoi(slash + 1) < 1) return bad(w, ln, "the prefix must be 1 to 30");
        mask = 0xFFFFFFFFul << (32 - atoi(slash + 1)) & 0xFFFFFFFFul;
        i = 3;
    } else if (strcmp(t[1], "dhcp")) {
        return bad(w, ln, "'%s' is neither dhcp nor static", t[1]);
    }
    const char *mac = "";
    while (i < n) {
        if (!strcmp(t[i], "gateway") && is_static && i + 1 < n) {
            if (!parse_ipv4(t[i + 1], &gw) || !usable_host(gw) || gw == addr || (gw & mask) != (addr & mask))
                return bad(w, ln, "the gateway %s is not inside the card's network", t[i + 1]);
            have_gw = true;
            i += 2;
        } else if (!strcmp(t[i], "dns") && is_static && i + 1 < n) {
            int k = 0;
            unsigned long d;
            for (i++; i < n && parse_ipv4(t[i], &d); i++) if (++k > 3) return bad(w, ln, "at most three name servers");
            if (!k) return bad(w, ln, "dns needs an address");
        } else if (!strcmp(t[i], "mac") && i + 1 < n) {
            if (!good_mac(t[i + 1])) return bad(w, ln, "'%s' is not a MAC address", t[i + 1]);
            mac = t[i + 1];
            i += 2;
        } else {
            return bad(w, ln, "unexpected '%s'", t[i]);
        }
    }
    (void)have_gw;
    char key[48];
    snprintf(key, sizeof key, "%s/%s", t[0], mac);
    if (seen_before(seen, key)) return bad(w, ln, "%s%s%s is configured twice", t[0], mac[0] ? " " : "", mac);
    return 0;
}

/* update.conf: key=value; url= (http:// or https://) and key= (16 lower-case hex digits, or empty) are understood. */
static int check_update(char *line, int ln, Why *w, Seen *seen) {
    char *eq = strchr(line, '=');
    if (!eq || eq == line) return bad(w, ln, "expected key=value");
    *eq = 0;
    const char *k = line, *v = eq + 1;
    for (const char *p = k; *p; p++) if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return bad(w, ln, "'%s' is not a key", k);
    if (seen_before(seen, k)) return bad(w, ln, "%s is set twice", k);
    if (!strcmp(k, "url")) {
        if (v[0] && strncmp(v, "http://", 7) && strncmp(v, "https://", 8)) return bad(w, ln, "the url must start with http:// or https://");
        if (strlen(v) > 200) return bad(w, ln, "the url is longer than 200 characters");
        for (const char *p = v; *p; p++) if (*p == ' ' || *p == '\t') return bad(w, ln, "the url contains a space");
    } else if (!strcmp(k, "key")) {
        if (v[0] && !is_hex(v, 16)) return bad(w, ln, "the key id must be 16 lower-case hex digits");
    }
    return 0;
}

static int check_oneline(const char *name, char *line, int ln, Why *w, int *count) {
    if (++*count > 1) return bad(w, ln, "%s holds a single line", name);
    size_t n = strlen(line);
    if (!strcmp(name, "hostname")) {
        if (n > 31) return bad(w, ln, "a host name has at most 31 characters");
        if (line[0] == '-' || line[n - 1] == '-') return bad(w, ln, "a host name does not start or end with '-'");
        for (const char *p = line; *p; p++)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-'))
                return bad(w, ln, "a host name has only letters, digits and '-'");
    } else {
        if (n > 63) return bad(w, ln, "a time zone has at most 63 characters");
        if (!((line[0] >= 'A' && line[0] <= 'Z') || (line[0] >= 'a' && line[0] <= 'z') || line[0] == '<'))
            return bad(w, ln, "a time zone starts with its name (UTC, JST-9, EST5EDT,M3.2.0,M11.1.0)");
        for (const char *p = line; *p; p++) if (*p == ' ' || *p == '\t') return bad(w, ln, "a time zone has no spaces");
    }
    return 0;
}

int etc_validate(const char *name, const char *data, size_t len, char *why, size_t whylen) {
    Why w = { why, whylen };
    if (why && whylen) why[0] = 0;
    if (!etc_is_kept(name)) return bad(&w, 0, "%s is not one of the kept files", name);
    if (len > ETC_MAX_BYTES) return bad(&w, 0, "%lu bytes, more than %d", (unsigned long)len, ETC_MAX_BYTES);
    bool conf = !strcmp(name, "network.conf") || !strcmp(name, "update.conf");
    if (len == 0) return conf ? 0 : bad(&w, 0, "the file is empty");
    if (data[len - 1] != '\n') return bad(&w, 0, "the last line is cut short, it has no newline");
    int ln = 1;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        if (c == '\n') { ln++; continue; }
        if (c == 0) return bad(&w, ln, "a NUL byte");
        if ((c < 0x20 && c != '\t') || c == 0x7F) return bad(&w, ln, "a control character (0x%02x)", c);
    }
    char *copy = malloc(len + 1);
    if (!copy) return bad(&w, 0, "out of memory");
    memcpy(copy, data, len);
    copy[len] = 0;
    Seen *seen = calloc(1, sizeof *seen);
    if (!seen) { free(copy); return bad(&w, 0, "out of memory"); }
    bool root = false;
    int rc = 0, count = 0;
    ln = 0;
    for (char *p = copy, *nl; rc == 0 && *p; p = nl + 1) {
        nl = strchr(p, '\n');
        *nl = 0;
        ln++;
        if (!*p) continue;                                           /* blank lines are allowed everywhere */
        if (conf && p[0] == '#') continue;
        if (!strcmp(name, "passwd")) rc = check_passwd(p, ln, &w, seen, &root);
        else if (!strcmp(name, "group")) rc = check_group(p, ln, &w, seen);
        else if (!strcmp(name, "shadow")) rc = check_shadow(p, ln, &w, seen, &root);
        else if (!strcmp(name, "network.conf")) rc = check_network(p, ln, &w, seen);
        else if (!strcmp(name, "update.conf")) rc = check_update(p, ln, &w, seen);
        else rc = check_oneline(name, p, ln, &w, &count);
    }
    if (rc == 0) {
        if ((!strcmp(name, "passwd") || !strcmp(name, "shadow")) && !root) rc = bad(&w, 0, "there is no line for root");
        else if ((!strcmp(name, "hostname") || !strcmp(name, "timezone")) && count == 0) rc = bad(&w, 0, "the file has no %s in it", name);
        else if (!strcmp(name, "group") && seen->n == 0) rc = bad(&w, 0, "there are no groups in it");
    }
    free(seen);
    free(copy);
    return rc;
}

/* ------------------------------------------------------------------------------------------------ writing */

int etc_volume_present(int *ro) {
    struct statfs st;
    if (ro) *ro = 0;
    if (statfs("/HumanVillage", &st) < 0 || strcmp(st.f_mntonname, "/HumanVillage")) return 0;
    if (ro) *ro = (st.f_flags & MNT_RDONLY) != 0;
    return 1;
}

int etc_write_atomic(const char *path, const void *data, size_t len, unsigned mode, int durable) {
    char tmp[256];
    if (snprintf(tmp, sizeof tmp, "%s.new", path) >= (int)sizeof tmp) { errno = ENAMETOOLONG; return -1; }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);          /* born with the final mode: shadow is never readable */
    if (fd < 0) return -1;
    const char *p = data;
    size_t left = len;
    while (left) {
        ssize_t n = write(fd, p, left);
        if (n <= 0) { int e = n < 0 ? errno : EIO; close(fd); unlink(tmp); errno = e; return -1; }
        p += n;
        left -= (size_t)n;
    }
    if (close(fd) < 0) { int e = errno; unlink(tmp); errno = e; return -1; }
    if (!durable) chmod(tmp, mode);                                  /* tmpfs: exactly MODE whatever the umask; FAT decides by itself */
    if (durable) sync();                                             /* the new contents reach the disk before the name moves */
    if (rename(tmp, path) < 0) { int e = errno; unlink(tmp); errno = e; return -1; }
    if (durable) sync();
    return 0;
}

static int keep_on_volume(const char *name, const void *data, size_t len) {
    int ro;
    if (!etc_volume_present(&ro)) return 0;                           /* no volume: /etc is all there is, as before */
    if (ro) { note("the HUMANVILLAGE volume is mounted read-only (it was not cleanly unmounted), so it cannot keep %s", name); errno = EROFS; return 1; }
    if ((mkdir("/HumanVillage/.system", 0700) < 0 && errno != EEXIST) || (mkdir(ETC_KEEP_DIR, 0700) < 0 && errno != EEXIST)) {
        note("cannot make %s: %s", ETC_KEEP_DIR, strerror(errno));
        return 1;
    }
    char path[128];
    snprintf(path, sizeof path, "%s/%s", ETC_KEEP_DIR, name);
    if (etc_write_atomic(path, data, len, etc_file_mode(name), 1) < 0) {
        note("cannot write %s: %s", path, strerror(errno));
        return 1;
    }
    return 0;
}

int etc_commit(const char *name, const void *data, size_t len) {
    g_note[0] = 0;
    char why[120];
    if (etc_validate(name, data, len, why, sizeof why) < 0) {
        note("%s: %s", name, why);
        errno = EINVAL;
        return -1;
    }
    char path[64];
    snprintf(path, sizeof path, "/etc/%s", name);
    if (etc_write_atomic(path, data, len, etc_file_mode(name), 0) < 0) {
        note("cannot write %s: %s", path, strerror(errno));
        return -1;
    }
    return keep_on_volume(name, data, len);
}

static char *read_small(const char *path, size_t *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    char *buf = malloc(ETC_MAX_BYTES + 1);
    size_t got = 0;
    ssize_t n = 0;
    while (buf && got <= ETC_MAX_BYTES && (n = read(fd, buf + got, ETC_MAX_BYTES + 1 - got)) > 0) got += (size_t)n;
    int e = errno;
    close(fd);
    if (!buf || n < 0) { free(buf); errno = buf ? e : ENOMEM; return NULL; }
    *len = got;
    return buf;
}

int etc_save(const char *name) {
    g_note[0] = 0;
    if (!etc_is_kept(name)) { note("%s is not one of the kept files", name); errno = EINVAL; return -1; }
    char path[64], why[120];
    snprintf(path, sizeof path, "/etc/%s", name);
    size_t len = 0;
    char *data = read_small(path, &len);
    if (!data) { note("cannot read %s: %s", path, strerror(errno)); return -1; }
    int rc;
    if (etc_validate(name, data, len, why, sizeof why) < 0) { note("%s: %s", path, why); errno = EINVAL; rc = -1; }
    else if (!etc_volume_present(NULL)) { note("there is no HUMANVILLAGE volume to keep it on"); errno = ENODEV; rc = 1; }
    else rc = keep_on_volume(name, data, len);
    free(data);
    return rc;
}

int etc_forget(const char *name) {
    g_note[0] = 0;
    if (!etc_is_kept(name)) { errno = EINVAL; return -1; }
    char path[128];
    snprintf(path, sizeof path, "%s/%s", ETC_KEEP_DIR, name);
    if (unlink(path) < 0) return -1;
    sync();
    return 0;
}

/* ------------------------------------------------------------------------------------------------ boot */

void etc_snapshot(EtcSnapshot *snap) {
    for (int i = 0; etc_kept_names[i] && i < 8; i++) {
        char path[64];
        struct stat st;
        snprintf(path, sizeof path, "/etc/%s", etc_kept_names[i]);
        if (stat(path, &st) == 0) { snap->mtime[i] = (long)st.st_mtime; snap->size[i] = (long)st.st_size; }
        else snap->mtime[i] = snap->size[i] = -1;
    }
}

/* The kept copy at PATH if it can be read and is well-formed. Otherwise NULL, with the reason in `why` ("cannot read it: I/O
 * error", "line 3: ...") or an empty `why` when there is no such file. */
static char *usable_copy(const char *name, const char *path, size_t *len, char *why, size_t whylen) {
    char *data = read_small(path, len);
    if (!data) {
        if (errno == ENOENT) why[0] = 0;
        else snprintf(why, whylen, "cannot read it: %s", strerror(errno));
        return NULL;
    }
    if (etc_validate(name, data, *len, why, whylen) < 0) { free(data); return NULL; }
    return data;
}

int etc_restore(const EtcSnapshot *snap, void (*log)(const char *line)) {
    if (!etc_volume_present(NULL)) return -1;
    char done[160] = "", line[240];
    int restored = 0, found = 0;
    EtcSnapshot now;
    if (snap) etc_snapshot(&now);
    for (int i = 0; etc_kept_names[i]; i++) {
        const char *name = etc_kept_names[i];
        char src[128], alt[136], dst[64], why[120], why_alt[120];
        snprintf(src, sizeof src, "%s/%s", ETC_KEEP_DIR, name);
        snprintf(alt, sizeof alt, "%s.new", src);
        snprintf(dst, sizeof dst, "/etc/%s", name);
        size_t len = 0;
        char *data = usable_copy(name, src, &len, why, sizeof why);
        bool from_alt = false;
        if (!data) {
            /* A power cut inside etc_write_atomic(), on a FAT driver that frees the replaced file's clusters before its entry is
             * rewritten, can leave NAME pointing at free clusters while NAME.new is complete (it was synced before the rename). */
            data = usable_copy(name, alt, &len, why_alt, sizeof why_alt);
            from_alt = data != NULL;
        }
        if (!data && !why[0]) continue;                                   /* not kept: the initrd default, nothing to say */
        found++;
        if (snap && (now.mtime[i] != snap->mtime[i] || now.size[i] != snap->size[i])) {
            snprintf(line, sizeof line, "%s changed since boot, so the copy in %s is not put over it", dst, ETC_KEEP_DIR);
        } else if (!data) {
            snprintf(line, sizeof line, "%s is damaged (%s): Keine falls back to the initrd default", src, why);
        } else if (etc_write_atomic(dst, data, len, etc_file_mode(name), 0) < 0) {
            snprintf(line, sizeof line, "cannot restore %s (%s): the initrd default stays", dst, strerror(errno));
        } else {
            restored++;
            size_t used = strlen(done);
            snprintf(done + used, sizeof done - used, " %s", name);
            if (from_alt && why[0]) snprintf(line, sizeof line, "%s is damaged (%s): the complete %s.new written just before is used", src, why, name);
            else if (from_alt) snprintf(line, sizeof line, "%s is missing: the complete %s.new written just before is used", src, name);
            else line[0] = 0;
        }
        if (line[0] && log) log(line);
        free(data);
    }
    if (log) {
        if (restored) snprintf(line, sizeof line, "/etc restored from the Human Village's records (%s):%s", ETC_KEEP_DIR, done);
        else if (!found) snprintf(line, sizeof line, "no records kept in %s: /etc keeps the initrd defaults", ETC_KEEP_DIR);
        if (restored || !found) log(line);
    }
    return restored;
}

int etc_reload_homes(void) {
    if (!etc_volume_present(NULL)) return 0;
    int fd = open("/HumanVillage", O_RDONLY | O_DIRECTORY);
    if (fd < 0) return -1;
    int rc = ioctl(fd, FATIOC_USERS_RELOAD, (void *)0);
    int e = errno;
    close(fd);
    errno = e;
    return rc;
}
