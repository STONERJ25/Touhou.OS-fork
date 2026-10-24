/* mcc's <stdarg.h>: the System V AMD64 va_list, as the compiler builds it. */
#ifndef _MCC_STDARG_H
#define _MCC_STDARG_H

typedef __builtin_va_list va_list;
typedef __builtin_va_list __gnuc_va_list;

#define va_start(ap, ...) __builtin_va_start(ap, 0)
#define va_end(ap) __builtin_va_end(ap)
#define va_arg(ap, ty) __builtin_va_arg(ap, ty)
#define va_copy(dest, src) __builtin_va_copy(dest, src)
#define __va_copy(dest, src) __builtin_va_copy(dest, src)

#endif
