#ifndef _SETJMP_H
#define _SETJMP_H
typedef unsigned long jmp_buf[8];
int setjmp(jmp_buf env);
__attribute__((noreturn)) void longjmp(jmp_buf env, int val);
#endif
