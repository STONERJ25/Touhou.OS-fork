/* mcc's <stddef.h> */
#ifndef _MCC_STDDEF_H
#define _MCC_STDDEF_H

#ifndef _MCC_SIZE_T
#define _MCC_SIZE_T
typedef unsigned long size_t;
#endif
#ifndef _MCC_PTRDIFF_T
#define _MCC_PTRDIFF_T
typedef long ptrdiff_t;
#endif
#ifndef _MCC_WCHAR_T
#define _MCC_WCHAR_T
typedef int wchar_t;
#endif
typedef struct {
    long long __ll;
    double __d;
} max_align_t;

#undef NULL
#define NULL ((void *)0)
#define offsetof(type, member) __builtin_offsetof(type, member)

#endif
