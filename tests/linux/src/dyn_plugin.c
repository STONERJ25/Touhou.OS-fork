/* A shared library for dyn_dlopen.c (libdynplug.so, built with -shared -fPIC by tools/linux-oracle.py): what a library can have that a loader must get right when it is
 * opened at run time: data, a constructor and a destructor, a thread-local variable (dynamic TLS: __tls_get_addr), a call back into the program through a function pointer. */
#include <stdio.h>

int plug_counter = 100;                         /* writable data of the library, reached through its GOT */
static int constructed;
static int *closed_flag;
static __thread int plug_tls = 7;               /* a thread-local variable of a library opened with dlopen: allocated after the thread started */
static const char *const plug_names[] = { "first", "second" };

__attribute__((constructor)) static void plug_up(void) { constructed++; }
__attribute__((destructor)) static void plug_down(void) { if (closed_flag) *closed_flag = 1; }      /* dlclose runs it: the program's flag shows that it did */
void plug_on_close(int *flag) { closed_flag = flag; }

int plug_add(int a, int b) { return a + b + (plug_counter - 100); }
int plug_constructed(void) { return constructed; }
int plug_tls_next(void) { return ++plug_tls; }
const char *plug_name(int i) { return plug_names[i & 1]; }
int plug_apply(int (*fn)(int), int v) { return fn(v) + 1; }       /* calls back into the program (a function that the program's own relocations made) */
