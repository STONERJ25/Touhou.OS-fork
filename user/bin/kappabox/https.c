/*
 * https_get: fetches a URL over HTTPS (or plain HTTP) with libhttp and libtls, verifying the server's certificate against the
 * trust store. docs/TLS.md has the details (there is no manual page yet).
 *
 *   https_get URL [-o FILE] [--ca FILE] [--insecure] [-v] [-I] [-q] [--max-redirs N] [--timeout S] [--tls12|--tls13]
 *             [--cipher aes128|aes256|chacha20] [--time EPOCH] [--require-clock] [--no-decompress]
 *
 * Exit status: 0 done (status below 400), 1 usage, 2 name or network failure, 3 no safe TLS connection (certificate refused,
 * no trust store, handshake failed), 4 the server answered with status 400 or above (the body is still written), 5 a bad or
 * cut response, 6 the output file could not be written. With -o the data goes to FILE.part first and is renamed only when
 * everything arrived, so a failed download never leaves a file that looks complete.
 */
#include "kappabox.h"
#include <http.h>
#include <tls.h>

typedef struct {
    int fd;
    int verbose, quiet, head;
    int werr;
    uint64_t written;
} get_ctx;

static int on_body(void *ctx, const uint8_t *d, size_t n) {
    get_ctx *g = ctx;
    while (n) {
        ssize_t w = write(g->fd, d, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { g->werr = errno ? errno : EIO; return 1; }
        d += w;
        n -= (size_t)w;
        g->written += (uint64_t)w;
    }
    return 0;
}

static void on_status(void *ctx, int status, const char *reason) {
    get_ctx *g = ctx;
    if (g->verbose) fprintf(stderr, "< HTTP %d %s\n", status, reason);
    if (g->head) printf("HTTP %d %s\n", status, reason);
}

static int on_header(void *ctx, const char *name, const char *value) {
    get_ctx *g = ctx;
    if (g->verbose) fprintf(stderr, "< %s: %s\n", name, value);
    if (g->head) printf("%s: %s\n", name, value);
    return 0;
}

static void on_event(void *ctx, const char *line) {
    get_ctx *g = ctx;
    if (g->verbose || !strncmp(line, "WARNING", 7)) fprintf(stderr, "%s%s\n", g->verbose ? "* " : "https_get: ", line);
}

static void date(int64_t t, char *buf, size_t n) {
    time_t tt = (time_t)t;
    struct tm tm;
    gmtime_r(&tt, &tm);
    strftime(buf, n, "%Y-%m-%d %H:%M:%S UTC", &tm);
}

static void on_tls(void *ctx, const TlsInfo *in) {
    get_ctx *g = ctx;
    char a[40], b[40];
    if (in->insecure) {
        fprintf(stderr, "https_get: WARNING: --insecure: the server's certificate was NOT checked; anyone on the way can read and change "
                        "this connection%s%s\n", in->verify_error ? ". Verification would have said: " : "",
                in->verify_error ? tls_strerror(in->verify_error) : "");
    }
    if (!g->verbose) return;
    if (in->version) {
        fprintf(stderr, "* %s, %s, key exchange %s, server signature %s%s\n", in->version_name, in->suite_name ? in->suite_name : "?",
                in->group_name ? in->group_name : "?", in->sigalg_name ? in->sigalg_name : "?", in->hello_retry ? ", after a HelloRetryRequest" : "");
        if (in->alpn[0]) fprintf(stderr, "* ALPN: %s\n", in->alpn);
    }
    date(in->clock_now, a, sizeof a);
    fprintf(stderr, "* clock: %s, %s\n", tls_clock_name(in->clock), a);
    fprintf(stderr, "* certificates (%s; the server sent %d):\n", in->verified ? "the verified path, leaf to root" : "as sent, NOT verified", in->sent_certs);
    for (int i = 0; i < in->path_len; i++) {
        const TlsCertInfo *c = &in->path[i];
        date(c->not_before, a, sizeof a);
        date(c->not_after, b, sizeof b);
        fprintf(stderr, "*   %d %s%s\n", i, c->subject, c->from_store ? "   (root from the trust store)" : "");
        fprintf(stderr, "*     issuer  %s\n", c->issuer);
        if (c->names[0]) fprintf(stderr, "*     names   %s\n", c->names);
        fprintf(stderr, "*     valid   %s .. %s\n*     key     %s, signed with %s\n*     sha256  ", a, b, c->key, c->sig);
        for (int k = 0; k < 32; k++) fprintf(stderr, "%02x%s", c->sha256[k], k < 31 ? ":" : "\n");
    }
}

static void usage(void) {
    fprintf(stderr,
            "usage: https_get URL [-o FILE] [--ca FILE] [--insecure] [-v] [-I] [-q] [--max-redirs N] [--timeout S]\n"
            "                 [--tls12|--tls13] [--cipher aes128|aes256|chacha20] [--time EPOCH] [--require-clock] [--no-decompress]\n"
            "  -o FILE         write the body to FILE (default: standard output)\n"
            "  --ca FILE       trust the root certificates in FILE (PEM) instead of $SSL_CERT_FILE or " TLS_DEFAULT_CA_FILE "\n"
            "  --insecure      DO NOT verify the server's certificate (experiments only)\n"
            "  -v              show the connection, the TLS parameters, the certificates and the headers on standard error\n"
            "  -I              HEAD request: print the response's header\n");
}

static int exit_code(const HttpResult *r) {
    switch (r->error) {
    case 0: return r->status >= 400 ? 4 : 0;
    case HTTP_E_URL: return 1;
    case HTTP_E_DNS: case HTTP_E_CONNECT: case HTTP_E_TIMEOUT: case HTTP_E_IO: return 2;
    case HTTP_E_TLS: return 3;
    case HTTP_E_CALLBACK: return 6;
    default: return 5;
    }
}

APPLET(https_get) {
    const char *url = NULL, *out = NULL, *ca = NULL;
    TlsConfig tc;
    memset(&tc, 0, sizeof tc);
    HttpOptions o;
    memset(&o, 0, sizeof o);
    get_ctx g = { 1, 0, 0, 0, 0, 0 };
    int64_t pin = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int more = i + 1 < argc;
        if (!strcmp(a, "-o") && more) out = argv[++i];
        else if (!strcmp(a, "--ca") && more) ca = argv[++i];
        else if (!strcmp(a, "--insecure") || !strcmp(a, "-k")) tc.flags |= TLS_INSECURE;
        else if (!strcmp(a, "-v")) g.verbose = 1;
        else if (!strcmp(a, "-q")) g.quiet = 1;
        else if (!strcmp(a, "-I") || !strcmp(a, "--head")) { g.head = 1; o.head = 1; }
        else if (!strcmp(a, "--max-redirs") && more) o.max_redirects = atoi(argv[++i]) == 0 ? -1 : atoi(argv[i]);
        else if (!strcmp(a, "--timeout") && more) { o.timeout_ms = atoi(argv[++i]) * 1000; tc.timeout_ms = o.timeout_ms; }
        else if (!strcmp(a, "--tls12")) tc.flags |= TLS_ONLY_TLS12;
        else if (!strcmp(a, "--tls13")) tc.flags |= TLS_ONLY_TLS13;
        else if (!strcmp(a, "--cipher") && more) {
            const char *c = argv[++i];
            tc.suites |= !strcmp(c, "aes128") ? TLS_SUITE_AES128_GCM : !strcmp(c, "aes256") ? TLS_SUITE_AES256_GCM : !strcmp(c, "chacha20") ? TLS_SUITE_CHACHA20 : 0;
            if (!tc.suites) { fprintf(stderr, "https_get: --cipher takes aes128, aes256 or chacha20\n"); return 1; }
        }
        else if (!strcmp(a, "--time") && more) pin = strtoll(argv[++i][0] == '@' ? argv[i] + 1 : argv[i], NULL, 10);
        else if (!strcmp(a, "--require-clock")) tc.flags |= TLS_REQUIRE_CLOCK;
        else if (!strcmp(a, "--no-decompress")) o.no_decode = 1;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (a[0] == '-' || url) { usage(); return 1; }
        else url = a;
    }
    if (!url) { usage(); return 1; }
    if (pin) tls_set_time(pin);
    tc.ca_file = ca;
    o.tls = &tc;
    o.on_body = on_body;
    o.on_status = on_status;
    o.on_header = on_header;
    o.on_event = on_event;
    o.on_tls = on_tls;
    o.ctx = &g;
    HttpCookieJar *jar = http_jar_new();
    o.jar = jar;
    char part[PATH_MAX];
    if (out && !g.head) {
        snprintf(part, sizeof part, "%s.part", out);
        g.fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (g.fd < 0) { fprintf(stderr, "https_get: %s: %s\n", part, strerror(errno)); http_jar_free(jar); return 6; }
    }
    HttpResult r;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = http_get(url, &o, &r);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    http_jar_free(jar);
    if (g.werr && rc == HTTP_E_CALLBACK) snprintf(r.error_text, sizeof r.error_text, "writing the body failed: %s", strerror(g.werr));
    int code = exit_code(&r);
    if (out && !g.head) {
        if (close(g.fd) != 0 && code == 0) { code = 6; snprintf(r.error_text, sizeof r.error_text, "%s: %s", part, strerror(errno)); }
        if (code == 0 || code == 4) {
            if (rename(part, out) != 0) { fprintf(stderr, "https_get: %s: %s\n", out, strerror(errno)); code = 6; }
        } else {
            unlink(part);
        }
    }
    if (rc != 0) fprintf(stderr, "https_get: %s\n", r.error_text);
    else if (r.status >= 400 && !g.quiet) fprintf(stderr, "https_get: the server answered %d %s\n", r.status, r.reason);
    if (g.verbose) {
        double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
        fprintf(stderr, "* %llu bytes%s in %.1f s from %s (status %d, %d redirect%s)\n", (unsigned long long)r.body_bytes,
                r.content_encoding[0] ? " (decompressed)" : "", secs, r.url, r.status, r.redirects, r.redirects == 1 ? "" : "s");
    }
    return code;
}
