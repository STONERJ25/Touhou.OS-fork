#ifndef K_CMDLINE_H
#define K_CMDLINE_H

#include "types.h"

void        cmdline_init(const char *s);
const char *cmdline_string(void);
bool        cmdline_flag(const char *name);                            /* "name" present as a word */
bool        cmdline_value(const char *key, char *out, size_t n);       /* "key=value" */
long        cmdline_int(const char *key, long def);

#endif
