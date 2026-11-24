/*
 * update pull: the machine fetches its own signed update from a web server (docs/UPDATE.md, "Pulling from a server").
 *
 *   update pull URL [--now] [--no-reboot] [--power-off-after] [--loader]
 *   update auto                      started by init when the command line has update.pull=URL: pull now and then, confirm a pulled trial by itself
 *   update autoconfirm [SECONDS]     (what `auto` does on a trial start) wait, prove the network works with the new files, confirm
 *
 * Follows the manifest format 1 of docs/UPDATE.md ("The manifest, format 1", the grammar that tools/serve-update.py writes and checks): ECDSA P-256 over
 * SHA-256 of the body (FIPS 186-4, SEC 1), the signature as raw r || s. The verification is libtls's (user/lib/tls, written from the same
 * specifications); the parser below is this file's own and refuses every byte sequence the format does not allow.
 *
 * The order is the safe one: the manifest is fetched (at most 4096 bytes), parsed, its key id and signature checked against the key of this image
 * (/etc/update/pubkey: 130 hex digits, the point 04 || X || Y; no key, no pull, never a built-in one), its anti-rollback number `issued` compared
 * with the last pull's (update.iss on the ESP); files whose installed copy already has the listed hash are not fetched; the others are fetched into
 * memory with their size as a hard limit, hashed as they arrive and checked; only then is the whole verified set handed to the push's own code
 * (update.c, install_detached(): the power-cut-safe ESP sequence, the kept previous set and the trial), and the machine restarts. Nothing is written
 * to the ESP before every byte has been verified. Every fetch has a deadline: a server that stalls or trickles is given up.
 */
#include "kappabox.h"
#include "espgrant.h"
#include <abi.h>
#include <crypto.h>
#include <http.h>
#include <stdarg.h>
#include "updmanifest.h"

/* from update.c */
const char *upd_looks_wrong(const char *name, const uint8_t *d, size_t size);
int upd_cmdline_word(const char *word, char *value, size_t cap);
int upd_written_here(void);
int upd_confirm(void);
int upd_install(int n, const char *const *names, uint8_t *const *data, const size_t *sizes, const uint8_t (*sha)[32], const char *version, long long issued,
                const char *url, int reboot_in, int allow_loader, int force);

/* from libtls (src/tlsint.h): 0 when the signature is valid */
int tlsc_ecdsa_verify(int curve, const uint8_t *pub, size_t publen, const uint8_t *hash, size_t hashlen, const uint8_t *r, size_t rlen, const uint8_t *s, size_t slen);
#define TLSC_P256 1

#define PUBKEY_PATH   "/etc/update/pubkey"
#define ISSUED_NAME   "update.iss"
#define MAX_URL       300
#define MIN_INTERVAL  60                 /* seconds: the shortest update.pull.interval that is not 0 */
#define FETCH_SILENCE_MS 20000          /* the longest silence while connecting or reading */
#define MANIFEST_SECONDS 60             /* the whole manifest fetch */
#define FILE_MIN_SECONDS 120            /* a file: this long, plus one second per 64 KiB (a link of 0.5 Mbit/s still makes it) */

enum { PULL_INSTALLED, PULL_UNCHANGED, PULL_REFUSED, PULL_FAILED };   /* what a pull ended with (the exit status: 0, 0, 2, 1) */

static void psay(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void psay(const char *fmt, ...) {
    char line[500];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    printf("update: %s\n", line);
    fflush(stdout);
}

static void sha256_buf(const void *d, size_t n, uint8_t out[32]) {
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, d, n);
    sha256_final(&c, out);
}

/* ------------------------------------------------------------------------------------------------ the key of this image */

static int load_pubkey(uint8_t pub[65], char key_id[17], char *why, size_t wn) {
    char buf[200];
    int fd = open(PUBKEY_PATH, O_RDONLY);
    if (fd < 0) { snprintf(why, wn, "no signing key in this image (%s): it pulls nothing (docs/UPDATE.md says how a build gets one)", PUBKEY_PATH); return -1; }
    ssize_t r = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (r < 0) r = 0;
    buf[r] = 0;
    size_t n = (size_t)r;
    if (n && buf[n - 1] == '\n') n--;
    if (n != 130 || upd_lower_hex(buf, 130, pub) || pub[0] != 4) { snprintf(why, wn, "%s is not 130 lowercase hex digits of an uncompressed P-256 point (04...)", PUBKEY_PATH); return -1; }
    uint8_t h[32];
    sha256_buf(pub, 65, h);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) { key_id[2 * i] = hex[h[i] >> 4]; key_id[2 * i + 1] = hex[h[i] & 15]; }
    key_id[16] = 0;
    return 0;
}

/* key id first (only to say "another key" rather than "bad signature"), then the signature over the body */
static int check_signature(const uint8_t *d, const Manifest *m, const uint8_t pub[65], const char *key_id, char *why, size_t wn) {
    if (strcmp(m->key_id, key_id)) { snprintf(why, wn, "the manifest is signed by key %s, not by this image's key %s", m->key_id, key_id); return -1; }
    uint8_t h[32];
    sha256_buf(d, m->body_len, h);
    if (tlsc_ecdsa_verify(TLSC_P256, pub, 65, h, 32, m->sig, 32, m->sig + 32, 32) != 0) {
        snprintf(why, wn, "the signature does not match (the manifest was changed, or signed by another key)");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------ fetching, bounded in size and time */

typedef struct {
    uint8_t *buf;
    size_t len, limit;
    time_t deadline;
    int too_big, late;
    sha256_ctx h;
} Sink;

static int sink_body(void *ctx, const uint8_t *d, size_t n) {
    Sink *s = ctx;
    if (n > s->limit - s->len) { s->too_big = 1; return 1; }
    if (time(NULL) > s->deadline) { s->late = 1; return 1; }
    memcpy(s->buf + s->len, d, n);
    sha256_update(&s->h, d, n);
    s->len += n;
    return 0;
}

/* GET url into a buffer of limit bytes. 0, or PULL_REFUSED (larger than limit) / PULL_FAILED (no answer, not 200, too slow) with why */
static int fetch(const char *url, size_t limit, int seconds, uint8_t **out, size_t *len, uint8_t sha[32], char *why, size_t wn) {
    Sink s = { .limit = limit, .deadline = time(NULL) + seconds };
    s.buf = malloc(limit ? limit : 1);
    if (!s.buf) { snprintf(why, wn, "out of memory for %zu bytes", limit); return PULL_FAILED; }
    sha256_init(&s.h);
    HttpOptions o = { .timeout_ms = FETCH_SILENCE_MS, .no_decode = 1, .on_body = sink_body, .ctx = &s, .extra_headers = "Cache-Control: no-cache\r\n" };
    HttpResult r;
    memset(&r, 0, sizeof r);
    int rc = http_get(url, &o, &r);
    if (s.too_big) { snprintf(why, wn, "%s: more than the %zu bytes it may have", url, limit); free(s.buf); return PULL_REFUSED; }
    if (s.late) { snprintf(why, wn, "%s: not complete after %d seconds (given up)", url, seconds); free(s.buf); return PULL_FAILED; }
    if (rc != 0) { snprintf(why, wn, "%s: %s", url, r.error_text[0] ? r.error_text : http_strerror(rc)); free(s.buf); return PULL_FAILED; }
    /* (the signature protects what is installed whatever the transport; but an https URL is the owner's wish that nobody on the way sees or delays the update,
     * and a redirect to plain http would quietly drop that) */
    if (r.downgraded) { snprintf(why, wn, "%s: redirected from https to plain http, which an update does not follow", url); free(s.buf); return PULL_REFUSED; }
    if (r.status != 200) { snprintf(why, wn, "%s: the server answered %d %s", url, r.status, r.reason); free(s.buf); return PULL_FAILED; }
    sha256_final(&s.h, sha);
    *out = s.buf;
    *len = s.len;
    return 0;
}

static int join_url(char *out, size_t cap, const char *base, const char *tail) {
    size_t bl = strlen(base);
    while (bl && base[bl - 1] == '/') bl--;
    int n = snprintf(out, cap, "%.*s/%s", (int)bl, base, tail);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

static int url_ok(const char *u) {
    if (strncmp(u, "http://", 7) && strncmp(u, "https://", 8)) return 0;
    if (strlen(u) >= MAX_URL - 140) return 0;                    /* room for "/files/" and the longest path */
    for (const char *p = u; *p; p++)
        if (*p <= ' ' || *p > '~') return 0;
    return 1;
}

/* ------------------------------------------------------------------------------------------------ the ESP, read-only (what is installed) */

typedef struct { EspSession s; int mounted; } RoEsp;

static int ro_open(RoEsp *e, char *why, size_t wn) {
    char dev[32];
    memset(e, 0, sizeof *e);
    if (esp_find(dev, sizeof dev, why, wn)) return -1;
    snprintf(e->s.dir, sizeof e->s.dir, "/BambooForest/updpull.%d", (int)getpid());
    mkdir(e->s.dir, 0700);
    char src[48];
    snprintf(src, sizeof src, "/dev/%s", dev);
    if (gap_mount_opts(src, e->s.dir, "fat32", MNT_RDONLY, "ro") < 0) { snprintf(why, wn, "cannot read the ESP (%s): %s", src, strerror(errno)); rmdir(e->s.dir); return -1; }
    e->mounted = 1;
    return 0;
}

static void ro_close(RoEsp *e) {
    if (!e->mounted) return;
    gap_umount(e->s.dir);
    rmdir(e->s.dir);
    e->mounted = 0;
}

static int installed_same(RoEsp *e, const MEntry *f) {
    size_t len = 0;
    uint8_t *cur = (uint8_t *)esp_read(&e->s, f->path, &len);
    if (!cur) return 0;
    uint8_t h[32];
    sha256_buf(cur, len, h);
    free(cur);
    return len == f->size && !memcmp(h, f->sha, 32);
}

static long long issued_line(const char *t) {           /* the number of the line "issued N" of a record, -1 without one */
    long long v = -1;
    const char *p = !strncmp(t, "issued ", 7) ? t : strstr(t, "\nissued ");
    if (!p || sscanf(p + (*p == '\n'), "issued %lld", &v) != 1 || v < 0) return -1;
    return v;
}

/* The anti-rollback number of this machine: the largest `issued` a pull has begun to install here, -1 when there was none. update.iss is the record (written
 * before the first rename of an install, update.c); *done says the install it names ran to its end ("done" in the file). update.try and update.mf of a pull carry
 * the number as well, and the largest of the three counts: one damaged or missing file (a bad sector, a careless hand on the ESP) then does not open the door to
 * an older manifest. A number that comes only from update.try is not "done"; one from update.mf is (it is written after the last rename). */
static long long installed_issued(RoEsp *e, int *done) {
    size_t len = 0;
    long long v = -1;
    int d = 0;
    char *t = esp_read(&e->s, ISSUED_NAME, &len);
    if (t) { v = issued_line(t); d = v >= 0 && strstr(t, "\ndone\n") != NULL; free(t); }
    if ((t = esp_read(&e->s, "update.mf", &len)) != NULL) {
        long long w = strstr(t, "\npulled ") ? issued_line(t) : -1;
        if (w > v) { v = w; d = 1; }
        free(t);
    }
    if ((t = esp_read(&e->s, "update.try", &len)) != NULL) {
        long long w = strstr(t, "\npulled\n") ? issued_line(t) : -1;
        if (w > v) { v = w; d = 0; }
        free(t);
    }
    if (done) *done = d;
    return v;
}

/* Is a volume that was not cleanly unmounted explained by this machine's own interrupted update? Yes when update.iss names a number without "done" (a pull was
 * begun and cut short: update.c writes the number first and "done" last) or update.try is there (a trial is open: its renames may have been cut). Then the install
 * may mount the ESP with force, as `update confirm` does: the staged-compared-renamed sequence copes with what its own cut left (docs/UPDATE.md, "The anti-rollback
 * number and a power cut"). Dirt that nothing explains (another system, a crash of something else) is still refused: a person looks first. */
static int dirt_explained(RoEsp *e) {
    size_t len = 0;
    int yes = 0;
    char *t = esp_read(&e->s, ISSUED_NAME, &len);
    if (t) { yes = issued_line(t) >= 0 && !strstr(t, "\ndone\n"); free(t); }
    if ((t = esp_read(&e->s, "update.try", &len)) != NULL) { yes = 1; free(t); }
    return yes;
}

/* ------------------------------------------------------------------------------------------------ the pull */

static const struct { const char *name; uint64_t max; } PULLABLE[] = {
    { "EFI/BOOT/BOOTX64.EFI", 4u << 20 }, { "cmdline.txt", 200 }, { "initrd.img", 256u << 20 }, { "kernel.elf", 64u << 20 },
};

typedef struct { int reboot_in, allow_loader; } PullOpts;

/* fetch, verify, and when there is something new install it on trial. Returns PULL_*; the messages are said on the way */
static int do_pull(const char *url, const PullOpts *po) {
    char why[600], u[MAX_URL];
    uint8_t pub[65];
    char key_id[17];
    if (!url_ok(url)) { kb_error("the URL must be http://... or https://..., printable, at most %d characters", MAX_URL - 141); return PULL_REFUSED; }
    if (upd_cmdline_word("update.fellback", NULL, 0)) { kb_error("this start runs the previous files (the last update did not come up): not pulling; `update rollback` or `update confirm --anyway` first"); return PULL_REFUSED; }
    /* (an update still on trial, update.try on the ESP, is refused by the install step, as for a push: "an earlier update is still on trial") */
    if (load_pubkey(pub, key_id, why, sizeof why)) { kb_error("%s", why); return PULL_FAILED; }

    uint8_t *md = NULL, msha[32];
    size_t ml = 0;
    join_url(u, sizeof u, url, "manifest");
    psay("fetching %s", u);
    int rc = fetch(u, MAX_MANIFEST, MANIFEST_SECONDS, &md, &ml, msha, why, sizeof why);
    if (rc) { kb_error("%s: nothing was changed", why); return rc; }
    Manifest m;
    if (upd_parse_manifest(md, ml, &m, why, sizeof why) || check_signature(md, &m, pub, key_id, why, sizeof why)) {
        kb_error("the manifest is refused: %s: nothing was changed", why);
        free(md);
        return PULL_REFUSED;
    }
    free(md);
    psay("the manifest is signed by this image's key %s: version %s, issued %lld, %d file%s", key_id, m.version, m.issued, m.n, m.n == 1 ? "" : "s");

    /* every path one of the four, each within its size */
    for (int i = 0; i < m.n; i++) {
        int ok = 0;
        for (size_t k = 0; k < sizeof PULLABLE / sizeof PULLABLE[0]; k++)
            if (!strcmp(m.f[i].path, PULLABLE[k].name)) ok = m.f[i].size >= 1 && m.f[i].size <= PULLABLE[k].max ? 1 : -1;
        if (ok == 0) { kb_error("%s is not a file an update may carry: nothing was changed", m.f[i].path); return PULL_REFUSED; }
        if (ok < 0) { kb_error("%s: %llu bytes is not a size it may have: nothing was changed", m.f[i].path, (unsigned long long)m.f[i].size); return PULL_REFUSED; }
    }

    /* what is installed: the anti-rollback number and the files' hashes */
    RoEsp e;
    if (geteuid() != 0) { kb_error("only root may update the system (use sudo)"); return PULL_FAILED; }
    if (ro_open(&e, why, sizeof why)) { kb_error("%s", why); return PULL_FAILED; }
    int last_done = 0;
    long long last = installed_issued(&e, &last_done);
    int force = dirt_explained(&e);
    int need[MAX_MFILES] = { 0 }, kern = -1, rd = -1, nneed = 0;
    for (int i = 0; i < m.n; i++) {
        if (!strcmp(m.f[i].path, "kernel.elf")) kern = i;
        if (!strcmp(m.f[i].path, "initrd.img")) rd = i;
        need[i] = !installed_same(&e, &m.f[i]);
    }
    ro_close(&e);
    if (m.issued < last) {
        kb_error("the manifest was issued at %lld, before the last pull installed here (%lld): an old manifest is not installed again (rollback protection); nothing was changed", m.issued, last);
        return PULL_REFUSED;
    }
    if (m.issued == last && !last_done)
        psay("issued %lld was begun here and not finished (a power cut, or an error on the way): comparing the files", m.issued);
    if (m.issued == last && last_done) {
        psay("issued %lld is the last pull installed here: nothing new", m.issued);
        psay("result: unchanged");
        return PULL_UNCHANGED;
    }
    if ((kern < 0) != (rd < 0)) { kb_error("the kernel and the initrd go together (a kernel only runs with the initrd of its own build): nothing was changed"); return PULL_REFUSED; }
    if (kern >= 0 && (need[kern] || need[rd])) need[kern] = need[rd] = 1;
    for (int i = 0; i < m.n; i++) {
        if (need[i] && !strcmp(m.f[i].path, "EFI/BOOT/BOOTX64.EFI") && !po->allow_loader) {
            psay("the loader in the manifest differs from the installed one; it is only replaced with --loader (a loader that does not work cannot fall back)");
            need[i] = 0;
        }
        nneed += need[i];
    }
    if (!nneed) {
        psay("everything in the manifest is installed already: nothing to do");
        psay("result: unchanged");
        return PULL_UNCHANGED;
    }

    /* fetch what is new, each against its size and hash */
    const char *names[MAX_MFILES];
    uint8_t *data[MAX_MFILES];
    size_t sizes[MAX_MFILES];
    uint8_t sha[MAX_MFILES][32];
    int k = 0;
    rc = PULL_INSTALLED;
    for (int i = 0; i < m.n && rc == PULL_INSTALLED; i++) {
        if (!need[i]) continue;
        const MEntry *f = &m.f[i];
        char tail[160];
        snprintf(tail, sizeof tail, "files/%s", f->path);
        join_url(u, sizeof u, url, tail);
        size_t got = 0;
        uint8_t *d = NULL;
        int fr = fetch(u, (size_t)f->size, FILE_MIN_SECONDS + (int)(f->size >> 16), &d, &got, sha[k], why, sizeof why);
        if (fr) { kb_error("%s: nothing was changed", why); rc = fr; break; }
        if (got != f->size) { kb_error("%s: %zu bytes, the manifest says %llu: nothing was changed", f->path, got, (unsigned long long)f->size); free(d); rc = PULL_REFUSED; break; }
        if (memcmp(sha[k], f->sha, 32)) { kb_error("%s arrived damaged (its hash is not the one the manifest signs): nothing was changed", f->path); free(d); rc = PULL_REFUSED; break; }
        const char *bad = upd_looks_wrong(f->path, d, got);
        if (bad) { kb_error("%s %s: nothing was changed", f->path, bad); free(d); rc = PULL_REFUSED; break; }
        psay("fetched %s (%zu bytes, hash ok)", f->path, got);
        names[k] = f->path;
        data[k] = d;
        sizes[k++] = got;
    }
    if (rc == PULL_INSTALLED) {
        psay("version %s: every file verified; installing it on trial", m.version);
        int ir = upd_install(k, names, data, sizes, (const uint8_t (*)[32])sha, m.version, m.issued, url, po->reboot_in, po->allow_loader, force);
        rc = ir == 0 ? PULL_INSTALLED : ir == 2 ? PULL_REFUSED : PULL_FAILED;
    }
    for (int i = 0; i < k; i++) free(data[i]);
    return rc;
}

static int status_of(int r) { return r == PULL_INSTALLED || r == PULL_UNCHANGED ? 0 : r == PULL_REFUSED ? 2 : 1; }

static void power_off(const char *why) {
    psay("switching off (%s)", why);
    sync();
    gap_reboot(REBOOT_POWEROFF);
}

int upd_cmd_pull(int argc, char **argv) {
    PullOpts po = { .reboot_in = 3 };
    int off_after = 0;
    const char *url = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--now")) po.reboot_in = 0;
        else if (!strcmp(argv[i], "--no-reboot")) po.reboot_in = -1;
        else if (!strcmp(argv[i], "--power-off-after")) off_after = 1;
        else if (!strcmp(argv[i], "--loader")) po.allow_loader = 1;
        else if (argv[i][0] != '-' && !url) url = argv[i];
        else url = NULL, i = argc;
    }
    if (!url) { kb_error("usage: update pull URL [--now] [--no-reboot] [--power-off-after] [--loader]"); return 2; }
    if (geteuid() != 0) { kb_error("only root may update the system (use sudo)"); return 1; }
    int r = do_pull(url, &po);
    if (off_after && r != PULL_INSTALLED) power_off("--power-off-after: nothing was installed");
    return status_of(r);
}

/* ------------------------------------------------------------------------------------------------ the automatic mode and the self-confirmation */

static long word_seconds(const char *word, long dflt) {
    char v[32];
    if (!upd_cmdline_word(word, v, sizeof v)) return dflt;
    if (!strcmp(v, "off")) return -1;
    /* digits only, at most nine of them: an empty value ("update.autoconfirm=" would otherwise mean 0 seconds: confirm at once), a sign, a unit or a number that
     * does not fit is not a number, and the default stands */
    size_t len = strlen(v);
    if (len < 1 || len > 9 || strspn(v, "0123456789") != len) return dflt;
    return strtol(v, NULL, 10);
}

/* The test of health on a trial start that a pull installed (docs/UPDATE.md, "The self-confirmation"): update.try says "pulled"; this start runs the files on
 * the ESP (the trial start, nothing written since); and a fresh fetch of URL/manifest through this start's own network stack, TCP, HTTP (and TLS for https)
 * returns a manifest that verifies with this image's key, whose `issued` is the one in update.iss, and whose kernel.elf and initrd.img are, byte for byte by
 * their hashes, the files on the ESP. A system whose network, HTTP client or SHA-256/ECDSA code is broken cannot pass it. 0: healthy. */
static int health(const char *url, char *why, size_t wn) {
    if (upd_cmdline_word("update.fellback", NULL, 0)) { snprintf(why, wn, "this start fell back"); return -1; }
    if (!upd_cmdline_word("update.trial=", NULL, 0) || upd_written_here()) { snprintf(why, wn, "this start is not the trial start of the files on the ESP"); return -1; }
    uint8_t pub[65];
    char key_id[17];
    if (load_pubkey(pub, key_id, why, wn)) return -1;
    RoEsp e;
    if (ro_open(&e, why, wn)) return -1;
    size_t tl = 0;
    char *t = esp_read(&e.s, "update.try", &tl);
    int pulled = t && strstr(t, "\npulled\n");
    free(t);
    long long iss = installed_issued(&e, NULL);
    if (!pulled) { ro_close(&e); snprintf(why, wn, "the files on trial were not installed by a pull (update.try does not say \"pulled\"): a person confirms them"); return -1; }
    char u[MAX_URL];
    uint8_t *md = NULL, msha[32];
    size_t ml = 0;
    join_url(u, sizeof u, url, "manifest");
    int rc = fetch(u, MAX_MANIFEST, MANIFEST_SECONDS, &md, &ml, msha, why, wn);
    Manifest m;
    if (!rc && (upd_parse_manifest(md, ml, &m, why, wn) || check_signature(md, &m, pub, key_id, why, wn))) rc = -1;
    free(md);
    if (!rc && m.issued != iss) { snprintf(why, wn, "the server's manifest (issued %lld) is not the one installed (issued %lld)", m.issued, iss); rc = -1; }
    int seen = 0;
    for (int i = 0; !rc && i < m.n; i++) {
        if (strcmp(m.f[i].path, "kernel.elf") && strcmp(m.f[i].path, "initrd.img")) continue;
        seen++;
        if (!installed_same(&e, &m.f[i])) { snprintf(why, wn, "%s on the ESP is not the one the manifest signs", m.f[i].path); rc = -1; }
    }
    if (!rc && seen != 2) { snprintf(why, wn, "the manifest does not list kernel.elf and initrd.img"); rc = -1; }
    ro_close(&e);
    return rc ? -1 : 0;
}

int upd_cmd_autoconfirm(int argc, char **argv) {
    char url[MAX_URL];
    if (!upd_cmdline_word("update.pull=", url, sizeof url) || !url_ok(url)) { kb_error("autoconfirm needs update.pull=URL on the command line"); return 2; }
    long secs = argc > 2 ? atol(argv[2]) : word_seconds("update.autoconfirm=", 180);
    if (secs < 0) { psay("update.autoconfirm=off: a person confirms this start"); return 0; }
    gap_setname("update");
    sleep((unsigned)secs);
    char why[600];
    int r = -1;
    for (int tries = 0; tries < 3 && r; tries++) {               /* (three tries a few seconds apart: one lost answer must not restart a good system) */
        if (tries) sleep(10);
        r = health(url, why, sizeof why);
    }
    if (r) {
        psay("self-confirmation: not confirming (%s); the watchdog restarts the machine unless somebody confirms", why);
        return 1;
    }
    psay("self-confirmation: the new files fetched and verified the signed manifest again through their own network stack");
    if (upd_confirm()) return 1;
    char then[16];
    if (upd_cmdline_word("update.pull.then=", then, sizeof then) && !strcmp(then, "poweroff")) power_off("update.pull.then=poweroff: the update is confirmed");
    return 0;
}

int upd_cmd_auto(int argc, char **argv) {
    (void)argc; (void)argv;
    char url[MAX_URL], then[16] = "";
    if (!upd_cmdline_word("update.pull=", url, sizeof url) || !url_ok(url)) { kb_error("update auto: no usable update.pull=URL on the command line"); return 2; }
    upd_cmdline_word("update.pull.then=", then, sizeof then);
    int off = !strcmp(then, "poweroff");
    gap_setname("update");
    if (upd_cmdline_word("update.fellback", NULL, 0)) { psay("update.pull: this start fell back to the previous files: not pulling (update status)"); return 0; }
    if (upd_cmdline_word("update.trial=", NULL, 0)) {
        char *args[] = { "update", "autoconfirm", NULL };
        return upd_cmd_autoconfirm(2, args);
    }
    long delay = word_seconds("update.pull.delay=", 90), interval = word_seconds("update.pull.interval=", 21600);
    if (delay < 0) delay = 90;                                           /* ("off" means something for update.autoconfirm only) */
    if (interval < 0) interval = 21600;
    if (interval > 0 && interval < MIN_INTERVAL) {                       /* a typo (interval=1) must not make the machine ask the server every second for ever */
        psay("update.pull.interval=%ld is raised to %d seconds (0 means once per start)", interval, MIN_INTERVAL);
        interval = MIN_INTERVAL;
    }
    if (delay > 0) sleep((unsigned)delay);
    PullOpts po = { .reboot_in = 3 };
    for (;;) {
        int r = do_pull(url, &po);
        if (r == PULL_INSTALLED) return 0;                               /* the restart is on its way */
        if (off) power_off("update.pull.then=poweroff: nothing was installed");
        if (interval <= 0) return status_of(r);
        sleep((unsigned)interval);
    }
}
