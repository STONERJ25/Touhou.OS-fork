#ifndef _LIBGAP_LIMITS_H
#define _LIBGAP_LIMITS_H
#include_next <limits.h>

#define PATH_MAX   1024
#define NAME_MAX   255
#define PIPE_BUF   4096
#define OPEN_MAX   64
#define ARG_MAX    (128 * 1024)
#define SSIZE_MAX  LONG_MAX
#ifndef MB_LEN_MAX
#define MB_LEN_MAX 4
#endif
#endif
