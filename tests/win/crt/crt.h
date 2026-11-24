/* A small C runtime of our own for the Windows test programs (docs/WINDOWS.md, "The mini C runtime"): written from the C standard's descriptions of the
 * functions, the Win32 documentation of the kernel32 calls it sits on, and Microsoft's "Parsing C++ command-line arguments" for argv. No CRT source was
 * looked at. It is linked into every program under tests/win/crtsrc; the entry point is mainCRTStartup. */
#ifndef TEST_CRT_H
#define TEST_CRT_H

typedef unsigned long long size_t;
typedef long long ptrdiff_t;
typedef __builtin_va_list va_list;
#define va_start(a, b) __builtin_va_start(a, b)
#define va_arg(a, t) __builtin_va_arg(a, t)
#define va_end(a) __builtin_va_end(a)
#define NULL ((void *)0)
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

typedef struct CrtFile FILE;
extern FILE *stdout, *stderr;

void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
char *strcpy(char *d, const char *s);
char *strcat(char *d, const char *s);

void *malloc(size_t n);
void free(void *p);
void *calloc(size_t n, size_t size);
void *realloc(void *p, size_t n);
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
int atexit(void (*fn)(void));
__declspec(noreturn) void exit(int code);
__declspec(noreturn) void abort(void);
int atoi(const char *s);

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int printf(const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
int puts(const char *s);
int putchar(int c);
int fputs(const char *s, FILE *f);
int fflush(FILE *f);

#endif
