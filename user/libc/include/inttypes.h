#ifndef _INTTYPES_H
#define _INTTYPES_H
#include <stdint.h>
#define PRId64 "ld"
#define PRIu64 "lu"
#define PRIx64 "lx"
#define PRId32 "d"
#define PRIu32 "u"
#define PRIx32 "x"
#define PRIX64 "lX"
#define PRIo64 "lo"
#define PRIi64 "li"
#define PRIX32 "X"
#define PRIo32 "o"
#define PRIi32 "i"
#define PRIdMAX "ld"
#define PRIuMAX "lu"
#define PRIxMAX "lx"
#define SCNd64 "ld"
#define SCNu64 "lu"
#define SCNx64 "lx"
#define SCNd32 "d"
#define SCNu32 "u"
#define SCNx32 "x"

intmax_t imaxabs(intmax_t x);
intmax_t strtoimax(const char *s, char **end, int base);
uintmax_t strtoumax(const char *s, char **end, int base);
#endif
