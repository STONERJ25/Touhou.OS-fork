/*
 * Public keys as the client meets them (a host's key in the key exchange, a line of a .pub file or of known_hosts), their fingerprints, and known_hosts.
 *
 * known_hosts (the format sshd(8) documents): one key per line, "[@marker] hosts keytype base64 [comment]". `hosts` is a comma-separated list of patterns
 * (* and ?, a leading ! excludes) or one hashed name "|1|base64(salt)|base64(HMAC-SHA1(salt, name))". A name is "host", or "[host]:port" when the port is
 * not 22. Markers: @revoked (this key must never be accepted) and @cert-authority (a CA key; certificates are not supported here, so the line is skipped).
 *
 * The decision follows ssh(1): the host's key is OK when an entry for the host holds it, CHANGED when an entry for the host holds another key of the same
 * type, NEW otherwise. A key of another type than the entries is NEW (this client only speaks ssh-ed25519, so a host known only by its RSA key is new to
 * it, and the caller can say so).
 */
#include <sshc.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------------------------------------ public keys */

static int mpint_bits(SshcRd *r) {
    size_t n;
    const uint8_t *m = sshc_rd_str(r, &n);
    if (r->bad) return 0;
    while (n && m[0] == 0) { m++; n--; }
    if (!n) return 0;
    int bits = (int)n * 8;
    for (uint8_t top = m[0]; !(top & 0x80); top <<= 1) bits--;
    return bits;
}

int sshc_pub_from_blob(const uint8_t *blob, size_t n, SshcPub *k) {
    SshcRd r;
    sshc_rd_init(&r, blob, n);
    size_t tl;
    const uint8_t *t = sshc_rd_str(&r, &tl);
    if (r.bad || tl == 0 || tl >= sizeof k->type || n > sizeof k->blob) return -1;
    for (size_t i = 0; i < tl; i++) if (t[i] < 0x21 || t[i] > 0x7e) return -1;
    memset(k, 0, sizeof *k);
    memcpy(k->type, t, tl);
    memcpy(k->blob, blob, n);
    k->blob_len = n;
    return 0;
}

int sshc_pub_parse(const char *line, SshcPub *k, char *comment, size_t ccap) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    const char *t0 = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
    size_t tl = (size_t)(p - t0);
    if (tl == 0 || tl >= sizeof k->type) return -1;
    while (*p == ' ' || *p == '\t') p++;
    const char *b0 = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
    size_t bl = (size_t)(p - b0);
    if (bl == 0) return -1;
    uint8_t blob[SSHC_BLOB_MAX];
    int got = sshc_b64_decode(b0, bl, blob, sizeof blob);
    if (got <= 0) return -1;
    SshcPub tmp;
    if (sshc_pub_from_blob(blob, (size_t)got, &tmp) != 0) return -1;
    if (strlen(tmp.type) != tl || memcmp(tmp.type, t0, tl) != 0) return -1;          /* the name in front must be the one inside */
    *k = tmp;
    if (comment && ccap) {
        while (*p == ' ' || *p == '\t') p++;
        const char *c0 = p;
        while (*p && *p != '\r' && *p != '\n') p++;
        size_t cl = (size_t)(p - c0);
        if (cl >= ccap) cl = ccap - 1;
        memcpy(comment, c0, cl);
        comment[cl] = 0;
    }
    return 0;
}

void sshc_pub_to_line(const SshcPub *k, const char *comment, char *out, size_t cap) {
    static char b64[2 * SSHC_BLOB_MAX];
    sshc_b64_encode(k->blob, k->blob_len, b64, true);
    if (comment && comment[0]) snprintf(out, cap, "%s %s %s", k->type, b64, comment);
    else snprintf(out, cap, "%s %s", k->type, b64);
}

void sshc_ed25519_blob(const uint8_t pk[32], uint8_t out[51]) {
    SshcWr w;
    sshc_wr_init(&w, out, 51);
    sshc_wr_cstr(&w, "ssh-ed25519");
    sshc_wr_str(&w, pk, 32);
}

bool sshc_pub_ed25519(const SshcPub *k, uint8_t pk[32]) {
    if (strcmp(k->type, "ssh-ed25519") != 0) return false;
    SshcRd r;
    sshc_rd_init(&r, k->blob, k->blob_len);
    size_t tl, kl;
    sshc_rd_str(&r, &tl);
    const uint8_t *key = sshc_rd_str(&r, &kl);
    if (r.bad || r.n != 0 || kl != 32) return false;
    memcpy(pk, key, 32);
    return true;
}

void sshc_fingerprint(const uint8_t *blob, size_t n, char out[64]) {
    uint8_t d[32];
    sshc_sha256(d, blob, n);
    memcpy(out, "SHA256:", 7);
    sshc_b64_encode(d, 32, out + 7, false);
}

void sshc_fingerprint_md5(const uint8_t *blob, size_t n, char out[64]) {
    uint8_t d[16];
    sshc_md5(d, blob, n);
    char *o = out;
    memcpy(o, "MD5:", 4);
    o += 4;
    for (int i = 0; i < 16; i++) o += sprintf(o, i ? ":%02x" : "%02x", d[i]);
}

int sshc_pub_bits(const SshcPub *k) {
    SshcRd r;
    sshc_rd_init(&r, k->blob, k->blob_len);
    size_t tl;
    sshc_rd_str(&r, &tl);
    if (!strcmp(k->type, "ssh-ed25519") || !strcmp(k->type, "sk-ssh-ed25519@openssh.com")) return 256;
    if (!strcmp(k->type, "ssh-rsa")) { mpint_bits(&r); return mpint_bits(&r); }        /* e, then n */
    if (!strcmp(k->type, "ssh-dss")) return mpint_bits(&r);                              /* p */
    if (!strcmp(k->type, "ecdsa-sha2-nistp256") || !strcmp(k->type, "sk-ecdsa-sha2-nistp256@openssh.com")) return 256;
    if (!strcmp(k->type, "ecdsa-sha2-nistp384")) return 384;
    if (!strcmp(k->type, "ecdsa-sha2-nistp521")) return 521;
    return 0;
}

const char *sshc_pub_label(const SshcPub *k) {
    if (!strcmp(k->type, "ssh-ed25519")) return "ED25519";
    if (!strcmp(k->type, "sk-ssh-ed25519@openssh.com")) return "ED25519-SK";
    if (!strcmp(k->type, "ssh-rsa")) return "RSA";
    if (!strcmp(k->type, "ssh-dss")) return "DSA";
    if (!strncmp(k->type, "ecdsa-sha2-", 11)) return "ECDSA";
    if (!strncmp(k->type, "sk-ecdsa-", 9)) return "ECDSA-SK";
    return "UNKNOWN";
}

/* ------------------------------------------------------------------------------------------------ randomart
 * The "drunken bishop" of OpenSSH: a bishop starts in the middle of a 17x9 board and, for each pair of bits of the fingerprint (least significant first),
 * steps diagonally; the squares he has visited are marked by how often. The picture lets a person notice a changed key without reading 43 characters. */

/* One border line of the picture: "+--[title]--+", the title centred in the 17 columns. */
static void art_border(char *row, int width, const char *title) {
    int tl = (int)strlen(title), p = 0;
    row[p++] = '+';
    for (int i = 0; i < (width - tl) / 2; i++) row[p++] = '-';
    for (int i = 0; i < tl; i++) row[p++] = title[i];
    while (p < width + 1) row[p++] = '-';
    row[p++] = '+';
    row[p] = 0;
}

void sshc_randomart(const uint8_t *blob, size_t n, const char *label, int bits, char *out, size_t cap) {
    enum { W = 17, H = 9 };
    static const char chars[] = " .o+=*BOX@%&#/^SE";
    const int len = (int)sizeof chars - 2;                    /* 16: the index of 'E'; 'S' is one before it, and a square never gets past '^' by visits */
    uint8_t d[32];
    sshc_sha256(d, blob, n);
    uint8_t field[W][H];
    memset(field, 0, sizeof field);
    int x = W / 2, y = H / 2;
    for (int i = 0; i < 32; i++) {
        int in = d[i];
        for (int b = 0; b < 4; b++) {                         /* each byte is four 2-bit moves: bit 0 steps in x, bit 1 in y */
            x += (in & 1) ? 1 : -1;
            y += (in & 2) ? 1 : -1;
            if (x < 0) x = 0;
            if (x > W - 1) x = W - 1;
            if (y < 0) y = 0;
            if (y > H - 1) y = H - 1;
            if (field[x][y] < len - 2) field[x][y]++;
            in >>= 2;
        }
    }
    field[W / 2][H / 2] = (uint8_t)(len - 1);                 /* 'S' */
    field[x][y] = (uint8_t)len;                               /* 'E' */
    char title[40], row[W + 8];
    snprintf(title, sizeof title, "[%s %d]", label, bits);
    size_t o = 0;
    art_border(row, W, title);
    o += (size_t)snprintf(out + o, cap - o, "%s\n", row);
    for (int j = 0; j < H && o < cap; j++) {
        int p = 0;
        row[p++] = '|';
        for (int i = 0; i < W; i++) row[p++] = chars[field[i][j] < len ? field[i][j] : len];
        row[p++] = '|';
        row[p] = 0;
        o += (size_t)snprintf(out + o, cap - o, "%s\n", row);
    }
    art_border(row, W, "[SHA256]");
    if (o < cap) snprintf(out + o, cap - o, "%s\n", row);
}

/* ------------------------------------------------------------------------------------------------ patterns */

bool sshc_pattern_match(const char *pat, const char *s) {
    const char *star = NULL, *mark = NULL;
    while (*s) {
        if (*pat == '*') { star = pat++; mark = s; continue; }
        if (*pat == '?' || tolower((unsigned char)*pat) == tolower((unsigned char)*s)) {
            if (*pat) { pat++; s++; continue; }
        }
        if (star) { pat = star + 1; s = ++mark; continue; }
        return false;
    }
    while (*pat == '*') pat++;
    return *pat == 0;
}

/* ------------------------------------------------------------------------------------------------ known_hosts */

void sshc_hk_name(const char *host, int port, char *out, size_t cap) {
    size_t n = 0;
    if (port == 22 || port <= 0) {
        for (; host[n] && n + 1 < cap; n++) out[n] = (char)tolower((unsigned char)host[n]);
        out[n] = 0;
        return;
    }
    char tmp[300];
    size_t hl = 0;
    for (; host[hl] && hl + 1 < sizeof tmp; hl++) tmp[hl] = (char)tolower((unsigned char)host[hl]);
    tmp[hl] = 0;
    snprintf(out, cap, "[%s]:%d", tmp, port);
}

/* Does the first field of a known_hosts line apply to `name`? Hashed entries are compared by recomputing the hash; plain ones are pattern lists. */
static bool hosts_field_matches(const char *f, size_t fl, const char *name) {
    if (fl > 3 && !memcmp(f, "|1|", 3)) {
        const char *salt = f + 3;
        const char *bar = memchr(salt, '|', fl - 3);
        if (!bar) return false;
        uint8_t s[64], want[40];
        int sl = sshc_b64_decode(salt, (size_t)(bar - salt), s, sizeof s);
        int wl = sshc_b64_decode(bar + 1, (size_t)(f + fl - (bar + 1)), want, sizeof want);
        if (sl <= 0 || wl != 20) return false;
        uint8_t got[20];
        sshc_hmac_sha1(got, s, (size_t)sl, name, strlen(name));
        return sshc_ct_equal(got, want, 20) != 0;
    }
    bool found = false;
    size_t i = 0;
    while (i < fl) {
        size_t j = i;
        while (j < fl && f[j] != ',') j++;
        char pat[300];
        size_t pl = j - i;
        bool neg = pl && f[i] == '!';
        if (neg) { i++; pl--; }
        if (pl < sizeof pat) {
            memcpy(pat, f + i, pl);
            pat[pl] = 0;
            if (sshc_pattern_match(pat, name)) {
                if (neg) return false;                      /* an exclusion wins over every inclusion */
                found = true;
            }
        }
        i = j + 1;
    }
    return found;
}

SshcHkResult sshc_hk_check_text(const char *text, size_t n, const char *file, const char *host, int port, const uint8_t *blob, size_t blob_len) {
    SshcHkResult res;
    memset(&res, 0, sizeof res);
    res.status = SSHC_HK_NEW;
    SshcPub want;
    if (sshc_pub_from_blob(blob, blob_len, &want) != 0) return res;
    char name[320];
    sshc_hk_name(host, port, name, sizeof name);
    int lineno = 0;
    bool have_changed = false;
    for (size_t pos = 0; pos < n;) {
        const char *line = text + pos;
        size_t ll = 0;
        while (pos + ll < n && line[ll] != '\n') ll++;
        pos += ll + 1;
        lineno++;
        const char *p = line, *end = line + ll;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (p >= end || *p == '#' || *p == '\r') continue;
        enum { M_NONE, M_REVOKED, M_CA } marker = M_NONE;
        if (*p == '@') {
            const char *m = p;
            while (p < end && *p != ' ' && *p != '\t') p++;
            if (p - m == 8 && !memcmp(m, "@revoked", 8)) marker = M_REVOKED;
            else if (p - m == 15 && !memcmp(m, "@cert-authority", 15)) marker = M_CA;
            else continue;
            while (p < end && (*p == ' ' || *p == '\t')) p++;
        }
        const char *hosts = p;
        while (p < end && *p != ' ' && *p != '\t') p++;
        size_t hl = (size_t)(p - hosts);
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        const char *kt = p;
        while (p < end && *p != ' ' && *p != '\t') p++;
        size_t ktl = (size_t)(p - kt);
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        const char *b64 = p;
        while (p < end && *p != ' ' && *p != '\t' && *p != '\r') p++;
        size_t bl = (size_t)(p - b64);
        if (!hl || !ktl || !bl || marker == M_CA) continue;
        uint8_t eb[SSHC_BLOB_MAX];
        int el = sshc_b64_decode(b64, bl, eb, sizeof eb);
        SshcPub have;
        if (el <= 0 || sshc_pub_from_blob(eb, (size_t)el, &have) != 0) continue;
        bool same_blob = (size_t)el == blob_len && !memcmp(eb, blob, blob_len);
        if (marker == M_REVOKED) {                          /* a revoked key is refused wherever it turns up */
            if (same_blob) {
                res.status = SSHC_HK_REVOKED;
                sshc_cpy(res.file, sizeof res.file, file ? file : "");
                res.line = lineno;
                return res;
            }
            continue;
        }
        if (!hosts_field_matches(hosts, hl, name)) continue;
        if (same_blob) {
            res.status = SSHC_HK_OK;
            sshc_cpy(res.file, sizeof res.file, file ? file : "");
            res.line = lineno;
            return res;
        }
        if (!strcmp(have.type, want.type)) {
            if (!have_changed) {
                have_changed = true;
                sshc_cpy(res.file, sizeof res.file, file ? file : "");
                res.line = lineno;
            }
        } else {
            res.other_types = true;
        }
    }
    res.status = have_changed ? SSHC_HK_CHANGED : SSHC_HK_NEW;
    return res;
}

char *sshc_read_file(const char *path, size_t max, size_t *len, bool check_mode, const char **why);

SshcHkResult sshc_hk_check(const char *const *files, int nfiles, const char *host, int port, const uint8_t *blob, size_t n) {
    SshcHkResult best;
    memset(&best, 0, sizeof best);
    best.status = SSHC_HK_NEW;
    bool changed = false;
    for (int i = 0; i < nfiles; i++) {
        size_t len = 0;
        const char *why = NULL;
        char *text = sshc_read_file(files[i], 4u << 20, &len, false, &why);
        if (!text) continue;                                /* no such file is the usual case, not an error */
        SshcHkResult r = sshc_hk_check_text(text, len, files[i], host, port, blob, n);
        free(text);
        if (r.status == SSHC_HK_OK || r.status == SSHC_HK_REVOKED) return r;
        if (r.status == SSHC_HK_CHANGED && !changed) { best = r; changed = true; }
        if (r.other_types) best.other_types = true;
    }
    return best;
}

int sshc_hk_add(const char *file, const char *host, int port, bool hash, const SshcPub *key) {
    char dir[512];
    sshc_cpy(dir, sizeof dir, file);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = 0;
        if (sshc_mkdir_p(dir) != 0) return -1;
    }
    char name[320], hostfield[200], b64[2 * SSHC_BLOB_MAX], line[2 * SSHC_BLOB_MAX + 400];
    sshc_hk_name(host, port, name, sizeof name);
    if (hash) {
        uint8_t salt[20], mac[20];
        char sb[40], mb[40];
        sshc_random(salt, sizeof salt);
        sshc_hmac_sha1(mac, salt, sizeof salt, name, strlen(name));
        sshc_b64_encode(salt, sizeof salt, sb, true);
        sshc_b64_encode(mac, sizeof mac, mb, true);
        snprintf(hostfield, sizeof hostfield, "|1|%s|%s", sb, mb);
    } else {
        sshc_cpy(hostfield, sizeof hostfield, name);
    }
    sshc_b64_encode(key->blob, key->blob_len, b64, true);
    int ll = snprintf(line, sizeof line, "%s %s %s\n", hostfield, key->type, b64);
    int fd = open(file, O_RDWR | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return -1;
    char last = '\n';
    off_t end = lseek(fd, 0, SEEK_END);
    if (end > 0 && lseek(fd, end - 1, SEEK_SET) == end - 1 && read(fd, &last, 1) != 1) last = '\n';
    lseek(fd, 0, SEEK_END);
    int rc = 0;
    if (last != '\n' && write(fd, "\n", 1) != 1) rc = -1;     /* a file without a final newline must not swallow the new line */
    if (rc == 0 && write(fd, line, (size_t)ll) != ll) rc = -1;
    if (close(fd) != 0) rc = -1;
    return rc;
}
