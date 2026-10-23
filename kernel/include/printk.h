#ifndef K_PRINTK_H
#define K_PRINTK_H

#include "types.h"

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* Raw console output: serial + framebuffer console + in-memory log. */
void console_write(const char *s, size_t n);

int  printk(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  vprintk(const char *fmt, va_list ap);

/* "[Subsystem] message\n" -- the tag is the Touhou-flavoured subsystem name. */
void klog(const char *subsystem, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif
