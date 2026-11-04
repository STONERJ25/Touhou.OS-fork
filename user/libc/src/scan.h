/* The scanf engine's input source (private to libgap): characters come one at a time, and the engine pushes back what it read
 * too far (usually one character; after "1e+x" or "0xg" it pushes back up to three, which strings and the stdio buffer allow). */
#ifndef LIBGAP_SCAN_H
#define LIBGAP_SCAN_H

#include <stdarg.h>

typedef struct Src {
    int (*get)(void *ctx);                      /* next character (0..255) or EOF */
    void (*unget)(void *ctx, int c);            /* gives back the last character(s) obtained with get, newest first */
    void *ctx;
} Src;

int __vscan(Src *src, const char *fmt, va_list ap);

#endif
