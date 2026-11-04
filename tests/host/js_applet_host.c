/*
 * js_applet_host: the `js` applet (user/bin/kappabox/js.c) built for the host.  The applet is ordinary POSIX C that uses
 * a few helpers of kappabox (kb_error, kb_read_all, kb_term_cols, kb_join); this file provides them and a main(), so that
 * tests/host/test_js_applet.py can run the real applet code under AddressSanitizer, with pipes and a pseudo terminal,
 * without booting the OS.  tests/case_js.py then only has to check the integration with the OS.
 */
#include "kappabox.h"
#include <stdarg.h>

const char *kb_name = "js";
int kb_color;

void kb_error(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s: ", kb_name);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void kb_die(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s: ", kb_name);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

int kb_term_cols(void) {
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col) return ws.ws_col;
    return 80;
}

char *kb_join(const char *dir, const char *name) {
    size_t n = strlen(dir) + strlen(name) + 2;
    char *p = malloc(n);
    if (p) snprintf(p, n, "%s/%s", dir, name);
    return p;
}

char *kb_read_all(int fd, size_t *len) {
    size_t cap = 1 << 14, n = 0;
    char *b = malloc(cap + 1);
    if (!b) return NULL;
    for (;;) {
        if (n == cap) {
            cap *= 2;
            char *nb = realloc(b, cap + 1);
            if (!nb) { free(b); return NULL; }
            b = nb;
        }
        ssize_t r = read(fd, b + n, cap - n);
        if (r < 0) { if (errno == EINTR) continue; free(b); return NULL; }
        if (r == 0) break;
        n += (size_t)r;
    }
    b[n] = 0;
    if (len) *len = n;
    return b;
}

int applet_js(int argc, char **argv);

int main(int argc, char **argv) {
    kb_color = isatty(1);
    return applet_js(argc, argv);
}
