/*
 * Driver for the host tests of the ssh client library (tests/host/test_sshclient.py): one command per line of argv, results on standard output, so that Python
 * can feed it data made by OpenSSH or by the `cryptography` package and compare the answers with theirs. Built with gcc, AddressSanitizer and UBSan against the very
 * sources the OS is built from (user/lib/sshcrypto/src/sshc_*.c); only sshc_random() differs (-DSSHC_HOST).
 *
 *   sshclient_tool sha1 HEX                        sha1 of the bytes
 *   sshclient_tool hmac-sha1 KEYHEX DATAHEX
 *   sshclient_tool fp PUBFILE                       fingerprints, size, label and randomart of the first key line of PUBFILE
 *   sshclient_tool hkcheck FILE HOST PORT PUBFILE   known_hosts decision
 *   sshclient_tool hkadd FILE HOST PORT HASH PUBFILE
 *   sshclient_tool pattern PATTERN STRING           yes / no
 *   sshclient_tool bcrypt PASSHEX SALTHEX ROUNDS KEYLEN
 *   sshclient_tool aesctr KEYHEX IVHEX DATAHEX
 *   sshclient_tool keyinfo FILE                     what the public part of a key file says
 *   sshclient_tool keyunlock FILE [PASS]            seed, public key and comment, or the error
 *   sshclient_tool keymake SEEDHEX COMMENT PASS ROUNDS    a key file's text (PASS "-" for none)
 *   sshclient_tool expand PATH HOST USER PORT
 *   sshclient_tool b64 enc|dec TEXT
 */
#include <sshc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sshcrypto.h>

static size_t unhex(const char *h, uint8_t *out, size_t cap) {
    size_t n = strlen(h) / 2;
    if (n > cap) n = cap;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(h + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
    return n;
}

static void put_hex(const uint8_t *p, size_t n) {
    char *h = malloc(2 * n + 1);
    sshc_hex(p, n, h);
    puts(h);
    free(h);
}

static int load_pub(const char *file, SshcPub *k, char *comment, size_t ccap) {
    size_t n;
    const char *why;
    char *text = sshc_read_file(file, 1 << 20, &n, false, &why);
    if (!text) { fprintf(stderr, "%s: %s\n", file, why); return -1; }
    int rc = sshc_pub_parse(text, k, comment, ccap);
    free(text);
    return rc;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: see the top of sshclient_tool.c\n"); return 2; }
    const char *cmd = argv[1];
    static uint8_t buf[1 << 16], buf2[1 << 16];
    if (!strcmp(cmd, "sha1") && argc == 3) {
        SshcSha1 c;
        uint8_t d[20];
        size_t n = unhex(argv[2], buf, sizeof buf);
        sshc_sha1_init(&c);
        sshc_sha1_update(&c, buf, n);
        sshc_sha1_final(&c, d);
        put_hex(d, 20);
    } else if (!strcmp(cmd, "hmac-sha1") && argc == 4) {
        uint8_t d[20];
        size_t kn = unhex(argv[2], buf, sizeof buf), dn = unhex(argv[3], buf2, sizeof buf2);
        sshc_hmac_sha1(d, buf, kn, buf2, dn);
        put_hex(d, 20);
    } else if (!strcmp(cmd, "fp") && argc == 3) {
        SshcPub k;
        char fp[64], md5[64], art[1024], comment[128];
        if (load_pub(argv[2], &k, comment, sizeof comment) != 0) { puts("not a key"); return 1; }
        sshc_fingerprint(k.blob, k.blob_len, fp);
        sshc_fingerprint_md5(k.blob, k.blob_len, md5);
        sshc_randomart(k.blob, k.blob_len, sshc_pub_label(&k), sshc_pub_bits(&k), art, sizeof art);
        printf("%d %s %s (%s)\n%s\ncomment=%s\n%s", sshc_pub_bits(&k), fp, comment, sshc_pub_label(&k), md5, comment, art);
    } else if (!strcmp(cmd, "hkcheck") && argc == 6) {
        SshcPub k;
        if (load_pub(argv[5], &k, NULL, 0) != 0) { puts("not a key"); return 1; }
        const char *files[1] = { argv[2] };
        SshcHkResult r = sshc_hk_check(files, 1, argv[3], atoi(argv[4]), k.blob, k.blob_len);
        static const char *const names[] = { "ok", "new", "changed", "revoked" };
        printf("%s %s:%d other=%d\n", names[r.status], r.file, r.line, r.other_types);
    } else if (!strcmp(cmd, "hkadd") && argc == 7) {
        SshcPub k;
        if (load_pub(argv[6], &k, NULL, 0) != 0) { puts("not a key"); return 1; }
        printf("%d\n", sshc_hk_add(argv[2], argv[3], atoi(argv[4]), atoi(argv[5]) != 0, &k));
    } else if (!strcmp(cmd, "pattern") && argc == 4) {
        puts(sshc_pattern_match(argv[2], argv[3]) ? "yes" : "no");
    } else if (!strcmp(cmd, "bcrypt") && argc == 6) {
        size_t pn = unhex(argv[2], buf, sizeof buf), sn = unhex(argv[3], buf2, sizeof buf2);
        size_t kl = (size_t)atoi(argv[5]);
        static uint8_t key[1024];
        sshc_bcrypt_pbkdf(buf, pn, buf2, sn, key, kl, (unsigned)atoi(argv[4]));
        put_hex(key, kl);
    } else if (!strcmp(cmd, "aesctr") && argc == 5) {
        uint8_t iv[16];
        SshcAes a;
        size_t kn = unhex(argv[2], buf, 32), in = unhex(argv[3], iv, 16), dn = unhex(argv[4], buf2, sizeof buf2);
        (void)in;
        sshc_aes_init(&a, buf, (int)kn);
        sshc_aes_ctr(&a, iv, buf2, buf2, dn);
        put_hex(buf2, dn);
    } else if (!strcmp(cmd, "keyinfo") && argc == 3) {
        size_t n;
        const char *why = NULL, *err = NULL;
        char *text = sshc_read_file(argv[2], 1 << 20, &n, false, &why);
        SshcKeyFile kf;
        if (!text) { printf("unreadable: %s\n", why); return 1; }
        if (sshc_keyfile_parse(text, n, &kf, &err) != 0) { printf("bad: %s\n", err); free(text); return 1; }
        char line[2048];
        sshc_pub_to_line(&kf.pub, NULL, line, sizeof line);
        printf("cipher=%s kdf=%s rounds=%u salt=%zu encrypted=%d\n%s\n", kf.cipher, kf.kdf, kf.rounds, kf.salt_len, kf.encrypted, line);
        sshc_keyfile_free(&kf);
        free(text);
    } else if (!strcmp(cmd, "keyunlock") && argc >= 3) {
        size_t n;
        const char *why = NULL, *err = NULL;
        char *text = sshc_read_file(argv[2], 1 << 20, &n, false, &why);
        SshcKeyFile kf;
        SshcEd25519 key;
        if (!text) { printf("unreadable: %s\n", why); return 1; }
        if (sshc_keyfile_parse(text, n, &kf, &err) != 0) { printf("bad: %s\n", err); free(text); return 1; }
        int rc = sshc_keyfile_unlock(&kf, argc > 3 ? argv[3] : NULL, &key, &err);
        if (rc != 0) printf("locked(%d): %s\n", rc, err);
        else {
            char sh[65], ph[65];
            sshc_hex(key.seed, 32, sh);
            sshc_hex(key.pk, 32, ph);
            printf("seed=%s pk=%s comment=%s\n", sh, ph, key.comment);
        }
        sshc_keyfile_free(&kf);
        free(text);
    } else if (!strcmp(cmd, "keymake") && argc == 6) {
        SshcEd25519 key;
        memset(&key, 0, sizeof key);
        unhex(argv[2], key.seed, 32);
        ed25519_pubkey(key.pk, key.seed);
        sshc_cpy(key.comment, sizeof key.comment, argv[3]);
        char *text;
        if (sshc_keyfile_make(&key, strcmp(argv[4], "-") ? argv[4] : NULL, (uint32_t)atoi(argv[5]), &text) != 0) { puts("failed"); return 1; }
        fputs(text, stdout);
        free(text);
    } else if (!strcmp(cmd, "expand") && argc == 6) {
        char out[256];
        if (!sshc_expand_path(argv[2], out, sizeof out, argv[3], argv[4], atoi(argv[5]))) puts("(does not fit or bad token)");
        else puts(out);
    } else if (!strcmp(cmd, "b64") && argc == 4) {
        if (!strcmp(argv[2], "enc")) {
            char out[4096];
            sshc_b64_encode((const uint8_t *)argv[3], strlen(argv[3]), out, true);
            puts(out);
        } else {
            int n = sshc_b64_decode(argv[3], strlen(argv[3]), buf, sizeof buf);
            if (n < 0) puts("(invalid)"); else { fwrite(buf, 1, (size_t)n, stdout); puts(""); }
        }
    } else {
        fprintf(stderr, "bad command line\n");
        return 2;
    }
    return 0;
}
