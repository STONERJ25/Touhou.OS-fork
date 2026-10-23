/* The printf engine's output sink (private to libgap). A sink is a staging buffer plus an optional way to empty it:
 *   snprintf-style: buf is the caller's buffer, no flush function; what does not fit is only counted.
 *   FILE / fd style: buf is a small stack buffer, flush hands it to fwrite/write. */
#ifndef LIBGAP_FORMAT_H
#define LIBGAP_FORMAT_H

#include <stddef.h>
#include <stdarg.h>

typedef struct Sink {
    char *buf;
    size_t pos, cap;                                            /* bytes staged / room */
    size_t count;                                               /* bytes produced so far, stored or not */
    int (*flush)(struct Sink *s);                               /* buf is full: empty it (0) or fail (-1); NULL: drop the rest */
    int (*direct)(struct Sink *s, const char *p, size_t n);     /* optional: write a big block past the staging buffer */
    void *ctx;
    int err;                                                    /* a flush or direct write failed */
} Sink;

int __vformat(Sink *s, const char *fmt, va_list ap);            /* returns the number of bytes produced */

#endif
