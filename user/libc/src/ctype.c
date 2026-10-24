/* The out-of-line versions of the <ctype.h> functions (the header expands the usual calls in line). */
#include <ctype.h>

#undef isalpha
#undef isdigit
#undef isalnum
#undef isspace
#undef isupper
#undef islower
#undef isprint
#undef isgraph
#undef iscntrl
#undef ispunct
#undef isxdigit
#undef isblank
#undef isascii
#undef toupper
#undef tolower

int isupper(int c) { return __ct_isupper(c); }
int islower(int c) { return __ct_islower(c); }
int isalpha(int c) { return __ct_isalpha(c); }
int isdigit(int c) { return __ct_isdigit(c); }
int isalnum(int c) { return __ct_isalnum(c); }
int isspace(int c) { return __ct_isspace(c); }
int isblank(int c) { return __ct_isblank(c); }
int iscntrl(int c) { return __ct_iscntrl(c); }
int isprint(int c) { return __ct_isprint(c); }
int isgraph(int c) { return __ct_isgraph(c); }
int ispunct(int c) { return __ct_ispunct(c); }
int isxdigit(int c) { return __ct_isxdigit(c); }
int isascii(int c) { return __ct_isascii(c); }
int toupper(int c) { return __ct_toupper(c); }
int tolower(int c) { return __ct_tolower(c); }
