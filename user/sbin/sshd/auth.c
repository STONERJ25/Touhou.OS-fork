/*
 * Who may come in (RFC 4252), and the key that tells the visitor this is the right house.
 *
 * Host key: one ssh-ed25519 key, kept as 64 hex digits in /HumanVillage/.system/ssh_host_ed25519_key (root's, 0600: the FAT driver makes everything
 * under .system root's). Without a volume the key lives in RAM and changes at every boot, which the client's known_hosts will complain about; the log
 * says so. The fingerprint is logged in the format ssh-keygen -l prints, for the first connection of a client.
 *
 * Login: the "publickey" method only. A key is accepted when it is listed in /etc/ssh/authorized_keys/NAME (put there by the image, root's) or in
 * ~NAME/.ssh/authorized_keys. Only ssh-ed25519 lines are read; a line with options in front is ignored, because options cannot be honoured here.
 * root never logs in by ssh: come in as a resident with sudo.
 */
#include "sshd.h"
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <crypto.h>
#include <etcstore.h>
#include <gensokyo.h>
#include <sshcrypto.h>

#define HOSTKEY_VOLUME_DIR "/HumanVillage/.system"
#define HOSTKEY_VOLUME     HOSTKEY_VOLUME_DIR "/ssh_host_ed25519_key"
#define HOSTKEY_RAM        "/etc/ssh_host_ed25519_key"
#define KEYS_DIR           "/etc/ssh/authorized_keys"

uint8_t g_host_pk[32];
uint8_t g_host_blob[51];
char g_host_fp[64];
static uint8_t g_host_seed[32];

/* ------------------------------------------------------------------------------------------------ host key */

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 1 read, 0 there is no such file, -1 there is one and it cannot be used (then it is NOT replaced: a damaged key file is for a person to look at, and a silently
 * new host key would make every client's known_hosts cry wolf). */
static int read_seed(const char *path, uint8_t seed[32]) {
    char buf[128];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    ssize_t n = read(fd, buf, sizeof buf);
    close(fd);
    bool ok = n >= 64;
    for (int i = 0; ok && i < 32; i++) {
        int hi = hexval(buf[2 * i]), lo = hexval(buf[2 * i + 1]);
        if (hi < 0 || lo < 0) ok = false;
        else seed[i] = (uint8_t)(hi << 4 | lo);
    }
    memset(buf, 0, sizeof buf);
    return ok ? 1 : -1;
}

static bool write_seed(const char *path, const uint8_t seed[32]) {
    char text[66];
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { text[2 * i] = hex[seed[i] >> 4]; text[2 * i + 1] = hex[seed[i] & 15]; }
    text[64] = '\n';
    return etc_write_atomic(path, text, 65, 0600, 1) == 0;
}

static void derive_public(void) {
    ed25519_pubkey(g_host_pk, g_host_seed);
    Wr w;
    wr_init(&w, g_host_blob, sizeof g_host_blob);
    wr_cstr(&w, "ssh-ed25519");
    wr_str(&w, g_host_pk, 32);
    uint8_t d[32];
    sha256_ctx h;
    sha256_init(&h);
    sha256_update(&h, g_host_blob, sizeof g_host_blob);
    sha256_final(&h, d);
    memcpy(g_host_fp, "SHA256:", 7);
    b64_encode(d, 32, g_host_fp + 7, false);
}

void host_key_init(void) {
    int ro = 0;
    bool volume = etc_volume_present(&ro) > 0;
    const char *how = NULL;
    int on_volume = volume ? read_seed(HOSTKEY_VOLUME, g_host_seed) : 0;
    if (on_volume > 0) how = "read from the HUMANVILLAGE volume";
    else if (read_seed(HOSTKEY_RAM, g_host_seed) > 0) how = "made earlier this boot (kept in RAM only)";
    if (!how) {
        random_bytes(g_host_seed, sizeof g_host_seed);
        if (on_volume < 0) {                                       /* the file is there and unusable: leave it alone, run on a key of RAM */
            write_seed(HOSTKEY_RAM, g_host_seed);
            how = "NEW and kept in RAM only: " HOSTKEY_VOLUME " exists but cannot be read as a key, so it is left as it is (remove it to get a new permanent key)";
        } else if (volume && !ro) {
            mkdir(HOSTKEY_VOLUME_DIR, 0700);
            if (write_seed(HOSTKEY_VOLUME, g_host_seed)) how = "new, kept on the HUMANVILLAGE volume";
        }
        if (!how) {
            write_seed(HOSTKEY_RAM, g_host_seed);
            how = !volume ? "new, kept in RAM only: there is no HUMANVILLAGE volume, so the key changes at every boot"
                          : "new, kept in RAM only: the HUMANVILLAGE volume is read-only, so the key changes at every boot";
        }
    }
    derive_public();
    slog("host key ssh-ed25519 %s (%s)", g_host_fp, how);
}

void host_key_sign(uint8_t sig[64], const uint8_t *msg, size_t n) { ed25519_sign(sig, msg, n, g_host_seed, g_host_pk); }

void host_key_forget(void) { memset(g_host_seed, 0, sizeof g_host_seed); }

/* ------------------------------------------------------------------------------------------------ authorized_keys */

static char g_keyfile[65536];

/* Is the key (blob) listed in the file at `path`? `owner_ok` says whose files count. */
static bool listed_in(const char *path, const Account *a, bool root_only, const uint8_t *blob, size_t blob_len) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW);                       /* not through a link, and what is checked below is the file that is read (no swap in between) */
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return false; }
    if (st.st_uid != 0 && (root_only || (int)st.st_uid != a->uid)) {
        slog("%s is owned by uid %d: not used", path, (int)st.st_uid);
        close(fd);
        return false;
    }
    if (st.st_mode & 022) {
        slog("%s can be written by others (mode %o): not used", path, (unsigned)(st.st_mode & 0777));
        close(fd);
        return false;
    }
    ssize_t n = read(fd, g_keyfile, sizeof g_keyfile - 1);
    close(fd);
    if (n <= 0) return false;
    g_keyfile[n] = 0;
    for (char *line = g_keyfile; *line;) {
        char *eol = strchr(line, '\n');
        if (eol) *eol = 0;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, "ssh-ed25519", 11) && (p[11] == ' ' || p[11] == '\t')) {
            p += 11;
            while (*p == ' ' || *p == '\t') p++;
            size_t len = 0;
            while (p[len] && p[len] != ' ' && p[len] != '\t' && p[len] != '\r') len++;
            uint8_t k[128];
            int got = b64_decode(p, len, k, sizeof k);
            if (got == (int)blob_len && !memcmp(k, blob, blob_len)) return true;
        }
        if (!eol) break;
        line = eol + 1;
    }
    return false;
}

static bool key_authorized(const Account *a, const uint8_t *blob, size_t blob_len) {
    char path[160];
    snprintf(path, sizeof path, KEYS_DIR "/%s", a->name);
    if (listed_in(path, a, true, blob, blob_len)) return true;
    snprintf(path, sizeof path, "%s/.ssh/authorized_keys", a->home);
    return listed_in(path, a, false, blob, blob_len);
}

/* Is there a resident of that name who may log in by ssh? Never root, and the shell has to exist. */
static bool lookup_account(const uint8_t *name, size_t n, Account *a) {
    char nm[32];
    if (n == 0 || n >= sizeof nm) return false;
    memcpy(nm, name, n);
    nm[n] = 0;
    if (strlen(nm) != n) return false;
    struct passwd *p = getpwnam(nm);
    if (!p || p->pw_uid == 0) return false;
    snprintf(a->name, sizeof a->name, "%s", p->pw_name);
    a->uid = (int)p->pw_uid;
    a->gid = (int)p->pw_gid;
    snprintf(a->home, sizeof a->home, "%s", p->pw_dir);
    snprintf(a->shell, sizeof a->shell, "%s", p->pw_shell);
    return access(a->shell, X_OK) == 0;
}

/* ------------------------------------------------------------------------------------------------ the method */

static void send_failure(Conn *c) {
    uint8_t buf[32];
    Wr w;
    wr_init(&w, buf, sizeof buf);
    wr_u8(&w, MSG_USERAUTH_FAILURE);
    wr_cstr(&w, "publickey");
    wr_bool(&w, false);
    send_msg(c, buf, w.n);
}

/* A key blob is string "ssh-ed25519", string key(32). */
static bool parse_key_blob(const uint8_t *blob, size_t n, const uint8_t **pk) {
    Rd r;
    rd_init(&r, blob, n);
    size_t tl, kl;
    const uint8_t *t = rd_str(&r, &tl);
    const uint8_t *k = rd_str(&r, &kl);
    if (r.bad || r.n || !str_is(t, tl, "ssh-ed25519") || kl != 32) return false;
    *pk = k;
    return true;
}

int auth_run(Conn *c, Account *who) {
    const uint8_t *pl;
    int n = next_msg(c, &pl);
    if (n <= 0) return -1;
    Rd r;
    rd_init(&r, pl, (size_t)n);
    size_t sl;
    if (rd_u8(&r) != MSG_SERVICE_REQUEST) {
        send_disconnect(c, DISC_PROTOCOL_ERROR, "expected a service request");
        return -1;
    }
    const uint8_t *svc = rd_str(&r, &sl);
    if (r.bad || !str_is(svc, sl, "ssh-userauth")) {
        send_disconnect(c, DISC_SERVICE_NOT_AVAILABLE, "only ssh-userauth is available before the login");
        return -1;
    }
    uint8_t accept[32];
    Wr w;
    wr_init(&w, accept, sizeof accept);
    wr_u8(&w, MSG_SERVICE_ACCEPT);
    wr_cstr(&w, "ssh-userauth");
    if (send_msg(c, accept, w.n) < 0) return -1;

    int failures = 0, requests = 0, nones = 0;
    for (;;) {
        n = next_msg(c, &pl);
        if (n <= 0) return -1;
        rd_init(&r, pl, (size_t)n);
        if (rd_u8(&r) != MSG_USERAUTH_REQUEST) {
            send_disconnect(c, DISC_PROTOCOL_ERROR, "expected a user authentication request");
            return -1;
        }
        if (++requests > MAX_AUTH_REQUESTS) {            /* queries about keys that fail nothing used to be free: now every request counts */
            slog("%s: more than %d authentication requests", c->peer, MAX_AUTH_REQUESTS);
            send_disconnect(c, DISC_NO_MORE_AUTH_METHODS, "too many authentication requests");
            return -1;
        }
        size_t ul, vl, ml;
        const uint8_t *user = rd_str(&r, &ul), *service = rd_str(&r, &vl), *method = rd_str(&r, &ml);
        if (r.bad) { send_disconnect(c, DISC_PROTOCOL_ERROR, "malformed request"); return -1; }
        char shown[64];
        printable(shown, sizeof shown, user, ul);

        if (!str_is(method, ml, "publickey") || !str_is(service, vl, "ssh-connection")) {
            if (!str_is(method, ml, "none")) {
                char how[32];
                printable(how, sizeof how, method, ml);
                slog("%s: %s for %s is not offered", c->peer, how, shown);
                failures++;
            } else if (++nones > 1) {                    /* "none" is how a client asks which methods exist: once is a question, again is noise */
                slog("%s: the method none asked for twice", c->peer);
                failures++;
            }
        } else {
            bool has_sig = rd_bool(&r);
            size_t al, bl;
            const uint8_t *alg = rd_str(&r, &al), *blob = rd_str(&r, &bl);
            if (r.bad) { send_disconnect(c, DISC_PROTOCOL_ERROR, "malformed request"); return -1; }
            const uint8_t *pk = NULL;
            Account acct;
            memset(&acct, 0, sizeof acct);
            bool listed = str_is(alg, al, "ssh-ed25519") && parse_key_blob(blob, bl, &pk) && lookup_account(user, ul, &acct) &&
                          key_authorized(&acct, blob, bl);
            if (!has_sig) {
                if (listed) {
                    uint8_t ok[128];
                    wr_init(&w, ok, sizeof ok);
                    wr_u8(&w, MSG_USERAUTH_PK_OK);
                    wr_str(&w, alg, al);
                    wr_str(&w, blob, bl);
                    if (w.bad || send_msg(c, ok, w.n) < 0) return -1;
                    continue;
                }
            } else {
                size_t gl;
                const uint8_t *sigblob = rd_str(&r, &gl);
                Rd sr;
                rd_init(&sr, sigblob, gl);
                size_t tl, sgl;
                const uint8_t *st = rd_str(&sr, &tl), *sg = rd_str(&sr, &sgl);
                bool sig_ok = false;
                if (!r.bad && !sr.bad && listed && str_is(st, tl, "ssh-ed25519") && sgl == 64) {
                    static uint8_t signed_data[512];
                    Wr m;
                    wr_init(&m, signed_data, sizeof signed_data);
                    wr_str(&m, c->session_id, 32);
                    wr_u8(&m, MSG_USERAUTH_REQUEST);
                    wr_str(&m, user, ul);
                    wr_str(&m, service, vl);
                    wr_str(&m, method, ml);
                    wr_bool(&m, true);
                    wr_str(&m, alg, al);
                    wr_str(&m, blob, bl);
                    sig_ok = !m.bad && ed25519_verify(sg, signed_data, m.n, pk) == 0;
                }
                if (sig_ok) {
                    uint8_t d[32];
                    char fp[64];
                    sha256_ctx h;
                    sha256_init(&h);
                    sha256_update(&h, blob, bl);
                    sha256_final(&h, d);
                    b64_encode(d, 32, fp, false);
                    slog("accepted publickey for %s from %s port %u: ED25519 SHA256:%s", acct.name, c->peer, c->peer_port, fp);
                    uint8_t ok = MSG_USERAUTH_SUCCESS;
                    if (send_msg(c, &ok, 1) < 0) return -1;
                    *who = acct;
                    c->deadline = 0;
                    return 0;
                }
            }
            slog("failed publickey for %s from %s port %u (%s)", shown, c->peer, c->peer_port,
                 listed ? "bad signature" : "no such resident, or the key is not authorized");
            failures++;
        }
        send_failure(c);
        if (failures >= MAX_AUTH_TRIES) {
            slog("%s: too many authentication failures", c->peer);
            send_disconnect(c, DISC_NO_MORE_AUTH_METHODS, "too many authentication failures");
            return -1;
        }
    }
}
