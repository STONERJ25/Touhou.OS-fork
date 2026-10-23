#ifndef _CTYPE_H
#define _CTYPE_H

/* The functions exist (for pointers to them), and the usual calls are expanded in line: no function call per character.
 * Each macro evaluates its argument once. */
int isalpha(int c);
int isdigit(int c);
int isalnum(int c);
int isspace(int c);
int isupper(int c);
int islower(int c);
int isprint(int c);
int isgraph(int c);
int iscntrl(int c);
int ispunct(int c);
int isxdigit(int c);
int isblank(int c);
int isascii(int c);
int toupper(int c);
int tolower(int c);

static inline int __ct_isupper(int c) { return (unsigned)(c - 'A') < 26u; }
static inline int __ct_islower(int c) { return (unsigned)(c - 'a') < 26u; }
static inline int __ct_isalpha(int c) { return (unsigned)((c | 32) - 'a') < 26u; }
static inline int __ct_isdigit(int c) { return (unsigned)(c - '0') < 10u; }
static inline int __ct_isalnum(int c) { return __ct_isalpha(c) || __ct_isdigit(c); }
static inline int __ct_isspace(int c) { return c == ' ' || (unsigned)(c - '\t') < 5u; }
static inline int __ct_isblank(int c) { return c == ' ' || c == '\t'; }
static inline int __ct_iscntrl(int c) { return (unsigned)c < 32u || c == 127; }
static inline int __ct_isprint(int c) { return (unsigned)(c - 32) < 95u; }
static inline int __ct_isgraph(int c) { return (unsigned)(c - 33) < 94u; }
static inline int __ct_ispunct(int c) { return __ct_isgraph(c) && !__ct_isalnum(c); }
static inline int __ct_isxdigit(int c) { return __ct_isdigit(c) || (unsigned)((c | 32) - 'a') < 6u; }
static inline int __ct_isascii(int c) { return (unsigned)c < 128u; }
static inline int __ct_toupper(int c) { return __ct_islower(c) ? c - 32 : c; }
static inline int __ct_tolower(int c) { return __ct_isupper(c) ? c + 32 : c; }

#define isalpha(c)  __ct_isalpha(c)
#define isdigit(c)  __ct_isdigit(c)
#define isalnum(c)  __ct_isalnum(c)
#define isspace(c)  __ct_isspace(c)
#define isupper(c)  __ct_isupper(c)
#define islower(c)  __ct_islower(c)
#define isprint(c)  __ct_isprint(c)
#define isgraph(c)  __ct_isgraph(c)
#define iscntrl(c)  __ct_iscntrl(c)
#define ispunct(c)  __ct_ispunct(c)
#define isxdigit(c) __ct_isxdigit(c)
#define isblank(c)  __ct_isblank(c)
#define isascii(c)  __ct_isascii(c)
#define toupper(c)  __ct_toupper(c)
#define tolower(c)  __ct_tolower(c)

#endif
