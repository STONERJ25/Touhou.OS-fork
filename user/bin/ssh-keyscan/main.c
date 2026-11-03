/*
 * ssh-keyscan [-4Hqv] [-f file] [-p port] [-T timeout] [-t ed25519] host ...
 *
 * Connects to each host, does the key exchange far enough to receive and verify the host key, and prints it in the form of a known_hosts line. Nothing is
 * trusted by this: it only reports what the host said, so the output has to be compared with a fingerprint from another source (`ssh-keygen -l -f -`) before it
 * is added to a known_hosts file. Only ssh-ed25519 host keys are asked for, since that is all this client's key exchange offers.
 */
#include <sshc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static void usage(void) {
    fprintf(stderr, "usage: ssh-keyscan [-4Hqv] [-f file] [-p port] [-T timeout] [-t ed25519] [host ...]\n");
    exit(1);
}

typedef struct { uint8_t blob[SSHC_BLOB_MAX]; size_t n; } Captured;

/* Remembers the key and refuses it, which ends the key exchange (and the connection) right here. */
static int capture(void *ctx, SshcConn *c, const uint8_t *blob, size_t n) {
    Captured *k = ctx;
    (void)c;
    if (n <= sizeof k->blob) { memcpy(k->blob, blob, n); k->n = n; }
    return 1;
}

static int scan(const char *host, int port, int timeout_s, bool hash, bool quiet) {
    char why[300], ip[48];
    int fd = sshc_tcp_connect(host, port, timeout_s * 1000, why, sizeof why, ip, sizeof ip);
    if (fd < 0) { fprintf(stderr, "%s: %s\n", host, why); return -1; }
    SshcConn *c = sshc_conn_new(fd);
    if (!c) { close(fd); return -1; }
    Captured cap;
    memset(&cap, 0, sizeof cap);
    c->hostkey_fn = capture;
    c->hostkey_ctx = &cap;
    c->deadline_ms = sshc_mono_ms() + (long)timeout_s * 1000;
    sshc_handshake(c);
    if (!cap.n) {
        fprintf(stderr, "%s: %s\n", host, c->err[0] ? c->err : "no host key");
        sshc_conn_free(c);
        return -1;
    }
    if (!quiet) fprintf(stderr, "# %s:%d %s\n", host, port, c->server_id);
    SshcPub key;
    sshc_pub_from_blob(cap.blob, cap.n, &key);
    char name[320], b64[2 * SSHC_BLOB_MAX];
    sshc_hk_name(host, port, name, sizeof name);
    sshc_b64_encode(key.blob, key.blob_len, b64, true);
    if (hash) {
        uint8_t salt[20], mac[20];
        char sb[40], mb[40];
        sshc_random(salt, sizeof salt);
        sshc_hmac_sha1(mac, salt, sizeof salt, name, strlen(name));
        sshc_b64_encode(salt, sizeof salt, sb, true);
        sshc_b64_encode(mac, sizeof mac, mb, true);
        printf("|1|%s|%s %s %s\n", sb, mb, key.type, b64);
    } else {
        printf("%s %s %s\n", name, key.type, b64);
    }
    fflush(stdout);
    sshc_conn_free(c);
    return 0;
}

int main(int argc, char **argv) {
    int port = 22, timeout = 5;
    bool hash = false, quiet = false;
    const char *file = NULL;
    int i = 1;
    for (; i < argc; i++) {
        char *a = argv[i];
        if (a[0] != '-' || !a[1]) break;
        if (!strcmp(a, "--")) { i++; break; }
        for (char *p = a + 1; *p; p++) {
            const char *val = NULL;
            if (strchr("fpTt", *p)) {
                val = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : NULL);
                if (!val) usage();
            }
            switch (*p) {
            case 'p': port = atoi(val); if (port < 1 || port > 65535) { fprintf(stderr, "ssh-keyscan: bad port\n"); return 1; } break;
            case 'T': timeout = atoi(val); if (timeout < 1) timeout = 5; break;
            case 'f': file = val; break;
            case 't':
                if (!strstr(val, "ed25519")) { fprintf(stderr, "ssh-keyscan: only ed25519 host keys can be asked for here\n"); return 1; }
                break;
            case 'H': hash = true; break;
            case 'q': quiet = true; break;
            case 'v': sshc_verbose = 1; break;
            case '4': break;
            case '6': fprintf(stderr, "ssh-keyscan: IPv6 is not available on this system\n"); return 1;
            default: usage();
            }
            if (val) break;
        }
    }
    int found = 0, tried = 0;
    for (; i < argc; i++, tried++) {
        char host[300];
        sshc_cpy(host, sizeof host, argv[i]);
        int p = port;
        if (host[0] == '[') {                                   /* [host]:port */
            char *rb = strchr(host, ']');
            if (rb && rb[1] == ':') { p = atoi(rb + 2); *rb = 0; memmove(host, host + 1, strlen(host)); }
        }
        if (scan(host, p, timeout, hash, quiet) == 0) found++;
    }
    if (file) {
        size_t len;
        const char *why;
        char *text = strcmp(file, "-") ? sshc_read_file(file, 1u << 20, &len, false, &why) : NULL;
        if (!text) { fprintf(stderr, "ssh-keyscan: %s: cannot read\n", file); return 1; }
        for (char *p = text; *p;) {
            char *eol = strchr(p, '\n');
            if (eol) *eol = 0;
            char *h = p;
            while (*h == ' ' || *h == '\t') h++;
            char *e = h + strlen(h);
            while (e > h && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
            if (*h && *h != '#') { tried++; if (scan(h, port, timeout, hash, quiet) == 0) found++; }
            if (!eol) break;
            p = eol + 1;
        }
        free(text);
    }
    if (tried == 0) usage();
    return found ? 0 : 1;
}
