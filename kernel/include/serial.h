#ifndef K_SERIAL_H
#define K_SERIAL_H

#include "types.h"

void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s, size_t n);
int  serial_getc(void);         /* -1 when no byte is waiting */

#endif
