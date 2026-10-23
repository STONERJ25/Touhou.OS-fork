#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdarg.h>
#include <sys/types.h>

#define EOF (-1)
#define BUFSIZ 4096
#define FILENAME_MAX 1024
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef struct FILE FILE;
extern FILE *stdin, *stdout, *stderr;
#define stdin  stdin
#define stdout stdout
#define stderr stderr

FILE  *fopen(const char *path, const char *mode);
FILE  *fdopen(int fd, const char *mode);
FILE  *freopen(const char *path, const char *mode, FILE *f);
int    fclose(FILE *f);
int    fflush(FILE *f);
size_t fread(void *buf, size_t size, size_t n, FILE *f);
size_t fwrite(const void *buf, size_t size, size_t n, FILE *f);
int    fgetc(FILE *f);
int    getc(FILE *f);
int    getchar(void);
int    ungetc(int c, FILE *f);
char  *fgets(char *buf, int n, FILE *f);
int    fputc(int c, FILE *f);
int    putc(int c, FILE *f);
int    putchar(int c);
int    fputs(const char *s, FILE *f);
ssize_t getline(char **line, size_t *cap, FILE *f);
ssize_t getdelim(char **line, size_t *cap, int delim, FILE *f);
int    puts(const char *s);
int    fseek(FILE *f, long off, int whence);
long   ftell(FILE *f);
void   rewind(FILE *f);
int    feof(FILE *f);
int    ferror(FILE *f);
void   clearerr(FILE *f);
int    fileno(FILE *f);
void   setbuf(FILE *f, char *buf);
int    setvbuf(FILE *f, char *buf, int mode, size_t size);
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

int    printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int    fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int    sprintf(char *buf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int    snprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int    vprintf(const char *fmt, va_list ap);
int    vfprintf(FILE *f, const char *fmt, va_list ap);
int    vsprintf(char *buf, const char *fmt, va_list ap);
int    vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int    dprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int    asprintf(char **out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int    vasprintf(char **out, const char *fmt, va_list ap);
int    vdprintf(int fd, const char *fmt, va_list ap);

int    sscanf(const char *s, const char *fmt, ...);
int    vsscanf(const char *s, const char *fmt, va_list ap);
int    scanf(const char *fmt, ...);
int    fscanf(FILE *f, const char *fmt, ...);

void   perror(const char *s);
int    remove(const char *path);
int    rename(const char *from, const char *to);
char  *tmpnam(char *buf);
FILE  *popen(const char *cmd, const char *mode);
int    pclose(FILE *f);

#endif
