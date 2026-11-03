/*
 * tlstool: the host-side driver of tests/host/test_tls_vectors.py. It is built with gcc and AddressSanitizer/UBSan from the
 * sources of user/lib/tls (and user/lib/sshcrypto, user/lib/http, user/lib/z), reads one command per line on stdin and prints
 * results as text. Hex is used for all binary arguments ("-" is an empty one).
 *
 *   primitives  hash ALG HEX | hmac ALG KEY DATA | hkdf_extract ALG SALT IKM | hkdf_expand ALG PRK INFO LEN
 *               hkdf_label ALG SECRET LABEL CTX LEN | prf ALG SECRET LABEL SEED LEN | chacha KEY NONCE CTR DATA
 *               poly1305 KEY DATA | cp_seal KEY NONCE AAD PT | cp_open KEY NONCE AAD CTTAG | aes MODE KEY BLOCK
 *               gcm_seal MODE KEY IV AAD PT | gcm_open MODE KEY IV AAD CTTAG | x25519 SCALAR POINT | ecdh_pub CURVE PRIV
 *               ecdh CURVE PRIV PEER | ecdsa CURVE PUB HASH R S | rsa_pkcs1 N E ALG HASH SIG | rsa_pss N E ALG HASH SIG SALT
 *               ed25519 PUB MSG SIG | modmul M A B | modexp M A E | tables
 *   x509        cert DER | name DER HOST | trust_reset | trust_add DER | trust_file PATH | verify NOW HOST DER...
 *               fuzz_der SEED ITERATIONS DER...
 *   network     connect HOST PORT SNI [key=value ...]          (see do_connect)
 *   http        http_parse CHUNKSIZE HEX | url BASE REF | http_get URL [key=value ...]
 * ALG is 256, 384 or 512; MODE is sw or hw; CURVE is 256 or 384.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "tlsint.h"
#include "x509.h"
#include "conn.h"
#include "tables.h"
#include <sshcrypto.h>
#ifdef WITH_HTTP
#include <http.h>
#endif

/* ---------------------------------------------------------------------------------------------------- helpers */

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* decodes hex ("-" is empty) into a fresh buffer (at least 1 byte); *n gets the length */
static u8 *unhex(const char *s, size_t *n) {
    size_t len = strcmp(s, "-") == 0 ? 0 : strlen(s);
    u8 *out = malloc(len / 2 + 1);
    if (!out) exit(3);
    size_t k = 0;
    for (size_t i = 0; i + 1 < len; i += 2) {
        int a = hexval(s[i]), b = hexval(s[i + 1]);
        if (a < 0 || b < 0) { fprintf(stderr, "bad hex\n"); exit(2); }
        out[k++] = (u8)(a << 4 | b);
    }
    *n = k;
    return out;
}

static void puthex(const u8 *p, size_t n) {
    for (size_t i = 0; i < n; i++) printf("%02x", p[i]);
    printf("\n");
}

static int alg_of(const char *s) { return strcmp(s, "384") == 0 ? TLSC_SHA384 : strcmp(s, "512") == 0 ? TLSC_SHA512 : TLSC_SHA256; }
static int curve_of(const char *s) { return strcmp(s, "384") == 0 ? TLSC_P384 : TLSC_P256; }

#define MAXARG 64
static char *argv_[MAXARG];
static int argc_;

static void split(char *line) {
    argc_ = 0;
    for (char *p = strtok(line, " \t\r\n"); p && argc_ < MAXARG; p = strtok(NULL, " \t\r\n")) argv_[argc_++] = p;
}

static const char *arg(int i) { return i < argc_ ? argv_[i] : "-"; }

/* deterministic "random" numbers for reproducible handshakes (xorshift64*) */
static void det_random(void *ctx, void *buf, size_t n) {
    unsigned long long *s = ctx;
    u8 *p = buf;
    for (size_t i = 0; i < n; i++) {
        *s ^= *s >> 12; *s ^= *s << 25; *s ^= *s >> 27;
        p[i] = (u8)((*s * 2685821657736338717ULL) >> 56);
    }
}

/* ---------------------------------------------------------------------------------------------------- primitives */

static void cmd_primitive(const char *cmd) {
    size_t n1, n2, n3, n4, n5;
    u8 out[1024];
    if (!strcmp(cmd, "hash")) {
        int alg = alg_of(arg(1));
        u8 *d = unhex(arg(2), &n1);
        tlsc_hash_once(alg, d, n1, out);
        puthex(out, tlsc_hash_len(alg));
        free(d);
    } else if (!strcmp(cmd, "hmac")) {
        int alg = alg_of(arg(1));
        u8 *k = unhex(arg(2), &n1), *d = unhex(arg(3), &n2);
        tlsc_hmac(alg, k, n1, d, n2, out);
        puthex(out, tlsc_hash_len(alg));
        free(k); free(d);
    } else if (!strcmp(cmd, "hkdf_extract")) {
        int alg = alg_of(arg(1));
        u8 *s = unhex(arg(2), &n1), *k = unhex(arg(3), &n2);
        tlsc_hkdf_extract(alg, n1 ? s : NULL, n1, k, n2, out);
        puthex(out, tlsc_hash_len(alg));
        free(s); free(k);
    } else if (!strcmp(cmd, "hkdf_expand")) {
        int alg = alg_of(arg(1));
        u8 *prk = unhex(arg(2), &n1), *info = unhex(arg(3), &n2);
        size_t len = (size_t)atoi(arg(4));
        u8 *o = malloc(len + 1);
        if (len > 255 * tlsc_hash_len(alg) || tlsc_hkdf_expand(alg, prk, n1, info, n2, o, len) != 0) printf("FAIL\n");
        else puthex(o, len);
        free(o); free(prk); free(info);
    } else if (!strcmp(cmd, "hkdf_label")) {                         /* the label is hex too (it has spaces) */
        int alg = alg_of(arg(1));
        u8 *sec = unhex(arg(2), &n1), *lab = unhex(arg(3), &n3), *ctx = unhex(arg(4), &n2);
        size_t len = (size_t)atoi(arg(5));
        char label[256];
        memcpy(label, lab, n3 < 255 ? n3 : 255);
        label[n3 < 255 ? n3 : 255] = 0;
        if (len > sizeof out || tlsc_hkdf_expand_label(alg, sec, label, n2 ? ctx : NULL, n2, out, len) != 0) printf("FAIL\n");
        else puthex(out, len);
        free(sec); free(ctx); free(lab);
    } else if (!strcmp(cmd, "prf")) {
        int alg = alg_of(arg(1));
        u8 *sec = unhex(arg(2), &n1), *lab = unhex(arg(3), &n3), *seed = unhex(arg(4), &n2);
        size_t len = (size_t)atoi(arg(5));
        char label[256];
        memcpy(label, lab, n3 < 255 ? n3 : 255);
        label[n3 < 255 ? n3 : 255] = 0;
        if (len > sizeof out) len = sizeof out;
        tlsc_prf(alg, sec, n1, label, seed, n2, NULL, 0, out, len);
        puthex(out, len);
        free(sec); free(seed); free(lab);
    } else if (!strcmp(cmd, "chacha")) {
        u8 *k = unhex(arg(1), &n1), *nonce = unhex(arg(2), &n2), *d = unhex(arg(4), &n3);
        u8 *o = malloc(n3 + 1);
        tlsc_chacha20_xor(k, nonce, (u32)strtoul(arg(3), NULL, 10), d, o, n3);
        puthex(o, n3);
        free(o); free(k); free(nonce); free(d);
    } else if (!strcmp(cmd, "poly1305")) {
        u8 *k = unhex(arg(1), &n1), *d = unhex(arg(2), &n2);
        tlsc_poly1305(k, d, n2, out);
        puthex(out, 16);
        free(k); free(d);
    } else if (!strcmp(cmd, "cp_seal") || !strcmp(cmd, "cp_open")) {
        u8 *k = unhex(arg(1), &n1), *nonce = unhex(arg(2), &n2), *aad = unhex(arg(3), &n3), *d = unhex(arg(4), &n4);
        u8 *o = malloc(n4 + 16 + 1);
        if (!strcmp(cmd, "cp_seal")) {
            tlsc_chacha20poly1305_seal(k, nonce, aad, n3, d, n4, o, o + n4);
            puthex(o, n4 + 16);
        } else if (n4 < 16 || tlsc_chacha20poly1305_open(k, nonce, aad, n3, d, n4 - 16, o, d + n4 - 16) != 0) {
            printf("FAIL\n");
        } else {
            puthex(o, n4 - 16);
        }
        free(o); free(k); free(nonce); free(aad); free(d);
    } else if (!strcmp(cmd, "aes")) {
        tlsc_aes_force_software(strcmp(arg(1), "sw") == 0);
        u8 *k = unhex(arg(2), &n1), *b = unhex(arg(3), &n2);
        tlsc_aes_key key;
        tlsc_aes_set_key(&key, k, n1);
        tlsc_aes_encrypt_block(&key, b, out);
        puthex(out, 16);
        tlsc_aes_force_software(0);
        free(k); free(b);
    } else if (!strcmp(cmd, "gcm_seal") || !strcmp(cmd, "gcm_open")) {
        tlsc_aes_force_software(strcmp(arg(1), "sw") == 0);
        u8 *k = unhex(arg(2), &n1), *iv = unhex(arg(3), &n2), *aad = unhex(arg(4), &n3), *d = unhex(arg(5), &n4);
        tlsc_gcm_ctx g;
        tlsc_gcm_init(&g, k, n1);
        u8 *o = malloc(n4 + 16 + 1);
        if (!strcmp(cmd, "gcm_seal")) {
            tlsc_gcm_seal(&g, iv, aad, n3, d, n4, o, o + n4);
            puthex(o, n4 + 16);
        } else if (n4 < 16 || tlsc_gcm_open(&g, iv, aad, n3, d, n4 - 16, o, d + n4 - 16) != 0) {
            printf("FAIL\n");
        } else {
            puthex(o, n4 - 16);
        }
        tlsc_aes_force_software(0);
        free(o); free(k); free(iv); free(aad); free(d);
    } else if (!strcmp(cmd, "aes_hw")) {
        printf("%d\n", tlsc_aes_hw_available());
    } else if (!strcmp(cmd, "x25519")) {
        u8 *s = unhex(arg(1), &n1), *p = unhex(arg(2), &n2);
        if (tlsc_x25519(out, s, p) != 0) printf("FAIL\n");
        else puthex(out, 32);
        free(s); free(p);
    } else if (!strcmp(cmd, "ecdh_pub")) {
        int curve = curve_of(arg(1));
        size_t sz = tlsc_ec_size(curve);
        u8 *priv = unhex(arg(2), &n1);
        u8 rnd[96] = { 0 }, pv[48], pub[97];
        memcpy(rnd + 2 * sz - n1, priv, n1);
        if (tlsc_ecdh_keypair(curve, rnd, pv, pub) != 0) printf("FAIL\n");
        else puthex(pub, 1 + 2 * sz);
        free(priv);
    } else if (!strcmp(cmd, "ecdh")) {
        int curve = curve_of(arg(1));
        u8 *priv = unhex(arg(2), &n1), *peer = unhex(arg(3), &n2);
        if (tlsc_ecdh(curve, priv, peer, n2, out) != 0) printf("FAIL\n");
        else puthex(out, tlsc_ec_size(curve));
        free(priv); free(peer);
    } else if (!strcmp(cmd, "ecdsa")) {
        int curve = curve_of(arg(1));
        u8 *pub = unhex(arg(2), &n1), *h = unhex(arg(3), &n2), *r = unhex(arg(4), &n3), *s = unhex(arg(5), &n4);
        printf("%s\n", tlsc_ecdsa_verify(curve, pub, n1, h, n2, r, n3, s, n4) == 0 ? "OK" : "FAIL");
        free(pub); free(h); free(r); free(s);
    } else if (!strcmp(cmd, "rsa_pkcs1") || !strcmp(cmd, "rsa_pss")) {
        u8 *nn = unhex(arg(1), &n1), *e = unhex(arg(2), &n2), *h = unhex(arg(4), &n3), *sig = unhex(arg(5), &n4);
        int alg = alg_of(arg(3)), rc;
        if (!strcmp(cmd, "rsa_pkcs1")) rc = tlsc_rsa_verify_pkcs1(nn, n1, e, n2, alg, h, sig, n4);
        else rc = tlsc_rsa_verify_pss(nn, n1, e, n2, alg, h, sig, n4, (size_t)atoi(arg(6)));
        printf("%s\n", rc == 0 ? "OK" : "FAIL");
        free(nn); free(e); free(h); free(sig);
    } else if (!strcmp(cmd, "ed25519")) {
        u8 *pub = unhex(arg(1), &n1), *m = unhex(arg(2), &n2), *sig = unhex(arg(3), &n3);
        printf("%s\n", n1 == 32 && n3 == 64 && ed25519_verify(sig, m, n2, pub) == 0 ? "OK" : "FAIL");
        free(pub); free(m); free(sig);
    } else if (!strcmp(cmd, "modmul") || !strcmp(cmd, "modexp")) {
        u8 *m = unhex(arg(1), &n1), *a = unhex(arg(2), &n2), *b = unhex(arg(3), &n3);
        size_t limbs = (n1 + 7) / 8;
        u64 mm[TLSC_BN_MAXLIMBS], aa[TLSC_BN_MAXLIMBS], bb[TLSC_BN_MAXLIMBS], r[TLSC_BN_MAXLIMBS];
        tlsc_mont c;
        tlsc_bn_from_bytes(mm, limbs, m, n1);
        tlsc_bn_from_bytes(aa, limbs, a, n2);
        if (tlsc_mont_init(&c, mm, limbs) != 0) {
            printf("FAIL\n");
        } else if (!strcmp(cmd, "modmul")) {
            tlsc_bn_from_bytes(bb, limbs, b, n3);
            tlsc_mont_to(&c, aa, aa);
            tlsc_mont_to(&c, bb, bb);
            tlsc_mont_mul(&c, r, aa, bb);
            tlsc_mont_from(&c, r, r);
            u8 o[TLSC_BN_MAXLIMBS * 8];
            tlsc_bn_to_bytes(o, n1, r, limbs);
            puthex(o, n1);
        } else {
            size_t el = (n3 + 7) / 8;
            u64 e[TLSC_BN_MAXLIMBS] = { 0 };
            tlsc_bn_from_bytes(e, el ? el : 1, b, n3);
            tlsc_mont_to(&c, aa, aa);
            tlsc_mont_pow(&c, r, aa, e, el ? el : 1);
            tlsc_mont_from(&c, r, r);
            u8 o[TLSC_BN_MAXLIMBS * 8];
            tlsc_bn_to_bytes(o, n1, r, limbs);
            puthex(o, n1);
        }
        free(m); free(a); free(b);
    } else if (!strcmp(cmd, "tables")) {
        printf("K256");
        for (int i = 0; i < 64; i++) printf(" %08x", TLSC_K256[i]);
        printf("\nK512");
        for (int i = 0; i < 80; i++) printf(" %016llx", (unsigned long long)TLSC_K512[i]);
        printf("\nH256");
        for (int i = 0; i < 8; i++) printf(" %08x", TLSC_H256[i]);
        printf("\nH384");
        for (int i = 0; i < 8; i++) printf(" %016llx", (unsigned long long)TLSC_H384[i]);
        printf("\nH512");
        for (int i = 0; i < 8; i++) printf(" %016llx", (unsigned long long)TLSC_H512[i]);
        printf("\nSBOX");
        for (int i = 0; i < 256; i++) printf(" %02x", TLSC_SBOX[i]);
        const u64 *v[10] = { TLSC_P256_P, TLSC_P256_B, TLSC_P256_N, TLSC_P256_GX, TLSC_P256_GY,
                             TLSC_P384_P, TLSC_P384_B, TLSC_P384_N, TLSC_P384_GX, TLSC_P384_GY };
        const char *names[10] = { "P256_P", "P256_B", "P256_N", "P256_GX", "P256_GY", "P384_P", "P384_B", "P384_N", "P384_GX", "P384_GY" };
        for (int k = 0; k < 10; k++) {
            int limbs = k < 5 ? 4 : 6;
            printf("\n%s ", names[k]);
            for (int i = limbs - 1; i >= 0; i--) printf("%016llx", (unsigned long long)v[k][i]);
        }
        printf("\n");
    } else {
        printf("ERR unknown command %s\n", cmd);
    }
    (void)n5;
}

/* ---------------------------------------------------------------------------------------------------- certificates */

static TlsTrust *g_trust;

static TlsTrust *trust(void) {
    if (!g_trust) g_trust = tls_trust_new();
    return g_trust;
}

static void print_cert(const u8 *der, size_t len) {
    x509_cert c;
    int rc = x509_parse(&c, der, len);
    printf("parse %d\n", rc);
    if (rc) return;
    TlsCertInfo ci;
    x509_describe(&c, &ci);
    printf("version %d\nsig_alg %d %d %u\nkey %s\nbits %u\nsubject %s\nissuer %s\nnames %s\n", c.version, c.sig_alg, c.sig_hash, c.sig_salt,
           ci.key, c.key_bits, ci.subject, ci.issuer, ci.names);
    printf("not_before %lld\nnot_after %lld\nca %d %d %d\nku %d %u\neku %d %u\nnc %d %zu %zu\nunknown_critical %d\nself_issued %d\nsig %s\n",
           (long long)c.not_before, (long long)c.not_after, c.has_bc, c.is_ca, c.path_len, c.has_ku, c.ku, c.has_eku, c.eku,
           c.has_nc, c.nc_permitted.n, c.nc_excluded.n, c.unknown_critical, c.self_issued, ci.sig);
    printf("self_signature %d\n", x509_check_signature(&c, &c));
}

static void cmd_x509(const char *cmd) {
    size_t n;
    if (!strcmp(cmd, "cert")) {
        u8 *d = unhex(arg(1), &n);
        print_cert(d, n);
        free(d);
    } else if (!strcmp(cmd, "name")) {
        u8 *d = unhex(arg(1), &n);
        printf("%d\n", tls_cert_matches_host(d, n, arg(2)));
        free(d);
    } else if (!strcmp(cmd, "trust_reset")) {
        tls_trust_free(g_trust);
        g_trust = NULL;
        printf("ok\n");
    } else if (!strcmp(cmd, "trust_add")) {
        u8 *d = unhex(arg(1), &n);
        printf("%d\n", tls_trust_add_der(trust(), d, n));
        free(d);
    } else if (!strcmp(cmd, "trust_file")) {
        int rc = tls_trust_load_file(trust(), arg(1));
        printf("%d %zu %zu\n", rc, tls_trust_count(trust()), tls_trust_skipped(trust()));
    } else if (!strcmp(cmd, "verify")) {
        int64_t now = strtoll(arg(1), NULL, 10);
        const char *host = strcmp(arg(2), "-") ? arg(2) : NULL;
        int count = argc_ - 3;
        if (count > 10) count = 10;
        u8 *ders[10];
        size_t lens[10];
        x509_cert chain[10];
        int bad = 0;
        for (int i = 0; i < count; i++) {
            ders[i] = unhex(arg(3 + i), &lens[i]);
            if (x509_parse(&chain[i], ders[i], lens[i]) != 0) bad = 1;
        }
        const x509_cert *path[11];
        int pn = 0;
        int rc = bad ? TLS_E_CERT_PARSE : x509_verify_chain(trust(), chain, count, host, now, path, &pn);
        printf("%d", rc);
        for (int i = 0; i < pn; i++) {
            char s[200];
            x509_name_string(path[i]->subject, s, sizeof s);
            printf(" | %s", s);
        }
        printf("\n");
        for (int i = 0; i < count; i++) free(ders[i]);
    } else if (!strcmp(cmd, "fuzz_der")) {
        unsigned long long seed = strtoull(arg(1), NULL, 10) * 2 + 1;
        long iters = atol(arg(2));
        int k = argc_ - 3;
        if (k < 1) { printf("ERR no input\n"); return; }
        u8 *ders[16];
        size_t lens[16];
        if (k > 16) k = 16;
        for (int i = 0; i < k; i++) ders[i] = unhex(arg(3 + i), &lens[i]);
        TlsTrust *empty = tls_trust_new();
        long parsed = 0;
        for (long it = 0; it < iters; it++) {
            u8 r[8];
            det_random(&seed, r, 8);
            int which = r[0] % k;
            size_t len = lens[which];
            u8 *buf = malloc(len + 1);
            memcpy(buf, ders[which], len);
            int muts = 1 + r[1] % 4;
            for (int m = 0; m < muts; m++) {
                u8 q[4];
                det_random(&seed, q, 4);
                size_t pos = len ? ((size_t)q[1] << 8 | q[2]) % len : 0;
                switch (q[0] % 6) {
                case 0: if (len) buf[pos] ^= (u8)(1u << (q[3] % 8)); break;
                case 1: if (len) buf[pos] = q[3]; break;
                case 2: if (len > 1) len = pos + 1; break;
                case 3: if (len) buf[pos] = (u8)(0x80 | (q[3] % 6)); break;      /* length forms */
                case 4: if (len) buf[pos] = 0xff; break;
                default: if (len > 2) { size_t a = pos, b = (pos + q[3]) % len; u8 t = buf[a]; buf[a] = buf[b]; buf[b] = t; } break;
                }
            }
            u8 *exact = malloc(len ? len : 1);                       /* an exact-size copy: ASan sees every over-read */
            memcpy(exact, buf, len);
            free(buf);
            x509_cert c;
            if (x509_parse(&c, exact, len) == 0) {
                parsed++;
                TlsCertInfo ci;
                x509_describe(&c, &ci);
                x509_name_matches(&c, "localhost");
                x509_name_matches(&c, "a.b.example.com");
                x509_name_matches(&c, "10.0.2.2");
                if (it % 64 == 0) x509_check_signature(&c, &c);     /* public-key work is slow under ASan: now and then */
                x509_cert chain[2] = { c, c };
                x509_verify_chain(empty, chain, 2, "localhost", 1800000000, NULL, NULL);
            }
            free(exact);
        }
        printf("fuzz %ld parsed %ld\n", iters, parsed);
        for (int i = 0; i < k; i++) free(ders[i]);
        tls_trust_free(empty);
    }
}

/* ---------------------------------------------------------------------------------------------------- connections */

static int tcp_connect(const char *host, const char *port) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;
    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    return fd;
}

static const char *errname(int rc) {
    switch (rc) {
    case 0: return "OK";
    case TLS_WANT_READ: return "WANT_READ";
    case TLS_WANT_WRITE: return "WANT_WRITE";
    case TLS_E_ARG: return "ARG"; case TLS_E_NOMEM: return "NOMEM"; case TLS_E_IO: return "IO"; case TLS_E_TIMEOUT: return "TIMEOUT";
    case TLS_E_EOF: return "EOF"; case TLS_E_TRUNCATED: return "TRUNCATED"; case TLS_E_PROTOCOL: return "PROTOCOL"; case TLS_E_ALERT: return "ALERT";
    case TLS_E_VERSION: return "VERSION"; case TLS_E_SUITE: return "SUITE"; case TLS_E_DECRYPT: return "DECRYPT";
    case TLS_E_HANDSHAKE_SIG: return "HANDSHAKE_SIG"; case TLS_E_FINISHED: return "FINISHED"; case TLS_E_RANDOM: return "RANDOM";
    case TLS_E_NO_TRUST: return "NO_TRUST"; case TLS_E_CLOCK: return "CLOCK"; case TLS_E_CLOSED: return "CLOSED"; case TLS_E_PLAINTEXT: return "PLAINTEXT";
    case TLS_E_CERT_PARSE: return "CERT_PARSE"; case TLS_E_CERT_UNTRUSTED: return "CERT_UNTRUSTED"; case TLS_E_CERT_SELF_SIGNED: return "CERT_SELF_SIGNED";
    case TLS_E_CERT_EXPIRED: return "CERT_EXPIRED"; case TLS_E_CERT_NOT_YET_VALID: return "CERT_NOT_YET_VALID"; case TLS_E_CERT_NAME: return "CERT_NAME";
    case TLS_E_CERT_SIGNATURE: return "CERT_SIGNATURE"; case TLS_E_CERT_USAGE: return "CERT_USAGE"; case TLS_E_CERT_UNSUPPORTED: return "CERT_UNSUPPORTED";
    case TLS_E_CERT_WEAK: return "CERT_WEAK"; case TLS_E_CERT_EMPTY: return "CERT_EMPTY"; case TLS_E_CERT_CONSTRAINTS: return "CERT_CONSTRAINTS";
    default: return "?";
    }
}

static const char *opt(const char *key) {
    size_t kl = strlen(key);
    for (int i = 4; i < argc_; i++)
        if (strncmp(argv_[i], key, kl) == 0 && argv_[i][kl] == '=') return argv_[i] + kl + 1;
    return NULL;
}

/* In non-blocking mode every call goes through here: poll for what the library asked, call again. */
static long nb_call(TlsConn *c, int which, void *buf, size_t n, int *waits) {
    for (;;) {
        long rc = which == 0 ? tls_handshake(c) : which == 1 ? tls_read(c, buf, n) : which == 2 ? tls_write(c, buf, n) : tls_flush(c);
        if (rc != TLS_WANT_READ && rc != TLS_WANT_WRITE) return rc;
        (*waits)++;
        struct pollfd p = { tls_fd(c), tls_poll_events(c), 0 };
        if (poll(&p, 1, 15000) <= 0) return TLS_E_TIMEOUT;
    }
}

/* connect HOST PORT SNI [ca=FILE] [insecure=1] [v=12|13] [suites=MASK] [alpn=P] [time=T] [nonblock=1] [timeout=MS] [seed=N]
 *         [send=HEX] [sendn=N] [readn=N] [readall=1] [keyupdate=0|1] [script_ch1=HEX] [script_ch2=HEX] [script_x=HEX]
 *         [script_p=HEX] [noaesni=1] [requireclock=1] [shutdown=1] [trust=1 (the roots loaded with trust_*)]
 * Prints result, the error text, the negotiated parameters, the path, and what was read (length and SHA-256). */
static void do_connect(void) {
    const char *host = arg(1), *port = arg(2), *sni = strcmp(arg(3), "-") ? arg(3) : NULL;
    TlsConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    const char *v;
    unsigned long long seed = 0;
    if ((v = opt("ca"))) cfg.ca_file = v;
    if ((v = opt("trust")) && atoi(v)) cfg.trust = trust();
    if ((v = opt("insecure")) && atoi(v)) cfg.flags |= TLS_INSECURE;
    if ((v = opt("v"))) cfg.flags |= atoi(v) == 12 ? TLS_ONLY_TLS12 : TLS_ONLY_TLS13;
    if ((v = opt("suites"))) cfg.suites = (unsigned)atoi(v);
    if ((v = opt("alpn"))) cfg.alpn = v;
    if ((v = opt("nonblock")) && atoi(v)) cfg.flags |= TLS_NONBLOCK;
    if ((v = opt("noaesni")) && atoi(v)) cfg.flags |= TLS_NO_AESNI;
    if ((v = opt("requireclock")) && atoi(v)) cfg.flags |= TLS_REQUIRE_CLOCK;
    if ((v = opt("timeout"))) cfg.timeout_ms = atoi(v);
    if ((v = opt("seed"))) { seed = strtoull(v, NULL, 10) | 1; cfg.random = det_random; cfg.random_ctx = &seed; }
    tls_set_time((v = opt("time")) ? strtoll(v, NULL, 10) : 0);
    int fd = tcp_connect(host, port);
    if (fd < 0) { printf("result connect-failed\n"); return; }
    TlsConn *c = tls_new(fd, sni, &cfg);
    if (!c) { printf("result nomem\n"); close(fd); return; }
    size_t l1 = 0, l2 = 0, lx = 0, lp = 0;
    u8 *ch1 = NULL, *ch2 = NULL, *sx = NULL, *sp = NULL;
    if ((v = opt("script_ch1"))) {
        ch1 = unhex(v, &l1);
        if ((v = opt("script_ch2"))) ch2 = unhex(v, &l2);
        if ((v = opt("script_x"))) sx = unhex(v, &lx);
        if ((v = opt("script_p"))) sp = unhex(v, &lp);
        tls_test_script(c, ch1, l1, ch2, l2, lx == 32 ? sx : NULL, lp == 32 ? sp : NULL);
    }
    int waits = 0;
    int nb = (cfg.flags & TLS_NONBLOCK) != 0;
    long rc = nb ? nb_call(c, 0, NULL, 0, &waits) : tls_handshake(c);
    printf("result %ld %s\n", rc, errname((int)rc));
    printf("message %s\n", tls_error_string(c));
    const TlsInfo *in = tls_info(c);
    printf("version %s\nsuite %s\ngroup %s\nsigalg %s\nalpn %s\nhello_retry %d\nverified %d\ninsecure %d\nverify_error %d %s\nclock %d %lld\n",
           in->version_name ? in->version_name : "-", in->suite_name ? in->suite_name : "-", in->group_name ? in->group_name : "-",
           in->sigalg_name ? in->sigalg_name : "-", in->alpn[0] ? in->alpn : "-", in->hello_retry, in->verified, in->insecure,
           in->verify_error, errname(in->verify_error), in->clock, (long long)in->clock_now);
    printf("alert_received %d\nalert_sent %d\nsent_certs %d\n", in->alert_received, in->alert_sent, in->sent_certs);
    for (int i = 0; i < in->path_len; i++)
        printf("path %d %s | %s | %s | %s | store=%d\n", i, in->path[i].subject, in->path[i].issuer, in->path[i].key, in->path[i].sig, in->path[i].from_store);
    if (rc == 0) {
        if ((v = opt("send"))) {
            size_t n;
            u8 *d = unhex(v, &n);
            long w = nb ? nb_call(c, 2, d, n, &waits) : tls_write(c, d, n);
            if (nb && w > 0) nb_call(c, 3, NULL, 0, &waits);
            printf("wrote %ld\n", w);
            free(d);
        }
        if ((v = opt("sendn"))) {                                    /* N pseudo-random bytes; their SHA-256 is printed */
            size_t n = (size_t)strtoull(v, NULL, 10), done = 0;
            unsigned long long s2 = 12345;
            tlsc_hash h;
            tlsc_hash_init(&h, TLSC_SHA256);
            u8 *blk = malloc(65536);
            while (done < n) {
                size_t k = n - done < 65536 ? n - done : 65536;
                det_random(&s2, blk, k);
                tlsc_hash_update(&h, blk, k);
                size_t off = 0;
                while (off < k) {
                    long w = nb ? nb_call(c, 2, blk + off, k - off, &waits) : tls_write(c, blk + off, k - off);
                    if (w <= 0) { printf("write-error %ld %s\n", w, tls_error_string(c)); off = k; done = n; break; }
                    off += (size_t)w;
                }
                done += k;
            }
            if (nb) nb_call(c, 3, NULL, 0, &waits);
            u8 dg[32];
            tlsc_hash_final(&h, dg);
            printf("sent %zu ", n);
            puthex(dg, 32);
            free(blk);
        }
        if ((v = opt("keyupdate"))) printf("keyupdate %d\n", tls_key_update(c, atoi(v)));
        long want = (v = opt("readn")) ? atol(v) : (opt("readall") ? -1 : 0);
        size_t show = (v = opt("show")) ? (size_t)atol(v) : 0;
        if (want) {
            tlsc_hash h;
            tlsc_hash_init(&h, TLSC_SHA256);
            long got = 0, r = 0;
            u8 buf[20000], head[4096];
            size_t nhead = 0;
            if (show > sizeof head) show = sizeof head;
            while (want < 0 || got < want) {
                size_t ask = want < 0 || want - got > (long)sizeof buf ? sizeof buf : (size_t)(want - got);
                r = nb ? nb_call(c, 1, buf, ask, &waits) : tls_read(c, buf, ask);
                if (r <= 0) break;
                tlsc_hash_update(&h, buf, (size_t)r);
                for (long i = 0; i < r && nhead < show; i++) head[nhead++] = buf[i];
                got += r;
            }
            u8 dg[32];
            tlsc_hash_final(&h, dg);
            printf("read %ld end %ld %s ", got, r > 0 ? 0 : r, errname((int)(r > 0 ? 0 : r)));
            puthex(dg, 32);
            if (show) { printf("head "); puthex(head, nhead); }
            if (r < 0) printf("read-message %s\n", tls_error_string(c));
        }
        if ((v = opt("send2"))) {
            size_t n;
            u8 *d = unhex(v, &n);
            long w = nb ? nb_call(c, 2, d, n, &waits) : tls_write(c, d, n);
            if (nb && w > 0) nb_call(c, 3, NULL, 0, &waits);
            printf("wrote2 %ld\n", w);
            free(d);
        }
        if ((v = opt("readn2"))) {                                   /* a second read, after send2 */
            long w2 = atol(v), got = 0, r = 0;
            u8 buf[4096];
            while (got < w2) {
                r = nb ? nb_call(c, 1, buf, sizeof buf < (size_t)(w2 - got) ? sizeof buf : (size_t)(w2 - got), &waits)
                       : tls_read(c, buf, sizeof buf < (size_t)(w2 - got) ? sizeof buf : (size_t)(w2 - got));
                if (r <= 0) break;
                got += r;
            }
            printf("read2 %ld end %ld %s\n", got, r > 0 ? 0 : r, errname((int)(r > 0 ? 0 : r)));
        }
        if ((v = opt("shutdown")) && atoi(v)) printf("shutdown %d\n", tls_shutdown(c));
    }
    printf("bytes %llu %llu waits %d\n", in->bytes_in, in->bytes_out, waits);
    tls_close(c);
    close(fd);
    free(ch1); free(ch2); free(sx); free(sp);
    tls_set_time(0);
    printf("done\n");
}

/* ---------------------------------------------------------------------------------------------------- http */

#ifdef WITH_HTTP
typedef struct {
    tlsc_hash body;
    unsigned long long n;
    int headers_done_ret;
    int stop_after;                                                  /* on_body returns 1 after this many bytes (-1 never) */
} pctx;

static int p_status(void *ctx, int status, const char *reason) { printf("status %d %s\n", status, reason); return 0; }
static int p_header(void *ctx, const char *name, const char *value) { printf("header %s: %s\n", name, value); return 0; }
static int p_done(void *ctx, int status) { pctx *c = ctx; printf("headers_done %d\n", status); return c->headers_done_ret; }
static int p_body(void *ctx, const uint8_t *d, size_t n) {
    pctx *c = ctx;
    tlsc_hash_update(&c->body, d, n);
    c->n += n;
    return c->stop_after >= 0 && c->n >= (unsigned long long)c->stop_after;
}

static void print_body(pctx *c) {
    u8 dg[32];
    tlsc_hash_final(&c->body, dg);
    printf("body %llu ", c->n);
    puthex(dg, 32);
}

static HttpCookieJar *g_jar;

static int g_status(void *ctx, int status, const char *reason) { (void)ctx; printf("status %d %s\n", status, reason); return 0; }

static void get_event(void *ctx, const char *line) { (void)ctx; printf("event %s\n", line); }
static int get_body(void *ctx, const uint8_t *d, size_t n) { pctx *c = ctx; tlsc_hash_update(&c->body, d, n); c->n += n; return 0; }
static void get_status(void *ctx, int status, const char *reason) { (void)ctx; printf("status %d %s\n", status, reason); }

static void http_commands(void) {
    const char *cmd = argv_[0];
    if (!strcmp(cmd, "http_parse")) {                                /* http_parse FLAGS CHUNK HEX [eof=1] [discard=1] [stop=N] */
        int flags = atoi(arg(1));
        size_t chunk = (size_t)atoi(arg(2));
        size_t n;
        u8 *d = unhex(arg(3), &n);
        pctx c;
        memset(&c, 0, sizeof c);
        tlsc_hash_init(&c.body, TLSC_SHA256);
        c.stop_after = -1;
        for (int i = 4; i < argc_; i++) {
            if (!strcmp(argv_[i], "discard=1")) c.headers_done_ret = 1;
            if (!strncmp(argv_[i], "stop=", 5)) c.stop_after = atoi(argv_[i] + 5);
        }
        HttpCallbacks cb = { p_status, p_header, p_done, p_body, &c };
        (void)g_status;
        HttpParser p;
        http_parser_init(&p, &cb, flags);
        int rc = 0;
        size_t off = 0, used_total = 0;
        while (off < n && rc == 0) {
            size_t k = n - off < chunk ? n - off : chunk, used = 0;
            rc = http_parser_feed(&p, d + off, k, &used);
            used_total += used;
            off += k;
        }
        int want_eof = 0;
        for (int i = 4; i < argc_; i++) if (!strcmp(argv_[i], "eof=1")) want_eof = 1;
        if (rc == 0 && want_eof) rc = http_parser_eof(&p);
        printf("result %d used %zu framing %d\n", rc, used_total, p.framing);
        if (rc < 0) printf("error %s\n", p.err);
        print_body(&c);
        http_parser_free(&p);
        free(d);
    } else if (!strcmp(cmd, "url")) {                                /* url BASE [REF] (hex, they may hold spaces) */
        size_t n1, n2;
        u8 *b = unhex(arg(1), &n1), *r = argc_ > 2 ? unhex(arg(2), &n2) : NULL;
        char bs[8192], rs[8192], out[8192];
        snprintf(bs, sizeof bs, "%.*s", (int)n1, (const char *)b);
        HttpUrl base, res;
        int rc = http_url_parse(bs, &base);
        if (rc == 0 && r) {
            snprintf(rs, sizeof rs, "%.*s", (int)n2, (const char *)r);
            rc = http_url_resolve(&base, rs, &res);
        } else {
            res = base;
        }
        if (rc) printf("error %d\n", rc);
        else if (http_url_format(&res, out, sizeof out) < 0) printf("error format\n");
        else printf("%s %s %d %s\n", out, res.host, res.port, res.path);
        free(b);
        free(r);
    } else if (!strcmp(cmd, "http_request")) {                       /* http_request URLHEX EXTRAHEX DECODE */
        size_t n1, n2;
        u8 *u = unhex(arg(1), &n1), *e = unhex(arg(2), &n2);
        char us[8192], es[4096], out[16384];
        snprintf(us, sizeof us, "%.*s", (int)n1, (const char *)u);
        snprintf(es, sizeof es, "%.*s", (int)n2, (const char *)e);
        HttpUrl url;
        if (http_url_parse(us, &url)) printf("error url\n");
        else {
            long len = http_request_build(out, sizeof out, "GET", &url, n2 ? es : NULL, atoi(arg(3)));
            if (len < 0) printf("error size\n");
            else puthex((u8 *)out, (size_t)len);
        }
        free(u);
        free(e);
    } else if (!strcmp(cmd, "http_cookie_set")) {                    /* http_cookie_set URLHEX SETCOOKIEHEX NOW */
        size_t n1, n2;
        u8 *u = unhex(arg(1), &n1), *s = unhex(arg(2), &n2);
        char us[4096], ss[4096];
        snprintf(us, sizeof us, "%.*s", (int)n1, (const char *)u);
        snprintf(ss, sizeof ss, "%.*s", (int)n2, (const char *)s);
        if (!g_jar) g_jar = http_jar_new();
        HttpUrl url;
        if (http_url_parse(us, &url) == 0) http_jar_set(g_jar, &url, ss, strtoll(arg(3), NULL, 10));
        printf("count %d\n", http_jar_count(g_jar));
        free(u);
        free(s);
    } else if (!strcmp(cmd, "http_cookie_get")) {                    /* http_cookie_get URLHEX NOW */
        size_t n1;
        u8 *u = unhex(arg(1), &n1);
        char us[4096], out[8192];
        snprintf(us, sizeof us, "%.*s", (int)n1, (const char *)u);
        HttpUrl url;
        if (!g_jar) g_jar = http_jar_new();
        if (http_url_parse(us, &url) == 0) http_jar_header(g_jar, &url, strtoll(arg(2), NULL, 10), out, sizeof out);
        else out[0] = 0;
        printf("cookie %s\n", out);
        free(u);
    } else if (!strcmp(cmd, "http_cookie_reset")) {
        http_jar_free(g_jar);
        g_jar = NULL;
        printf("ok\n");
    } else if (!strcmp(cmd, "http_get")) {                           /* http_get URL [ca=F] [insecure=1] [head=1] [redirs=N] [timeout=MS] [nodecode=1] [time=T] [jar=1] */
        TlsConfig tc;
        memset(&tc, 0, sizeof tc);
        HttpOptions o;
        memset(&o, 0, sizeof o);
        pctx c;
        memset(&c, 0, sizeof c);
        tlsc_hash_init(&c.body, TLSC_SHA256);
        for (int i = 2; i < argc_; i++) {
            const char *a = argv_[i];
            if (!strncmp(a, "ca=", 3)) tc.ca_file = a + 3;
            else if (!strcmp(a, "insecure=1")) tc.flags |= TLS_INSECURE;
            else if (!strcmp(a, "head=1")) o.head = 1;
            else if (!strncmp(a, "redirs=", 7)) o.max_redirects = atoi(a + 7);
            else if (!strncmp(a, "timeout=", 8)) o.timeout_ms = atoi(a + 8);
            else if (!strcmp(a, "nodecode=1")) o.no_decode = 1;
            else if (!strncmp(a, "time=", 5)) tls_set_time(strtoll(a + 5, NULL, 10));
            else if (!strcmp(a, "jar=1")) { if (!g_jar) g_jar = http_jar_new(); o.jar = g_jar; }
        }
        o.tls = &tc;
        o.on_body = get_body;
        o.on_status = get_status;
        o.on_event = get_event;
        o.ctx = &c;
        HttpResult r;
        int rc = http_get(arg(1), &o, &r);
        printf("result %d status %d redirects %d downgraded %d tls %d tls_error %d\n", rc, r.status, r.redirects, r.downgraded, r.tls, r.tls_error);
        printf("url %s\nerror %s\ntype %s\nencoding %s\nlength %lld wire %llu\n", r.url, r.error_text, r.content_type, r.content_encoding,
               (long long)r.content_length, (unsigned long long)r.wire_bytes);
        print_body(&c);
        tls_set_time(0);
    } else {
        printf("ERR unknown command %s\n", cmd);
    }
}
#endif

/* ---------------------------------------------------------------------------------------------------- main */

int main(void) {
    char *line = NULL;
    size_t cap = 0;
    setvbuf(stdout, NULL, _IOLBF, 0);
    while (getline(&line, &cap, stdin) > 0) {
        split(line);
        if (argc_ == 0) continue;
        const char *cmd = argv_[0];
        if (!strcmp(cmd, "cert") || !strcmp(cmd, "name") || !strncmp(cmd, "trust_", 6) || !strcmp(cmd, "verify") || !strcmp(cmd, "fuzz_der")) cmd_x509(cmd);
        else if (!strcmp(cmd, "connect")) do_connect();
#ifdef WITH_HTTP
        else if (!strncmp(cmd, "http", 4) || !strcmp(cmd, "url")) http_commands();
#endif
        else cmd_primitive(cmd);
        printf(".\n");                                               /* the end of this command's output */
        fflush(stdout);
    }
    free(line);
    tls_trust_free(g_trust);
#ifdef WITH_HTTP
    http_jar_free(g_jar);
#endif
    return 0;
}
