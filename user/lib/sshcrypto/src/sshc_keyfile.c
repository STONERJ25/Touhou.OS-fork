/*
 * Private key files in OpenSSH's own format (PROTOCOL.key in the OpenSSH sources), the only format ssh-keygen has written by default since 2018:
 *
 *   -----BEGIN OPENSSH PRIVATE KEY-----
 *   base64 of:  "openssh-key-v1\0"
 *               string ciphername   ("none" or "aes256-ctr" ...)
 *               string kdfname      ("none" or "bcrypt")
 *               string kdfoptions   (empty, or: string salt, uint32 rounds)
 *               uint32 number of keys (1)
 *               string public key blob (in the clear, so a login can offer it before the passphrase is asked for)
 *               string private section, encrypted: uint32 check, uint32 check (equal: a wrong passphrase shows up here),
 *                      string "ssh-ed25519", string public key, string secret (seed followed by the public key), string comment,
 *                      then padding bytes 1, 2, 3 ... up to the cipher's block size
 *   -----END OPENSSH PRIVATE KEY-----
 *
 * The key for the cipher comes from bcrypt_pbkdf(passphrase, salt, rounds): key bytes then IV bytes (48 for aes256-ctr; 64 and no IV for
 * chacha20-poly1305@openssh.com, which appends a 16-byte tag after the encrypted section).
 *
 * Only ed25519 keys are used by this client; a file holding another type parses (so the program can say what it is) but cannot be unlocked.
 */
#include <sshc.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sshcrypto.h>

static const char BEGIN[] = "-----BEGIN OPENSSH PRIVATE KEY-----";
static const char END[] = "-----END OPENSSH PRIVATE KEY-----";
static const char MAGIC[] = "openssh-key-v1";           /* followed by a NUL on the wire */

char *sshc_read_file(const char *path, size_t max, size_t *len, bool check_mode, const char **why) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { if (why) *why = errno == ENOENT ? "no such file" : "cannot be opened"; return NULL; }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); if (why) *why = "not a regular file"; return NULL; }
    if (check_mode && (st.st_mode & 077) != 0) {            /* the check is on the open descriptor, so what was checked is what is read */
        close(fd);
        if (why) *why = "permissions are too open: a private key must not be accessible by group or others";
        return NULL;
    }
    if ((uint64_t)st.st_size > max) { close(fd); if (why) *why = "file is too large"; return NULL; }
    size_t cap = (size_t)st.st_size + 1, got = 0;
    char *buf = malloc(cap + 1);
    if (!buf) { close(fd); if (why) *why = "out of memory"; return NULL; }
    for (;;) {
        if (got == cap) {                                   /* the size lied (a /proc file, a growing file): make room, within the limit */
            if (cap >= max) break;
            cap = cap * 2 > max ? max : cap * 2;
            char *nb = realloc(buf, cap + 1);
            if (!nb) { free(buf); close(fd); if (why) *why = "out of memory"; return NULL; }
            buf = nb;
        }
        ssize_t r = read(fd, buf + got, cap - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            free(buf); close(fd); if (why) *why = "read error";
            return NULL;
        }
        if (r == 0) break;
        got += (size_t)r;
    }
    close(fd);
    buf[got] = 0;
    if (len) *len = got;
    return buf;
}

/* ------------------------------------------------------------------------------------------------ parsing */

void sshc_keyfile_free(SshcKeyFile *kf) {
    if (kf->priv) { memset(kf->priv, 0, kf->priv_len); free(kf->priv); }
    memset(kf, 0, sizeof *kf);
}

static size_t authlen_of(const char *cipher) { return !strcmp(cipher, "chacha20-poly1305@openssh.com") ? 16 : 0; }

static bool contains(const char *text, size_t n, const char *needle) {
    size_t nl = strlen(needle);
    for (size_t i = 0; i + nl <= n; i++)
        if (!memcmp(text + i, needle, nl)) return true;
    return false;
}

int sshc_keyfile_parse(const char *text, size_t n, SshcKeyFile *kf, const char **err) {
    const char *e = "not an OpenSSH private key";
    memset(kf, 0, sizeof *kf);
    const char *b = NULL, *x = NULL;
    for (size_t i = 0; i + sizeof BEGIN - 1 <= n; i++)
        if (!memcmp(text + i, BEGIN, sizeof BEGIN - 1)) { b = text + i + sizeof BEGIN - 1; break; }
    if (!b) {
        if (contains(text, n, "-----BEGIN RSA PRIVATE KEY-----") || contains(text, n, "-----BEGIN EC PRIVATE KEY-----") ||
            contains(text, n, "-----BEGIN PRIVATE KEY-----"))
            e = "this is a key in the older PEM format: only OpenSSH-format ed25519 keys are supported (ssh-keygen -t ed25519 makes one)";
        goto fail;
    }
    for (const char *p = b; p + sizeof END - 1 <= text + n; p++)
        if (!memcmp(p, END, sizeof END - 1)) { x = p; break; }
    if (!x) { e = "the key file is cut off (no END line)"; goto fail; }
    char *bin = malloc((size_t)(x - b) + 4);
    if (!bin) { e = "out of memory"; goto fail; }
    size_t bl = 0;
    {                                                        /* base64 without the line breaks and blanks */
        char *clean = malloc((size_t)(x - b) + 1);
        if (!clean) { free(bin); e = "out of memory"; goto fail; }
        size_t cl = 0;
        for (const char *p = b; p < x; p++)
            if (*p != '\n' && *p != '\r' && *p != ' ' && *p != '\t') clean[cl++] = *p;
        int got = sshc_b64_decode(clean, cl, (uint8_t *)bin, (size_t)(x - b) + 3);
        free(clean);
        if (got < 0) { free(bin); e = "the key file is not valid base64"; goto fail; }
        bl = (size_t)got;
    }
    SshcRd r;
    sshc_rd_init(&r, bin, bl);
    if (bl < sizeof MAGIC || memcmp(bin, MAGIC, sizeof MAGIC) != 0) { free(bin); e = "not an openssh-key-v1 file"; goto fail; }
    sshc_rd_skip(&r, sizeof MAGIC);
    size_t cl, kl, ol, pl, sl;
    const uint8_t *cipher = sshc_rd_str(&r, &cl), *kdf = sshc_rd_str(&r, &kl), *opts = sshc_rd_str(&r, &ol);
    uint32_t nkeys = sshc_rd_u32(&r);
    const uint8_t *pub = sshc_rd_str(&r, &pl);
    if (r.bad || cl >= sizeof kf->cipher || kl >= sizeof kf->kdf || nkeys != 1) { free(bin); e = "the key file is damaged, or holds more than one key"; goto fail; }
    memcpy(kf->cipher, cipher, cl);
    memcpy(kf->kdf, kdf, kl);
    if (sshc_pub_from_blob(pub, pl, &kf->pub) != 0) { free(bin); e = "the public key in the file is damaged"; goto fail; }
    kf->encrypted = strcmp(kf->cipher, "none") != 0;
    if (kf->encrypted) {
        SshcRd o;
        sshc_rd_init(&o, opts, ol);
        const uint8_t *salt = sshc_rd_str(&o, &sl);
        kf->rounds = sshc_rd_u32(&o);
        if (o.bad || sl > sizeof kf->salt || sl == 0 || kf->rounds == 0 || kf->rounds > 1000000) {
            if (strcmp(kf->kdf, "bcrypt") != 0) { free(bin); e = "the key is protected in a way this client does not know"; goto fail; }
            free(bin); e = "the key file's passphrase settings are damaged"; goto fail;
        }
        memcpy(kf->salt, salt, sl);
        kf->salt_len = sl;
    } else if (strcmp(kf->kdf, "none") != 0) {
        free(bin); e = "the key file is damaged (a kdf without a cipher)"; goto fail;
    }
    uint32_t enc_len = sshc_rd_u32(&r);
    size_t total = (size_t)enc_len + authlen_of(kf->cipher);
    if (r.bad || total > r.n || enc_len < 16) { free(bin); e = "the key file is damaged (the private section is cut off)"; goto fail; }
    kf->priv = malloc(total);
    if (!kf->priv) { free(bin); e = "out of memory"; goto fail; }
    memcpy(kf->priv, r.p, total);
    kf->priv_len = total;
    memset(bin, 0, bl);
    free(bin);
    return 0;
fail:
    if (err) *err = e;
    sshc_keyfile_free(kf);
    return -1;
}

/* ------------------------------------------------------------------------------------------------ unlocking */

typedef struct { const char *name; int keylen, ivlen; } CipherInfo;
static const CipherInfo CIPHERS[] = {
    { "aes256-ctr", 32, 16 }, { "aes192-ctr", 24, 16 }, { "aes128-ctr", 16, 16 }, { "chacha20-poly1305@openssh.com", 64, 0 },
};

static const CipherInfo *cipher_info(const char *name) {
    for (size_t i = 0; i < sizeof CIPHERS / sizeof CIPHERS[0]; i++)
        if (!strcmp(CIPHERS[i].name, name)) return &CIPHERS[i];
    return NULL;
}

/* Parses the decrypted private section. 0, -1 damaged, -2 the two check numbers differ (wrong passphrase). */
static int parse_private(const uint8_t *p, size_t n, const SshcKeyFile *kf, SshcEd25519 *key, const char **err) {
    SshcRd r;
    sshc_rd_init(&r, p, n);
    uint32_t c1 = sshc_rd_u32(&r), c2 = sshc_rd_u32(&r);
    if (r.bad || c1 != c2) { *err = "incorrect passphrase supplied to decrypt private key"; return -2; }
    size_t tl, pkl, skl, cml;
    const uint8_t *type = sshc_rd_str(&r, &tl);
    if (r.bad || !sshc_str_is(type, tl, "ssh-ed25519")) { *err = "this client only uses ed25519 keys"; return -1; }
    const uint8_t *pk = sshc_rd_str(&r, &pkl), *sk = sshc_rd_str(&r, &skl), *cm = sshc_rd_str(&r, &cml);
    if (r.bad || pkl != 32 || skl != 64) { *err = "the key file is damaged (bad ed25519 key)"; return -1; }
    for (size_t i = 0; i < r.n; i++)                         /* the padding is 1, 2, 3, ... */
        if (r.p[i] != (uint8_t)(i + 1)) { *err = "the key file is damaged (bad padding)"; return -1; }
    uint8_t derived[32];
    ed25519_pubkey(derived, sk);
    uint8_t blobpk[32];
    if (!sshc_pub_ed25519(&kf->pub, blobpk) || memcmp(derived, pk, 32) != 0 || memcmp(sk + 32, pk, 32) != 0 || memcmp(blobpk, pk, 32) != 0) {
        *err = "the key file is damaged (the public and private halves disagree)";
        return -1;
    }
    memcpy(key->seed, sk, 32);
    memcpy(key->pk, pk, 32);
    size_t cc = cml < sizeof key->comment - 1 ? cml : sizeof key->comment - 1;
    sshc_printable(key->comment, cc + 1, cm, cc, false);
    return 0;
}

int sshc_keyfile_unlock(const SshcKeyFile *kf, const char *pass, SshcEd25519 *key, const char **err) {
    const char *dummy;
    if (!err) err = &dummy;
    if (strcmp(kf->pub.type, "ssh-ed25519") != 0) { *err = "this client only uses ed25519 keys"; return -1; }
    size_t enc_len = kf->priv_len - authlen_of(kf->cipher);
    uint8_t *plain = malloc(enc_len);
    if (!plain) { *err = "out of memory"; return -1; }
    int rc;
    if (!kf->encrypted) {
        memcpy(plain, kf->priv, enc_len);
    } else {
        const CipherInfo *ci = cipher_info(kf->cipher);
        if (!ci || strcmp(kf->kdf, "bcrypt") != 0) { free(plain); *err = "the key is protected with a cipher this client does not have"; return -1; }
        if (!pass) { free(plain); *err = "a passphrase is needed"; return -2; }
        uint8_t material[64 + 16];
        sshc_bcrypt_pbkdf((const uint8_t *)pass, strlen(pass), kf->salt, kf->salt_len, material, (size_t)(ci->keylen + ci->ivlen), kf->rounds);
        if (ci->ivlen) {                                    /* aes-ctr */
            SshcAes aes;
            uint8_t iv[16];
            sshc_aes_init(&aes, material, ci->keylen);
            memcpy(iv, material + ci->keylen, 16);
            if (enc_len % 16) { free(plain); *err = "the key file is damaged (the private section is not a whole number of blocks)"; return -1; }
            sshc_aes_ctr(&aes, iv, kf->priv, plain, enc_len);
            memset(&aes, 0, sizeof aes);
        } else {                                            /* chacha20-poly1305@openssh.com with sequence number 0, no associated data */
            static const uint8_t nonce[8] = { 0 };
            uint8_t pk[32], zero[32] = { 0 }, tag[16];
            chacha20_xor(pk, zero, 32, material, nonce, 0);
            poly1305_mac(tag, kf->priv, enc_len, pk);
            if (!sshc_ct_equal(tag, kf->priv + enc_len, 16)) {
                memset(material, 0, sizeof material); free(plain);
                *err = "incorrect passphrase supplied to decrypt private key";
                return -2;
            }
            chacha20_xor(plain, kf->priv, enc_len, material, nonce, 1);
        }
        memset(material, 0, sizeof material);
    }
    rc = parse_private(plain, enc_len, kf, key, err);
    memset(plain, 0, enc_len);
    free(plain);
    return rc;
}

/* ------------------------------------------------------------------------------------------------ writing */

int sshc_keyfile_make(const SshcEd25519 *key, const char *pass, uint32_t rounds, char **text) {
    bool enc = pass && pass[0];
    const CipherInfo *ci = cipher_info("aes256-ctr");
    size_t block = enc ? 16 : 8;
    uint8_t pub[51];
    sshc_ed25519_blob(key->pk, pub);
    size_t comment_len = strlen(key->comment);
    size_t priv_cap = 8 + 4 + 11 + 4 + 32 + 4 + 64 + 4 + comment_len + block + 8;
    uint8_t *priv = malloc(priv_cap);
    if (!priv) return -1;
    SshcWr w;
    sshc_wr_init(&w, priv, priv_cap);
    uint8_t check[4];
    sshc_random(check, 4);
    sshc_wr_raw(&w, check, 4);
    sshc_wr_raw(&w, check, 4);
    sshc_wr_cstr(&w, "ssh-ed25519");
    sshc_wr_str(&w, key->pk, 32);
    uint8_t sk[64];
    memcpy(sk, key->seed, 32);
    memcpy(sk + 32, key->pk, 32);
    sshc_wr_str(&w, sk, 64);
    sshc_wr_cstr(&w, key->comment);
    for (uint8_t pad = 1; w.n % block; pad++) sshc_wr_u8(&w, pad);
    if (w.bad) { free(priv); return -1; }
    uint8_t salt[16];
    if (enc) {
        uint8_t material[48], iv[16];
        sshc_random(salt, sizeof salt);
        sshc_bcrypt_pbkdf((const uint8_t *)pass, strlen(pass), salt, sizeof salt, material, sizeof material, rounds);
        SshcAes aes;
        sshc_aes_init(&aes, material, ci->keylen);
        memcpy(iv, material + ci->keylen, 16);
        sshc_aes_ctr(&aes, iv, priv, priv, w.n);
        memset(&aes, 0, sizeof aes);
        memset(material, 0, sizeof material);
    }
    size_t bin_cap = sizeof MAGIC + 4 + 10 + 4 + 6 + 4 + 4 + 16 + 4 + 4 + 4 + sizeof pub + 4 + w.n + 16;
    uint8_t *bin = malloc(bin_cap);
    if (!bin) { free(priv); return -1; }
    SshcWr o;
    sshc_wr_init(&o, bin, bin_cap);
    sshc_wr_raw(&o, MAGIC, sizeof MAGIC);                   /* with its NUL */
    sshc_wr_cstr(&o, enc ? "aes256-ctr" : "none");
    sshc_wr_cstr(&o, enc ? "bcrypt" : "none");
    if (enc) {
        uint8_t opts[4 + 16 + 4];
        SshcWr ow;
        sshc_wr_init(&ow, opts, sizeof opts);
        sshc_wr_str(&ow, salt, sizeof salt);
        sshc_wr_u32(&ow, rounds);
        sshc_wr_str(&o, opts, ow.n);
    } else {
        sshc_wr_str(&o, "", 0);
    }
    sshc_wr_u32(&o, 1);
    sshc_wr_str(&o, pub, sizeof pub);
    sshc_wr_str(&o, priv, w.n);
    memset(priv, 0, priv_cap);
    free(priv);
    if (o.bad) { memset(bin, 0, bin_cap); free(bin); return -1; }
    size_t b64_len = (o.n + 2) / 3 * 4;
    size_t out_cap = sizeof BEGIN + sizeof END + b64_len + b64_len / 70 + 16;
    char *b64 = malloc(b64_len + 1), *out = malloc(out_cap);
    if (!b64 || !out) { free(b64); free(out); memset(bin, 0, bin_cap); free(bin); return -1; }
    sshc_b64_encode(bin, o.n, b64, true);
    memset(bin, 0, bin_cap);
    free(bin);
    size_t pos = (size_t)snprintf(out, out_cap, "%s\n", BEGIN);
    for (size_t i = 0; i < b64_len; i += 70) {
        size_t take = b64_len - i < 70 ? b64_len - i : 70;
        memcpy(out + pos, b64 + i, take);
        pos += take;
        out[pos++] = '\n';
    }
    pos += (size_t)snprintf(out + pos, out_cap - pos, "%s\n", END);
    free(b64);
    *text = out;
    return 0;
}
