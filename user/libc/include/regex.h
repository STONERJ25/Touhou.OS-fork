#ifndef _REGEX_H
#define _REGEX_H
#include <stddef.h>

typedef struct { void *prog; size_t re_nsub; int flags; } regex_t;
typedef long regoff_t;
typedef struct { regoff_t rm_so, rm_eo; } regmatch_t;

#define REG_EXTENDED 1
#define REG_ICASE    2
#define REG_NOSUB    4
#define REG_NEWLINE  8
#define REG_NOTBOL   1
#define REG_STARTEND 4      /* regexec: search str[pmatch[0].rm_so .. pmatch[0].rm_eo) (no NUL needed); ^ and \b still see the whole
                               of str, and the offsets that come back are relative to str */
#define REG_NOMATCH  1
#define REG_BADPAT   2
#define REG_ESPACE   3

int    regcomp(regex_t *re, const char *pattern, int flags);
int    regexec(const regex_t *re, const char *str, size_t nmatch, regmatch_t pmatch[], int eflags);
void   regfree(regex_t *re);
size_t regerror(int code, const regex_t *re, char *buf, size_t n);
#endif
