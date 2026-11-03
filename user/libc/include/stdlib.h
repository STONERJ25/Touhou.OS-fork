#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 2147483647

void  *malloc(size_t n);
void  *calloc(size_t n, size_t sz);
void  *realloc(void *p, size_t n);
void   free(void *p);
void  *aligned_alloc(size_t align, size_t n);
int    posix_memalign(void **out, size_t align, size_t n);

__attribute__((noreturn)) void exit(int status);
__attribute__((noreturn)) void _exit(int status);
__attribute__((noreturn)) void abort(void);
int    atexit(void (*fn)(void));

int    abs(int x);
long   labs(long x);
long long llabs(long long x);
typedef struct { int quot, rem; } div_t;
typedef struct { long quot, rem; } ldiv_t;
div_t  div(int a, int b);
ldiv_t ldiv(long a, long b);

int    atoi(const char *s);
long   atol(const char *s);
long long atoll(const char *s);
double atof(const char *s);
long   strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
double strtod(const char *s, char **end);
float  strtof(const char *s, char **end);

int    rand(void);
void   srand(unsigned seed);
long   random(void);
void   srandom(unsigned seed);

void   qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
void  *bsearch(const void *key, const void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));

extern char **environ;
char  *getenv(const char *name);
int    setenv(const char *name, const char *value, int overwrite);
int    unsetenv(const char *name);
int    putenv(char *s);
int    system(const char *cmd);
char  *realpath(const char *path, char *out);

#endif
